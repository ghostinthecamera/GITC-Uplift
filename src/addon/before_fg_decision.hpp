#pragma once

#include <cstdint>
#include <optional>

#include "addon/frame_config.hpp"
#include "addon/streamline_present.hpp"
#include "text/before_fg_messages.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// Before FG policy is decided at the primary route's preparation site, after its earlier return paths and guarded
// mask probe. The decision describes early execution, an outer handoff, a refusal or ordinary preparation. It does
// not claim NR, record commands or advance time. Route adapters apply the explicit writes in order: helper drain
// precedes native preparation, the native sidecar snapshot precedes inner policy writes, and bridge preparation
// clears pending ownership before any bridge-construction return. Refused attempts preserve observed effects; an
// armed snapshot disables them. FgPresentPath carries policy; game_facing_present is a separate observation.
// Recording outcomes remain owned notes until the route's reset policy clears them.

// Primary Present route; API allocation and NR claim ownership stay with the adapter.
enum class BeforeFgRoute {
  DX12,  // The game device records through the observed DXGI handoff.
  VK_NATIVE,  // The game Vulkan device owns Present NR.
  VK_BRIDGE,  // The in-process private D3D12 device owns Present NR.
  VK_HELPER,  // The helper process owns Present NR.
  UNSUPPORTED_BRIDGE,  // This bridge has no real-frame handoff.
};
// Primary adapter action; the route and explicit writes determine what ordinary preparation retains.
enum class BeforeFgState {
  ORDINARY,  // Resume ordinary preparation; an unsupported route makes no handoff writes.
  EARLY,  // ReShade already observes the game-facing Present.
  ARM_OUTER,  // Store this frame for the next observed outer callback.
  UNAVAILABLE,  // Refuse this outer attempt while retaining the requested stage group.
};
// Motion-write prefix, including writes already made when an outer attempt is refused.
enum class OuterInputAction {
  UNCHANGED,  // No motion field was reached, including a mask refusal.
  DISABLE_EFFECT_MOTION,  // Disable effect motion without changing the saved game-stage choice.
  USE_DLSS,  // Save game-stage motion, disable the effect-motion request and use DLSS at Present.
  USE_NONE,  // Save game-stage motion, disable the effect-motion request and disable Present motion.
};

struct BeforeFgInputs {
  BeforeFgRoute route;
  bool enabled;
  ui::Settings::FgPresent requested;
  streamline::FgMode mode;
  uint32_t ngx_live;
  bool observed_fg_off;  // OFF from this Present's one mode read; masks only the FrameFacts detector count.
  bool measured_cadence;
  bool early_confirmed;
  bool early_dxgi_matches;  // The verified early DXGI chain is this primary chain.
  bool inside_outer_present;  // Nested forwarding cannot also qualify as an early DXGI Present.
  bool outer_matches_primary;  // The observed outer binding points to this accepted primary chain.
  bool hooks_installed;
  bool handoff_pending;  // This route already has a pending payload, so rearming retains its note.
  bool mask_conflicts;  // The guarded runtime probe found a declared mask whose effect is running.
  ui::MotionVectorSource motion_source;
  ui::Settings::FgMotion before_fg_motion;
  bool upscaler_live;  // This device has live DLSS on the API whose motion the outer recording uses.
};

struct BeforeFgDecision {
  BeforeFgState state = BeforeFgState::ORDINARY;
  bool active = false;  // Primary route activation, including measured cadence only on DX12.
  bool outer_motion_candidate = false;
  bool clear_pending = false;
  OuterInputAction inputs = OuterInputAction::UNCHANGED;
  // nullopt retains the current note; NONE clears it.
  std::optional<text::BeforeFgNote> note;
};

// Preparation reads these common facts before the primary route reaches its guarded input probe.
// The NGX count FrameFacts hands the frame-generation detector; activation never reads this mask.
[[nodiscard]] uint32_t BeforeFgConfigCount(const BeforeFgInputs& inputs);
// The one activation predicate for every route and preparation step; measured cadence counts only on DX12.
[[nodiscard]] bool BeforeFgActive(const BeforeFgInputs& inputs);
[[nodiscard]] bool BeforeFgOuterMotionCandidate(const BeforeFgInputs& inputs);
[[nodiscard]] bool BeforeFgChecksOuterInputs(const BeforeFgInputs& inputs);

[[nodiscard]] BeforeFgDecision DecideBeforeFg(const BeforeFgInputs& inputs);
// SELECT maps to USE_NONE; a new host handoff refuses SELECT before calling this projection.
[[nodiscard]] OuterInputAction OuterMotionAction(ui::MotionVectorSource source, ui::Settings::FgMotion choice);
// The motion fields Before FG's outer present overrides in a frame's settings, and the game stage's choice it keeps in its facts.
void ApplyOuterInputs(FrameSettings& settings, FrameFacts& facts, OuterInputAction action);

}  // namespace uplift::addon
