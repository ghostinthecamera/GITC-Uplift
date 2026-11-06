#pragma once

#include <chrono>
#include <optional>

#include "nr/types.hpp"

namespace uplift::sources {

// The Resolution presets' scale of the output size.
inline constexpr double QUALITY_SCALE = 2.0 / 3.0;
inline constexpr double BALANCED_SCALE = 0.58;
inline constexpr double PERFORMANCE_SCALE = 0.5;

// Size rule for a target of `target_width` x `target_height` (the output times the
// mode's scale, or Match game's create-time render size), which must be finite:
// - each side is rounded to the nearest even number;
// - below the NR floor, both sides are scaled up by one factor until it is met, rounded up to even;
// - the result is capped at `output`.
// A target that reaches `output` in both sides returns `output` itself (Full), odd sides included.
[[nodiscard]] nr::Size WorkSize(double target_width, double target_height, nr::Size output);
// The floor-sized canvas an image below the floor is padded into, landscape or
// portrait; `image` itself when it meets the floor.
[[nodiscard]] nr::Size CanvasSize(nr::Size image);

// The applied size moves to the target:
// - once the target has differed from it by more than SETTLE_PERCENT in either side for SETTLE_TIME;
// - once the target has rested (stayed unchanged) for SETTLE_TIME, whatever the difference;
// - at once on the first update after construction or Reset, and on the first update after a hold.
// While `held` (the Custom slider is being dragged) the applied size waits.
class WorkSizeSettle {
 public:
  nr::Size Update(nr::Size target, std::chrono::steady_clock::time_point now, bool held);
  void Reset();

 private:
  nr::Size applied_;
  nr::Size last_target_;
  std::chrono::steady_clock::time_point target_since_;
  std::optional<std::chrono::steady_clock::time_point> differing_since_;
  bool was_held_ = false;
};

}  // namespace uplift::sources
