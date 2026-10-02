#pragma once

#include <cstdint>

namespace uplift::sources {

// The motion vectors a recording bound, for the overlay's "Motion vectors" line (Plan 4 fix round 4).
// NrPipeline reports this mechanically, from what it was given or what it found for Present; it cannot
// tell "MotionVectors = None" from "the game passed none" (both arrive as a null resource), so a caller
// with that context (DeviceContext) turns NONE into the line's actual reason. In its own header (Plan 14) so the 32-bit add-on can read
// the 64-bit helper's source (ipc::Status::motion_source) without NR's pipeline headers.
enum class MotionSource : uint8_t {
  NONE,          // the all-zero texture
  DIRECT,        // the game's own DLSS motion vectors, bound in place (After DLSS, Before upscaling)
  PRESENT_COPY,  // Amendment 7's copy, made at a DLSS evaluate for this present (Source = Present)
  LAUNCHPAD,     // Plan 6 (v2 design §3.20): UPLIFT_MV from Uplift.fx, bound on the Present path
};

}  // namespace uplift::sources
