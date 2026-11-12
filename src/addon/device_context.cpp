#include "addon/device_context.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "addon/dred.hpp"
#include "nr/log.hpp"
#include "text/nr_messages.hpp"

namespace uplift::addon {
namespace {

constexpr uint32_t TRACE_FRAMES = 8u;  // per-frame traces for this many frames after a state change
// No present for this long while evaluates continue means the swap chain is starving.
constexpr auto PRESENT_STARVATION = std::chrono::milliseconds(250);
// DLSS_IDLE_MINIMUM is shared by the native and bridged placement state.
// A device removed this soon after Uplift recorded on a game list latches the DLSS placements off.
constexpr auto DLSS_LATCH_WINDOW = std::chrono::seconds(10);
// How long nr::SkipReason::DESCRIPTORS_BUSY may persist, back to back, before it is
// worth one WARN -- long enough that a normal submit-only-thread stall (aged out by Timeline's own
// AGED_SUBMISSION_TIME, at most a few hundred ms) never triggers it.
constexpr auto DESCRIPTORS_BUSY_WARN_AGE = std::chrono::seconds(1);
// Split chain: why a later group cannot run at Present while the game's DLSS-stage NR goes to another queue than the presents. With
// frame generation on, the presents come from its own queue (as in Onimusha), so the card says that instead.
constexpr std::string_view OTHER_QUEUE_REASON = "the game's DLSS runs on another queue";
constexpr std::string_view FRAME_GEN_QUEUE_REASON = "with frame generation on, the presents come from another queue";
// The completion-token counts of the five-second diagnostic: which path stamped them, what was recorded and never submitted, what is outstanding.
constexpr char TOKENS_DIAGNOSTIC_FORMAT[] =
    "DLSS diagnostic tokens: stamped {} by their thread ({} at a later ExecuteCommandLists call), {} by age; {} recorded but never submitted; {} outstanding (cumulative for this device)";

}  // namespace

std::unique_ptr<DeviceContext> DeviceContext::Create(ID3D12Device* device, nr::SnippetConfig snippet_config,
                                                     std::string* error, ContextOptions options) {
  std::unique_ptr<nr::Timeline> timeline = nr::Timeline::CreateD3D12(device);
  if (!timeline) {
    *error = "could not create the frame fence";
    return nullptr;
  }
  // The bridge's private device is one an adopted NGX core has never seen.
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
    dlss_stages_ = options.dlss_stages;  // A Direct3D 11 bridge's context runs the DLSS stages through the bridge's hand-off
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
  if ((bridged_ && !dlss_stages_) || reason == dlss_unavailable_reason_) return;  // A bridged context keeps its own (but Direct3D 11's)
  if (!reason.empty()) {
    nr::Logf(nr::LogLevel::INFO, "DLSS placements unavailable: {}", reason);
  }
  dlss_unavailable_reason_ = std::move(reason);
}

TriggerPoint DeviceContext::BeginFrame(ID3D12CommandQueue* queue, const FrameSettings& settings, const FrameFacts& facts, const TargetInfo& target,
                                       bool marker_expected, std::chrono::steady_clock::time_point now) {
  frame_ready_ = false;
  // Checked every frame, even OFF: a removed device must never be signalled or loaded into, and OFF
  // is exactly the state a Session settles into right after a mid-frame removal. The last
  // frame's status is reset before bailing, so a caller never sees a stale "applied" past this.
  if (torn_down_ || device_lost_ || NoteDeviceRemoved(now)) {
    status_.Clear();
    return TriggerPoint::NONE;
  }
  settings_ = settings;
  facts_ = facts;
  if (!settings.session.enabled) {
    present_jitter_.Reset();
  }
  controls_ = settings.controls;
  last_present_ = now;
  if (core_hold_.Resume(facts.upscaler_creates)) {
    // The game created its DLSS on this device after its NGX shutdown, so it re-initialised NGX (a DLSS setting changed): NR may load again,
    // through the normal path (UpdateSession below), into the core the game initialised anew.
    nr::Log(nr::LogLevel::INFO, "Direct3D 12: the game's DLSS started again on its device (a new DLSS feature); NR may load again");
  }
  timeline_->BeginFrame(queue);
  timeline_->StampAged(now);
  present_queue_ = queue;
  if (!facts.game_facing_present) frame_generation_.OnPresent(now);
  frame_generation_.Update(now, facts.ngx_frame_generation);

  // The game switched its DLSS off after DLSS ran here (it released its last DLSS feature, or on Direct3D 11 shut NGX down;
  // and that held for DLSS_IDLE_MINIMUM and two presents): the DLSS stages fall back to Present instead of waiting for it forever; a pause keeps a live
  // feature, so it still waits. Not while held after the game's NGX shutdown (the core-shutdown hold keeps priority: the shutdown ends the features too).
  dlss_off_.OnPresent(SwitchOffEvidence(dlss_seen_, facts.dlss_released, dlss_unavailable_reason_, core_hold_.Held()), now);
  const bool dlss_off = DlssStagesOff(dlss_off_.Off(), core_hold_.Held());
  const StageProblems problems = {.after_dlss = after_dlss_problem_, .before_upscaling = before_upscaling_problem_};
  // `logged_placement_` starts unset, so the very first placement this device
  // chooses is always logged once, NONE (with its reason) included. A stage the Direct3D 11 bridge cannot share is never chosen.
  placement_ = ChoosePlacement(settings.source, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(), problems, dlss_off);
  if (dlss_off && !dlss_off_logged_
      && placement_.placement != ChoosePlacement(settings.source, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(), problems).placement) {
    // Logged once a stage really fell back (with Source = Present nothing did).
    dlss_off_logged_ = true;
    nr::Log(nr::LogLevel::INFO, DLSS_OFF_LINE);
    logged_trigger_ = TriggerPoint::NONE;  // NR's triggers are logged again: at Present now, and after DLSS when it returns
    logged_after_dlss_ = false;
  }
  NotePlacementChange();
  // The back buffer as NR at Present sees it: pass 1's output problem there, and whether a later group can run there. Not read
  // when nothing could run at Present.
  std::string present_problem;
  std::optional<color::Encoding> present_encoding;
  nr::Size present_size;
  // For a later group only while some pass is stored at another stage, so the default path reads nothing more.
  const bool present_possible = (placement_.placement == Placement::PRESENT || SplitStored(settings_, facts_.split_capable));
  if (present_possible && !target.problem.empty()) {
    present_problem = std::string(target.problem);  // the D3D11 bridge cannot hand this back buffer to NR
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
      present_encoding = color::ResolveEncoding(settings.encoding, target.color_space, format);
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
    status_.Clear();
  }
  ran_since_present_ = false;
  // The Present motion copies are only useful with Source = Present and
  // MotionVectors wanting them; release them (through the Timeline, like any other intermediate) as
  // soon as either stops holding, rather than waiting for the Session to next go OFF. A no-op once
  // there is nothing left to release. Split chain: a later group at Present binds them too.
  const std::optional<uint32_t> present_group = GroupAt(chain_, Stage::PRESENT);
  if (!present_group || !settings.motion_vectors) {
    present_source_->Pipeline().ReleaseMotionCopies();
    present_jitter_.Reset();
  }
  // Launchpad's converted motion goes the same way once the Present path cannot bind UPLIFT_MV.
  if (!present_group || !settings.launchpad_motion) {
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

  ApplySessionOptions(*session_, settings, chain_, settings_generation_);
  UpdateSession(now);

  if (present_group && present_encoding) {
    const float diffuse_white_nits = (settings.diffuse_white_nits > 0.f ? settings.diffuse_white_nits
                                                                      : color::DefaultDiffuseWhiteNits(*present_encoding));
    if (*present_group == 0u) {
      diffuse_white_nits_ = diffuse_white_nits;  // the status describes pass 1's group
    }
    frame_target_ = {
        .resource = target.resource,
        .encoding = *present_encoding,
        .diffuse_white_nits = diffuse_white_nits,
        .transfer_strength = settings.transfer_strength,
        .color_strength = settings.color_strength,
    };
    // A described target (the D3D11 bridge before its shared copy exists) has nothing to record on yet.
    frame_ready_ = (session_->State() == nr::SessionState::ACTIVE && target.resource != nullptr);
  }
  return trigger_.OnPresent(marker_expected, facts.effects_on, facts.marker_rendered_early);
}

sources::PresentResult DeviceContext::Run(FrameHost* host, D3D12_RESOURCE_STATES entry_state, TriggerPoint point,
                                          ID3D12Resource* motion, bool motion_is_dlss, float dlss_scale_x, float dlss_scale_y,
                                          nr::Rect dlss_motion_region) {
  if (point == TriggerPoint::NONE || !frame_ready_) return {};
  frame_ready_ = false;
  ran_since_present_ = true;
  // Split chain: the Present group is pass 1's (group 0, the single chain of before) or a later one, whose result only Status's groups report.
  const uint32_t group = GroupAt(chain_, Stage::PRESENT).value_or(0u);
  StageState& stage = stages_[static_cast<size_t>(Stage::PRESENT)];
  const auto skip = [&](nr::SkipReason reason) -> sources::PresentResult {
    stage.nr_applied = false;
    stage.passes_run = 0u;
    stage.skip_reason = reason;
    stage.motion_source = sources::MotionSource::NONE;
    if (group == 0u) {
      // As Vulkan's SkipPresent: NR did not run, so the card says why instead of reading the last recording (BeginFrame keeps a status a Run wrote).
      status_.NoteSkip(reason, RecordingStatus::Reset::ALL_BUT_MOTION);
    } else {
      present_reset_owed_ = true;  // the Session never saw this frame: the group's next recording restarts its history
    }
    return {.reason = reason};
  };
  // The detector's evidence of frame generation: NGX's live features and the present cadence.
  const bool frame_generation_on = frame_generation_.ObservedState().active;
  if (FrameGenerationSkipsPresent(frame_generation_on, facts_.fg_present_path)) return skip(nr::SkipReason::FRAME_GENERATION_ON);
  // Split chain: a later group the budget cut to no pass (or the Defaults view hides) skips before its source copy and encode.
  if (group > 0u && !session_->SliceRuns(slices_[group])) return skip(SliceSkipReason(settings_, slices_[group]));
  // Default: keep distinct queues apart until UpdateChain remaps Present. The experimental opt-in uses the game's existing image handoff; it adds no wait.
  if (group > 0u && !facts_.game_facing_present && !settings_.allow_cross_queue_passes && !timeline_->StageWorkDone(dlss_stage_token_, present_queue_)) {
    return skip(nr::SkipReason::WAITING_EARLIER_STAGES);
  }
  host->FlushPending();
  ApplyLookConfig(last_present_.value_or(std::chrono::steady_clock::now()), group);
  host->TargetBarrier(entry_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  frame_target_.dlss_motion = {.resource = (motion_is_dlss && settings_.motion_vectors ? motion : nullptr),
                                .rect = (motion_is_dlss ? dlss_motion_region : nr::Rect{})};
  frame_target_.launchpad_motion = {.resource = (!motion_is_dlss && settings_.launchpad_motion ? motion : nullptr)};
  if (point == TriggerPoint::EARLY_MARKER) {
    frame_target_.launchpad_motion.rect = {.width = facts_.early_motion_extent.width, .height = facts_.early_motion_extent.height};
  }
  frame_target_.motion_scale_x = settings_.motion_scale_x;
  frame_target_.motion_scale_y = settings_.motion_scale_y;
  // Direct3D 11's ring keeps DLSS's raw vectors, so their scale travels as values; Vulkan's copies are already scaled (1).
  frame_target_.dlss_motion_scale_x = (motion_is_dlss ? dlss_scale_x : 1.f);
  frame_target_.dlss_motion_scale_y = (motion_is_dlss ? dlss_scale_y : 1.f);
  frame_target_.dlss_motion_raw = (motion_is_dlss && dlss_stages_);  // the motion copy filters them, as every other Present source
  frame_target_.dlss_motion_flip = (frame_target_.dlss_motion_raw && facts_.dlss_motion_upside_down);  // 1.1.6 (Vulkan's copies are flipped by theirs)
  const sources::PresentResult result = present_source_->Record(host->NativeList(), frame_target_, controls_, present_reset_owed_, present_layout_,
                                                                {.slice = slices_[group], .set = group});
  host->TargetBarrier(D3D12_RESOURCE_STATE_COPY_SOURCE, entry_state);
  if (result.nr_applied) {
    present_reset_owed_ = false;
  }
  NoteResult(Stage::PRESENT, group, result);
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
             (result.nr_applied ? "applied" : "skipped"), TriggerPointName(point), result.passes_run, nr::SkipReasonText(result.reason));
  }
  return result;
}

bool DeviceContext::NoteMainEvaluate(const ngx_hooks::DlssFrame& frame, std::chrono::steady_clock::time_point now) {
  // After a device removal Uplift never records on a game list or calls the snippet again. Reset the
  // last evaluate's status before bailing: while
  // presents starve, no BeginFrame runs to do it, so a caller could otherwise see a stale "applied"
  // from before the removal indefinitely.
  if (torn_down_ || device_lost_ || NoteDeviceRemoved(now)) {
    status_.Clear();
    descriptors_busy_since_.reset();
    return false;
  }
  if (core_hold_.ResumeOnEvaluate(frame.serial)) {
    // A successful evaluate of a feature the game created after its NGX shutdown proves the core serves this device
    // again. Before the catch-up below, so a game whose presents starve resumes here too.
    nr::Log(nr::LogLevel::INFO, "Direct3D 12: the game's DLSS started again on its device (a DLSS evaluate); NR may load again");
  }
  dlss_seen_ = true;
  last_main_evaluate_ = now;
  ++main_evaluates_;
  main_snapshot_ = frame.snapshot;
  main_feature_ = frame.feature;
  if (dlss_off_.OnEvaluate(!core_hold_.Held()) && std::exchange(dlss_off_logged_, false)) {
    // The game's DLSS runs again after it switched it off: the stage it fell back from returns from the next present (or the catch-up
    // below), as on first sight. While held, only an evaluate that ended the hold (above) counts. Every evaluate starts the release window over.
    nr::Log(nr::LogLevel::INFO, DlssOnAgainLine(ChoosePlacement(settings_.source, dlss_unavailable_reason_, true, BeforeUpscaling(),
                                                                {.after_dlss = after_dlss_problem_, .before_upscaling = before_upscaling_problem_})));
  }
  // Every evaluate updates this, whatever placement follows below, so the overlay's "Motion vectors"
  // line can tell "the game passed no motion vectors" from Present's other None reasons (Minor -- the
  // user's motion-vector-indicator request).
  last_motion_vectors_missing_ = (frame.motion_vectors == nullptr);
  frame_generation_.OnMainEvaluate(now);
  // Some frame-generation swap chains stop presenting while the game still evaluates;
  // the evaluate then drives the Timeline and the Session. The same
  // per-frame work a present would have done, so an idle drain during starvation still reloads NR on
  // the very evaluate that ends the idle period, instead of waiting for the next present.
  if (present_queue_ != nullptr && last_present_.has_value() && now - *last_present_ >= PRESENT_STARVATION) {
    timeline_->BeginFrame(present_queue_);
    timeline_->StampAged(now);
    // A DLSS first seen while presents starve (dlss_seen_ just went true, above) must be able to
    // switch Auto from Present to After DLSS right here too, ahead of UpdateSession below -- no present
    // is coming along to notice it otherwise, so NR would never run until presents resume.
    placement_ = ChoosePlacement(settings_.source, dlss_unavailable_reason_, dlss_seen_, BeforeUpscaling(),
                                 {.after_dlss = after_dlss_problem_, .before_upscaling = before_upscaling_problem_},
                                 DlssStagesOff(dlss_off_.Off(), core_hold_.Held()));
    // A placement change decided here needs the same bookkeeping BeginFrame gives
    // it, or the log stays silent and the work size keeps the old placement's until the next present.
    NotePlacementChange();
    UpdateChain();  // split chain: the chain follows pass 1's placement, with the latest present's back buffer
    // The same release BeginFrame gives a placement change, so the Present motion
    // copies do not linger past starvation switching Auto away from Present (or MotionVectors off).
    const bool present_group = GroupAt(chain_, Stage::PRESENT).has_value();
    if (!present_group || !settings_.motion_vectors) {
      present_source_->Pipeline().ReleaseMotionCopies();
      present_jitter_.Reset();
    }
    if (!present_group || !settings_.launchpad_motion) {
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
    return {.reason = nr::SkipReason::DEVICE_REMOVED};
  }
  NoteDlssFeature(frame.serial);
  const std::optional<bool> before_reset = std::exchange(before_reset_, std::nullopt);  // this evaluate's, from BeforeDlssEvaluate
  const ngx_hooks::MotionJitterOffset jitter = present_jitter_.Update(frame);
  // Copy the game's motion vectors on its list for the present that closes this frame: with Source = Present, and (split
  // chain) for a later group at Present.
  const auto copy_motion = [&]() -> sources::PipelineResult {
    ID3D12GraphicsCommandList* const list = host.NativeList();
    if (!settings_.motion_vectors || frame.motion_vectors == nullptr || session_->State() != nr::SessionState::ACTIVE || list == nullptr
        || !host.StateKnown() || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      return {.reason = nr::SkipReason::NOT_AFTER_DLSS};
    }
    const sources::PipelineResult copied = sources::RecordOnGameList(host, *timeline_, settings_.restore, [&] {
      return sources::PipelineResult{
          .recorded = present_source_->Pipeline().RecordMotionCopy(list, {.resource = frame.motion_vectors, .rect = frame.motion_region},
                                                                   frame.mv_scale_x * settings_.motion_scale_x,
                                                                   frame.mv_scale_y * settings_.motion_scale_y, facts_.dlss_motion_upside_down,
                                                                   jitter.x * static_cast<float>(frame.motion_region.width) * settings_.motion_scale_x,
                                                                   jitter.y * static_cast<float>(frame.motion_region.height) * settings_.motion_scale_y),
      };
    });
    if (copied.recorded) {
      last_game_list_recording_ = now;
    }
    return copied;
  };
  if (placement_.placement == Placement::PRESENT) {
    if (settings_.source != ui::PlacementSource::PRESENT) return {.reason = nr::SkipReason::NOT_AFTER_DLSS};
    return copy_motion();
  }
  // Pass 1 runs inside the game's frame here, so a Present group can only be a later one.
  sources::PipelineResult motion_copy = {.reason = nr::SkipReason::NOT_AFTER_DLSS};
  if (const std::optional<uint32_t> present = GroupAt(chain_, Stage::PRESENT); present && session_->SliceRuns(slices_[*present])) {
    motion_copy = copy_motion();  // only for a Present group that runs
  }
  const std::optional<uint32_t> group = GroupAt(chain_, Stage::AFTER_DLSS);
  if (!group || dlss_idle_) {
    descriptors_busy_since_.reset();
    return motion_copy;  // Before upscaling already ran in BeforeDlssEvaluate
  }
  const nr::Rect region = sources::ResolveOutputRegion(frame);
  const sources::WorkLayout layout = SettleLayout({region.width, region.height}, Stage::AFTER_DLSS, now);  // in every state
  // Only ACTIVE has anything to evaluate. LOADING, GRACE, DRAINING and
  // FAILED would otherwise reach AfterDlssSource::Run and report its misleading nr::SkipReason::LIST_STATE_UNKNOWN.
  if (session_->State() != nr::SessionState::ACTIVE) {
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].nr_applied = false;
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].skip_reason = nr::SkipReason::NONE;
    if (*group == 0u) {
      status_.NoteSkip(nr::SkipReason::NONE, RecordingStatus::Reset::APPLIED_AND_REASON);  // NR is off: nothing to explain, and the list's state is not tracked
    }
    descriptors_busy_since_.reset();
    return {.reason = nr::SkipReason::NR_OFF};
  }
  if (*group > 0u && !session_->SliceRuns(slices_[*group])) {
    // Split chain: a later group the budget cut to no pass (or the Defaults view hides) skips before its source copy; its next recording starts afresh.
    after_dlss_->OweReset();
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].nr_applied = false;
    stages_[static_cast<size_t>(Stage::AFTER_DLSS)].skip_reason = SliceSkipReason(settings_, slices_[*group]);
    return {.reason = SliceSkipReason(settings_, slices_[*group])};
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
    last_dlss_token_ = result.token;  // marked if the chain splits before its work is done
    if (chain_.Split()) {
      dlss_stage_token_ = std::max(dlss_stage_token_, result.token);
      timeline_->MarkStageToken(result.token);  // where its list goes decides whether Present may run beside it
    }
  }
  NoteResult(Stage::AFTER_DLSS, group, result);
  // A submit-only thread's tokens age out only after Timeline's own
  // AGED_SUBMISSION_TIME, so the ring can stay fully busy for a while there by design (see the ring
  // size in color_pipeline.hpp). One WARN per context once that has held for over a second running,
  // so an engine shape worse than the ring was sized for is diagnosable in the log.
  if (result.reason == nr::SkipReason::DESCRIPTORS_BUSY) {
    if (!descriptors_busy_since_) {
      descriptors_busy_since_ = now;
    } else if (!warned_descriptors_busy_ && now - *descriptors_busy_since_ > DESCRIPTORS_BUSY_WARN_AGE) {
      warned_descriptors_busy_ = true;
      nr::Logf(nr::LogLevel::WARN, uplift::text::DESCRIPTORS_BUSY_LINE, DESCRIPTORS_BUSY_WARN_AGE.count());
    }
  } else {
    descriptors_busy_since_.reset();
  }
  if (group == 0u) {
    frame_size_ = frame.snapshot.output;
    if (result.nr_applied && !logged_after_dlss_) {
      logged_after_dlss_ = true;  // once per activation, as the Present path logs its trigger
      nr::Log(nr::LogLevel::INFO, "NR trigger: after DLSS");
    }
  }
  if (trace_frames_ > 0u) {
    --trace_frames_;
    nr::Logf(nr::LogLevel::TRACE, "evaluate: NR {} after DLSS, {} pass(es) {}", (result.nr_applied ? "applied" : "skipped"),
             result.passes_run, nr::SkipReasonText(result.reason));
  }
  return result;
}

void DeviceContext::NoteResult(Stage stage, uint32_t group, const sources::PipelineResult& result) {
  StageState& state = stages_[static_cast<size_t>(stage)];
  state.nr_applied = result.nr_applied;
  state.passes_run = result.passes_run;
  state.skip_reason = (result.nr_applied ? nr::SkipReason::NONE : result.reason);
  state.motion_source = result.motion_source;
  if (group == 0u) {
    // The status and NR's heartbeat follow pass 1's group: one NR frame per frame.
    status_.From(result, RecordingStatus::Scale::RESULT);
    nr_recordings_ += (result.nr_applied ? 1u : 0u);
  }
}

bool DeviceContext::BeforeUpscaling() const {
  // A bridged context runs the DLSS stages only as the Direct3D 11 bridge's (dlss_stages_), which keeps DLSS-SR only.
  const ngx_hooks::NgxApi api = (bridged_ ? ngx_hooks::NgxApi::D3D11 : ngx_hooks::NgxApi::D3D12);
  return settings_.pre_upscale && PreSrFeatureAllowed(main_feature_, api);
}

ID3D12Resource* DeviceContext::PrepareMaskCopy(const D3D12_RESOURCE_DESC* mask) {
  const bool wanted = (!torn_down_ && !device_lost_ && settings_.look.mask && session_->State() == nr::SessionState::ACTIVE);
  return present_source_->Pipeline().PrepareMaskCopy(wanted ? mask : nullptr);
}

void DeviceContext::ApplyLookConfig(std::chrono::steady_clock::time_point now, uint32_t group) {
  const std::optional<float> frame_seconds = FrameSecondsSince(last_look_recordings_[group], now);
  const uint32_t shaped = ShapedGroup(chain_, [this](uint32_t index) { return session_->SliceRuns(slices_[index]); });
  present_source_->Pipeline().SetLookConfig(GroupLookConfig(settings_, chain_, group, shaped, frame_seconds));
}

void DeviceContext::UpdateChain() {
  // Why each stage cannot run now.
  const std::string_view dlss_cause = DlssCause(dlss_unavailable_reason_, dlss_seen_, DlssStagesOff(dlss_off_.Off(), core_hold_.Held()));
  ChainFacts facts = {
      .pass1 = placement_.placement,
      .passes = settings_.session.pass_count,
      .stored = settings_.pass_stages,
      .split_capable = facts_.split_capable,
  };
  // A stage's own problem: the Direct3D 11 bridge cannot share its image (NoteStageProblem).
  facts.cannot[static_cast<size_t>(Stage::BEFORE_UPSCALING)] = BeforeUpscalingCause(dlss_cause, before_upscaling_problem_, BeforeUpscaling());
  facts.cannot[static_cast<size_t>(Stage::AFTER_DLSS)] = AfterDlssCause(dlss_cause, after_dlss_problem_);
  // Observe the game's queues even while opted in, so disabling the experiment immediately restores the existing remap without waiting for a new recording.
  if (present_queue_ != nullptr && SplitStored(settings_, facts_.split_capable)) {
    ID3D12CommandQueue* const other = timeline_->StageQueueOtherThan(present_queue_);
    if (other != other_dlss_queue_) {
      if (other != nullptr) {
        nr::Logf(nr::LogLevel::INFO, "Direct3D 12: the game's DLSS runs on another queue ({}) than its presents ({}){}: {}",
                 static_cast<const void*>(other), static_cast<const void*>(present_queue_),
                 (frame_generation_.ReportedState(IsBeforeFg(facts_.fg_present_path)).active ? " with frame generation on" : ""),
                 (settings_.allow_cross_queue_passes ? "experimental cross-queue passes allowed" : "passes stored at Present run at the DLSS stage"));
      } else {
        nr::Logf(nr::LogLevel::INFO, "Direct3D 12: the presents ({}) are on the game's DLSS queue again: passes stored at Present run there again",
                 static_cast<const void*>(present_queue_));
      }
      other_dlss_queue_ = other;
    }
  }
  // The detector's evidence of frame generation: NGX's live features and the present cadence.
  const bool frame_generation_on = frame_generation_.ObservedState().active;
  std::string_view present_cause;
  if (!present_cannot_.empty()) {
    present_cause = present_cannot_;
  } else if (FrameGenerationKeepsLaterPresentOff(frame_generation_on, facts_.fg_present_path)) {
    present_cause = FRAME_GENERATION_REASON;
  } else if (!facts_.game_facing_present && !settings_.allow_cross_queue_passes && other_dlss_queue_ != nullptr) {
    present_cause = (frame_generation_on ? FRAME_GEN_QUEUE_REASON : OTHER_QUEUE_REASON);
  }
  facts.cannot[static_cast<size_t>(Stage::PRESENT)] = LaterPresentCause(facts_.fg_present_path, present_cause);
  ChainPlan chain = DecideChain(facts);

  LogChainChange(chain, chain_);
  const std::array<bool, STAGE_COUNT> reset = StagesToReset(chain, chain_);
  for (size_t stage = 0u; stage < STAGE_COUNT; ++stage) {
    if (reset[stage]) {
      stages_[stage] = {};
    }
  }
  if (chain.Split() && !chain_.Split() && last_dlss_token_ != 0u) {
    // The DLSS-stage recording made under the one-group chain (unmarked) may still run on another queue when a later Present group first
    // runs: marked now, it holds that group (StageWorkDone) until it is on the present queue or complete, and names its queue to the latch.
    timeline_->MarkStageToken(last_dlss_token_);
    dlss_stage_token_ = std::max(dlss_stage_token_, last_dlss_token_);
  }
  chain_ = std::move(chain);
  FillSlices(chain_, slices_);
  if (chain_.group_count > 0u) {
    session_->SetChain(std::span(slices_).first(chain_.group_count));
  }
  // A group the chain no longer has frees its set behind its last use; set 0 stays, as the single set always did.
  present_source_->Pipeline().RetireSetsFrom(std::max(chain_.group_count, 1u));
}

void DeviceContext::NotePlacementChange() {
  // `logged_placement_` starts unset, so the very first
  // placement either caller (BeginFrame, OnDlssEvaluate's present-starvation catch-up) decides is
  // always logged once, NONE (with its reason) included.
  if (!PlacementChanged(logged_placement_, placement_.placement)) return;
  last_dlss_diagnostic_.reset();
  nr::Log(nr::LogLevel::INFO, FormatPlacementLine(placement_, main_snapshot_));
  // A new placement's work size applies at once.
  if (const std::optional<Stage> stage = PlacementStage(placement_.placement)) {
    stages_[static_cast<size_t>(*stage)].settle.Reset();
  }
}

sources::WorkLayout DeviceContext::SettleLayout(nr::Size output, Stage stage, std::chrono::steady_clock::time_point now) {
  // Split chain: pass 1's group works at Setup's Resolution; a later group at its stage's own (Before upscaling is always pass 1's).
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

sources::AfterDlssConfig DeviceContext::DlssConfig(const sources::WorkLayout& layout, uint32_t group) const {
  return {
      .encoding = settings_.encoding,
      .diffuse_white_nits = settings_.diffuse_white_nits,
      .transfer_strength = settings_.transfer_strength,
      .color_strength = settings_.color_strength,
      .motion_vectors = facts_.motion_at_dlss_stage.value_or(settings_.motion_vectors),
      .motion_scale_x = settings_.motion_scale_x,
      .motion_scale_y = settings_.motion_scale_y,
      .depth_inverted = settings_.depth_inverted,
      .chained_history = settings_.chained_history,
      .restore = settings_.restore,
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
  const sources::WorkLayout layout = SettleLayout(frame_size_, Stage::BEFORE_UPSCALING, now);  // in every state
  if (session_->State() != nr::SessionState::ACTIVE) {
    status_.NoteSkip(nr::SkipReason::NONE, RecordingStatus::Reset::APPLIED_AND_REASON);
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
    last_dlss_token_ = result.token;  // as After DLSS's
    if (chain_.Split()) {
      dlss_stage_token_ = std::max(dlss_stage_token_, result.token);
      timeline_->MarkStageToken(result.token);  // as After DLSS's
    }
  }
  NoteResult(Stage::BEFORE_UPSCALING, 0u, result);  // always pass 1's group
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
  bridged_layouts_[0] = SettleLayout(frame_size_, Stage::BEFORE_UPSCALING, now);  // in every state
  if (session_->State() != nr::SessionState::ACTIVE) {
    status_.NoteSkip(nr::SkipReason::NONE, RecordingStatus::Reset::APPLIED_AND_REASON);
    return {};
  }
  if (controls_.intensity <= 0.f) {
    NoteBridgedSkip(BridgedDlssWork::BEFORE_UPSCALING, nr::SkipReason::INTENSITY_ZERO);  // exact pass-through: nothing crosses, DLSS reads the game's own Color
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
    // Source = Present copies DLSS's vectors for the present that closes this frame, as on Direct3D 12, into the bridge's ring.
    if (settings_.source != ui::PlacementSource::PRESENT || !settings_.motion_vectors || frame.motion_vectors == nullptr
        || session_->State() != nr::SessionState::ACTIVE) {
      return {};
    }
    return {.work = BridgedDlssWork::PRESENT_MOTION,
            .motion_scale_x = frame.mv_scale_x * settings_.motion_scale_x,
            .motion_scale_y = frame.mv_scale_y * settings_.motion_scale_y};
  }
  // Split chain: pass 1 runs inside the game's frame here, so a Present group is a later one: DLSS's vectors go into the ring for it, as Direct3D 12
  // native copies them (only for a Present group that runs), and the add-on copies them before After DLSS's hand-off.
  BridgedDlssDecision decision;
  if (const std::optional<uint32_t> present = GroupAt(chain_, Stage::PRESENT);
      present && settings_.motion_vectors && frame.motion_vectors != nullptr && session_->State() == nr::SessionState::ACTIVE
      && session_->SliceRuns(slices_[*present])) {
    decision.present_motion = true;
    decision.motion_scale_x = frame.mv_scale_x * settings_.motion_scale_x;
    decision.motion_scale_y = frame.mv_scale_y * settings_.motion_scale_y;
  }
  const std::optional<uint32_t> group = GroupAt(chain_, Stage::AFTER_DLSS);
  if (!group || dlss_idle_) {
    descriptors_busy_since_.reset();
    before_reset_.reset();
    return decision;
  }
  bridged_layouts_[1] = SettleLayout(output_region, Stage::AFTER_DLSS, now);  // in every state
  StageState& stage = stages_[static_cast<size_t>(Stage::AFTER_DLSS)];
  if (session_->State() != nr::SessionState::ACTIVE) {
    stage.nr_applied = false;
    stage.skip_reason = nr::SkipReason::NONE;
    if (*group == 0u) {
      status_.NoteSkip(nr::SkipReason::NONE, RecordingStatus::Reset::APPLIED_AND_REASON);
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
    stage.skip_reason = SliceSkipReason(settings_, slices_[*group]);
    before_reset_.reset();
    return decision;
  }
  const nr::SkipReason skip = (!nr::MeetsNrFloor(output_region) ? nr::SkipReason::FRAME_TOO_SMALL
                               : controls_.intensity <= 0.f     ? nr::SkipReason::INTENSITY_ZERO
                                                                : nr::SkipReason::NONE);
  if (skip != nr::SkipReason::NONE) {
    NoteBridgedSkip(BridgedDlssWork::AFTER_DLSS, skip);  // as AfterDlssSource::Run says it, and owes it; nothing crosses
    before_reset_.reset();
    return decision;
  }
  decision.work = BridgedDlssWork::AFTER_DLSS;
  return decision;
}

sources::PipelineResult DeviceContext::RecordBridgedStage(BridgedDlssWork work, const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                                          std::chrono::steady_clock::time_point now) {
  // Split chain: After DLSS records its own group (pass 1's, or a later one behind Before upscaling, which then takes the reset that group's
  // recording filtered in this evaluate); Before upscaling is always pass 1's.
  switch (work) {
    case BridgedDlssWork::AFTER_DLSS:
      return RecordAfterDlss(frame, host, bridged_layouts_[1], now, GroupAt(chain_, Stage::AFTER_DLSS).value_or(0u), std::exchange(before_reset_, std::nullopt));
    case BridgedDlssWork::BEFORE_UPSCALING: return RecordBeforeUpscaling(frame, host, bridged_layouts_[0], now);
    case BridgedDlssWork::NONE:
    case BridgedDlssWork::PRESENT_MOTION:   break;
  }
  return {.reason = nr::SkipReason::NOT_AFTER_DLSS};
}

bool DeviceContext::PresentRingWanted() const {
  if (placement_.placement == Placement::PRESENT) return facts_.present_motion_copy;
  return (settings_.motion_vectors && GroupAt(chain_, Stage::PRESENT).has_value());
}

void DeviceContext::NoteStageProblem(BridgedDlssWork stage, std::string problem) {
  std::string& slot = (stage == BridgedDlssWork::BEFORE_UPSCALING ? before_upscaling_problem_ : after_dlss_problem_);
  if (problem.empty() || slot == problem) return;
  nr::Logf(nr::LogLevel::WARN, "{} cannot run here: {}; {}", (stage == BridgedDlssWork::BEFORE_UPSCALING ? "NR before upscaling" : "NR after DLSS"),
           problem, (stage == BridgedDlssWork::BEFORE_UPSCALING ? "NR runs after DLSS instead" : "Auto runs at Present"));
  slot = std::move(problem);
}

void DeviceContext::NoteBridgedSkip(BridgedDlssWork stage, nr::SkipReason reason) {
  if (stage == BridgedDlssWork::BEFORE_UPSCALING) {
    pre_sr_->OweReset();
  } else {
    after_dlss_->OweReset();
  }
  // Split chain: the stage's own group reports it (Status's groups and the card's pass count); the status above them follows pass 1's group only.
  const Stage chain_stage = (stage == BridgedDlssWork::BEFORE_UPSCALING ? Stage::BEFORE_UPSCALING : Stage::AFTER_DLSS);
  StageState& state = stages_[static_cast<size_t>(chain_stage)];
  state.nr_applied = false;
  state.passes_run = 0u;
  state.skip_reason = reason;
  state.motion_source = sources::MotionSource::NONE;
  if (GroupAt(chain_, chain_stage).value_or(0u) != 0u) return;
  status_.NoteSkip(reason, RecordingStatus::Reset::ALL);  // the motion source as a skip from the sources' Run leaves it
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
  // BeforeDlssEvaluate already applied NR into the private colour and set
  // the status applied, on the assumption that the detour would swap it into the game's own Color. The
  // detour calls this only when that swap could not happen (the game's NGX block had no readable Color
  // to swap), so DLSS reads its own Color unchanged this evaluate: the status must say so, not "applied".
  if (placement_.placement != Placement::BEFORE_UPSCALING || !status_.nr_applied) return;
  status_.NoteSkip(nr::SkipReason::NO_COLOR_SWAP, RecordingStatus::Reset::APPLIED_AND_REASON);
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
  dropped_tokens_.fetch_add(tokens.size());
  timeline_->Drop(tokens);
}

void DeviceContext::StampThread(uint32_t thread_id, bool later_call) {
  if (!device_lost_) {
    const uint64_t before = timeline_->ThreadStamps();
    timeline_->StampThread(thread_id);
    if (later_call) {
      later_call_stamps_.fetch_add(timeline_->ThreadStamps() - before);
    }
  }
}

void DeviceContext::UpdateSession(std::chrono::steady_clock::time_point now) {
  const bool dlss_placement =
      (placement_.placement == Placement::AFTER_DLSS || placement_.placement == Placement::BEFORE_UPSCALING);
  const bool was_idle = dlss_idle_;
  dlss_idle_ = (dlss_placement && last_main_evaluate_.has_value()
                && now - *last_main_evaluate_ > std::max(2 * settings_.session.grace, DLSS_IDLE_MINIMUM));
  dlss_paused_ = (last_main_evaluate_.has_value() && now - *last_main_evaluate_ > DLSS_IDLE_MINIMUM);
  // Split chain: NR is wanted while some group can run. A later Present group runs on while the game's DLSS idles (a menu), so the DLSS idle
  // drain waits for none then; the DLSS groups skip those frames and restart their history when DLSS returns.
  const bool later_present = (dlss_placement && GroupAt(chain_, Stage::PRESENT).has_value());
  if (chain_.Split() && dlss_paused_) {
    pre_sr_->OweReset();
    after_dlss_->OweReset();
    // The card counts the passes that run; the DLSS groups run none while DLSS is paused.
    for (const Stage stage : {Stage::BEFORE_UPSCALING, Stage::AFTER_DLSS}) {
      stages_[static_cast<size_t>(stage)].nr_applied = false;
      stages_[static_cast<size_t>(stage)].passes_run = 0u;
    }
  }
  const bool placement_ready =
      (dlss_placement ? (!dlss_idle_ || later_present) : (placement_.placement == Placement::PRESENT && encoding_.has_value()));
  // Never while held after the game's NGX shutdown (no NGX call on this device until its DLSS starts again), at any placement.
  const bool want = (settings_.session.enabled && facts_.nr_allowed && blocked_reason_.empty() && placement_ready && !core_hold_.Held());
  if (nr::IsLogEnabled(nr::LogLevel::TRACE) && settings_.session.enabled
      && (!last_dlss_diagnostic_ || now - *last_dlss_diagnostic_ >= std::chrono::seconds(5) || was_idle != dlss_idle_)) {
    last_dlss_diagnostic_ = now;
    const int64_t age = (last_main_evaluate_ ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_main_evaluate_).count() : -1);
    nr::Logf(nr::LogLevel::TRACE,
             "DLSS diagnostic context {}: {}; accepted main calls {}; last main age {} ms (-1 means none); idle threshold {} ms, idle {}, paused {}; enabled {}, allowed {}, blocked [{}], held {}, abandoned {}, placement ready {}, want NR {}, session {}",
             static_cast<const void*>(device_.Get()), FormatPlacementLine(placement_, main_snapshot_), main_evaluates_, age,
             std::max(2 * settings_.session.grace, DLSS_IDLE_MINIMUM).count(), dlss_idle_, dlss_paused_, settings_.session.enabled,
             facts_.nr_allowed, blocked_reason_, core_hold_.Held(), core_hold_.Abandoned(), placement_ready, want, nr::SessionStateName(session_->State()));
    nr::Logf(nr::LogLevel::TRACE, TOKENS_DIAGNOSTIC_FORMAT, timeline_->ThreadStamps(), later_call_stamps_.load(), timeline_->AgedStamps(),
             dropped_tokens_.load(), timeline_->OutstandingTokens());
  }
  // An OFF Session can still be suspended with its enable request retained. A real disable must reach SetEnabled(false) to cancel that request,
  // and toggling off then on must permit an explicit resume. Only an idle-only disable on an already-suspended Session is suppressed: forwarding
  // it would erase the suspension and let the next enable bypass its resume hold (session_test.cpp's "disabling while suspended cancels
  // auto-resume"). DRAINING alone is not suspended.
  // The core-shutdown hold always takes priority so no suspension can auto-resume NR before the game's DLSS returns.
  const bool idle_only =
      (settings_.session.enabled && facts_.nr_allowed && blocked_reason_.empty() && dlss_placement && dlss_idle_ && !later_present && !core_hold_.Held());
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
  // A bridged context's device is Uplift's private one: its removal gets Retry now, never the DLSS latch (NoteGameDeviceRemoved decides that for
  // the game's device).
  dlss_latch_tripped_ = (!bridged_ && last_game_list_recording_.has_value() && now - *last_game_list_recording_ <= DLSS_LATCH_WINDOW);
  output_message_ = (dlss_latch_tripped_
                         ? std::format("Device removed ({:#010x}) soon after NR ran inside the game's frame: the DLSS "
                                       "placements stay off from the next start (Clear latch to try again)",
                                       static_cast<uint32_t>(removed))
                         : std::format("Device removed ({:#010x}): NR stays off on this device", static_cast<uint32_t>(removed)));
  nr::Log(nr::LogLevel::ERR, output_message_);
  if (bridged_) {
    LogDred(device_.Get());  // Uplift's private device (a bridge's, or the helper's): what was in flight, and any page fault
  }
  return true;
}

void DeviceContext::Teardown() {
  if (torn_down_) return;
  torn_down_ = true;
  frame_ready_ = false;
  // A removal first observed at teardown must decide the latch before abandoning. Once device_lost_ is set, the original decision is kept;
  // recomputing against teardown's later time could miss the recording window.
  if (!device_lost_) {
    NoteDeviceRemoved(std::chrono::steady_clock::now());
  }
  if (device_lost_) {
    // Never call into the snippet after a removal. OnDeviceLost drops every NR
    // object without an NGX call and restores the snippet's IAT and log hooks (Snippet::Abandon),
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
    // The Session was abandoned at the game's NGX shutdown, so its own flush below does nothing: what waits for the GPU goes here (the game idled it).
    timeline_->Flush(std::chrono::seconds(2));
  }
  session_->OnDeviceDestroyed();  // flushes the timeline and every queue's tokens (Session's TEARDOWN_WAIT cap), then unloads; after the game's NGX shutdown, nothing
                                  // of NGX's is left to release here (OnCoreShutdown did it, or NR was off), so this makes no NGX call
  // NR is unloaded and the device still held (a bridge releases its private device after this): a core Uplift initialised on it is shut down now,
  // before the device goes. A removed device returned above, with no NGX call.
  nr::Snippet::ShutdownD3D12Core(device_.Get());
}

void DeviceContext::OnCoreShutdown(std::chrono::milliseconds cap, uint64_t upscaler_creates, uint64_t serial) {
  if (bridged_ || torn_down_ || device_lost_) return;
  const auto now = std::chrono::steady_clock::now();
  if (NoteDeviceRemoved(now)) return;  // a removed device: never an NGX call (Teardown abandons the runtime)
  if (core_hold_.Abandoned()) return;  // a shutdown after an abandon: nothing of NGX's is held any more
  core_hold_.Hold(upscaler_creates, serial);
  frame_ready_ = false;  // nothing is recorded on this device until the hold ends
  status_.ClearKeepScale();
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
      && !GroupAt(chain_, Stage::PRESENT).has_value()) {
    placement_note = (status.session.state == nr::SessionState::OFF ? "Waiting for DLSS: NR memory released"
                                                                    : "Waiting for DLSS: releasing NR memory");
  }
  // After the game's NGX shutdown NR waits for the game's DLSS (temporary), or, abandoned there, stays off for the session (an output problem: no
  // BeginFrame clears it, unlike the target's own).
  // The game's own Direct3D 11 device removed comes first after the abandon; BeginFrame rebuilds output_message_.
  const std::string_view output_problem = (core_hold_.Abandoned()           ? ui::D3D12_NGX_ABANDONED_REASON
                                           : !game_device_removed_.empty() ? std::string_view(game_device_removed_)
                                                                           : std::string_view(output_message_));
  if (core_hold_.Held() && !core_hold_.Abandoned()) {
    status.held = ui::D3D12_NGX_SHUT_DOWN_REASON;
    placement_note = std::string(ui::D3D12_NGX_SHUT_DOWN_REASON);
  }
  status.abandoned = core_hold_.Abandoned();
  status.after_dlss_problem = after_dlss_problem_;  // Setup greys a stage the Direct3D 11 bridge cannot share
  status.before_upscaling_problem = before_upscaling_problem_;
  const bool claimed_elsewhere = (!facts_.nr_allowed && settings_.session.enabled);
  status.message = ContextMessage({
      .blocked = (blocked_reason_.empty() && claimed_elsewhere ? std::string_view("NR runs on another D3D12 device in this game")
                                                               : std::string_view(blocked_reason_)),
      .output = output_problem,
      .placement = placement_note,
      .skip_reason = status_.skip_reason,
      .skip_from_session = status_.skip_from_session,
      .session_message = status.session.message,
  });
  status.trigger = (placement_.placement == Placement::PRESENT ? trigger_used_ : TriggerPoint::NONE);
  status.nr_applied = status_.nr_applied;
  status.passes_run = status_.passes_run;
  if (chain_.Split()) status.passes_run = stages_.SplitPassesRun(chain_);
  status.frame = frame_size_;
  status.encoding = encoding_;
  status.diffuse_white_nits = diffuse_white_nits_;
  status.intermediate_bytes = present_source_->HeldBytes();
  status.keep_faces_supported = present_source_->Pipeline().KeepFacesSupported();
  status.device_lost = device_lost_;
  status.placement = placement_.placement;
  status.placement_line = FormatPlacementLine(placement_, main_snapshot_);
  status.frame_generation = frame_generation_.ReportedState(IsBeforeFg(facts_.fg_present_path));
  status.dlss_seen = dlss_seen_;
  status.dlss_latch_tripped = dlss_latch_tripped_;
  status.work = work_size_;
  status.canvas = canvas_size_;
  // The pieces of `message`, flattened for the status card ladder.
  status.blocked = blocked_reason_;
  status.claimed_elsewhere = claimed_elsewhere;
  status.output_problem = output_problem;
  status.placement_note = placement_note;
  status.skip_reason = status_.skip_reason;
  status.skip_from_session = status_.skip_from_session;
  // The facts Setup and the working card read.
  status.game_passes_motion = !last_motion_vectors_missing_;
  if (dlss_seen_ && last_motion_vectors_missing_) {
    status.dlss_motion_gap = ui::MotionGap::GAME_PASSED_NONE;
  } else if (placement_.placement == Placement::PRESENT && dlss_seen_ && settings_.motion_vectors && !settings_.motion_vectors_want_launchpad
             && status_.motion_source != sources::MotionSource::PRESENT_COPY) {
    // DLSS passes vectors, but this present's copy did not land. Launchpad covering for it (Auto) counts too, so Setup never reads
    // it as DLSS not having started. The game's DLSS paused (a menu, a loading screen) is not the GPU running behind.
    status.dlss_motion_gap = (dlss_paused_ ? ui::MotionGap::DLSS_PAUSED : ui::MotionGap::NO_COPY);
  }
  if (placement_.placement == Placement::PRESENT && settings_.launchpad_motion && status_.motion_source == sources::MotionSource::NONE) {
    status.launchpad_gap = ui::MotionGap::NO_UPLIFT_MV;
  }
  // While the game's DLSS is off, Match game runs at Full too.
  const bool match_game_unread = (settings_.resolution == ui::ResolutionMode::MATCH_GAME && (!main_snapshot_ || dlss_off_.Off()));
  status.resolution_applied = ((match_game_unread && placement_.placement != Placement::BEFORE_UPSCALING) ? ui::ResolutionMode::FULL : settings_.resolution);
  status.upsampling = settings_.upsampling;
  if (!work_size_.Empty()) {
    std::string detail;
    if (canvas_size_ != work_size_) {
      detail = std::format(" (a {}x{} render, padded)", work_size_.width, work_size_.height);
    } else if (work_size_ != frame_size_ && !frame_size_.Empty()) {
      detail = std::format(" of {}x{} ({})", frame_size_.width, frame_size_.height,
                           (settings_.upsampling == color::Upsampling::CLASSIC ? "Classic" : "Edge-aware"));
    }
    if (match_game_unread) {
      detail += (dlss_off_.Off() ? " (Match game: the game's DLSS is off)" : " (Match game: waiting for DLSS)");
    }
    status.work_line = std::format("Working at {}x{}{}", canvas_size_.width, canvas_size_.height, detail);
  }
  if (status_.nr_applied) {
    status.exposure_line = sources::ExposureLine(present_source_->Pipeline().LastExposure());
  }
  stages_.WriteChain(chain_, status);
  // The user's motion-vector-indicator request: NrPipeline's mechanical report (the status's motion source, and the
  // scale for DIRECT) plus the reason for NONE, which only DeviceContext has enough context to say.
  status.motion_source = status_.motion_source;
  switch (status_.motion_source) {
    case sources::MotionSource::DIRECT: {
      // MotionVectors = Launchpad, but placement runs inside the game's frame, where Launchpad's
      // UPLIFT_MV cannot bind -- this frame's vectors are the game's own DLSS ones, and the readout says why.
      const bool launchpad_would_run = (settings_.motion_vectors_want_launchpad && placement_.placement != Placement::PRESENT);
      status.motion_line = (launchpad_would_run
                                ? std::format("Motion vectors: DLSS ({} feeds the presented image only)", UpliftMvName(facts_))
                                : std::format("Motion vectors: DLSS (the game's own, scale {} x {})", status_.motion_scale_x,
                                              status_.motion_scale_y));
      break;
    }
    case sources::MotionSource::PRESENT_COPY:
      status.motion_line = "Motion vectors: DLSS (copied for Present)";
      break;
    case sources::MotionSource::LAUNCHPAD:
      status.motion_line =
          std::format("Motion vectors: {} (UPLIFT_MV, scale {} x {})", UpliftMvName(facts_), settings_.motion_scale_x, settings_.motion_scale_y);
      break;
    case sources::MotionSource::NONE: {
      std::string_view reason = "the game passed no motion vectors";
      if (!settings_.motion_vectors && !settings_.launchpad_motion) {
        reason = "set to Off";
      } else if (!settings_.motion_vectors) {
        reason = (placement_.placement == Placement::PRESENT ? NoUpliftMvReason(facts_)
                                                              : (facts_.uplift_mv_lumenite ? "Lumenite works on the presented image only"
                                                                                            : "Launchpad works on the presented image only"));
      } else if (placement_.placement == Placement::PRESENT) {
        if (bridged_ && (facts_.vulkan || dlss_stages_)) {
          // On Vulkan DLSS's vectors reach the Present path through the native context's copies, while the add-on asks for them. Direct3D
          // 11's ring is the same copy for the Present path's reasons.
          reason = (facts_.present_motion_copy ? "no copy of DLSS's vectors this frame"
                    : settings_.launchpad_motion  ? NoUpliftMvReason(facts_)
                                                : "DLSS's vectors do not reach the Present path on this device");
        } else if (bridged_) {
          // A bridged context never sees the game's DLSS, so only Launchpad can feed it.
          reason = (settings_.launchpad_motion ? NoUpliftMvReason(facts_)
                                             : "DLSS vectors need a 64-bit Direct3D 11, Direct3D 12 or Vulkan game");
        } else if (!dlss_seen_) {
          // Auto is only ever transiently on Present while it waits for DLSS to show up; Source = Present
          // forced has nothing to wait for, so it reads as a plain description instead.
          reason = (settings_.source == ui::PlacementSource::PRESENT ? "Present without DLSS" : "no DLSS running yet");
        } else if (!last_motion_vectors_missing_) {
          // DLSS is running and passes motion vectors -- this present's own copy
          // simply did not land -- so this must not read as "Present without DLSS" (no DLSS at all).
          reason = "no copy this frame";
        }
      }
      status.motion_line = std::format("Motion vectors: none ({})", reason);
      break;
    }
  }
  if (settings_.pre_upscale && placement_.placement == Placement::AFTER_DLSS && !BeforeUpscaling()) {
    status.placement_line += " (Ray Reconstruction keeps NR after DLSS)";  // the status says so
  }
  if (placement_.placement == Placement::BEFORE_UPSCALING && main_feature_ == NVSDK_NGX_Feature_RayReconstruction) {
    status.placement_line += " (Ray Reconstruction's input)";  // NR runs before RR denoises and upscales
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
