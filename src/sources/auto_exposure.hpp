#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace uplift::sources {

// Plan 17 (1.0.1 design §2): one look at the game's DLSS exposure beside Uplift's meter, read back from the meter's state a few frames after it was written.
struct ExposureSample {
  float texture_value = 1.f;  // the game's ExposureTexture texel (1 without one)
  float factor = 1.f;         // DLSS.Exposure.Scale / DLSS.Pre.Exposure
  float game = 1.f;           // what the encode multiplies by: color::GameExposure of the two
  float metered_stops = 0.f;  // the meter's governed exposure E (Metered multiplies by 2^E)
  float target_stops = 0.f;   // the meter's target this frame, which E moves toward at the Adaptation rates
};

// Plan 17: Input exposure = Auto with a game that passes an exposure. The game's exposure is used while it agrees with Uplift's meter within LIMIT_STOPS;
// once it has disagreed for SUSTAIN_SECONDS (and SUSTAIN_SAMPLES samples) in a row, Auto latches to Metered for the pipeline's life: a game whose value is
// wrong stays wrong, and a latch never flips back. Pure, so the unit tests drive it.
class AutoExposure {
 public:
  static constexpr float LIMIT_STOPS = 2.f;
  static constexpr double SUSTAIN_SECONDS = 1.0;
  static constexpr uint32_t SUSTAIN_SAMPLES = 8u;

  // `seconds`: a steady clock, in seconds, when the sample was read. True when this sample latched Metered.
  bool Observe(const ExposureSample& sample, double seconds);
  [[nodiscard]] bool Latched() const { return latched_; }
  // At the latch: how far the game's exposure was from the meter's E, in stops (always positive).
  [[nodiscard]] float StopsOff() const { return stops_off_; }
  // How far `sample`'s game exposure lies outside the meter's [E, target] span, in stops: the nearer end counts, so neither side's adaptation lag does.
  // Not finite for an unusable sample, which Observe ignores.
  [[nodiscard]] static float Disagreement(const ExposureSample& sample);

 private:
  bool latched_ = false;
  float stops_off_ = 0.f;
  std::optional<double> since_;  // the first sample of the current run of disagreements
  uint32_t run_samples_ = 0u;
};

// Plan 17: what the latest recording's encode read as its exposure, for the Details line.
enum class ExposureUse : uint8_t {
  NONE,     // none: an sRGB, scRGB or HDR10 image, Manual, or Game without a game exposure
  GAME,     // the game's DLSS exposure
  METERED,  // Uplift's meter
};
struct ExposureReport {
  ExposureUse use = ExposureUse::NONE;
  std::optional<float> stops_off;  // METERED because Auto latched: how far off the game's exposure was
  friend bool operator==(const ExposureReport&, const ExposureReport&) = default;
};
// "Input exposure: the game's", "Input exposure: metered (the game's exposure was 6.3 stops off)", "Input exposure: metered"; empty for NONE.
[[nodiscard]] std::string ExposureLine(const ExposureReport& report);
// The one INFO line Auto logs when it latches to Metered: the game's texel, its factor, what that gives, and the meter's values, in stops too.
[[nodiscard]] std::string AutoLatchMessage(const ExposureSample& sample, float stops_off);

}  // namespace uplift::sources
