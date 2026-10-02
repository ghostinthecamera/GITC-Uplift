#pragma once

#include <chrono>
#include <optional>

namespace uplift::addon {

// Plan 10 (design §4.4, verdict M: Task 1 showed LaunchPad compiles on ReShade's D3D9 backend, so the gate stays on): the ONE switch for
// LaunchPad on Direct3D 9. On: both hosts' link blocks include D3D9 devices (Uplift sets UPLIFT_USE_LAUNCHPAD for them),
// HelperFront::Technique passes UPLIFT_MV to NR, and the motion readout is the helper's. Off: no D3D9 link, no D3D9 motion copy, and the
// readout says "Motion vectors: none (Launchpad does not run on Direct3D 9)". Turn it off only if LaunchPad turns out to fail on someone's
// D3D9 setup. fx/Uplift.fx's own `>= 0x9000` gate only matters to a user who sets UPLIFT_USE_LAUNCHPAD by hand; set that gate back to
// `>= 0xa000` together with this switch.
inline constexpr bool LAUNCHPAD_ON_D3D9 = true;

// What the link reads at a present.
struct LaunchPadLinkFrame {
  // ReShade is not (re)loading effects, and Uplift.fx is among them: only then may UPLIFT_USE_LAUNCHPAD be set.
  bool ready = false;
  bool wanted = false;  // Motion vectors can use LaunchPad, and LaunchPad's technique is enabled
};

// User-approved addition, final review I-1: when Uplift sets UPLIFT_USE_LAUNCHPAD itself. Every change reloads
// Uplift.fx (a one-time hitch the user accepted), so a value is set only when the wanted state really changes, and
// compared with the value Uplift itself last set, never with ReShade's read-back. The first ready frame after the
// link's own set never sets again: that reload re-applies the preset's technique states, which can flip `wanted`
// (a LaunchPad toggled but not saved), and the link must not answer its own reload with another one. Pure.
class LaunchPadLink {
 public:
  // At every present: the value to set now, or nullopt.
  [[nodiscard]] std::optional<bool> Update(const LaunchPadLinkFrame& frame);
  // A new runtime, a runtime reinit, or a new preset: the next ready frame decides afresh, once.
  void Reset() { *this = {}; }

 private:
  std::optional<bool> set_;   // the value Uplift last set on this runtime
  std::optional<bool> seen_;  // `wanted` at the last ready frame
  bool settling_ = false;     // Uplift set a value, and no ready frame has followed yet
};

// Plan 14 (batch 1 review, minor 6): Setup's "Launchpad is ready" (its technique and Uplift.fx's Uplift technique are both enabled). ReShade lists no technique
// while it reloads effects, which choosing Launchpad does (the link flips UPLIFT_USE_LAUNCHPAD): a loading frame must not read as "turned off", or the option
// flickers Launchpad, Off, Launchpad within a second or two. While the techniques are not listed the last ready value holds for HOLD; a technique that is
// listed and disabled is off at once. Pure.
class LaunchpadReadiness {
 public:
  static constexpr std::chrono::seconds HOLD{10};
  // At every present with the effects on. `listed`: both techniques are found. `ready`: and both are enabled.
  [[nodiscard]] bool Update(bool listed, bool ready, std::chrono::steady_clock::time_point now);
  // A new runtime or a new preset: nothing is held over.
  void Reset() { *this = {}; }

 private:
  std::optional<std::chrono::steady_clock::time_point> ready_at_;  // the last present that was ready; none while off
};

}  // namespace uplift::addon
