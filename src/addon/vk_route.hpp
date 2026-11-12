#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "nr/session_status.hpp"
#include "ui/settings.hpp"

// The Vulkan route's policy: where a 64-bit Vulkan device's NR at Present runs, who owns NR there, which streams a native context records into,
// the motion input its features keep, and the Present path's copies of DLSS's vectors. Pure: each takes the add-on's facts as arguments.
namespace uplift::addon {

// The gates of the native Vulkan context's copies of DLSS's motion vectors, made in the game's frame for the Present path.
struct PresentMotionGates {
  bool enabled = false;
  nr::SessionState nr_state = nr::SessionState::OFF;  // the bridged context's; OFF without one (the native context's at native Present)
  bool bridge_gpu_ordered = false;                    // a bridge exists, is GPU-ordered and has not stopped (a CPU-ordered one holds the add-on's lock across capped waits)
  bool native_present = false;                        // NR at Present runs natively (no bridge involved), which binds the copies as the bridge does
  bool same_queue = false;                            // the game presents on the effect runtime's queue (else the bridge waits a GPU frame inside the present event)
  bool native_wants_nr = false;                       // the native context runs a DLSS placement, which binds DLSS's vectors directly
  // Split chain: the native context's chain also has a later group at Present (pass 1 at a DLSS stage), which binds these copies: they are
  // wanted then, as for pass 1 at Present.
  bool native_present_group = false;
  bool native_can_copy = true;                        // the native context, if there is one, has not been shut down, dropped or torn down
  bool problem = false;                               // a native problem, or a reason the DLSS placements are off
  bool dlss_motion = false;                           // the motion preference is DLSS (MotionVectors Auto or DLSS)
  // The game created a DLSS feature on this device, so a game with NGX's extensions but DLSS off never gets a native context for
  // the copies (it stays at the minimum observation footprint: no frame semaphore signalled at every present, no evaluate watched).
  bool upscaler_created = false;
};
// True while the copies are wanted. NR "on" is LOADING, ACTIVE or GRACE: not FAILED or DRAINING, which cannot run NR. Natively at Present OFF counts as on too
// (NR loads within the present that reads the gates, and its first recording must already have them).
[[nodiscard]] bool PresentMotionWanted(const PresentMotionGates& gates);

// Where NR at Present runs on a 64-bit Vulkan device. Native, on the game's own VkDevice, whenever the setting asks for it and
// nothing makes it impossible; the private Direct3D 12 device (VkBridge) otherwise, with the reason the card and the log give. The chain is
// Native -> Direct3D 12 -> Helper (NR in gitc-uplift-helper64.exe, through HelperFront and VkClient), each taken when the one before cannot run.
enum class VkNrRoute : uint8_t {
  NATIVE,
  DIRECT3D_12,
  HELPER,
};
// The game's NGX shutdown while NR ran natively at Present becomes a stop (the route moves along the chain) only once the game has gone
// on presenting for this long, both counts: a game that shuts NGX down as it exits reaches vkDestroyDevice first, so no fallback is ever built for it.
inline constexpr uint32_t CORE_SHUTDOWN_STOP_PRESENTS = 120u;
inline constexpr std::chrono::seconds CORE_SHUTDOWN_STOP_AFTER{2};
// `presents` presents and `elapsed` since the shutdown. Pure.
[[nodiscard]] inline bool CoreShutdownStopDue(uint32_t presents, std::chrono::steady_clock::duration elapsed) {
  return presents >= CORE_SHUTDOWN_STOP_PRESENTS && elapsed >= CORE_SHUTDOWN_STOP_AFTER;
}
// Which command buffers a native Vulkan context's NR records into.
// NR's Session (its NGX features, its intermediates) belongs to one of them; a placement that wants the other reloads it from OFF first, after the GPU has
// finished the old one's work, so the features are always created by the path that uses them (the cold path, proven in game).
// Every native placement is its own stream, After DLSS and Before upscaling included (both record into the game's buffers, but feed NR
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
// The streams a native chain records into, one bit per stream (NONE has none). A one-group chain's set is its one stream.
using VkStreamSet = uint8_t;
[[nodiscard]] constexpr VkStreamSet VkStreamBit(VkNrStream stream) {
  return (stream == VkNrStream::NONE ? VkStreamSet{0u} : static_cast<VkStreamSet>(1u << static_cast<uint32_t>(stream)));
}
// VkStreamNeedsReload for the chain's set: any change of it (a stream joins or leaves) reloads the Session from OFF, never a warm per-group rebuild. Pure.
[[nodiscard]] inline bool VkStreamsNeedReload(VkStreamSet loaded_for, VkStreamSet wanted, bool session_off) {
  return !session_off && loaded_for != 0u && wanted != 0u && wanted != loaded_for;
}
// NGX may set a feature up at its first evaluate from the motion input it is handed, so the live features never see a
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
inline constexpr uint32_t VK_MOTION_RELOAD_RECORDINGS = 10u;  // about a sixth of a second at 60 fps per settings change
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
// While the native context runs a DLSS stage, the helper never drives NR (it is not even attached, so ReShade's effect events stay
// with the native context). The native context runs a DLSS stage only while VulkanNr is Native (DecideVkStages), so
// with VulkanNr Helper the helper always runs. Pure.
[[nodiscard]] inline bool VkHelperRuns(VkNrRoute route, bool native_at_dlss_stage) {
  return route == VkNrRoute::HELPER && !native_at_dlss_stage;
}
// Whether a Vulkan present that reaches the bridged path makes the Direct3D 12 route's VkBridge. Pure.
[[nodiscard]] inline bool VkPresentMakesBridge(VkNrRoute route) {
  return route == VkNrRoute::DIRECT3D_12;
}
// An explicit DLSS stage belongs to native Vulkan. The bridge or helper can execute its Present work while that preference is stored.
[[nodiscard]] constexpr ui::PlacementSource VkPresentSource(ui::PlacementSource stored) {
  return stored == ui::PlacementSource::DLSS ? ui::PlacementSource::AUTO : stored;
}
// NR has exactly one owner per 64-bit Vulkan device at a time.
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
// Why native Vulkan NR at Present cannot run on a device, kept for the session. Its causes exclude one another on a device: what the device lacks
// keeps the native context from being made, a context that could not be made never records, and a native Present records only with its own
// command buffers, whose loss moves Present off the native route before it could record again. So a device has at most one.
struct VkNativeBlock {
  enum class Cause : uint8_t {
    CREATE_FAILURE,   // NGX could not create NR's feature on the game's device at native Present: `detail` is the Session's message, with NGX's result
    START_FAILED,     // the native context could not be made: `detail` is its error; the DLSS stages cannot run either
    PRESENT_BUFFERS,  // Uplift's own command buffers cannot run at Present: `detail` says why; the DLSS stages stay native
    NEEDS,            // what native Vulkan NR needs that the device lacks (NGX's extensions, the hooks...): `detail` names it; no DLSS stage either
  };
  Cause cause = Cause::NEEDS;
  std::string detail;
};
// Why the private Direct3D 12 route cannot run on a device, kept for the session as the native block is.
struct VkD3d12Block {
  enum class Cause : uint8_t {
    CREATE_FAILURE,  // NGX could not create NR's feature on the private Direct3D 12 device: `detail` is the Session's message
    START_FAILED,    // the bridge or its context could not start here: `detail` is their error
  };
  Cause cause = Cause::START_FAILED;
  std::string detail;
};
// Why each route cannot run, as the panel greys it and the route's reason gives it: "NGX could not create NR's feature on the game's Vulkan device
// (...)", "native Vulkan NR could not start (...)" (both start causes), "native Vulkan NR needs ..."; "NGX could not create NR's feature on the
// private Direct3D 12 device (...)", "the private Direct3D 12 device could not start (...)".
[[nodiscard]] std::string VkNativeUnavailable(const VkNativeBlock& block);
[[nodiscard]] std::string VkD3d12Unavailable(const VkD3d12Block& block);
// Whether `block` keeps native NR from the game's DLSS too (what the device lacks, or a context that could not be made), so that no native context is
// made for it; a creation failure at Present or Present's own command buffers leave the DLSS stages native.
[[nodiscard]] bool VkNativeBlocksDlss(const std::optional<VkNativeBlock>& block);
// Why native NR cannot run inside the game's DLSS on the device, for the stages and the log: "NR after DLSS on Vulkan needs ..." or "NR after DLSS
// on Vulkan could not start: ..."; empty when VkNativeBlocksDlss is false.
[[nodiscard]] std::string VkNativeDlssProblem(const std::optional<VkNativeBlock>& block);
// The panel's lock on native Present: the stop's sentence (`stopped`), else the block's own detail, except what the device lacks (the route's reason
// says that); empty: no lock.
[[nodiscard]] std::string VkNativeLockReason(const std::optional<std::string>& stopped, const std::optional<VkNativeBlock>& block);
// What decides it, per device. `native_off` must outlive the call.
struct VkRouteFacts {
  ui::VulkanNrMode setting = ui::VulkanNrMode::NATIVE;
  std::optional<VkNativeBlock> native;  // why native NR cannot run here; nullopt: nothing the device has keeps it off
  // Native NR at Present stopped here (second-queue timeouts, the game's NGX shutdown) or is latched off from an earlier start or a device
  // loss: a whole sentence, used as it is; empty: no.
  std::string_view native_off;
  std::optional<VkD3d12Block> d3d12;  // why the Direct3D 12 route cannot run here; nullopt: nothing keeps it off
};
struct VkRouteChoice {
  VkNrRoute route = VkNrRoute::DIRECT3D_12;
  // Why NR does not run natively (the Direct3D 12 route), or why it runs in the helper (the chain's reasons, joined by "; ", or the setting); empty on the
  // native route.
  std::string reason;
  std::string native_unavailable;  // why Native cannot run here: the panel greys it with this; empty: it can
  std::string d3d12_unavailable;   // why Direct3D 12 cannot run here, the same way; empty: it can
};
// The chain starts at the setting's route. A route that cannot run hands over to the next one: a native impossibility (a creation failure, then
// `native_off`, then the other native blocks: a start failure or what the device lacks) to Direct3D 12, a Direct3D 12 one to the helper, which is final. A creation
// failure stays for the session (the caller keeps it), so the routes never ping-pong. On the Direct3D 12 route a native impossibility outranks the setting's
// own reason. Pure.
[[nodiscard]] VkRouteChoice ChooseVkNrRoute(const VkRouteFacts& facts);
// The log's line when a device's route is decided or changes: "Vulkan: NR at Present runs natively on the game's Vulkan device", "Vulkan: NR at Present
// runs on a private Direct3D 12 device: {reason}", or "Vulkan: NR at Present runs in Uplift's helper process: {reason}".
[[nodiscard]] std::string VkRouteLine(const VkRouteChoice& choice);
// Only native Vulkan NR runs inside the game's DLSS.
// The DLSS stages are greyed, and the native context is given Present whatever is
// stored, only while VulkanNr is Direct3D 12 or Helper, or a failure stops native NR inside the game's DLSS too (what the device lacks, the game's NGX
// shutdown, a loss, an abandon). A failure of native Present alone (its command buffers, the present queue's timeouts, the VulkanNativeNr latch) moves
// Present along the chain and leaves the DLSS stages native and selectable.
// Split chain: `dlss_stage_wanted` is the chain's: some group runs at a DLSS stage natively, which is pass 1's group (groups run in stage order, so
// a chain with one at a DLSS stage has pass 1 there). Either native owner is the NATIVE holder: a split adds no hand-over.
struct VkStageFacts {
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
// A pick of Direct3D 12 or Helper in VulkanNr (from `before` to `after`) while the stored stage is After DLSS or Before upscaling moves the
// stored stage to Present at once, as a click on Present. A pick of Native leaves the stage where it is. Pure.
[[nodiscard]] bool VkRoutePickMovesStage(ui::VulkanNrMode before, ui::VulkanNrMode after, ui::SourcePick stage);

}  // namespace uplift::addon
