#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "addon/placement_kind.hpp"
#include "ngx_hooks/feature_registry.hpp"
#include "nr/session_status.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

struct PlacementChoice {
  Placement placement = Placement::NONE;
  std::string reason;  // why NONE
};

// v2 design §3.4. `dlss_unavailable_reason` is empty when a DLSS placement can run this session.
// `dlss_seen`: an evaluate of a main handle reached this device. Auto stays with DLSS once seen (an
// idle game drains instead of flipping back), and DLSS forced never falls back to Present.
// `before_upscaling`: PreUpscale is on and the main handle is DLSS-SR (v2 design §3.9); every AFTER_DLSS
// choice below becomes BEFORE_UPSCALING instead (Ray Reconstruction keeps NR after DLSS).
[[nodiscard]] PlacementChoice ChoosePlacement(ui::PlacementSource source, std::string_view dlss_unavailable_reason,
                                              bool dlss_seen, bool before_upscaling = false);
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
