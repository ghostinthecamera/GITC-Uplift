#include "nr/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <utility>

#include "nr/log.hpp"

namespace uplift::nr {
namespace {

constexpr uint32_t MAX_PASSES = 10u;
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
  if (runtime_loaded_ || !features_.empty() || !retired_.empty()) {
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
      reset_pending_ = true;
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
    case SessionState::ACTIVE:  EnterGrace(); break;
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
  // No reset_pending_: a pass that keeps running keeps its input and its history. A pass that starts running
  // (a new feature, or a kept idle one) restarts alone, through Evaluate's index >= passes_evaluated_.
  if (state_ == SessionState::FAILED && enabled_ && !teardown_pending_) {
    BeginLoading();
  }
}

void Session::SetCreateOptions(uint32_t preset, uint32_t performance) {
  preset_ = preset;
  performance_ = performance;
}

void Session::RetryNow() {
  retry_attempts_ = 0u;
  retry_requested_ = (state_ == SessionState::FAILED && enabled_ && !device_lost_);
}

void Session::BeginLoading() {
  suspended_ = false;
  message_.clear();
  passes_limit_ = MAX_PASSES;
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
  consecutive_failures_ = 0u;
  draining_from_failure_ = false;  // a successful reload closes out any failure drain
  reset_pending_ = true;
  last_sample_.reset();
  budget_.ResetYield();
  state_ = SessionState::ACTIVE;
  Log(LogLevel::INFO, "NR runtime loaded");
}

void Session::EnterGrace() {
  state_ = SessionState::GRACE;
  grace_elapsed_ = std::chrono::milliseconds(0);
}

void Session::BeginTeardown(SessionState next) {
  state_ = next;
  teardown_pending_ = runtime_loaded_ || !features_.empty() || !retired_.empty();
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
  Log(LogLevel::INFO, "NR runtime unloaded; NR memory released");
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
  const auto elapsed = (last_tick_ ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_tick_)
                                   : std::chrono::milliseconds(0));
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
  const auto live = static_cast<uint32_t>(features_.size());
  const MemoryInfo memory = host_.QueryMemory();
  switch (budget_.Sample(memory, live)) {
    case YieldAction::DROP_PASS:
      // Pooled memory only returns when the last feature is released, so a
      // pass drop is a full rebuild with one pass fewer.
      passes_limit_ = live - 1u;
      RetireAllFeatures();
      reset_pending_ = true;
      // Any headroom already held toward restoring a previous drop no longer
      // applies: the target it was measured against just changed.
      budget_.ResetYield();
      message_ = std::format("Budget: dropped to {} pass(es), the game needs VRAM", passes_limit_);
      Log(LogLevel::WARN, message_);
      break;
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
      if (passes_limit_ < pass_count_) {
        const uint64_t need = budget_.FeatureBytes(resume_size_);
        if (budget_.ResumeReady(memory, need, now)) {
          ++passes_limit_;
          budget_.ResetYield();  // a further increment needs its own fresh 10 s
          Logf(LogLevel::INFO, "VRAM headroom restored; raising to {} of {} pass(es)", passes_limit_, pass_count_);
        }
      }
      break;
  }
}

void Session::TryResume(std::chrono::steady_clock::time_point now) {
  if (last_sample_ && now - *last_sample_ < SAMPLE_PERIOD) return;
  last_sample_ = now;
  const uint64_t need = static_cast<uint64_t>(pass_count_) * budget_.FeatureBytes(resume_size_) + resume_surface_bytes_;
  if (!budget_.ResumeReady(host_.QueryMemory(), need, now)) return;
  BeginLoading();
  Log(LogLevel::INFO, "VRAM headroom restored; resuming NR");
}

void Session::RetireFeature(size_t index) {
  retired_.push_back({last_use_mark_, std::move(features_[index])});
  features_.erase(features_.begin() + static_cast<std::ptrdiff_t>(index));
}

void Session::RetireAllFeatures() {
  while (!features_.empty()) {
    RetireFeature(features_.size() - 1u);
  }
}

void Session::ReleaseDueRetired() {
  std::erase_if(retired_, [this](Retired& retired) {
    if (!timeline_.IsComplete(retired.mark)) return false;
    retired.feature->Release();
    return true;
  });
}

uint64_t Session::LiveFeatureBytes() const {
  uint64_t bytes = 0u;
  for (const auto& feature : features_) {
    bytes += budget_.FeatureBytes(feature->Capacity());
  }
  return bytes;
}

bool Session::EnsureFeatures(ID3D12GraphicsCommandList* list, const PassChain& chain, std::string_view* reason) {
  const auto now = host_.Now();
  const uint32_t desired = std::min(pass_count_, passes_limit_);
  // A pass-count decrease keeps its surplus features live and idle, as the Defaults view does: Evaluate runs the
  // first `desired`. Under the pool rule, releasing them frees nothing while pass 1 lives (E10). They go with the
  // next rebuild, yield or disable, all of which empty the pool.
  bool rebuild = false;
  for (const auto& feature : features_) {
    if (!Fits(chain.size, feature->Capacity())) {
      rebuild = true;
    }
  }
  if (!rebuild && !features_.empty()) {
    const Size capacity = features_.front()->Capacity();
    if (static_cast<double>(chain.size.Pixels()) <= SHRINK_FRACTION * static_cast<double>(capacity.Pixels())) {
      if (!small_since_) {
        small_since_ = now;
      }
      rebuild = now - *small_since_ >= SHRINK_HOLD;
    } else {
      small_since_.reset();
    }
  }
  if (rebuild) {
    // The snippet keeps feature memory in one pool that only empties when its
    // last feature is released (spike E10), so a rebuild retires everything
    // and recreates only once the pool is empty. Old and new never coexist.
    RetireAllFeatures();
    small_since_.reset();
    reset_pending_ = true;
  }
  if (!retired_.empty()) {
    *reason = "resizing";
    return false;
  }
  if (features_.size() >= desired) return true;

  const FitResult fit = budget_.Fit(chain.size, desired, chain.surface_bytes, host_.QueryMemory(),
                                    LiveFeatureBytes() + chain.held_intermediate_bytes, !first_use_paid_);
  const auto target = std::max(fit.passes, static_cast<uint32_t>(features_.size()));
  if (target == 0u) {
    message_ = std::format("NR skipped: needs {}, {} free", Gigabytes(fit.need_bytes), Gigabytes(fit.available_bytes));
    *reason = "budget";
    return false;
  }
  const bool fresh_set = features_.empty();
  while (features_.size() < target) {
    auto feature = host_.NewFeature();
    const auto before = host_.QueryRuntimeBytes();
    last_use_mark_ = timeline_.MarkNow();  // Create is recorded on this frame's list, win or lose
    const NVSDK_NGX_Result result = feature->Create(list, {.capacity = chain.size, .preset = preset_, .performance = performance_});
    if (NVSDK_NGX_FAILED(result)) {
      EnterFailed(std::format("NR feature creation failed: {:#010x}", static_cast<uint32_t>(result)));
      *reason = "create failed";
      return false;
    }
    first_use_paid_ = true;
    const auto after = host_.QueryRuntimeBytes();
    if (before && after && *after > *before + budget_.StatsOvercountBytes()) {
      // Create-time stats exclude the 8 B/px capacity buffer a feature adds at its
      // first smaller frame (E6); keep charging it (spec §6.2).
      budget_.Calibrate(chain.size,
                        *after - *before - budget_.StatsOvercountBytes() + 8u * chain.size.Pixels());
    }
    features_.push_back(std::move(feature));
  }
  if (fresh_set) {
    reset_pending_ = true;  // a new set starts every pass afresh; an appended pass resets alone (index >= passes_evaluated_)
  }
  message_ = (target < pass_count_ ? std::format("Budget: running {} of {} passes", target, pass_count_) : std::string());
  return true;
}

void Session::NoteFrameSize(Size size) {
  if (!size.Empty()) {
    resume_size_ = size;
  }
  if (state_ == SessionState::FAILED && enabled_ && !teardown_pending_ && !runtime_loaded_ && !size.Empty()) {
    // Retry only on a real change from a known size: an empty last_size_ means
    // no size has been recorded yet, not that the size just "changed".
    if (last_size_.Empty()) {
      last_size_ = size;
    } else if (size != last_size_) {
      last_size_ = size;
      BeginLoading();
    }
  }
}

EvaluateResult Session::Evaluate(ID3D12GraphicsCommandList* list, const PassChain& chain,
                                 const FrameInputs& inputs, const Controls& controls) {
  NoteFrameSize(chain.size);
  resume_surface_bytes_ = chain.surface_bytes;
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
  last_size_ = chain.size;

  std::string_view reason;
  if (!EnsureFeatures(list, chain, &reason)) return {nullptr, 0u, reason};

  ID3D12Resource* color = chain.a;
  ID3D12Resource* output = chain.b;
  const bool reset = reset_pending_ || inputs.reset_hint;
  // Plan 7: a pass-count decrease keeps its surplus features idle; Evaluate runs only the first `desired`, exactly
  // as the Defaults view's pass_view_limit_ already narrows it, without a rebuild either. A pass that did not run
  // the previous frame starts its history afresh (index >= passes_evaluated_, below).
  const uint32_t desired = std::min(pass_count_, passes_limit_);
  size_t run_count = std::min<size_t>(features_.size(), desired);
  if (pass_view_limit_ != 0u) {
    run_count = std::min<size_t>(run_count, pass_view_limit_);
  }
  for (size_t index = 0u; index < run_count; ++index) {
    auto& feature = features_[index];
    // Chained history off (F25) is a diagnostic: every pass after the first starts fresh each frame.
    const bool pass_reset = reset || (!inputs.chained_history && index > 0u) || index >= passes_evaluated_;
    // Plan 5: pass n ≥ 2 runs with its own controls unless it follows pass 1.
    const std::optional<Controls>* const own =
        (index > 0u && index - 1u < inputs.later_passes.size() ? &inputs.later_passes[index - 1u] : nullptr);
    const Controls& pass_controls = (own != nullptr && own->has_value() ? **own : controls);
    host_.Transition(list, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const NVSDK_NGX_Result result = feature->Evaluate(
        list,
        {
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
        },
        pass_controls);
    host_.Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    last_use_mark_ = timeline_.MarkNow();
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
      inputs.resolver->ResolvePass(list, static_cast<uint32_t>(index), color, output);
    }
    std::swap(color, output);
  }
  consecutive_failures_ = 0u;
  reset_pending_ = false;
  passes_evaluated_ = run_count;
  const auto run = static_cast<uint32_t>(run_count);
  const uint32_t wanted = (pass_view_limit_ == 0u ? pass_count_ : std::min(pass_count_, pass_view_limit_));
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
    feature->Abandon();
  }
  features_.clear();
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
  status.passes_live = static_cast<uint32_t>(features_.size());
  status.capacity = (features_.empty() ? Size{} : features_.front()->Capacity());
  status.runtime_bytes = runtime_bytes_;
  status.bytes_per_megapixel = budget_.BytesPerMegapixel();
  status.message = message_;
  status.suspended = suspended_;
  if (state_ == SessionState::GRACE) {
    status.grace_remaining = std::max(config_.grace - grace_elapsed_, std::chrono::milliseconds(0));
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
