#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace uplift::ngx_hooks {

struct FrameGenState {
  bool active = false;
  uint32_t multiplier = 1u;  // presented frames per rendered frame: 2, 3 or 4 when active
  bool from_ngx = false;     // a live NGX FrameGeneration feature says so
};

// v2 design §3.5. A live NGX FrameGeneration feature is authoritative. Frame generation made outside
// NGX shows as a cadence: over the last 1 s, presents per main-handle evaluate of at least 1.8, 2.7
// or 3.6 mean 2x, 3x or 4x, and a new reading must hold for 3 s before it is shown. Not thread-safe.
class FrameGenDetector {
 public:
  void OnPresent(std::chrono::steady_clock::time_point now);
  void OnMainEvaluate(std::chrono::steady_clock::time_point now);
  // Once per present, after OnPresent. `ngx_live`: the live NGX FrameGeneration features.
  FrameGenState Update(std::chrono::steady_clock::time_point now, uint32_t ngx_live);
  [[nodiscard]] FrameGenState State() const { return state_; }

 private:
  std::deque<std::chrono::steady_clock::time_point> presents_;
  std::deque<std::chrono::steady_clock::time_point> evaluates_;
  uint32_t shown_multiplier_ = 1u;
  uint32_t candidate_multiplier_ = 1u;
  std::optional<std::chrono::steady_clock::time_point> candidate_since_;
  FrameGenState state_;
};

}  // namespace uplift::ngx_hooks
