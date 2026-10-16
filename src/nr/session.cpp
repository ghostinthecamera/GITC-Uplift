#include "nr/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <utility>

#include "nr/log.hpp"

namespace uplift::nr {
namespace {

constexpr uint32_t MAX_CONSECUTIVE_FAILURES = 3u;
constexpr double SHRINK_FRACTION = 0.6;
constexpr auto SHRINK_HOLD = std::chrono::seconds(2);
constexpr auto SAMPLE_PERIOD = std::chrono::seconds(1);
constexpr auto TEARDOWN_WAIT = std::chrono::milliseconds(2000);
// Plan 6 (v2 design §3.18, D11): the automatic retries after a failure; then none until a change or Retry now.
constexpr std::array<std::chrono::milliseconds, 6> RETRY_DELAYS = {
    std::chrono::seconds(1),
    std::chrono::seconds(2),
    std::chrono::seconds(4),
    std::chrono::seconds(8),
    std::chrono::seconds(16),
    std::chrono::seconds(30),
};
// ACTIVE this long counts as a recovery: the next failure starts the backoff over.
constexpr auto RETRY_RECOVERED = std::chrono::seconds(30);

bool Fits(Size frame, Size capacity) {
  return frame.width <= capacity.width && frame.height <= capacity.height;
}

std::string Gigabytes(uint64_t bytes) {
  return std::format("{:.1f} GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
}

std::string_view ReasonFor(SessionState state) {
  switch (state) {
    case SessionState::OFF:      return "off";
    case SessionState::LOADING:  return "loading";
    case SessionState::GRACE:    return "grace";
    case SessionState::DRAINING: return "draining";
    case SessionState::FAILED:   return "failed";
    case SessionState::ACTIVE:   return "";
  }
  return "";
}

}  // namespace

Session::Session(Host& host, Timeline& timeline, SessionConfig config)
    : host_(host), timeline_(timeline), config_(config), budget_(config.budget) {}

Session::~Session() {
  if (runtime_loaded_ || LiveFeatureCount() > 0u || !retired_.empty()) {
    Log(LogLevel::ERR, "Session destroyed before OnDeviceDestroyed(); tearing down synchronously");
    OnDeviceDestroyed();
  }
}

void Session::SetEnabled(bool enabled) {
  if (device_lost_) return;         // permanently inert: never load the runtime again
  if (enabled == enabled_) return;  // a repeat must not cancel a suspension or skip the resume hold
  enabled_ = enabled;
  if (enabled) {
    if (state_ == SessionState::OFF) {
      BeginLoading();
    } else if ((state_ == SessionState::GRACE || state_ == SessionState::DRAINING) && runtime_loaded_
               && !draining_from_failure_) {
      // A FAILED session's own DRAINING (below) never gets this shortcut: its
      // features are the ones that failed and must never be resumed silently.
      teardown_pending_ = false;
      suspended_ = false;
      message_.clear();
      ResetAllStages();
      // Time spent in GRACE or DRAINING is not sampled, so any resume hold a
      // prior drop was accumulating must not silently count that elapsed
      // wall-clock time as confirmed headroom once ACTIVE sampling resumes.
      budget_.ResetYield();
      state_ = SessionState::ACTIVE;
    }
    return;
  }
  // T10-B (fix round 3): a real disable always clears a suspension, in every state -- not only OFF,
  // whose own case used to do this alone and left `message_` behind ("Suspended: game needs VRAM"
  // stale on the overlay), and not skipping DRAINING, which fell through to `default: break` and kept
  // both flags. The state-specific transitions below are unaffected: they only ever run once this is settled.
  if (suspended_) {
    suspended_ = false;
    message_.clear();
  }
  switch (state_) {
    case SessionState::LOADING: state_ = SessionState::OFF; break;
    case SessionState::ACTIVE:
      // Helper hand-over round: no grace (an owner hand-over) drains at once, never through a GRACE that ends in the same frame.
      if (config_.grace.count() == 0) {
        BeginTeardown(SessionState::DRAINING);
      } else {
        EnterGrace();
      }
      break;
    case SessionState::FAILED:
      draining_from_failure_ = teardown_pending_;
      state_ = (teardown_pending_ ? SessionState::DRAINING : SessionState::OFF);
      break;
    default: break;
  }
}

void Session::SetPassCount(uint32_t passes) {
  passes = std::clamp(passes, 1u, MAX_PASSES);
  if (passes == pass_count_) return;
  pass_count_ = passes;
  passes_limit_ = MAX_PASSES;
  // No reset: a pass that keeps running keeps its input and its history. A pass that starts running
  // (a new feature, or a kept idle one) restarts alone, through Evaluate's index >= evaluated_end.
  if (state_ == SessionState::FAILED && enabled_ && !teardown_pending_) {
    BeginLoading();
  }
}

void Session::SetCreateOptions(uint32_t preset, uint32_t performance) {
  preset_ = preset;
  performance_ = performance;
}

void Session::SetChain(std::span<const StageSlice> groups) {
  std::array<StageSlice, SESSION_STAGES> chosen = {};
  uint32_t chosen_count = 0u;
  for (const StageSlice& group : groups) {
    if (chosen_count == SESSION_STAGES) break;
    if (group.stage >= SESSION_STAGES || group.first >= MAX_PASSES) continue;
    chosen[chosen_count] = {.stage = group.stage, .first = group.first, .count = std::min(group.count, MAX_PASSES - group.first)};
    ++chosen_count;
  }
  if (chosen_count == 0u) return;
  const auto uses_stage = [](std::span<const StageSlice> chain, uint8_t stage) {
    return std::ranges::any_of(chain, [stage](const StageSlice& group) { return group.stage == stage; });
  };
  const uint8_t old_first_stage = FirstStage();
  const uint8_t new_first_stage = chosen[0].stage;
  if (new_first_stage != old_first_stage && !uses_stage(std::span(chosen).first(chosen_count), old_first_stage)
      && !uses_stage(std::span(groups_).first(group_count_), new_first_stage)) {
    // Pass 1's group moved to a stage the chain did not use (a placement switch): its state moves with it, its sizes, its history marks, Keep faces'
    // twin and its pause, as the single chain's always did across a switch.
    std::swap(stages_[old_first_stage], stages_[new_first_stage]);
  }
  groups_ = chosen;
  group_count_ = chosen_count;
  for (uint8_t stage = 0u; stage < SESSION_STAGES; ++stage) {
    if (!uses_stage(std::span(groups_).first(group_count_), stage)) {
      RetireFaceFeature(&stages_[stage]);  // a stage that left the chain: its twin goes alone, at its last use, as when Keep faces is turned off there
    }
  }
  for (const StageSlice& group : std::span(groups_).first(group_count_)) {
    for (uint32_t index = group.first; index < group.first + group.count; ++index) {
      if (pass_stages_[index] != group.stage) {
        // Design 3.2: a pass at another stage starts its history afresh there; its feature stays while it fits that stage's size.
        pass_stages_[index] = group.stage;
        stage_reset_owed_[index] = true;
      }
    }
  }
}

bool Session::SliceRuns(const StageSlice& slice) const {
  uint32_t end = std::min(pass_count_, passes_limit_);
  if (pass_view_limit_ != 0u) {
    end = std::min(end, pass_view_limit_);
  }
  return slice.count > 0u && slice.first < end;
}

void Session::NoteFacesWanted(bool wanted) {
  if (wanted) return;
  for (StageState& stage : stages_) {
    stage.faces_pause = FacesPause::NONE;
    stage.faces_requested = false;
  }
}

void Session::NoteFacesWanted(uint8_t stage, bool wanted) {
  if (wanted || stage >= SESSION_STAGES) return;
  stages_[stage].faces_pause = FacesPause::NONE;
  stages_[stage].faces_requested = false;
}

bool Session::FacesPaused() const {
  return FacesPaused(FirstStage());
}

bool Session::FacesPaused(uint8_t stage) const {
  return stage < SESSION_STAGES && stages_[stage].faces_pause != FacesPause::NONE;
}

void Session::RetryNow() {
  retry_attempts_ = 0u;
  retry_requested_ = (state_ == SessionState::FAILED && enabled_ && !device_lost_);
}

void Session::BeginLoading() {
  suspended_ = false;
  message_.clear();
  passes_limit_ = MAX_PASSES;
  for (StageState& stage : stages_) {
    stage.faces_pause = FacesPause::NONE;  // a load from OFF fits (and creates) the twin afresh
  }
  retry_requested_ = false;
  state_ = SessionState::LOADING;
}

void Session::Load() {
  const NVSDK_NGX_Result result = host_.LoadRuntime();
  if (NVSDK_NGX_FAILED(result)) {
    state_ = SessionState::FAILED;
    message_ = std::format("NR runtime failed to load: {:#010x}", static_cast<uint32_t>(result));
    Log(LogLevel::ERR, message_);
    failed_at_ = host_.Now();
    return;
  }
  runtime_loaded_ = true;
  create_failed_ = false;  // Plan 19: a load that works ends the creation failure
  consecutive_failures_ = 0u;
  draining_from_failure_ = false;  // a successful reload closes out any failure drain
  ResetAllStages();
  last_sample_.reset();
  budget_.ResetYield();
  state_ = SessionState::ACTIVE;
  Log(LogLevel::INFO, "NR runtime loaded");
}

void Session::EnterGrace() {
  state_ = SessionState::GRACE;
  grace_elapsed_ = std::chrono::steady_clock::duration::zero();
}

void Session::BeginTeardown(SessionState next) {
  state_ = next;
  teardown_pending_ = runtime_loaded_ || LiveFeatureCount() > 0u || !retired_.empty();
  teardown_mark_ = last_use_mark_;
  if (!teardown_pending_ && next == SessionState::DRAINING) {
    state_ = SessionState::OFF;
  }
}

void Session::FinishTeardown() {
  ReleaseAllFeaturesAndUnload();
  runtime_bytes_.reset();
  teardown_pending_ = false;
  if (state_ == SessionState::DRAINING) {
    // A failed teardown that is still wanted retries with a fresh Load; the
    // features that failed are already gone, so this is never a silent resume.
    if (draining_from_failure_ && enabled_) {
      BeginLoading();
    } else {
      state_ = SessionState::OFF;
    }
  }
  draining_from_failure_ = false;
}

void Session::ReleaseAllFeaturesAndUnload() {
  RetireAllFeatures();
  for (auto& retired : retired_) {
    retired.feature->Release();
  }
  retired_.clear();
  if (runtime_loaded_) {
    host_.UnloadRuntime();
    runtime_loaded_ = false;
    // Helper hand-over round: logged by every path that unloads the runtime (a drain, a device's destruction, a release before the game's NGX shutdown),
    // once per unload, so ReShade.log pairs each "NR runtime loaded" with it. A dropped runtime (device lost, abandoned) stays mapped: never logged so.
    Log(LogLevel::INFO, "NR runtime unloaded; NR memory released");
  }
}

void Session::EnterFailed(std::string message) {
  Log(LogLevel::ERR, message);
  message_ = std::move(message);
  failed_at_ = host_.Now();
  BeginTeardown(SessionState::FAILED);
}

void Session::Tick() {
  timeline_.Poll();
  const auto now = host_.Now();
  const std::chrono::steady_clock::duration elapsed = (last_tick_ ? now - *last_tick_ : std::chrono::steady_clock::duration::zero());
  last_tick_ = now;
  ReleaseDueRetired();
  switch (state_) {
    case SessionState::LOADING:
      Load();
      break;
    case SessionState::GRACE:
      grace_elapsed_ += elapsed;
      if (grace_elapsed_ >= config_.grace) {
        BeginTeardown(SessionState::DRAINING);
      }
      break;
    case SessionState::ACTIVE:
      SampleBudget(now);
      if (!active_since_) {
        active_since_ = now;
      } else if (now - *active_since_ >= RETRY_RECOVERED) {
        retry_attempts_ = 0u;  // recovered: a later failure starts the backoff over
      }
      break;
    case SessionState::FAILED: {
      // Plan 6 (D11): Retry now, or the next automatic retry, once the failed teardown has finished. Always through
      // BeginLoading, so a retry starts exactly like a fresh enable.
      const bool automatic_due = (config_.auto_retry && failed_at_.has_value() && retry_attempts_ < RETRY_DELAYS.size()
                                  && now - *failed_at_ >= RETRY_DELAYS[retry_attempts_]);
      if (enabled_ && !teardown_pending_ && (retry_requested_ || automatic_due)) {
        if (retry_requested_) {
          Log(LogLevel::INFO, "retrying NR (Retry now)");
        } else {
          ++retry_attempts_;
          Logf(LogLevel::INFO, "retrying NR (automatic retry {} of {})", retry_attempts_, RETRY_DELAYS.size());
        }
        BeginLoading();
      }
      break;
    }
    case SessionState::OFF:
      if (enabled_ && suspended_ && config_.auto_resume) {
        TryResume(now);
      }
      break;
    default:
      break;
  }
  if (state_ != SessionState::ACTIVE) {
    active_since_.reset();
  }
  if (teardown_pending_ && timeline_.IsComplete(teardown_mark_)) {
    FinishTeardown();
  }
}

void Session::SampleBudget(std::chrono::steady_clock::time_point now) {
  if (last_sample_ && now - *last_sample_ < SAMPLE_PERIOD) return;
  last_sample_ = now;
  runtime_bytes_ = host_.QueryRuntimeBytes();
  const uint32_t live = LiveFeatureCount();
  // Decision Q5a: the last stage's twin yields first, then the earlier stages' twins, before any pass.
  StageState* yielding_faces = nullptr;
  uint32_t faces_live = 0u;
  for (StageState& stage : stages_) {
    if (stage.face_feature) {
      yielding_faces = &stage;
      ++faces_live;
    }
  }
  const MemoryInfo memory = host_.QueryMemory();
  const YieldAction action = budget_.Sample(memory, live + faces_live);
  if (action != YieldAction::NONE && yielding_faces != nullptr) {
    // Keep faces yields first, before any pass drops or NR suspends. Pooled memory only returns when the last feature is released, so this too is a full
    // rebuild: the same passes, without the twin.
    yielding_faces->faces_pause = FacesPause::BUDGET;
    RetireAllFeatures();
    ResetAllStages();
    budget_.ResetYield();
    Log(LogLevel::WARN, "Budget: Keep faces paused, the game needs VRAM");
    return;
  }
  switch (action) {
    case YieldAction::DROP_PASS: {
      // Pooled memory only returns when the last feature is released, so a
      // pass drop is a full rebuild with one pass fewer. Split chain: the
      // last pass goes, so the last stage group shrinks first. Fix round 1
      // (M1): the last live one below the limit, wherever the live passes
      // are (pass 1's feature is missing while its stage pauses), so the
      // limit is its index, not the live count less one.
      uint32_t last_live = 0u;
      for (uint32_t index = std::min(passes_limit_, MAX_PASSES); index > 0u; --index) {
        if (features_[index - 1u]) {
          last_live = index - 1u;
          break;
        }
      }
      passes_limit_ = last_live;
      RetireAllFeatures();
      ResetAllStages();
      // Any headroom already held toward restoring a previous drop no longer
      // applies: the target it was measured against just changed.
      budget_.ResetYield();
      message_ = std::format("Budget: dropped to {} pass(es), the game needs VRAM", passes_limit_);
      Log(LogLevel::WARN, message_);
      break;
    }
    case YieldAction::SUSPEND:
      suspended_ = true;
      message_ = "Suspended: game needs VRAM";
      Log(LogLevel::WARN, message_);
      budget_.ResetYield();
      BeginTeardown(SessionState::DRAINING);
      break;
    case YieldAction::NONE:
      // While a drop still holds passes_limit_ below what was requested,
      // reuse the resume hold (§6.5) for the cost of one more feature: once
      // headroom for it holds for 10 continuous seconds, raise the limit by
      // one. EnsureFeatures adds the feature on the next Evaluate, with no
      // rebuild, and its own message logic takes over once desired changes.
      // Split chain: that pass costs a feature at its own stage's size.
      if (passes_limit_ < pass_count_) {
        const uint64_t need = budget_.FeatureBytes(stages_[pass_stages_[passes_limit_]].resume_size);
        if (budget_.ResumeReady(memory, need, now)) {
          ++passes_limit_;
          budget_.ResetYield();  // a further increment needs its own fresh 10 s
          Logf(LogLevel::INFO, "VRAM headroom restored; raising to {} of {} pass(es)", passes_limit_, pass_count_);
        }
        break;
      }
      // Keep faces comes back last, after every dropped pass, with the same hold for the cost of its one feature and its surfaces (fix round 1, M5: a
      // twin that did not fit waits for it too, so it never cycles at the budget's edge). Split chain: the first stage's twin first, the yield's reverse.
      for (const StageSlice& group : std::span(groups_).first(group_count_)) {
        StageState& stage = stages_[group.stage];
        if (stage.faces_pause != FacesPause::BUDGET && stage.faces_pause != FacesPause::UNFIT) continue;
        const uint64_t need = budget_.FeatureBytes(stage.resume_size) + stage.faces_surface_bytes + (first_use_paid_ ? 0u : budget_.FirstUseBytes());
        if (budget_.ResumeReady(memory, need, now)) {
          stage.faces_pause = FacesPause::NONE;
          budget_.ResetYield();
          Log(LogLevel::INFO, "VRAM headroom restored; Keep faces resumes");
        }
        break;
      }
      break;
  }
}

void Session::TryResume(std::chrono::steady_clock::time_point now) {
  if (last_sample_ && now - *last_sample_ < SAMPLE_PERIOD) return;
  last_sample_ = now;
  // Split chain: every group's passes at its stage's size, and the surfaces. Fix round 1 (M2): each stage notes the caller's whole surfaces (every stage
  // set), so they count once, as the largest any group noted.
  uint64_t need = 0u;
  uint64_t surfaces = 0u;
  for (const StageSlice& group : std::span(groups_).first(group_count_)) {
    const uint32_t end = std::min(group.first + group.count, pass_count_);
    if (end <= group.first) continue;
    const StageState& stage = stages_[group.stage];
    need += static_cast<uint64_t>(end - group.first) * budget_.FeatureBytes(stage.resume_size);
    surfaces = std::max(surfaces, stage.surface_bytes);
  }
  need += surfaces;
  if (!budget_.ResumeReady(host_.QueryMemory(), need, now)) return;
  BeginLoading();
  Log(LogLevel::INFO, "VRAM headroom restored; resuming NR");
}

void Session::RetireFeature(size_t index) {
  if (!features_[index]) return;
  retired_.push_back({last_use_mark_, std::move(features_[index])});
}

void Session::RetireFaceFeature(StageState* stage) {
  if (!stage->face_feature) return;
  retired_.push_back({last_use_mark_, std::move(stage->face_feature)});
  stage->faces_evaluated = false;
}

void Session::RetireAllFeatures() {
  for (StageState& stage : stages_) {
    RetireFaceFeature(&stage);
  }
  for (size_t index = features_.size(); index > 0u; --index) {
    RetireFeature(index - 1u);
  }
}

void Session::ReleaseDueRetired() {
  std::erase_if(retired_, [this](Retired& retired) {
    if (!timeline_.IsComplete(retired.mark)) return false;
    retired.feature->Release();
    return true;
  });
}

void Session::ResetAllStages() {
  for (StageState& stage : stages_) {
    stage.reset_pending = true;
  }
}

uint32_t Session::LiveFeatureCount() const {
  return static_cast<uint32_t>(std::ranges::count_if(features_, [](const auto& feature) { return feature != nullptr; }));
}

uint64_t Session::LiveFeatureBytes(const StageSlice& slice) const {
  const StageState& stage = stages_[slice.stage];
  uint64_t bytes = (stage.face_feature ? budget_.FeatureBytes(stage.face_feature->Capacity()) : 0u);
  for (uint32_t index = slice.first; index < slice.first + slice.count; ++index) {
    if (features_[index]) {
      bytes += budget_.FeatureBytes(features_[index]->Capacity());
    }
  }
  return bytes;
}

bool Session::EnsureFeatures(ID3D12GraphicsCommandList* list, const PassChain& chain, const StageSlice& slice, bool faces_wanted, bool faces_ready,
                             std::string_view* reason) {
  const auto now = host_.Now();
  StageState& stage = stages_[slice.stage];
  const uint32_t desired = std::min(pass_count_, passes_limit_);
  const uint32_t slice_end = slice.first + slice.count;
  const uint32_t wanted_end = std::max(slice.first, std::min(slice_end, desired));
  const auto live_between = [this](uint32_t begin, uint32_t end) {
    uint32_t live = 0u;
    for (uint32_t index = begin; index < end; ++index) {
      if (features_[index]) {
        ++live;
      }
    }
    return live;
  };
  // A pass-count decrease keeps its surplus features live and idle, as the Defaults view does: Evaluate runs the
  // first `desired`. Under the pool rule, releasing them frees nothing while pass 1 lives (E10). They go with the
  // next rebuild, yield or disable, all of which empty the pool.
  bool rebuild = false;
  const FeatureInterface* first_live = nullptr;
  for (uint32_t index = slice.first; index < slice_end; ++index) {
    if (!features_[index]) continue;
    if (first_live == nullptr) {
      first_live = features_[index].get();
    }
    if (!Fits(chain.size, features_[index]->Capacity())) {
      rebuild = true;
    }
  }
  if (stage.face_feature && !Fits(chain.size, stage.face_feature->Capacity())) {
    rebuild = true;
  }
  if (!rebuild && first_live != nullptr) {
    const Size capacity = first_live->Capacity();
    if (static_cast<double>(chain.size.Pixels()) <= SHRINK_FRACTION * static_cast<double>(capacity.Pixels())) {
      if (!stage.small_since) {
        stage.small_since = now;
      }
      rebuild = now - *stage.small_since >= SHRINK_HOLD;
    } else {
      stage.small_since.reset();
    }
  }
  if (rebuild) {
    // The snippet keeps feature memory in one pool that only empties when its
    // last feature is released (spike E10), so a rebuild retires everything
    // and recreates only once the pool is empty. Old and new never coexist.
    // Split chain (decision Q2): every stage's features, so every stage waits.
    RetireAllFeatures();
    for (StageState& each : stages_) {
      each.small_since.reset();
    }
    ResetAllStages();
  }
  if (!faces_wanted) {
    if (stage.face_feature) {
      // Keep faces turned off: the twin goes alone, at its last use. Under the pool rule this frees nothing while pass 1 lives, so the passes keep running
      // undisturbed instead of waiting for it.
      RetireFaceFeature(&stage);
      Log(LogLevel::INFO, "Keep faces: its extra NR run stopped");
    }
    // Off also ends a pause and a failure: turned on again, the twin is fitted (and created) afresh.
    stage.faces_pause = FacesPause::NONE;
  }
  // Only a rebuild (every feature retired at once) waits for the pool to empty; the twin retired alone leaves the passes live.
  if (LiveFeatureCount() == 0u && !retired_.empty()) {
    *reason = "resizing";
    return false;
  }
  // Fix round 1 (M4): without its surfaces this frame the twin is only skipped, never retired.
  const bool want_face = (faces_wanted && faces_ready && stage.faces_pause == FacesPause::NONE);
  const bool need_face = (want_face && !stage.face_feature);
  const uint32_t wanted = wanted_end - slice.first;
  const uint32_t live_before = live_between(slice.first, wanted_end);
  if (live_before >= wanted && !need_face) return true;

  // Design 3.2: an earlier group whose stage has not run yet (DLSS paused in a menu while a Present group runs) keeps the memory its passes need, at its
  // stage's last known size.
  uint64_t reserve_bytes = 0u;
  for (const StageSlice& group : std::span(groups_).first(group_count_)) {
    if (group.first >= slice.first) break;
    const uint64_t group_pass_bytes = budget_.FeatureBytes(stages_[group.stage].resume_size);
    for (uint32_t index = group.first; index < std::min(group.first + group.count, desired); ++index) {
      if (!features_[index]) {
        reserve_bytes += group_pass_bytes;
      }
    }
  }
  // The twin is fitted beside the passes, and yields first: it is made only when every wanted pass fits beside it.
  const FitResult fit = budget_.Fit(chain.size, wanted + (want_face ? 1u : 0u), chain.surface_bytes, host_.QueryMemory(),
                                    LiveFeatureBytes(slice) + chain.held_intermediate_bytes, !first_use_paid_, reserve_bytes);
  const bool face_fits = (want_face && fit.passes > wanted);
  const uint32_t target = std::max(std::min(fit.passes, wanted), live_before);
  if (target == 0u) {
    message_ = std::format("NR skipped: needs {}, {} free", Gigabytes(fit.need_bytes), Gigabytes(fit.available_bytes));
    *reason = "budget";
    return false;
  }
  const auto create = [&](std::unique_ptr<FeatureInterface>* created) -> NVSDK_NGX_Result {
    auto feature = host_.NewFeature();
    const auto before = host_.QueryRuntimeBytes();
    last_use_mark_ = timeline_.MarkNow();  // Create is recorded on this frame's list, win or lose
    const NVSDK_NGX_Result result = feature->Create(list, {.capacity = chain.size, .preset = preset_, .performance = performance_});
    if (NVSDK_NGX_FAILED(result)) return result;
    first_use_paid_ = true;
    const auto after = host_.QueryRuntimeBytes();
    if (before && after && *after > *before + budget_.StatsOvercountBytes()) {
      // Create-time stats exclude the 8 B/px capacity buffer a feature adds at its
      // first smaller frame (E6); keep charging it (spec §6.2).
      budget_.Calibrate(chain.size,
                        *after - *before - budget_.StatsOvercountBytes() + 8u * chain.size.Pixels());
    }
    *created = std::move(feature);
    return result;
  };
  uint32_t live = live_before;
  for (uint32_t index = slice.first; index < wanted_end && live < target; ++index) {
    if (features_[index]) continue;
    if (const NVSDK_NGX_Result result = create(&features_[index]); NVSDK_NGX_FAILED(result)) {
      create_failed_ = true;
      create_result_ = static_cast<uint32_t>(result);
      EnterFailed(std::format("NR feature creation failed: {:#010x}", static_cast<uint32_t>(result)));
      *reason = "create failed";
      return false;
    }
    ++live;
  }
  if (live_before == 0u) {
    stage.reset_pending = true;  // a new set starts every pass afresh; an appended pass resets alone (index >= evaluated_end)
  }
  if (need_face && face_fits) {
    // Fix round 1 (C2): a twin that cannot be created never turns NR off; the passes run on without it until Keep faces is toggled or NR reloads.
    if (const NVSDK_NGX_Result result = create(&stage.face_feature); NVSDK_NGX_FAILED(result)) {
      stage.faces_pause = FacesPause::FAILED_CREATE;
      Logf(LogLevel::ERR, "Keep faces stopped: its extra NR run could not start ({:#010x}); NR runs on without it", static_cast<uint32_t>(result));
    } else {
      Logf(LogLevel::INFO, "Keep faces: its extra NR run started (pass {} runs twice)", slice.first + 1u);
    }
  } else if (need_face) {
    stage.faces_pause = FacesPause::UNFIT;  // back after the resume hold (SampleBudget), not at the next frame that fits
    budget_.ResetYield();                   // fix round 2 (1): a fresh hold, never one an earlier pause had been accumulating
    Log(LogLevel::WARN, "Keep faces paused: no video memory for its extra NR run beside the passes");
  }
  // Split chain: the count names every live pass, whichever stage made it; the slice says whether the budget cut it.
  const uint32_t slice_requested = std::max(slice.first, std::min(slice_end, pass_count_)) - slice.first;
  const uint32_t running = live_between(0u, pass_count_);
  message_ = (target < slice_requested ? std::format("Budget: running {} of {} passes", running, pass_count_) : std::string());
  return true;
}

void Session::NoteFrameSize(Size size) {
  NoteFrameSize(FirstStage(), size);
}

void Session::NoteFrameSize(uint8_t stage_index, Size size) {
  if (stage_index >= SESSION_STAGES) return;
  StageState& stage = stages_[stage_index];
  if (!size.Empty()) {
    stage.resume_size = size;
  }
  if (state_ == SessionState::FAILED && enabled_ && !teardown_pending_ && !runtime_loaded_ && !size.Empty()) {
    // Retry only on a real change from a known size: an empty last_size means
    // no size has been recorded yet, not that the size just "changed".
    if (stage.last_size.Empty()) {
      stage.last_size = size;
    } else if (size != stage.last_size) {
      stage.last_size = size;
      BeginLoading();
    }
  }
}

EvaluateResult Session::Evaluate(ID3D12GraphicsCommandList* list, const PassChain& chain,
                                 const FrameInputs& inputs, const Controls& controls, StageSlice slice) {
  if (slice.stage >= SESSION_STAGES || slice.first >= MAX_PASSES) return {nullptr, 0u, "invalid input"};
  slice.count = std::min(slice.count, MAX_PASSES - slice.first);
  StageState& stage = stages_[slice.stage];
  NoteFrameSize(slice.stage, chain.size);
  stage.surface_bytes = chain.surface_bytes;
  if (state_ != SessionState::ACTIVE) return {nullptr, 0u, ReasonFor(state_)};
  if (list == nullptr || chain.a == nullptr || chain.b == nullptr || chain.size.Empty()) {
    return {nullptr, 0u, "invalid input"};
  }
  if (!MeetsNrFloor(chain.size)) {
    // Bail before any feature creation, rebuild or shrink check (EnsureFeatures below): the hang
    // this guards against was observed on a tiny frame that fit inside a larger, already-live
    // feature's capacity, so size alone must gate this, regardless of what is currently live.
    message_ = std::format("NR needs a frame of at least {}x{} (this frame is {}x{})", MIN_NR_LONG_SIDE,
                           MIN_NR_SHORT_SIDE, chain.size.width, chain.size.height);
    return {nullptr, 0u, "frame too small"};
  }
  stage.last_size = chain.size;
  if (!SliceRuns(slice)) {
    // Split chain: none of this slice's passes runs (the budget cut its group to 0, or the Defaults view shows fewer passes), so its stage skips. Its
    // passes did not run this frame: when they do again, each restarts its history.
    stage.evaluated_end = slice.first;
    const bool budget_cut = (slice.first < pass_count_ && slice.first >= passes_limit_);
    return {nullptr, 0u, (budget_cut ? std::string_view("budget") : std::string_view())};
  }

  // Keep faces needs the resolver, which recombines the twin's output with pass 1's.
  const bool faces_wanted = (inputs.faces.wanted && inputs.resolver != nullptr);
  stage.faces_requested = faces_wanted;
  stage.faces_surface_bytes = (faces_wanted ? inputs.faces.surface_bytes : 0u);
  std::string_view reason;
  if (!EnsureFeatures(list, chain, slice, faces_wanted, inputs.faces.output != nullptr, &reason)) return {nullptr, 0u, reason};
  const bool faces_run = (faces_wanted && inputs.faces.output != nullptr && stage.face_feature != nullptr && stage.faces_pause == FacesPause::NONE);
  bool faces_ran = false;

  ID3D12Resource* color = chain.a;
  ID3D12Resource* output = chain.b;
  const bool reset = stage.reset_pending || inputs.reset_hint;
  // Plan 7: a pass-count decrease keeps its surplus features idle; Evaluate runs only the first `desired`, exactly
  // as the Defaults view's pass_view_limit_ already narrows it, without a rebuild either. A pass that did not run
  // the previous frame starts its history afresh (index >= evaluated_end, below). Split chain: the slice's live passes,
  // in order, by their global index.
  const uint32_t desired = std::min(pass_count_, passes_limit_);
  const uint32_t slice_end = slice.first + slice.count;
  uint32_t run_end = slice.first;
  while (run_end < std::min(slice_end, desired) && features_[run_end]) {
    ++run_end;
  }
  if (pass_view_limit_ != 0u) {
    run_end = std::max(slice.first, std::min(run_end, pass_view_limit_));
  }
  // A slice whose first pass the budget left unmade (a later pass of it is live from an earlier chain) runs nothing.
  if (run_end == slice.first) {
    stage.evaluated_end = slice.first;
    return {nullptr, 0u, "budget"};
  }
  for (uint32_t index = slice.first; index < run_end; ++index) {
    auto& feature = features_[index];
    // Chained history off (F25) is a diagnostic: every pass after the first starts fresh each frame.
    const bool pass_reset = reset || (!inputs.chained_history && index > 0u) || index >= stage.evaluated_end || stage_reset_owed_[index];
    // Plan 5: pass n ≥ 2 runs with its own controls unless it follows pass 1.
    const std::optional<Controls>* const own =
        (index > 0u && index - 1u < inputs.later_passes.size() ? &inputs.later_passes[index - 1u] : nullptr);
    const Controls& pass_controls = (own != nullptr && own->has_value() ? **own : controls);
    EvaluateInputs evaluate_inputs = {
        .color = color,
        .output = output,
        .motion = inputs.motion,
        .motion_scale_x = inputs.motion_scale_x,
        .motion_scale_y = inputs.motion_scale_y,
        .control_mask = inputs.control_mask,
        .backbuffer = inputs.backbuffer,
        .ui = inputs.ui,
        .ui_alpha = inputs.ui_alpha,
        .reset = pass_reset,
        .bypass = false,
        .ui_correction = inputs.ui_correction,
        .depth_inverted = inputs.depth_inverted,
    };
    host_.Transition(list, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NVSDK_NGX_Result result = feature->Evaluate(list, evaluate_inputs, pass_controls);
    host_.Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    last_use_mark_ = timeline_.MarkNow();
    if (index == slice.first && faces_run && NVSDK_NGX_SUCCEED(result)) {
      // Keep faces: the slice's first pass's twin on the same input, with the same inputs but its own controls, output and history (a twin that did not run
      // the previous frame starts afresh), then the recombination of the two into that pass's result.
      evaluate_inputs.output = inputs.faces.output;
      evaluate_inputs.reset = (pass_reset || !stage.faces_evaluated);
      host_.Transition(list, inputs.faces.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      const NVSDK_NGX_Result twin_result = stage.face_feature->Evaluate(list, evaluate_inputs, inputs.faces.controls);
      host_.Transition(list, inputs.faces.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      last_use_mark_ = timeline_.MarkNow();
      if (NVSDK_NGX_SUCCEED(twin_result)) {
        inputs.resolver->CombineFaces(list, output, inputs.faces.output);
        faces_ran = true;
      } else {
        // Fix round 1 (C2): a failed twin never turns NR off. The pass's own result stands (no recombination), the twin goes, and Keep faces stays stopped
        // until it is toggled or NR loads again; NR's failure count is untouched.
        stage.faces_pause = FacesPause::FAILED_EVALUATE;
        RetireFaceFeature(&stage);
        Logf(LogLevel::ERR, "Keep faces stopped: its extra NR run failed ({:#010x}); NR runs on without it", static_cast<uint32_t>(twin_result));
      }
    }
    if (NVSDK_NGX_FAILED(result)) {
      ++consecutive_failures_;
      message_ = std::format("NR evaluate failed: {:#010x}", static_cast<uint32_t>(result));
      Log(LogLevel::WARN, message_);
      if (consecutive_failures_ >= MAX_CONSECUTIVE_FAILURES) {
        EnterFailed(std::format("NR disabled after {} failed frames (last {:#010x})", consecutive_failures_,
                                static_cast<uint32_t>(result)));
      }
      return {nullptr, 0u, "evaluate failed"};
    }
    if (index > 0u && inputs.resolver != nullptr) {
      inputs.resolver->ResolvePass(list, index, color, output);
    }
    std::swap(color, output);
  }
  consecutive_failures_ = 0u;
  stage.reset_pending = false;
  stage.evaluated_end = run_end;
  std::fill(stage_reset_owed_.begin() + slice.first, stage_reset_owed_.begin() + run_end, false);
  stage.faces_evaluated = faces_ran;
  const uint32_t run = run_end - slice.first;
  uint32_t wanted_end = std::min(slice_end, pass_count_);
  if (pass_view_limit_ != 0u) {
    wanted_end = std::min(wanted_end, pass_view_limit_);
  }
  const uint32_t wanted = wanted_end - slice.first;
  return {color, run, (run < wanted ? std::string_view("budget") : std::string_view())};
}

void Session::OnDeviceDestroyed() {
  if (device_lost_) return;  // OnDeviceLost already ran: never Flush or touch a removed device again
  timeline_.Flush(TEARDOWN_WAIT);
  ReleaseAllFeaturesAndUnload();
  teardown_pending_ = false;
  runtime_bytes_.reset();
  state_ = SessionState::OFF;
  // The device is gone: never let a later Tick auto-resume into a LoadRuntime
  // call on it, and never leave a stale failure-drain flag to misread a later
  // re-enable or suspend cycle once this Session is given a new device.
  enabled_ = false;
  suspended_ = false;
  draining_from_failure_ = false;
}

void Session::OnDeviceLost() {
  Drop("device lost: NR stopped for this device; the runtime stays loaded and is never called again");
}

void Session::Abandon() {
  Drop("NR abandoned at the game's NGX shutdown: the runtime stays loaded and is never called again");
}

void Session::Drop(std::string_view message) {
  if (device_lost_) return;
  device_lost_ = true;
  // Drop every feature and retired entry without releasing them through NGX:
  // after a device removal, calling into the snippet again is unsafe. Never
  // Flush the Timeline either -- it would wait on a fence the removed device
  // can no longer signal.
  for (auto& feature : features_) {
    if (feature) {
      feature->Abandon();
      feature.reset();
    }
  }
  for (StageState& stage : stages_) {
    if (stage.face_feature) {
      stage.face_feature->Abandon();
      stage.face_feature.reset();
    }
  }
  for (auto& retired : retired_) {
    retired.feature->Abandon();
  }
  retired_.clear();
  // Every other field is settled *before* AbandonRuntime(), which transitively
  // logs (and, per log.hpp, a sink still must not throw even though Log() now
  // guards against it): if it throws, this Session must already be fully
  // inert rather than caught with some fields reset and others not. Same
  // guarantees as OnDeviceDestroyed, plus SetEnabled now refuses every future
  // call, so nothing can ever route back into Load().
  runtime_loaded_ = false;
  teardown_pending_ = false;
  runtime_bytes_.reset();
  state_ = SessionState::OFF;
  enabled_ = false;
  suspended_ = false;
  draining_from_failure_ = false;
  host_.AbandonRuntime();
  Log(LogLevel::ERR, message);
}

SessionStatus Session::Status() const {
  SessionStatus status;
  status.state = state_;
  status.passes_requested = pass_count_;
  status.passes_live = LiveFeatureCount();
  if (const auto first = std::ranges::find_if(features_, [](const auto& feature) { return feature != nullptr; }); first != features_.end()) {
    status.capacity = (*first)->Capacity();
  }
  status.runtime_bytes = runtime_bytes_;
  status.bytes_per_megapixel = budget_.BytesPerMegapixel();
  status.message = message_;
  status.suspended = suspended_;
  status.create_failed = create_failed_;
  status.create_result = (create_failed_ ? create_result_ : 0u);
  status.group_count = group_count_;
  for (uint32_t group_index = 0u; group_index < group_count_; ++group_index) {
    const StageSlice& group = groups_[group_index];
    const uint32_t end = std::max(group.first, std::min(group.first + group.count, pass_count_));
    SessionGroupStatus& entry = status.groups[group_index];
    entry = {.stage = group.stage, .first = group.first, .count = end - group.first};
    for (uint32_t index = group.first; index < end; ++index) {
      if (!features_[index]) continue;
      if (entry.live == 0u) {
        entry.capacity = features_[index]->Capacity();
      }
      ++entry.live;
    }
    // Keep faces: the first stage in chain order whose twin is wanted but does not run.
    const StageState& stage = stages_[group.stage];
    if (!status.faces_note.empty() || !stage.faces_requested) continue;
    switch (stage.faces_pause) {
      case FacesPause::NONE:            break;
      case FacesPause::BUDGET:          status.faces_note = "Keep faces paused: the game needs video memory"; break;
      case FacesPause::UNFIT:           status.faces_note = "Keep faces paused: no video memory for its extra NR run"; break;
      case FacesPause::FAILED_CREATE:   status.faces_note = "Keep faces stopped: its extra NR run could not start"; break;
      case FacesPause::FAILED_EVALUATE: status.faces_note = "Keep faces stopped: its extra NR run failed"; break;
    }
  }
  if (state_ == SessionState::GRACE) {
    status.grace_remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(config_.grace - grace_elapsed_), std::chrono::milliseconds(0));
  }
  if (state_ == SessionState::FAILED && enabled_ && config_.auto_retry && failed_at_) {
    if (retry_attempts_ < RETRY_DELAYS.size()) {
      const auto elapsed = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(host_.Now() - *failed_at_),
                                    std::chrono::milliseconds(0));
      status.retry_in = std::max(RETRY_DELAYS[retry_attempts_] - elapsed, std::chrono::milliseconds(0));
    } else {
      status.retries_exhausted = true;
    }
  }
  return status;
}

}  // namespace uplift::nr
