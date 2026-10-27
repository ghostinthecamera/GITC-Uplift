#include "addon/device_context.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "addon/dred.hpp"
#include "nr/log.hpp"

namespace uplift::addon {
namespace {

constexpr uint32_t TRACE_FRAMES = 8u;  // spec §13: per-frame traces for 8 frames after a state change
// v2 design §3.1: no present for this long while evaluates continue means the swap chain is starving.
constexpr auto PRESENT_STARVATION = std::chrono::milliseconds(250);
// v2 design §3.1 (plan amendment 11): DLSS_IDLE_MINIMUM is in addon/placement.hpp since Plan 18 Task 12 (fix round 1), which shares it.
// v2 design §3.2: a device removed this soon after Uplift recorded on a game list latches the DLSS placements off.
constexpr auto DLSS_LATCH_WINDOW = std::chrono::seconds(10);
// Important 1 (fix round 2): how long "descriptors busy" may persist, back to back, before it is
// worth one WARN -- long enough that a normal submit-only-thread stall (aged out by Timeline's own
// AGED_SUBMISSION_TIME, at most a few hundred ms) never triggers it.
constexpr auto DESCRIPTORS_BUSY_WARN_AGE = std::chrono::seconds(1);
// Split chain (fix round 1, I1): why a later group cannot run at Present while the game's DLSS-stage NR goes to another queue than the presents. With
// frame generation on, the presents come from its own queue (the owner's Onimusha check), so the card says that instead.
constexpr std::string_view OTHER_QUEUE_REASON = "the game's DLSS runs on another queue";
constexpr std::string_view FRAME_GEN_QUEUE_REASON = "with frame generation on, the presents come from another queue";

}  // namespace

static_assert(ui::MAX_PASSES - 1u == sources::LATER_PASSES);

sources::LookConfig LookConfigFrom(const ui::Settings& settings,
                                   const std::array<std::optional<nr::Controls>, sources::LATER_PASSES>& later_controls) {
  sources::LookConfig config = {
      .look = settings.look,
      .fixes = {
          .primaries = settings.source_primaries,
          .linear_unit_nits = settings.linear_unit_nits,
          .input_exposure = settings.input_exposure,
          .auto_mode = settings.auto_exposure_mode,
          .auto_blend = settings.auto_exposure_blend,
          .smooth_adapt = (settings.exposure_adapt == ui::ExposureAdapt::SMOOTH),
          .adapt_brighter = settings.adapt_brighter_stops,
          .adapt_darker = settings.adapt_darker_stops,
          .transfer = settings.neural_transfer,
          .chroma_clamp = settings.chroma_clamp,
          .near_black_guard = settings.near_black_guard,
      },
      .later_controls = later_controls,
      .mask = (settings.mask == ui::MaskMode::AUTO),
      .present_ui_correction = (settings.ui_correction == ui::UiCorrection::ON),
      .keep_faces = {.enabled = settings.keep_faces, .protection = settings.face_protection, .lighting_scale = settings.lighting_scale,
                     .show_mask = settings.show_face_mask, .tuning = settings.face_tuning},
  };
  for (size_t index = 0u; index < settings.passes.size(); ++index) {
    const ui::PassSettings& pass = settings.passes[index];
    if (!pass.follow_pass1) {
      config.later_strengths[index] = {.transfer = pass.transfer_strength, .color = pass.color_strength};
    }
  }
  return config;
}

std::unique_ptr<DeviceContext> DeviceContext::Create(ID3D12Device* device, nr::SnippetConfig snippet_config,
                                                     std::string* error, ContextOptions options) {
  std::unique_ptr<nr::Timeline> timeline = nr::Timeline::CreateD3D12(device);
  if (!timeline) {
    *error = "could not create the frame fence";
    return nullptr;
  }
  // Plan 7: the bridge's private device is one an adopted NGX core has never seen.
  snippet_config.initialize_core_for_device = options.bridged;
  std::unique_ptr<DeviceContext> context(
      new DeviceContext(device, std::move(snippet_config), std::move(timeline), std::move(options)));
  if (!context->present_source_->Initialize(error)) return nullptr;
  return context;
}

DeviceContext::DeviceContext(ID3D12Device* device, nr::SnippetConfig snippet_config,
                             std::unique_ptr<nr::Timeline> timeline, ContextOptions options)
    : device_(device),
      host_(options.host != nullptr ? std::move(options.host)
                                    : std::unique_ptr<nr::Host>(std::make_unique<nr::RealHost>(
                                        device, std::move(snippet_config), std::move(options.game_memory)))),
      timeline_(std::move(timeline)),
      session_(std::make_unique<nr::Session>(*host_, *timeline_)),
      present_source_(std::make_unique<sources::PresentSource>(device, *session_, *timeline_)),
      after_dlss_(std::make_unique<sources::AfterDlssSource>(present_source_->Pipeline(), *timeline_)),
      pre_sr_(std::make_unique<sources::PreSrSource>(present_source_->Pipeline(), *timeline_)) {
  if (options.bridged) {
    bridged_ = true;
    dlss_stages_ = options.dlss_stages;  // Plan 18: a Direct3D 11 bridge's context runs the DLSS stages through the bridge's hand-off
    if (!dlss_stages_) {
      dlss_unavailable_reason_ = std::string(BRIDGED_DLSS_REASON);
    }
  }
}

DeviceContext::~DeviceContext() {
  Teardown();
}

void DeviceContext::SetBlockedReason(std::string reason) {
  if (reason == blocked_reason_) return;
  if (!reason.empty()) {
    nr::Logf(nr::LogLevel::WARN, "NR blocked: {}", reason);
  }
  blocked_reason_ = std::move(reason);
}

void DeviceContext::SetDlssUnavailableReason(std::string reason) {
  if ((bridged_ && !dlss_stages_) || reason == dlss_unavailable_reason_) return;  // Plan 7: a bridged context keeps its own (Plan 18: but Direct3D 11's)
  if (!reason.empty()) {
    nr::Logf(nr::LogLevel::INFO, "DLSS placements unavailable: {}", reason);
  }
  dlss_unavailable_reason_ = std::move(reason);
}

TriggerPoint DeviceContext::BeginFrame(ID3D12CommandQueue* queue, const FrameConfig& config, const TargetInfo& target,
                                       bool marker_expected, std::chrono::steady_clock::time_point now) {
  frame_ready_ = false;
  // Checked every frame, even OFF: a removed device must never be signalled or loaded into, and OFF
  // is exactly the state a Session settles into right after a mid-frame removal. Minor 6 (fix round
  // 1): reset the last frame's status before bailing, so a caller never sees a stale "applied" past this.
  if (torn_down_ || device_lost_ || NoteDeviceRemoved(now)) {
    nr_applied_ = false;
    passes_run_ = 0u;
    skip_reason_ = {};
    skip_from_session_ = false;
    motion_source_ = sources::MotionSource::NONE;
    motion_scale_x_ = 1.f;
    motion_scale_y_ = 1.f;
    return TriggerPoint::NONE;
  }
  config_ = config;
  if (!config.session.enabled) {
    present_jitter_.Reset();
  }
  controls_ = config.controls;
  last_present_ = now;
  if (core_hold_.Resume(config.upscaler_creates)) {
    // Plan 15: the game created its DLSS on this device after its NGX shutdown, so it re-initialised NGX (a DLSS setting changed): NR may load again,
    // through the normal path (UpdateSession below), into the core the game initialised anew.
    nr::Log(nr::LogLevel::INFO, "Direct3D 12: the game's DLSS started again on its device (a new DLSS feature); NR may load again");
  }
  timeline_->BeginFrame(queue);
  timeline_->StampAged(now);
  present_queue_ = queue;
  frame_generation_.OnPresent(now);
  frame_generation_.Update(now, config.ngx_frame_generation);

  // Plan 18 Task 12: the game switched its DLSS off after DLSS ran here (it released its last DLSS feature, or on Direct3D 11 shut NGX down; fix round 1,
  // I-2: and that held for DLSS_IDLE_MINIMUM and two presents): the DLSS stages fall back to Present instead of waiting for it forever; a pause keeps a live
  // feature, so it still waits. Not while held after the game's NGX shutdown (Plan 15's hold keeps priority: the shutdown ends the features too).
  dlss_off_.OnPresent(dlss_seen_ && config.dlss_released && dlss_unavailable_reason_.empty() && !core_hold_.Held(), now);
  const bool dlss_off = (dlss_off_.Off() && !core_hold_.Held());
  const StageProblems problems = {.after_dlss = after_dlss_problem_, .before_upscaling = before_upscaling_problem_};
  // Minor 8 (fix round 1): logged_placement_ starts unset, so the very first placement this device
  // chooses is always logged once, NONE (with its reason) included. Plan 18: a stage the Direct3D 11 bridge cannot share is never chosen.
  placement_ = ChoosePlacement(config.source, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(), problems, dlss_off);
  if (dlss_off && !dlss_off_logged_
      && placement_.placement != ChoosePlacement(config.source, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(), problems).placement) {
    // Fix round 1 (M-6): logged once a stage really fell back (with Source = Present nothing did).
    dlss_off_logged_ = true;
    nr::Log(nr::LogLevel::INFO, DLSS_OFF_LINE);
    logged_trigger_ = TriggerPoint::NONE;  // NR's triggers are logged again: at Present now, and after DLSS when it returns
    logged_after_dlss_ = false;
  }
  NotePlacementChange();
  // The back buffer as NR at Present sees it: pass 1's output problem there, and (split chain, design 6.1) whether a later group can run there. Not read
  // when nothing could run at Present.
  std::string present_problem;
  std::optional<color::Encoding> present_encoding;
  nr::Size present_size;
  // Fix round 1 (M3): for a later group only while some pass is stored at another stage, so the default path reads nothing more.
  const bool present_possible = (placement_.placement == Placement::PRESENT || SplitStored());
  if (present_possible && !target.problem.empty()) {
    present_problem = std::string(target.problem);  // Plan 7: the D3D11 bridge cannot hand this back buffer to NR
  } else if (present_possible && (target.resource != nullptr || target.format != DXGI_FORMAT_UNKNOWN)) {
    DXGI_FORMAT format = target.format;
    present_size = target.size;
    if (target.resource != nullptr) {
      const D3D12_RESOURCE_DESC description = target.resource->GetDesc();
      present_size = {static_cast<uint32_t>(description.Width), description.Height};
      format = description.Format;
    }
    if (!color::DescribeFormat(format).has_value()) {
      present_problem = std::format("Unsupported back-buffer format (DXGI_FORMAT {})", static_cast<int>(format));
    } else {
      present_encoding = color::ResolveEncoding(config.encoding, target.color_space, format);
      if (!present_encoding) {
        present_problem = "HDR10 HLG output is not supported";
      }
    }
  }
  present_cannot_ = (!present_problem.empty() ? present_problem : (present_encoding ? std::string() : std::string("the back buffer cannot reach NR")));
  UpdateChain();
  const bool dlss_placement =
      (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  if (!dlss_placement && !ran_since_present_) {
    // The Present path reports each frame afresh; the DLSS paths keep their last evaluate's result. 1.2.4: so does a Present-path Run that came
    // before this present (effects rendered mid-frame by another add-on), or the card would read "Waiting for a frame" while NR runs.
    nr_applied_ = false;
    passes_run_ = 0u;
    skip_reason_ = {};
    skip_from_session_ = false;
    motion_source_ = sources::MotionSource::NONE;
    motion_scale_x_ = 1.f;
    motion_scale_y_ = 1.f;
  }
  ran_since_present_ = false;
  // Minor 4 (fix round 4): the Present motion copies are only useful with Source = Present and
  // MotionVectors wanting them; release them (through the Timeline, like any other intermediate) as
  // soon as either stops holding, rather than waiting for the Session to next go OFF. A no-op once
  // there is nothing left to release. Split chain: a later group at Present binds them too.
  const std::optional<uint32_t> present_group = GroupAt(Stage::PRESENT);
  if (!present_group || !config.motion_vectors) {
    present_source_->Pipeline().ReleaseMotionCopies();
    present_jitter_.Reset();
  }
  // 1.0.1 (fix round 1, minor 2): Launchpad's converted motion goes the same way once the Present path cannot bind UPLIFT_MV.
  if (!present_group || !config.launchpad_motion) {
    present_source_->Pipeline().ReleaseLaunchpadMotion();
  }

  std::string message;
  encoding_.reset();
  if (!dlss_placement) {
    frame_size_ = {};
  }
  if (placement_.placement == Placement::PRESENT) {
    message = std::move(present_problem);
    encoding_ = present_encoding;
    frame_size_ = present_size;
  }
  if (message != output_message_) {
    if (!message.empty()) {
      nr::Log(nr::LogLevel::WARN, message);
    }
    output_message_ = std::move(message);
  }

  if (present_group && present_encoding) {
    present_layout_ = SettleLayout(present_size, Stage::PRESENT, now);  // before UpdateSession, so its Tick sees the size
  }

  const ui::SessionOptions& options = config.session;
  session_->SetGrace(options.grace);
  session_->SetAutoResume(options.auto_resume);
  session_->SetMarginOverride(options.margin_override_bytes);
  session_->SetVramCheck(options.vram_check);
  session_->SetCreateOptions(options.presets, options.performance);
  session_->SetPassCount(options.pass_count);
  session_->NoteFacesWanted(config.look.keep_faces.enabled);  // Keep faces fix round 2 (4): its off edge, even on a frame NR then skips
  for (uint32_t group = 1u; group < chain_.group_count; ++group) {
    const Stage stage = chain_.groups[group].stage;  // decision Q5a: a later group's own "Keep faces here"
    session_->NoteFacesWanted(static_cast<uint8_t>(stage), config.look.keep_faces.enabled && LaterStageKeepsFaces(stage));
  }
  session_->SetAutoRetry(options.auto_retry);
  session_->SetPassViewLimit(config.pass_view_limit);
  if (config.settings_generation != settings_generation_) {
    settings_generation_ = config.settings_generation;
    session_->ResetRetries();  // v2 design §3.18: a settings change restarts the backoff
  }
  UpdateSession(now);

  if (present_group && present_encoding) {
    const float diffuse_white_nits = (config.diffuse_white_nits > 0.f ? config.diffuse_white_nits
                                                                      : color::DefaultDiffuseWhiteNits(*present_encoding));
    if (*present_group == 0u) {
      diffuse_white_nits_ = diffuse_white_nits;  // the status describes pass 1's group
    }
    frame_target_ = {
        .resource = target.resource,
        .encoding = *present_encoding,
        .diffuse_white_nits = diffuse_white_nits,
        .transfer_strength = config.transfer_strength,
        .color_strength = config.color_strength,
    };
    // A described target (the D3D11 bridge before its shared copy exists) has nothing to record on yet.
    frame_ready_ = (session_->State() == nr::SessionState::ACTIVE && target.resource != nullptr);
  }
  return trigger_.OnPresent(marker_expected, config.effects_on);
}

sources::PresentResult DeviceContext::Run(FrameHost* host, D3D12_RESOURCE_STATES entry_state, TriggerPoint point,
                                          ID3D12Resource* motion, bool motion_is_dlss, float dlss_scale_x, float dlss_scale_y,
                                          nr::Rect dlss_motion_region) {
  if (point == TriggerPoint::NONE || !frame_ready_) return {};
  frame_ready_ = false;
  ran_since_present_ = true;
  // Split chain: the Present group is pass 1's (group 0, the single chain of before) or a later one, whose result only Status's groups report.
  const uint32_t group = GroupAt(Stage::PRESENT).value_or(0u);
  StageState& stage = stages_[static_cast<size_t>(Stage::PRESENT)];
  const auto skip = [&](std::string_view reason) -> sources::PresentResult {
    stage.nr_applied = false;
    stage.passes_run = 0u;
    stage.skip_reason = reason;
    stage.motion_source = sources::MotionSource::NONE;
    if (group == 0u) {
      skip_reason_ = reason;
      skip_from_session_ = false;
    } else {
      present_reset_owed_ = true;  // the Session never saw this frame: the group's next recording restarts its history
    }
    return {.reason = reason};
  };
  // Spec §11: on the Present path NR would also run on generated frames.
  if (frame_generation_.State().active && !config_.present_with_frame_gen) return skip("frame generation is on");
  // Split chain: a later group the budget cut to no pass (or the Defaults view hides) skips before its source copy and encode.
  if (group > 0u && !session_->SliceRuns(slices_[group])) return skip(SliceSkipReason(group));
  // Default: keep distinct queues apart until UpdateChain remaps Present. The experimental opt-in uses the game's existing image handoff; it adds no wait.
  if (group > 0u && !config_.allow_cross_queue_passes && !timeline_->StageWorkDone(dlss_stage_token_, present_queue_)) {
    return skip("waiting for the earlier stages' NR");
  }
  host->FlushPending();
  ApplyLookConfig(last_present_.value_or(std::chrono::steady_clock::now()), group);
  host->TargetBarrier(entry_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  frame_target_.dlss_motion = {.resource = (motion_is_dlss && config_.motion_vectors ? motion : nullptr),
                                .rect = (motion_is_dlss ? dlss_motion_region : nr::Rect{})};
  frame_target_.launchpad_motion = {.resource = (!motion_is_dlss && config_.launchpad_motion ? motion : nullptr)};
  frame_target_.motion_scale_x = config_.motion_scale_x;
  frame_target_.motion_scale_y = config_.motion_scale_y;
  // Plan 18: Direct3D 11's ring keeps DLSS's raw vectors, so their scale travels as values; Vulkan's copies are already scaled (1).
  frame_target_.dlss_motion_scale_x = (motion_is_dlss ? dlss_scale_x : 1.f);
  frame_target_.dlss_motion_scale_y = (motion_is_dlss ? dlss_scale_y : 1.f);
  frame_target_.dlss_motion_raw = (motion_is_dlss && dlss_stages_);  // fix round 1 (M-1): the motion copy filters them, as every other Present source
  frame_target_.dlss_motion_flip = (frame_target_.dlss_motion_raw && config_.dlss_motion_upside_down);  // 1.1.6 (Vulkan's copies are flipped by theirs)
  const sources::PresentResult result = present_source_->Record(host->NativeList(), frame_target_, controls_, present_reset_owed_, present_layout_,
                                                                {.slice = slices_[group], .set = group});
  host->TargetBarrier(D3D12_RESOURCE_STATE_COPY_SOURCE, entry_state);
  if (result.nr_applied) {
    present_reset_owed_ = false;
  }
  stage.nr_applied = result.nr_applied;
  stage.passes_run = result.passes_run;
  stage.skip_reason = (result.nr_applied ? std::string_view() : result.reason);
  stage.motion_source = result.motion_source;
  if (group == 0u) {
    // The status and NR's heartbeat follow pass 1's group: one NR frame per frame.
    nr_applied_ = result.nr_applied;
    nr_recordings_ += (result.nr_applied ? 1u : 0u);
    passes_run_ = result.passes_run;
    skip_reason_ = (result.nr_applied ? std::string_view() : result.reason);
    skip_from_session_ = (result.nr_applied ? false : result.from_session);
    motion_source_ = result.motion_source;
    motion_scale_x_ = result.motion_scale_x;
    motion_scale_y_ = result.motion_scale_y;
  }
  if (result.nr_applied) {
    trigger_used_ = point;
    if (point != logged_trigger_) {
      logged_trigger_ = point;
      nr::Logf(nr::LogLevel::INFO, "NR trigger: {}", TriggerPointName(point));
    }
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "frame {}: NR {} at {}, {} pass(es) {}", timeline_->CurrentFrame(),
             (result.nr_applied ? "applied" : "skipped"), TriggerPointName(point), result.passes_run, result.reason);
  }
  return result;
}

bool DeviceContext::NoteMainEvaluate(const ngx_hooks::DlssFrame& frame, std::chrono::steady_clock::time_point now) {
  // After a device removal Uplift never records on a game list or calls the snippet again. Reset the
  // last evaluate's status before bailing (fix round 2, mirroring Minor 6's BeginFrame fix): while
  // presents starve, no BeginFrame runs to do it, so a caller could otherwise see a stale "applied"
  // from before the removal indefinitely.
  if (torn_down_ || device_lost_ || NoteDeviceRemoved(now)) {
    nr_applied_ = false;
    passes_run_ = 0u;
    skip_reason_ = {};
    skip_from_session_ = false;
    motion_source_ = sources::MotionSource::NONE;
    motion_scale_x_ = 1.f;
    motion_scale_y_ = 1.f;
    descriptors_busy_since_.reset();
    return false;
  }
  if (core_hold_.ResumeOnEvaluate(frame.serial)) {
    // Plan 15 fix round (minors 1 and 2): a successful evaluate of a feature the game created after its NGX shutdown proves the core serves this device
    // again. Before the catch-up below, so a game whose presents starve resumes here too.
    nr::Log(nr::LogLevel::INFO, "Direct3D 12: the game's DLSS started again on its device (a DLSS evaluate); NR may load again");
  }
  dlss_seen_ = true;
  last_main_evaluate_ = now;
  ++main_evaluates_;
  main_snapshot_ = frame.snapshot;
  main_feature_ = frame.feature;
  if (dlss_off_.OnEvaluate(!core_hold_.Held()) && std::exchange(dlss_off_logged_, false)) {
    // Plan 18 Task 12: the game's DLSS runs again after it switched it off: the stage it fell back from returns from the next present (or the catch-up
    // below), as on first sight. While held, only an evaluate that ended the hold (above) counts. Every evaluate starts the release window over (I-2).
    nr::Log(nr::LogLevel::INFO, DlssOnAgainLine(ChoosePlacement(config_.source, dlss_unavailable_reason_, true, BeforeUpscaling(),
                                                                {.after_dlss = after_dlss_problem_, .before_upscaling = before_upscaling_problem_})));
  }
  // Every evaluate updates this, whatever placement follows below, so the overlay's "Motion vectors"
  // line can tell "the game passed no motion vectors" from Present's other None reasons (Minor -- the
  // user's motion-vector-indicator request).
  last_motion_vectors_missing_ = (frame.motion_vectors == nullptr);
  frame_generation_.OnMainEvaluate(now);
  // v2 design §3.1: some frame-generation swap chains stop presenting while the game still evaluates;
  // the evaluate then drives the Timeline and the Session. Important 2 (fix round 1): the same
  // per-frame work a present would have done, so an idle drain during starvation still reloads NR on
  // the very evaluate that ends the idle period, instead of waiting for the next present.
  if (present_queue_ != nullptr && last_present_.has_value() && now - *last_present_ >= PRESENT_STARVATION) {
    timeline_->BeginFrame(present_queue_);
    timeline_->StampAged(now);
    // P13: a DLSS first seen while presents starve (dlss_seen_ just went true, above) must be able to
    // switch Auto from Present to After DLSS right here too, ahead of UpdateSession below -- no present
    // is coming along to notice it otherwise, so NR would never run until presents resume.
    placement_ = ChoosePlacement(config_.source, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(),
                                 {.after_dlss = after_dlss_problem_, .before_upscaling = before_upscaling_problem_}, dlss_off_.Off() && !core_hold_.Held());
    // Minor 2 (fix round 4): a placement change decided here needs the same bookkeeping BeginFrame gives
    // it, or the log stays silent and the work size keeps the old placement's until the next present.
    NotePlacementChange();
    UpdateChain();  // split chain: the chain follows pass 1's placement, with the latest present's back buffer
    // Batch 1 re-review R1: the same release BeginFrame gives a placement change, so the Present motion
    // copies do not linger past starvation switching Auto away from Present (or MotionVectors off).
    const bool present_group = GroupAt(Stage::PRESENT).has_value();
    if (!present_group || !config_.motion_vectors) {
      present_source_->Pipeline().ReleaseMotionCopies();
      present_jitter_.Reset();
    }
    if (!present_group || !config_.launchpad_motion) {
      present_source_->Pipeline().ReleaseLaunchpadMotion();
    }
    UpdateSession(now);
  }
  return true;
}

void DeviceContext::NoteDlssFeature(uint64_t serial) {
  if (serial == dlss_feature_serial_) return;
  if (dlss_feature_serial_ != 0u) {
    pre_sr_->OweReset();
    after_dlss_->OweReset();
    present_reset_owed_ = true;
    before_reset_.reset();
  }
  dlss_feature_serial_ = serial;
}

sources::PipelineResult DeviceContext::OnDlssEvaluate(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                                      std::chrono::steady_clock::time_point now) {
  if (!NoteMainEvaluate(frame, now)) {
    before_reset_.reset();
    return {.reason = "device removed"};
  }
  NoteDlssFeature(frame.serial);
  const std::optional<bool> before_reset = std::exchange(before_reset_, std::nullopt);  // this evaluate's, from BeforeDlssEvaluate
  const ngx_hooks::MotionJitterOffset jitter = present_jitter_.Update(frame);
  // Amendment 7 (Key decision 7): copy the game's motion vectors on its list for the present that closes this frame: with Source = Present, and (split
  // chain) for a later group at Present.
  const auto copy_motion = [&]() -> sources::PipelineResult {
    ID3D12GraphicsCommandList* const list = host.NativeList();
    if (!config_.motion_vectors || frame.motion_vectors == nullptr || session_->State() != nr::SessionState::ACTIVE || list == nullptr
        || !host.StateKnown() || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      return {.reason = "not placed after DLSS"};
    }
    const sources::PipelineResult copied = sources::RecordOnGameList(host, *timeline_, config_.restore, [&] {
      return sources::PipelineResult{
          .recorded = present_source_->Pipeline().RecordMotionCopy(list, {.resource = frame.motion_vectors, .rect = frame.motion_region},
                                                                   frame.mv_scale_x * config_.motion_scale_x,
                                                                   frame.mv_scale_y * config_.motion_scale_y, config_.dlss_motion_upside_down,
                                                                   jitter.x * static_cast<float>(frame.motion_region.width) * config_.motion_scale_x,
                                                                   jitter.y * static_cast<float>(frame.motion_region.height) * config_.motion_scale_y),
      };
    });
    if (copied.recorded) {
      last_game_list_recording_ = now;
    }
    return copied;
  };
  if (placement_.placement == Placement::PRESENT) {
    if (config_.source != ui::PlacementSource::PRESENT) return {.reason = "not placed after DLSS"};
    return copy_motion();
  }
  // Pass 1 runs inside the game's frame here, so a Present group can only be a later one.
  sources::PipelineResult motion_copy = {.reason = "not placed after DLSS"};
  if (const std::optional<uint32_t> present = GroupAt(Stage::PRESENT); present && session_->SliceRuns(slices_[*present])) {
    motion_copy = copy_motion();  // only for a Present group that runs (fix round 1, M4)
  }
  const std::optional<uint32_t> group = GroupAt(Stage::AFTER_DLSS);
  if (!group || dlss_idle_) {
    descriptors_busy_since_.reset();
    return motion_copy;  // Before upscaling already ran in BeforeDlssEvaluate
  }
  const nr::Rect region = sources::ResolveOutputRegion(frame);
  const sources::WorkLayout layout = SettleLayout({region.width, region.height}, Stage::AFTER_DLSS, now);  // N10: in every state
  // Important 3 (fix round 1, N11): only ACTIVE has anything to evaluate. LOADING, GRACE, DRAINING and
  // FAILED would otherwise reach AfterDlssSource::Run and report its misleading "list state unknown".
  if (session_->State() != nr::SessionState::ACTIVE) {
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].nr_applied = false;
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].skip_reason = {};
    if (*group == 0u) {
      nr_applied_ = false;
      skip_reason_ = {};  // NR is off: nothing to explain, and the list's state is not tracked
      skip_from_session_ = false;
    }
    descriptors_busy_since_.reset();
    return {.reason = "NR is off"};
  }
  if (*group > 0u && !session_->SliceRuns(slices_[*group])) {
    // Split chain: a later group the budget cut to no pass (or the Defaults view hides) skips before its source copy; its next recording starts afresh.
    after_dlss_->OweReset();
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].nr_applied = false;
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].skip_reason = SliceSkipReason(*group);
    return {.reason = SliceSkipReason(*group)};
  }
  return RecordAfterDlss(frame, host, layout, now, *group, before_reset);
}

sources::PipelineResult DeviceContext::RecordAfterDlss(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                                       const sources::WorkLayout& layout, std::chrono::steady_clock::time_point now, uint32_t group,
                                                       std::optional<bool> before_reset) {
  ApplyLookConfig(now, group);
  // The game's reset, filtered once per evaluate: behind a Before upscaling group, the answer that group's recording got.
  bool reset = false;
  if (before_reset) {
    reset = *before_reset;
  } else {
    reset = reset_merger_.Filter(frame.reset, now);
  }
  const sources::PipelineResult result = after_dlss_->Run(host, frame, reset, DlssConfig(layout, group));
  if (result.recorded) {
    last_game_list_recording_ = now;
    last_dlss_token_ = result.token;  // fix round 2 (I1): marked if the chain splits before its work is done
    if (chain_.Split()) {
      dlss_stage_token_ = std::max(dlss_stage_token_, result.token);
      timeline_->MarkStageToken(result.token);  // fix round 1 (I1): where its list goes decides whether Present may run beside it
    }
  }
  StageState& stage = stages_[static_cast<size_t>(Stage::AFTER_DLSS)];
  stage.nr_applied = result.nr_applied;
  stage.passes_run = result.passes_run;
  stage.skip_reason = (result.nr_applied ? std::string_view() : result.reason);
  stage.motion_source = result.motion_source;
  // Important 1 (fix round 2): a submit-only thread's tokens age out only after Timeline's own
  // AGED_SUBMISSION_TIME, so the ring can stay fully busy for a while there by design (see the ring
  // size in color_pipeline.hpp). One WARN per context once that has held for over a second running,
  // so an engine shape worse than the ring was sized for is diagnosable in the log.
  if (result.reason == "descriptors busy") {
    if (!descriptors_busy_since_) {
      descriptors_busy_since_ = now;
    } else if (!warned_descriptors_busy_ && now - *descriptors_busy_since_ > DESCRIPTORS_BUSY_WARN_AGE) {
      warned_descriptors_busy_ = true;
      nr::Log(nr::LogLevel::WARN,
              "NR after DLSS: descriptors busy for over 1 s in a row; the descriptor ring may be too small for "
              "this thread's submission timing");
    }
  } else {
    descriptors_busy_since_.reset();
  }
  if (group == 0u) {
    // The status and NR's heartbeat follow pass 1's group: one NR frame per frame.
    nr_applied_ = result.nr_applied;
    nr_recordings_ += (result.nr_applied ? 1u : 0u);
    passes_run_ = result.passes_run;
    skip_reason_ = (result.nr_applied ? std::string_view() : result.reason);
    skip_from_session_ = (result.nr_applied ? false : result.from_session);
    motion_source_ = result.motion_source;
    motion_scale_x_ = result.motion_scale_x;
    motion_scale_y_ = result.motion_scale_y;
    frame_size_ = frame.snapshot.output;
    if (result.nr_applied && !logged_after_dlss_) {
      logged_after_dlss_ = true;  // once per activation, as the Present path logs its trigger
      nr::Log(nr::LogLevel::INFO, "NR trigger: after DLSS");
    }
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "evaluate: NR {} after DLSS, {} pass(es) {}", (result.nr_applied ? "applied" : "skipped"),
             result.passes_run, result.reason);
  }
  return result;
}

bool DeviceContext::BeforeUpscaling() const {
  // A bridged context runs the DLSS stages only as the Direct3D 11 bridge's (dlss_stages_), which keeps DLSS-SR only.
  const ngx_hooks::NgxApi api = (bridged_ ? ngx_hooks::NgxApi::D3D11 : ngx_hooks::NgxApi::D3D12);
  return config_.pre_upscale && PreSrFeatureAllowed(main_feature_, api);
}

ID3D12Resource* DeviceContext::PrepareMaskCopy(const D3D12_RESOURCE_DESC* mask) {
  const bool wanted = (!torn_down_ && !device_lost_ && config_.look.mask && session_->State() == nr::SessionState::ACTIVE);
  return present_source_->Pipeline().PrepareMaskCopy(wanted ? mask : nullptr);
}

void DeviceContext::ApplyLookConfig(std::chrono::steady_clock::time_point now, uint32_t group) {
  sources::LookConfig look = config_.look;
  std::optional<std::chrono::steady_clock::time_point>& last_look_recording = last_look_recordings_[group];
  if (last_look_recording) {
    look.frame_seconds = std::chrono::duration<float>(now - *last_look_recording).count();  // the pipeline clamps it
  }
  last_look_recording = now;
  // Decision Q5b: Result shaping acts on the last group's change only (shaping twice would double it); an earlier group's change passes through as it is.
  // Fix round 1 (I2): the last group that runs, so a later group the budget cut (or the Defaults view hides) never takes the shaping away. A group that
  // skips for the order (I1) cannot be known ahead: rare, accepted.
  uint32_t shaped = (chain_.group_count > 0u ? chain_.group_count - 1u : 0u);
  while (shaped > 0u && !session_->SliceRuns(slices_[shaped])) {
    --shaped;
  }
  if (group != shaped) {
    look.look.enabled = false;
  }
  // Decision Q5a: pass 1's group is protected whenever Keep faces is on, a later group only with its stage's "Keep faces here".
  if (group > 0u && group < chain_.group_count) {
    look.keep_faces.enabled = (look.keep_faces.enabled && LaterStageKeepsFaces(chain_.groups[group].stage));
  }
  present_source_->Pipeline().SetLookConfig(look);
}

void DeviceContext::UpdateChain() {
  // Design 6.1: why each stage cannot run now. A DLSS pause is a wait, never a cause (decision Q4): its groups skip those frames.
  const bool dlss_off = (dlss_off_.Off() && !core_hold_.Held());
  std::string_view dlss_cause;
  if (!dlss_unavailable_reason_.empty()) {
    dlss_cause = dlss_unavailable_reason_;
  } else if (!dlss_seen_) {
    dlss_cause = "DLSS has not run yet";
  } else if (dlss_off) {
    dlss_cause = "the game's DLSS is off";
  }
  ChainFacts facts = {
      .pass1 = placement_.placement,
      .passes = config_.session.pass_count,
      .stored = config_.pass_stages,
      .split_capable = config_.split_capable,
  };
  std::string_view before_cause = dlss_cause;
  if (before_cause.empty()) {
    before_cause = (!before_upscaling_problem_.empty() ? std::string_view(before_upscaling_problem_)
                    : BeforeUpscaling()                ? std::string_view()
                                                       : std::string_view("Before upscaling is off"));
  }
  facts.cannot[static_cast<size_t>(Stage::BEFORE_UPSCALING)] = before_cause;
  facts.cannot[static_cast<size_t>(Stage::AFTER_DLSS)] = (dlss_cause.empty() ? std::string_view(after_dlss_problem_) : dlss_cause);
  // Observe the game's queues even while opted in, so disabling the experiment immediately restores the existing remap without waiting for a new recording.
  if (present_queue_ != nullptr && SplitStored()) {
    ID3D12CommandQueue* const other = timeline_->StageQueueOtherThan(present_queue_);
    if (other != other_dlss_queue_) {
      if (other != nullptr) {
        nr::Logf(nr::LogLevel::INFO, "Direct3D 12: the game's DLSS runs on another queue ({}) than its presents ({}){}: {}",
                 static_cast<const void*>(other), static_cast<const void*>(present_queue_),
                 (frame_generation_.State().active ? " with frame generation on" : ""),
                 (config_.allow_cross_queue_passes ? "experimental cross-queue passes allowed" : "passes stored at Present run at the DLSS stage"));
      } else {
        nr::Logf(nr::LogLevel::INFO, "Direct3D 12: the presents ({}) are on the game's DLSS queue again: passes stored at Present run there again",
                 static_cast<const void*>(present_queue_));
      }
      other_dlss_queue_ = other;
    }
  }
  std::string_view present_cause;
  if (!present_cannot_.empty()) {
    present_cause = present_cannot_;
  } else if (frame_generation_.State().active && !config_.present_with_frame_gen) {
    present_cause = "frame generation is on";
  } else if (!config_.allow_cross_queue_passes && other_dlss_queue_ != nullptr) {
    present_cause = (frame_generation_.State().active ? FRAME_GEN_QUEUE_REASON : OTHER_QUEUE_REASON);
  }
  facts.cannot[static_cast<size_t>(Stage::PRESENT)] = present_cause;
  ChainPlan chain = DecideChain(facts);

  const auto same_groups = [](const ChainPlan& first, const ChainPlan& second) {
    if (first.group_count != second.group_count) return false;
    for (uint32_t index = 0u; index < first.group_count; ++index) {
      const StageGroup& a = first.groups[index];
      const StageGroup& b = second.groups[index];
      if (a.stage != b.stage || a.first != b.first || a.count != b.count) return false;
    }
    return true;
  };
  if (!same_groups(chain, chain_)) {
    // Logged only around a split, so a one-group chain logs exactly what the placement line already says.
    if (chain.Split()) {
      nr::Logf(nr::LogLevel::INFO, "NR chain: {}", FormatChainLine(chain, {}));
    } else if (chain_.Split()) {
      nr::Log(nr::LogLevel::INFO, "NR chain: every pass at one stage");
    }
    for (size_t stage = 0u; stage < STAGE_COUNT; ++stage) {
      const auto uses = [stage](const ChainPlan& plan) {
        return std::ranges::any_of(std::span(plan.groups).first(plan.group_count),
                                   [stage](const StageGroup& group) { return static_cast<size_t>(group.stage) == stage; });
      };
      if (!uses(chain) || !uses(chain_)) {
        stages_[stage] = {};  // a stage that joins the chain works at its own size at once, and reports nothing stale from an earlier time
      }
    }
  }
  if (chain.note != chain_.note && !chain.note.empty()) {
    nr::Logf(nr::LogLevel::INFO, "NR chain: {}", chain.note);
  }
  if (chain.Split() && !chain_.Split() && last_dlss_token_ != 0u) {
    // Fix round 2 (I1): the DLSS-stage recording made under the one-group chain (unmarked) may still run on another queue when a later Present group first
    // runs: marked now, it holds that group (StageWorkDone) until it is on the present queue or complete, and names its queue to the latch.
    timeline_->MarkStageToken(last_dlss_token_);
    dlss_stage_token_ = std::max(dlss_stage_token_, last_dlss_token_);
  }
  chain_ = std::move(chain);
  for (uint32_t index = 0u; index < chain_.group_count; ++index) {
    const StageGroup& group = chain_.groups[index];
    const bool last = (index + 1u == chain_.group_count);
    slices_[index] = {.stage = static_cast<uint8_t>(group.stage), .first = group.first, .count = (last ? nr::MAX_PASSES - group.first : group.count)};
  }
  if (chain_.group_count > 0u) {
    session_->SetChain(std::span(slices_).first(chain_.group_count));
  }
  // Design 3.3: a group the chain no longer has frees its set behind its last use; set 0 stays, as the single set always did.
  present_source_->Pipeline().RetireSetsFrom(std::max(chain_.group_count, 1u));
}

std::optional<uint32_t> DeviceContext::GroupAt(Stage stage) const {
  for (uint32_t index = 0u; index < chain_.group_count; ++index) {
    if (chain_.groups[index].stage == stage) return index;
  }
  return std::nullopt;
}

bool DeviceContext::SplitStored() const {
  if (!config_.split_capable) return false;
  const uint32_t later = std::min(config_.session.pass_count, ui::MAX_PASSES) - std::min(config_.session.pass_count, 1u);
  return std::any_of(config_.pass_stages.begin(), config_.pass_stages.begin() + later, [](ui::PassStage stage) { return stage != ui::PassStage::SAME; });
}

std::string_view DeviceContext::SliceSkipReason(uint32_t group) const {
  // As Session::Evaluate says it: the Defaults view hides a pass without a reason; a budget cut is "budget".
  const bool hidden_by_view = (config_.pass_view_limit != 0u && slices_[group].first >= config_.pass_view_limit);
  return (hidden_by_view ? std::string_view() : std::string_view("budget"));
}

bool DeviceContext::LaterStageKeepsFaces(Stage stage) const {
  switch (stage) {
    case Stage::AFTER_DLSS:       return config_.stage_keep_faces[0];
    case Stage::PRESENT:          return config_.stage_keep_faces[1];
    case Stage::BEFORE_UPSCALING: break;  // always pass 1's group
  }
  return false;
}

void DeviceContext::NotePlacementChange() {
  // Minor 8 (fix round 1) / Minor 2 (fix round 4): logged_placement_ starts unset, so the very first
  // placement either caller (BeginFrame, OnDlssEvaluate's present-starvation catch-up) decides is
  // always logged once, NONE (with its reason) included.
  if (logged_placement_ && *logged_placement_ == placement_.placement) return;
  logged_placement_ = placement_.placement;
  last_dlss_diagnostic_.reset();
  nr::Log(nr::LogLevel::INFO, FormatPlacementLine(placement_, main_snapshot_));
  // A new placement's work size applies at once.
  switch (placement_.placement) {
    case Placement::BEFORE_UPSCALING: stages_[static_cast<size_t>(Stage::BEFORE_UPSCALING)].settle.Reset(); break;
    case Placement::AFTER_DLSS:       stages_[static_cast<size_t>(Stage::AFTER_DLSS)].settle.Reset(); break;
    case Placement::PRESENT:          stages_[static_cast<size_t>(Stage::PRESENT)].settle.Reset(); break;
    case Placement::NONE:             break;
  }
}

sources::WorkLayout DeviceContext::SettleLayout(nr::Size output, Stage stage, std::chrono::steady_clock::time_point now) {
  // Split chain (design 4): pass 1's group works at Setup's Resolution; a later group at its stage's own (Before upscaling is always pass 1's).
  const bool pass1 = (chain_.group_count == 0u || chain_.groups[0].stage == stage);
  ui::ResolutionMode mode = config_.resolution;
  float custom_scale = config_.resolution_scale;
  bool slider_held = config_.resolution_slider_held;
  if (!pass1) {
    const FrameConfig::StageResolution& own = config_.stage_resolution[(stage == Stage::AFTER_DLSS ? 0u : 1u)];
    mode = own.mode;
    custom_scale = own.scale;
    slider_held = own.slider_held;
  }
  const bool before_upscaling = (stage == Stage::BEFORE_UPSCALING);
  double target_width = output.width;
  double target_height = output.height;
  double scale = 1.0;
  switch (mode) {
    case ui::ResolutionMode::FULL:        break;
    case ui::ResolutionMode::QUALITY:     scale = sources::QUALITY_SCALE; break;
    case ui::ResolutionMode::BALANCED:    scale = sources::BALANCED_SCALE; break;
    case ui::ResolutionMode::PERFORMANCE: scale = sources::PERFORMANCE_SCALE; break;
    case ui::ResolutionMode::CUSTOM:      scale = custom_scale / 100.0; break;
    case ui::ResolutionMode::MATCH_GAME:
      // v2 design §3.8 (D3): DLSS's create-time render size, stable under dynamic resolution. Before
      // upscaling, NR already works at the render size; until DLSS is seen, Full. Plan 18 Task 12 (fix round 1, M-1): Full while the game's DLSS is off
      // (its last create-time size is stale: the game renders at the output size again).
      if (main_snapshot_ && !main_snapshot_->render.Empty() && !before_upscaling && !dlss_off_.Off()) {
        target_width = main_snapshot_->render.width;
        target_height = main_snapshot_->render.height;
      }
      break;
  }
  StageState& state = stages_[static_cast<size_t>(stage)];
  const nr::Size image = state.settle.Update(sources::WorkSize(target_width * scale, target_height * scale, output), now, slider_held);
  const nr::Size canvas = ((before_upscaling && !nr::MeetsNrFloor(image)) ? sources::CanvasSize(image) : image);
  state.work = image;
  state.canvas = canvas;
  if (pass1) {
    work_size_ = image;
    canvas_size_ = canvas;
  }
  session_->NoteFrameSize(static_cast<uint8_t>(stage), canvas);
  return {.image = image, .canvas = canvas, .upsampling = config_.upsampling};
}

sources::AfterDlssConfig DeviceContext::DlssConfig(const sources::WorkLayout& layout, uint32_t group) const {
  return {
      .encoding = config_.encoding,
      .diffuse_white_nits = config_.diffuse_white_nits,
      .transfer_strength = config_.transfer_strength,
      .color_strength = config_.color_strength,
      .motion_vectors = config_.motion_vectors,
      .motion_scale_x = config_.motion_scale_x,
      .motion_scale_y = config_.motion_scale_y,
      .depth_inverted = config_.depth_inverted,
      .chained_history = config_.chained_history,
      .restore = config_.restore,
      .controls = controls_,
      .layout = layout,
      .part = {.slice = slices_[group], .set = group},
  };
}

ID3D12Resource* DeviceContext::BeforeDlssEvaluate(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                                  std::chrono::steady_clock::time_point now) {
  if (torn_down_ || device_lost_ || NoteDeviceRemoved(now)) return nullptr;
  NoteDlssFeature(frame.serial);
  if (placement_.placement != Placement::BEFORE_UPSCALING || dlss_idle_) return nullptr;
  frame_size_ = {frame.color_region.width, frame.color_region.height};
  const sources::WorkLayout layout = SettleLayout(frame_size_, Stage::BEFORE_UPSCALING, now);  // N10: in every state
  if (session_->State() != nr::SessionState::ACTIVE) {
    nr_applied_ = false;
    skip_reason_ = {};
    skip_from_session_ = false;
    return nullptr;
  }
  const sources::PipelineResult result = RecordBeforeUpscaling(frame, host, layout, now);
  return (result.nr_applied ? PrivateColor() : nullptr);
}

sources::PipelineResult DeviceContext::RecordBeforeUpscaling(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                                             const sources::WorkLayout& layout, std::chrono::steady_clock::time_point now) {
  ApplyLookConfig(now, 0u);  // Before upscaling is always pass 1's group
  const bool reset = reset_merger_.Filter(frame.reset, now);
  const sources::PipelineResult result = pre_sr_->Run(host, frame, reset, DlssConfig(layout, 0u));
  if (chain_.Split()) {
    before_reset_ = reset;  // the After DLSS group behind it in this evaluate takes the same answer
  }
  if (result.recorded) {
    last_game_list_recording_ = now;  // the latch window covers pre-SR's recordings too
    last_dlss_token_ = result.token;  // fix round 2 (I1), as After DLSS's
    if (chain_.Split()) {
      dlss_stage_token_ = std::max(dlss_stage_token_, result.token);
      timeline_->MarkStageToken(result.token);  // fix round 1 (I1), as After DLSS's
    }
  }
  StageState& stage = stages_[static_cast<size_t>(Stage::BEFORE_UPSCALING)];
  stage.nr_applied = result.nr_applied;
  stage.passes_run = result.passes_run;
  stage.skip_reason = (result.nr_applied ? std::string_view() : result.reason);
  stage.motion_source = result.motion_source;
  nr_applied_ = result.nr_applied;
  nr_recordings_ += (result.nr_applied ? 1u : 0u);
  passes_run_ = result.passes_run;
  skip_reason_ = (result.nr_applied ? std::string_view() : result.reason);
  skip_from_session_ = (result.nr_applied ? false : result.from_session);
  motion_source_ = result.motion_source;
  motion_scale_x_ = result.motion_scale_x;
  motion_scale_y_ = result.motion_scale_y;
  if (result.nr_applied && !logged_after_dlss_) {
    logged_after_dlss_ = true;  // once per activation, as After DLSS logs its trigger
    nr::Log(nr::LogLevel::INFO, "NR trigger: before upscaling");
  }
  return result;
}

BridgedDlssDecision DeviceContext::DecideBeforeEvaluate(const ngx_hooks::DlssFrame& frame, nr::Size render_region,
                                                        std::chrono::steady_clock::time_point now) {
  before_reset_.reset();  // split chain: each evaluate's own, set only by this evaluate's Before upscaling recording
  if (torn_down_ || device_lost_ || NoteDeviceRemoved(now)) return {};
  if (placement_.placement != Placement::BEFORE_UPSCALING || dlss_idle_ || frame.feature != NVSDK_NGX_Feature_SuperSampling) return {};
  frame_size_ = render_region;
  bridged_layouts_[0] = SettleLayout(frame_size_, Stage::BEFORE_UPSCALING, now);  // N10: in every state
  if (session_->State() != nr::SessionState::ACTIVE) {
    nr_applied_ = false;
    skip_reason_ = {};
    skip_from_session_ = false;
    return {};
  }
  if (controls_.intensity <= 0.f) {
    NoteBridgedSkip(BridgedDlssWork::BEFORE_UPSCALING, "intensity 0");  // exact pass-through: nothing crosses, DLSS reads the game's own Color
    return {};
  }
  return {.work = BridgedDlssWork::BEFORE_UPSCALING};
}

BridgedDlssDecision DeviceContext::DecideAfterEvaluate(const ngx_hooks::DlssFrame& frame, nr::Size output_region,
                                                       std::chrono::steady_clock::time_point now) {
  if (!NoteMainEvaluate(frame, now)) {
    before_reset_.reset();
    return {};
  }
  if (placement_.placement == Placement::PRESENT) {
    // Plan 18 (design §4): Source = Present copies DLSS's vectors for the present that closes this frame, as Direct3D 12's amendment 7, into the bridge's ring.
    if (config_.source != ui::PlacementSource::PRESENT || !config_.motion_vectors || frame.motion_vectors == nullptr
        || session_->State() != nr::SessionState::ACTIVE) {
      return {};
    }
    return {.work = BridgedDlssWork::PRESENT_MOTION,
            .motion_scale_x = frame.mv_scale_x * config_.motion_scale_x,
            .motion_scale_y = frame.mv_scale_y * config_.motion_scale_y};
  }
  // Split chain (Task 4): pass 1 runs inside the game's frame here, so a Present group is a later one: DLSS's vectors go into the ring for it, as Direct3D 12
  // native copies them (only for a Present group that runs, fix round 1 M4), and the add-on copies them before After DLSS's hand-off.
  BridgedDlssDecision decision;
  if (const std::optional<uint32_t> present = GroupAt(Stage::PRESENT);
      present && config_.motion_vectors && frame.motion_vectors != nullptr && session_->State() == nr::SessionState::ACTIVE
      && session_->SliceRuns(slices_[*present])) {
    decision.present_motion = true;
    decision.motion_scale_x = frame.mv_scale_x * config_.motion_scale_x;
    decision.motion_scale_y = frame.mv_scale_y * config_.motion_scale_y;
  }
  const std::optional<uint32_t> group = GroupAt(Stage::AFTER_DLSS);
  if (!group || dlss_idle_) {
    descriptors_busy_since_.reset();
    before_reset_.reset();
    return decision;
  }
  bridged_layouts_[1] = SettleLayout(output_region, Stage::AFTER_DLSS, now);  // N10: in every state
  StageState& stage = stages_[static_cast<size_t>(Stage::AFTER_DLSS)];
  if (session_->State() != nr::SessionState::ACTIVE) {
    stage.nr_applied = false;
    stage.skip_reason = {};
    if (*group == 0u) {
      nr_applied_ = false;
      skip_reason_ = {};
      skip_from_session_ = false;
    }
    descriptors_busy_since_.reset();
    before_reset_.reset();
    return decision;
  }
  if (*group > 0u && !session_->SliceRuns(slices_[*group])) {
    // Split chain: a later group the budget cut to no pass (or the Defaults view hides) crosses nothing; its next recording starts afresh.
    after_dlss_->OweReset();
    stage.nr_applied = false;
    stage.passes_run = 0u;
    stage.skip_reason = SliceSkipReason(*group);
    before_reset_.reset();
    return decision;
  }
  const std::string_view skip = (!nr::MeetsNrFloor(output_region) ? std::string_view("frame too small")
                                 : controls_.intensity <= 0.f     ? std::string_view("intensity 0")
                                                                  : std::string_view());
  if (!skip.empty()) {
    NoteBridgedSkip(BridgedDlssWork::AFTER_DLSS, skip);  // as AfterDlssSource::Run says it, and owes it; nothing crosses
    before_reset_.reset();
    return decision;
  }
  decision.work = BridgedDlssWork::AFTER_DLSS;
  return decision;
}

sources::PipelineResult DeviceContext::RecordBridgedStage(BridgedDlssWork work, const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                                          std::chrono::steady_clock::time_point now) {
  // Split chain (Task 4): After DLSS records its own group (pass 1's, or a later one behind Before upscaling, which then takes the reset that group's
  // recording filtered in this evaluate); Before upscaling is always pass 1's.
  switch (work) {
    case BridgedDlssWork::AFTER_DLSS:
      return RecordAfterDlss(frame, host, bridged_layouts_[1], now, GroupAt(Stage::AFTER_DLSS).value_or(0u), std::exchange(before_reset_, std::nullopt));
    case BridgedDlssWork::BEFORE_UPSCALING: return RecordBeforeUpscaling(frame, host, bridged_layouts_[0], now);
    case BridgedDlssWork::NONE:
    case BridgedDlssWork::PRESENT_MOTION:   break;
  }
  return {.reason = "not placed after DLSS"};
}

bool DeviceContext::PresentRingWanted() const {
  if (placement_.placement == Placement::PRESENT) return config_.present_motion_copy;
  return (config_.motion_vectors && GroupAt(Stage::PRESENT).has_value());
}

void DeviceContext::NoteStageProblem(BridgedDlssWork stage, std::string problem) {
  std::string& slot = (stage == BridgedDlssWork::BEFORE_UPSCALING ? before_upscaling_problem_ : after_dlss_problem_);
  if (problem.empty() || slot == problem) return;
  nr::Logf(nr::LogLevel::WARN, "{} cannot run here: {}; {}", (stage == BridgedDlssWork::BEFORE_UPSCALING ? "NR before upscaling" : "NR after DLSS"),
           problem, (stage == BridgedDlssWork::BEFORE_UPSCALING ? "NR runs after DLSS instead" : "Auto runs at Present"));
  slot = std::move(problem);
}

void DeviceContext::NoteBridgedSkip(BridgedDlssWork stage, std::string_view reason) {
  if (stage == BridgedDlssWork::BEFORE_UPSCALING) {
    pre_sr_->OweReset();
  } else {
    after_dlss_->OweReset();
  }
  // Split chain (Task 4): the stage's own group reports it (Status's groups and the card's pass count); the status above them follows pass 1's group only.
  const Stage chain_stage = (stage == BridgedDlssWork::BEFORE_UPSCALING ? Stage::BEFORE_UPSCALING : Stage::AFTER_DLSS);
  StageState& state = stages_[static_cast<size_t>(chain_stage)];
  state.nr_applied = false;
  state.passes_run = 0u;
  state.skip_reason = reason;
  state.motion_source = sources::MotionSource::NONE;
  if (GroupAt(chain_stage).value_or(0u) != 0u) return;
  nr_applied_ = false;
  passes_run_ = 0u;
  skip_reason_ = reason;
  skip_from_session_ = false;
  motion_source_ = sources::MotionSource::NONE;  // as a skip from the sources' Run leaves it
  motion_scale_x_ = 1.f;
  motion_scale_y_ = 1.f;
}

bool DeviceContext::StageResetOwed(BridgedDlssWork stage) const {
  return (stage == BridgedDlssWork::BEFORE_UPSCALING ? pre_sr_->ResetOwed() : after_dlss_->ResetOwed());
}

void DeviceContext::NoteGameDeviceRemoved(HRESULT reason, std::chrono::steady_clock::time_point now) {
  if (!bridged_ || dlss_latch_tripped_ || !last_game_list_recording_ || now - *last_game_list_recording_ > DLSS_LATCH_WINDOW) return;
  dlss_latch_tripped_ = true;
  game_device_removed_ = std::format("The game's device was removed ({:#010x}) soon after NR ran inside its frame: the DLSS placements stay off from the next "
                                     "start (Clear latch to try again)", static_cast<uint32_t>(reason));
  nr::Log(nr::LogLevel::ERR, game_device_removed_);
}

void DeviceContext::OnColorSwapRejected() {
  // Minor 9 (fix round 4): BeforeDlssEvaluate already applied NR into the private colour and set
  // nr_applied_ true, on the assumption that the detour would swap it into the game's own Color. The
  // detour calls this only when that swap could not happen (the game's NGX block had no readable Color
  // to swap), so DLSS reads its own Color unchanged this evaluate: the status must say so, not "applied".
  if (placement_.placement != Placement::BEFORE_UPSCALING || !nr_applied_) return;
  nr_applied_ = false;
  skip_reason_ = "no DLSS colour to swap";
  skip_from_session_ = false;
}

void DeviceContext::SubmitTokens(std::span<const uint64_t> tokens, ID3D12CommandQueue* queue, uint32_t thread_id,
                                 std::chrono::steady_clock::time_point now) {
  timeline_->Submit(tokens, queue, thread_id, now);
}

void DeviceContext::ResubmitTokens(uint64_t first_token, ID3D12CommandQueue* queue, uint32_t thread_id,
                                   std::chrono::steady_clock::time_point now) {
  timeline_->Resubmit(first_token, queue, thread_id, now);
}

void DeviceContext::DropTokens(std::span<const uint64_t> tokens) {
  timeline_->Drop(tokens);
}

void DeviceContext::StampThread(uint32_t thread_id) {
  if (!device_lost_) {
    timeline_->StampThread(thread_id);
  }
}

void DeviceContext::UpdateSession(std::chrono::steady_clock::time_point now) {
  const bool dlss_placement =
      (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  const bool was_idle = dlss_idle_;
  dlss_idle_ = (dlss_placement && last_main_evaluate_.has_value()
                && now - *last_main_evaluate_ > std::max(2 * config_.session.grace, DLSS_IDLE_MINIMUM));
  dlss_paused_ = (last_main_evaluate_.has_value() && now - *last_main_evaluate_ > DLSS_IDLE_MINIMUM);
  // Split chain (design 3.4): NR is wanted while some group can run. A later Present group runs on while the game's DLSS idles (a menu), so the DLSS idle
  // drain waits for none then; the DLSS groups skip those frames and restart their history when DLSS returns (design 6.1).
  const bool later_present = (dlss_placement && GroupAt(Stage::PRESENT).has_value());
  if (chain_.Split() && dlss_paused_) {
    pre_sr_->OweReset();
    after_dlss_->OweReset();
    // Fix round 2 (m2): the card counts the passes that run; the DLSS groups run none while DLSS is paused.
    for (const Stage stage : {Stage::BEFORE_UPSCALING, Stage::AFTER_DLSS}) {
      stages_[static_cast<size_t>(stage)].nr_applied = false;
      stages_[static_cast<size_t>(stage)].passes_run = 0u;
    }
  }
  const bool placement_ready =
      (dlss_placement ? (!dlss_idle_ || later_present) : (placement_.placement == Placement::PRESENT && encoding_.has_value()));
  // Plan 15: never while held after the game's NGX shutdown (no NGX call on this device until its DLSS starts again), at any placement.
  const bool want = (config_.session.enabled && config_.nr_allowed && blocked_reason_.empty() && placement_ready && !core_hold_.Held());
  if (nr::IsLogEnabled(nr::LogLevel::TRACE) && config_.session.enabled
      && (!last_dlss_diagnostic_ || now - *last_dlss_diagnostic_ >= std::chrono::seconds(5) || was_idle != dlss_idle_)) {
    last_dlss_diagnostic_ = now;
    const int64_t age = (last_main_evaluate_ ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_main_evaluate_).count() : -1);
    nr::Logf(nr::LogLevel::TRACE,
             "DLSS diagnostic context {}: {}; accepted main calls {}; last main age {} ms (-1 means none); idle threshold {} ms, idle {}, paused {}; enabled {}, allowed {}, blocked [{}], held {}, abandoned {}, placement ready {}, want NR {}, session {}",
             static_cast<const void*>(device_.Get()), FormatPlacementLine(placement_, main_snapshot_), main_evaluates_, age,
             std::max(2 * config_.session.grace, DLSS_IDLE_MINIMUM).count(), dlss_idle_, dlss_paused_, config_.session.enabled,
             config_.nr_allowed, blocked_reason_, core_hold_.Held(), core_hold_.Abandoned(), placement_ready, want, nr::SessionStateName(session_->State()));
  }
  // New Important 1 (fix round 2), replacing Important 4 (fix round 1): that guard skipped
  // SetEnabled(false) whenever the Session was OFF, whatever made `want` false -- including the
  // user's own Enabled toggle, nr_allowed, or a blocked reason. While suspended the Session is OFF
  // with enabled_ still true, so a real disable was swallowed too: with AutoResume off, toggling
  // Enabled off then on no longer resumed NR (the re-enable read as a no-op repeat); with AutoResume
  // on, a user disable no longer cancelled it, so Session kept trying to resume NR the user had just
  // turned off (session_test.cpp's "disabling while suspended cancels auto-resume").
  // Skip SetEnabled(false) only when the DLSS idle is the SOLE reason `want` is false, AND the
  // Session is actually suspended (not merely OFF, which also closes the residual window where an
  // idle starting while a suspension is still DRAINING passed the disable through and later skipped
  // the hold) -- there is nothing to drain then, and routing it through SetEnabled(false) would clear
  // the suspension via Session::SetEnabled's OFF case, letting the next real enable bypass the resume
  // hold entirely.
  // Plan 15: never while held, so a suspension cannot auto-resume NR (a load) before the game's DLSS starts again.
  const bool idle_only =
      (config_.session.enabled && config_.nr_allowed && blocked_reason_.empty() && dlss_placement && dlss_idle_ && !later_present && !core_hold_.Held());
  if (!(idle_only && session_->Status().suspended)) {
    session_->SetEnabled(want);
  }
  // Logged before and after the tick, so a load (OFF -> LOADING -> ACTIVE) shows both steps.
  const auto note_state = [this] {
    const nr::SessionState state = session_->State();
    if (state == logged_state_) return;
    nr::Logf(nr::LogLevel::INFO, "session {} -> {}", nr::SessionStateName(logged_state_), nr::SessionStateName(state));
    logged_state_ = state;
    logged_trigger_ = TriggerPoint::NONE;  // each activation logs its trigger again
    logged_after_dlss_ = false;
    trace_frames_ = TRACE_FRAMES;
  };
  note_state();
  session_->Tick();
  note_state();
  if (session_->State() == nr::SessionState::OFF || session_->State() == nr::SessionState::FAILED) {
    present_source_->ReleaseIntermediates();
  }
}

bool DeviceContext::NoteDeviceRemoved(std::chrono::steady_clock::time_point now) {
  const HRESULT removed = device_->GetDeviceRemovedReason();
  if (SUCCEEDED(removed)) return false;
  device_lost_ = true;
  // Plan 18: a bridged context's device is Uplift's private one: its removal gets Retry now, never the DLSS latch (NoteGameDeviceRemoved decides that for
  // the game's device).
  dlss_latch_tripped_ = (!bridged_ && last_game_list_recording_.has_value() && now - *last_game_list_recording_ <= DLSS_LATCH_WINDOW);
  output_message_ = (dlss_latch_tripped_
                         ? std::format("Device removed ({:#010x}) soon after NR ran inside the game's frame: the DLSS "
                                       "placements stay off from the next start (Clear latch to try again)",
                                       static_cast<uint32_t>(removed))
                         : std::format("Device removed ({:#010x}): NR stays off on this device", static_cast<uint32_t>(removed)));
  nr::Log(nr::LogLevel::ERR, output_message_);
  if (bridged_) {
    LogDred(device_.Get());  // Plan 17: Uplift's private device (a bridge's, or the helper's): what was in flight, and any page fault
  }
  return true;
}

void DeviceContext::Teardown() {
  if (torn_down_) return;
  torn_down_ = true;
  frame_ready_ = false;
  // Fix round 1, Important 1: a removal first seen only here (no present or evaluate ran since it
  // happened) must still decide the latch before abandoning. Guarded on device_lost_, exactly as
  // BeginFrame and OnDlssEvaluate guard it, so an already-decided latch is never recomputed against
  // this call's `now` (T10-A: the decision is made once, at the first sighting, and kept). This also
  // replaces the removal check the old code ran inline here (FAILED(GetDeviceRemovedReason())):
  // NoteDeviceRemoved sets device_lost_ from that very same query, plus the latch it never used to decide.
  if (!device_lost_) {
    NoteDeviceRemoved(std::chrono::steady_clock::now());
  }
  if (device_lost_) {
    // Spec §13 (amendment 9): never call into the snippet after a removal. OnDeviceLost drops every NR
    // object without an NGX call and restores the snippet's IAT and log hooks (Plan 1's Snippet::Abandon),
    // so nothing points into this add-on after ReShade unloads it. The Session and RealHost are then
    // leaked on purpose, so no destructor can reach the snippet either.
    nr::Log(nr::LogLevel::WARN, "device removed: abandoning the NR runtime");
    session_->OnDeviceLost();
    static_cast<void>(session_.release());
    static_cast<void>(host_.release());
    return;
  }
  present_source_->ReleaseIntermediates();
  if (core_hold_.Abandoned()) {
    // Plan 15: the Session was abandoned at the game's NGX shutdown, so its own flush below does nothing: what waits for the GPU goes here (the game idled it).
    timeline_->Flush(std::chrono::seconds(2));
  }
  session_->OnDeviceDestroyed();  // flushes the timeline and every queue's tokens (2 s cap), then unloads; Plan 15: after the game's NGX shutdown, nothing
                                  // of NGX's is left to release here (OnCoreShutdown did it, or NR was off), so this makes no NGX call
}

void DeviceContext::OnCoreShutdown(std::chrono::milliseconds cap, uint64_t upscaler_creates, uint64_t serial) {
  if (bridged_ || torn_down_ || device_lost_) return;
  const auto now = std::chrono::steady_clock::now();
  if (NoteDeviceRemoved(now)) return;  // a removed device: never an NGX call (Teardown abandons the runtime)
  if (core_hold_.Abandoned()) return;  // a shutdown after an abandon: nothing of NGX's is held any more
  core_hold_.Hold(upscaler_creates, serial);
  frame_ready_ = false;  // nothing is recorded on this device until the hold ends
  nr_applied_ = false;
  passes_run_ = 0u;
  skip_reason_ = {};
  skip_from_session_ = false;
  motion_source_ = sources::MotionSource::NONE;
  // OFF (and LOADING, which loads nothing before its Tick, and which the next UpdateSession turns OFF) holds nothing of NGX's: no wait, no release.
  if (const nr::SessionState state = session_->State(); state == nr::SessionState::OFF || state == nr::SessionState::LOADING) {
    nr::Log(nr::LogLevel::INFO, "Direct3D 12: the game shut NGX down on its device; NR was off there");
    return;
  }
  // The Present path's work is in the frames the frame fence counts (the current one is signalled now, after ReShade's submissions at the last present), and
  // every recording inside the game's frame in a completion token on its queue's fence: together they say when the GPU is done with NR's features.
  if (const std::string_view late = timeline_->WaitIdle(cap); !late.empty()) {
    if (NoteDeviceRemoved(std::chrono::steady_clock::now())) return;  // a removal seen during the wait: Teardown abandons the runtime
    nr::Logf(nr::LogLevel::WARN, "Direct3D 12: NR's last frames did not finish within {} ms of the game's NGX shutdown ({}); NR's runtime stays loaded until the game exits",
             cap.count(), late);
    core_hold_.Abandon();  // Status says so (ui::D3D12_NGX_ABANDONED_REASON) from now on
    session_->Abandon();   // no NGX call: the game's frames may still be running NR
    return;
  }
  present_source_->ReleaseIntermediates();  // A, B and the private colour go with the flush below (the wait above found their marks complete)
  session_->OnDeviceDestroyed();  // the features' release, DestroyParameters, the snippet's Shutdown1, the unload: all before the core's Shutdown1
  nr::Log(nr::LogLevel::INFO, "Direct3D 12: the game shut NGX down on its device; NR released before it");
}

std::string DeviceContext::CreateFailure() const {
  if (!session_) return {};
  nr::SessionStatus status = session_->Status();
  const bool out_of_memory = (status.create_result == static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_OutOfGPUMemory));
  return ((status.create_failed && !out_of_memory) ? std::move(status.message) : std::string());
}

ContextStatus DeviceContext::Status() const {
  ContextStatus status;
  if (session_) {
    status.session = session_->Status();
  }
  std::string placement_note = placement_.reason;
  // Split chain: a later Present group runs on while the game's DLSS idles, so NR is not waiting then.
  if ((placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING) && dlss_idle_
      && !GroupAt(Stage::PRESENT).has_value()) {
    placement_note = (status.session.state == nr::SessionState::OFF ? "Waiting for DLSS: NR memory released"
                                                                    : "Waiting for DLSS: releasing NR memory");
  }
  // Plan 15: after the game's NGX shutdown NR waits for the game's DLSS (temporary), or, abandoned there, stays off for the session (an output problem: no
  // BeginFrame clears it, unlike the target's own).
  // Plan 18 (fix round 1, M-2): the game's own Direct3D 11 device removed comes first after the abandon; BeginFrame rebuilds output_message_.
  const std::string_view output_problem = (core_hold_.Abandoned()           ? ui::D3D12_NGX_ABANDONED_REASON
                                           : !game_device_removed_.empty() ? std::string_view(game_device_removed_)
                                                                           : std::string_view(output_message_));
  if (core_hold_.Held() && !core_hold_.Abandoned()) {
    status.held = ui::D3D12_NGX_SHUT_DOWN_REASON;
    placement_note = std::string(ui::D3D12_NGX_SHUT_DOWN_REASON);
  }
  status.abandoned = core_hold_.Abandoned();
  status.after_dlss_problem = after_dlss_problem_;  // Plan 18 (design §3): Setup greys a stage the Direct3D 11 bridge cannot share
  status.before_upscaling_problem = before_upscaling_problem_;
  const bool claimed_elsewhere = (!config_.nr_allowed && config_.session.enabled);
  status.message = ContextMessage({
      .blocked = (blocked_reason_.empty() && claimed_elsewhere ? std::string_view("NR runs on another D3D12 device in this game")
                                                               : std::string_view(blocked_reason_)),
      .output = output_problem,
      .placement = placement_note,
      .skip_reason = skip_reason_,
      .skip_from_session = skip_from_session_,
      .session_message = status.session.message,
  });
  status.trigger = (placement_.placement == Placement::PRESENT ? trigger_used_ : TriggerPoint::NONE);
  status.nr_applied = nr_applied_;
  status.passes_run = passes_run_;
  if (chain_.Split()) {
    // Split chain: the card and Details count every group's passes, not only pass 1's group (the owner's Onimusha check showed "Running 1 of 3" with all
    // three running). A group whose latest recording skipped counts none, and a group's latest recording counts at most its own passes (it may predate a
    // chain change in this frame).
    status.passes_run = 0u;
    for (uint32_t index = 0u; index < chain_.group_count; ++index) {
      const StageGroup& group = chain_.groups[index];
      const StageState& stage = stages_[static_cast<size_t>(group.stage)];
      status.passes_run += (stage.nr_applied ? std::min(stage.passes_run, group.count) : 0u);
    }
  }
  status.frame = frame_size_;
  status.encoding = encoding_;
  status.diffuse_white_nits = diffuse_white_nits_;
  status.intermediate_bytes = present_source_->HeldBytes();
  status.keep_faces_supported = present_source_->Pipeline().KeepFacesSupported();
  status.device_lost = device_lost_;
  status.placement = placement_.placement;
  status.placement_line = FormatPlacementLine(placement_, main_snapshot_);
  status.frame_generation = frame_generation_.State();
  status.dlss_seen = dlss_seen_;
  status.dlss_latch_tripped = dlss_latch_tripped_;
  status.work = work_size_;
  status.canvas = canvas_size_;
  // Plan 6 (D1): the pieces of `message`, flattened for the status card ladder.
  status.blocked = blocked_reason_;
  status.claimed_elsewhere = claimed_elsewhere;
  status.output_problem = output_problem;
  status.placement_note = placement_note;
  status.skip_reason = skip_reason_;
  status.skip_from_session = skip_from_session_;
  // Plan 14 (design §1.3, §1.6): the facts Setup and the working card read.
  status.ray_reconstruction = (main_feature_ == NVSDK_NGX_Feature_RayReconstruction);
  status.game_passes_motion = !last_motion_vectors_missing_;
  if (dlss_seen_ && last_motion_vectors_missing_) {
    status.dlss_motion_gap = ui::MotionGap::GAME_PASSED_NONE;
  } else if (placement_.placement == Placement::PRESENT && dlss_seen_ && config_.motion_vectors && !config_.motion_vectors_want_launchpad
             && motion_source_ != sources::MotionSource::PRESENT_COPY) {
    // DLSS passes vectors, but this present's copy did not land. Final review, minor 2: Launchpad covering for it (Auto) counts too, so Setup never reads
    // it as DLSS not having started. Minor 1: the game's DLSS paused (a menu, a loading screen) is not the GPU running behind.
    status.dlss_motion_gap = (dlss_paused_ ? ui::MotionGap::DLSS_PAUSED : ui::MotionGap::NO_COPY);
  }
  if (placement_.placement == Placement::PRESENT && config_.launchpad_motion && motion_source_ == sources::MotionSource::NONE) {
    status.launchpad_gap = ui::MotionGap::NO_UPLIFT_MV;
  }
  // Plan 18 Task 12 (fix round 1, M-1): while the game's DLSS is off, Match game runs at Full too.
  const bool match_game_unread = (config_.resolution == ui::ResolutionMode::MATCH_GAME && (!main_snapshot_ || dlss_off_.Off()));
  status.resolution_applied = ((match_game_unread && placement_.placement != Placement::BEFORE_UPSCALING) ? ui::ResolutionMode::FULL : config_.resolution);
  status.upsampling = config_.upsampling;
  if (!work_size_.Empty()) {
    std::string detail;
    if (canvas_size_ != work_size_) {
      detail = std::format(" (a {}x{} render, padded)", work_size_.width, work_size_.height);
    } else if (work_size_ != frame_size_ && !frame_size_.Empty()) {
      detail = std::format(" of {}x{} ({})", frame_size_.width, frame_size_.height,
                           (config_.upsampling == color::Upsampling::CLASSIC ? "Classic" : "Edge-aware"));
    }
    if (match_game_unread) {
      detail += (dlss_off_.Off() ? " (Match game: the game's DLSS is off)" : " (Match game: waiting for DLSS)");
    }
    status.work_line = std::format("Working at {}x{}{}", canvas_size_.width, canvas_size_.height, detail);
  }
  if (nr_applied_) {
    status.exposure_line = sources::ExposureLine(present_source_->Pipeline().LastExposure());  // Plan 17
  }
  // Split chain (design 6.2): the chain line and note, and each group's latest recording.
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
  // The user's motion-vector-indicator request: NrPipeline's mechanical report (motion_source_, and the
  // scale for DIRECT) plus the reason for NONE, which only DeviceContext has enough context to say.
  status.motion_source = motion_source_;
  switch (motion_source_) {
    case sources::MotionSource::DIRECT: {
      // Q1(a): MotionVectors = Launchpad, but placement runs inside the game's frame, where Launchpad's
      // UPLIFT_MV cannot bind -- this frame's vectors are the game's own DLSS ones, and the readout says why.
      const bool launchpad_would_run = (config_.motion_vectors_want_launchpad && placement_.placement != Placement::PRESENT);
      status.motion_line = (launchpad_would_run
                                ? std::format("Motion vectors: DLSS ({} feeds the presented image only)", UpliftMvName(config_))
                                : std::format("Motion vectors: DLSS (the game's own, scale {} x {})", motion_scale_x_,
                                              motion_scale_y_));
      break;
    }
    case sources::MotionSource::PRESENT_COPY:
      status.motion_line = "Motion vectors: DLSS (copied for Present)";
      break;
    case sources::MotionSource::LAUNCHPAD:
      status.motion_line =
          std::format("Motion vectors: {} (UPLIFT_MV, scale {} x {})", UpliftMvName(config_), config_.motion_scale_x, config_.motion_scale_y);
      break;
    case sources::MotionSource::NONE: {
      std::string_view reason = "the game passed no motion vectors";
      if (!config_.motion_vectors && !config_.launchpad_motion) {
        reason = "set to Off";
      } else if (!config_.motion_vectors) {
        reason = (placement_.placement == Placement::PRESENT ? NoUpliftMvReason(config_)
                                                              : (config_.uplift_mv_lumenite ? "Lumenite works on the presented image only"
                                                                                            : "Launchpad works on the presented image only"));
      } else if (placement_.placement == Placement::PRESENT) {
        if (bridged_ && (config_.vulkan || dlss_stages_)) {
          // Plan 14: on Vulkan DLSS's vectors reach the Present path through the native context's copies, while the add-on asks for them. Plan 18: Direct3D
          // 11's ring is the same copy for the Present path's reasons.
          reason = (config_.present_motion_copy ? "no copy of DLSS's vectors this frame"
                    : config_.launchpad_motion  ? NoUpliftMvReason(config_)
                                                : "DLSS's vectors do not reach the Present path on this device");
        } else if (bridged_) {
          // Plan 8: a bridged context never sees the game's DLSS, so only Launchpad can feed it.
          reason = (config_.launchpad_motion ? NoUpliftMvReason(config_)
                                             : "DLSS vectors need a 64-bit Direct3D 11, Direct3D 12 or Vulkan game");
        } else if (!dlss_seen_) {
          // Auto is only ever transiently on Present while it waits for DLSS to show up; Source = Present
          // forced has nothing to wait for, so it reads as a plain description instead.
          reason = (config_.source == ui::PlacementSource::PRESENT ? "Present without DLSS" : "no DLSS running yet");
        } else if (!last_motion_vectors_missing_) {
          // Batch 1 re-review R2: DLSS is running and passes motion vectors -- this present's own copy
          // simply did not land -- so this must not read as "Present without DLSS" (no DLSS at all).
          reason = "no copy this frame";
        }
      }
      status.motion_line = std::format("Motion vectors: none ({})", reason);
      break;
    }
  }
  if (config_.pre_upscale && placement_.placement == Placement::AFTER_DLSS && !BeforeUpscaling()) {
    status.placement_line += " (Ray Reconstruction keeps NR after DLSS)";  // v2 design §3.9: the status says so
  }
  if (placement_.placement == Placement::BEFORE_UPSCALING && main_feature_ == NVSDK_NGX_Feature_RayReconstruction) {
    status.placement_line += " (Ray Reconstruction's input)";  // split-chain design 11: NR runs before RR denoises and upscales
  }
  switch (placement_.placement) {
    case Placement::AFTER_DLSS:
    case Placement::BEFORE_UPSCALING:
      status.ui_correction_note = "Not needed here: the HUD is drawn after NR";
      break;
    case Placement::PRESENT:
      status.ui_correction_note = "Unavailable here: the HUD is already in this image; use the NR mask";
      break;
    case Placement::NONE: break;
  }
  return status;
}

}  // namespace uplift::addon
