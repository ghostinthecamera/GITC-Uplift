#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/controls_coalescer.hpp"
#include "ui/settings.hpp"
#include "ui/status_text.hpp"

namespace uplift::ui {

// What the panel shows besides the settings.
struct OverlayView {
  std::string status_line;
  uint64_t intermediate_bytes = 0u;
  std::optional<uint64_t> runtime_bytes;  // the NR runtime's own allocation, when its stats report it
  float effective_diffuse_white_nits = 0.f;
  uint32_t last_key_pressed = 0u;         // effect_runtime::last_key_pressed, for hotkey capture
  // The lines the Details header shows after the status line, also promoted into the status card while it is
  // working (its fixed height has spare lines then). Each is the engine's or the bridge's own text.
  std::string api_line;  // the Direct3D 11/10 bridge's or the helper front's line; empty on native Direct3D 12
  std::string frame_generation_line;     // FormatFrameGenerationLine; empty before a context exists and in 32-bit games
  bool frame_generation_active = false;  // the working card shows frame_generation_line only when this is set
  bool dlss_latched = false;              // DlssPlacementBlocked: the device-removal latch is set
  std::string ui_correction_note;  // Plan 5 (D7): why UI correction does nothing on this placement
  StatusCard card;  // Plan 6 (D1): replaces the message line at the top
  // The engine's own lines, which Details lists under the status line (Plan 14: the rows no longer repeat them as "Now: ..." readouts).
  std::string placement_line;  // FormatPlacementLine; "Placement: NR is off" before a device exists
  std::string motion_line;     // "Motion vectors: DLSS (the game's own, scale 1 x 1)"; never empty
  std::string work_line;       // "Working at 1920x1080 of 3840x2160 (Edge-aware)"; empty before a frame
  std::string exposure_line;   // Plan 17: "Input exposure: metered (the game's exposure was 6.3 stops off)"; empty when none is used
  std::string mask_note;       // "NR mask: UPLIFT_MASK (1920x1080)", or why not
  std::string frame_generation_warning;  // shown under Pass count when it applies; empty otherwise
  // ui-review.md §4.1: why no DLSS placement can run this session (bridged, hooks, ReShade version, the
  // latch); empty when they can. Greys Game state after NR (the NR stage and Motion vectors toggles read it through Setup).
  std::string dlss_unavailable;
  // Plan 13 (the user's decision 1): a Vulkan device, where Auto stays at Present and Before upscaling and After DLSS are explicit choices
  // (SourcePickOf's `explicit_dlss`); their tips name the one-time cost.
  bool dlss_explicit = false;
  // Plan 9 (design §2.9): the Direct3D 9Ex row. `d3d9ex_unavailable` non-empty greys it ("Only for 32-bit Direct3D 9
  // games" in gitc-uplift.addon32, "Only for Direct3D 9 games" in gitc-uplift.addon64 since Plan 10) and is what shows under it; otherwise
  // `d3d9ex_readout` ("Now: ...") does.
  std::string d3d9ex_readout;
  std::string d3d9ex_unavailable;
  // Plan 14 (design §1): Setup (every option's state, the three highlights, the Why) and the working card's rows, from FinishSetup. `setup`'s views point
  // into this view's own strings (and static text).
  SetupView setup;
  LiveCard live;
};

// Panel state kept across frames.
struct OverlayState {
  DragState drag;
  bool capturing_key = false;
  // Set on the frame a new hotkey is captured. That key press is still "pressed" in the same
  // frame's input snapshot, so the hotkey handler must skip it once instead of toggling Enable.
  bool key_captured_this_frame = false;
  // 1.1.0: when DrawOverlay last showed the capture; a capture not shown for a moment (the overlay closed, another tab) ends (ExpireKeyCapture).
  std::chrono::steady_clock::time_point capture_seen;
  bool defaults_view = false;   // Plan 6 (D6): session only; the add-on keeps it when the overlay closes
  bool confirm_restore = false;  // G2: the first click of Restore all defaults
  bool retry_now = false;        // D11: the add-on calls DeviceContext::RetryNow after DrawOverlay and clears it
  // An APPLY_ON_RELEASE slider being held: the value it shows. The setting keeps the applied one until release.
  std::optional<int> pending_release;
  std::string_view pending_key;  // the held row's key (schema keys are static)
  // Plan 14 (R89): the Setup highlights' settle, for the device they belong to (a change of device starts it afresh).
  SetupSettle setup_settle;
  const void* setup_device = nullptr;
};

// Plan 14: resolves Setup from `facts` (the shown device's), settles its highlights in `state` and, for a working card, builds the card's rows. Sets
// `view->setup` and `view->live`; `view->card` must be set already. `device`: the shown device's identity. `facts`' views must outlive the draw.
void FinishSetup(OverlayView* view, OverlayState* state, const void* device, const SetupFacts& facts, std::chrono::steady_clock::time_point now);

// Draws the Uplift panel (spec §12) inside the window ReShade opened for it, editing `settings`
// in place. Returns true when a setting changed; the caller then sanitises, saves and applies.
bool DrawOverlay(const OverlayView& view, Settings* settings, OverlayState* state);

// 1.1.0: a hotkey capture the overlay has not shown for a moment (Esc closed ReShade's overlay, or another tab is open) ends, keeping the old hotkey, so
// the hotkey (which a capture holds off) works again. Called at each present.
inline void ExpireKeyCapture(OverlayState* state, std::chrono::steady_clock::time_point now) {
  if (state->capturing_key && now - state->capture_seen > std::chrono::milliseconds(250)) {
    state->capturing_key = false;
  }
}

}  // namespace uplift::ui
