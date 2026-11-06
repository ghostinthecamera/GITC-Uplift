#pragma once

#include <cstdint>

namespace uplift::bridge {

// Recordings in a row without motion through which a bridge keeps its motion share. A shorter gap (a frame without UPLIFT_MV, or without DLSS's copy)
// would otherwise retire the share and make it again at the next recording: an allocation and an import per missed frame, which shows as flicker. Past it
// the motion has stopped, and the share goes.
inline constexpr uint32_t MOTION_KEEP_PRESENTS = 30u;

// One bridge's run of recordings without motion while it holds its motion share. Only the recording that reaches the bridge's motion step counts (the
// skipped ones never do), and every recording that comes with motion ends the run. Pure, so the unit tests drive it.
class MotionShareKeep {
 public:
  // A recording that came with motion: the run ends.
  void Arrived() { missed_ = 0u; }
  // A recording without motion while the share is held: true once the run is longer than MOTION_KEEP_PRESENTS (the share goes), and the count starts again.
  [[nodiscard]] bool RetireOnMiss() {
    if (++missed_ <= MOTION_KEEP_PRESENTS) return false;
    missed_ = 0u;
    return true;
  }

 private:
  uint32_t missed_ = 0u;  // recordings in a row without motion since the last one with it
};

}  // namespace uplift::bridge
