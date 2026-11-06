#include "addon/vk_dlss_context.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "memory_units.hpp"
#include "nr/log.hpp"
#include "text/nr_messages.hpp"
#include "text/timeout_messages.hpp"
#include "nr/vk_handles.hpp"
#include "nr/vk_host.hpp"
#include "sources/after_dlss_source.hpp"
#include "sources/pipeline_rules.hpp"
#include "vk/format.hpp"
#include "vk/loader.hpp"
// Only the CPU unit target defines UPLIFT_VK_PRESENT_CPU_TEST and supplies Create and the two visit counters.
#ifdef UPLIFT_VK_PRESENT_CPU_TEST
#include "vk_present_visits.hpp"
#endif

namespace uplift::addon {

// PresentStopped spells PRESENT_QUEUE_TIMEOUT_LIMIT in words.
static_assert(VkDlssContext::PRESENT_QUEUE_TIMEOUT_LIMIT < uplift::text::COUNT_WORDS.size());

namespace {

// The per-frame trace's name of the motion a present bound (an older copy adds its age there).
template <typename Bound>
std::string_view BoundMotionName(Bound bound) {
  switch (bound) {
    case Bound::NONE:       return "none";
    case Bound::LAUNCHPAD:  return "Launchpad's UPLIFT_MV";
    case Bound::OWN_COPY:   return "DLSS's vectors (this frame's copy)";
    case Bound::OLDER_COPY: return "DLSS's vectors (an older copy)";
    case Bound::NO_COPY:    return "no copy of DLSS's vectors";
  }
  return "none";
}

constexpr uint32_t TRACE_FRAMES = 8u;  // per-frame traces for this many frames after a state change
// Vulkan's NGX shutdown is terminal: OnCoreShutdown stops the context for good, so no hold keeps NR off for a later resume (Direct3D 12's CoreShutdownHold).
constexpr bool CORE_HELD = false;
// Before upscaling's pipelines could not be built: the stage stays off for the session.
constexpr std::string_view VK_PRE_SR_PIPELINES_PROBLEM = "The Vulkan colour pipelines for Before upscaling could not be built";
// PresentBlock's PRESENT_BUFFERS: the ring could not be made again, or the effect queue's family was never seen.
constexpr std::string_view RING_REMADE_FAILED_DETAIL = "Uplift's own command buffers could not be made again on the effect queue's family (ReShade.log has the error)";
constexpr std::string_view NO_QUEUE_FAMILY_DETAIL = "Uplift's own command buffers need the effect queue's family, which Uplift did not see when the device was created";
// The per-device reservation NGX keeps on a Vulkan device after native NR's first use.
constexpr uint64_t VK_FIRST_USE_BYTES = 640ull << 20u;
// How long Teardown waits for Uplift's own command buffers (the game idled the device, so they should be done already).
constexpr auto RING_TEARDOWN_WAIT = std::chrono::milliseconds(2000);
// How long a switch from native Present to a DLSS stage waits for Uplift's own command buffers before NR's features may go.
constexpr auto RING_SWITCH_WAIT = std::chrono::milliseconds(2000);

// What NGX is handed as motion for `inputs` at `canvas` (`in_canvas`: Before upscaling pads into a larger canvas, so the game's
// vectors are copied into an RG16F canvas image).
VkMotionShape MotionShapeOf(const nr::FrameInputs& inputs, nr::Size canvas, bool in_canvas) {
  if (inputs.motion.resource == nullptr) return {.kind = VkMotionKind::NONE, .format = VK_FORMAT_R16G16_SFLOAT};
  if (in_canvas) return {.kind = VkMotionKind::GAME_VECTORS, .format = VK_FORMAT_R16G16_SFLOAT};
  const NVSDK_NGX_ImageViewInfo_VK& view = nr::VkResourceOf(inputs.motion.resource)->Resource.ImageViewInfo;
  const uint32_t width = (inputs.motion.rect.width != 0u ? inputs.motion.rect.width : view.Width);
  const uint32_t height = (inputs.motion.rect.height != 0u ? inputs.motion.rect.height : view.Height);
  return {.kind = VkMotionKind::GAME_VECTORS, .format = static_cast<uint32_t>(view.Format), .low_res = (width < canvas.width || height < canvas.height)};
}

std::string_view MotionKindName(VkMotionKind kind) {
  switch (kind) {
    case VkMotionKind::NONE:         return "none";
    case VkMotionKind::GAME_VECTORS: return "the game's vectors";
    case VkMotionKind::PRESENT_COPY: return "DLSS's vectors copied for Present";
    case VkMotionKind::LAUNCHPAD:    return "Launchpad's UPLIFT_MV";
  }
  return "none";
}

std::string DescribeMotion(const VkMotionShape& shape) {
  if (shape.kind == VkMotionKind::NONE) return std::string(MotionKindName(shape.kind));
  return std::format("{} (VkFormat {}, {})", MotionKindName(shape.kind), shape.format, (shape.low_res ? "low-res" : "full-res"));
}

std::string_view StreamName(VkNrStream stream) {
  switch (stream) {
    case VkNrStream::OWN_BUFFERS:      return "Present (Uplift's own command buffers)";
    case VkNrStream::AFTER_DLSS:       return "After DLSS (the game's command buffers)";
    case VkNrStream::BEFORE_UPSCALING: return "Before upscaling (the game's command buffers)";
    case VkNrStream::NONE:             break;
  }
  return "no placement";
}

// Split chain: a set of streams in chain order, "Before upscaling (the game's command buffers) + Present (Uplift's own command buffers)"; a set of
// one reads as StreamName.
std::string StreamsName(VkStreamSet streams) {
  std::string name;
  for (const VkNrStream stream : {VkNrStream::BEFORE_UPSCALING, VkNrStream::AFTER_DLSS, VkNrStream::OWN_BUFFERS}) {
    if ((streams & VkStreamBit(stream)) != 0u) {
      name += std::format("{}{}", (name.empty() ? "" : " + "), StreamName(stream));
    }
  }
  return (name.empty() ? std::string(StreamName(VkNrStream::NONE)) : name);
}

// Split chain: why a later group cannot run at Present while the game's DLSS-stage NR goes to another queue than the
// effect queue's.
constexpr std::string_view OTHER_QUEUE_REASON = "the game's DLSS runs on another queue";
// Native NR at Present fell back off Native (to the private Direct3D 12 device or the helper), which hold one stage only.
constexpr std::string_view NOT_NATIVE_PRESENT_REASON = "NR at Present does not run natively on this device now";

// The format the swap-chain image's bits are copied raw into: the bridge's shared format, with an sRGB one (a swap chain ReShade did not make
// mutable reports its own sRGB format) taken to its UNORM sibling, so the RGBA16F blit never linearises the stored values.
DXGI_FORMAT PresentCopyFormat(DXGI_FORMAT reported) {
  const DXGI_FORMAT shared = vk::SharedFormatOf(reported);
  switch (shared) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:                              return shared;
  }
}

// The layout ReShade keeps the swap-chain image in at a trigger (vk::Usage is ReShade's api::resource_usage; its Vulkan backend maps them so).
VkImageLayout LayoutOf(vk::Usage usage) {
  switch (usage) {
    case vk::Usage::PRESENT:       return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case vk::Usage::RENDER_TARGET: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    default:                       return VK_IMAGE_LAYOUT_GENERAL;
  }
}

}  // namespace

#ifndef UPLIFT_VK_PRESENT_CPU_TEST
std::unique_ptr<VkDlssContext> VkDlssContext::Create(VkContextSetup setup, std::string* error) {
  const vk::Loader* const loader = vk::Loader::Get();
  if (loader == nullptr || setup.binding.device == VK_NULL_HANDLE || setup.binding.physical == VK_NULL_HANDLE || !setup.signal) {
    *error = "the Vulkan loader, the game's device or the present signal is missing";
    return nullptr;
  }
  std::optional<vk::NrFunctions> functions = vk::NrFunctions::Open(setup.binding.device);
  if (!functions) {
    *error = "the game's device lacks a function native NR needs (VK_KHR_push_descriptor, events, timeline semaphores)";
    return nullptr;
  }
  // NR's own images are RGBA16F and RG16F storage images, and DLSS's output is written in place as one of the variants'
  // formats. A format that is not a storage format on this physical device is refused before any shader module exists (its variant reads
  // "unsupported format"); RGBA16F or RG16F missing refuses native NR as a whole.
  const auto storage = [&loader, &setup](VkFormat format) {
    if (loader->get_physical_device_format_properties == nullptr) return true;  // nothing to ask: NVIDIA's drivers support all of them
    VkFormatProperties properties = {};
    loader->get_physical_device_format_properties(setup.binding.physical, format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0u;
  };
  if (!storage(VK_FORMAT_R16G16B16A16_SFLOAT) || !storage(VK_FORMAT_R16G16_SFLOAT)) {
    *error = "this GPU's Vulkan driver cannot write RGBA16F and RG16F storage images, which native NR's shaders need";
    return nullptr;
  }
  std::array<bool, color::VkColorPipeline::OUTPUT_VARIANTS> storage_formats = {};
  for (const VkFormat format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R32G32B32A32_SFLOAT}) {
    storage_formats[*color::VkColorPipeline::OutputVariantOf(format)] = storage(format);
  }
  VkPhysicalDeviceMemoryProperties memory = {};
  loader->get_memory_properties(setup.binding.physical, &memory);

  std::unique_ptr<VkDlssContext> context(new VkDlssContext(*functions, setup.binding.device, storage_formats));
  context->physical_ = setup.binding.physical;
  context->completion_ = vk::NrCompletion::Create(context->functions_, context->device_, std::move(setup.signal), error);
  if (!context->completion_) return nullptr;
  // False for a DLSS game, whose own core is never initialised a second time. Decided again at each present
  // (FrameFacts::vk_initialize_core), before the Session loads.
  setup.snippet.initialize_core_for_device = false;
  if (setup.host != nullptr) {
    context->host_ = std::move(setup.host);
  } else {
    auto vk_host = std::make_unique<nr::VkHost>(std::move(setup.snippet), setup.binding, std::move(setup.adapter), context->functions_);
    context->vk_host_ = vk_host.get();
    context->host_ = std::move(vk_host);
  }
  context->session_ = std::make_unique<nr::Session>(*context->host_, context->completion_->Timeline(),
                                                    nr::SessionConfig{.budget = {.first_use_bytes = VK_FIRST_USE_BYTES}});
  context->pipeline_ = std::make_unique<sources::VkNrPipeline>(context->functions_, context->device_, memory, *context->session_,
                                                               context->completion_->Timeline());
  if (!context->pipeline_->Initialize(error)) {
    context->Teardown();  // nothing ran on the GPU yet: the objects that exist go now
    return nullptr;
  }
  // The look stage's pyramid peaks (R16F) and scene-cut state (RGBA32F) are storage images too; without them the look is skipped, as on a
  // Direct3D 12 device without typed UAV loads.
  context->pipeline_->SetLookStorage(storage(VK_FORMAT_R16_SFLOAT) && storage(VK_FORMAT_R32G32B32A32_SFLOAT));
  // Native NR at Present records into command buffers of Uplift's own, from the effect queue's family; never ReShade's immediate list.
  if (setup.queue_family != UINT32_MAX) {
    context->before_present_load_ = std::move(setup.before_present_load);
    context->queue_family_ = setup.queue_family;
    context->ring_ = vk::CommandRing::Create(context->functions_, context->device_, setup.queue_family, error);
    if (!context->ring_) {
      context->Teardown();  // nothing ran on the GPU yet
      return nullptr;
    }
  }
  return context;
}
#endif

VkDlssContext::VkDlssContext(vk::NrFunctions functions, VkDevice device, std::array<bool, color::VkColorPipeline::OUTPUT_VARIANTS> storage_formats)
    : functions_(functions), device_(device), storage_formats_(storage_formats) {}

VkDlssContext::~VkDlssContext() {
  if (!torn_down_) {
    Abandon();
  }
}

void VkDlssContext::SetBlockedReason(std::string reason) {
  if (reason == blocked_reason_) return;
  if (!reason.empty()) {
    nr::Logf(nr::LogLevel::WARN, "NR blocked: {}", reason);
  }
  blocked_reason_ = std::move(reason);
}

void VkDlssContext::SetDlssUnavailableReason(std::string reason) {
  if (reason == dlss_unavailable_reason_) return;
  if (!reason.empty()) {
    nr::Logf(nr::LogLevel::INFO, "DLSS placements unavailable: {}", reason);
  }
  dlss_unavailable_reason_ = std::move(reason);
}

bool VkDlssContext::AtDlssPlacement() const {
  return (!dropped_ || core_shut_down_) && !torn_down_ && (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
}

bool VkDlssContext::WouldRunDlssStage(ui::PlacementSource source) const {
  if (torn_down_ || dropped_ || core_shut_down_ || source != ui::PlacementSource::DLSS) return false;
  // BeginFrame's own choice (its `choose`), with the off latch as it stands.
  const PlacementChoice choice = ChoosePlacement(ui::PlacementSource::AUTO, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(), {},
                                                 DlssStagesOff(dlss_off_.Off(), CORE_HELD));
  return choice.placement == Placement::AFTER_DLSS || choice.placement == Placement::BEFORE_UPSCALING;
}

bool VkDlssContext::WantsNr() const {
  // After the game's NGX shutdown only a DLSS placement keeps the claim (shutdown protection); at Present the route goes on along the chain.
  // A dropped NR keeps it for good, as its runtime may still be mapped.
  if (dropped_ && !torn_down_) return true;
  return AtDlssPlacement() || (!dropped_ && !core_shut_down_ && !torn_down_ && NativePresent());
}

TriggerPoint VkDlssContext::BeginFrame(void* present_queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool present_holds_nr,
                                       std::chrono::steady_clock::time_point now, const VkPresentFrame& present) {
  if (!BeginPresent(true, present_queue, facts, now)) return TriggerPoint::NONE;
  return PrepareFrame(present_queue, settings, facts, nr_allowed, present_holds_nr, now, present);
}

TriggerPoint VkDlssContext::BeginOuterFrame(bool& outer_ticked, void* queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool holds_nr,
                                           std::chrono::steady_clock::time_point now, const VkPresentFrame& present) {
  outer_ticked = true;
  return BeginFrame(queue, settings, facts, nr_allowed, holds_nr, now, present);
}

TriggerPoint VkDlssContext::BeginInnerFrame(bool outer_owns_frame, bool outer_ticked, void* queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool holds_nr,
                                           std::chrono::steady_clock::time_point now, const VkPresentFrame& present) {
  if (outer_owns_frame && outer_ticked) return TriggerPoint::NONE;
  if (!BeginPresent(!outer_ticked, queue, facts, now)) return TriggerPoint::NONE;
  if (outer_owns_frame) {
    // An armed handoff with no outer callback still retires eligible releases. Its session preparation belongs to the outer.
    completion_->Timeline().Poll();
    return TriggerPoint::NONE;
  }
  return PrepareFrame(queue, settings, facts, nr_allowed, holds_nr, now, present);
}

bool VkDlssContext::BeginPresent(bool advance, void* queue, const FrameFacts& facts, std::chrono::steady_clock::time_point now) {
  frame_ready_ = false;
  // Check even on a preparation-only visit: device loss can occur between callbacks.
  if (!torn_down_ && !dropped_ && completion_->PollDeviceLost()) {
    NoteDeviceLost(now);
  }
  // A clean NGX shutdown only stops native Present if the game continues displaying frames.
  if (advance && pending_stop_since_ && !present_stopped_ && !torn_down_ && !dropped_
      && CoreShutdownStopDue(++pending_stop_presents_, now - *pending_stop_since_)) {
    present_stopped_ = "the game shut NVIDIA's NGX down on its device";
  }
  if (torn_down_ || dropped_ || core_shut_down_) {
    status_.Clear();
    return false;
  }
  if (!advance) return true;
  nr::Timeline& timeline = completion_->Timeline();
  timeline.BeginFrame(nr::AsQueue(queue));
  timeline.StampAged(now);
  completion_->DropStale(now, timeline.CurrentFrame());
#ifdef UPLIFT_VK_PRESENT_CPU_TEST
  ++test::VkTestVisits(device_).stale_sweeps;
#endif
  pipeline_->FreeFinished();
  dlss_off_.OnPresent(SwitchOffEvidence(dlss_seen_, facts.dlss_released, dlss_unavailable_reason_, CORE_HELD), now);
#ifdef UPLIFT_VK_PRESENT_CPU_TEST
  ++test::VkTestVisits(device_).dlss_off_visits;
#endif
  return true;
}

TriggerPoint VkDlssContext::PrepareFrame(void* queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool holds_nr,
                                        std::chrono::steady_clock::time_point now, const VkPresentFrame& present) {
  settings_ = settings;
  facts_ = facts;
  if (!settings.session.enabled) {
    present_jitter_.Reset();
  }
  present_frame_ = present;
  if (vk_host_ != nullptr) {
    vk_host_->SetInitializeCoreForDevice(facts.vk_initialize_core);
  }
  facts_.nr_allowed = nr_allowed;
  present_holds_nr_ = holds_nr;
  controls_ = settings.controls;
  effect_queue_ = queue;
  const auto& timeline = completion_->Timeline();
  stage_work_.Prune(effect_queue_, [&timeline](uint64_t token) { return timeline.TokenComplete(token); });
  // Auto stays at Present on Vulkan. Source = DLSS is the explicit choice, which takes the DLSS placement once the game's DLSS
  // is seen (Present, the bridge's, until then) and never while a reason makes the DLSS placements unavailable.
  const auto choose = [this, &settings](bool dlss_off) {
    return (settings.source == ui::PlacementSource::DLSS
                ? ChoosePlacement(ui::PlacementSource::AUTO, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(), {}, dlss_off)
                : PlacementChoice{.placement = Placement::PRESENT});
  };
  const bool dlss_off = DlssStagesOff(dlss_off_.Off(), CORE_HELD);
  placement_ = choose(dlss_off);
  if (dlss_off && !dlss_off_logged_ && placement_.placement != choose(false).placement) {
    dlss_off_logged_ = true;  // logged once a DLSS placement really fell back (with Source = Auto or Present nothing did)
    nr::Log(nr::LogLevel::INFO, DLSS_OFF_LINE);
  }
  NotePlacementChange();
  if (placement_.placement != Placement::AFTER_DLSS && placement_.placement != Placement::BEFORE_UPSCALING) {
    status_.Clear();  // the DLSS placements keep their last evaluate's result
  }
  // The frame native NR at Present runs on, resolved before the Session hears its size. Split chain: and before the chain, which
  // reads whether a later group can run at Present (only while some pass is stored at another stage, so the default path reads nothing more).
  if (facts_.vk_native_present && (placement_.placement == Placement::PRESENT || SplitStored(settings_, facts_.split_capable))) {
    ResolvePresentFrame();
  } else {
    present_encoding_.reset();
    SetPresentProblem({});
  }
  UpdateChain();
  // Uplift's own command buffers are made again when Present comes back after a DLSS stage released them (split chain: a later
  // Present group's too).
  if (PresentGroupNative() && ring_ == nullptr && queue_family_ != UINT32_MAX && !ring_failed_) {
    std::string error;
    ring_ = vk::CommandRing::Create(functions_, device_, queue_family_, &error);
    if (!ring_) {
      ring_failed_ = true;
      nr::Logf(nr::LogLevel::ERR, "Vulkan: {}; NR at Present cannot run natively here", error);
    }
  }
  if (PresentGroupNative() && present_encoding_.has_value()) {
    present_layout_ = SettleLayout(present_frame_.back_buffer.size, Stage::PRESENT, now);  // the Session hears the canvas
  }

  ApplySessionOptions(*session_, settings, chain_, settings_generation_);
  // The look stage, the colour fixes and passes 2..10 reach the pipeline at each recording (ApplyLookConfig, with its frame time); the mask copy goes
  // as soon as Mask is off.
  if (!settings.look.mask) {
    pipeline_->ReleaseMaskCopy();
  }
  // The Present path's copies of DLSS's motion vectors exist while the add-on asks for them. Their pipeline is built here, in the
  // present event, so its first compile stays out of the game's evaluate; they are released (at their marks) as soon as the add-on stops asking.
  if (facts.present_motion_copy) {
    pipeline_->PreparePresentMotion();
    NotePresentMotionSecond(now);
  } else {
    pipeline_->ReleasePresentMotion();
    present_jitter_.Reset();
    present_motion_logged_ = false;
    present_motion_counters_ = {};  // the next window starts when the copies are asked for again
  }
  // The Session hears the frame size every frame, in every state (its FAILED retry and the resume size depend on it). Split chain: each later
  // group's stage hears its own canvas.
  if (const nr::Size frame_size = SessionFrameSize(); !frame_size.Empty()) {
    session_->NoteFrameSize(frame_size);
  }
  for (uint32_t group = 1u; group < chain_.group_count; ++group) {
    const Stage stage = chain_.groups[group].stage;
    if (const nr::Size canvas = stages_[static_cast<size_t>(stage)].canvas; !canvas.Empty()) {
      session_->NoteFrameSize(static_cast<uint8_t>(stage), canvas);
    }
  }
  UpdateSession(now);
  if (dropped_ || !PresentGroupNative()) return TriggerPoint::NONE;  // a switch whose old GPU work did not finish drops NR
  // As DeviceContext's Present path: a recording is readied only while NR runs and the frame can take it.
  frame_ready_ = (session_->State() == nr::SessionState::ACTIVE && ring_ != nullptr && present_problem_.empty() && present_encoding_.has_value()
                  && present.back_buffer.image != VK_NULL_HANDLE);
  return trigger_.OnPresent(present.marker_expected, facts.effects_on, facts.marker_rendered_early);
}

void VkDlssContext::ResolvePresentFrame() {
  const vk::ImageInfo& image = present_frame_.back_buffer;
  present_encoding_.reset();
  if (image.image == VK_NULL_HANDLE) return;  // no frame this present: nothing to say (the last problem stands, so a later group never flips for it)
  // The bridge's rules for what reaches NR (copy access, one sample, a format Uplift processes), without its sharing extension.
  std::string problem = vk::BackBufferProblem(image, true);
  const DXGI_FORMAT copy_format = PresentCopyFormat(image.format);
  copy_format_ = vk::VkFormatOf(copy_format);
  if (problem.empty() && !Blittable(copy_format_)) {
    problem = std::format("This GPU's Vulkan driver cannot convert the back buffer's format (VkFormat {}) for NR at Present", static_cast<int>(copy_format_));
  }
  if (problem.empty()) {
    // The copy keeps the image's stored values (an sRGB-created image's included), so the encoding is the one the bridge resolves for its shared copy.
    present_encoding_ = color::ResolveEncoding(settings_.encoding, present_frame_.color_space, copy_format);
    if (!present_encoding_) {
      problem = "HDR10 HLG output is not supported";
    }
  }
  SetPresentProblem(std::move(problem));
  if (!present_encoding_) return;
  present_diffuse_white_nits_ = (settings_.diffuse_white_nits > 0.f ? settings_.diffuse_white_nits : color::DefaultDiffuseWhiteNits(*present_encoding_));
}

void VkDlssContext::SetPresentProblem(std::string problem) {
  if (problem == present_problem_) return;
  if (!problem.empty()) {
    nr::Log(nr::LogLevel::WARN, problem);
  }
  present_problem_ = std::move(problem);
}

bool VkDlssContext::Blittable(VkFormat copy_format) {
  if (copy_format == blit_checked_format_) return blit_supported_;
  blit_checked_format_ = copy_format;
  const vk::Loader* const loader = vk::Loader::Get();
  if (loader == nullptr || loader->get_physical_device_format_properties == nullptr || physical_ == VK_NULL_HANDLE) {
    blit_supported_ = true;  // nothing to ask: NVIDIA's drivers blit every display format
    return blit_supported_;
  }
  const auto blits = [&](VkFormat format) {
    VkFormatProperties properties = {};
    loader->get_physical_device_format_properties(physical_, format, &properties);
    constexpr VkFormatFeatureFlags BOTH = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (properties.optimalTilingFeatures & BOTH) == BOTH;
  };
  blit_supported_ = (copy_format != VK_FORMAT_UNDEFINED && blits(copy_format) && blits(VK_FORMAT_R16G16B16A16_SFLOAT));
  return blit_supported_;
}

nr::SkipReason VkDlssContext::CheckMotion(Stage stage, const VkMotionShape& shape) {
  StageState& state = stages_[static_cast<size_t>(stage)];
  switch (VkMotionCheck(state.motion, shape, state.motion_mismatches)) {
    case VkMotionVerdict::RUN:
      state.motion = shape;
      state.motion_mismatches = 0u;
      return nr::SkipReason::NONE;
    case VkMotionVerdict::SKIP:
      ++state.motion_mismatches;
      return nr::SkipReason::MOTION_CHANGED;
    case VkMotionVerdict::RELOAD:
      ++state.motion_mismatches;
      if (input_reload_reason_.empty()) {
        input_reload_reason_ = std::format("its motion vectors changed from {} to {}", DescribeMotion(*state.motion), DescribeMotion(shape));
      }
      return nr::SkipReason::MOTION_RELOADS;
  }
  return nr::SkipReason::NONE;
}

std::optional<VkNativeBlock> VkDlssContext::PresentBlock() const {
  if (present_create_failure_) return VkNativeBlock{VkNativeBlock::Cause::CREATE_FAILURE, *present_create_failure_};
  if (!CanRunPresent()) return VkNativeBlock{VkNativeBlock::Cause::PRESENT_BUFFERS, std::string(ring_failed_ ? RING_REMADE_FAILED_DETAIL : NO_QUEUE_FAMILY_DETAIL)};
  return std::nullopt;
}

sources::PipelineResult VkDlssContext::SkipPresent(nr::SkipReason reason, uint32_t group) {
  StageState& stage = stages_[static_cast<size_t>(Stage::PRESENT)];
  stage.nr_applied = false;
  stage.passes_run = 0u;
  stage.skip_reason = reason;
  stage.motion_source = sources::MotionSource::NONE;
  if (group > 0u) {
    present_reset_owed_ = true;  // split chain: the Session never saw this frame: the group's next recording restarts its history
    return {.reason = reason};
  }
  status_.NoteSkip(reason, RecordingStatus::Reset::ALL_BUT_MOTION);
  return {.reason = reason};
}

sources::PipelineResult VkDlssContext::RunPresent(TriggerPoint point, vk::Usage usage, const VkPresentQueues& queues, const VkLaunchpadMotion& launchpad,
                                                  std::chrono::steady_clock::time_point now) {
  if (point == TriggerPoint::NONE || !frame_ready_) return {};
  frame_ready_ = false;
  if (torn_down_ || dropped_ || core_shut_down_ || ring_ == nullptr || queues.queue == VK_NULL_HANDLE || !present_encoding_) return {};
  // Split chain: the Present group is pass 1's (group 0, the single chain of before) or a later one, whose result only Status's groups report.
  const uint32_t group = GroupAt(chain_, Stage::PRESENT).value_or(0u);
  // The evidence of frame generation: the live NGX FrameGeneration features, as this context has no detector.
  const bool frame_generation_on = (facts_.ngx_frame_generation > 0u);
  if (FrameGenerationSkipsPresent(frame_generation_on, facts_.fg_present_path)) return SkipPresent(nr::SkipReason::FRAME_GENERATION_ON, group);
  // Split chain: a later group the budget cut to no pass (or the Defaults view hides) skips before anything is recorded. No GPU waits:
  // DLSS-stage NR on another queue keeps Present off for later groups (UpdateChain); until that shows, a later Present group runs only once every earlier
  // DLSS-stage recording off the effect queue has completed, else it skips this frame. On the effect queue itself the queue's order keeps them apart.
  if (group > 0u && !session_->SliceRuns(slices_[group])) return SkipPresent(SliceSkipReason(settings_, slices_[group]), group);
  if (group > 0u && queues.waits.empty() && !StageWorkDone()) return SkipPresent(nr::SkipReason::WAITING_EARLIER_STAGES, group);
  const auto log_failure = [this](std::string_view what) {
    if (!std::exchange(present_failure_logged_, true)) {
      nr::Logf(nr::LogLevel::WARN, "Vulkan: {}; NR skips such frames at Present", what);
    }
  };
  if (queues.flush_present_queue) {
    // Uplift's submission is ordered after the game's frame only on the effect queue, so the present queue is flushed
    // and waited for on the CPU first, before ReShade's effects so far are flushed (they must not reach the queue ahead of the game's frame either).
    second_queue_ = true;
    switch (ring_->WaitForQueue(queues.flush_present_queue, PRESENT_QUEUE_WAIT)) {
      case vk::CommandRing::Status::READY:
        present_queue_timeouts_ = 0u;
        break;
      case vk::CommandRing::Status::BUSY: return SkipPresent(nr::SkipReason::PRESENT_QUEUE_BUSY, group);  // the last timeout's fence is still pending
      case vk::CommandRing::Status::TIMEOUT:
        log_failure(uplift::text::PresentQueueTimeoutText(PRESENT_QUEUE_WAIT));
        // PRESENT_QUEUE_TIMEOUT_LIMIT reached: no more waits here; the add-on moves NR at Present along the chain (PresentStopped).
        if (++present_queue_timeouts_ >= PRESENT_QUEUE_TIMEOUT_LIMIT && !present_stopped_) {
          present_stopped_ = uplift::text::PresentQueueStoppedText(PRESENT_QUEUE_WAIT, PRESENT_QUEUE_TIMEOUT_LIMIT);
        }
        return SkipPresent(nr::SkipReason::PRESENT_QUEUE_BUSY, group);
      case vk::CommandRing::Status::LOST:
        NoteDeviceLost(now);
        return {};
      case vk::CommandRing::Status::FAILED:
        log_failure("a submission on the game's present queue failed");
        return SkipPresent(nr::SkipReason::PRESENT_QUEUE_FAILED, group);
    }
  }
  VkCommandBuffer buffer = VK_NULL_HANDLE;
  switch (ring_->Begin(&buffer)) {
    case vk::CommandRing::Status::READY: break;
    case vk::CommandRing::Status::BUSY:
    case vk::CommandRing::Status::TIMEOUT:
      if (busy_skips_++ == 0u) {
        nr::Log(nr::LogLevel::INFO, "Vulkan: a frame went without NR at Present: Uplift's command buffers were all still on the GPU (counted in Details)");
      }
      return SkipPresent(nr::SkipReason::COMMAND_BUFFERS_BUSY, group);
    case vk::CommandRing::Status::LOST:
      NoteDeviceLost(now);
      return {};
    case vk::CommandRing::Status::FAILED:
      log_failure("Uplift's command buffer could not be begun");
      return SkipPresent(nr::SkipReason::COMMAND_BUFFER_BEGIN_FAILED, group);
  }
  ApplyLookConfig(now, group);
  // As DeviceContext::Run: DLSS's copy for this present while MotionVectors wants DLSS's (it stamps the copy's use), else Launchpad's UPLIFT_MV.
  const vk::NrImage dlss_motion = ((settings_.motion_vectors && facts_.present_motion_copy) ? PresentMotion() : vk::NrImage{});
  const bool launchpad_wanted = (settings_.launchpad_motion && launchpad.view != VK_NULL_HANDLE);
  if (!settings_.motion_vectors || !facts_.present_motion_copy) {
    present_motion_bound_ = (launchpad_wanted ? BoundMotion::LAUNCHPAD : BoundMotion::NONE);  // flicker fix: the per-frame trace names the motion
  }
  // The motion input this present is meant to have, from the settings, not from what arrived. DLSS's copies while
  // MotionVectors wants them (a present whose copy is missing gets zeros of the copies' own shape: VkPresentTarget::dlss_motion_gap_size), else Launchpad's
  // UPLIFT_MV at the Uplift technique (a missed conversion gets zeros of its shape), else none. So a source that skips a frame never changes the input's kind:
  // no skip and no reload; only a real change of setting or shape does.
  const nr::Size present_canvas = sources::ResolveLayout(present_layout_, present_frame_.back_buffer.size).canvas;
  const bool copies_meant = (settings_.motion_vectors && facts_.present_motion_copy && !dlss_motion_size_.Empty());
  nr::Size copy_size = dlss_motion_size_;
  if (dlss_motion.image != VK_NULL_HANDLE) {
    copy_size = {dlss_motion.ngx.Resource.ImageViewInfo.Width, dlss_motion.ngx.Resource.ImageViewInfo.Height};
  }
  VkMotionShape motion_shape = {.kind = (launchpad_wanted ? VkMotionKind::LAUNCHPAD : VkMotionKind::NONE), .format = VK_FORMAT_R16G16_SFLOAT};
  if (copies_meant || dlss_motion.image != VK_NULL_HANDLE) {
    motion_shape = {.kind = VkMotionKind::PRESENT_COPY, .format = VK_FORMAT_R16G16_SFLOAT,
                    .low_res = (copy_size.width < present_canvas.width || copy_size.height < present_canvas.height)};
  }
  if (const nr::SkipReason motion_skip = CheckMotion(Stage::PRESENT, motion_shape); motion_skip != nr::SkipReason::NONE) return SkipPresent(motion_skip, group);
  const sources::VkPresentTarget target = {
      .image = present_frame_.back_buffer.image,
      .layout = LayoutOf(usage),
      .size = present_frame_.back_buffer.size,
      .copy_format = copy_format_,
      .encoding = *present_encoding_,
      .diffuse_white_nits = present_diffuse_white_nits_,
      .transfer_strength = settings_.transfer_strength,
      .color_strength = settings_.color_strength,
      .dlss_motion = (dlss_motion.image != VK_NULL_HANDLE ? &dlss_motion.ngx : nullptr),
      .dlss_motion_gap_size = ((copies_meant && dlss_motion.image == VK_NULL_HANDLE) ? copy_size : nr::Size{}),
      .launchpad_motion = (launchpad_wanted ? color::VkSampledView{.view = launchpad.view, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                                            : color::VkSampledView{}),
      .launchpad_size = (point == TriggerPoint::EARLY_MARKER ? facts_.early_motion_extent : launchpad.size),
      .launchpad_format = launchpad.format,
      .motion_scale_x = settings_.motion_scale_x,
      .motion_scale_y = settings_.motion_scale_y,
  };
  const sources::PipelineResult result =
      pipeline_->RecordPresent(buffer, target, controls_, present_reset_owed_, present_layout_, {.slice = slices_[group], .set = group});
  if (result.reason == nr::SkipReason::CREATE_FAILED && !present_create_failure_) {
    // A creation that failed at Present, not for want of memory (the Session's retry and budget handle that), ends native Present for the session.
    const nr::SessionStatus status = session_->Status();
    if (status.create_failed && status.create_result != static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_OutOfGPUMemory)) {
      present_create_failure_ = status.message;
    }
  }
  if (result.recorded) {
    // Hard rule 2: the game's frame, ReShade's work so far, then Uplift's NR (the rest of ReShade's work follows on the same queue).
    queues.flush_effects();
    if (const VkResult submitted = ring_->Submit(queues.queue, queues.waits); submitted != VK_SUCCESS) {
      if (submitted == VK_ERROR_DEVICE_LOST) {
        NoteDeviceLost(now);
        return {};
      }
      log_failure(std::format("Uplift's command buffer could not be submitted (VkResult {})", static_cast<int>(submitted)));
      return SkipPresent(nr::SkipReason::COMMAND_BUFFER_SUBMIT_FAILED, group);
    }
    last_present_submission_ = now;  // A device loss soon after this latches native NR off
  }
  // Native Present's vectors reach NR already scaled (DLSS's copies, Launchpad's conversion): its status reports the unit scale.
  NoteResult(Stage::PRESENT, group, result, RecordingStatus::Scale::UNIT);
  present_reset_owed_ = (present_reset_owed_ && !result.nr_applied);
  if (result.nr_applied) {
    trigger_used_ = point;
    if (point != logged_trigger_) {
      logged_trigger_ = point;
      nr::Logf(nr::LogLevel::INFO, "NR trigger: {} (Vulkan, native)", TriggerPointName(point));
    }
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "frame {}: NR {} at {} (Vulkan, native), {} pass(es) {}; motion: {}", completion_->Timeline().CurrentFrame(),
             (result.nr_applied ? "applied" : "skipped"), TriggerPointName(point), result.passes_run, nr::SkipReasonText(result.reason),
             (present_motion_bound_ == BoundMotion::OLDER_COPY ? std::format("DLSS's vectors (the copy {} frame(s) older)", present_motion_age_)
                                                                : std::string(BoundMotionName(present_motion_bound_))));
  }
  return result;
}

nr::Size VkDlssContext::SessionFrameSize() const {
  if (NativePresent()) return canvas_size_;  // the settled canvas of the swap-chain image (SettleLayout, as Direct3D 12's Present path)
  const bool dlss_placement = (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  return ((dlss_placement && !canvas_size_.Empty()) ? canvas_size_ : output_size_);
}

sources::WorkLayout VkDlssContext::SettleLayout(nr::Size output, Stage stage, std::chrono::steady_clock::time_point now) {
  // As DeviceContext::SettleLayout: StageLayoutTarget's target, settled, and Before upscaling's floor canvas. Pass 1's group works at Setup's Resolution,
  // a later group at its stage's own.
  const bool pass1 = Pass1Group(chain_, stage);
  const LayoutTarget target = StageLayoutTarget(settings_, output, stage, pass1, main_snapshot_, dlss_off_.Off());
  StageState& state = stages_[static_cast<size_t>(stage)];
  const nr::Size image = state.settle.Update(sources::WorkSize(target.width, target.height, output), now, target.slider_held);
  const nr::Size canvas = StageCanvas(image, stage);
  state.work = image;
  state.canvas = canvas;
  if (pass1) {
    work_size_ = image;
    canvas_size_ = canvas;
  }
  session_->NoteFrameSize(static_cast<uint8_t>(stage), canvas);
  return {.image = image, .canvas = canvas, .upsampling = settings_.upsampling};
}

void VkDlssContext::UpdateSession(std::chrono::steady_clock::time_point now) {
  const bool dlss_placement = (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  dlss_idle_ = (dlss_placement && last_main_evaluate_.has_value()
                && now - *last_main_evaluate_ > std::max(2 * settings_.session.grace, DLSS_IDLE_MINIMUM));
  dlss_paused_ = (last_main_evaluate_.has_value() && now - *last_main_evaluate_ > DLSS_IDLE_MINIMUM);
  // Split chain: a later Present group runs on while the game's DLSS idles (a menu), so the DLSS idle drain waits for none then; the DLSS
  // groups skip those frames and restart their history when DLSS returns.
  const bool later_present = (dlss_placement && PresentGroupNative());
  if (chain_.Split() && dlss_paused_) {
    for (const Stage stage : {Stage::BEFORE_UPSCALING, Stage::AFTER_DLSS}) {
      StageState& state = stages_[static_cast<size_t>(stage)];
      state.reset_owed = true;
      state.nr_applied = false;  // the card counts the passes that run; the DLSS groups run none while DLSS is paused
      state.passes_run = 0u;
    }
  }
  // A problem with the surface the placement works on (DLSS's output for After DLSS, the game's Color for Before upscaling: its format, its pipelines) keeps
  // the Session off, so a game that can never run NR pays nothing for it.
  const bool before_upscaling = (placement_.placement == Placement::BEFORE_UPSCALING);
  bool placement_ready = ((placement_.placement == Placement::AFTER_DLSS || before_upscaling) && !dlss_idle_ && output_message_.empty());
  bool later_present_ready = false;
  const bool allowed = (settings_.session.enabled && facts_.nr_allowed && blocked_reason_.empty());
  // Native NR at Present loads once the frame can take it (its encoding resolved) and the RGBA16F intermediate's pipelines are built, here, as the
  // DLSS placements' are. Split chain: a later Present group's frame too, which keeps NR loaded while the game's DLSS idles.
  if (PresentGroupNative() && ring_ != nullptr && present_problem_.empty() && present_encoding_.has_value()) {
    bool present_ready = true;
    if (allowed && !pipeline_->Prepare(VK_FORMAT_R16G16B16A16_SFLOAT)) {
      SetPresentProblem("The Vulkan colour pipelines for NR at Present could not be built");
      present_ready = false;
    }
    if (NativePresent()) {
      placement_ready = present_ready;
    } else {
      later_present_ready = present_ready;
    }
  }
  // The pipelines the placement needs are built here, in the present, when NR is about to load: the first compile (a few ms, once
  // per output format, or once for Before upscaling) stays out of the game's evaluate, and a placement whose pipelines cannot be built never loads NR.
  if (placement_ready && allowed && before_upscaling && !pipeline_->PreparePreSr()) {
    pre_sr_unbuildable_ = true;
    SetOutputProblem(std::string(VK_PRE_SR_PIPELINES_PROBLEM));
    placement_ready = false;
  } else if (placement_ready && allowed && (!before_upscaling || GroupAt(chain_, Stage::AFTER_DLSS).has_value()) && output_format_ != VK_FORMAT_UNDEFINED
             && !pipeline_->Prepare(output_format_)) {
    // Split chain: an After DLSS group behind Before upscaling needs DLSS's output pipelines too.
    unbuildable_formats_.push_back(output_format_);
    SetOutputProblem(std::format("The Vulkan colour pipeline for DLSS's output format (VkFormat {}) could not be built", static_cast<int>(output_format_)));
    placement_ready = false;
  }
  bool want = (allowed && (placement_ready || later_present_ready));
  // NR's Session belongs to the command buffers it was loaded for. A placement that moves between native Present (Uplift's own
  // buffers) and a DLSS stage (the game's) reloads it from OFF: the old path's GPU work finished first (the ring's fences, capped; the DLSS stage's through the
  // Session's own teardown marks), NGX's features, A, B and the Present surfaces released, and the new path creates its features afresh, as from cold.
  // After DLSS and Before upscaling are streams of their own, and a lasting change of the motion input reloads too.
  // Split chain: the chain's set of streams; any change of it reloads, and so does a pass moving to another stage while the set stays (its
  // feature would be evaluated in another stream). Never a warm per-group rebuild.
  const VkStreamSet wanted_streams = ChainStreams();
  constexpr VkStreamSet OWN = VkStreamBit(VkNrStream::OWN_BUFFERS);
  if (session_->State() == nr::SessionState::OFF) {
    session_streams_ = 0u;
    own_drain_waited_ = false;
    for (StageState& stage : stages_) {
      stage.motion.reset();
      stage.motion_mismatches = 0u;
    }
    input_reload_reason_.clear();
  }
  if (!stream_reload_ && !input_reload_reason_.empty()) {
    stream_reload_ = true;
    nr::Logf(nr::LogLevel::INFO, "Vulkan: NR reloads from off: {}", input_reload_reason_);
    input_reload_reason_.clear();
  }
  const bool streams_change = VkStreamsNeedReload(session_streams_, wanted_streams, session_->State() == nr::SessionState::OFF);
  const bool passes_move =
      (chain_passes_moved_ && session_streams_ != 0u && wanted_streams == session_streams_ && session_->State() != nr::SessionState::OFF);
  if (!stream_reload_ && (streams_change || passes_move)) {
    stream_reload_ = true;
    if (streams_change) {
      nr::Logf(nr::LogLevel::INFO, "Vulkan: NR moves from {} to {}: it reloads from off", StreamsName(session_streams_), StreamsName(wanted_streams));
    } else {
      nr::Logf(nr::LogLevel::INFO, "Vulkan: NR's passes move between its stages ({}): it reloads from off", FormatChainLine(chain_, {}));
    }
    if ((session_streams_ & OWN) != 0u && ring_ && !std::exchange(own_drain_waited_, true) && !ring_->WaitIdle(RING_SWITCH_WAIT)) {
      nr::Logf(nr::LogLevel::WARN, uplift::text::VK_RING_DRAIN_TIMEOUT_LINE, std::chrono::duration<double>(RING_SWITCH_WAIT).count());
      Drop(false);  // no NGX call: the GPU may still run them
      return;
    }
  }
  // Transitions: a hand-over to another owner (the add-on sets no grace then) drains NR loaded for Present here at once; Uplift's own command buffers finish
  // first (capped), as at a stage change. A drain with the configured grace (NR off by the user) needs no wait: its marks come long after.
  if (!want && (session_streams_ & OWN) != 0u && session_->State() != nr::SessionState::OFF && settings_.session.grace.count() == 0 && ring_
      && !std::exchange(own_drain_waited_, true) && !ring_->WaitIdle(RING_SWITCH_WAIT)) {
    nr::Logf(nr::LogLevel::WARN, uplift::text::VK_RING_DRAIN_TIMEOUT_LINE, std::chrono::duration<double>(RING_SWITCH_WAIT).count());
    Drop(false);
    return;
  }
  if (stream_reload_) {
    if (session_->State() == nr::SessionState::OFF) {
      stream_reload_ = false;
    } else {
      want = false;
      session_->SetGrace(std::chrono::milliseconds(0));  // straight to the drain; BeginFrame sets the configured grace again next present
    }
  }
  // As in DeviceContext, skip SetEnabled(false) only when the DLSS idle is the sole reason and the Session is suspended.
  const bool idle_only = (allowed && dlss_placement && dlss_idle_ && !later_present);
  if (!(idle_only && session_->Status().suspended)) {
    session_->SetEnabled(want);
  }
  const auto note_state = [this] {
    const nr::SessionState state = session_->State();
    if (state == logged_state_) return;
    nr::Logf(nr::LogLevel::INFO, "session {} -> {} (Vulkan)", nr::SessionStateName(logged_state_), nr::SessionStateName(state));
    logged_state_ = state;
    logged_after_dlss_ = false;  // each activation logs its trigger again
    logged_trigger_ = TriggerPoint::NONE;
    trace_frames_ = TRACE_FRAMES;
  };
  note_state();
  if (session_->State() == nr::SessionState::LOADING && PresentGroupNative() && before_present_load_) {
    before_present_load_();  // the crash marker goes on disk before NGX initialises on the game's device
  }
  session_->Tick();
  note_state();
  if (session_->State() != nr::SessionState::OFF && session_streams_ == 0u) {
    session_streams_ = wanted_streams;  // the buffers this load is for
  }
  // VRAM release after a disable: A, B and the zero motion go with NR.
  if (session_->State() == nr::SessionState::OFF || session_->State() == nr::SessionState::FAILED) {
    pipeline_->ReleaseIntermediates();
    // Off Present, Uplift's own command buffers go too, once their fences say the GPU is done (made again for Present).
    if (ring_ && !PresentGroupNative() && ring_->WaitIdle(std::chrono::milliseconds(0))) {
      ring_->Destroy();
      ring_.reset();
    }
  }
}

void VkDlssContext::UpdateChain() {
  // Why each stage cannot run now.
  const std::string_view dlss_cause = DlssCause(dlss_unavailable_reason_, dlss_seen_, DlssStagesOff(dlss_off_.Off(), CORE_HELD));
  ChainFacts facts = {
      .pass1 = placement_.placement,
      .passes = settings_.session.pass_count,
      .stored = settings_.pass_stages,
      .split_capable = facts_.split_capable,
  };
  facts.cannot[static_cast<size_t>(Stage::BEFORE_UPSCALING)] =
      BeforeUpscalingCause(dlss_cause, (pre_sr_unbuildable_ ? VK_PRE_SR_PIPELINES_PROBLEM : NO_STAGE_PROBLEM), BeforeUpscaling());
  // DLSS's output format is known only at the evaluate (output_message_), after the present decided the chain: the After DLSS group skips that evaluate
  // ("unsupported format") instead of the chain moving it, so the stage brings no problem of its own here.
  facts.cannot[static_cast<size_t>(Stage::AFTER_DLSS)] = AfterDlssCause(dlss_cause, NO_STAGE_PROBLEM);
  // As on Direct3D 12, DLSS-stage NR of a split seen on another queue than the effect queue (where a later Present group records) keeps
  // Present off for later groups: their passes join the DLSS stage, so NR never runs on two queues at once (no GPU wait can order them safely). Checked only
  // while a split is stored.
  if (effect_queue_ != nullptr && SplitStored(settings_, facts_.split_capable)) {
    const void* const other_queue = stage_work_.OtherQueue(effect_queue_);
    if (other_queue != other_dlss_queue_) {
      if (other_queue != nullptr) {
        nr::Logf(nr::LogLevel::INFO, "Vulkan: the game's DLSS runs on another queue ({}) than ReShade's effects ({}): passes stored at Present run at the DLSS "
                 "stage", other_queue, effect_queue_);
      } else {
        nr::Log(nr::LogLevel::INFO, "Vulkan: the game's DLSS-stage NR runs on ReShade's effect queue again: passes stored at Present run there again");
      }
      other_dlss_queue_ = other_queue;
    }
  }
  // The evidence of frame generation: the live NGX FrameGeneration features, as this context has no detector.
  const bool frame_generation_on = (facts_.ngx_frame_generation > 0u);
  std::string_view present_cause;
  if (!facts_.vk_native_present) {
    present_cause = NOT_NATIVE_PRESENT_REASON;  // the bridge and the helper hold one stage only
  } else if (!present_problem_.empty()) {
    present_cause = present_problem_;  // the back buffer cannot take NR (ResolvePresentFrame): a lasting cause, never one frame's missing image
  } else if (FrameGenerationKeepsLaterPresentOff(frame_generation_on, facts_.fg_present_path)) {
    present_cause = FRAME_GENERATION_REASON;
  } else if (other_dlss_queue_ != nullptr) {
    present_cause = OTHER_QUEUE_REASON;
  }
  facts.cannot[static_cast<size_t>(Stage::PRESENT)] = LaterPresentCause(facts_.fg_present_path, present_cause);
  ChainPlan chain = DecideChain(facts);

  // A pass that moves to another stage while the chain's streams stay would have its feature evaluated in another stream; the
  // Session reloads from OFF then, as for a stream change (UpdateSession).
  chain_passes_moved_ = false;
  LogChainChange(chain, chain_);
  if (!SameGroups(chain, chain_)) {
    const auto passes_of = [](const ChainPlan& plan) {
      return (plan.group_count == 0u ? 0u : plan.groups[plan.group_count - 1u].first + plan.groups[plan.group_count - 1u].count);
    };
    const uint32_t common = std::min(passes_of(chain), passes_of(chain_));
    for (uint32_t pass = 0u; pass < common; ++pass) {
      chain_passes_moved_ = (chain_passes_moved_ || chain.effective[pass] != chain_.effective[pass]);
    }
  }
  const std::array<bool, STAGE_COUNT> reset = StagesToReset(chain, chain_);
  for (size_t stage = 0u; stage < STAGE_COUNT; ++stage) {
    if (reset[stage]) {
      stages_[stage] = {};
    }
  }
  chain_ = std::move(chain);
  FillSlices(chain_, slices_);
  if (chain_.group_count > 0u) {
    session_->SetChain(std::span(slices_).first(chain_.group_count));
  }
  // A group the chain no longer has frees its set behind its last use; set 0 stays, as the single set always did.
  pipeline_->RetireSetsFrom(std::max(chain_.group_count, 1u));
  if (!chain_.Split()) {
    stage_work_.Clear();  // no later Present group waits for them
  }
}

VkStreamSet VkDlssContext::ChainStreams() const {
  VkStreamSet streams = 0u;
  for (uint32_t index = 0u; index < chain_.group_count; ++index) {
    switch (chain_.groups[index].stage) {
      case Stage::BEFORE_UPSCALING: streams |= VkStreamBit(VkNrStream::BEFORE_UPSCALING); break;
      case Stage::AFTER_DLSS:       streams |= VkStreamBit(VkNrStream::AFTER_DLSS); break;
      case Stage::PRESENT:          streams |= (facts_.vk_native_present ? VkStreamBit(VkNrStream::OWN_BUFFERS) : VkStreamSet{0u}); break;
    }
  }
  return streams;
}

bool VkDlssContext::StageWorkDone() const {
  const nr::Timeline& timeline = completion_->Timeline();
  return stage_work_.Done(effect_queue_, [&timeline](uint64_t token) { return timeline.TokenComplete(token); });
}

void VkDlssContext::NoteResult(Stage stage, uint32_t group, const sources::PipelineResult& result, RecordingStatus::Scale scale) {
  StageState& state = stages_[static_cast<size_t>(stage)];
  state.nr_applied = result.nr_applied;
  state.passes_run = result.passes_run;
  state.skip_reason = (result.nr_applied ? nr::SkipReason::NONE : result.reason);
  state.motion_source = result.motion_source;
  stage_recordings_[static_cast<size_t>(stage)] += (result.nr_applied ? 1u : 0u);
  if (group == 0u) {
    status_.From(result, scale);  // the status follows pass 1's group: one NR frame per frame
  }
}

void VkDlssContext::NotePlacementChange() {
  if (!PlacementChanged(logged_placement_, placement_.placement)) return;
  nr::Log(nr::LogLevel::INFO, FormatPlacementLine(placement_, main_snapshot_, true));
  // A new placement's work size applies at once.
  if (const std::optional<Stage> stage = PlacementStage(placement_.placement)) {
    stages_[static_cast<size_t>(*stage)].settle.Reset();
  }
}

void VkDlssContext::SetOutputProblem(std::string problem) {
  if (problem == output_message_) return;
  if (!problem.empty()) {
    if (placement_.placement == Placement::PRESENT) {
      // At Present the evaluate only feeds the copies of DLSS's vectors; the problem matters to the DLSS placements alone.
      nr::Logf(nr::LogLevel::INFO, "{} (NR runs on the presented frame here: this applies to the DLSS placements only)", problem);
    } else {
      nr::Log(nr::LogLevel::WARN, problem);
    }
  }
  output_message_ = std::move(problem);
}

void VkDlssContext::NoteDlssFeature(uint64_t serial) {
  if (serial == dlss_feature_serial_) return;
  if (dlss_feature_serial_ != 0u) {
    stages_[static_cast<size_t>(Stage::BEFORE_UPSCALING)].reset_owed = true;
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].reset_owed = true;
    present_reset_owed_ = true;
    before_reset_.reset();
  }
  dlss_feature_serial_ = serial;
}

sources::PipelineResult VkDlssContext::OnDlssEvaluate(VkCommandBuffer buffer, const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies,
                                                      VkListState& list, std::chrono::steady_clock::time_point now) {
  // After a device loss Uplift never records in a game command buffer or calls the snippet again.
  if (!torn_down_ && !dropped_ && completion_->PollDeviceLost()) {
    NoteDeviceLost(now);
  }
  if (torn_down_ || dropped_ || core_shut_down_) {
    before_reset_.reset();
    status_.Clear();
    return {.reason = (core_shut_down_ ? nr::SkipReason::NGX_SHUT_DOWN : nr::SkipReason::DEVICE_LOST)};
  }
  NoteDlssFeature(frame.serial);
  const std::optional<bool> before_reset = std::exchange(before_reset_, std::nullopt);  // this evaluate's, from BeforeDlssEvaluate
  dlss_seen_ = true;
  last_main_evaluate_ = now;
  const ngx_hooks::MotionJitterOffset jitter = present_jitter_.Update(frame);
  main_snapshot_ = frame.snapshot;
  main_feature_ = frame.feature;
  if (dlss_off_.OnEvaluate(!CORE_HELD) && std::exchange(dlss_off_logged_, false)) {
    // The game's DLSS runs again after it switched it off: the DLSS placement returns from the next present, as on first sight. Every
    // evaluate starts the release window over.
    nr::Log(nr::LogLevel::INFO, DlssOnAgainLine(settings_.source == ui::PlacementSource::DLSS
                                                    ? ChoosePlacement(ui::PlacementSource::AUTO, dlss_unavailable_reason_, true, BeforeUpscaling())
                                                    : PlacementChoice{.placement = Placement::PRESENT}));
  }
  last_motion_vectors_missing_ = (frame.motion_vectors == nullptr);
  // No present-starvation catch-up on Vulkan (D3D12's ticks the Session here): the frame semaphore can only be signalled in the present, where
  // ReShade holds the queue's lock. A newly seen DLSS moves the placement at the next present.
  const nr::Rect region = ngx_hooks::ResolveVkOutputRegion(frame, copies);
  output_size_ = {region.width, region.height};
  const bool image = (frame.output != nullptr && copies.output.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW);
  output_format_ = (image ? copies.output.Resource.ImageViewInfo.Format : VK_FORMAT_UNDEFINED);
  // The Session hears the size NR works at in every state. After DLSS: the settled work canvas of the output region at the Resolution mode.
  // Split chain: the After DLSS group's, pass 1's or a later one behind Before upscaling (at its stage's own Resolution).
  const std::optional<uint32_t> after_group = GroupAt(chain_, Stage::AFTER_DLSS);
  sources::WorkLayout layout;
  if (after_group && !dlss_idle_ && !output_size_.Empty()) {
    layout = SettleLayout(output_size_, Stage::AFTER_DLSS, now);
  } else {
    session_->NoteFrameSize(SessionFrameSize());
  }
  // What keeps NR off for this output (Before upscaling: for the game's Color), before anything is recorded (and before NR ever loads for it).
  const std::optional<uint32_t> variant = color::VkColorPipeline::OutputVariantOf(output_format_);
  std::string color_problem;
  if (BeforeUpscaling()) {
    // The padded canvas takes any render size: only the Color's own format can keep NR off.
    const bool color_image = (frame.color != nullptr && copies.color.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW);
    const VkFormat color_format = (color_image ? copies.color.Resource.ImageViewInfo.Format : VK_FORMAT_UNDEFINED);
    if (!color_image) {
      color_problem = "DLSS's colour input is not an image view";
    } else if (!sources::VkNrPipeline::PreSrColorFormatSupported(color_format)) {
      color_problem = std::format("Unsupported DLSS colour format on Vulkan (VkFormat {})", static_cast<int>(color_format));
    } else if (pre_sr_unbuildable_) {
      color_problem = VK_PRE_SR_PIPELINES_PROBLEM;
    }
  }
  // DLSS's output, for After DLSS (pass 1's, or split chain: a later group behind Before upscaling).
  nr::SkipReason skip = nr::SkipReason::NONE;
  std::string problem;
  if (!BeforeUpscaling() || after_group) {
    if (!image) {
      skip = nr::SkipReason::INVALID_INPUT;
      problem = "DLSS's output is not an image view";
    } else if (!nr::MeetsNrFloor(output_size_)) {
      skip = nr::SkipReason::FRAME_TOO_SMALL;
      problem = std::format("NR needs a frame of at least {}x{} (DLSS's output region is {}x{})", nr::MIN_NR_LONG_SIDE, nr::MIN_NR_SHORT_SIDE,
                            output_size_.width, output_size_.height);
    } else if (!variant) {
      skip = nr::SkipReason::UNSUPPORTED_FORMAT;
      problem = std::format("Unsupported DLSS output format on Vulkan (VkFormat {})", static_cast<int>(output_format_));
    } else if (!storage_formats_[*variant]) {
      skip = nr::SkipReason::UNSUPPORTED_FORMAT;
      problem = std::format("DLSS's output format (VkFormat {}) is not a storage format on this GPU's Vulkan driver", static_cast<int>(output_format_));
    } else if (std::ranges::find(unbuildable_formats_, output_format_) != unbuildable_formats_.end()) {
      skip = nr::SkipReason::UNSUPPORTED_FORMAT;
      problem = std::format("The Vulkan colour pipeline for DLSS's output format (VkFormat {}) could not be built", static_cast<int>(output_format_));
    }
  }
  SetOutputProblem((BeforeUpscaling() && (!color_problem.empty() || !after_group)) ? std::move(color_problem) : std::move(problem));
  // Whether this evaluate's vectors can be copied for the Present path. Noted at every evaluate the context sees,
  // and kept while it sees none (with the copies not asked for, the evaluate is a passthrough), so a game that passes no readable vectors keeps DLSS's vectors
  // greyed at Present instead of offering them again after a pick away from them.
  const bool image_view = (frame.motion_vectors != nullptr && copies.motion_vectors.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW);
  const NVSDK_NGX_ImageViewInfo_VK& vectors = copies.motion_vectors.Resource.ImageViewInfo;  // read only for an image view
  nr::Rect motion_region = frame.motion_region;
  if (image_view && (motion_region.width == 0u || motion_region.height == 0u)) {
    motion_region = {.x = 0u, .y = 0u, .width = vectors.Width, .height = vectors.Height};
  }
  const bool readable = (image_view && sources::VkNrPipeline::MotionFormatReadable(vectors.Format) && uint64_t{motion_region.x} + motion_region.width <= vectors.Width
                         && uint64_t{motion_region.y} + motion_region.height <= vectors.Height);
  present_motion_gap_ = (readable ? ui::MotionGap::NONE : ui::MotionGap::GAME_PASSED_NONE);
  if (readable) {
    dlss_motion_size_ = {motion_region.width, motion_region.height};  // the Present copies' shape, for a present that misses its copy
  }
  // NR runs on the Present path with DLSS's own motion vectors, copied here in the game's frame, right after DLSS read them, for the present that closes it:
  // pass 1's (the Session never loads for the copy: nothing of NGX's is called on this device), or a later group's, which the
  // After DLSS group's recording then follows on the same buffer (only for a Present group that runs).
  const bool pass1_present = (placement_.placement == Placement::PRESENT);
  const std::optional<uint32_t> present_group = GroupAt(chain_, Stage::PRESENT);
  const bool later_present_copy = (!pass1_present && present_group && session_->SliceRuns(slices_[*present_group]));
  sources::PipelineResult motion_copy = {.reason = nr::SkipReason::NOT_AFTER_DLSS};
  if (facts_.present_motion_copy && (pass1_present || later_present_copy)) {
    PresentMotionCounters& counters = present_motion_counters_;
    ++counters.evaluates;
    counters.offset_x = jitter.x * static_cast<float>(motion_region.width) * settings_.motion_scale_x;
    counters.offset_y = jitter.y * static_cast<float>(motion_region.height) * settings_.motion_scale_y;
    const nr::Timeline& timeline = completion_->Timeline();
    const uint64_t current = timeline.CurrentFrame();
    const uint64_t completed = timeline.CompletedFrame();
    const uint64_t lag = (current > completed ? current - completed : 0u);
    counters.lag_min = std::min(counters.lag_min, lag);
    counters.lag_max = std::max(counters.lag_max, lag);
    if (!readable) {
      ++counters.skipped_input;
      motion_copy = {.reason = nr::SkipReason::NO_READABLE_MOTION};
    } else {
      const bool opened_here = OpenToken(list);
      const sources::PresentMotionWrite write =
          pipeline_->RecordPresentMotion(buffer, {.view = vectors.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, motion_region,
                                         frame.mv_scale_x * settings_.motion_scale_x, frame.mv_scale_y * settings_.motion_scale_y, current + 1u,
                                         facts_.dlss_motion_upside_down,
                                         jitter.x * static_cast<float>(motion_region.width) * settings_.motion_scale_x,
                                         jitter.y * static_cast<float>(motion_region.height) * settings_.motion_scale_y);  // a throw leaves the token open
      const bool copied = (write == sources::PresentMotionWrite::WRITTEN);
      switch (write) {
        case sources::PresentMotionWrite::WRITTEN:   ++counters.copies; break;
        case sources::PresentMotionWrite::NO_INPUT:  ++counters.skipped_input; break;
        case sources::PresentMotionWrite::RING_BUSY: ++counters.skipped_ring; break;
        case sources::PresentMotionWrite::SLOT_BUSY: ++counters.skipped_slot; break;
      }
      if (opened_here && !copied) {
        DropOpenToken(list);
      }
      if (copied) {
        last_game_list_recording_ = now;  // the latch window covers this recording too
        if (!present_motion_logged_) {
          present_motion_logged_ = true;
          nr::Logf(nr::LogLevel::INFO, "Vulkan: DLSS's motion vectors copied for Present ({}x{})", motion_region.width, motion_region.height);
        }
      }
      motion_copy = {.recorded = copied, .reason = nr::SkipReason::NOT_AFTER_DLSS};
    }
  }
  if (pass1_present) return motion_copy;
  if (!after_group || dlss_idle_) return motion_copy;  // Before upscaling already ran in BeforeDlssEvaluate
  const uint32_t group = *after_group;
  StageState& stage = stages_[static_cast<size_t>(Stage::AFTER_DLSS)];
  const auto skip_group = [&](nr::SkipReason reason, bool owe_reset) -> sources::PipelineResult {
    stage.nr_applied = false;
    stage.passes_run = 0u;
    stage.skip_reason = reason;
    stage.motion_source = sources::MotionSource::NONE;
    stage.reset_owed = (stage.reset_owed || owe_reset);
    if (group == 0u) {
      status_.NoteSkip(reason, RecordingStatus::Reset::ALL_BUT_MOTION);
    }
    return {.reason = reason};
  };
  if (skip != nr::SkipReason::NONE) return skip_group(skip, false);
  if (session_->State() != nr::SessionState::ACTIVE) {
    stage.nr_applied = false;
    stage.skip_reason = nr::SkipReason::NONE;
    if (group == 0u) {
      status_.NoteSkip(nr::SkipReason::NONE, RecordingStatus::Reset::APPLIED_AND_REASON);  // NR is off: nothing to explain
    }
    return {.reason = nr::SkipReason::NR_OFF};
  }
  // Split chain: a later group the budget cut to no pass (or the Defaults view hides) skips before anything is recorded; its next recording starts afresh.
  if (group > 0u && !session_->SliceRuns(slices_[group])) return skip_group(SliceSkipReason(settings_, slices_[group]), true);
  // DLSS's output changed format while NR runs (an HDR toggle). Its pipelines are built at the next present, never inside the
  // game's evaluate (a first compile takes milliseconds): this one frame keeps DLSS's own output, and NR's history starts afresh after it.
  if (!pipeline_->Prepared(output_format_)) return skip_group(nr::SkipReason::PREPARING_FORMAT, true);
  // Never a different motion input from the one this load's features were first evaluated with.
  if (const nr::SkipReason motion_skip =
          CheckMotion(Stage::AFTER_DLSS, MotionShapeOf(ReadDlssFrame(frame, copies, &copies.output, region, false).inputs, sources::ResolveLayout(layout, output_size_).canvas, false));
      motion_skip != nr::SkipReason::NONE) {
    return skip_group(motion_skip, true);
  }

  // Pessimistic; only a frame that applies NR clears the owed reset.
  const bool reset_was_owed = std::exchange(stage.reset_owed, true);
  ApplyLookConfig(now, group);
  // The game's reset, filtered once per evaluate: behind a Before upscaling group (split chain), the answer that group's recording got.
  const bool game_reset = (before_reset ? *before_reset : reset_merger_.Filter(frame.reset, now));
  const ReadFrame game = ReadDlssFrame(frame, copies, &copies.output, region, (game_reset || reset_was_owed));
  // The token is issued before recording (every mark taken while recording includes it) and stays open on the list; EndEvaluate sets
  // its event at the end of the hooked call, on every path. A token Before upscaling (or the Present motion copy) opened in this call is reused.
  const bool opened_here = OpenToken(list);
  const sources::PipelineResult result =
      pipeline_->RecordAfterDlss(buffer, game.target, game.inputs, controls_, layout, {.slice = slices_[group], .set = group});  // a throw leaves the token open
  if (opened_here && !result.recorded) {
    DropOpenToken(list);
  }
  if (result.recorded) {
    last_game_list_recording_ = now;
    if (chain_.Split()) {
      stage_work_.Note(list.open_token);  // where its buffer goes decides whether Present may run beside it
    }
  }
  if (result.nr_applied) {
    stage.reset_owed = false;
  }
  NoteResult(Stage::AFTER_DLSS, group, result, RecordingStatus::Scale::RESULT);
  if (group == 0u) {
    if (result.nr_applied && !logged_after_dlss_) {
      logged_after_dlss_ = true;  // once per activation
      nr::Log(nr::LogLevel::INFO, "NR trigger: after DLSS (Vulkan)");
    }
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "evaluate: NR {} after DLSS (Vulkan), {} pass(es) {}", (result.nr_applied ? "applied" : "skipped"), result.passes_run,
             nr::SkipReasonText(result.reason));
  }
  return result;
}

VkDlssContext::ReadFrame VkDlssContext::ReadDlssFrame(const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies,
                                                      const NVSDK_NGX_Resource_VK* resource, nr::Rect region, bool reset_hint) const {
  const sources::GameFrame game = sources::ReadGameFrame(frame, nullptr, region, reset_hint,
                                                         {
                                                             .encoding = settings_.encoding,
                                                             .diffuse_white_nits = settings_.diffuse_white_nits,
                                                             .transfer_strength = settings_.transfer_strength,
                                                             .color_strength = settings_.color_strength,
                                                             .motion_vectors = facts_.motion_at_dlss_stage.value_or(settings_.motion_vectors),
                                                             .motion_scale_x = settings_.motion_scale_x,
                                                             .motion_scale_y = settings_.motion_scale_y,
                                                             .depth_inverted = settings_.depth_inverted,
                                                             .chained_history = settings_.chained_history,
                                                         });
  return {
      .target =
          {
              .resource = resource,
              .region = region,
              .encoding = game.target.encoding,
              .diffuse_white_nits = game.target.diffuse_white_nits,
              .transfer_strength = game.target.transfer_strength,
              .color_strength = game.target.color_strength,
              .exposure = (frame.exposure_texture != nullptr ? &copies.exposure_texture : nullptr),
              .exposure_factor = game.target.exposure_factor,
          },
      .inputs = game.inputs,
  };
}

void VkDlssContext::ApplyLookConfig(std::chrono::steady_clock::time_point now, uint32_t group) {
  // As DeviceContext::ApplyLookConfig, into this context's pipeline.
  const std::optional<float> frame_seconds = FrameSecondsSince(last_look_recordings_[group], now);
  const uint32_t shaped = ShapedGroup(chain_, [this](uint32_t index) { return session_->SliceRuns(slices_[index]); });
  pipeline_->SetLookConfig(GroupLookConfig(settings_, chain_, group, shaped, frame_seconds));
}

bool VkDlssContext::OpenToken(VkListState& list) {
  if (list.open_token != 0u) return false;
  list.open_token = completion_->Timeline().IssueToken();
  list.tokens.push_back(list.open_token);  // recording order: a re-execution maps to the last one's event
  if (list.first_token == 0u) {
    list.first_token = list.open_token;
  }
  return true;
}

void VkDlssContext::DropOpenToken(VkListState& list) {
  const uint64_t token = std::exchange(list.open_token, 0u);
  list.tokens.pop_back();
  if (list.first_token == token) {
    list.first_token = 0u;
  }
  completion_->Timeline().Drop(std::span<const uint64_t>(&token, 1u));
}

const NVSDK_NGX_Resource_VK* VkDlssContext::BeforeDlssEvaluate(VkCommandBuffer buffer, const ngx_hooks::DlssFrame& frame,
                                                               const ngx_hooks::VkDlssResources& copies, VkListState& list,
                                                               std::chrono::steady_clock::time_point now) {
  before_reset_.reset();  // split chain: each evaluate's own, set only by this evaluate's Before upscaling recording
  // After a device loss Uplift never records in a game command buffer or calls the snippet again.
  if (!torn_down_ && !dropped_ && completion_->PollDeviceLost()) {
    NoteDeviceLost(now);
  }
  if (torn_down_ || dropped_ || core_shut_down_) return nullptr;
  NoteDlssFeature(frame.serial);
  if (placement_.placement != Placement::BEFORE_UPSCALING || dlss_idle_) return nullptr;  // Before upscaling is always pass 1's group
  // The work image is the settled size of the render region at the Resolution mode, and below the floor it is padded into the floor
  // canvas. The set is keyed on it while the render region moves (the decode upsamples). The Session hears the canvas in every state.
  frame_size_ = {frame.color_region.width, frame.color_region.height};
  const sources::WorkLayout layout = (frame_size_.Empty() ? sources::WorkLayout{} : SettleLayout(frame_size_, Stage::BEFORE_UPSCALING, now));
  if (session_->State() != nr::SessionState::ACTIVE) {
    status_.NoteSkip(nr::SkipReason::NONE, RecordingStatus::Reset::APPLIED_AND_REASON);  // NR is off: nothing to explain
    return nullptr;
  }
  StageState& stage = stages_[static_cast<size_t>(Stage::BEFORE_UPSCALING)];
  {
    // As After DLSS's. In a padded canvas the game's vectors reach NGX as an RG16F canvas copy (or the zero image).
    const nr::FrameInputs probe = ReadDlssFrame(frame, copies, (frame.color != nullptr ? &copies.color : nullptr), frame.color_region, false).inputs;
    const nr::Size image = (layout.image.Empty() ? frame_size_ : layout.image);
    const nr::Size canvas = sources::ResolveLayout(layout, frame_size_).canvas;
    if (const nr::SkipReason motion_skip = CheckMotion(Stage::BEFORE_UPSCALING, MotionShapeOf(probe, canvas, canvas != image)); motion_skip != nr::SkipReason::NONE) {
      status_.NoteSkip(motion_skip, RecordingStatus::Reset::ALL_BUT_MOTION);
      stage.nr_applied = false;
      stage.passes_run = 0u;
      stage.skip_reason = motion_skip;
      stage.reset_owed = true;
      return nullptr;
    }
  }
  // Pessimistic; only a frame that applies NR clears the owed reset.
  const bool reset_was_owed = std::exchange(stage.reset_owed, true);
  ApplyLookConfig(now, 0u);
  const bool game_reset = reset_merger_.Filter(frame.reset, now);
  if (chain_.Split()) {
    before_reset_ = game_reset;  // the After DLSS group behind it in this evaluate takes the same answer
  }
  const ReadFrame game =
      ReadDlssFrame(frame, copies, (frame.color != nullptr ? &copies.color : nullptr), frame.color_region, (game_reset || reset_was_owed));
  const bool opened_here = OpenToken(list);
  const sources::PipelineResult result =
      pipeline_->RecordPreSr(buffer, game.target, game.inputs, controls_, layout, {.slice = slices_[0], .set = 0u});  // a throw leaves the token open
  if (opened_here && !result.recorded) {
    DropOpenToken(list);
  }
  if (result.recorded) {
    last_game_list_recording_ = now;  // the latch window covers pre-SR's recordings too
    if (chain_.Split()) {
      stage_work_.Note(list.open_token);  // as After DLSS's
    }
  }
  if (result.nr_applied) {
    stage.reset_owed = false;
  }
  NoteResult(Stage::BEFORE_UPSCALING, 0u, result, RecordingStatus::Scale::RESULT);  // always pass 1's group
  if (result.nr_applied && !logged_after_dlss_) {
    logged_after_dlss_ = true;  // once per activation
    nr::Log(nr::LogLevel::INFO, "NR trigger: before upscaling (Vulkan)");
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "evaluate: NR {} before upscaling (Vulkan), {} pass(es) {}", (result.nr_applied ? "applied" : "skipped"), result.passes_run,
             nr::SkipReasonText(result.reason));
  }
  return (result.nr_applied ? pipeline_->PrivateColor() : nullptr);
}

VkImage VkDlssContext::PrepareMaskCopy(VkFormat format, nr::Size size, bool* first) {
  if (torn_down_ || dropped_ || core_shut_down_ || !pipeline_) return VK_NULL_HANDLE;
  const bool dlss_placement = (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  // At a DLSS placement, and at native Present (split chain: a later group's too).
  const bool wanted = (settings_.look.mask && (dlss_placement || PresentGroupNative()) && session_->State() == nr::SessionState::ACTIVE);
  return pipeline_->PrepareMaskCopy((wanted ? format : VK_FORMAT_UNDEFINED), size, first);
}

void VkDlssContext::NoteMaskCopied() {
  if (torn_down_ || dropped_ || !pipeline_) return;
  pipeline_->NoteMaskCopied();
}

void VkDlssContext::ReleaseMask() {
  // After the game's NGX shutdown the mask copy waits for Teardown (ReShade's queue may still copy into it, and every mark reads complete
  // once the shutdown declared the timeline idle).
  if (torn_down_ || dropped_ || core_shut_down_ || !pipeline_) return;
  pipeline_->ReleaseMaskCopy();
}

void VkDlssContext::OnColorSwapRejected() {
  // As DeviceContext's: BeforeDlssEvaluate applied NR into the private colour and its status says so, on the assumption the detour would swap it into the game's
  // block. It calls this only when it could not (the block held no Color), so DLSS read its own Color unchanged this evaluate.
  if (placement_.placement != Placement::BEFORE_UPSCALING || !status_.nr_applied) return;
  status_.NoteSkip(nr::SkipReason::NO_COLOR_SWAP, RecordingStatus::Reset::APPLIED_AND_REASON);
}

void VkDlssContext::EndEvaluate(VkCommandBuffer buffer, VkListState& list) {
  const uint64_t token = std::exchange(list.open_token, 0u);
  if (token == 0u || torn_down_ || dropped_) return;
  // ALL_COMMANDS: the event's first scope is everything earlier on the queue, so it covers DLSS, NR and Uplift's passes on whichever queue the buffer runs.
  if (const VkEvent event = completion_->EventFor(token); event != VK_NULL_HANDLE) {
    functions_.vkCmdSetEvent(buffer, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  }
}

void VkDlssContext::Executed(std::span<const uint64_t> tokens, void* queue) {
  if (torn_down_ || dropped_) return;
  completion_->Executed(tokens);
  // Split chain: where a split's DLSS-stage NR goes. A queue other than the effect queue keeps later groups off Present (UpdateChain).
  stage_work_.Executed(tokens, queue);
}

void VkDlssContext::Reexecuted(std::span<const uint64_t> tokens, uint64_t first_token, void* queue) {
  if (torn_down_ || dropped_) return;
  completion_->Reexecuted(tokens, first_token, queue);
  stage_work_.Executed(tokens, queue);  // submitted again, maybe elsewhere: the latch and the completed-work check hear of it
}

void VkDlssContext::Recycle(std::span<const uint64_t> tokens) {
  if (torn_down_ || dropped_) return;
  completion_->Recycle(tokens);
  // By Vulkan's rules the buffer was not pending: its DLSS-stage recordings are complete, or never ran.
  stage_work_.Recycled(tokens);
}

void VkDlssContext::NoteDeviceLost(std::chrono::steady_clock::time_point now) {
  if (device_lost_) return;
  device_lost_ = true;
  dlss_latch_tripped_ = (last_game_list_recording_.has_value() && now - *last_game_list_recording_ <= DLSS_LATCH_WINDOW);
  present_latch_tripped_ = (last_present_submission_.has_value() && now - *last_present_submission_ <= DLSS_LATCH_WINDOW);  // the same DLSS_LATCH_WINDOW, for native Present
  output_message_ = (dlss_latch_tripped_ ? std::string("Vulkan device lost soon after NR ran inside the game's frame: the DLSS placements stay off from the next start "
                                                       "(Clear latch to try again)")
                                         : std::string("Vulkan device lost: NR stays off on this device"));
  nr::Log(nr::LogLevel::ERR, output_message_);
  Drop(true);
}

void VkDlssContext::Drop(bool device_lost) {
  if (dropped_) return;
  dropped_ = true;
  if (device_lost && session_) {
    session_->OnDeviceLost();  // no NGX call; the runtime's patches restored, its module left mapped
  } else if (host_) {
    host_->AbandonRuntime();  // no NGX call: the runtime's patches restored (nothing to do for an unloaded one)
  }
  // Leaked on purpose. Their destructors would call NGX or Vulkan; the device's own end takes their memory.
  static_cast<void>(pipeline_.release());
  static_cast<void>(session_.release());
  static_cast<void>(host_.release());
  static_cast<void>(ring_.release());  // its buffers may still be pending; the device's end takes them
  if (completion_) {
    completion_->Abandon();  // every query says complete; no event or semaphore destroyed
  }
}

void VkDlssContext::OnCoreShutdown(std::chrono::milliseconds cap) {
  if (torn_down_ || dropped_ || core_shut_down_) return;
  core_shut_down_ = true;
  // At native Present the route goes on along the chain, but only after a clean release (below) and once the game has gone on
  // presenting (BeginFrame). WantsNr is false from now on. A Drop or a loss here never hands NR on: an abandoned runtime may still be mapped.
  const auto pend_stop = [this] {
    if (NativePresent()) {
      pending_stop_since_ = std::chrono::steady_clock::now();
      pending_stop_presents_ = 0u;
    }
  };
  status_.Clear();
  output_message_ = "The game shut NVIDIA's NGX down on this device: NR stays off here until the game restarts";
  if (completion_->PollDeviceLost()) {
    NoteDeviceLost(std::chrono::steady_clock::now());
    return;
  }
  // The Present path's copy slots and the mask copy are also used on ReShade's queue (the bridge's copy-in reads a slot, the add-on copies
  // into the mask), after the game's last command buffer. No completion token covers that work, and the frame signal that would only comes with the next
  // present, which runs no frame of Uplift's any more (BeginFrame returns early from now on). So nothing of theirs is freed here: they wait for Teardown,
  // after the game's own idle for vkDestroyDevice (VkNrPipeline keeps them out of the timeline's pending releases, which the Session's flush below runs).
  const auto note_kept = [this] {
    if (const uint64_t kept = pipeline_->HeldBytes(); kept > 0u) {
      nr::Logf(nr::LogLevel::INFO, "Vulkan: Uplift's copies that ReShade's queue may still use ({:.1f} MiB) stay until the game's device goes",
               static_cast<double>(kept) / BYTES_PER_MIB);
    }
  };
  if (session_->State() == nr::SessionState::OFF) {
    nr::Log(nr::LogLevel::INFO, "Vulkan: the game shut NGX down on its device; NR was off there");
    note_kept();
    pend_stop();
    return;  // nothing of NGX's is held and nothing is freed: no wait
  }
  // Every piece of NR's own work at the DLSS placements is in a game command buffer whose recording ends with its token's event, so the events say when the
  // GPU is done with it. At Present it is in Uplift's own command buffers, whose fences say so.
  const auto started = std::chrono::steady_clock::now();
  bool finished = completion_->WaitSubmitted(cap);
  if (finished && ring_ != nullptr) {
    const auto left = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(cap - (std::chrono::steady_clock::now() - started)),
                               std::chrono::milliseconds(0));
    finished = ring_->WaitIdle(left);
  }
  if (completion_->DeviceLost()) {
    NoteDeviceLost(std::chrono::steady_clock::now());
    return;
  }
  if (!finished) {
    nr::Logf(nr::LogLevel::WARN, "Vulkan: NR's last frames did not finish within {} ms of the game's NGX shutdown; NR's runtime stays loaded until the game exits",
             cap.count());
    Drop(false);  // no NGX call: the game's command buffers may still be running NR
    return;
  }
  // Not a GPU idle: it declares Uplift's timeline idle, on the evidence of the events, so every mark reads complete from now on and the
  // Session's flush below neither signals a queue nor waits. What that flush and the release below free was used in the game's command buffers alone.
  completion_->DeviceIdle();
  session_->OnDeviceDestroyed();  // the features' release, DestroyParameters, the snippet's Shutdown1, the unload: all before the core's Shutdown1
  // BeginFrame returns early from now on, so UpdateSession's release at OFF never runs again; a game that goes on after
  // shutting NGX down would keep A, B and the private colour until it exits. The mask copy is not among them (see above).
  pipeline_->ReleaseNrSurfaces();
  nr::Log(nr::LogLevel::INFO, "Vulkan: NR released on the game's device before the game's NGX shutdown");
  note_kept();
  pend_stop();
}

void VkDlssContext::Teardown() {
  if (torn_down_) return;
  torn_down_ = true;
  if (dropped_) return;
  if (completion_->PollDeviceLost()) {
    NoteDeviceLost(std::chrono::steady_clock::now());
    return;
  }
  // The game idled the device, which covers the effect queue where Uplift's own command buffers ran; their fences confirm it before anything they
  // used goes. If they do not (a broken idle), nothing is freed and NGX is not called: as an abandon.
  if (ring_ && !ring_->WaitIdle(RING_TEARDOWN_WAIT)) {
    nr::Log(nr::LogLevel::WARN, "Vulkan: Uplift's command buffers had not finished when the game's device went; NR's runtime stays loaded until the game exits");
    Drop(false);
    return;
  }
  completion_->DeviceIdle();  // the game idled the device, so the flush below finds every token and frame complete
  if (ring_) {
    ring_->Destroy();  // the idle covers the effect queue, where Uplift's own command buffers ran
  }
  pipeline_->ReleasePresentMotion();
  present_jitter_.Reset();
  pipeline_->ReleaseIntermediates();
  if (!core_shut_down_) {
    session_->OnDeviceDestroyed();  // the flush (immediate now), the features' release, Shutdown1, the unload
  } else {
    completion_->Timeline().Flush(std::chrono::seconds(2));  // the Session's flush did not run (the core shut down first): what is still pending goes now
  }
  pipeline_.reset();       // the surfaces, the pipelines, the constants ring and the placeholders
  completion_->FreeAll();  // the events and the frame semaphore
}

void VkDlssContext::Abandon() {
  if (torn_down_) return;
  torn_down_ = true;
  if (!dropped_ && session_ && session_->State() != nr::SessionState::OFF) {
    nr::Log(nr::LogLevel::WARN, "Vulkan: the game's device went before Uplift could release NR on it; NR's runtime stays loaded until the game exits");
  }
  Drop(false);
}

std::string VkDlssContext::DetailsLine() const {
  if (core_shut_down_) return "Vulkan: the game shut NVIDIA's NGX down on this device; NR stays off here until the game restarts";
  if (!WantsNr() && NrState() == nr::SessionState::OFF) return {};
  const nr::SessionStatus status = (session_ ? session_->Status() : nr::SessionStatus{});
  const uint64_t bytes = status.runtime_bytes.value_or(0u) + (pipeline_ ? pipeline_->HeldBytes() : 0u);
  if (NativePresent()) {
    // The route the card names, with what the Present path's own counters say (as the bridge's line does).
    std::string line = std::format("Vulkan: NR runs natively on the game's Vulkan device ({:.0f} MiB; {})", static_cast<double>(bytes) / BYTES_PER_MIB, VK_FIRST_USE_NOTE);
    if (busy_skips_ > 0u) {
      line += std::format(", {} frame(s) without NR while Uplift's command buffers were busy", busy_skips_);
    }
    if (second_queue_) {
      line += ", presents from a second queue";
    }
    return line;
  }
  return std::format("Vulkan: NR runs inside the game's frame (native Vulkan NR, {:.0f} MiB; {})", static_cast<double>(bytes) / BYTES_PER_MIB, VK_FIRST_USE_NOTE);
}

vk::NrImage VkDlssContext::PresentMotion() {
  // None once the copies stopped. The frame no longer advances then, so the last slot would be bound again at every present.
  if (!CanCopy() || !pipeline_) return {};
  uint64_t age = 0u;
  const vk::NrImage copy = pipeline_->PresentMotion(completion_->Timeline().CurrentFrame(), &age);
  if (facts_.present_motion_copy) {
    PresentMotionCounters& counters = present_motion_counters_;
    ++counters.presents;
    if (copy.image == VK_NULL_HANDLE) {
      ++counters.missed;
      present_motion_bound_ = BoundMotion::NO_COPY;
    } else if (age == 0u) {
      ++counters.own;
      present_motion_bound_ = BoundMotion::OWN_COPY;
    } else {
      ++counters.older;
      present_motion_bound_ = BoundMotion::OLDER_COPY;
      present_motion_age_ = age;
    }
  }
  return copy;
}

void VkDlssContext::NotePresentMotionSecond(std::chrono::steady_clock::time_point now) {
  PresentMotionCounters& counters = present_motion_counters_;
  if (!counters.since) {
    counters.since = now;
    return;
  }
  if (now - *counters.since < std::chrono::seconds(1)) return;
  if (counters.presents > 0u || counters.evaluates > 0u) {
    nr::Logf(nr::LogLevel::TRACE,
             "Vulkan: Present motion: {} evaluates for {} presents; {} copies, skipped {} (input) {} (ring slot) {} (slot's last use); presents bound {} own, "
             "{} older, {} none; GPU lag at the evaluates {}..{} frames; latest jitter correction {} x {} px",
             counters.evaluates, counters.presents, counters.copies, counters.skipped_input, counters.skipped_ring, counters.skipped_slot, counters.own,
             counters.older, counters.missed, (counters.lag_min == UINT64_MAX ? 0u : counters.lag_min), counters.lag_max, counters.offset_x, counters.offset_y);
  }
  counters = {.since = now};
}

ui::MotionGap VkDlssContext::PresentMotionGap() const {
  if (!CanCopy() || !pipeline_) return ui::MotionGap::NONE;  // Setup greys DLSS's vectors with the fixed reason then
  // The latest evaluate's "no readable vectors" holds while the copies are not asked for, until an evaluate with readable ones.
  if (present_motion_gap_ != ui::MotionGap::NONE) return present_motion_gap_;
  if (!facts_.present_motion_copy || !dlss_seen_ || pipeline_->HasPresentMotion(completion_->Timeline().CurrentFrame())) return ui::MotionGap::NONE;
  // No evaluate for a while is the game's DLSS pausing (a menu, a loading screen), not a busy slot or constants ring.
  return (dlss_paused_ ? ui::MotionGap::DLSS_PAUSED : ui::MotionGap::NO_COPY);
}

ContextStatus VkDlssContext::Status() const {
  ContextStatus status;
  if (session_) {
    status.session = session_->Status();
  }
  const bool dlss_placement = (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  const bool before_upscaling = (placement_.placement == Placement::BEFORE_UPSCALING);
  std::string placement_note = placement_.reason;
  // Split chain: a later Present group runs on while the game's DLSS idles, so NR is not waiting then.
  if (dlss_placement && dlss_idle_ && !PresentGroupNative()) {
    placement_note = (status.session.state == nr::SessionState::OFF ? "Waiting for DLSS: NR memory released" : "Waiting for DLSS: releasing NR memory");
  }
  // While this device's own Present path still holds NR, the native placement is taking it over (one grace period):
  // the card and the line say so, not "another device".
  const bool waiting_for_claim = (!facts_.nr_allowed && settings_.session.enabled);
  const bool claimed_elsewhere = (waiting_for_claim && !present_holds_nr_);
  if (waiting_for_claim && present_holds_nr_ && dlss_placement) {
    placement_note = "Switching from Present: NR starts here once the Present path has released it";
  }
  // At native Present the frame's own problem is the output's, unless the context stopped (a loss or the game's NGX shutdown say so instead).
  const bool native_present = NativePresent();
  const std::string_view output_problem =
      ((native_present && !device_lost_ && !core_shut_down_) ? std::string_view(present_problem_) : std::string_view(output_message_));
  status.message = ContextMessage({
      .blocked = (blocked_reason_.empty() && claimed_elsewhere ? std::string_view("NR runs on another device in this game") : std::string_view(blocked_reason_)),
      .output = output_problem,
      .placement = placement_note,
      .skip_reason = status_.skip_reason,
      .skip_from_session = status_.skip_from_session,
      .session_message = status.session.message,
  });
  status.nr_applied = status_.nr_applied;
  status.passes_run = status_.passes_run;
  if (chain_.Split()) {
    // Split chain (as Direct3D 12's): the card and Details count every group's passes, a group whose latest recording skipped none, and a group's latest
    // recording at most its own passes (it may predate a chain change).
    status.passes_run = 0u;
    for (uint32_t index = 0u; index < chain_.group_count; ++index) {
      const StageGroup& group = chain_.groups[index];
      const StageState& stage = stages_[static_cast<size_t>(group.stage)];
      status.passes_run += (stage.nr_applied ? std::min(stage.passes_run, group.count) : 0u);
    }
  }
  status.frame = (before_upscaling ? frame_size_ : output_size_);  // Before upscaling: the render size NR works on, not DLSS's output
  if (native_present) {
    // As Direct3D 12's Present path reports itself.
    status.frame = present_frame_.back_buffer.size;
    status.trigger = trigger_used_;
    status.encoding = present_encoding_;
    status.diffuse_white_nits = present_diffuse_white_nits_;
  }
  status.intermediate_bytes = (pipeline_ ? pipeline_->HeldBytes() : 0u);
  status.device_lost = device_lost_;
  status.placement = placement_.placement;
  status.placement_line = FormatPlacementLine(placement_, main_snapshot_, true);
  if (before_upscaling && main_feature_ == NVSDK_NGX_Feature_RayReconstruction) {
    status.placement_line += " (Ray Reconstruction's input)";  // NR runs before RR denoises and upscales
  }
  status.dlss_seen = dlss_seen_;
  status.dlss_latch_tripped = dlss_latch_tripped_;
  status.work = work_size_;  // both placements settle a work size (SettleLayout)
  status.canvas = canvas_size_;
  status.blocked = blocked_reason_;
  status.claimed_elsewhere = claimed_elsewhere;
  status.output_problem = output_problem;
  status.placement_note = placement_note;
  status.skip_reason = status_.skip_reason;
  status.skip_from_session = status_.skip_from_session;
  // The facts Setup and the working card read. The Resolution mode applies at both placements (DeviceContext's rule).
  status.game_passes_motion = !last_motion_vectors_missing_;
  if (dlss_seen_ && last_motion_vectors_missing_) {
    status.dlss_motion_gap = ui::MotionGap::GAME_PASSED_NONE;
  }
  // While the game's DLSS is off, Match game runs at Full too.
  const bool match_game_waiting = (settings_.resolution == ui::ResolutionMode::MATCH_GAME && (!main_snapshot_ || dlss_off_.Off()) && !before_upscaling);
  status.resolution_applied = (match_game_waiting ? ui::ResolutionMode::FULL : settings_.resolution);
  status.upsampling = settings_.upsampling;
  if (!status.canvas.Empty() && (dlss_placement || native_present)) {
    // As Direct3D 12's line: below the floor the render is padded into the canvas; below the output size the change returns through the upsampling.
    std::string detail;
    if (status.canvas != status.work) {
      detail = std::format(" (a {}x{} render, padded)", status.work.width, status.work.height);
    } else if (status.work != status.frame && !status.frame.Empty()) {
      detail = std::format(" of {}x{} ({})", status.frame.width, status.frame.height, (settings_.upsampling == color::Upsampling::CLASSIC ? "Classic" : "Edge-aware"));
    }
    if (match_game_waiting) {
      detail += " (Match game: waiting for DLSS)";
    }
    status.work_line = std::format("Working at {}x{}{}", status.canvas.width, status.canvas.height, detail);
  }
  if (status_.nr_applied && pipeline_) {
    status.exposure_line = sources::ExposureLine(pipeline_->LastExposure());
  }
  // Split chain: the chain line and note, and each group's latest recording.
  std::array<nr::Size, STAGE_COUNT> canvases = {};
  for (size_t stage = 0u; stage < STAGE_COUNT; ++stage) {
    canvases[stage] = stages_[stage].canvas;
  }
  if (chain_.Split()) {
    status.chain_line = std::format("Chain: {}", FormatChainLine(chain_, canvases));
  }
  status.chain_note = chain_.note;
  status.group_count = chain_.group_count;
  for (uint32_t index = 0u; index < chain_.group_count; ++index) {
    const StageGroup& group = chain_.groups[index];
    const StageState& stage = stages_[static_cast<size_t>(group.stage)];
    status.groups[index] = {.stage = group.stage, .first = group.first, .count = group.count, .nr_applied = stage.nr_applied,
                            .passes_run = stage.passes_run, .skip_reason = stage.skip_reason, .motion_source = stage.motion_source,
                            .work = stage.work, .canvas = stage.canvas};
  }
  status.motion_source = status_.motion_source;
  if (native_present) {
    // The Present path's lines, as Direct3D 12's bridged Vulkan context gives them.
    switch (status_.motion_source) {
      case sources::MotionSource::PRESENT_COPY: status.motion_line = "Motion vectors: DLSS (copied for Present)"; break;
      case sources::MotionSource::LAUNCHPAD:
        status.motion_line = std::format("Motion vectors: {} (UPLIFT_MV, scale {} x {})", UpliftMvName(facts_), settings_.motion_scale_x, settings_.motion_scale_y);
        break;
      case sources::MotionSource::DIRECT:
      case sources::MotionSource::NONE: {
        const std::string_view reason = ((!settings_.motion_vectors && !settings_.launchpad_motion) ? "set to Off"
                                         : facts_.present_motion_copy                           ? "no copy of DLSS's vectors this frame"
                                         : settings_.launchpad_motion                              ? NoUpliftMvReason(facts_)
                                                                                                 : "DLSS's vectors do not reach the Present path on this device");
        status.motion_line = std::format("Motion vectors: none ({})", reason);
        break;
      }
    }
  } else {
    switch (status_.motion_source) {
      case sources::MotionSource::DIRECT:
        status.motion_line = std::format("Motion vectors: DLSS (the game's own, scale {} x {})", status_.motion_scale_x, status_.motion_scale_y);
        break;
      case sources::MotionSource::NONE:
      case sources::MotionSource::PRESENT_COPY:
      case sources::MotionSource::LAUNCHPAD:    {
        std::string_view reason = "the game passed no motion vectors";
        if (!settings_.motion_vectors) {
          reason = "set to Off";
        } else if (!dlss_seen_) {
          reason = "no DLSS running yet";
        } else if (!last_motion_vectors_missing_) {
          reason = "NR has not run yet";
        }
        status.motion_line = std::format("Motion vectors: none ({})", reason);
        break;
      }
    }
  }
  if (native_present && settings_.launchpad_motion && status_.motion_source == sources::MotionSource::NONE) {
    status.launchpad_gap = ui::MotionGap::NO_UPLIFT_MV;  // as Direct3D 12's Present path
  }
  if (dlss_placement) {
    status.ui_correction_note = "Not needed here: the HUD is drawn after NR";
  } else if (native_present) {
    status.ui_correction_note = "Unavailable here: the HUD is already in this image; use the NR mask";  // as Direct3D 12's Present path
  }
  status.keep_faces_supported = (!pipeline_ || pipeline_->KeepFacesSupported());
  return status;
}

}  // namespace uplift::addon
