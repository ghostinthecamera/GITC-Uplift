#include "sources/nr_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

#include "nr/log.hpp"
#include "sources/look_plan.hpp"
#include "sources/pipeline_rules.hpp"

namespace uplift::sources {
namespace {

// The Present decode is a pixel shader; everything else reads through compute-legal states only.
constexpr D3D12_RESOURCE_STATES PIXEL_AND_COMPUTE_READ =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES COMPUTE_READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t MODEL_BYTES_PER_PIXEL = 8u;   // RGBA16F
constexpr uint32_t MOTION_BYTES_PER_PIXEL = 4u;  // RG16F
constexpr uint64_t METER_STATE_BYTES = 32u;  // the meter's 2x1 RGBA32F state texture (the second texel is Auto's check)
constexpr nr::Size STATE_SIZE = {2u, 1u};
// The look's scene-cut state: one RGBA32F texel.
constexpr nr::Size LOOK_STATE_SIZE = {1u, 1u};
constexpr uint32_t LOOK_STATE_BYTES_PER_PIXEL = 16u;
// One readback slot per placed footprint of the 2x1 state (512 B apart, a 256 B row).
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

}  // namespace

color::StabilizeMotion StabilizeMotionOf(const nr::BoundResource& motion, float scale_x, float scale_y) {
  if (motion.resource == nullptr) return {};
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  const std::optional<nr::Rect> rect = MotionRectOf(motion.rect, {static_cast<uint32_t>(description.Width), description.Height});
  if (!view_format || !rect) return {};
  return {.vectors = {.resource = motion.resource, .rect = *rect}, .view_format = *view_format, .scale_x = scale_x, .scale_y = scale_y};
}

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
  const uint64_t ring = (dlss_present_motion_.texture ? dlss_present_motion_.size.Pixels() * MOTION_BYTES_PER_PIXEL : 0u);
  // Split chain: every stage set's surfaces count.
  uint64_t sets = 0u;
  for (const StageSet& set : sets_) {
    sets += (set.intermediates.model_a ? set.intermediates.bytes : 0u) + set.look.bytes + (set.exposure.state ? METER_STATE_BYTES : 0u) + set.faces.bytes;
  }
  return sets + copies + launchpad + ring + mask_.bytes;
}

bool NrPipeline::SupportsUavTarget(DXGI_FORMAT format) const {
  const std::optional<color::UavFormatInfo> info = color::DescribeUavFormat(format);
  return info.has_value() && color_.SupportsTypedUavStore(info->uav_format);
}

void NrPipeline::ReleaseIntermediates() {
  RetireSetsFrom(0u);
  RetireMotionCopies();
  RetireLaunchpadMotion();
  RetireDlssPresentMotion();
  RetireMask();
}

void NrPipeline::RetireSetsFrom(uint32_t first) {
  for (uint32_t index = first; index < STAGE_SETS; ++index) {
    StageSet* const set = &sets_[index];
    RetireSet(set);
    RetireLook(set);
    RetireFaces(set);
    RetireExposure(set);
  }
}

void NrPipeline::RetireSet(StageSet* set) {
  if (!set->intermediates.model_a) return;
  // Every recording takes its mark before its first command, so once the GPU has passed
  // the newest one the set goes at once; otherwise the callback's copy keeps it alive until then.
  if (!timeline_.IsComplete(set->intermediates.last_use)) {
    timeline_.ReleaseAfter(set->intermediates.last_use, [retired = set->intermediates] {});
  }
  set->intermediates = {};
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

void NrPipeline::RetireLook(StageSet* set) {
  if (set->look.plan == LookPlan{}) return;
  if (!timeline_.IsComplete(set->look.last_use)) {
    timeline_.ReleaseAfter(set->look.last_use, [retired = set->look] {});
  }
  set->look = {};
}

void NrPipeline::RetireFaces(StageSet* set) {
  if (!set->faces.twin) return;
  if (!timeline_.IsComplete(set->faces.last_use)) {
    timeline_.ReleaseAfter(set->faces.last_use, [retired = set->faces] {});
  }
  set->faces = {};
}

bool NrPipeline::EnsureFaces(nr::Size canvas) {
  if (set_->faces.twin && set_->faces.size == canvas) return true;
  RetireFaces(set_);
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
  set_->faces = std::move(created);
  return true;
}

void NrPipeline::RecordFaces(ID3D12GraphicsCommandList* list, color::FacesPass pass) {
  ID3D12Resource* const atlas = set_->faces.atlas.Get();
  pass.atlas = atlas;
  pass.detail = set_->faces.detail.Get();
  pass.fill = set_->faces.fill.Get();
  pass.layout = set_->faces.layout;
  pass.parameters = look::MakeFacesParameters(config_.keep_faces.lighting_scale, config_.keep_faces.tuning, set_->intermediates.plan.sizes.image.height, set_->faces.layout.levels);
  set_->faces.last_use = slot_marks_[resolve_slot_];
  const bool fill = (pass.mask == nullptr && pass.parameters.fill);  // pass 1's pyramid writes the fill's too
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
    // The difference despiked before the combine rewrites `changed` (and pass 1's twin) in place.
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

void NrPipeline::RetireExposure(StageSet* set) {
  if (set->exposure.state && !timeline_.IsComplete(set->exposure.last_use)) {
    timeline_.ReleaseAfter(set->exposure.last_use, [retired = set->exposure.state] {});
  }
  set->exposure = {};
  RetireExposureCheck(set);
}

void NrPipeline::RetireExposureCheck(StageSet* set) {
  // The samples still in flight are dropped (Auto's run starts again with the next ones); the buffer goes once the GPU has passed its copies.
  for (uint32_t index = 0u; index < CHECK_SLOTS; ++index) {
    if (set->check.readback && set->check.pending[index] && !timeline_.IsComplete(set->check.marks[index])) {
      timeline_.ReleaseAfter(set->check.marks[index], [retired = set->check.readback] {});
    }
  }
  set->check = {};
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

nr::SkipReason NrPipeline::EnsureIntermediates(const SetPlan& plan) {
  if (set_->intermediates.model_a && set_->intermediates.plan == plan) return nr::SkipReason::NONE;
  RetireSet(set_);
  constexpr D3D12_RESOURCE_FLAGS UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const bool pre_sr = (plan.kind == TargetKind::PRE_SR);
  Intermediates created = {
      .source_copy = (plan.copy_format == DXGI_FORMAT_UNKNOWN
                          ? Texture()
                          : CreateTexture(plan.sizes.output, plan.copy_format, D3D12_RESOURCE_FLAG_NONE,
                                          (plan.kind == TargetKind::PRESENT ? PIXEL_AND_COMPUTE_READ : COMPUTE_READ),
                                          L"Uplift source copy")),
      .model_a = CreateTexture(plan.sizes.canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift model A"),
      .model_b = CreateTexture(plan.sizes.canvas, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift model B"),
      .zero_motion = CreateTexture(plan.sizes.canvas, DXGI_FORMAT_R16G16_FLOAT, UAV, COMPUTE_READ, L"Uplift zero motion"),
      .change = CreateTexture(plan.sizes.image, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ, L"Uplift change field"),
      .canvas_motion = (plan.canvas_motion
                            ? CreateTexture(plan.sizes.canvas, DXGI_FORMAT_R16G16_FLOAT, UAV, COMPUTE_READ, L"Uplift canvas motion")
                            : Texture()),
      .private_color = (pre_sr ? CreateTexture(plan.sizes.private_color, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, COMPUTE_READ,
                                               L"Uplift pre-SR colour")
                               : Texture()),
      .plan = plan,
      .bytes = plan.sizes.output.Pixels() * plan.source_bytes_per_pixel
               + plan.sizes.canvas.Pixels() * (2u * MODEL_BYTES_PER_PIXEL + MOTION_BYTES_PER_PIXEL)
               + plan.sizes.image.Pixels() * MODEL_BYTES_PER_PIXEL
               + (plan.canvas_motion ? plan.sizes.canvas.Pixels() * MOTION_BYTES_PER_PIXEL : 0u)
               + plan.sizes.private_color.Pixels() * MODEL_BYTES_PER_PIXEL,
  };
  const bool complete = created.model_a && created.model_b && created.zero_motion
                        && (plan.copy_format == DXGI_FORMAT_UNKNOWN || created.source_copy) && created.change
                        && (!plan.canvas_motion || created.canvas_motion) && (!pre_sr || created.private_color);
  if (!complete) {
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} NR surfaces", plan.sizes.canvas.width, plan.sizes.canvas.height);
      allocation_failure_logged_ = true;
    }
    return nr::SkipReason::OUT_OF_MEMORY;
  }
  allocation_failure_logged_ = false;
  set_->intermediates = std::move(created);
  return nr::SkipReason::NONE;
}

void NrPipeline::SetLookConfig(const LookConfig& config) {
  // A new Detail radius or Stabilize mode starts the stabiliser afresh. Split chain: in every set.
  const bool restart_stabilizer = (config.look.detail_radius != config_.look.detail_radius || config.look.stabilize != config_.look.stabilize
                                   || config.look.stabilize_detail != config_.look.stabilize_detail);
  for (StageSet& set : sets_) {
    if (restart_stabilizer) {
      set.look.history_valid = false;
    }
  }
  config_ = config;
}

LookPlan NrPipeline::LookPlanFor(nr::Size image) const {
  return PlanLook(config_, image, color_.SupportsUavLoads());
}

bool NrPipeline::EnsureLook(const LookPlan& plan) {
  if (plan == set_->look.plan) return true;
  RetireLook(set_);
  if (plan == LookPlan{}) return true;
  LookSurfaces created = {.plan = plan, .atlas = (plan.shape ? look::MakeAtlas(plan.image) : look::Atlas{})};
  const auto make = [this, &created](bool wanted, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, const wchar_t* name) {
    if (!wanted) return Texture();
    created.bytes += size.Pixels() * bytes_per_pixel;
    return CreateTexture(size, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, COMPUTE_READ, name);
  };
  created.basis = make(plan.shape, plan.image, DXGI_FORMAT_R16G16_FLOAT, 4u, L"Uplift look basis");
  created.gauss = make(plan.shape, created.atlas.size, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look pyramid");
  created.peaks = make(plan.shape, created.atlas.size, DXGI_FORMAT_R16_FLOAT, 2u, L"Uplift look peaks");
  for (Texture& history : created.history) {
    history = make(plan.stabilize, created.atlas.size, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look history");
  }
  for (Texture& history : created.detail_history) {
    history = make(plan.detail, plan.image, DXGI_FORMAT_R16G16B16A16_FLOAT, 8u, L"Uplift look detail history");
  }
  created.state = make(plan.stabilize, LOOK_STATE_SIZE, DXGI_FORMAT_R32G32B32A32_FLOAT, LOOK_STATE_BYTES_PER_PIXEL, L"Uplift look state");
  const bool complete = (!plan.shape || (created.basis && created.gauss && created.peaks))
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
  set_->look = std::move(created);
  return true;
}

void NrPipeline::NoteMeterBasis(uint8_t stage, color::Encoding encoding, float input_scale) {
  const MeterBasis basis = {.stage = stage, .encoding = encoding, .primaries = color::ResolvePrimaries(config_.fixes.primaries, encoding), .input_scale = input_scale};
  if (set_->exposure.basis == basis) return;
  set_->exposure.basis = basis;
  set_->exposure.snap = true;
  RetireExposureCheck(set_);
}

NrPipeline::ExposureChoice NrPipeline::ChooseExposure(color::Encoding encoding, ID3D12Resource* game_texture, float game_factor) {
  PollExposureChecks();
  const bool game_exposure = (game_texture != nullptr || game_factor != 1.f);
  // Only relative scene-linear images are metered, and the meter needs typed UAV loads.
  const bool can_meter = (color::Meterable(encoding) && color_.SupportsUavLoads());
  const ExposureDecision decision = DecideExposure(config_.fixes, game_exposure, can_meter, set_->auto_exposure.Latched());
  if (!decision.WantsState() && set_->exposure.state) {
    RetireExposure(set_);  // InputExposure left Metered/auto-metered; the state is no longer read
  }
  if (decision.WantsState() && !set_->exposure.state) {
    set_->exposure.state = CreateTexture(STATE_SIZE, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, PIXEL_AND_COMPUTE_READ,
                                    L"Uplift exposure state");
    set_->exposure.snap = true;  // undefined until the meter's first snap
  }
  const ExposureOutcome outcome = PickExposure(decision, config_.fixes, game_exposure, static_cast<bool>(set_->exposure.state), set_->auto_exposure);
  set_->exposure_report = outcome.report;
  ExposureChoice choice = {.metered = outcome.metered, .probe = outcome.probe, .game_weight = outcome.game_weight};
  switch (outcome.source) {
    case ExposureSource::STATE: choice.texture = set_->exposure.state.Get(); break;
    case ExposureSource::GAME:
      choice.texture = game_texture;
      choice.factor = game_factor;
      break;
    case ExposureSource::NONE: break;
  }
  if (outcome.with_game) {
    choice.game_texture = game_texture;
    choice.game_factor = game_factor;
  }
  return choice;
}

void NrPipeline::PollExposureChecks() {
  if (!set_->check.readback) return;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  for (uint32_t step = 0u; step < CHECK_SLOTS; ++step) {
    const uint32_t index = (set_->check.next + step) % CHECK_SLOTS;  // the oldest first
    if (!set_->check.pending[index] || !timeline_.IsComplete(set_->check.marks[index])) continue;
    set_->check.pending[index] = false;
    std::array<float, METER_SAMPLE_FLOATS> texels = {};
    const D3D12_RANGE read = {.Begin = index * CHECK_STRIDE, .End = index * CHECK_STRIDE + sizeof(texels)};
    void* mapped = nullptr;
    if (FAILED(set_->check.readback->Map(0u, &read, &mapped))) continue;
    std::memcpy(texels.data(), static_cast<const std::byte*>(mapped) + read.Begin, sizeof(texels));
    const D3D12_RANGE written = {.Begin = 0u, .End = 0u};
    set_->check.readback->Unmap(0u, &written);
    const ExposureSample sample = ExposureSampleFromTexels(texels);
    if (set_->auto_exposure.Observe(sample, seconds)) {
      nr::Log(nr::LogLevel::INFO, AutoLatchMessage(sample, set_->auto_exposure));
    } else if (!set_->auto_exposure.Latched() && std::isfinite(AutoExposure::OutsideSpan(sample)) && set_->auto_exposure.DebugDue(seconds)) {
      nr::Log(nr::LogLevel::TRACE, AutoDebugMessage(sample, config_.fixes.auto_mode, config_.fixes.auto_blend, set_->auto_exposure.HeldThroughMove()));  // once every AutoExposure::DEBUG_SECONDS
    }
  }
}

void NrPipeline::RecordMeter(ID3D12GraphicsCommandList* list, uint32_t slot, color::MeterPass pass, const ExposureChoice& exposure) {
  ID3D12Resource* const state = set_->exposure.state.Get();
  pass.state = state;
  pass.snap = set_->exposure.snap;
  pass.smooth = config_.fixes.smooth_adapt;
  pass.brighter_rate = config_.fixes.adapt_brighter;
  pass.darker_rate = config_.fixes.adapt_darker;
  pass.frame_seconds = config_.frame_seconds;
  pass.probe = exposure.probe;
  pass.game_exposure = (exposure.probe ? exposure.game_texture : nullptr);
  pass.game_exposure_factor = (exposure.probe ? exposure.game_factor : 1.f);
  pass.game_weight = (exposure.probe ? exposure.game_weight : 0.f);
  Transition(list, state, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // Never a failed dispatch here: ColorPipeline::RecordMeter fails only for a slot outside the ring, which next_slot_ never is, and its one meter pipeline
  // exists once Initialize succeeded. Vulkan's meter pipelines are per source format and may be missing, so only there does a Metered recording abort.
  color_.RecordMeter(list, slot, pass);
  Transition(list, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PIXEL_AND_COMPUTE_READ);
  set_->exposure.snap = false;
  set_->exposure.last_use = slot_marks_[slot];
  if (!exposure.probe) return;
  // Auto's check. The state's two texels into the next readback slot, unless the GPU has not passed that slot's last copy yet (skipped).
  const uint32_t index = set_->check.next;
  if (set_->check.pending[index]) return;
  if (!set_->check.readback) {
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
                                                IID_PPV_ARGS(&set_->check.readback)))) {
      return;
    }
    set_->check.readback->SetName(L"Uplift exposure check readback");
  }
  D3D12_TEXTURE_COPY_LOCATION destination = {.pResource = set_->check.readback.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  destination.PlacedFootprint = {
      .Offset = index * CHECK_STRIDE,
      .Footprint = {.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .Width = STATE_SIZE.width, .Height = STATE_SIZE.height, .Depth = 1u,
                    .RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT},
  };
  const D3D12_TEXTURE_COPY_LOCATION origin = {.pResource = state, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
  Transition(list, state, PIXEL_AND_COMPUTE_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&destination, 0u, 0u, 0u, &origin, nullptr);
  Transition(list, state, D3D12_RESOURCE_STATE_COPY_SOURCE, PIXEL_AND_COMPUTE_READ);
  set_->check.marks[index] = slot_marks_[slot];
  set_->check.pending[index] = true;
  set_->check.next = (index + 1u) % CHECK_SLOTS;
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
  const bool swapped = (given != set_->intermediates.model_a.Get());
  if (const PassStrengths& strengths = config_.later_strengths[index - 1u]; strengths != PassStrengths{}) {
    Transition(list, returned, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordResolve(list, resolve_slot_,
                         {.given = given, .returned = returned, .size = set_->intermediates.plan.sizes.canvas,
                          .transfer_strength = strengths.transfer, .color_strength = strengths.color,
                          .swapped = swapped});
    Transition(list, returned, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  if (faces_ran_ && index != faces_first_) {
    // Keep faces: inside pass 1's face mask only this pass's broad change is kept. Split chain: a later group's own mask,
    // from its first pass's twin, whose recombination already made that pass's result.
    RecordFaces(list, {.changed = returned, .reference = given, .mask = set_->faces.twin.Get(), .swapped = swapped});
  }
}

NrPipeline::AfterNr NrPipeline::RecordAfterNr(ID3D12GraphicsCommandList* list, uint32_t slot, const color::EncodePass& encode,
                                              ID3D12Resource* nr_output, uint32_t passes_run, bool reduced, bool look_ready,
                                              color::Upsampling upsampling, const color::StabilizeMotion& motion, bool reset) {
  if (set_->look.plan != LookPlan{}) {
    set_->look.last_use = slot_marks_[slot];
  }
  ID3D12Resource* const change = set_->intermediates.change.Get();
  const bool shape = (look_ready && set_->look.plan.shape);
  ID3D12Resource* const basis = (shape ? set_->look.basis.Get() : nullptr);
  // 1. NR's change field and, for the look, NR's input chroma κ_M.
  Transition(list, change, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (basis != nullptr) {
    Transition(list, basis, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  // A still holds the original FP16 input after one pass; later passes overwrite it through ping-pong.
  const bool changed = color_.RecordChange(list, slot, {.encode = encode, .nr_output = nr_output, .change = change, .basis = basis,
                                                       .model_input = (passes_run == 1u ? set_->intermediates.model_a.Get() : nullptr)});
  Transition(list, change, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  if (basis != nullptr) {
    Transition(list, basis, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  if (!changed) return {.ok = false};
  const AfterNr after = {.texture = change, .upsampling = (reduced ? std::optional{upsampling} : std::nullopt)};
  if (!shape) return after;
  // 2. The pyramids.
  const look::Atlas& atlas = set_->look.atlas;
  const look::Bands bands = look::MakeBands(config_.look.detail_radius, atlas.image.height, atlas.levels);
  ID3D12Resource* const gauss = set_->look.gauss.Get();
  ID3D12Resource* const peaks = set_->look.peaks.Get();
  Transition(list, gauss, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(list, peaks, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  color_.RecordPyramid(list, slot, {.change = change, .gauss = gauss, .peaks = peaks, .atlas = atlas});
  Transition(list, gauss, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  Transition(list, peaks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  // 3. The stabiliser: the two levels the low band reads, then the fine band.
  ID3D12Resource* history = nullptr;
  ID3D12Resource* detail = nullptr;
  if (set_->look.plan.stabilize) {
    const uint32_t current = set_->look.current;
    ID3D12Resource* const state = set_->look.state.Get();
    history = set_->look.history[current].Get();
    color::StabilizePass stabilize = {
        .gauss = gauss, .previous = set_->look.history[1u - current].Get(), .current = history, .state = state, .atlas = atlas,
        .bands = bands, .rate = look::StabilizeRate(config_.frame_seconds, config_.look.stabilize_ms),
        .motion_mode = (config_.look.stabilize == look::StabilizeMode::MOTION), .reset = (reset || !set_->look.history_valid),
        .motion = motion,
    };
    Transition(list, history, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, state, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordStabilize(list, slot, stabilize);
    Transition(list, history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    if (set_->look.plan.detail) {
      detail = set_->look.detail_history[current].Get();
      stabilize.change = change;
      stabilize.previous = set_->look.detail_history[1u - current].Get();
      stabilize.current = detail;
      Transition(list, detail, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      color_.RecordDetail(list, slot, stabilize);
      Transition(list, detail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    }
    Transition(list, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
    set_->look.current = 1u - current;
    set_->look.history_valid = true;
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
                                        const nr::Controls& controls, const nr::StageSlice& slice) {
  if (inputs.motion.resource == nullptr) {
    inputs.motion = {.resource = set_->intermediates.zero_motion.Get()};
    inputs.motion_scale_x = 1.f;
    inputs.motion_scale_y = 1.f;
  }
  inputs.reset_hint = (inputs.reset_hint || set_->intermediates.reset_pending);
  // Passes 2..10's own controls, and their own strengths between passes.
  inputs.later_passes = config_.later_controls;
  inputs.resolver = this;
  resolve_slot_ = slot;
  // Keep faces: pass 1's twin, with its surfaces made before the fit like the set's. Off, unsupported, or paused by the Session,
  // they go; a frame whose surfaces cannot be made only goes without the twin.
  // Split chain: the slice's first pass's twin, at its stage, when the device context's config protects this group.
  faces_ran_ = false;
  faces_first_ = slice.first;
  if (config_.keep_faces.enabled && KeepFacesSupported()) {
    inputs.faces = {.wanted = true, .controls = FaceTwinControls(controls, config_.keep_faces.protection),
                    .surface_bytes = FaceSurfaceBytes(set_->intermediates.plan.sizes.canvas)};
    if (session_.FacesPaused(slice.stage)) {
      RetireFaces(set_);
    } else if (EnsureFaces(set_->intermediates.plan.sizes.canvas)) {
      inputs.faces.output = set_->faces.twin.Get();
      set_->faces.last_use = slot_marks_[slot];  // the Session may evaluate the twin into it even when no recombination follows
    }
  } else {
    RetireFaces(set_);
  }
  const uint64_t held = HeldBytes();  // allocated before the fit: charged as the chain's surfaces and credited as held
  const nr::EvaluateResult evaluated = session_.Evaluate(
      list,
      {.a = set_->intermediates.model_a.Get(), .b = set_->intermediates.model_b.Get(), .size = set_->intermediates.plan.sizes.canvas,
       .surface_bytes = held, .held_intermediate_bytes = held},
      inputs, controls, slice);
  if (evaluated.output != nullptr) {
    set_->intermediates.reset_pending = false;  // only a frame that ran NR pays off the owed reset
  }
  if (evaluated.output != nullptr && faces_ran_ && config_.keep_faces.show_mask) {
    // Show the face mask: the mask tinted into NR's result, which the change field and the decode then carry to the image.
    Transition(list, evaluated.output, COMPUTE_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color_.RecordFacesShow(list, slot, evaluated.output, set_->faces.twin.Get(), set_->intermediates.plan.sizes.canvas);
    Transition(list, evaluated.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  }
  return evaluated;
}

void NrPipeline::NoteEncoding(color::Encoding encoding, float input_scale) {
  // NR's history belongs to one encoding and input scale; a change starts it afresh.
  if (encoding != set_->intermediates.encoding || input_scale != set_->intermediates.input_scale) {
    set_->intermediates.encoding = encoding;
    set_->intermediates.input_scale = input_scale;
    set_->intermediates.reset_pending = true;
  }
}

void NrPipeline::CommitSlot(uint32_t slot) {
  // The GPU reads this slot's descriptors, and the intermediates, until this recording completes: the
  // current frame, and the token an After-DLSS caller issued before recording.
  slot_marks_[slot] = timeline_.MarkNow();
  set_->intermediates.last_use = slot_marks_[slot];
  next_slot_ = (slot + 1u) % color::ColorPipeline::RING_SLOTS;
}

bool NrPipeline::Encode(ID3D12GraphicsCommandList* list, uint32_t slot, color::EncodePass pass) {
  ID3D12Resource* const model_a = set_->intermediates.model_a.Get();
  ID3D12Resource* const zero_motion = set_->intermediates.zero_motion.Get();
  const bool clear_motion = set_->intermediates.motion_needs_clear;
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
      set_->intermediates.motion_needs_clear = false;
    }
  }
  return encoded;
}

PipelineResult NrPipeline::RecordPresent(ID3D12GraphicsCommandList* list, const PresentTarget& target,
                                         const nr::Controls& controls, bool reset_hint, const WorkLayout& layout, const ChainPart& part) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  if (list == nullptr || target.resource == nullptr || part.set >= STAGE_SETS) return {.reason = nr::SkipReason::INVALID_INPUT};
  set_ = &sets_[part.set];
  const D3D12_RESOURCE_DESC description = target.resource->GetDesc();
  const std::optional<color::FormatInfo> format = color::DescribeFormat(description.Format);
  if (!format) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  // Never run NR on a frame whose result could not be written back.
  if (!color_.HasDecodePipeline(format->target_view_format)) return {.reason = nr::SkipReason::NO_TARGET_PIPELINE};
  const uint64_t frame = timeline_.CurrentFrame();
  if (frame == last_present_frame_) return {.reason = nr::SkipReason::ALREADY_RAN};
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const nr::Size size = {static_cast<uint32_t>(description.Width), description.Height};
  const WorkLayout work = ResolveLayout(layout, size);
  if (const nr::SkipReason problem = EnsureIntermediates({
          .sizes = {.output = size, .image = work.image, .canvas = work.canvas},
          .copy_format = format->copy_format,
          .source_bytes_per_pixel = format->bytes_per_pixel,
          .kind = TargetKind::PRESENT,
      });
      problem != nr::SkipReason::NONE) {
    return {.reason = problem};
  }
  last_present_frame_ = frame;
  const bool reduced = (work.image != size);
  const bool look_ready = EnsureLook(LookPlanFor(work.image));  // before the fit, like the set
  CommitSlot(slot);
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(target.encoding, input_scale);
  const uint32_t options = ShaderOptions(config_.fixes);
  // The Present path never has the game's own exposure.
  NoteMeterBasis(part.slice.stage, target.encoding, input_scale);
  const ExposureChoice exposure = ChooseExposure(target.encoding, nullptr, 1.f);
  ID3D12Resource* const source_copy = set_->intermediates.source_copy.Get();
  // The copy of the game's motion vectors made for this very frame, when
  // there is one, from the slot the frame it was made for selects (PresentMotionSlotOf).
  nr::FrameInputs inputs = {.ui_correction = config_.present_ui_correction, .reset_hint = reset_hint};
  MotionSource motion_source = MotionSource::NONE;
  std::array<uint64_t, MOTION_COPY_SLOTS> copy_frames = {};
  for (size_t index = 0u; index < MOTION_COPY_SLOTS; ++index) {
    copy_frames[index] = (motion_copies_.textures[index] ? motion_copies_.frames[index] : 0u);
  }
  const PresentMotionPick pick = PickPresentMotion(copy_frames, frame);
  if (pick.slot) {
    // This frame's copy, else (flicker fix) the newest at most PRESENT_MOTION_MAX_AGE frames older: still DLSS's vectors, so no provider switch.
    const size_t copy = *pick.slot;
    inputs.motion = {.resource = motion_copies_.textures[copy].Get()};  // full subrect, scale (1, 1)
    motion_copies_.last_use[copy] = slot_marks_[slot];
    motion_source = MotionSource::PRESENT_COPY;
  } else if (target.dlss_motion.resource != nullptr && target.dlss_motion_raw) {
    // Direct3D 11's ring slot holds DLSS's raw vectors (its rect the evaluate's region, at (0, 0)). They go through the motion
    // copy here, at the region's size with MV.Scale x MotionScale applied, as the Direct3D 12 copy above was made, so every Present source is filtered.
    if (const nr::BoundResource converted =
            ConvertDlssMotion(list, slot, target.dlss_motion, target.dlss_motion_scale_x, target.dlss_motion_scale_y, target.dlss_motion_flip);
        converted.resource != nullptr) {
      inputs.motion = converted;  // full subrect, scale (1, 1)
      motion_source = MotionSource::PRESENT_COPY;
    }
  } else if (target.dlss_motion.resource != nullptr) {
    // The Vulkan bridge's copy of DLSS's vectors, in the motion region's own pixels (the scale was applied by the copy): the full subrect and a
    // scale of (1, 1), as the copy of the Direct3D 12 path above.
    inputs.motion = target.dlss_motion;
    motion_source = MotionSource::PRESENT_COPY;
  } else if (target.launchpad_motion.resource != nullptr) {
    // LaunchPad's motion, written by Uplift.fx this frame, in back-buffer pixels. Never bound as it is. The
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
  if (motion_copies_.textures[0] || present_motion_counters_.evaluates != 0u) {
    PresentMotionCounters& counters = present_motion_counters_;
    ++counters.presents;
    if (!pick.slot) {
      ++counters.missed;
    } else if (pick.age == 0u) {
      ++counters.own;
    } else {
      ++counters.older;
    }
    counters.age_max = std::max(counters.age_max, pick.age);
    counters.resets += (inputs.reset_hint || motion_source != set_->present_motion ? 1u : 0u);
    const auto now = std::chrono::steady_clock::now();
    if (counters.since == std::chrono::steady_clock::time_point{}) {
      counters.since = now;
    } else if (now - counters.since >= std::chrono::seconds(1)) {
      nr::Logf(nr::LogLevel::TRACE, "Direct3D 12: Present motion: {} copy attempts for {} presents; {} copies, skipped {} (input) {} (ring slot) {} (slot's last use); presents bound {} own, {} older, {} none; max copy age {}, max GPU lag {}; {} pipeline reset hints; latest jitter correction {} x {} px",
               counters.evaluates, counters.presents, counters.copies, counters.skipped_input, counters.skipped_ring, counters.skipped_slot,
               counters.own, counters.older, counters.missed, counters.age_max, counters.lag_max, counters.resets, counters.offset_x, counters.offset_y);
      counters = {.since = now};
    }
  }
  inputs.reset_hint = (inputs.reset_hint || motion_source != set_->present_motion);  // a provider switch
  set_->present_motion = motion_source;
  // 1. Source copy ← target.
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
  if (!Encode(list, slot, encode)) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
  // 3. NR.
  const bool look_reset = (inputs.reset_hint || set_->intermediates.reset_pending);  // Evaluate pays the owed reset off
  const nr::EvaluateResult evaluated = Evaluate(list, slot, inputs, controls, part.slice);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = nr::SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source};
  }
  // 4. The change field and optional look stage. The Present
  //    copy is in its own pixels with the full rect: scale (1, 1).
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
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
  if (!decoded) return {.recorded = true, .reason = nr::SkipReason::NO_TARGET_PIPELINE, .motion_source = motion_source};
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = nr::SessionWroteMessage(evaluated.reason), .motion_source = motion_source};
}

PipelineResult NrPipeline::RecordUav(ID3D12GraphicsCommandList* list, const UavTarget& target,
                                     const nr::FrameInputs& inputs, const nr::Controls& controls, const WorkLayout& layout,
                                     const ChainPart& part) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  if (list == nullptr || target.resource == nullptr || part.set >= STAGE_SETS) return {.reason = nr::SkipReason::INVALID_INPUT};
  set_ = &sets_[part.set];
  const D3D12_RESOURCE_DESC description = target.resource->GetDesc();
  const std::optional<color::UavFormatInfo> format = color::DescribeUavFormat(description.Format);
  // For UAV targets: without typed UAV stores the decode could not write NR's result back.
  if (!format || !color_.SupportsTypedUavStore(format->uav_format)) return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  nr::Rect region = target.region;
  if (region.width == 0u && region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const WorkLayout work = ResolveLayout(layout, size);
  if (const nr::SkipReason problem = EnsureIntermediates({
          .sizes = {.output = size, .image = work.image, .canvas = work.canvas},
          .copy_format = format->copy_format,
          .source_bytes_per_pixel = format->bytes_per_pixel,
          .kind = TargetKind::UAV,
      });
      problem != nr::SkipReason::NONE) {
    return {.reason = problem};
  }
  const bool reduced = (work.image != size);
  const bool look_ready = EnsureLook(LookPlanFor(work.image));  // before the fit, like the set
  CommitSlot(slot);
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(target.encoding, input_scale);
  const uint32_t options = ShaderOptions(config_.fixes);
  NoteMeterBasis(part.slice.stage, target.encoding, input_scale);
  const ExposureChoice exposure = ChooseExposure(target.encoding, target.exposure, target.exposure_factor);
  ID3D12Resource* const source_copy = set_->intermediates.source_copy.Get();
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
  if (!Encode(list, slot, encode)) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source};
  // 3. The NR passes, with the game's motion vectors bound in place.
  const bool look_reset = (inputs.reset_hint || set_->intermediates.reset_pending);  // Evaluate pays the owed reset off
  const nr::EvaluateResult evaluated = Evaluate(list, slot, inputs, controls, part.slice);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = nr::SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 4. The change field and optional look stage.
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source,
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
    return {.recorded = true, .reason = nr::SkipReason::DECODE_FAILED, .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = nr::SessionWroteMessage(evaluated.reason), .motion_source = motion_source,
          .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

PipelineResult NrPipeline::RecordPreSr(ID3D12GraphicsCommandList* list, const UavTarget& color, const nr::FrameInputs& inputs,
                                       const nr::Controls& controls, const WorkLayout& layout, const ChainPart& part) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = nr::SkipReason::INACTIVE};
  if (list == nullptr || color.resource == nullptr || part.set >= STAGE_SETS) return {.reason = nr::SkipReason::INVALID_INPUT};
  set_ = &sets_[part.set];
  const D3D12_RESOURCE_DESC description = color.resource->GetDesc();
  const std::optional<color::UavFormatInfo> format = color::DescribeUavFormat(description.Format);
  // A 2D, single-sample texture Uplift reads through a typed, non-sRGB view.
  if (!format || description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1u
      || description.SampleDesc.Count != 1u) {
    return {.reason = nr::SkipReason::UNSUPPORTED_FORMAT};
  }
  const nr::Rect region = color.region;
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    return {.reason = nr::SkipReason::INVALID_INPUT};
  }
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = nr::SkipReason::DESCRIPTORS_BUSY};
  const WorkLayout work = ResolveLayout(layout, size);
  // In a canvas, readable motion vectors are copied into it; unconverted ones cannot be bound there (tools/nrspike's e9).
  const std::optional<DXGI_FORMAT> motion_format =
      ((work.canvas != work.image && inputs.motion.resource != nullptr) ? MotionViewFormat(inputs.motion.resource->GetDesc().Format)
                                                                        : std::nullopt);
  // Keyed on the settled work image, never the per-frame render region --
  // a game with dynamic resolution sets `size` on almost every evaluate, and the old `.output = size`
  // reallocated (and restarted NR's history on) the whole set for each step. EnsureIntermediates always
  // allocates the change texture for PRE_SR now (its own `reduced` computation), so the set is stable
  // here even though `size` still varies frame to frame.
  if (const nr::SkipReason problem = EnsureIntermediates({
          .sizes = {.output = work.image, .image = work.image, .canvas = work.canvas,
                    .private_color = {static_cast<uint32_t>(description.Width), description.Height}},
          .kind = TargetKind::PRE_SR,
          .canvas_motion = motion_format.has_value(),
      });
      problem != nr::SkipReason::NONE) {
    return {.reason = problem};
  }
  const bool reduced = (work.image != size);
  const bool look_ready = EnsureLook(LookPlanFor(work.image));  // before the fit, like the set; C stays main-set
  CommitSlot(slot);
  const float input_scale = color::InputScale(color.encoding, color.diffuse_white_nits, config_.fixes.linear_unit_nits);
  NoteEncoding(color.encoding, input_scale);
  const uint32_t options = ShaderOptions(config_.fixes);
  NoteMeterBasis(part.slice.stage, color.encoding, input_scale);
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
  if (!Encode(list, slot, encode)) return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED};
  // 2. In a canvas the motion vectors move into canvas pixels, mirrored like the
  //    image, bound with the canvas as their subrect and a scale of (1, 1).
  nr::FrameInputs nr_inputs = inputs;
  if (motion_format) {
    nr::Rect motion_region = inputs.motion.rect;
    if (motion_region.width == 0u || motion_region.height == 0u) {
      const D3D12_RESOURCE_DESC motion = inputs.motion.resource->GetDesc();
      motion_region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(motion.Width), .height = motion.Height};
    }
    ID3D12Resource* const canvas_motion = set_->intermediates.canvas_motion.Get();
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
  const bool look_reset = (inputs.reset_hint || set_->intermediates.reset_pending);  // Evaluate pays the owed reset off
  const nr::EvaluateResult evaluated = Evaluate(list, slot, nr_inputs, controls, part.slice);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = nr::SessionWroteMessage(evaluated.reason),
            .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 4. The change field and optional look stage. The stabiliser
  // runs on the game's own vectors (`inputs`, not `nr_inputs`), since it acts on the image, not the canvas.
  const AfterNr after = RecordAfterNr(list, slot, encode, evaluated.output, evaluated.passes_run, reduced, look_ready, work.upsampling,
                                      StabilizeMotionOf(inputs.motion, inputs.motion_scale_x, inputs.motion_scale_y), look_reset);
  if (!after.ok) {
    return {.recorded = true, .reason = nr::SkipReason::ENCODE_FAILED, .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  // 5. The compose into the private colour's region.
  ID3D12Resource* const private_color = set_->intermediates.private_color.Get();
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
    return {.recorded = true, .reason = nr::SkipReason::DECODE_FAILED, .motion_source = motion_source,
            .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason,
          .from_session = nr::SessionWroteMessage(evaluated.reason), .motion_source = motion_source,
          .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

bool NrPipeline::RecordMotionCopy(ID3D12GraphicsCommandList* list, const nr::BoundResource& motion, float scale_x, float scale_y, bool flip_y, float offset_x, float offset_y) {
  PresentMotionCounters& counters = present_motion_counters_;
  ++counters.evaluates;
  counters.offset_x = offset_x;
  counters.offset_y = offset_y;
  const uint64_t current = timeline_.CurrentFrame();
  const uint64_t completed = timeline_.CompletedFrame();
  counters.lag_max = std::max(counters.lag_max, current > completed ? current - completed : 0u);
  if (session_.State() != nr::SessionState::ACTIVE || list == nullptr || motion.resource == nullptr) {
    ++counters.skipped_input;
    return false;
  }
  const D3D12_RESOURCE_DESC description = motion.resource->GetDesc();
  const std::optional<DXGI_FORMAT> view_format = MotionViewFormat(description.Format);
  nr::Rect region = motion.rect;
  if (region.width == 0u || region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(description.Width), .height = description.Height};
  }
  if (!view_format || uint64_t{region.x} + region.width > description.Width
      || uint64_t{region.y} + region.height > description.Height) {
    ++counters.skipped_input;
    return false;
  }
  // The present that closes the current frame binds the copy made for it (RecordPresent).
  const uint64_t frame = timeline_.CurrentFrame() + 1u;
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) {
    ++counters.skipped_ring;
    return false;
  }
  const nr::Size size = {region.width, region.height};
  if (motion_copies_.size != size || !motion_copies_.textures[0]) {
    RetireMotionCopies();
    if (!MakeMotionCopies(0u, PRESENT_MOTION_MIN_SLOTS, size)) {
      motion_copies_ = {};
      ++counters.skipped_input;
      return false;
    }
    motion_copies_.size = size;
  }
  const size_t index = PresentMotionSlotOf(frame, motion_copies_.slots);
  if (!timeline_.IsComplete(motion_copies_.last_use[index])) {
    ++counters.skipped_slot;
    // The GPU runs too far behind for the minimum motion ring: the ring grows once, its new copies made beside the first ones (none of those is freed or
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
                          .offset_x = offset_x,
                          .offset_y = offset_y,
                          .flip_y = flip_y,
                      });
  Transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, COMPUTE_READ);
  ++counters.copies;
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
  // Nothing was written (a slot out of range): NR runs with the zero motion, never with this texture's old or undefined contents.
  if (!recorded) return {};
  return {.resource = target};
}

}  // namespace uplift::sources
