#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "color/encoding.hpp"
#include "look/look_math.hpp"
#include "nr/types.hpp"

namespace uplift::sources {

// Plan 5 (v2 design §3.10): pass n ≥ 2's own Transfer and Colour strength between passes; (1, 1) resolves nothing.
struct PassStrengths {
  float transfer = 1.f;
  float color = 1.f;
  friend bool operator==(const PassStrengths&, const PassStrengths&) = default;
};

// Plan 5 (v2 design §3.14): the colour fixes. The defaults keep Plan 4's output (key decision 1).
struct ColorFixes {
  color::Primaries primaries = color::Primaries::AUTO;
  float linear_unit_nits = 0.f;  // 0 = automatic
  color::InputExposure input_exposure = color::InputExposure::GAME;
  bool smooth_adapt = true;      // ExposureAdapt = Smooth
  float adapt_brighter = 2.f;    // stops per second
  float adapt_darker = 0.7f;
  color::NeuralTransfer transfer = color::NeuralTransfer::BOUNDED_RATIO;
  float chroma_clamp = 0.f;      // stops; 0 = off
  bool near_black_guard = false;
  friend bool operator==(const ColorFixes&, const ColorFixes&) = default;
};

inline constexpr size_t LATER_PASSES = 9u;  // passes 2..10

// Plan 5: what the next recordings apply beyond their target; the device context sets it before each one.
struct LookConfig {
  look::LookSettings look;
  ColorFixes fixes;
  std::array<std::optional<nr::Controls>, LATER_PASSES> later_controls = {};  // pass n at [n - 2]; nullopt follows pass 1
  std::array<PassStrengths, LATER_PASSES> later_strengths = {};              // pass n's own; (1, 1) resolves nothing
  bool mask = false;                   // Mask = Auto: bind the mask copy once the add-on has copied into it
  bool present_ui_correction = false;  // UICorrection = On: forwarded on the Present path only (no effect without a Backbuffer)
  float frame_seconds = 1.f / 60.f;    // Δt since this placement's last recording: the stabiliser and the governor
};

// Plan 5 (key decision 4): the look stage's surfaces one recording needs, apart from the main set so that the look never restarts NR's history.
// Plan 14: shared by Direct3D 12's NrPipeline and Vulkan's VkNrPipeline, which allocate them alike.
struct LookPlan {
  nr::Size image;           // the change field's size; empty: no set
  bool own_change = false;  // C lives here: the main set has none (the output size) but something acts on the change
  bool shape = false;       // the basis and both atlases
  bool stabilize = false;   // the level histories and the scene-cut state
  bool detail = false;      // the detail histories
  friend bool operator==(const LookPlan&, const LookPlan&) = default;
};

// Key decision 3: something acts on NR's change, so a recording at the output size still goes through the change field. `mask_ready`: a mask copy
// was made (it binds with Mask = Auto); `look_loads`: the device loads RGBA16F, R16F and RGBA32F storage, which the look's passes need.
[[nodiscard]] bool NeedsChangePath(const LookConfig& config, bool mask_ready, bool look_loads);
// The look surfaces of a recording whose work image is `image` (`reduced`: below the output size). `main_set_has_change`: the main set holds C already
// (below the output size, and always before upscaling).
[[nodiscard]] LookPlan PlanLook(const LookConfig& config, nr::Size image, bool reduced, bool main_set_has_change, bool mask_ready, bool look_loads);

}  // namespace uplift::sources
