#include "ngx_hooks/frame_generation.hpp"

#include <algorithm>

namespace uplift::ngx_hooks {
namespace {

struct CadenceStep {
  double min_presents_per_evaluate;
  uint32_t multiplier;
};

constexpr uint32_t BASE_MULTIPLIER = 1u;
constexpr uint32_t MIN_FG_MULTIPLIER = 2u;
constexpr CadenceStep CADENCE_STEPS[] = {{3.6, 4u}, {2.7, 3u}, {1.8, MIN_FG_MULTIPLIER}};
constexpr auto WINDOW = std::chrono::seconds(1);
constexpr auto HYSTERESIS = std::chrono::seconds(3);

}  // namespace

void FrameGenDetector::OnPresent(std::chrono::steady_clock::time_point now) {
  presents_.push_back(now);
}

void FrameGenDetector::OnMainEvaluate(std::chrono::steady_clock::time_point now) {
  evaluates_.push_back(now);
  while (now - evaluates_.front() > WINDOW) {  // bounded even while presents starve
    evaluates_.pop_front();
  }
}

FrameGenState FrameGenDetector::Update(std::chrono::steady_clock::time_point now, uint32_t ngx_live) {
  for (auto* const events : {&presents_, &evaluates_}) {
    while (!events->empty() && now - events->front() > WINDOW) {
      events->pop_front();
    }
  }
  uint32_t measured = BASE_MULTIPLIER;
  if (!evaluates_.empty()) {
    const double ratio = static_cast<double>(presents_.size()) / static_cast<double>(evaluates_.size());
    for (const CadenceStep& step : CADENCE_STEPS) {
      if (ratio >= step.min_presents_per_evaluate) {
        measured = step.multiplier;
        break;
      }
    }
  }
  if (measured != candidate_multiplier_ || !candidate_since_) {
    candidate_multiplier_ = measured;
    candidate_since_ = now;
  }
  if (candidate_multiplier_ != shown_multiplier_ && now - *candidate_since_ >= HYSTERESIS) {
    shown_multiplier_ = candidate_multiplier_;
  }
  from_ngx_ = static_cast<bool>(ngx_live);
  return ObservedState();
}

bool FrameGenDetector::HasCadence() const {
  return shown_multiplier_ >= MIN_FG_MULTIPLIER;
}

FrameGenState FrameGenDetector::ReportedState(bool route_active) const {
  const bool reported = from_ngx_ || route_active;
  return {.active = reported || HasCadence(),
          .multiplier = std::max(shown_multiplier_, reported ? MIN_FG_MULTIPLIER : BASE_MULTIPLIER),
          .from_ngx = from_ngx_};
}

}  // namespace uplift::ngx_hooks
