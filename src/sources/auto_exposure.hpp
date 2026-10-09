#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "color/encoding.hpp"

namespace uplift::sources {

// Plan 17 (1.0.1 design §2): one look at the game's DLSS exposure beside Uplift's meter, read back from the meter's state a few frames after it was written.
struct ExposureSample {
  float texture_value = 1.f;  // the game's ExposureTexture texel (1 without one)
  float factor = 1.f;         // DLSS.Exposure.Scale / DLSS.Pre.Exposure
  float game = 1.f;           // what the encode multiplies by: color::GameExposure of the two
  float metered_stops = 0.f;  // the meter's governed exposure E (Metered multiplies by 2^E)
  float target_stops = 0.f;   // the meter's target this frame, which E moves toward at the Adaptation rates
  float anchor_stops = 0.f;   // 2026-10-09: the meter's anchor (the scene's mean log luminance it exposes to middle grey), for the held check
};

// 2026-10-09: which validity rule latched Auto to Metered.
enum class AutoLatch : uint8_t {
  NONE,
  ABOVE,  // the game's exposure sat more than ABOVE_LIMIT_STOPS above the meter's span
  BELOW,  // more than BELOW_LIMIT_STOPS below it
};

// Plan 17: Input exposure = Auto with a game that passes an exposure. The game's exposure is valid while it lies within the window around Uplift's meter;
// once it has been invalid for SUSTAIN_SECONDS (and SUSTAIN_SAMPLES samples) in a row, Auto latches to Metered for the pipeline's life: a game whose value is
// wrong stays wrong, and a latch never flips back. Pure, so the unit tests drive it.
// 2026-10-09 (.superpowers/sdd/2026-10-09-auto-exposure): the window is asymmetric. A game exposure that holds still while the scene moves is noted for the
// debug line only (the coordinator's ruling): a fixed exposure, or DLSS's Exposure 1 on pre-exposed images, is legitimate, and a constant 1 on
// physical-unit images already lands above the window.
class AutoExposure {
 public:
  // NR After DLSS sees the image before the game's tone mapper, so a right game exposure sits 1.5-2.5 stops above the meter, and a scene the game keeps dark
  // on purpose sits below it. A game passing 1 on physical-unit images sits 5-10 stops above.
  static constexpr float ABOVE_LIMIT_STOPS = 4.f;
  static constexpr float BELOW_LIMIT_STOPS = 5.f;
  // Held (information only): the game's exposure within HELD_EPSILON_STOPS of one value while the meter's anchor spans more than HELD_ANCHOR_STOPS within
  // SUSTAIN_SECONDS.
  static constexpr float HELD_EPSILON_STOPS = 0.01f;
  static constexpr float HELD_ANCHOR_STOPS = 2.f;
  static constexpr double SUSTAIN_SECONDS = 1.0;
  static constexpr uint32_t SUSTAIN_SAMPLES = 8u;
  static constexpr double DEBUG_SECONDS = 1.0;  // one debug line per this many seconds

  // `seconds`: a steady clock, in seconds, when the sample was read. True when this sample latched Metered.
  bool Observe(const ExposureSample& sample, double seconds);
  [[nodiscard]] bool Latched() const { return latched_; }
  [[nodiscard]] AutoLatch Reason() const { return reason_; }
  // At the latch: how far the game's exposure was from the meter's E, in stops (always positive).
  [[nodiscard]] float StopsOff() const { return stops_off_; }
  // How far the meter's anchor has moved while the game's exposure held still, in stops, once that is more than HELD_ANCHOR_STOPS within SUSTAIN_SECONDS;
  // none while the game's exposure follows the scene. Information for the debug line: never a reason to latch.
  [[nodiscard]] std::optional<float> HeldThroughMove() const;
  // The game's exposure minus the meter's E, in stops, of the latest usable sample: none before one.
  [[nodiscard]] std::optional<float> LastGap() const { return last_gap_; }
  // True once per DEBUG_SECONDS: the pipelines log one debug line when it is.
  bool DebugDue(double seconds);
  // How far `sample`'s game exposure lies outside the meter's [E, target] span, in stops: positive above it, negative below, 0 inside. The nearer end counts,
  // so neither side's adaptation lag does. Not finite for an unusable sample, which Observe ignores.
  [[nodiscard]] static float OutsideSpan(const ExposureSample& sample);

 private:
  // The held check's bookkeeping for one usable sample.
  void NoteHeld(float game_stops, float anchor_stops, double seconds);

  bool latched_ = false;
  AutoLatch reason_ = AutoLatch::NONE;
  float stops_off_ = 0.f;
  std::optional<float> last_gap_;
  std::optional<double> since_;  // the first sample of the current run of invalid ones
  uint32_t run_samples_ = 0u;
  std::optional<double> last_debug_;
  // Held: the value the game's exposure holds, the anchors seen within the last SUSTAIN_SECONDS since, and whether they spanned the limit (kept until the
  // game's exposure moves).
  std::optional<float> held_stops_;
  std::deque<std::pair<double, float>> held_anchors_;
  bool held_through_move_ = false;
  float held_anchor_span_ = 0.f;
};

// 2026-10-09: NR's input exposure under Auto with a game exposure, in stops: Switch the game's, Blend game + blend · (E − game) with `blend` in [0, 1]; the
// meter's E once latched. meter_cs.hlsl writes the blend into the state the encode reads.
[[nodiscard]] float AutoStops(color::AutoExposureMode mode, float blend, bool latched, float game_stops, float metered_stops);

// Plan 17: what the latest recording's encode read as its exposure, for the Details line.
enum class ExposureUse : uint8_t {
  NONE,     // none: an sRGB, scRGB or HDR10 image, Manual, or Game without a game exposure
  GAME,     // the game's DLSS exposure
  METERED,  // Uplift's meter
  BLEND,    // 2026-10-09: Auto's Blend of the two
};
struct ExposureReport {
  ExposureUse use = ExposureUse::NONE;
  std::optional<float> stops_off;  // METERED because Auto latched: how far off the game's exposure was
  AutoLatch latch = AutoLatch::NONE;  // and which rule fired
  float blend = 0.f;                  // BLEND: AutoExposureBlend, 0 the game's, 1 the meter's
  std::optional<float> gap;           // BLEND: the game's exposure minus the meter's E, in stops, once a check has been read
  friend bool operator==(const ExposureReport&, const ExposureReport&) = default;
};
// "Input exposure: the game's", "Input exposure: metered (the game's was 6.3 stops off: above)", "Input exposure: metered",
// "Input exposure: blend 50 % game/meter (gap +2.3 stops)"; empty for NONE.
[[nodiscard]] std::string ExposureLine(const ExposureReport& report);
// The one INFO line Auto logs when it latches to Metered: the rule, the game's texel, its factor, what that gives, and the meter's values, in stops too.
[[nodiscard]] std::string AutoLatchMessage(const ExposureSample& sample, const AutoExposure& judge);
// The debug line Auto logs once a second before a latch: the game in stops, the meter's E and target, the gap, what the mode gives, and `held_move`
// (AutoExposure::HeldThroughMove) when there is one.
[[nodiscard]] std::string AutoDebugMessage(const ExposureSample& sample, color::AutoExposureMode mode, float blend, std::optional<float> held_move);

}  // namespace uplift::sources
