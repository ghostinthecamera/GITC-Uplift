#include "addon/vk_dlss_context.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "nr/log.hpp"
#include "nr/vk_handles.hpp"
#include "nr/vk_host.hpp"
#include "sources/after_dlss_source.hpp"
#include "vk/loader.hpp"

namespace uplift::addon {
namespace {

constexpr uint32_t TRACE_FRAMES = 8u;  // spec §13: per-frame traces for 8 frames after a state change
// v2 design §3.1 (plan amendment 11): DLSS counts as idle only after this long without a main evaluate.
constexpr auto DLSS_IDLE_MINIMUM = std::chrono::milliseconds(250);
// v2 design §3.2: a device lost this soon after Uplift recorded in a game command buffer latches the DLSS placements off.
constexpr auto DLSS_LATCH_WINDOW = std::chrono::seconds(10);
// Key decision h (design §1.2): the per-device reservation NGX keeps on a Vulkan device after native NR's first use.
constexpr uint64_t VK_FIRST_USE_BYTES = 640ull << 20u;
constexpr double MIB = 1024.0 * 1024.0;

}  // namespace

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
  // Batch 2 review (minor 2): NR's own images are RGBA16F and RG16F storage images, and DLSS's output is written in place as one of the variants'
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
  context->completion_ = vk::NrCompletion::Create(context->functions_, context->device_, std::move(setup.signal), error);
  if (!context->completion_) return nullptr;
  setup.snippet.initialize_core_for_device = false;  // batch 1 review, minor 4: the game's own core is never initialised a second time
  context->host_ = (setup.host != nullptr ? std::move(setup.host)
                                          : std::make_unique<nr::VkHost>(std::move(setup.snippet), setup.binding, std::move(setup.adapter),
                                                                         context->functions_));
  context->session_ = std::make_unique<nr::Session>(*context->host_, context->completion_->Timeline(),
                                                    nr::SessionConfig{.budget = {.first_use_bytes = VK_FIRST_USE_BYTES}});
  context->pipeline_ = std::make_unique<sources::VkNrPipeline>(context->functions_, context->device_, memory, *context->session_,
                                                               context->completion_->Timeline());
  if (!context->pipeline_->Initialize(error)) {
    context->Teardown();  // nothing ran on the GPU yet: the objects that exist go now
    return nullptr;
  }
  // Plan 14: the look stage's pyramid peaks (R16F) and scene-cut state (RGBA32F) are storage images too; without them the look is skipped, as on a
  // Direct3D 12 device without typed UAV loads.
  context->pipeline_->SetLookStorage(storage(VK_FORMAT_R16_SFLOAT) && storage(VK_FORMAT_R32G32B32A32_SFLOAT));
  return context;
}

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

bool VkDlssContext::WantsNr() const {
  return (!dropped_ || core_shut_down_) && !torn_down_ && (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
}

void VkDlssContext::ResetFrameStatus() {
  nr_applied_ = false;
  passes_run_ = 0u;
  skip_reason_ = {};
  skip_from_session_ = false;
  motion_source_ = sources::MotionSource::NONE;
  motion_scale_x_ = 1.f;
  motion_scale_y_ = 1.f;
}

void VkDlssContext::BeginFrame(void* present_queue, const FrameConfig& config, bool nr_allowed, bool present_holds_nr,
                               std::chrono::steady_clock::time_point now) {
  // Checked every frame, even OFF: a lost device is never signalled or loaded into again.
  if (!torn_down_ && !dropped_ && completion_->PollDeviceLost()) {
    NoteDeviceLost(now);
  }
  if (torn_down_ || dropped_ || core_shut_down_) {
    ResetFrameStatus();
    return;
  }
  config_ = config;
  config_.nr_allowed = nr_allowed;
  present_holds_nr_ = present_holds_nr;
  controls_ = config.controls;
  nr::Timeline& timeline = completion_->Timeline();
  timeline.BeginFrame(nr::AsQueue(present_queue));  // signals the previous frame's value on the effect runtime's queue
  timeline.StampAged(now);                          // on Vulkan only the stuck-token warning: nothing is stamped
  completion_->DropStale(now, timeline.CurrentFrame());
  pipeline_->FreeFinished();  // final review C-1: the retired copy slots and mask copies whose marks the GPU has passed

  // The user's decision 1: Auto stays at Present on Vulkan. Source = DLSS is the explicit choice, which takes the DLSS placement once the game's DLSS
  // is seen (Present, the bridge's, until then) and never while a reason makes the DLSS placements unavailable.
  placement_ = (config.source == ui::PlacementSource::DLSS ? ChoosePlacement(ui::PlacementSource::AUTO, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling())
                                                           : PlacementChoice{.placement = Placement::PRESENT});
  NotePlacementChange();
  if (placement_.placement != Placement::AFTER_DLSS && placement_.placement != Placement::BEFORE_UPSCALING) {
    ResetFrameStatus();  // the DLSS placements keep their last evaluate's result
  }

  const ui::SessionOptions& options = config.session;
  session_->SetGrace(options.grace);
  session_->SetAutoResume(options.auto_resume);
  session_->SetMarginOverride(options.margin_override_bytes);
  session_->SetCreateOptions(options.preset, options.performance);
  session_->SetPassCount(options.pass_count);
  session_->SetAutoRetry(options.auto_retry);
  session_->SetPassViewLimit(config.pass_view_limit);
  if (config.settings_generation != settings_generation_) {
    settings_generation_ = config.settings_generation;
    session_->ResetRetries();  // v2 design §3.18: a settings change restarts the backoff
  }
  // Plan 14: the look stage, the colour fixes and passes 2..10 reach the pipeline at each recording (ApplyLookConfig, with its frame time); the mask copy goes
  // as soon as Mask is off.
  if (!config.look.mask) {
    pipeline_->ReleaseMaskCopy();
  }
  // Plan 14 (design §2.3): the Present path's copies of DLSS's motion vectors exist while the add-on asks for them. Their pipeline is built here, in the
  // present event, so its first compile stays out of the game's evaluate; they are released (at their marks) as soon as the add-on stops asking.
  if (config.present_motion_copy) {
    pipeline_->PreparePresentMotion();
  } else {
    pipeline_->ReleasePresentMotion();
    present_motion_logged_ = false;
  }
  // Batch 2 carry: the Session hears the frame size every frame, in every state (its FAILED retry and the resume size depend on it).
  if (const nr::Size frame_size = SessionFrameSize(); !frame_size.Empty()) {
    session_->NoteFrameSize(frame_size);
  }
  UpdateSession(now);
}

nr::Size VkDlssContext::SessionFrameSize() const {
  const bool dlss_placement = (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  return ((dlss_placement && !canvas_size_.Empty()) ? canvas_size_ : output_size_);
}

sources::WorkLayout VkDlssContext::SettleLayout(nr::Size output, bool before_upscaling, std::chrono::steady_clock::time_point now) {
  // As DeviceContext::SettleLayout (v2 design §3.8): the Resolution mode's share of `output`, settled; Match game takes DLSS's create-time render size (Before
  // upscaling already works at the render size; until DLSS's create is seen, Full).
  double target_width = output.width;
  double target_height = output.height;
  double scale = 1.0;
  switch (config_.resolution) {
    case ui::ResolutionMode::FULL:        break;
    case ui::ResolutionMode::QUALITY:     scale = sources::QUALITY_SCALE; break;
    case ui::ResolutionMode::BALANCED:    scale = sources::BALANCED_SCALE; break;
    case ui::ResolutionMode::PERFORMANCE: scale = sources::PERFORMANCE_SCALE; break;
    case ui::ResolutionMode::CUSTOM:      scale = config_.resolution_scale / 100.0; break;
    case ui::ResolutionMode::MATCH_GAME:
      if (main_snapshot_ && !main_snapshot_->render.Empty() && !before_upscaling) {
        target_width = main_snapshot_->render.width;
        target_height = main_snapshot_->render.height;
      }
      break;
  }
  const nr::Size image =
      settle_.Update(sources::WorkSize(target_width * scale, target_height * scale, output), now, config_.resolution_slider_held);
  const nr::Size canvas = ((before_upscaling && !nr::MeetsNrFloor(image)) ? sources::CanvasSize(image) : image);
  work_size_ = image;
  canvas_size_ = canvas;
  session_->NoteFrameSize(canvas);
  return {.image = image, .canvas = canvas, .upsampling = config_.upsampling};
}

void VkDlssContext::UpdateSession(std::chrono::steady_clock::time_point now) {
  const bool dlss_placement = (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  dlss_idle_ = (dlss_placement && last_main_evaluate_.has_value()
                && now - *last_main_evaluate_ > std::max(2 * config_.session.grace, DLSS_IDLE_MINIMUM));
  dlss_paused_ = (last_main_evaluate_.has_value() && now - *last_main_evaluate_ > DLSS_IDLE_MINIMUM);
  // A problem with the surface the placement works on (DLSS's output for After DLSS, the game's Color for Before upscaling: its format, its pipelines) keeps
  // the Session off, so a game that can never run NR pays nothing for it.
  const bool before_upscaling = (placement_.placement == Placement::BEFORE_UPSCALING);
  bool placement_ready = ((placement_.placement == Placement::AFTER_DLSS || before_upscaling) && !dlss_idle_ && output_message_.empty());
  const bool allowed = (config_.session.enabled && config_.nr_allowed && blocked_reason_.empty());
  // Batch 2 review (minor 3): the pipelines the placement needs are built here, in the present, when NR is about to load: the first compile (a few ms, once
  // per output format, or once for Before upscaling) stays out of the game's evaluate, and a placement whose pipelines cannot be built never loads NR.
  if (placement_ready && allowed && before_upscaling && !pipeline_->PreparePreSr()) {
    pre_sr_unbuildable_ = true;
    SetOutputProblem("The Vulkan colour pipelines for Before upscaling could not be built");
    placement_ready = false;
  } else if (placement_ready && allowed && !before_upscaling && output_format_ != VK_FORMAT_UNDEFINED && !pipeline_->Prepare(output_format_)) {
    unbuildable_formats_.push_back(output_format_);
    SetOutputProblem(std::format("The Vulkan colour pipeline for DLSS's output format (VkFormat {}) could not be built", static_cast<int>(output_format_)));
    placement_ready = false;
  }
  const bool want = (allowed && placement_ready);
  // As DeviceContext (fix round 2, Important 1): skip SetEnabled(false) only when the DLSS idle is the sole reason and the Session is suspended.
  const bool idle_only = (allowed && dlss_placement && dlss_idle_);
  if (!(idle_only && session_->Status().suspended)) {
    session_->SetEnabled(want);
  }
  const auto note_state = [this] {
    const nr::SessionState state = session_->State();
    if (state == logged_state_) return;
    nr::Logf(nr::LogLevel::INFO, "session {} -> {} (Vulkan)", nr::SessionStateName(logged_state_), nr::SessionStateName(state));
    logged_state_ = state;
    logged_after_dlss_ = false;  // each activation logs its trigger again
    trace_frames_ = TRACE_FRAMES;
  };
  note_state();
  session_->Tick();
  note_state();
  // Batch 2 review (minor 7), the VRAM release after a disable: A, B and the zero motion go with NR.
  if (session_->State() == nr::SessionState::OFF || session_->State() == nr::SessionState::FAILED) {
    pipeline_->ReleaseIntermediates();
  }
}

void VkDlssContext::NotePlacementChange() {
  if (logged_placement_ && *logged_placement_ == placement_.placement) return;
  logged_placement_ = placement_.placement;
  nr::Log(nr::LogLevel::INFO, FormatPlacementLine(placement_, main_snapshot_, true));
  settle_.Reset();  // a new placement's work size applies at once
}

void VkDlssContext::SetOutputProblem(std::string problem) {
  if (problem == output_message_) return;
  if (!problem.empty()) {
    if (placement_.placement == Placement::PRESENT) {
      // Plan 14 (batch 2 review, minor 6): at Present the evaluate only feeds the copies of DLSS's vectors; the problem matters to the DLSS placements alone.
      nr::Logf(nr::LogLevel::INFO, "{} (NR runs on the presented frame here: this applies to the DLSS placements only)", problem);
    } else {
      nr::Log(nr::LogLevel::WARN, problem);
    }
  }
  output_message_ = std::move(problem);
}

sources::PipelineResult VkDlssContext::OnDlssEvaluate(VkCommandBuffer buffer, const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies,
                                                      VkListState& list, std::chrono::steady_clock::time_point now) {
  // After a device loss Uplift never records in a game command buffer or calls the snippet again.
  if (!torn_down_ && !dropped_ && completion_->PollDeviceLost()) {
    NoteDeviceLost(now);
  }
  if (torn_down_ || dropped_ || core_shut_down_) {
    ResetFrameStatus();
    return {.reason = (core_shut_down_ ? "NGX shut down" : "device lost")};
  }
  dlss_seen_ = true;
  last_main_evaluate_ = now;
  main_snapshot_ = frame.snapshot;
  main_feature_ = frame.feature;
  last_motion_vectors_missing_ = (frame.motion_vectors == nullptr);
  // No present-starvation catch-up on Vulkan (D3D12's ticks the Session here): the frame semaphore can only be signalled in the present, where
  // ReShade holds the queue's lock. A newly seen DLSS moves the placement at the next present.
  const nr::Rect region = ngx_hooks::ResolveVkOutputRegion(frame, copies);
  output_size_ = {region.width, region.height};
  const bool image = (frame.output != nullptr && copies.output.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW);
  output_format_ = (image ? copies.output.Resource.ImageViewInfo.Format : VK_FORMAT_UNDEFINED);
  // N10: the Session hears the size NR works at in every state. After DLSS (Plan 14): the settled work canvas of the output region at the Resolution mode.
  sources::WorkLayout layout;
  if (placement_.placement == Placement::AFTER_DLSS && !dlss_idle_ && !output_size_.Empty()) {
    layout = SettleLayout(output_size_, false, now);
  } else {
    session_->NoteFrameSize(SessionFrameSize());
  }
  // What keeps NR off for this output (Before upscaling: for the game's Color), before anything is recorded (and before NR ever loads for it).
  const std::optional<uint32_t> variant = color::VkColorPipeline::OutputVariantOf(output_format_);
  std::string_view skip;
  std::string problem;
  if (BeforeUpscaling()) {
    // The padded canvas takes any render size: only the Color's own format can keep NR off.
    const bool color_image = (frame.color != nullptr && copies.color.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW);
    const VkFormat color_format = (color_image ? copies.color.Resource.ImageViewInfo.Format : VK_FORMAT_UNDEFINED);
    if (!color_image) {
      skip = "invalid input";
      problem = "DLSS's colour input is not an image view";
    } else if (!sources::VkNrPipeline::PreSrColorFormatSupported(color_format)) {
      skip = "unsupported format";
      problem = std::format("Unsupported DLSS colour format on Vulkan (VkFormat {})", static_cast<int>(color_format));
    } else if (pre_sr_unbuildable_) {
      skip = "unsupported format";
      problem = "The Vulkan colour pipelines for Before upscaling could not be built";
    }
  } else if (!image) {
    skip = "invalid input";
    problem = "DLSS's output is not an image view";
  } else if (!nr::MeetsNrFloor(output_size_)) {
    skip = "frame too small";
    problem = std::format("NR needs a frame of at least {}x{} (DLSS's output region is {}x{})", nr::MIN_NR_LONG_SIDE, nr::MIN_NR_SHORT_SIDE,
                          output_size_.width, output_size_.height);
  } else if (!variant) {
    skip = "unsupported format";
    problem = std::format("Unsupported DLSS output format on Vulkan (VkFormat {})", static_cast<int>(output_format_));
  } else if (!storage_formats_[*variant]) {
    skip = "unsupported format";
    problem = std::format("DLSS's output format (VkFormat {}) is not a storage format on this GPU's Vulkan driver", static_cast<int>(output_format_));
  } else if (std::ranges::find(unbuildable_formats_, output_format_) != unbuildable_formats_.end()) {
    skip = "unsupported format";
    problem = std::format("The Vulkan colour pipeline for DLSS's output format (VkFormat {}) could not be built", static_cast<int>(output_format_));
  }
  SetOutputProblem(std::move(problem));
  // Plan 14 (design §2.3): whether this evaluate's vectors can be copied for the Present path. Final review, minor 6: noted at every evaluate the context sees,
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
  if (placement_.placement == Placement::PRESENT && config_.present_motion_copy) {
    // NR runs on the Present path (the bridge's) with DLSS's own motion vectors, copied here in the game's frame, right after DLSS read them, for the present
    // that closes it. The Session never loads: nothing of NGX's is called on this device.
    if (!readable) return {.reason = "no readable motion vectors"};
    const bool opened_here = OpenToken(list);
    const bool copied = pipeline_->RecordPresentMotion(buffer, {.view = vectors.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, motion_region,
                                                       frame.mv_scale_x * config_.motion_scale_x, frame.mv_scale_y * config_.motion_scale_y,
                                                       completion_->Timeline().CurrentFrame() + 1u);  // a throw leaves the token open
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
    return {.recorded = copied, .reason = "not placed after DLSS"};
  }
  if (placement_.placement != Placement::AFTER_DLSS || dlss_idle_) return {.reason = "not placed after DLSS"};
  if (!skip.empty()) {
    nr_applied_ = false;
    passes_run_ = 0u;
    skip_reason_ = skip;
    skip_from_session_ = false;
    return {.reason = skip};
  }
  if (session_->State() != nr::SessionState::ACTIVE) {
    nr_applied_ = false;
    skip_reason_ = {};  // NR is off: nothing to explain
    skip_from_session_ = false;
    return {.reason = "NR is off"};
  }
  // Batch 3 review, minor 5: DLSS's output changed format while NR runs (an HDR toggle). Its pipelines are built at the next present, never inside the
  // game's evaluate (a first compile takes milliseconds): this one frame keeps DLSS's own output, and NR's history starts afresh after it.
  if (!pipeline_->Prepared(output_format_)) {
    nr_applied_ = false;
    passes_run_ = 0u;
    skip_reason_ = "preparing the new format";
    skip_from_session_ = false;
    reset_owed_ = true;
    return {.reason = skip_reason_};
  }

  // Plan 3, Minor 4: pessimistic; only a frame that applies NR clears the owed reset.
  const bool reset_was_owed = std::exchange(reset_owed_, true);
  ApplyLookConfig(now);
  const ReadFrame game = ReadDlssFrame(frame, copies, &copies.output, region, (reset_merger_.Filter(frame.reset, now) || reset_was_owed));
  // Key decision c: the token is issued before recording (every mark taken while recording includes it) and stays open on the list; EndEvaluate sets
  // its event at the end of the hooked call, on every path. A token Before upscaling opened in this call is reused.
  const bool opened_here = OpenToken(list);
  const sources::PipelineResult result = pipeline_->RecordAfterDlss(buffer, game.target, game.inputs, controls_, layout);  // a throw leaves the token open
  if (opened_here && !result.recorded) {
    DropOpenToken(list);
  }
  if (result.recorded) {
    last_game_list_recording_ = now;
  }
  if (result.nr_applied) {
    reset_owed_ = false;
  }
  nr_applied_ = result.nr_applied;
  passes_run_ = result.passes_run;
  skip_reason_ = (result.nr_applied ? std::string_view() : result.reason);
  skip_from_session_ = (result.nr_applied ? false : result.from_session);
  motion_source_ = result.motion_source;
  motion_scale_x_ = result.motion_scale_x;
  motion_scale_y_ = result.motion_scale_y;
  if (result.nr_applied && !logged_after_dlss_) {
    logged_after_dlss_ = true;  // once per activation
    nr::Log(nr::LogLevel::INFO, "NR trigger: after DLSS (Vulkan)");
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "evaluate: NR {} after DLSS (Vulkan), {} pass(es) {}", (result.nr_applied ? "applied" : "skipped"), result.passes_run,
             result.reason);
  }
  return result;
}

VkDlssContext::ReadFrame VkDlssContext::ReadDlssFrame(const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies,
                                                      const NVSDK_NGX_Resource_VK* resource, nr::Rect region, bool reset_hint) const {
  const sources::GameFrame game = sources::ReadGameFrame(frame, nullptr, region, reset_hint,
                                                         {
                                                             .encoding = config_.encoding,
                                                             .diffuse_white_nits = config_.diffuse_white_nits,
                                                             .transfer_strength = config_.transfer_strength,
                                                             .color_strength = config_.color_strength,
                                                             .motion_vectors = config_.motion_vectors,
                                                             .motion_scale_x = config_.motion_scale_x,
                                                             .motion_scale_y = config_.motion_scale_y,
                                                             .depth_inverted = config_.depth_inverted,
                                                             .chained_history = config_.chained_history,
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

void VkDlssContext::ApplyLookConfig(std::chrono::steady_clock::time_point now) {
  // As DeviceContext::ApplyLookConfig: the frame time since this context's last recording (look::StabilizeRate clamps it).
  sources::LookConfig look = config_.look;
  if (last_look_recording_) {
    look.frame_seconds = std::chrono::duration<float>(now - *last_look_recording_).count();
  }
  last_look_recording_ = now;
  pipeline_->SetLookConfig(look);
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
  // After a device loss Uplift never records in a game command buffer or calls the snippet again.
  if (!torn_down_ && !dropped_ && completion_->PollDeviceLost()) {
    NoteDeviceLost(now);
  }
  if (torn_down_ || dropped_ || core_shut_down_) return nullptr;
  if (placement_.placement != Placement::BEFORE_UPSCALING || dlss_idle_) return nullptr;
  // v2 design §3.8 (Plan 14): the work image is the settled size of the render region at the Resolution mode, and below the floor it is padded into the floor
  // canvas (§3.9). The set is keyed on it while the render region moves (the decode upsamples: R88). N10: the Session hears the canvas in every state.
  frame_size_ = {frame.color_region.width, frame.color_region.height};
  const sources::WorkLayout layout = (frame_size_.Empty() ? sources::WorkLayout{} : SettleLayout(frame_size_, true, now));
  if (session_->State() != nr::SessionState::ACTIVE) {
    nr_applied_ = false;
    skip_reason_ = {};  // NR is off: nothing to explain
    skip_from_session_ = false;
    return nullptr;
  }
  // Plan 3, Minor 4: pessimistic; only a frame that applies NR clears the owed reset.
  const bool reset_was_owed = std::exchange(reset_owed_, true);
  ApplyLookConfig(now);
  const ReadFrame game = ReadDlssFrame(frame, copies, (frame.color != nullptr ? &copies.color : nullptr), frame.color_region, (reset_merger_.Filter(frame.reset, now) || reset_was_owed));
  const bool opened_here = OpenToken(list);
  const sources::PipelineResult result = pipeline_->RecordPreSr(buffer, game.target, game.inputs, controls_, layout);  // a throw leaves the token open
  if (opened_here && !result.recorded) {
    DropOpenToken(list);
  }
  if (result.recorded) {
    last_game_list_recording_ = now;  // the latch window covers pre-SR's recordings too
  }
  if (result.nr_applied) {
    reset_owed_ = false;
  }
  nr_applied_ = result.nr_applied;
  passes_run_ = result.passes_run;
  skip_reason_ = (result.nr_applied ? std::string_view() : result.reason);
  skip_from_session_ = (result.nr_applied ? false : result.from_session);
  motion_source_ = result.motion_source;
  motion_scale_x_ = result.motion_scale_x;
  motion_scale_y_ = result.motion_scale_y;
  if (result.nr_applied && !logged_after_dlss_) {
    logged_after_dlss_ = true;  // once per activation
    nr::Log(nr::LogLevel::INFO, "NR trigger: before upscaling (Vulkan)");
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "evaluate: NR {} before upscaling (Vulkan), {} pass(es) {}", (result.nr_applied ? "applied" : "skipped"), result.passes_run,
             result.reason);
  }
  return (result.nr_applied ? pipeline_->PrivateColor() : nullptr);
}

VkImage VkDlssContext::PrepareMaskCopy(VkFormat format, nr::Size size, bool* first) {
  if (torn_down_ || dropped_ || core_shut_down_ || !pipeline_) return VK_NULL_HANDLE;
  const bool dlss_placement = (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  const bool wanted = (config_.look.mask && dlss_placement && session_->State() == nr::SessionState::ACTIVE);
  return pipeline_->PrepareMaskCopy((wanted ? format : VK_FORMAT_UNDEFINED), size, first);
}

void VkDlssContext::NoteMaskCopied() {
  if (torn_down_ || dropped_ || !pipeline_) return;
  pipeline_->NoteMaskCopied();
}

void VkDlssContext::ReleaseMask() {
  // Final review C-1: after the game's NGX shutdown the mask copy waits for Teardown (ReShade's queue may still copy into it, and every mark reads complete
  // once the shutdown declared the timeline idle).
  if (torn_down_ || dropped_ || core_shut_down_ || !pipeline_) return;
  pipeline_->ReleaseMaskCopy();
}

void VkDlssContext::OnColorSwapRejected() {
  // As DeviceContext's: BeforeDlssEvaluate applied NR into the private colour and set nr_applied_ on the assumption the detour would swap it into the game's
  // block. It calls this only when it could not (the block held no Color), so DLSS read its own Color unchanged this evaluate.
  if (placement_.placement != Placement::BEFORE_UPSCALING || !nr_applied_) return;
  nr_applied_ = false;
  skip_reason_ = "no DLSS colour to swap";
  skip_from_session_ = false;
}

void VkDlssContext::EndEvaluate(VkCommandBuffer buffer, VkListState& list) {
  const uint64_t token = std::exchange(list.open_token, 0u);
  if (token == 0u || torn_down_ || dropped_) return;
  // ALL_COMMANDS: the event's first scope is everything earlier on the queue, so it covers DLSS, NR and Uplift's passes on whichever queue the buffer runs.
  if (const VkEvent event = completion_->EventFor(token); event != VK_NULL_HANDLE) {
    functions_.vkCmdSetEvent(buffer, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  }
}

void VkDlssContext::Executed(std::span<const uint64_t> tokens) {
  if (torn_down_ || dropped_) return;
  completion_->Executed(tokens);
}

void VkDlssContext::Reexecuted(std::span<const uint64_t> tokens, uint64_t first_token, void* queue) {
  if (torn_down_ || dropped_) return;
  completion_->Reexecuted(tokens, first_token, queue);
}

void VkDlssContext::Recycle(std::span<const uint64_t> tokens) {
  if (torn_down_ || dropped_) return;
  completion_->Recycle(tokens);
}

void VkDlssContext::NoteDeviceLost(std::chrono::steady_clock::time_point now) {
  if (device_lost_) return;
  device_lost_ = true;
  dlss_latch_tripped_ = (last_game_list_recording_.has_value() && now - *last_game_list_recording_ <= DLSS_LATCH_WINDOW);
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
  // Batch 2 review (minor 7) and R83: leaked on purpose. Their destructors would call NGX or Vulkan; the device's own end takes their memory.
  static_cast<void>(pipeline_.release());
  static_cast<void>(session_.release());
  static_cast<void>(host_.release());
  if (completion_) {
    completion_->Abandon();  // every query says complete; no event or semaphore destroyed
  }
}

void VkDlssContext::OnCoreShutdown(std::chrono::milliseconds cap) {
  if (torn_down_ || dropped_ || core_shut_down_) return;
  core_shut_down_ = true;
  ResetFrameStatus();
  output_message_ = "The game shut NVIDIA's NGX down on this device: NR stays off here until the game restarts";
  if (completion_->PollDeviceLost()) {
    NoteDeviceLost(std::chrono::steady_clock::now());
    return;
  }
  // Final review C-1: the Present path's copy slots and the mask copy are also used on ReShade's queue (the bridge's copy-in reads a slot, the add-on copies
  // into the mask), after the game's last command buffer. No completion token covers that work, and the frame signal that would only comes with the next
  // present, which runs no frame of Uplift's any more (BeginFrame returns early from now on). So nothing of theirs is freed here: they wait for Teardown,
  // after the game's own idle for vkDestroyDevice (VkNrPipeline keeps them out of the timeline's pending releases, which the Session's flush below runs).
  const auto note_kept = [this] {
    if (const uint64_t kept = pipeline_->HeldBytes(); kept > 0u) {
      nr::Logf(nr::LogLevel::INFO, "Vulkan: Uplift's copies that ReShade's queue may still use ({:.1f} MiB) stay until the game's device goes",
               static_cast<double>(kept) / MIB);
    }
  };
  if (session_->State() == nr::SessionState::OFF) {
    nr::Log(nr::LogLevel::INFO, "Vulkan: the game shut NGX down on its device; NR was off there");
    note_kept();
    return;  // nothing of NGX's is held and nothing is freed: no wait
  }
  // Every piece of NR's own work is in a game command buffer whose recording ends with its token's event, so the events say when the GPU is done with it.
  const bool finished = completion_->WaitSubmitted(cap);
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
  // Not a GPU idle (final review C-1): it declares Uplift's timeline idle, on the evidence of the events, so every mark reads complete from now on and the
  // Session's flush below neither signals a queue nor waits. What that flush and the release below free was used in the game's command buffers alone.
  completion_->DeviceIdle();
  session_->OnDeviceDestroyed();  // the features' release, DestroyParameters, the snippet's Shutdown1, the unload: all before the core's Shutdown1
  // Plan 13 final review, minor 2: BeginFrame returns early from now on, so UpdateSession's release at OFF never runs again; a game that goes on after
  // shutting NGX down would keep A, B and the private colour until it exits. The mask copy is not among them (C-1, above).
  pipeline_->ReleaseNrSurfaces();
  nr::Log(nr::LogLevel::INFO, "Vulkan: NR released on the game's device before the game's NGX shutdown");
  note_kept();
}

void VkDlssContext::Teardown() {
  if (torn_down_) return;
  torn_down_ = true;
  if (dropped_) return;
  if (completion_->PollDeviceLost()) {
    NoteDeviceLost(std::chrono::steady_clock::now());
    return;
  }
  completion_->DeviceIdle();  // batch 1 carry: the game idled the device, so the flush below finds every token and frame complete
  pipeline_->ReleasePresentMotion();
  pipeline_->ReleaseIntermediates();
  if (!core_shut_down_) {
    session_->OnDeviceDestroyed();  // the flush (immediate now), the features' release, Shutdown1, the unload
  } else {
    completion_->Timeline().Flush(std::chrono::seconds(2));  // Plan 14: the Session's flush did not run (the core shut down first): what is still pending goes now
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
  return std::format("Vulkan: NR runs inside the game's frame (native Vulkan NR, {:.0f} MiB; {})", static_cast<double>(bytes) / MIB, VK_FIRST_USE_NOTE);
}

vk::NrImage VkDlssContext::PresentMotion() {
  // Final review C-1: none once the copies stopped. The frame no longer advances then, so the last slot would be bound again at every present.
  return ((CanCopy() && pipeline_) ? pipeline_->PresentMotion(completion_->Timeline().CurrentFrame()) : vk::NrImage{});
}

ui::MotionGap VkDlssContext::PresentMotionGap() const {
  if (!CanCopy() || !pipeline_) return ui::MotionGap::NONE;  // final review I-1: Setup greys DLSS's vectors with the fixed reason then
  // Final review, minor 6: the latest evaluate's "no readable vectors" holds while the copies are not asked for, until an evaluate with readable ones.
  if (present_motion_gap_ != ui::MotionGap::NONE) return present_motion_gap_;
  if (!config_.present_motion_copy || !dlss_seen_ || pipeline_->HasPresentMotion(completion_->Timeline().CurrentFrame())) return ui::MotionGap::NONE;
  // Final review, minor 1: no evaluate for a while is the game's DLSS pausing (a menu, a loading screen), not a busy slot or constants ring.
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
  if (dlss_placement && dlss_idle_) {
    placement_note = (status.session.state == nr::SessionState::OFF ? "Waiting for DLSS: NR memory released" : "Waiting for DLSS: releasing NR memory");
  }
  // Batch 3 review, minor 2: while this device's own Present path still holds NR, the native placement is taking it over (one grace period, design R86):
  // the card and the line say so, not "another device".
  const bool waiting_for_claim = (!config_.nr_allowed && config_.session.enabled);
  const bool claimed_elsewhere = (waiting_for_claim && !present_holds_nr_);
  if (waiting_for_claim && present_holds_nr_ && dlss_placement) {
    placement_note = "Switching from Present: NR starts here once the Present path has released it";
  }
  status.message = ContextMessage({
      .blocked = (blocked_reason_.empty() && claimed_elsewhere ? std::string_view("NR runs on another device in this game") : std::string_view(blocked_reason_)),
      .output = output_message_,
      .placement = placement_note,
      .skip_reason = skip_reason_,
      .skip_from_session = skip_from_session_,
      .session_message = status.session.message,
  });
  status.nr_applied = nr_applied_;
  status.passes_run = passes_run_;
  status.frame = (before_upscaling ? frame_size_ : output_size_);  // Before upscaling: the render size NR works on, not DLSS's output
  status.intermediate_bytes = (pipeline_ ? pipeline_->HeldBytes() : 0u);
  status.device_lost = device_lost_;
  status.placement = placement_.placement;
  status.placement_line = FormatPlacementLine(placement_, main_snapshot_, true);
  status.dlss_seen = dlss_seen_;
  status.dlss_latch_tripped = dlss_latch_tripped_;
  status.work = work_size_;  // Plan 14: both placements settle a work size (SettleLayout)
  status.canvas = canvas_size_;
  status.blocked = blocked_reason_;
  status.claimed_elsewhere = claimed_elsewhere;
  status.output_problem = output_message_;
  status.placement_note = placement_note;
  status.skip_reason = skip_reason_;
  status.skip_from_session = skip_from_session_;
  // Plan 14 (design §1.3, §1.6): the facts Setup and the working card read. Task 8: the Resolution mode applies at both placements (DeviceContext's rule).
  status.ray_reconstruction = (main_feature_ == NVSDK_NGX_Feature_RayReconstruction);
  status.game_passes_motion = !last_motion_vectors_missing_;
  if (dlss_seen_ && last_motion_vectors_missing_) {
    status.dlss_motion_gap = ui::MotionGap::GAME_PASSED_NONE;
  }
  const bool match_game_waiting = (config_.resolution == ui::ResolutionMode::MATCH_GAME && !main_snapshot_ && !before_upscaling);
  status.resolution_applied = (match_game_waiting ? ui::ResolutionMode::FULL : config_.resolution);
  status.upsampling = config_.upsampling;
  if (!status.canvas.Empty() && dlss_placement) {
    // As Direct3D 12's line: below the floor the render is padded into the canvas; below the output size the change returns through the upsampling.
    std::string detail;
    if (status.canvas != status.work) {
      detail = std::format(" (a {}x{} render, padded)", status.work.width, status.work.height);
    } else if (status.work != status.frame && !status.frame.Empty()) {
      detail = std::format(" of {}x{} ({})", status.frame.width, status.frame.height, (config_.upsampling == color::Upsampling::CLASSIC ? "Classic" : "Edge-aware"));
    }
    if (match_game_waiting) {
      detail += " (Match game: waiting for DLSS)";
    }
    status.work_line = std::format("Working at {}x{}{}", status.canvas.width, status.canvas.height, detail);
  }
  status.motion_source = motion_source_;
  switch (motion_source_) {
    case sources::MotionSource::DIRECT:
      status.motion_line = std::format("Motion vectors: DLSS (the game's own, scale {} x {})", motion_scale_x_, motion_scale_y_);
      break;
    case sources::MotionSource::NONE:
    case sources::MotionSource::PRESENT_COPY:
    case sources::MotionSource::LAUNCHPAD:    {
      std::string_view reason = "the game passed no motion vectors";
      if (!config_.motion_vectors) {
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
  if (dlss_placement) {
    status.ui_correction_note = "Not needed here: the HUD is drawn after NR";
  }
  return status;
}

}  // namespace uplift::addon
