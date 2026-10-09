#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "color/encoding.hpp"
#include "nr/budget.hpp"
#include "nr/session_status.hpp"
#include "nr/types.hpp"
#include "ui/settings.hpp"

namespace uplift::ui {

// Plan 14 (design §1.2, the user's decision 1: "a user shouldnt be able to select a state which is not possible in the setup"): one option of a Setup
// row. Empty `why`: selectable. Otherwise greyed, unclickable, and `why` is the tooltip after "Unavailable here: ".
struct OptionState {
  std::string_view why;
  bool temporary = false;  // the cause can clear by itself this session (DLSS starting, Ray Reconstruction off, Launchpad turned on)
};
inline constexpr std::string_view DLSS_NOT_SEEN_REASON = "Available once the game renders with DLSS";
// Plan 18 Task 12: the game switched its DLSS off (it released its last DLSS feature, or on Direct3D 11 shut NGX down) after DLSS ran on this device: the
// DLSS stages, DLSS's vectors and Match game wait for it (a temporary cause: SetupFacts::dlss_off), and NR runs at Present, at Full, meanwhile.
inline constexpr std::string_view DLSS_OFF_REASON = "The game's DLSS is off: turn it on in the game's settings";
// On Vulkan at Present the game's DLSS motion vectors reach NR through copies made in the game's frame, on a GPU-ordered bridge only: a CPU-ordered Vulkan
// bridge holds the add-on's lock across capped CPU waits, which a hooked DLSS evaluate must not wait on.
inline constexpr std::string_view VULKAN_CPU_ORDERED_MOTION = "Needs the GPU-ordered Vulkan bridge, which this device cannot have (Details says why once NR has run)";
// The game presents from a second queue: the bridge would wait a GPU frame inside the present event for every copy.
inline constexpr std::string_view VULKAN_SECOND_QUEUE_MOTION = "The game presents from a second queue, where copying DLSS's vectors would stall its render thread";
// Final review I-1: the native Vulkan context stopped for good (the game shut NGX down on this device, or the device was lost). The DLSS stages and DLSS's
// vectors cannot run again this session (SetupFacts::dlss_unavailable).
inline constexpr std::string_view VULKAN_NGX_SHUT_DOWN_REASON =
    "The game shut NVIDIA's NGX down on this device (or the device was lost): NR inside the game's frame stays off until the game restarts";
// And while NR sat at a DLSS stage then, Present cannot run either: the native context keeps NR's claim, so the Present path never loads NR into a game that
// may be quitting (Plan 13's exit protection; SetupFacts::present_fixed).
inline constexpr std::string_view VULKAN_NGX_SHUT_DOWN_PRESENT_REASON =
    "The game shut NVIDIA's NGX down on this device (or the device was lost) while NR ran inside its frame: NR stays off until the game restarts";
// Plan 15: on Direct3D 12 the game shut NVIDIA's NGX down on its device (some games do when a DLSS setting changes, every Streamline game when it quits):
// Uplift released NR first and holds it off there, every stage alike, until the game creates its DLSS again (a temporary cause: SetupFacts::held, CardFacts::held).
inline constexpr std::string_view D3D12_NGX_SHUT_DOWN_REASON = "The game shut NVIDIA's NGX down; NR resumes when the game's DLSS starts again";
// Plan 15: the same shutdown came while NR's last frames had not finished within 2 s, so NR was abandoned without an NGX call: nothing runs again on the device
// this session (fix round, minor 6: SetupFacts::stopped greys every option with it, and the card is "NR stopped" with it).
inline constexpr std::string_view D3D12_NGX_ABANDONED_REASON =
    "The game shut NVIDIA's NGX down while NR's work was still running: NR stays off on this device until the game restarts";
// Plan 17 (the 2-strike rule): NR's private Direct3D 12 device (a bridge's, or the 64-bit helper's) stopped a second time this session, so Retry now is no
// longer offered: the card's reason, and Setup's fixed cause (SetupFacts::stopped).
inline constexpr std::string_view PRIVATE_DEVICE_STOPPED_TWICE_REASON = "NR stopped twice this session: it stays off on this device until the game restarts";
// Test phase (review M-3): a bridge's private device that is the adapter's shared one (no device factory in this game's D3D12 runtime) stopped. No new device
// can be made while the game runs, so the first stop is final: Setup's fixed cause, and the log's line.
inline constexpr std::string_view PRIVATE_DEVICE_STOPPED_FINAL_REASON =
    "NR stopped, and this game's Direct3D 12 runtime cannot give Uplift a new device: it stays off on this device until the game restarts";
// Final review, minor 3: on Vulkan at Present Match game cannot read the game's DLSS render size, but a DLSS stage can (a temporary cause).
inline constexpr std::string_view VULKAN_MATCH_GAME_REASON = "On Vulkan, Match game applies at After DLSS and Before upscaling";
// Plan 18 (design §5): in a Direct3D 11 game whose DLSS runs on a separate Direct3D 12 device (a mod's own), which Uplift cannot reach: a fixed cause for the
// DLSS stages and DLSS's vectors (SetupFacts::dlss_unavailable). NR at Present runs as before.
inline constexpr std::string_view FOREIGN_D3D12_DLSS_REASON =
    "This game's DLSS runs on a separate Direct3D 12 device (a mod), which Uplift cannot reach. NR runs at Present";

// A selectable motion source that delivered nothing on the latest recording (design §1.4).
enum class MotionGap : uint8_t {
  NONE,
  GAME_PASSED_NONE,  // the game's evaluate carried no readable motion vectors
  NO_COPY,           // DLSS's vectors exist, but no copy of them reached the Present path's recording (a busy slot: the GPU runs behind)
  NO_UPLIFT_MV,      // Launchpad's UPLIFT_MV did not arrive at the recording (Uplift.fx's Uplift technique is not below Launchpad)
  DLSS_PAUSED,       // final review, minor 1: no copy because the game's DLSS has not run for a while (a menu, a loading screen)
};

// What Setup and the card need about the device the overlay shows (design §1.2-1.4). Views must outlive ResolveSetup and BuildLiveCard.
struct SetupFacts {
  // The stored preference: read, never written.
  SourcePick preferred_stage = SourcePick::AFTER_DLSS;  // SourcePickOf(settings, explicit_dlss)
  MotionPick preferred_motion = MotionPick::DLSS;       // MotionPickOf(settings)
  bool motion_auto = true;                              // MotionVectors = Auto: DLSS, else Launchpad, else Lumenite, else Off
  ResolutionMode preferred_resolution = ResolutionMode::FULL;
  float custom_scale = 100.f;
  // What is possible now (§1.2).
  std::string_view dlss_unavailable;  // fixed: the bridges, OpenGL, the helper, hooks, ReShade, the latch, Vulkan's native problems
  // Plan 18 (design §3): a DLSS stage that cannot run here although DLSS runs (the Direct3D 11 bridge cannot share its image): a fixed cause for that stage
  // alone; Present and DLSS's vectors stay as they are.
  std::string_view after_dlss_fixed;
  std::string_view before_upscaling_fixed;
  bool dlss_seen = false;             // sticky for the session
  bool dlss_off = false;              // Plan 18 Task 12: seen, and the game switched its DLSS off since (DLSS_OFF_REASON); a pause is not off
  bool ray_reconstruction = false;    // the main DLSS feature is Ray Reconstruction
  bool frame_generation_blocks_present = false;
  std::string_view present_fixed;      // final review I-1: Present cannot run this session (VULKAN_NGX_SHUT_DOWN_PRESENT_REASON)
  // Plan 15: NR is held off on this Direct3D 12 device until the game's DLSS starts again (D3D12_NGX_SHUT_DOWN_REASON): every stage and DLSS's and
  // Launchpad's vectors wait (temporary), except the highlighted ones; a fixed cause keeps its own text.
  std::string_view held;
  // Plan 15 fix round (minor 6): nothing can run again on this device this session (D3D12_NGX_ABANDONED_REASON): every option of the three rows is greyed
  // with it, a fixed cause, the highlights included (none of them runs).
  std::string_view stopped;
  bool game_passes_motion = true;      // false while the latest main evaluate carried no readable motion vectors
  std::string_view dlss_motion_fixed;  // Vulkan at Present on a CPU-ordered bridge: DLSS's motion vectors cannot reach NR there (VULKAN_CPU_ORDERED_MOTION)
  bool launchpad_ready = false;        // Launchpad's technique and Uplift.fx's Uplift technique are enabled
  std::string_view launchpad_fixed;    // Launchpad's vectors cannot reach NR on this device: a 32-bit Direct3D 10 or 11 device without fences shares no images
  bool lumenite_ready = false;         // 2026-10-08: Lumenite's Kernel and Uplift.fx's Uplift technique are enabled
  std::string_view lumenite_fixed;     // Lumenite's vectors cannot reach NR on this device (Direct3D 9, or no images shared, as Launchpad's)
  bool uplift_mv_lumenite = false;     // UPLIFT_MV holds Lumenite's vectors now (the running provider reads as Lumenite, not Launchpad)
  bool match_game_readable = true;     // false on the bridges, OpenGL, the helper and Vulkan at Present
  bool match_game_at_dlss_stages = false;  // final review, minor 3: unreadable here, but a DLSS stage on this device reads it (Vulkan at Present)
  std::string_view below_full_fixed;   // why the modes below Full cannot run (fixed); no path sets it since Plan 14 Task 8
  // What runs (§1.3).
  std::optional<SourcePick> stage;   // the shown context's decided placement; none without a context
  std::optional<MotionPick> motion;  // the latest recording's provider while NR works; none otherwise
  MotionGap dlss_motion_gap = MotionGap::NONE;
  MotionGap launchpad_gap = MotionGap::NONE;
  std::optional<ResolutionMode> resolution;  // the shown context's applied mode
  // The card's detail and footer.
  bool motion_copied = false;  // DLSS's vectors, copied for Present
  color::Upsampling upsampling = color::Upsampling::EDGE_AWARE;
  nr::Size work;    // the settled work image
  nr::Size canvas;  // NR's size: `work`, or the floor canvas it is padded into
  nr::Size frame;   // what the mode scales: the output region; Before upscaling, the render region
  std::string_view api;
  uint32_t passes_run = 0u;
  uint32_t passes_requested = 1u;
  uint64_t vram_bytes = 0u;
  std::string_view latch_note;  // the note line when there is no Why: the first non-empty, in this order
  std::string_view passes_note;
  std::string_view frame_generation_line;
};

struct SetupView {
  std::array<OptionState, 3> stage_options = {};       // SourcePick order
  std::array<OptionState, 4> motion_options = {};      // MotionPick order
  std::array<OptionState, 6> resolution_options = {};  // ResolutionMode order
  SourcePick stage = SourcePick::PRESENT;              // the highlights: what runs (§1.3)
  MotionPick motion = MotionPick::OFF;
  ResolutionMode resolution = ResolutionMode::FULL;
  std::string why;  // without "Why: "; empty unless a highlight is a temporary fallback from the preference
  bool stopped = false;  // Plan 15 fix round: SetupFacts::stopped; the highlights stay greyed too (SetupSettle never un-greys them)
};
// Every option's state, the three highlights and the Why line, from the facts alone (design §1.2-§1.4): pure.
[[nodiscard]] SetupView ResolveSetup(const SetupFacts& facts);

// The working card's rows (they say what `shown` highlights), its note and its footer.
struct LiveCard {
  std::string stage;
  std::string motion;
  std::string resolution;
  std::string note;
  std::string footer;  // "Direct3D 12 · 1/1 pass · VRAM 412 MiB" (the separator is U+00B7, a Latin-1 character)
};
[[nodiscard]] LiveCard BuildLiveCard(const SetupFacts& facts, const SetupView& shown);

// R89: the highlights the overlay draws. An engine-driven change shows once it has lasted `settle`; a click shows at once.
enum class SetupRow : uint8_t {
  STAGE,
  MOTION,
  RESOLUTION,
};
class SetupSettle {
 public:
  // The view to draw: `resolved`'s options, with each highlight settled. While a shown highlight differs from `resolved`'s the Why is empty
  // (it belongs to the resolved highlights) and the shown option is never greyed. After a gap of twice `settle` (the overlay was closed) the
  // resolved highlights show at once.
  [[nodiscard]] SetupView Update(SetupView resolved, std::chrono::steady_clock::time_point now,
                                 std::chrono::milliseconds settle = std::chrono::milliseconds(500));
  void Clicked(SetupRow row, uint32_t index);
  // A Reset writes the stored default as a click does, but the default's option may not be able to run now (After DLSS before the game's DLSS is seen on
  // Direct3D 12): the highlight then goes at once to the best option that can, by ResolveSetup's own fallbacks (Before upscaling to After DLSS, a DLSS stage
  // to Present; DLSS's motion vectors, under Auto, to Launchpad where it can run, else Off; a mode that cannot run to Full), never to the one that cannot,
  // which the settle would move off after half a second. `options` is the view being drawn; do the stage row before the motion row (Launchpad runs at
  // Present only).
  void ClickedBest(SetupRow row, uint32_t index, const SetupView& options, bool motion_auto = false);
  void Reset() { *this = {}; }

 private:
  struct Row {
    std::optional<uint32_t> shown;
    std::optional<uint32_t> candidate;
    std::chrono::steady_clock::time_point since;
  };
  std::array<Row, 3> rows_ = {};
  std::optional<std::chrono::steady_clock::time_point> last_update_;
};

// Plan 7: the DLSS placements' reason on a bridged context without them (the Direct3D 10 bridge, OpenGL, the 32-bit helper); Plan 18: Direct3D 11 has them.
// Plan 9 moved it here, from addon/device_context.hpp, so the 32-bit add-on shows the same text without that header.
inline constexpr std::string_view BRIDGED_DLSS_REASON = "NR after DLSS needs a 64-bit Direct3D 11, Direct3D 12 or Vulkan game";

struct StatusView {
  nr::SessionState state = nr::SessionState::OFF;
  bool suspended = false;
  uint32_t passes_requested = 1u;
  uint32_t passes_run = 0u;
  nr::Size frame;
  std::string_view trigger;                 // where NR ran; empty when it has not
  std::optional<color::Encoding> encoding;  // resolved
  float diffuse_white_nits = 0.f;           // effective; never shown for sRGB
  std::chrono::milliseconds grace_remaining{0};  // only meaningful while state == GRACE
};

// One status line for the overlay; only ACTIVE shows the detail.
[[nodiscard]] std::string FormatStatusLine(const StatusView& view);

// v2 design §3.5: "Frame generation: on (2x, NGX)", "Frame generation: on (3x)", "Frame generation: off".
[[nodiscard]] std::string FormatFrameGenerationLine(bool active, uint32_t multiplier, bool from_ngx);
// With frame generation on, every pass beyond the first can push it past its frame budget ([V7] §4.2).
[[nodiscard]] std::optional<std::string> FrameGenerationWarning(bool active, uint32_t passes);

// Keep faces (2026-10-08): what decides whether Keep faces can run on the shown device. NR's Character mask (DLSSNR.UseAutoMask), which the extra run
// needs, is forced off by NR whenever a DLSSNR.ControlMask is bound; Uplift binds none anywhere (its NR mask, UPLIFT_MASK, is applied after NR, in the
// compose), so that never greys it.
struct KeepFacesFacts {
  bool nr_runs = true;       // Uplift runs NR on this kind of device
  bool gpu_ready = true;  // the GPU loads and stores the formats the recombination needs (as the look stage's), and its pipeline was built
};
// Why Keep faces cannot run on the shown device, or empty.
[[nodiscard]] std::string_view KeepFacesUnavailable(const KeepFacesFacts& facts);

// Plan 6 (v2 design §3.18, D1): the status card, the first failing stage of the verdict ladder.
enum class CardStage : uint8_t {
  DEVICE,
  CONFLICTS,
  RUNTIME,
  SWITCH,
  SESSION,
  PLACEMENT,
  BUDGET,
  FRAME,
  EVALUATE,
  WORKING,
};
enum class CardButton : uint8_t {
  NONE,
  TURN_ON,
  RETRY_NOW,  // with the device-removal latch set, the overlay says "Retry now and clear the latch"
  CLEAR_LATCH,
};
struct StatusCard {
  CardStage stage = CardStage::SWITCH;
  bool working = false;
  std::string title;
  std::string reason;
  std::vector<std::string> fixes;  // at most two
  CardButton button = CardButton::NONE;
};

// What the add-on knows about one device, flattened so the ladder stays testable. Views must outlive the call.
struct CardFacts {
  std::string_view device_problem;  // no context here: not D3D12, not NVIDIA, could not start, or no frame yet
  // Plan 9: the 32-bit add-on's 64-bit helper exited, hung or could not start; checked right after device_problem, and only
  // while `enabled` (with NR off the card is the normal off state; the failure shows again when NR is turned on).
  std::string_view helper_problem;
  // Plan 17: NR's private Direct3D 12 device (a bridge's) stopped: its device was removed or hung, or the CPU-ordered timeouts. While `enabled` the card offers
  // Retry now. `private_stops_final`: it stopped twice this session (the 2-strike rule), so the card says so instead, whatever `enabled` is; the helper's
  // second stop sets it too.
  std::string_view private_stopped;
  bool private_stops_final = false;
  // Test phase (review M-3): the stop in `private_stopped` cannot be retried (the adapter's shared device): the card says to restart, with no Retry now,
  // whatever `enabled` is.
  bool private_stop_unretryable = false;
  bool device_lost = false;
  std::string_view stopped;  // Plan 15 fix round (minor 6): NR stopped on this device for the session (D3D12_NGX_ABANDONED_REASON); checked after `device_lost`
  std::string_view blocked;  // a conflicting host, a missing runtime, another NR producer
  CardStage blocked_stage = CardStage::CONFLICTS;
  bool runtime_in_game_folder = false;  // 1.1.4: `blocked` says NVIDIA's DLSS loaded nvngx_dlssnr.dll from the game's folder first
  bool claimed_elsewhere = false;  // NR runs on another D3D12 device in this game
  bool enabled = false;
  std::string_view held;  // Plan 15: why NR waits for the game's DLSS after the game's NGX shutdown (D3D12_NGX_SHUT_DOWN_REASON); checked right after `enabled`
  nr::SessionStatus session;
  std::string_view output_problem;  // a device-removal message, an unsupported back-buffer format or colour space
  std::string_view placement_note;  // why no placement runs, or DLSS idle
  bool dlss_latched = false;
  bool nr_applied = false;
  uint32_t passes_run = 0u;
  std::string_view skip_reason;
  bool skip_from_session = false;
  std::string_view working_line;  // FormatStatusLine, for a working card
  nr::VramCheck vram_check = nr::VramCheck::CAREFUL;  // 2026-10-09: the video memory cards offer the looser checks there are
  // Plan 9: the add-on's own file name, for the RUNTIME card's fix ("next to gitc-uplift.addon64 or the game").
  std::string_view addon_file = "gitc-uplift.addon64";
};
[[nodiscard]] StatusCard BuildStatusCard(const CardFacts& facts);

}  // namespace uplift::ui
