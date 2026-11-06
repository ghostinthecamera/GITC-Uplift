#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "color/color_pipeline.hpp"
#include "color/encoding.hpp"
#include "nr/types.hpp"
#include "sources/auto_exposure.hpp"
#include "sources/look_plan.hpp"
#include "sources/nr_pipeline.hpp"

namespace uplift::sources {

// The meter's RGBA32F state as shaders/meter_cs.hlsl writes it: texel 0 is (2^E, E, the anchor, set), texel 1 (the game's texel, its factor, the game's
// exposure, the meter's target). Auto's check reads both texels back.
inline constexpr uint32_t METER_STATE_TEXELS = 2u;
inline constexpr uint32_t METER_TEXEL_COMPONENTS = 4u;
inline constexpr size_t METER_SAMPLE_FLOATS = size_t{METER_STATE_TEXELS} * METER_TEXEL_COMPONENTS;
// Texel 0's components the check reads: the meter's E, and the scene's anchor.
inline constexpr size_t METERED_STOPS_COMPONENT = 1u;
inline constexpr size_t ANCHOR_STOPS_COMPONENT = 2u;
// Texel 1's: the game's exposure texel, its factor, what the encode would multiply by, and the meter's target.
inline constexpr size_t GAME_TEXEL_COMPONENT = METER_TEXEL_COMPONENTS + 0u;
inline constexpr size_t GAME_FACTOR_COMPONENT = METER_TEXEL_COMPONENTS + 1u;
inline constexpr size_t GAME_EXPOSURE_COMPONENT = METER_TEXEL_COMPONENTS + 2u;
inline constexpr size_t TARGET_STOPS_COMPONENT = METER_TEXEL_COMPONENTS + 3u;
// AutoExposureBlend's ends: 0 the game's exposure, 1 the meter's.
inline constexpr float AUTO_BLEND_GAME = 0.f;
inline constexpr float AUTO_BLEND_METER = 1.f;

// A recording at `output` runs at: an empty image is the output itself, an empty canvas the image.
[[nodiscard]] inline WorkLayout ResolveLayout(const WorkLayout& layout, nr::Size output) {
  const nr::Size image = (layout.image.Empty() ? output : layout.image);
  return {.image = image, .canvas = (layout.canvas.Empty() ? image : layout.canvas), .upsampling = layout.upsampling};
}

// The encode's and the decodes' shader_options for `fixes`.
[[nodiscard]] inline uint32_t ShaderOptions(const ColorFixes& fixes) {
  return static_cast<uint32_t>(fixes.primaries)  // the shaders resolve AUTO per encoding
         | (fixes.near_black_guard ? color::shader_options::NEAR_BLACK_GUARD : 0u)
         | (fixes.transfer == color::NeuralTransfer::CONSISTENT ? color::shader_options::CONSISTENT : 0u);
}

// One of Auto's checks, from the meter state's two texels as read back.
[[nodiscard]] inline ExposureSample ExposureSampleFromTexels(const std::array<float, METER_SAMPLE_FLOATS>& texels) {
  return {.texture_value = texels[GAME_TEXEL_COMPONENT],
          .factor = texels[GAME_FACTOR_COMPONENT],
          .game = texels[GAME_EXPOSURE_COMPONENT],
          .metered_stops = texels[METERED_STOPS_COMPONENT],
          .target_stops = texels[TARGET_STOPS_COMPONENT],
          .anchor_stops = texels[ANCHOR_STOPS_COMPONENT]};
}

// InputExposure's choice for one recording, before the meter's state is made or retired.
struct ExposureDecision {
  bool use_game = false;  // the encode and the decode read the game's exposure
  bool metered = false;   // they read the meter's state
  bool probe = false;     // the meter runs beside the game's exposure for Auto's check
  bool blend = false;     // Auto's Blend: the meter writes the blend of the two into its state
  // The meter's state is read: made when missing, retired when no longer wanted.
  [[nodiscard]] bool WantsState() const { return metered || probe; }
};

// `game_exposure`: the target carries the game's exposure (a texture, or a factor other than 1). `can_meter`: the image is relative scene-linear and the
// device can run the meter. `latched`: Auto latched to Metered for the pipeline's life (AutoExposure::Latched).
[[nodiscard]] inline ExposureDecision DecideExposure(const ColorFixes& fixes, bool game_exposure, bool can_meter, bool latched) {
  ExposureDecision decision;
  switch (fixes.input_exposure) {
    case color::InputExposure::AUTO:
      // The game's exposure while it is valid against the meter, which runs beside it (Auto's check), and the meter once it has not been for a
      // sustained invalid-sample window; the meter without one, and the game's where the meter cannot run. Blend has the meter write the blend of the two into
      // its state, which the encode and the decode read as Metered's.
      if (!game_exposure) {
        decision.metered = true;
      } else if (can_meter && latched) {
        decision.metered = true;
      } else if (can_meter && fixes.auto_mode == color::AutoExposureMode::BLEND) {
        decision.metered = true;
        decision.probe = true;
        decision.blend = true;
      } else {
        decision.use_game = true;
        decision.probe = can_meter;
      }
      break;
    case color::InputExposure::GAME:    decision.use_game = true; break;
    case color::InputExposure::METERED: decision.metered = true; break;
    case color::InputExposure::MANUAL:  break;
  }
  decision.metered = (decision.metered && can_meter);
  return decision;
}

// What the encode and the decode bind as their exposure.
enum class ExposureSource : uint8_t {
  NONE,   // nothing
  GAME,   // the game's texture and factor
  STATE,  // the meter's state, at a factor of 1
};
struct ExposureOutcome {
  ExposureSource source = ExposureSource::NONE;
  bool metered = false;
  bool probe = false;
  bool with_game = false;   // the meter is handed the game's texture and factor too
  float game_weight = 0.f;  // with `probe`: the game's share of the state's multiplier in stops
  ExposureReport report;    // the Details line's
};

// The decision's outcome once the meter's state was made, or could not be (`have_state`).
[[nodiscard]] inline ExposureOutcome PickExposure(const ExposureDecision& decision, const ColorFixes& fixes, bool game_exposure, bool have_state,
                                                  const AutoExposure& auto_exposure) {
  if (decision.blend && have_state) {
    return {.source = ExposureSource::STATE, .metered = true, .probe = true, .with_game = true,
            .game_weight = AUTO_BLEND_METER - std::clamp(fixes.auto_blend, AUTO_BLEND_GAME, AUTO_BLEND_METER),
            .report = {.use = ExposureUse::BLEND, .blend = fixes.auto_blend, .gap = auto_exposure.LastGap()}};
  }
  if (decision.metered && have_state) {
    const bool latched = (fixes.input_exposure == color::InputExposure::AUTO && game_exposure && auto_exposure.Latched());
    return {.source = ExposureSource::STATE, .metered = true,
            .report = {.use = ExposureUse::METERED,
                       .stops_off = (latched ? std::optional<float>(auto_exposure.StopsOff()) : std::nullopt),
                       .latch = (latched ? auto_exposure.Reason() : AutoLatch::NONE)}};
  }
  if (decision.use_game || decision.blend) {  // Blend without its state: the game's
    return {.source = ExposureSource::GAME, .probe = (decision.probe && have_state), .with_game = true,
            .report = {.use = (game_exposure ? ExposureUse::GAME : ExposureUse::NONE)}};
  }
  return {};
}

// The rect of motion vectors the stabiliser reads in a texture of `texture`: the whole texture for a rect with an empty side; none when the rect reaches
// outside the texture.
[[nodiscard]] inline std::optional<nr::Rect> MotionRectOf(nr::Rect rect, nr::Size texture) {
  if (rect.width == 0u || rect.height == 0u) {
    rect = {.x = 0u, .y = 0u, .width = texture.width, .height = texture.height};
  }
  if (uint64_t{rect.x} + rect.width > texture.width || uint64_t{rect.y} + rect.height > texture.height) return std::nullopt;
  return rect;
}

}  // namespace uplift::sources
