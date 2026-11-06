#pragma once

namespace uplift::text {

// Must match the shipped fx/Uplift.fx filename.
inline constexpr char UPLIFT_FX_EFFECT[] = "Uplift.fx";
// Must match the motion texture declared by fx/Uplift.fx.
inline constexpr char UPLIFT_MOTION_TEXTURE[] = "UPLIFT_MV";
// Matches the mask name declared by fx/UpliftMask.fx; custom effects may declare it too.
inline constexpr char UPLIFT_MASK_TEXTURE[] = "UPLIFT_MASK";

}  // namespace uplift::text
