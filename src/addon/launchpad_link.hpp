#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "ui/settings.hpp"

namespace uplift::addon {

// Plan 10 (design §4.4, verdict M: Task 1 showed LaunchPad compiles on ReShade's D3D9 backend, so the gate stays on): the ONE switch for
// LaunchPad on Direct3D 9. On: both hosts' link blocks include D3D9 devices (Uplift sets UPLIFT_USE_LAUNCHPAD for them),
// HelperFront::Technique passes UPLIFT_MV to NR, and the motion readout is the helper's. Off: no D3D9 link, no D3D9 motion copy, and the
// readout says "Motion vectors: none (Launchpad does not run on Direct3D 9)". Turn it off only if LaunchPad turns out to fail on someone's
// D3D9 setup. fx/Uplift.fx's own `>= 0x9000` gate only matters to a user who sets UPLIFT_USE_LAUNCHPAD by hand; set that gate back to
// `>= 0xa000` together with this switch.
inline constexpr bool LAUNCHPAD_ON_D3D9 = true;

// Lumenite (owner, 2026-10-08): LumeniteFX's Kernel computes a 1/8-resolution optical flow (Kernel::tFlow, current -> previous in UV) and a confidence
// (Kernel::tConfidence) once per frame for every effect that redeclares them. Uplift.fx, compiled with UPLIFT_USE_LUMENITE, upsamples them into UPLIFT_MV
// exactly where it writes Launchpad's, so every path that reads UPLIFT_MV takes them as they are. Only Lumenite's own technique, Kernel, makes them: an
// effect that merely imports its textures does not count. Off on Direct3D 9: Uplift.fx's Lumenite pass is compiled from Direct3D 10 on only (its nine-cell
// edge-aware upsample is not checked on shader model 3), so the Lumenite choice is greyed there with LUMENITE_D3D9_REASON.
inline constexpr bool LUMENITE_ON_D3D9 = false;
inline constexpr char LUMENITE_TECHNIQUE[] = "Lumenite_Kernel";
inline constexpr std::string_view LUMENITE_D3D9_REASON = "Lumenite's vectors are not used on Direct3D 9";

// Which source Uplift.fx compiles into UPLIFT_MV (UPLIFT_USE_LAUNCHPAD or UPLIFT_USE_LUMENITE, one LaunchPadLink each): never both. `launchpad_on` and
// `lumenite_on`: the technique that makes them is enabled (MartysMods_Launchpad, Lumenite_Kernel) where Uplift can use it. Auto takes Launchpad when both
// are on (the owner's default), else Lumenite; an explicit choice takes only its own; DLSS and None take neither. Pure.
struct UpliftMvSources {
  bool launchpad = false;
  bool lumenite = false;
};
[[nodiscard]] UpliftMvSources ChooseUpliftMvSources(ui::MotionVectorSource setting, bool launchpad_on, bool lumenite_on);
// MotionVectors can use UPLIFT_MV at all (Auto, Launchpad, Lumenite): only then is it copied for NR (a 32 MiB copy at 4K otherwise). Pure.
[[nodiscard]] bool MotionUsesUpliftMv(ui::MotionVectorSource setting);

// 1.2.1: the source Uplift.fx is compiled with, which may not be the one wanted. A link never answers its own reload, so after a reload that re-applies a
// preset (auto-save off) a define can stay set while its technique is off: UPLIFT_MV then holds that source's last vectors, frozen.
enum class UpliftMvSource : uint8_t {
  NONE,
  LAUNCHPAD,
  LUMENITE,
};
// From the two defines as they stand (`*_now`: the value a link set this frame, else the definition read, false where unknown) and whether the device can
// use each source; Uplift.fx's own #if order: LaunchPad's pass wins when both are set. Pure.
[[nodiscard]] UpliftMvSource CompiledUpliftMv(bool launchpad_here, bool launchpad_now, bool lumenite_here, bool lumenite_now);
// UPLIFT_MV holds this frame's vectors only while the technique of the source Uplift.fx is compiled with is on; otherwise the readers treat it as no
// UPLIFT_MV this frame. `launchpad_on` / `lumenite_on`: MartysMods_Launchpad / Lumenite_Kernel is enabled. Pure.
[[nodiscard]] bool UpliftMvValid(UpliftMvSource compiled, bool launchpad_on, bool lumenite_on);

// What the link reads at a present.
struct LaunchPadLinkFrame {
  // ReShade is not (re)loading effects, and Uplift.fx is among them: only then may UPLIFT_USE_LAUNCHPAD be set.
  bool ready = false;
  bool wanted = false;  // Motion vectors can use LaunchPad, and LaunchPad's technique is enabled
  // 1.0.1 (test phase): the UPLIFT_USE_LAUNCHPAD Uplift.fx compiles with now, as far as Uplift can tell (LaunchPadDefinition): false without any
  // definition (Uplift.fx's own #ifndef), and nullopt, "unknown", for a value other than exactly 0 or 1, which never vetoes a set.
  std::optional<bool> current;
};

// User-approved addition, final review I-1: when Uplift sets UPLIFT_USE_LAUNCHPAD itself. Every change reloads
// Uplift.fx (a one-time hitch the user accepted), so a value is set only when the wanted state really changes, and
// compared with the value Uplift itself last set, never with ReShade's read-back. The first ready frame after the
// link's own set never sets again: that reload re-applies the preset's technique states, which can flip `wanted`
// (a LaunchPad toggled but not saved), and the link must not answer its own reload with another one.
// 1.0.1 (test phase): ReShade's value (`current`) only vetoes a set that would change nothing, when it is known. Every set recompiles Uplift.fx, a
// single-effect reload that can leave other add-ons holding stale handles, and a game without LaunchPad got one at every start (0 over Uplift.fx's own 0).
// Pure.
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

// 1.0.1 (the Launchpad investigation, F3): the INFO line when the link sets UPLIFT_USE_LAUNCHPAD. Each set makes ReShade recompile Uplift.fx, which
// frees and rebuilds its technique and texture lists for every effect: an add-on that keeps ReShade handles across that and does not refresh them on
// reshade_reloaded_effects is left holding freed memory. Uplift keeps none (it looks every handle up again each frame); the line says so in the log, in
// both halves, at the moment it happens.
[[nodiscard]] std::string LaunchPadLinkLine(bool value);
// The same for UPLIFT_USE_LUMENITE: "Lumenite link: UPLIFT_USE_LUMENITE = 1; ...".
[[nodiscard]] std::string LumeniteLinkLine(bool value);

// 1.0.1 (test phase, review I-4): the UPLIFT_USE_LAUNCHPAD that Uplift.fx compiles with. ReShade takes the effect scope's definition first, then the
// preset's, then the global one. `effect_value` is the effect scope's (get_preprocessor_definition_for_effect on "Uplift.fx", which reads that scope only),
// and `outer_value` the preset's or global one (get_preprocessor_definition); nullopt where there is none. With neither, false (Uplift.fx's #ifndef).
// Known only for exactly "0" or "1"; any other value (empty, " 1", "0x1") is nullopt, unknown, so the link sets its own value over it.
[[nodiscard]] std::optional<bool> LaunchPadDefinition(std::optional<std::string_view> effect_value, std::optional<std::string_view> outer_value);

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
