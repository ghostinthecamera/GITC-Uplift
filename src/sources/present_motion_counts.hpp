#pragma once

#include <cstdint>

namespace uplift::sources {

// The Present-motion diagnostics both APIs count, written to the once-a-second TRACE line. Each pipeline's PresentMotionCounters derives from it and
// adds the figures only its API keeps.
struct PresentMotionCounts {
  uint64_t evaluates = 0u;
  uint64_t presents = 0u;
  uint64_t copies = 0u;
  uint64_t skipped_input = 0u;  // no readable vectors, or the copy could not be made
  uint64_t skipped_ring = 0u;   // the copy pipeline's constants ring slot was busy
  uint64_t skipped_slot = 0u;   // the copy slot's last reader was not complete
  uint64_t own = 0u;            // presents that bound their own frame's copy
  uint64_t older = 0u;          // presents that bound an older copy (PRESENT_MOTION_MAX_AGE)
  uint64_t missed = 0u;         // presents with no copy that recent
  float offset_x = 0.f;
  float offset_y = 0.f;
  uint64_t lag_max = 0u;
};

}  // namespace uplift::sources
