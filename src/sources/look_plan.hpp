#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "color/encoding.hpp"
#include "look/look_math.hpp"
#include "nr/types.hpp"

namespace uplift::sources {

inline constexpr size_t LATER_PASSES = nr::MAX_PASSES - 1u;

// Pass n ≥ 2's own Transfer and Colour strength between passes; (1, 1) resolves nothing.
struct PassStrengths {
  float transfer = 1.f;
  float color = 1.f;
  friend bool operator==(const PassStrengths&, const PassStrengths&) = default;
};

// Colour preparation, exposure and final composition settings.
struct ColorFixes {
  color::Primaries primaries = color::Primaries::AUTO;
  float linear_unit_nits = 0.f;  // 0 = automatic
  color::InputExposure input_exposure = color::InputExposure::GAME;
  // Auto with a valid game exposure. The default here is Switch (the settings' default is Blend); `auto_blend` is 0 the game's, 1 the meter's.
  color::AutoExposureMode auto_mode = color::AutoExposureMode::SWITCH;
  float auto_blend = 0.5f;
  bool smooth_adapt = true;      // ExposureAdapt = Smooth
  float adapt_brighter = 2.f;    // stops per second
  float adapt_darker = 0.7f;
  color::NeuralTransfer transfer = color::NeuralTransfer::BOUNDED_RATIO;
  float chroma_clamp = 0.f;      // stops; 0 = off
  bool near_black_guard = false;
  friend bool operator==(const ColorFixes&, const ColorFixes&) = default;
};

// Keep faces: pass 1 runs a second time, the twin, with NR's Character mask on and Skin
// structure at `protection`; on characters only the part of NR's change broader than `lighting_scale` (% of the work image's height) is kept from the user's
// own run, in pass 1 and in every later pass.
struct KeepFacesConfig {
  bool enabled = false;
  float protection = 0.f;
  float lighting_scale = 2.5f;
  bool show_mask = false;  // tints the recovered face mask into the result, for tuning
  look::FacesTuning tuning;  // the mask's Advanced rows
  friend bool operator==(const KeepFacesConfig&, const KeepFacesConfig&) = default;
};

// What the next recordings apply beyond their target; the device context sets it before each one.
struct LookConfig {
  look::LookSettings look;
  ColorFixes fixes;
  std::array<std::optional<nr::Controls>, LATER_PASSES> later_controls = {};  // pass n at [n - 2]; nullopt follows pass 1
  std::array<PassStrengths, LATER_PASSES> later_strengths = {};              // pass n's own; (1, 1) resolves nothing
  bool mask = false;                   // Mask = Auto: bind the mask copy once the add-on has copied into it
  bool present_ui_correction = false;  // UICorrection = On: forwarded on the Present path only (no effect without a Backbuffer)
  float frame_seconds = 1.f / 60.f;    // Δt since this placement's last recording: the stabiliser and the governor
  KeepFacesConfig keep_faces;
  friend bool operator==(const LookConfig&, const LookConfig&) = default;
};

// Keep faces: what its four surfaces (the twin's output, the despiked difference, the atlas and the fill's atlas, RGBA16F) cost at `canvas`.
[[nodiscard]] uint64_t FaceSurfaceBytes(nr::Size canvas);
// Keep faces: the twin's controls, pass 1's with the Character mask on and Skin structure at `protection`.
[[nodiscard]] nr::Controls FaceTwinControls(const nr::Controls& pass1, float protection);

// The look stage's surfaces one recording needs, apart from the main set so that the look never restarts NR's history.
// Shared by Direct3D 12's NrPipeline and Vulkan's VkNrPipeline, which allocate them alike.
struct LookPlan {
  nr::Size image;           // the work image size; empty when shaping is inactive
  bool shape = false;       // the basis and both atlases
  bool stabilize = false;   // the level histories and the scene-cut state
  bool detail = false;      // the detail histories
  friend bool operator==(const LookPlan&, const LookPlan&) = default;
};

// Optional shaping surfaces; the required change field belongs to the main processing set.
[[nodiscard]] LookPlan PlanLook(const LookConfig& config, nr::Size image, bool look_loads);

}  // namespace uplift::sources
