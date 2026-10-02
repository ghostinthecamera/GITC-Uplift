#include "ngx_hooks/frame_generation.hpp"

#include <algorithm>

namespace uplift::ngx_hooks {
namespace {

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
  uint32_t measured = 1u;
  if (!evaluates_.empty()) {
    const double ratio = static_cast<double>(presents_.size()) / static_cast<double>(evaluates_.size());
    if (ratio >= 3.6) {
      measured = 4u;
    } else if (ratio >= 2.7) {
      measured = 3u;
    } else if (ratio >= 1.8) {
      measured = 2u;
    }
  }
  if (measured != candidate_multiplier_ || !candidate_since_) {
    candidate_multiplier_ = measured;
    candidate_since_ = now;
  }
  if (candidate_multiplier_ != shown_multiplier_ && now - *candidate_since_ >= HYSTERESIS) {
    shown_multiplier_ = candidate_multiplier_;
  }
  const bool from_ngx = (ngx_live > 0u);
  state_ = {
      .active = (from_ngx || shown_multiplier_ >= 2u),
      .multiplier = std::max(shown_multiplier_, (from_ngx ? 2u : 1u)),
      .from_ngx = from_ngx,
  };
  return state_;
}

}  // namespace uplift::ngx_hooks
