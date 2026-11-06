#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace uplift::ngx_hooks {

struct FrameGenState {
  bool active = false;
  uint32_t multiplier = 1u;  // presented frames per rendered frame
  bool from_ngx = false;     // a live NGX FrameGeneration feature says so
};

// A live NGX feature takes effect immediately. A change in measured cadence must persist through the hysteresis interval. Callers serialize access.
class FrameGenDetector {
 public:
  void OnPresent(std::chrono::steady_clock::time_point now);
  void OnMainEvaluate(std::chrono::steady_clock::time_point now);
  // Once per present, after OnPresent. `ngx_live`: the live NGX FrameGeneration features.
  FrameGenState Update(std::chrono::steady_clock::time_point now, uint32_t ngx_live);
  [[nodiscard]] bool HasCadence() const;
  [[nodiscard]] FrameGenState ObservedState() const { return ReportedState(false); }
  // Route activity applies to this read only. It never contributes to the measured cadence or NGX provenance.
  [[nodiscard]] FrameGenState ReportedState(bool route_active) const;

 private:
  std::deque<std::chrono::steady_clock::time_point> presents_;
  std::deque<std::chrono::steady_clock::time_point> evaluates_;
  uint32_t shown_multiplier_ = 1u;
  uint32_t candidate_multiplier_ = 1u;
  std::optional<std::chrono::steady_clock::time_point> candidate_since_;
  bool from_ngx_ = false;
};

}  // namespace uplift::ngx_hooks
