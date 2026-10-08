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
  nr::SessionState nr_state = nr::SessionState::OFF;  // the bridged context's; OFF without one (Plan 19: the native context's at native Present)
  bool bridge_gpu_ordered = false;                    // a bridge exists, is GPU-ordered and has not stopped (a CPU-ordered one holds the add-on's lock across capped waits)
  bool native_present = false;                        // Plan 19: NR at Present runs natively (no bridge involved), which binds the copies as the bridge does
  bool same_queue = false;                            // the game presents on the effect runtime's queue (else the bridge waits a GPU frame inside the present event)
  bool native_wants_nr = false;                       // the native context runs a DLSS placement, which binds DLSS's vectors directly
  bool native_can_copy = true;                        // the native context, if there is one, has not been shut down, dropped or torn down
  bool problem = false;                               // a native problem, or a reason the DLSS placements are off
  bool dlss_motion = false;                           // the motion preference is DLSS (MotionVectors Auto or DLSS)
  // Final review, minor 5: the game created a DLSS feature on this device, so a game with NGX's extensions but DLSS off never gets a native context for
  // the copies (it stays at Plan 13's footprint: no frame semaphore signalled at every present, no evaluate watched).
  bool upscaler_created = false;
};
// True while the copies are wanted. NR "on" is LOADING, ACTIVE or GRACE: not FAILED or DRAINING, which cannot run NR. Natively at Present OFF counts as on too
// (stress round: NR loads within the present that reads the gates, and its first recording must already have them).
[[nodiscard]] bool PresentMotionWanted(const PresentMotionGates& gates);

// Plan 19 (the owner's decisions): where NR at Present runs on a 64-bit Vulkan device. Native, on the game's own VkDevice, whenever the setting asks for it and
// nothing makes it impossible; the private Direct3D 12 device (VkBridge) otherwise, with the reason the card and the log give. T5 (2026-10-08): the chain is
// Native -> Direct3D 12 -> Helper (NR in gitc-uplift-helper64.exe, through HelperFront and VkClient), each taken when the one before cannot run.
enum class VkNrRoute : uint8_t {
  NATIVE,
  DIRECT3D_12,
  HELPER,
};
// T6 fix round (I-A): the game's NGX shutdown while NR ran natively at Present becomes a stop (the route moves along the chain) only once the game has gone
// on presenting for this long, both counts: a game that shuts NGX down as it exits reaches vkDestroyDevice first, so no fallback is ever built for it.
inline constexpr uint32_t CORE_SHUTDOWN_STOP_PRESENTS = 120u;
inline constexpr std::chrono::seconds CORE_SHUTDOWN_STOP_AFTER{2};
// `presents` presents and `elapsed` since the shutdown. Pure.
[[nodiscard]] inline bool CoreShutdownStopDue(uint32_t presents, std::chrono::steady_clock::duration elapsed) {
  return presents >= CORE_SHUTDOWN_STOP_PRESENTS && elapsed >= CORE_SHUTDOWN_STOP_AFTER;
}
// In-game round 1 (bug A, a GPU hang switching a running native Present to After DLSS): which command buffers a native Vulkan context's NR records into.
// NR's Session (its NGX features, its intermediates) belongs to one of them; a placement that wants the other reloads it from OFF first, after the GPU has
// finished the old one's work, so the features are always created by the path that uses them (the cold path, proven in game).
// Review 97f99e3: every native placement is its own stream, After DLSS and Before upscaling included (both record into the game's buffers, but feed NR
// different inputs).
enum class VkNrStream : uint8_t {
  NONE,              // no NR runs (no placement, or Present on the Direct3D 12 or helper route)
  OWN_BUFFERS,       // native Present: Uplift's own command buffers on ReShade's effect queue
  AFTER_DLSS,        // the game's command buffers, after its DLSS evaluate
  BEFORE_UPSCALING,  // the game's command buffers, before its DLSS evaluate
};
// True when the Session, loaded for `loaded_for` and not OFF, must go through OFF before NR runs for `wanted`. Pure.
[[nodiscard]] inline bool VkStreamNeedsReload(VkNrStream loaded_for, VkNrStream wanted, bool session_off) {
  return !session_off && loaded_for != VkNrStream::NONE && wanted != VkNrStream::NONE && wanted != loaded_for;
}
// Review 97f99e3 (Important 2): NGX may set a feature up at its first evaluate from the motion input it is handed, so the live features never see a
// different kind of motion input from the one they were first evaluated with: what NGX is handed as motion vectors, in kind, format and resolution.
enum class VkMotionKind : uint8_t {
  NONE,          // Uplift's all-zero image at the canvas
  GAME_VECTORS,  // the game's own (in place, or copied into Before upscaling's canvas)
  PRESENT_COPY,  // DLSS's vectors copied for Present
  LAUNCHPAD,     // UPLIFT_MV converted to the work image
};
struct VkMotionShape {
  VkMotionKind kind = VkMotionKind::NONE;
  uint32_t format = 0u;  // the VkFormat NGX reads
  bool low_res = false;  // smaller than the canvas NR runs at (DLSS's render-resolution vectors)
  friend bool operator==(const VkMotionShape&, const VkMotionShape&) = default;
};
// A different shape for this many recordings in a row reloads the Session from OFF; fewer (a copy missing for a frame or two) only skip NR on those
// recordings, so NGX never evaluates the live features with it.
inline constexpr uint32_t VK_MOTION_RELOAD_RECORDINGS = 10u;  // stress round: was 30 (half a second at 60 fps per settings change)
enum class VkMotionVerdict : uint8_t {
  RUN,     // the shape the features were first evaluated with (or the first evaluate of this load)
  SKIP,    // a different one: no NR on this recording
  RELOAD,  // a different one for VK_MOTION_RELOAD_RECORDINGS recordings: no NR, and the Session reloads from OFF
};
// `loaded_with`: the shape of this load's first evaluate (nullopt: none yet); `mismatches_before`: different recordings in a row before this one. Pure.
[[nodiscard]] inline VkMotionVerdict VkMotionCheck(const std::optional<VkMotionShape>& loaded_with, const VkMotionShape& shape, uint32_t mismatches_before) {
  if (!loaded_with || *loaded_with == shape) return VkMotionVerdict::RUN;
  return (mismatches_before + 1u >= VK_MOTION_RELOAD_RECORDINGS ? VkMotionVerdict::RELOAD : VkMotionVerdict::SKIP);
}
// In-game round 1 (bug B): while the native context runs a DLSS stage, the helper never drives NR (it is not even attached, so ReShade's effect events stay
// with the native context). In-game round 2 (the owner, 2026-10-08): the native context runs a DLSS stage only while VulkanNr is Native (DecideVkStages), so
// with VulkanNr Helper the helper always runs. Pure.
[[nodiscard]] inline bool VkHelperRuns(VkNrRoute route, bool native_at_dlss_stage) {
  return route == VkNrRoute::HELPER && !native_at_dlss_stage;
}
// Transitions (owner, 2026-10-08: "Make sure all transition possibilities are robust"): NR has exactly one owner per 64-bit Vulkan device at a time.
// Every owner change, whatever causes it, goes through one hand-over: the old owner stops recording at once, its GPU work finishes (capped), its Session
// drains to OFF with no grace and unloads, and only then the new owner loads from OFF (NrClaim and the helper's claim keep them apart). Only a plain NR off
// and on by the user (the owner stays) and a VRAM yield keep the grace.
enum class VkNrOwner : uint8_t {
  NONE,               // nobody: the native context dropped NR (its runtime may still be mapped) or the device was lost
  NATIVE_PRESENT,     // the native context, at Present (the native route)
  NATIVE_DLSS_STAGE,  // the native context, at After DLSS or Before upscaling (VulkanNr governs Present only)
  BRIDGE,             // the bridge's context on the private Direct3D 12 device, at Present
  HELPER,             // Uplift's helper process, at Present
};
// What decides the owner this frame. The stage and the route chain are the ones ChooseVkNrRoute and the native context's placement give.
struct VkOwnerFacts {
  bool abandoned = false;             // the native context dropped NR, or the device was lost
  bool native_at_dlss_stage = false;  // the native context's placement is After DLSS or Before upscaling
  VkNrRoute route = VkNrRoute::NATIVE;
};
[[nodiscard]] VkNrOwner ChooseVkNrOwner(const VkOwnerFacts& facts);
// The things that can hold NR loaded on a device: the native context (both of its owners), the bridge's context, the helper.
enum class VkNrHolder : uint8_t {
  NATIVE,
  BRIDGE,
  HELPER,
};
// Whether `holder` drains now, with no grace (the hand-over's steps 1-3): it is not the owner. The owner keeps its grace (a plain NR off and on).
[[nodiscard]] bool VkHolderDrainsNow(VkNrOwner owner, VkNrHolder holder);
enum class VkOwnerCause : uint8_t {
  STAGE,     // the native context's placement moved between Present and a DLSS stage
  SETTING,   // VulkanNr changed
  FALLBACK,  // the route chain moved by itself (a route became impossible or possible again)
};
struct VkOwnerChange {
  bool changed = false;  // an owner change between two owners (the first decision, and a move to nobody, are none)
  VkOwnerCause cause = VkOwnerCause::FALLBACK;
};
// `stage_changed` and `setting_changed` compare this frame's facts with the previous frame's; the stage outranks the setting. Pure.
[[nodiscard]] VkOwnerChange VkOwnerChangeOf(VkNrOwner previous, VkNrOwner now, bool stage_changed, bool setting_changed);
[[nodiscard]] std::string_view VkOwnerName(VkNrOwner owner);
// "NR moves from {old owner} to {new owner} ({cause}): the old one drains now, the new one starts from off"; `detail`: the route's reason for a fallback.
[[nodiscard]] std::string VkOwnerChangeLine(VkNrOwner previous, VkNrOwner now, VkOwnerCause cause, std::string_view detail);
// What decides it, per device. Views must outlive the call.
struct VkRouteFacts {
  ui::VulkanNrMode setting = ui::VulkanNrMode::NATIVE;
  std::string_view native_needs;    // what native Vulkan NR needs that the device lacks (NGX's extensions, ReShade 6.8, the hooks...); empty: nothing
  std::string_view start_error;     // the native context could not start here (its error, or no command buffers of Uplift's own); empty: it could
  std::string_view create_failure;  // NGX could not create NR's feature on the device this session (the Session's message, with NGX's result); empty: no
  // T5: the Direct3D 12 route's own impossibilities, kept for the session as the native ones are.
  std::string_view d3d12_start_error;     // the bridge or its context could not start here (their error); empty: they could
  std::string_view d3d12_create_failure;  // NGX could not create NR's feature on the private Direct3D 12 device (the Session's message); empty: no
  // T6 (I-2, I-3, I-4): native NR at Present stopped here (second-queue timeouts, the game's NGX shutdown) or is latched off from an earlier start or a device
  // loss: a whole sentence, used as it is; empty: no.
  std::string_view native_off;
};
struct VkRouteChoice {
  VkNrRoute route = VkNrRoute::DIRECT3D_12;
  // Why NR does not run natively (the Direct3D 12 route), or why it runs in the helper (the chain's reasons, joined by "; ", or the setting); empty on the
  // native route.
  std::string reason;
  std::string native_unavailable;  // why Native cannot run here: the panel greys it with this; empty: it can
  std::string d3d12_unavailable;   // T5: why Direct3D 12 cannot run here, the same way; empty: it can
};
// The chain starts at the setting's route. A route that cannot run hands over to the next one: a native impossibility (a creation failure, then T6's
// `native_off`, then a start failure, then what the device lacks) to Direct3D 12, a Direct3D 12 one (a creation failure, then a start failure) to the helper, which is final. A creation
// failure stays for the session (the caller keeps it), so the routes never ping-pong. On the Direct3D 12 route a native impossibility outranks the setting's
// own reason. Pure.
[[nodiscard]] VkRouteChoice ChooseVkNrRoute(const VkRouteFacts& facts);
// The log's line when a device's route is decided or changes: "Vulkan: NR at Present runs natively on the game's Vulkan device", "Vulkan: NR at Present
// runs on a private Direct3D 12 device: {reason}", or "Vulkan: NR at Present runs in Uplift's helper process: {reason}".
[[nodiscard]] std::string VkRouteLine(const VkRouteChoice& choice);
// In-game round 2 (the owner, 2026-10-08; replaces round 1's "VulkanNr governs the Present stage only"): only native Vulkan NR runs inside the game's DLSS.
// Fix round 1 (I2, the coordinator's ruling: what works stays available): the DLSS stages are greyed, and the native context is given Present whatever is
// stored, only while VulkanNr is Direct3D 12 or Helper, or a failure stops native NR inside the game's DLSS too (what the device lacks, the game's NGX
// shutdown, a loss, an abandon). A failure of native Present alone (its command buffers, the present queue's timeouts, the VulkanNativeNr latch) moves
// Present along the chain and leaves the DLSS stages native and selectable.
struct VkStageFacts {
  VkNrRoute route = VkNrRoute::NATIVE;                  // where NR at Present runs (never decides the stages: kept for the matrix)
  ui::VulkanNrMode setting = ui::VulkanNrMode::NATIVE;  // VulkanNr
  std::string_view native_dlss_off;                     // why native NR cannot run inside the game's DLSS either; empty: it can
  bool dlss_stage_wanted = false;                       // the native context would run a DLSS stage this present (the stored stage, DLSS seen, nothing in the way)
  bool stopped_at_dlss_stage = false;                   // the native context stopped at a DLSS stage (the game's NGX shutdown) and keeps it
};
struct VkStageDecision {
  std::string stages_off;             // why After DLSS and Before upscaling are greyed; empty: selectable
  bool native_present_only = false;   // the native context gets Present (Source Auto, PreUpscale off) whatever is stored
  bool native_at_dlss_stage = false;  // the native context runs a DLSS stage this present: ChooseVkNrOwner's input
};
[[nodiscard]] VkStageDecision DecideVkStages(const VkStageFacts& facts);
// In-game round 2: a pick of Direct3D 12 or Helper in VulkanNr (from `before` to `after`) while the stored stage is After DLSS or Before upscaling moves the
// stored stage to Present at once, as a click on Present. A pick of Native leaves the stage where it is. Pure.
[[nodiscard]] bool VkRoutePickMovesStage(ui::VulkanNrMode before, ui::VulkanNrMode after, ui::SourcePick stage);

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
