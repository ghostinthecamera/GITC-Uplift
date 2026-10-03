#include "sources/auto_exposure.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace uplift::sources {

float AutoExposure::Disagreement(const ExposureSample& sample) {
  if (!(sample.game > 0.f) || !std::isfinite(sample.game) || !std::isfinite(sample.metered_stops) || !std::isfinite(sample.target_stops)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  const float game = std::log2(sample.game);
  const float low = std::min(sample.metered_stops, sample.target_stops);
  const float high = std::max(sample.metered_stops, sample.target_stops);
  return std::max({low - game, game - high, 0.f});
}

bool AutoExposure::Observe(const ExposureSample& sample, double seconds) {
  if (latched_) return false;
  const float off = Disagreement(sample);
  if (!std::isfinite(off)) return false;  // an unusable sample neither counts nor breaks a run
  if (off <= LIMIT_STOPS) {
    since_.reset();
    run_samples_ = 0u;
    return false;
  }
  if (!since_) {
    since_ = seconds;
  }
  ++run_samples_;
  if (run_samples_ < SUSTAIN_SAMPLES || seconds - *since_ < SUSTAIN_SECONDS) return false;
  latched_ = true;
  stops_off_ = std::abs(std::log2(sample.game) - sample.metered_stops);
  return true;
}

std::string ExposureLine(const ExposureReport& report) {
  switch (report.use) {
    case ExposureUse::GAME: return "Input exposure: the game's";
    case ExposureUse::METERED:
      if (report.stops_off) return std::format("Input exposure: metered (the game's exposure was {:.1f} stops off)", *report.stops_off);
      return "Input exposure: metered";
    case ExposureUse::NONE: break;
  }
  return {};
}

std::string AutoLatchMessage(const ExposureSample& sample, float stops_off) {
  return std::format("Input exposure Auto: the game's exposure stayed {:.1f} stops off Uplift's meter for over {:.0f} s, so NR's input is metered from now "
                     "on (ExposureTexture {:.4g}, factor {:.4g}: the game's {:.4g} = {:+.2f} stops; metered {:.4g} = {:+.2f} stops, target {:+.2f} stops)",
                     stops_off, AutoExposure::SUSTAIN_SECONDS, sample.texture_value, sample.factor, sample.game, std::log2(sample.game),
                     std::exp2(sample.metered_stops), sample.metered_stops, sample.target_stops);
}

}  // namespace uplift::sources
