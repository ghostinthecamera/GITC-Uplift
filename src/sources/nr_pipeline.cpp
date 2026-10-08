#include "sources/nr_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <utility>

#include "nr/log.hpp"
#include "sources/look_plan.hpp"

namespace uplift::sources {
namespace {

// The Present decode is a pixel shader; everything else reads through compute-legal states only.
constexpr D3D12_RESOURCE_STATES PIXEL_AND_COMPUTE_READ =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES COMPUTE_READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t MODEL_BYTES_PER_PIXEL = 8u;   // RGBA16F
constexpr uint32_t MOTION_BYTES_PER_PIXEL = 4u;  // RG16F
constexpr uint64_t STATE_BYTES = 32u;  // a 2x1 RGBA32F state texture (Plan 17: the second texel is Auto's check)
constexpr nr::Size STATE_SIZE = {2u, 1u};
// Plan 17: one readback slot per placed footprint of the 2x1 state (512 B apart, a 256 B row).
constexpr uint64_t CHECK_STRIDE = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE};
  barrier.Transition = {.pResource = resource, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        .StateBefore = before, .StateAfter = after};
  list->ResourceBarrier(1u, &barrier);
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER written = {.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE};
  written.UAV = {.pResource = resource};
  list->ResourceBarrier(1u, &written);
}

// A recording at `output` runs at: an empty image is the output itself, an empty canvas the image.
WorkLayout Resolve(const WorkLayout& layout, nr::Size output) {
  const nr::Size image = (layout.image.Empty() ? output : layout.image);
  return {.image = image, .canvas = (layout.canvas.Empty() ? image : layout.canvas), .upsampling = layout.upsampling};
}

// A float SRV format for the game's motion-vector texture, or nullopt when Uplift cannot read it.
std::optional<DXGI_FORMAT> MotionViewFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:          return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:          return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                                return std::nullopt;
  }
}

// The stabiliser's motion vectors (current -> previous) with a resolved rect and their scale, or none: absent or
// unreadable vectors make Motion change-gated static (decision D4).
color::StabilizeMotion StabilizeMotionOf(const nr::BoundResource& motion, float scale_x, float scale_y) {
  if (motion.resource == nullptr) return {};
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  if (!view_format) return {};
  nr::Rect rect = motion.rect;
  if (rect.width == 0u || rect.height == 0u) {
    rect = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  return {.vectors = {.resource = motion.resource, .rect = rect}, .view_format = *view_format, .scale_x = scale_x, .scale_y = scale_y};
}

// Fix round 2, M1: Session::Evaluate (via EnsureFeatures) writes a fresh message_ for exactly these
// EvaluateResult reasons. "resizing" (EnsureFeatures' own !retired_.empty() branch) and anything else
// (an unreachable ReasonFor(state) or Session::Evaluate's own "invalid input", both pre-filtered by
// this class's own checks before Evaluate() is ever called) leave message_ stale from an earlier call,
// so ContextMessage must not be told to trust it.
bool SessionWroteMessage(std::string_view reason) {
  return reason == "frame too small" || reason == "budget" || reason == "create failed" || reason == "evaluate failed";
}

}  // namespace

NrPipeline::NrPipeline(ID3D12Device* device, nr::Session& session, nr::Timeline& timeline)
    : device_(device), session_(session), timeline_(timeline) {}

NrPipeline::~NrPipeline() = default;

bool NrPipeline::Initialize(std::string* error) {
  return color_.Initialize(device_.Get(), error);
}

uint64_t NrPipeline::HeldBytes() const {
  const uint64_t copies =
      (motion_copies_.textures[0]
           ? static_cast<uint64_t>(motion_copies_.slots) * motion_copies_.size.Pixels() * MOTION_BYTES_PER_PIXEL
           : 0u);
  const uint64_t launchpad = (launchpad_motion_.texture ? launchpad_motion_.size.Pixels() * MOTION_BYTES_PER_PIXEL : 0u);
  const uint64_t ring = (dlss_present_motion_.texture ? dlss_present_motion_.size.Pixels() * MOTION_BYTES_PER_PIXEL : 0u);  // Plan 18 (fix round 1)
  return (intermediates_.model_a ? intermediates_.bytes : 0u) + copies + launchpad + ring + look_.bytes + mask_.bytes
         + (exposure_.state ? STATE_BYTES : 0u) + faces_.bytes;
}

bool NrPipeline::SupportsUavTarget(DXGI_FORMAT format) const {
  const std::optional<color::UavFormatInfo> info = color::DescribeUavFormat(format);
  return info.has_value() && color_.SupportsTypedUavStore(info->uav_format);
}

void NrPipeline::ReleaseIntermediates() {
  RetireSet();
  RetireLook();
  RetireFaces();
  RetireMotionCopies();
  RetireLaunchpadMotion();
  RetireDlssPresentMotion();
  RetireMask();
  RetireExposure();
}

void NrPipeline::RetireSet() {
  if (!intermediates_.model_a) return;
  // Plan 3's rule: every recording takes its mark before its first command, so once the GPU has passed
  // the newest one the set goes at once; otherwise the callback's copy keeps it alive until then.
  if (!timeline_.IsComplete(intermediates_.last_use)) {
    timeline_.ReleaseAfter(intermediates_.last_use, [retired = intermediates_] {});
  }
  intermediates_ = {};
}

bool NrPipeline::MakeMotionCopies(size_t first, size_t end, nr::Size size) {
  bool made = true;
  for (size_t index = first; index < end; ++index) {
    motion_copies_.textures[index] = CreateTexture(size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ,
                                                   L"Uplift DLSS motion copy");
    made = made && static_cast<bool>(motion_copies_.textures[index]);
  }
  if (!made) {
    for (size_t index = first; index < end; ++index) {
      motion_copies_.textures[index].Reset();  // never used: none was recorded into yet
    }
  }
  return made;
}

void NrPipeline::RetireMotionCopies() {
  for (size_t index = 0u; index < motion_copies_.textures.size(); ++index) {
    if (motion_copies_.textures[index] && !timeline_.IsComplete(motion_copies_.last_use[index])) {
      timeline_.ReleaseAfter(motion_copies_.last_use[index], [retired = motion_copies_.textures[index]] {});
    }
  }
  motion_copies_ = {};
}

void NrPipeline::RetireLaunchpadMotion() {
  if (launchpad_motion_.texture && !timeline_.IsComplete(launchpad_motion_.last_use)) {
    timeline_.ReleaseAfter(launchpad_motion_.last_use, [retired = launchpad_motion_.texture] {});
  }
  launchpad_motion_ = {};
}

void NrPipeline::RetireDlssPresentMotion() {
  if (dlss_present_motion_.texture && !timeline_.IsComplete(dlss_present_motion_.last_use)) {
    timeline_.ReleaseAfter(dlss_present_motion_.last_use, [retired = dlss_present_motion_.texture] {});
  }
  dlss_present_motion_ = {};
}

void NrPipeline::RetireLook() {
  if (look_.plan == LookPlan{}) return;
  if (!timeline_.IsComplete(look_.last_use)) {
    timeline_.ReleaseAfter(look_.last_use, [retired = look_] {});
  }
  look_ = {};
}

void NrPipeline::RetireFaces() {
  if (!faces_.twin) return;
  if (!timeline_.IsComplete(faces_.last_use)) {
    timeline_.ReleaseAfter(faces_.last_use, [retired = faces_] {});
  }
  faces_ = {};
}

bool NrPipeline::EnsureFaces(nr::Size canvas) {
  if (faces_.twin && faces_.size == canvas) return true;
  RetireFaces();
  const look::Atlas layout = look::MakeAtlas(canvas);
  constexpr D3D12_RESOURCE_FLAGS UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  FaceSurfaces created = {
      .twin = CreateTexture(canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces twin"),
      .detail = CreateTexture(canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces detail"),
      .atlas = CreateTexture(layout.size, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces pyramid"),
      .fill = CreateTexture(layout.size, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift Keep faces fill pyramid"),
      .layout = layout,
      .size = canvas,
      .bytes = FaceSurfaceBytes(canvas),
  };
  if (!created.twin || !created.detail || !created.atlas || !created.fill) {
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} Keep faces surfaces", canvas.width, canvas.height);
      allocation_failure_logged_ = true;
    }
    return false;  // `created` was never used by the GPU: freed here
  }
  allocation_failure_logged_ = false;
  faces_ = std::move(created);
  return true;
}

void NrPipeline::RecordFaces(ID3D12GraphicsCommandList* list, color::FacesPass pass) {
  ID3D12Resource* const atlas = faces_.atlas.Get();
  pass.atlas = atlas;
  pass.detail = faces_.detail.Get();
  pass.fill = faces_.fill.Get();
  pass.layout = faces_.layout;
  pass.parameters = look::MakeFacesParameters(config_.keep_faces.lighting_scale, config_.keep_faces.tuning, intermediates_.plan.image.height, faces_.layout.levels);
  faces_.last_use = slot_marks_[resolve_slot_];
  const bool fill = (pass.mask == nullptr && pass.parameters.fill);  // round 5: pass 1's pyramid writes the fill's too
  Transition(list, atlas, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (fill) {
    Transition(list, pass.fill, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  color_.RecordFacesPyramid(list, resolve_slot_, pass);
  Transition(list, atlas, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (fill) {
    Transition(list, pass.fill, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  if (pass.parameters.speck_radius != 0u) {
    // Fix round 4: the difference despiked before the combine rewrites `changed` (and pass 1's twin) in place.
    Transition(list, pass.detail, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordFacesDespike(list, resolve_slot_, pass);
    Transition(list, pass.detail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  Transition(list, pass.changed, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (pass.mask == nullptr) {
    Transition(list, pass.reference, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);  // pass 1: the twin's output becomes the face mask
  }
  color_.RecordFacesCombine(list, resolve_slot_, pass);
  Transition(list, pass.changed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (pass.mask == nullptr) {
    Transition(list, pass.reference, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
}

void NrPipeline::CombineFaces(ID3D12GraphicsCommandList* list, ID3D12Resource* lighting, ID3D12Resource* faces) {
  RecordFaces(list, {.changed = lighting, .reference = faces});
  faces_ran_ = true;
}

void NrPipeline::RetireMask() {
  if (mask_.texture && !timeline_.IsComplete(mask_.last_use)) {
    timeline_.ReleaseAfter(mask_.last_use, [retired = mask_.texture] {});
  }
  mask_ = {};
}

void NrPipeline::RetireExposure() {
  if (exposure_.state && !timeline_.IsComplete(exposure_.last_use)) {
    timeline_.ReleaseAfter(exposure_.last_use, [retired = exposure_.state] {});
  }
  exposure_ = {};
  RetireExposureCheck();
}

void NrPipeline::RetireExposureCheck() {
  // The samples still in flight are dropped (Auto's run starts again with the next ones); the buffer goes once the GPU has passed its copies.
  for (uint32_t index = 0u; index < CHECK_SLOTS; ++index) {
    if (check_.readback && check_.pending[index] && !timeline_.IsComplete(check_.marks[index])) {
      timeline_.ReleaseAfter(check_.marks[index], [retired = check_.readback] {});
    }
  }
  check_ = {};
}

NrPipeline::Texture NrPipeline::CreateTexture(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                              D3D12_RESOURCE_STATES state, const wchar_t* name) {
  const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC description = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = 0u,
      .Width = size.width,
      .Height = size.height,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = format,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = flags,
  };
  Texture texture;
  if (SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                 IID_PPV_ARGS(&texture)))) {
    texture->SetName(name);
  }
  return texture;
}

std::string_view NrPipeline::EnsureIntermediates(const SetPlan& plan) {
  if (intermediates_.model_a && intermediates_.plan == plan) return {};
  RetireSet();
  constexpr D3D12_RESOURCE_FLAGS UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const bool pre_sr = (plan.kind == TargetKind::PRE_SR);
  // I-1 (Plan 4 fix round 4): pre-SR always gets the image-sized change texture, whatever plan.output
  // is this set -- a dynamic-resolution game's per-frame render region no longer keys the set at all
  // (RecordPreSr below), so whether a frame needs the change path can no longer be read off the set.
  const bool reduced = (plan.image != plan.output) || pre_sr;
  Intermediates created = {
      .source_copy = (plan.copy_format == DXGI_FORMAT_UNKNOWN
                          ? Texture()
                          : CreateTexture(plan.output, plan.copy_format, D3D12_RESOURCE_FLAG_NONE,
                                          (plan.kind == TargetKind::PRESENT ? PIXEL_AND_COMPUTE_READ : COMPUTE_READ),
                                          L"Uplift source copy")),
      .model_a = CreateTexture(plan.canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift model A"),
      .model_b = CreateTexture(plan.canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift model B"),
      .zero_motion = CreateTexture(plan.canvas, DXGI_FORMAT_R16G16_FLOAT, UAV, COMPUTE_READ, L"Uplift zero motion"),
      .change = (reduced ? CreateTexture(plan.image, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift change field")
                         : Texture()),
      .canvas_motion = (plan.canvas_motion
                            ? CreateTexture(plan.canvas, DXGI_FORMAT_R16G16_FLOAT, UAV, COMPUTE_READ, L"Uplift canvas motion")
                            : Texture()),
      .private_color = (pre_sr ? CreateTexture(plan.private_color, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ,
                                               L"Uplift pre-SR colour")
                               : Texture()),
      .plan = plan,
      .bytes = plan.output.Pixels() * plan.source_bytes_per_pixel
               + plan.canvas.Pixels() * (2u * MODEL_BYTES_PER_PIXEL + MOTION_BYTES_PER_PIXEL)
               + (reduced ? plan.image.Pixels() * MODEL_BYTES_PER_PIXEL : 0u)
               + (plan.canvas_motion ? plan.canvas.Pixels() * MOTION_BYTES_PER_PIXEL : 0u)
               + plan.private_color.Pixels() * MODEL_BYTES_PER_PIXEL,
  };
  const bool complete = created.model_a && created.model_b && created.zero_motion
                        && (plan.copy_format == DXGI_FORMAT_UNKNOWN || created.source_copy) && (!reduced || created.change)
                        && (!plan.canvas_motion || created.canvas_motion) && (!pre_sr || created.private_color);
  if (!complete) {
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} NR surfaces", plan.canvas.width, plan.canvas.height);
      allocation_failure_logged_ = true;
    }
    return "out of memory";
  }
  allocation_failure_logged_ = false;
  intermediates_ = std::move(created);
  return {};
}

void NrPipeline::SetLookConfig(const LookConfig& config) {
  // v2 design §3.12: a new Detail radius or Stabilize mode starts the stabiliser afresh.
  if (config.look.detail_radius != config_.look.detail_radius || config.look.stabilize != config_.look.stabilize
      || config.look.stabilize_detail != config_.look.stabilize_detail) {
    look_.history_valid = false;
  }
  if (config.fixes != config_.fixes) {
    exposure_.snap = true;  // v2 design §3.14: the governor snaps on a settings change
  }
  config_ = config;
}

LookPlan NrPipeline::LookPlanFor(nr::Size image, bool reduced) const {
  return PlanLook(config_, image, reduced, static_cast<bool>(intermediates_.change), mask_.copied, color_.SupportsUavLoads());
}

bool NrPipeline::EnsureLook(const LookPlan& plan) {
  if (plan == look_.plan) return true;
  RetireLook();
  if (plan == LookPlan{}) return true;
  LookSurfaces created = {.plan = plan, .atlas = (plan.shape ? look::MakeAtlas(plan.image) : look::Atlas{})};
  const auto make = [this, &created](bool wanted, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, const wchar_t* name) {
    if (!wanted) return Texture();
    created.bytes += size.Pixels() * bytes_per_pixel;
    return CreateTexture(size, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ, name);
  };
  created.change = make(plan.own_change, plan.image, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift change field (output size)");
  created.basis = make(plan.shape, plan.image, DXGI_FORMAT_R16G16_FLOAT, 4u, L"Uplift look basis");
  created.gauss = make(plan.shape, created.atlas.size, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look pyramid");
  created.peaks = make(plan.shape, created.atlas.size, DXGI_FORMAT_R16_FLOAT, 2u, L"Uplift look peaks");
  for (Texture& history : created.history) {
    history = make(plan.stabilize, created.atlas.size, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look history");
  }
  for (Texture& history : created.detail_history) {
    history = make(plan.detail, plan.image, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look detail history");
  }
  created.state = make(plan.stabilize, {1u, 1u}, DXGI_FORMAT_R32G32B32A32_FLOAT, static_cast<uint32_t>(STATE_BYTES), L"Uplift look state");
  const bool complete = (!plan.own_change || created.change) && (!plan.shape || (created.basis && created.gauss && created.peaks))
                        && (!plan.stabilize || (created.history[0] && created.history[1] && created.state))
                        && (!plan.detail || (created.detail_history[0] && created.detail_history[1]));
  if (!complete) {
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} look surfaces", plan.image.width, plan.image.height);
      allocation_failure_logged_ = true;
    }
    return false;  // `created` was never used by the GPU: freed here
  }
  allocation_failure_logged_ = false;
  look_ = std::move(created);
  return true;
}

uint32_t NrPipeline::ShaderOptions() const {
  return static_cast<uint32_t>(config_.fixes.primaries)  // the shaders resolve AUTO per encoding
         | (config_.fixes.near_black_guard ? color::shader_options::NEAR_BLACK_GUARD : 0u)
         | (config_.fixes.transfer == color::NeuralTransfer::CONSISTENT ? color::shader_options::CONSISTENT : 0u);
}

NrPipeline::ExposureChoice NrPipeline::ChooseExposure(color::Encoding encoding, ID3D12Resource* game_texture, float game_factor) {
  PollExposureChecks();
  const bool game_exposure = (game_texture != nullptr || game_factor != 1.f);
  // v2 design §3.14: only relative scene-linear images are metered, and the meter needs typed UAV loads.
  const bool can_meter = (color::Meterable(encoding) && color_.SupportsUavLoads());
  const bool automatic = (config_.fixes.input_exposure == color::InputExposure::AUTO);
  bool use_game = false;
  bool metered = false;
  bool probe = false;
  switch (config_.fixes.input_exposure) {
    case color::InputExposure::AUTO:
      // Plan 17: the game's exposure while it agrees with the meter, which runs beside it (Auto's check), and the meter once it has not for a sustained
      // second; the meter without one, and the game's where the meter cannot run.
      if (!game_exposure) {
        metered = true;
      } else if (can_meter && auto_exposure_.Latched()) {
        metered = true;
      } else {
        use_game = true;
        probe = can_meter;
      }
      break;
    case color::InputExposure::GAME:    use_game = true; break;
    case color::InputExposure::METERED: metered = true; break;
    case color::InputExposure::MANUAL:  break;
  }
  metered = (metered && can_meter);
  if (!metered && !probe && exposure_.state) {
    RetireExposure();  // Batch 1 review: InputExposure left Metered/auto-metered; the state is no longer read
  }
  if ((metered || probe) && !exposure_.state) {
    exposure_.state = CreateTexture(STATE_SIZE, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, PIXEL_AND_COMPUTE_READ,
                                    L"Uplift exposure state");
    exposure_.snap = true;  // undefined until the meter's first snap
  }
  if (metered && exposure_.state) {
    exposure_report_ = {.use = ExposureUse::METERED,
                        .stops_off = ((automatic && game_exposure && auto_exposure_.Latched()) ? std::optional<float>(auto_exposure_.StopsOff()) : std::nullopt)};
    return {.texture = exposure_.state.Get(), .factor = 1.f, .metered = true};
  }
  if (use_game) {
    exposure_report_ = {.use = (game_exposure ? ExposureUse::GAME : ExposureUse::NONE)};
    return {.texture = game_texture, .factor = game_factor, .probe = (probe && exposure_.state)};
  }
  exposure_report_ = {};
  return {};
}

void NrPipeline::PollExposureChecks() {
  if (!check_.readback) return;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  for (uint32_t step = 0u; step < CHECK_SLOTS; ++step) {
    const uint32_t index = (check_.next + step) % CHECK_SLOTS;  // the oldest first
    if (!check_.pending[index] || !timeline_.IsComplete(check_.marks[index])) continue;
    check_.pending[index] = false;
    std::array<float, 8> texels = {};  // (2^E, E, the anchor, set), (the game's texel, its factor, the game's exposure, the meter's target)
    const D3D12_RANGE read = {.Begin = index * CHECK_STRIDE, .End = index * CHECK_STRIDE + sizeof(texels)};
    void* mapped = nullptr;
    if (FAILED(check_.readback->Map(0u, &read, &mapped))) continue;
    std::memcpy(texels.data(), static_cast<const std::byte*>(mapped) + read.Begin, sizeof(texels));
    const D3D12_RANGE written = {.Begin = 0u, .End = 0u};
    check_.readback->Unmap(0u, &written);
    const ExposureSample sample = {.texture_value = texels[4], .factor = texels[5], .game = texels[6], .metered_stops = texels[1], .target_stops = texels[7]};
    if (auto_exposure_.Observe(sample, seconds)) {
      nr::Log(nr::LogLevel::INFO, AutoLatchMessage(sample, auto_exposure_.StopsOff()));
    }
  }
}

void NrPipeline::RecordMeter(ID3D12GraphicsCommandList* list, uint32_t slot, color::MeterPass pass, const ExposureChoice& exposure) {
  ID3D12Resource* const state = exposure_.state.Get();
  pass.state = state;
  pass.snap = (exposure_.snap || intermediates_.reset_pending);  // the first frame, a settings change, a rebuild
  pass.smooth = config_.fixes.smooth_adapt;
  pass.brighter_rate = config_.fixes.adapt_brighter;
  pass.darker_rate = config_.fixes.adapt_darker;
  pass.frame_seconds = config_.frame_seconds;
  pass.probe = exposure.probe;
  pass.game_exposure = (exposure.probe ? exposure.texture : nullptr);
  pass.game_exposure_factor = (exposure.probe ? exposure.factor : 1.f);
  Transition(list, state, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  color_.RecordMeter(list, slot, pass);
  Transition(list, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PIXEL_AND_COMPUTE_READ);
  exposure_.snap = false;
  exposure_.last_use = slot_marks_[slot];
  if (!exposure.probe) return;
  // Plan 17: Auto's check. The state's two texels into the next readback slot, unless the GPU has not passed that slot's last copy yet (skipped).
  const uint32_t index = check_.next;
  if (check_.pending[index]) return;
  if (!check_.readback) {
    const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC description = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0u,
        .Width = CHECK_SLOTS * CHECK_STRIDE,
        .Height = 1u,
        .DepthOrArraySize = 1u,
        .MipLevels = 1u,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1u, .Quality = 0u},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&check_.readback)))) {
      return;
    }
    check_.readback->SetName(L"Uplift exposure check readback");
  }
  D3D12_TEXTURE_COPY_LOCATION destination = {.pResource = check_.readback.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  destination.PlacedFootprint = {
      .Offset = index * CHECK_STRIDE,
      .Footprint = {.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .Width = STATE_SIZE.width, .Height = STATE_SIZE.height, .Depth = 1u,
                    .RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT},
  };
  const D3D12_TEXTURE_COPY_LOCATION origin = {.pResource = state, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
  Transition(list, state, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&destination, 0u, 0u, 0u, &origin, nullptr);
  Transition(list, state, D3D12_RESOURCE_STATE_COPY_SOURCE, PIXEL_AND_COMPUTE_READ);
  check_.marks[index] = slot_marks_[slot];
  check_.pending[index] = true;
  check_.next = (index + 1u) % CHECK_SLOTS;
}

ID3D12Resource* NrPipeline::BoundMask(uint32_t slot) {
  if (!config_.mask || !mask_.copied) return nullptr;
  mask_.last_use = slot_marks_[slot];
  return mask_.texture.Get();
}

ID3D12Resource* NrPipeline::PrepareMaskCopy(const D3D12_RESOURCE_DESC* mask) {
  const std::optional<color::MaskFormatInfo> format = (mask != nullptr ? color::DescribeMaskFormat(mask->Format) : std::nullopt);
  if (!format || mask->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || mask->SampleDesc.Count != 1u) {
    RetireMask();
    return nullptr;
  }
  const nr::Size size = {static_cast<uint32_t>(mask->Width), mask->Height};
  if (!mask_.texture || mask_.size != size || mask_.format != mask->Format) {
    RetireMask();
    Texture texture = CreateTexture(size, mask->Format, D3D12_RESOURCE_FLAG_NONE, PIXEL_AND_COMPUTE_READ, L"Uplift mask copy");
    if (!texture) return nullptr;
    mask_ = {.texture = std::move(texture), .size = size, .format = mask->Format, .view_format = format->view_format,
             .bytes = size.Pixels() * format->bytes_per_pixel};
  }
  mask_.last_use = timeline_.MarkNow();  // the add-on copies into it on this frame's present-queue list
  return mask_.texture.Get();
}

void NrPipeline::ResolvePass(ID3D12GraphicsCommandList* list, uint32_t index, ID3D12Resource* given, ID3D12Resource* returned) {
  if (index == 0u || index > LATER_PASSES || !color_.SupportsUavLoads()) return;
  const bool swapped = (given != intermediates_.model_a.Get());
  if (const PassStrengths& strengths = config_.later_strengths[index - 1u]; strengths != PassStrengths{}) {
    Transition(list, returned, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordResolve(list, resolve_slot_,
                         {.given = given, .returned = returned, .size = intermediates_.plan.canvas,
                          .transfer_strength = strengths.transfer, .color_strength = strengths.color,
                          .swapped = swapped});
    Transition(list, returned, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  if (faces_ran_) {
    // Keep faces (design "Passes"): inside pass 1's face mask only this pass's broad change is kept.
    RecordFaces(list, {.changed = returned, .reference = given, .mask = faces_.twin.Get(), .swapped = swapped});
  }
}

NrPipeline::AfterNr NrPipeline::RecordAfterNr(ID3D12GraphicsCommandList* list, uint32_t slot, const color::EncodePass& encode,
                                              ID3D12Resource* nr_output, bool reduced, bool look_ready,
                                              color::Upsampling upsampling, const color::StabilizeMotion& motion, bool reset) {
  // At the output size with nothing acting on NR's change: Plan 4's restore, bit for bit (key decision 3).
  if (!reduced && !(look_ready && NeedsChangePath(config_, mask_.copied, color_.SupportsUavLoads()))) return {.texture = nr_output};
  if (look_.plan != LookPlan{}) {
    look_.last_use = slot_marks_[slot];
  }
  ID3D12Resource* const change = (intermediates_.change ? intermediates_.change.Get() : look_.change.Get());
  const bool shape = (look_ready && look_.plan.shape);
  ID3D12Resource* const basis = (shape ? look_.basis.Get() : nullptr);
  // 1. NR's change field (v2 design §3.7) and, for the look, NR's input chroma κ_M.
  Transition(list, change, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (basis != nullptr) {
    Transition(list, basis, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  const bool changed = color_.RecordChange(list, slot, {.encode = encode, .nr_output = nr_output, .change = change, .basis = basis});
  Transition(list, change, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (basis != nullptr) {
    Transition(list, basis, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  if (!changed) return {.ok = false};
  // At the output size the compose reads C texel for texel: Classic at 1:1 samples each texel exactly.
  const AfterNr after = {.texture = change, .upsampling = (reduced ? upsampling : color::Upsampling::CLASSIC)};
  if (!shape) return after;
  // 2. The pyramids.
  const look::Atlas& atlas = look_.atlas;
  const look::Bands bands = look::MakeBands(config_.look.detail_radius, atlas.image.height, atlas.levels);
  ID3D12Resource* const gauss = look_.gauss.Get();
  ID3D12Resource* const peaks = look_.peaks.Get();
  Transition(list, gauss, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(list, peaks, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  color_.RecordPyramid(list, slot, {.change = change, .gauss = gauss, .peaks = peaks, .atlas = atlas});
  Transition(list, gauss, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  Transition(list, peaks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  // 3. The stabiliser (key decision 5): the two levels the low band reads, then the fine band.
  ID3D12Resource* history = nullptr;
  ID3D12Resource* detail = nullptr;
  if (look_.plan.stabilize) {
    const uint32_t current = look_.current;
    ID3D12Resource* const state = look_.state.Get();
    history = look_.history[current].Get();
    color::StabilizePass stabilize = {
        .gauss = gauss, .previous = look_.history[1u - current].Get(), .current = history, .state = state, .atlas = atlas,
        .bands = bands, .rate = look::StabilizeRate(config_.frame_seconds, config_.look.stabilize_ms),
        .motion_mode = (config_.look.stabilize == look::StabilizeMode::MOTION), .reset = (reset || !look_.history_valid),
        .motion = motion,
    };
    Transition(list, history, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, state, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordStabilize(list, slot, stabilize);
    Transition(list, history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    if (look_.plan.detail) {
      detail = look_.detail_history[current].Get();
      stabilize.change = change;
      stabilize.previous = look_.detail_history[1u - current].Get();
      stabilize.current = detail;
      Transition(list, detail, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      color_.RecordDetail(list, slot, stabilize);
      Transition(list, detail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    }
    Transition(list, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    look_.current = 1u - current;
    look_.history_valid = true;
  }
  // 4. The shape pass rewrites C in place.
  Transition(list, change, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  color_.RecordShape(list, slot,
                     {.change = change, .basis = basis, .gauss = gauss, .peaks = peaks, .history = history, .detail = detail,
                      .atlas = atlas, .bands = bands, .settings = config_.look.shape, .sdr = color::IsSdr(encode.encoding)});
  Transition(list, change, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  return after;
}

nr::EvaluateResult NrPipeline::Evaluate(ID3D12GraphicsCommandList* list, uint32_t slot, nr::FrameInputs inputs,
                                        const nr::Controls& controls) {
  if (inputs.motion.resource == nullptr) {
    inputs.motion = {.resource = intermediates_.zero_motion.Get()};
    inputs.motion_scale_x = 1.f;
    inputs.motion_scale_y = 1.f;
  }
  inputs.reset_hint = (inputs.reset_hint || intermediates_.reset_pending);
  // Plan 5 (v2 design §3.10): passes 2..10's own controls, and their own strengths between passes.
  inputs.later_passes = config_.later_controls;
  inputs.resolver = this;
  resolve_slot_ = slot;
  // Keep faces (2026-10-08): pass 1's twin, with its surfaces made before the fit like the set's. Off, unsupported, or paused by the Session (fix round 1,
  // I3), they go; a frame whose surfaces cannot be made only goes without the twin (M4).
  faces_ran_ = false;
  if (config_.keep_faces.enabled && KeepFacesSupported()) {
    inputs.faces = {.wanted = true, .controls = FaceTwinControls(controls, config_.keep_faces.protection),
                    .surface_bytes = FaceSurfaceBytes(intermediates_.plan.canvas)};
    if (session_.FacesPaused()) {
      RetireFaces();
    } else if (EnsureFaces(intermediates_.plan.canvas)) {
      inputs.faces.output = faces_.twin.Get();
      faces_.last_use = slot_marks_[slot];  // fix round 1 (C1): the Session may evaluate the twin into it even when no recombination follows
    }
  } else {
    RetireFaces();
  }
  const uint64_t held = HeldBytes();  // allocated before the fit: charged as the chain's surfaces and credited as held
  const nr::EvaluateResult evaluated = session_.Evaluate(
      list,
      {.a = intermediates_.model_a.Get(), .b = intermediates_.model_b.Get(), .size = intermediates_.plan.canvas,
       .surface_bytes = held, .held_intermediate_bytes = held},
      inputs, controls);
  if (evaluated.output != nullptr) {
    intermediates_.reset_pending = false;  // Plan 3: only a frame that ran NR pays off the owed reset
  }
  if (evaluated.output != nullptr && faces_ran_ && config_.keep_faces.show_mask) {
    // Show the face mask: the mask tinted into NR's result, which the change field and the decode then carry to the image.
    Transition(list, evaluated.output, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordFacesShow(list, slot, evaluated.output, faces_.twin.Get(), intermediates_.plan.canvas);
    Transition(list, evaluated.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  return evaluated;
}

void NrPipeline::NoteEncoding(color::Encoding encoding, float input_scale) {
  // Plan 2 final review M4: NR's history belongs to one encoding and input scale; a change starts it afresh.
  if (encoding != intermediates_.encoding || input_scale != intermediates_.input_scale) {
    intermediates_.encoding = encoding;
    intermediates_.input_scale = input_scale;
    intermediates_.reset_pending = true;
  }
}

void NrPipeline::CommitSlot(uint32_t slot) {
  // The GPU reads this slot's descriptors, and the intermediates, until this recording completes: the
  // current frame, and the token an After-DLSS caller issued before recording.
  slot_marks_[slot] = timeline_.MarkNow();
  intermediates_.last_use = slot_marks_[slot];
  next_slot_ = (slot + 1u) % color::ColorPipeline::RING_SLOTS;
}

bool NrPipeline::Encode(ID3D12GraphicsCommandList* list, uint32_t slot, color::EncodePass pass) {
  ID3D12Resource* const model_a = intermediates_.model_a.Get();
  ID3D12Resource* const zero_motion = intermediates_.zero_motion.Get();
  const bool clear_motion = intermediates_.motion_needs_clear;
  Transition(list, model_a, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (clear_motion) {
    Transition(list, zero_motion, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  pass.model = model_a;
  pass.motion = (clear_motion ? zero_motion : nullptr);
  const bool encoded = color_.RecordEncode(list, slot, pass);
  Transition(list, model_a, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (clear_motion) {
    Transition(list, zero_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    if (encoded) {
      intermediates_.motion_needs_clear = false;
    }
  }
  return encoded;
}

PipelineResult NrPipeline::RecordPresent(ID3D12GraphicsCommandList* list, const PresentTarget& target,
                                         const nr::Controls& controls, bool reset_hint, const WorkLayout& layout) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = "inactive"};
  if (list == nullptr || target.resource == nullptr) return {.reason = "invalid input"};
  const D3D12_RESOURCE_DESC description = target.resource->GetDesc();
  const std::optional<color::FormatInfo> format = color::DescribeFormat(description.Format);
  if (!format) return {.reason = "unsupported format"};
  // Plan 2 final review M5: never run NR on a frame whose result could not be written back.
  if (!color_.HasDecodePipeline(format->target_view_format)) return {.reason = "no pipeline for the target format"};
  const uint64_t frame = timeline_.CurrentFrame();
  if (frame == last_present_frame_) return {.reason = "already ran this frame"};
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = "descriptors busy"};
  const nr::Size size = {static_cast<uint32_t>(description.Width), description.Height};
  const WorkLayout work = Resolve(layout, size);
  if (const std::string_view problem = EnsureIntermediates({
          .output = size,
          .image = work.image,
          .canvas = work.canvas,
          .copy_format = format->copy_format,
          .source_bytes_per_pixel = format->bytes_per_pixel,
          .kind = TargetKind::PRESENT,
      });
      !problem.empty()) {
    return {.reason = problem};
  }
  last_present_frame_ = frame;
  const bool reduced = (work.image != size);
  const bool look_ready = EnsureLook(LookPlanFor(work.image, reduced));  // before the fit, like the set
  CommitSlot(slot);
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(target.encoding, input_scale);
  const uint32_t options = ShaderOptions();
  // v2 design §3.14, key decision 10: the Present path never has the game's own exposure.
  const ExposureChoice exposure = ChooseExposure(target.encoding, nullptr, 1.f);
  ID3D12Resource* const source_copy = intermediates_.source_copy.Get();
  // Amendment 7, deepened by I-2: the copy of the game's motion vectors made for this very frame, when
  // there is one, from the slot the frame it was made for selects (PresentMotionSlotOf).
  nr::FrameInputs inputs = {.ui_correction = config_.present_ui_correction, .reset_hint = reset_hint};
  MotionSource motion_source = MotionSource::NONE;
  std::array<uint64_t, MOTION_COPY_SLOTS> copy_frames = {};
  for (size_t index = 0u; index < MOTION_COPY_SLOTS; ++index) {
    copy_frames[index] = (motion_copies_.textures[index] ? motion_copies_.frames[index] : 0u);
  }
  if (const PresentMotionPick pick = PickPresentMotion(copy_frames, frame); pick.slot) {
    // This frame's copy, else (flicker fix) the newest at most PRESENT_MOTION_MAX_AGE frames older: still DLSS's vectors, so no provider switch.
    const size_t copy = *pick.slot;
    inputs.motion = {.resource = motion_copies_.textures[copy].Get()};  // full subrect, scale (1, 1)
    motion_copies_.last_use[copy] = slot_marks_[slot];
    motion_source = MotionSource::PRESENT_COPY;
  } else if (target.dlss_motion.resource != nullptr && target.dlss_motion_raw) {
    // Plan 18 (fix round 1, M-1): Direct3D 11's ring slot holds DLSS's raw vectors (its rect the evaluate's region, at (0, 0)). They go through the motion
    // copy here, at the region's size with MV.Scale x MotionScale applied, as the Direct3D 12 copy above was made, so every Present source is filtered.
    if (const nr::BoundResource converted =
            ConvertDlssMotion(list, slot, target.dlss_motion, target.dlss_motion_scale_x, target.dlss_motion_scale_y, target.dlss_motion_flip);
        converted.resource != nullptr) {
      inputs.motion = converted;  // full subrect, scale (1, 1)
      motion_source = MotionSource::PRESENT_COPY;
    }
  } else if (target.dlss_motion.resource != nullptr) {
    // Plan 14 (design §2.4): the Vulkan bridge's copy of DLSS's vectors, in the motion region's own pixels (the scale was applied by the copy): the full subrect and a
    // scale of (1, 1), as the copy of the Direct3D 12 path above.
    inputs.motion = target.dlss_motion;
    motion_source = MotionSource::PRESENT_COPY;
  } else if (target.launchpad_motion.resource != nullptr) {
    // Plan 6 (v2 design §3.20): LaunchPad's motion, written by Uplift.fx this frame, in back-buffer pixels. 1.0.1 (F1): never bound as it is. The
    // motion copy resamples it to the work image in its pixels, drops non-finite vectors and clamps the rest (motion_cs.hlsl), as the DLSS copy does,
    // and NR binds the result whole at a scale of (1, 1), right at every Resolution.
    if (const nr::BoundResource converted =
            ConvertLaunchpadMotion(list, slot, target.launchpad_motion, target.motion_scale_x, target.motion_scale_y, work);
        converted.resource != nullptr) {
      inputs.motion = converted;
      motion_source = MotionSource::LAUNCHPAD;
    }
  }
  if (motion_source != MotionSource::LAUNCHPAD && launchpad_motion_.texture) {
    RetireLaunchpadMotion();  // Launchpad's vectors stopped coming: its copy goes, behind the recordings that used it
  }
  inputs.reset_hint = (inputs.reset_hint || motion_source != present_motion_);  // spec §9: a provider switch
  present_motion_ = motion_source;
  // 1. Source copy ← target (Plan 3, unchanged).
  Transition(list, source_copy, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(source_copy, target.resource);
  Transition(list, source_copy, D3D12_RESOURCE_STATE_COPY_DEST, PIXEL_AND_COMPUTE_READ);
  const nr::Rect copied = {.x = 0u, .y = 0u, .width = size.width, .height = size.height};
  if (exposure.metered || exposure.probe) {
    RecordMeter(list, slot,
                {.source = source_copy, .source_view_format = format->source_view_format, .region = copied, .encoding = target.encoding,
                 .primaries = (options & color::shader_options::PRIMARIES_MASK), .input_scale = input_scale},
                exposure);
  }
  // 2. Encode into A, resampled to the work image.
  const color::EncodePass encode = {
      .source = source_copy,
      .source_view_format = format->source_view_format,
      .width = work.image.width,
      .height = work.image.height,
      .encoding = target.encoding,
      .input_scale = input_scale,
      .exposure = exposure.texture,
      .exposure_factor = exposure.factor,
      .region = copied,
      .canvas = work.canvas,
      .options = options,
  };
  if (!Encode(list, slot, encode)) return {.recorded = true, .reason = "encode failed", .motion_source = motion_source};
  // 3. NR.
  const bool look_reset = (inputs.reset_hint || intermediates_.reset_pending);  // Evaluate pays the owed reset off
  const nr::EvaluateResult evaluated = Evaluate(list, slot, inputs, controls);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source};
  }
  // 4. Plan 5: the change field and the look stage, or NR's own output for the full-size restore. The Present
  //    copy is in its own pixels with the full rect: scale (1, 1).
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = "encode failed", .motion_source = motion_source};
  }
  // 5. The decode or compose through an RTV.
  Transition(list, after.texture, COMPUTE_READ, PIXEL_AND_COMPUTE_READ);
  Transition(list, target.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const bool decoded = color_.RecordDecode(
      list, slot,
      {.source = source_copy, .source_view_format = format->source_view_format, .nr_output = after.texture,
       .target = target.resource, .target_view_format = format->target_view_format,
       .target_view_srgb = format->target_view_srgb, .width = size.width, .height = size.height,
       .encoding = target.encoding, .input_scale = input_scale, .transfer_strength = target.transfer_strength,
       .color_strength = target.color_strength, .exposure = exposure.texture, .exposure_factor = exposure.factor,
       .upsampling = after.upsampling, .options = options, .chroma_clamp = config_.fixes.chroma_clamp,
       .mask = BoundMask(slot), .mask_view_format = mask_.view_format});
  Transition(list, target.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(list, after.texture, PIXEL_AND_COMPUTE_READ, COMPUTE_READ);
  if (!decoded) return {.recorded = true, .reason = "no pipeline for the target format", .motion_source = motion_source};
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = SessionWroteMessage(evaluated.reason), .motion_source = motion_source};
}

PipelineResult NrPipeline::RecordUav(ID3D12GraphicsCommandList* list, const UavTarget& target,
                                     const nr::FrameInputs& inputs, const nr::Controls& controls, const WorkLayout& layout) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = "inactive"};
  if (list == nullptr || target.resource == nullptr) return {.reason = "invalid input"};
  const D3D12_RESOURCE_DESC description = target.resource->GetDesc();
  const std::optional<color::UavFormatInfo> format = color::DescribeUavFormat(description.Format);
  // M5 for UAV targets: without typed UAV stores the decode could not write NR's result back.
  if (!format || !color_.SupportsTypedUavStore(format->uav_format)) return {.reason = "unsupported format"};
  nr::Rect region = target.region;
  if (region.width == 0u && region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    return {.reason = "invalid input"};
  }
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = "descriptors busy"};
  const WorkLayout work = Resolve(layout, size);
  if (const std::string_view problem = EnsureIntermediates({
          .output = size,
          .image = work.image,
          .canvas = work.canvas,
          .copy_format = format->copy_format,
          .source_bytes_per_pixel = format->bytes_per_pixel,
          .kind = TargetKind::UAV,
      });
      !problem.empty()) {
    return {.reason = problem};
  }
  const bool reduced = (work.image != size);
  const bool look_ready = EnsureLook(LookPlanFor(work.image, reduced));  // before the fit, like the set
  CommitSlot(slot);
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(target.encoding, input_scale);
  const uint32_t options = ShaderOptions();
  const ExposureChoice exposure = ChooseExposure(target.encoding, target.exposure, target.exposure_factor);
  ID3D12Resource* const source_copy = intermediates_.source_copy.Get();
  const MotionSource motion_source = (inputs.motion.resource != nullptr ? MotionSource::DIRECT : MotionSource::NONE);

  // 1. Source copy ← the region. DLSS leaves its Output in UNORDERED_ACCESS, and so does Uplift.
  Transition(list, target.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(list, source_copy, COMPUTE_READ, D3D12_RESOURCE_STATE_COPY_DEST);
  const D3D12_TEXTURE_COPY_LOCATION destination = {
      .pResource = source_copy, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
  const D3D12_TEXTURE_COPY_LOCATION origin = {
      .pResource = target.resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
  const D3D12_BOX box = {.left = region.x, .top = region.y, .front = 0u, .right = region.x + region.width,
                         .bottom = region.y + region.height, .back = 1u};
  list->CopyTextureRegion(&destination, 0u, 0u, 0u, &origin, &box);
  Transition(list, source_copy, D3D12_RESOURCE_STATE_COPY_DEST, COMPUTE_READ);
  Transition(list, target.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const nr::Rect copied = {.x = 0u, .y = 0u, .width = size.width, .height = size.height};
  if (exposure.metered || exposure.probe) {
    RecordMeter(list, slot,
                {.source = source_copy, .source_view_format = format->source_view_format, .region = copied, .encoding = target.encoding,
                 .primaries = (options & color::shader_options::PRIMARIES_MASK), .input_scale = input_scale},
                exposure);
  }
  // 2. Encode into A with the game's exposure, resampled to the work image.
  const color::EncodePass encode = {
      .source = source_copy,
      .source_view_format = format->source_view_format,
      .width = work.image.width,
      .height = work.image.height,
      .encoding = target.encoding,
      .input_scale = input_scale,
      .exposure = exposure.texture,
      .exposure_factor = exposure.factor,
      .region = copied,
      .canvas = work.canvas,
      .options = options,
  };
  if (!Encode(list, slot, encode)) return {.recorded = true, .reason = "encode failed", .motion_source = motion_source};
  // 3. The NR passes, with the game's motion vectors bound in place.
  const bool look_reset = (inputs.reset_hint || intermediates_.reset_pending);  // Evaluate pays the owed reset off
  const nr::EvaluateResult evaluated = Evaluate(list, slot, inputs, controls);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 4. Plan 5: the change field and the look stage, or NR's own output for the full-size restore.
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = "encode failed", .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 5. The compute decode into the region, then a UAV barrier so the game's next reads see NR's result.
  const bool decoded = color_.RecordComputeDecode(
      list, slot,
      {.source = source_copy, .source_view_format = format->source_view_format, .nr_output = after.texture,
       .target = target.resource, .target_uav_format = format->uav_format, .origin_x = region.x, .origin_y = region.y,
       .width = size.width, .height = size.height, .encoding = target.encoding, .input_scale = input_scale,
       .transfer_strength = target.transfer_strength, .color_strength = target.color_strength,
       .exposure = exposure.texture, .exposure_factor = exposure.factor, .source_x = 0u, .source_y = 0u,
       .upsampling = after.upsampling, .options = options, .chroma_clamp = config_.fixes.chroma_clamp,
       .mask = BoundMask(slot), .mask_view_format = mask_.view_format});
  UavBarrier(list, target.resource);
  if (!decoded) {
    return {.recorded = true, .reason = "decode failed", .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = SessionWroteMessage(evaluated.reason), .motion_source = motion_source,
          .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

PipelineResult NrPipeline::RecordPreSr(ID3D12GraphicsCommandList* list, const UavTarget& color, const nr::FrameInputs& inputs,
                                       const nr::Controls& controls, const WorkLayout& layout) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = "inactive"};
  if (list == nullptr || color.resource == nullptr) return {.reason = "invalid input"};
  const D3D12_RESOURCE_DESC description = color.resource->GetDesc();
  const std::optional<color::UavFormatInfo> format = color::DescribeUavFormat(description.Format);
  // v2 design §3.9 step 1: a 2D, single-sample texture Uplift reads through a typed, non-sRGB view.
  if (!format || description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1u
      || description.SampleDesc.Count != 1u) {
    return {.reason = "unsupported format"};
  }
  const nr::Rect region = color.region;
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    return {.reason = "invalid input"};
  }
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = "descriptors busy"};
  const WorkLayout work = Resolve(layout, size);
  // In a canvas, readable motion vectors are copied into it; unconverted ones cannot be bound there (E9).
  const std::optional<DXGI_FORMAT> motion_format =
      ((work.canvas != work.image && inputs.motion.resource != nullptr) ? MotionViewFormat(inputs.motion.resource->GetDesc().Format)
                                                                        : std::nullopt);
  // I-1 (Plan 4 fix round 4): keyed on the settled work image, never the per-frame render region --
  // a game with dynamic resolution sets `size` on almost every evaluate, and the old `.output = size`
  // reallocated (and restarted NR's history on) the whole set for each step. EnsureIntermediates always
  // allocates the change texture for PRE_SR now (its own `reduced` computation), so the set is stable
  // here even though `size` still varies frame to frame.
  if (const std::string_view problem = EnsureIntermediates({
          .output = work.image,
          .image = work.image,
          .canvas = work.canvas,
          .kind = TargetKind::PRE_SR,
          .private_color = {static_cast<uint32_t>(description.Width), description.Height},
          .canvas_motion = motion_format.has_value(),
      });
      !problem.empty()) {
    return {.reason = problem};
  }
  const bool reduced = (work.image != size);
  const bool look_ready = EnsureLook(LookPlanFor(work.image, reduced));  // before the fit, like the set; I-1: C stays main-set
  CommitSlot(slot);
  const float input_scale = color::InputScale(color.encoding, color.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(color.encoding, input_scale);
  const uint32_t options = ShaderOptions();
  const ExposureChoice exposure = ChooseExposure(color.encoding, color.exposure, color.exposure_factor);
  if (exposure.metered || exposure.probe) {
    RecordMeter(list, slot,
                {.source = color.resource, .source_view_format = format->source_view_format, .region = region, .encoding = color.encoding,
                 .primaries = (options & color::shader_options::PRIMARIES_MASK), .input_scale = input_scale},
                exposure);
  }
  // 1. Encode the region straight from the game's Color: it is only read, never copied or transitioned.
  const color::EncodePass encode = {
      .source = color.resource,
      .source_view_format = format->source_view_format,
      .width = work.image.width,
      .height = work.image.height,
      .encoding = color.encoding,
      .input_scale = input_scale,
      .exposure = exposure.texture,
      .exposure_factor = exposure.factor,
      .region = region,
      .canvas = work.canvas,
      .options = options,
  };
  if (!Encode(list, slot, encode)) return {.recorded = true, .reason = "encode failed"};
  // 2. v2 design §3.9 step 4: in a canvas the motion vectors move into canvas pixels, mirrored like the
  //    image, bound with the canvas as their subrect and a scale of (1, 1).
  nr::FrameInputs nr_inputs = inputs;
  if (motion_format) {
    nr::Rect motion_region = inputs.motion.rect;
    if (motion_region.width == 0u || motion_region.height == 0u) {
      const D3D12_RESOURCE_DESC motion = inputs.motion.resource->GetDesc();
      motion_region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(motion.Width), .height = motion.Height};
    }
    ID3D12Resource* const canvas_motion = intermediates_.canvas_motion.Get();
    Transition(list, canvas_motion, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordMotion(list, slot,
                        {
                            .source = inputs.motion.resource,
                            .source_view_format = *motion_format,
                            .region = motion_region,
                            .target = canvas_motion,
                            .image = work.image,
                            .canvas = work.canvas,
                            .scale_x = inputs.motion_scale_x * static_cast<float>(work.image.width) / static_cast<float>(motion_region.width),
                            .scale_y = inputs.motion_scale_y * static_cast<float>(work.image.height) / static_cast<float>(motion_region.height),
                        });
    Transition(list, canvas_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    nr_inputs.motion = {.resource = canvas_motion};
    nr_inputs.motion_scale_x = 1.f;
    nr_inputs.motion_scale_y = 1.f;
  } else if (work.canvas != work.image) {
    nr_inputs.motion = {};  // an unreadable format in a canvas: the zero texture
  }
  // Reported as bound in place either way: a canvas copy still carries the game's own motion vectors,
  // scaled by what the caller asked for (inputs.motion_scale_x/y), not nr_inputs' own post-copy (1, 1).
  const MotionSource motion_source = (nr_inputs.motion.resource != nullptr ? MotionSource::DIRECT : MotionSource::NONE);
  // 3. NR on the canvas (or the image).
  const bool look_reset = (inputs.reset_hint || intermediates_.reset_pending);  // Evaluate pays the owed reset off
  const nr::EvaluateResult evaluated = Evaluate(list, slot, nr_inputs, controls);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 4. Plan 5: the change field and the look stage, or NR's own output for the full-size restore. The stabiliser
  // runs on the game's own vectors (`inputs`, not `nr_inputs`), since it acts on the image, not the canvas.
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = "encode failed", .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 5. The compose into the private colour's region.
  ID3D12Resource* const private_color = intermediates_.private_color.Get();
  Transition(list, private_color, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const bool composed = color_.RecordComputeDecode(
      list, slot,
      {
          .source = color.resource,
          .source_view_format = format->source_view_format,
          .nr_output = after.texture,
          .target = private_color,
          .target_uav_format = DXGI_FORMAT_R16G16B16A16_FLOAT,
          .origin_x = region.x,
          .origin_y = region.y,
          .width = size.width,
          .height = size.height,
          .encoding = color.encoding,
          .input_scale = input_scale,
          .transfer_strength = color.transfer_strength,
          .color_strength = color.color_strength,
          .exposure = exposure.texture,
          .exposure_factor = exposure.factor,
          .source_x = region.x,
          .source_y = region.y,
          .upsampling = after.upsampling,
          .options = options,
          .chroma_clamp = config_.fixes.chroma_clamp,
          .mask = BoundMask(slot),
          .mask_view_format = mask_.view_format,
      });
  // DLSS reads it next, in NON_PIXEL_SHADER_RESOURCE; leaving UNORDERED_ACCESS also finishes the writes.
  Transition(list, private_color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (!composed) {
    return {.recorded = true, .reason = "decode failed", .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = SessionWroteMessage(evaluated.reason), .motion_source = motion_source,
          .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

bool NrPipeline::RecordMotionCopy(ID3D12GraphicsCommandList* list, const nr::BoundResource& motion, float scale_x, float scale_y, bool flip_y) {
  if (session_.State() != nr::SessionState::ACTIVE || list == nullptr || motion.resource == nullptr) return false;
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  nr::Rect region = motion.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!view_format || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    return false;
  }
  // The present that closes the current frame binds the copy made for it (RecordPresent).
  const uint64_t frame = timeline_.CurrentFrame() + 1u;
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return false;
  const nr::Size size = {region.width, region.height};
  if (motion_copies_.size != size || !motion_copies_.textures[0]) {
    RetireMotionCopies();
    if (!MakeMotionCopies(0u, PRESENT_MOTION_MIN_SLOTS, size)) {
      motion_copies_ = {};
      return false;
    }
    motion_copies_.size = size;
  }
  const size_t index = PresentMotionSlotOf(frame, motion_copies_.slots);
  if (!timeline_.IsComplete(motion_copies_.last_use[index])) {
    // Fix round 1 (M6): the GPU runs too far behind for four slots (a lag past 3): the ring grows once, its new copies made beside the first ones (none of those is freed or
    // moved). This frame goes without its copy; its present binds an older one.
    if (PresentMotionGrows(motion_copies_.slots, PresentMotionWrite::SLOT_BUSY) && MakeMotionCopies(motion_copies_.slots, MOTION_COPY_SLOTS, size)) {
      nr::Logf(nr::LogLevel::INFO, "Direct3D 12: the GPU runs too far behind for four slots: DLSS's motion copies for Present grow from {} to {} slots",
               motion_copies_.slots, MOTION_COPY_SLOTS);
      motion_copies_.slots = MOTION_COPY_SLOTS;
    }
    return false;
  }
  CommitSlot(slot);
  ID3D12Resource* const target = motion_copies_.textures[index].Get();
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  color_.RecordMotion(list, slot,
                      {
                          .source = motion.resource,
                          .source_view_format = *view_format,
                          .region = region,
                          .target = target,
                          .image = size,
                          .canvas = size,
                          .scale_x = scale_x,
                          .scale_y = scale_y,
                          .flip_y = flip_y,
                      });
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  motion_copies_.frames[index] = frame;
  motion_copies_.last_use[index] = slot_marks_[slot];
  return true;
}

nr::BoundResource NrPipeline::ConvertDlssMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const nr::BoundResource& motion, float scale_x,
                                                float scale_y, bool flip_y) {
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  nr::Rect region = motion.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!view_format || uint64_t{region.x} + region.width > description.Width || uint64_t{region.y} + region.height > description.Height) return {};
  const nr::Size size = {region.width, region.height};
  if (dlss_present_motion_.size != size || !dlss_present_motion_.texture) {
    RetireDlssPresentMotion();
    dlss_present_motion_.texture = CreateTexture(size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ,
                                                 L"Uplift DLSS motion for Present");
    if (!dlss_present_motion_.texture) {
      dlss_present_motion_ = {};
      return {};
    }
    dlss_present_motion_.size = size;
  }
  ID3D12Resource* const target = dlss_present_motion_.texture.Get();
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const bool recorded = color_.RecordMotion(list, slot,
                                            {
                                                .source = motion.resource,
                                                .source_view_format = *view_format,
                                                .region = region,
                                                .target = target,
                                                .image = size,
                                                .canvas = size,
                                                .scale_x = scale_x,
                                                .scale_y = scale_y,
                                                .flip_y = flip_y,
                                            });
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  dlss_present_motion_.last_use = slot_marks_[slot];
  if (!recorded) return {};  // nothing was written: NR runs with the zero motion, never with this texture's old or undefined contents
  return {.resource = target};
}

nr::BoundResource NrPipeline::ConvertLaunchpadMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const nr::BoundResource& motion, float scale_x,
                                                     float scale_y, const WorkLayout& work) {
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  nr::Rect region = motion.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!view_format || region.width == 0u || region.height == 0u || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height || work.image.Empty() || work.canvas.Empty()) {
    return {};
  }
  if (launchpad_motion_.size != work.canvas || !launchpad_motion_.texture) {
    RetireLaunchpadMotion();
    launchpad_motion_.texture = CreateTexture(work.canvas, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ,
                                              L"Uplift Launchpad motion");
    if (!launchpad_motion_.texture) {
      launchpad_motion_ = {};
      return {};
    }
    launchpad_motion_.size = work.canvas;
  }
  ID3D12Resource* const target = launchpad_motion_.texture.Get();
  Transition(list, target, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // UPLIFT_MV holds back-buffer (region) pixels: times MotionScale, and times image / region into the work image's pixels (Resolution below Full).
  const bool recorded = color_.RecordMotion(list, slot,
                                            {
                                                .source = motion.resource,
                                                .source_view_format = *view_format,
                                                .region = region,
                                                .target = target,
                                                .image = work.image,
                                                .canvas = work.canvas,
                                                .scale_x = scale_x * static_cast<float>(work.image.width) / static_cast<float>(region.width),
                                                .scale_y = scale_y * static_cast<float>(work.image.height) / static_cast<float>(region.height),
                                            });
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  launchpad_motion_.last_use = slot_marks_[slot];
  // Fix round 1, minor 4: nothing was written (a slot out of range): NR runs with the zero motion, never with this texture's old or undefined contents.
  if (!recorded) return {};
  return {.resource = target};
}

}  // namespace uplift::sources
