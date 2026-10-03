#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "addon/placement_kind.hpp"
#include "ngx_hooks/feature_registry.hpp"
#include "nr/session_status.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

struct PlacementChoice {
  Placement placement = Placement::NONE;
  std::string reason;  // why NONE
};

// Plan 18 (design §3): a DLSS stage that cannot run on this device although DLSS runs (the Direct3D 11 bridge cannot share its image); empty: it can.
struct StageProblems {
  std::string_view after_dlss;
  std::string_view before_upscaling;
};

// v2 design §3.4. `dlss_unavailable_reason` is empty when a DLSS placement can run this session.
// `dlss_seen`: an evaluate of a main handle reached this device. Auto stays with DLSS once seen (an
// idle game drains instead of flipping back), and DLSS forced never falls back to Present.
// `before_upscaling`: PreUpscale is on and the main handle is DLSS-SR (v2 design §3.9); every AFTER_DLSS
// choice below becomes BEFORE_UPSCALING instead (Ray Reconstruction keeps NR after DLSS).
// Plan 18: `problems`: a stage with a problem is never chosen; Before upscaling falls to After DLSS, Auto to Present, and DLSS forced says the problem.
// Plan 18 Task 12: `dlss_off`: the game switched its DLSS off on this device after DLSS ran there (DlssOffLatch: it released its last DLSS feature, or on
// Direct3D 11 shut NGX down): Auto and DLSS forced both run at Present (a pause keeps a live feature, so it still waits). A reason that makes the DLSS
// placements unavailable says itself first.
[[nodiscard]] PlacementChoice ChoosePlacement(ui::PlacementSource source, std::string_view dlss_unavailable_reason, bool dlss_seen,
                                              bool before_upscaling = false, const StageProblems& problems = {}, bool dlss_off = false);
// Plan 18 Task 12: the DLSS-off fallback's INFO lines, one per fallback per device (fix round 1, M-6: only when a DLSS stage really fell back, never with
// Source = Present): the off line, and the "on again" line with what `resumed` (the placement the preference returns to) runs: "DLSS is on again on this
// device: After DLSS resumes" ("Before upscaling resumes", "NR stays at Present", or "NR stays off (why)").
inline constexpr std::string_view DLSS_OFF_LINE = "DLSS is off on this device (the game released its DLSS): NR runs at Present";
[[nodiscard]] std::string DlssOnAgainLine(const PlacementChoice& resumed);
// v2 design §3.1 (plan amendment 11): DLSS counts as idle only after this long without a main evaluate, however short GraceSeconds is. With GraceSeconds = 0
// every present would find DLSS idle, because the frame's evaluate always precedes it, and the After-DLSS placement would never run. Plan 18 Task 12 (fix
// round 1, I-2): also how long the game's release must hold before DLSS counts as switched off. Was DeviceContext's and VkDlssContext's own constant.
inline constexpr std::chrono::milliseconds DLSS_IDLE_MINIMUM{250};
// Plan 18 Task 12 (fix round 1, M-2, M-5): the add-on's FrameConfig::dlss_released and SetupFacts::dlss_off: DLSS was seen on `device` and no DLSS feature of
// `api` is live there now (FeatureRegistry::UpscalerLive): the game released it, or on Direct3D 11 shut NGX down (Uplift's NR there is on its private device,
// so nothing waits on the game's NGX). Direct3D 12's and Vulkan's shutdowns never reach the registry: Plan 15's hold and the native Vulkan context's stopped
// rule gate them first. The registry is not asked before DLSS was seen.
[[nodiscard]] bool DlssReleased(const ngx_hooks::FeatureRegistry& registry, const void* device, ngx_hooks::NgxApi api, bool dlss_seen);
// Plan 18 Task 12 (fix round 1, I-2): whether the game switched its DLSS off on a device, with hysteresis. Off once the release (`released`: DlssReleased, and
// the context's own conditions) has held for DLSS_IDLE_MINIMUM and at least two presents in a row, so a settings change that re-creates DLSS across a
// present, or a game that creates, evaluates and releases DLSS within each frame, is no switch-off (no flip, no log, no shares dropped). Off until the next
// main evaluate. Pure; the add-on's lock.
class DlssOffLatch {
 public:
  void OnPresent(bool released, std::chrono::steady_clock::time_point now) {
    if (!released) {
      released_since_.reset();
      released_presents_ = 0u;
      return;
    }
    if (!released_since_) {
      released_since_ = now;
    }
    ++released_presents_;
    if (released_presents_ >= MIN_PRESENTS && now - *released_since_ >= DLSS_IDLE_MINIMUM) {
      off_ = true;
    }
  }
  // A main evaluate: DLSS runs, so the release window starts over. True when it ended a switch-off, which only happens with `may_end` (not while Plan 15
  // holds the device).
  bool OnEvaluate(bool may_end) {
    released_since_.reset();
    released_presents_ = 0u;
    return (may_end && std::exchange(off_, false));
  }
  [[nodiscard]] bool Off() const { return off_; }

 private:
  static constexpr uint32_t MIN_PRESENTS = 2u;
  std::optional<std::chrono::steady_clock::time_point> released_since_;
  uint32_t released_presents_ = 0u;
  bool off_ = false;
};
// Plan 14 (design §1.2, batch 1 review I-1): whether Setup counts the game's DLSS as seen on this device. Only an evaluate says DLSS runs (`context_evaluated`: a
// native Vulkan context's flag; a Direct3D 12 context's own flag reaches the host through its status). A create (`upscaler_created`) counts on Vulkan alone,
// where the passthrough context sees no evaluate until a DLSS pick makes it watch, and the NGX create hook is the only signal.
[[nodiscard]] bool SetupDlssSeen(bool vulkan, bool upscaler_created, bool context_evaluated);

// Plan 14 (design §2.2, batch 2 review): the gates of the native Vulkan context's copies of DLSS's motion vectors, made in the game's frame for the Present path.
struct PresentMotionGates {
  bool enabled = false;
  nr::SessionState nr_state = nr::SessionState::OFF;  // the bridged context's; OFF without one
  bool bridge_gpu_ordered = false;                    // a bridge exists, is GPU-ordered and has not stopped (a CPU-ordered one holds the add-on's lock across capped waits)
  bool same_queue = false;                            // the game presents on the effect runtime's queue (else the bridge waits a GPU frame inside the present event)
  bool native_wants_nr = false;                       // the native context runs a DLSS placement, which binds DLSS's vectors directly
  bool native_can_copy = true;                        // the native context, if there is one, has not been shut down, dropped or torn down
  bool problem = false;                               // a native problem, or a reason the DLSS placements are off
  bool dlss_motion = false;                           // the motion preference is DLSS (MotionVectors Auto or DLSS)
  // Final review, minor 5: the game created a DLSS feature on this device, so a game with NGX's extensions but DLSS off never gets a native context for
  // the copies (it stays at Plan 13's footprint: no frame semaphore signalled at every present, no evaluate watched).
  bool upscaler_created = false;
};
// True while the copies are wanted. NR "on" is LOADING, ACTIVE or GRACE: not FAILED or DRAINING, which cannot run NR.
[[nodiscard]] bool PresentMotionWanted(const PresentMotionGates& gates);

// Plan 15 (design 2026-10-02 §2): NR held off a Direct3D 12 device after the game's own NGX shutdown there, until the game's DLSS starts on it again: a
// present that sees a DLSS create on the device since the shutdown (FeatureRegistry::UpscalerCreates rising past its value then: the game re-initialised
// NGX, as some do when a DLSS setting changes), or (fix round) a successful main-handle evaluate of a feature created after it (its serial above
// FeatureRegistry::LastSerial then), which also ends it while presents starve. A feature created before the shutdown never counts, so an evaluate that
// finished alongside the shutdown on another thread cannot end it. A game that quits never creates or evaluates DLSS again, so NR never restarts in it.
// An abandoned NR (the shutdown's capped wait ran out) never resumes. A value: the add-on keeps a device's copy across its contexts (fix round, minor 3).
// Pure; the add-on's lock.
class CoreShutdownHold {
 public:
  // The game's NGX shutdown on the device: `creates` is the device's DLSS creates so far, `serial` the newest feature's serial on any device. A second
  // shutdown while held keeps the hold and takes the newer values.
  void Hold(uint64_t creates, uint64_t serial) {
    held_ = true;
    creates_at_hold_ = creates;
    serial_at_hold_ = serial;
  }
  // NR could not be released in time and was abandoned: held for good.
  void Abandon() {
    held_ = true;
    abandoned_ = true;
  }
  // True once, when `creates` shows a DLSS create since the hold: the hold ends there, and NR may load again through the normal path.
  [[nodiscard]] bool Resume(uint64_t creates) {
    if (!held_ || abandoned_ || creates <= creates_at_hold_) return false;
    held_ = false;
    return true;
  }
  // True once, when the game's DLSS evaluated successfully a feature of `serial` created after the hold: the hold ends there.
  [[nodiscard]] bool ResumeOnEvaluate(uint64_t serial) {
    if (!held_ || abandoned_ || serial <= serial_at_hold_) return false;
    held_ = false;
    return true;
  }
  [[nodiscard]] bool Held() const { return held_; }
  [[nodiscard]] bool Abandoned() const { return abandoned_; }

 private:
  bool held_ = false;
  bool abandoned_ = false;
  uint64_t creates_at_hold_ = 0u;
  uint64_t serial_at_hold_ = 0u;
};

// NVSDK_NGX_PerfQuality_Value as the overlay names it; "custom" for anything else.
[[nodiscard]] std::string_view QualityName(int perf_quality);
// "Placement: after DLSS (Performance, 1920x1080 -> 3840x2160)", "Placement: Present", "Placement: none (why)". Plan 13 (design §5.1): with
// `vulkan`, a DLSS placement names the API first: "Placement: after DLSS (Vulkan, Performance, 960x540 -> 1920x1080)".
[[nodiscard]] std::string FormatPlacementLine(const PlacementChoice& choice,
                                              const std::optional<ngx_hooks::CreateSnapshot>& main, bool vulkan = false);

struct MessageInputs {
  std::string_view blocked;      // a conflicting host, a missing runtime, NR on another device
  std::string_view output;       // the Present path's back-buffer format or colour space
  std::string_view placement;    // why no placement runs, or DLSS idle
  std::string_view skip_reason;  // this frame's static skip reason
  // Fix round 1, M1: whether `skip_reason` is Session::Evaluate's own (PipelineResult::from_session),
  // not a string-set guess -- a pipeline-level reason such as AfterDlssSource's own "frame too small"
  // can collide with one of Session's, and a stale session_message must not stand in for it then.
  bool skip_from_session = false;
  std::string_view session_message;  // the Session's own message
};

// The one status sentence. Plan 2 final review M1: when the Session itself skipped the frame, its own
// message ("NR skipped: needs 1.2 GB, 0.8 GB free") says more than the one-word reason.
[[nodiscard]] std::string ContextMessage(const MessageInputs& inputs);

}  // namespace uplift::addon
