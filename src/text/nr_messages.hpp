#pragma once

namespace uplift::text {

// The card's reason when the file Uplift would load is already mapped outside the game's folder (Snippet::Load refuses it): another host of the
// same file, or Uplift's own module left mapped after ReShade reloaded the add-on. The mapped path follows in parentheses.
inline constexpr char RUNTIME_MAPPED_ELSEWHERE_LINE[] =
    "nvngx_dlssnr.dll is already mapped in this game by another component, or stayed mapped after Uplift unloaded it";

// The NR producer text while the game's own NR evaluates (ForeignNr = Yield).
inline constexpr char FOREIGN_NR_EVALUATING_LINE[] = "another tool is running NR through NVIDIA NGX in this game";

inline constexpr char NVIDIA_GPU_REQUIRED_LINE[] = "Uplift needs an NVIDIA GPU; this game renders on another adapter";

inline constexpr char GAME_QUEUE_NOT_WAITING_LINE[] = "not yet at its wait after {} ms";
inline constexpr char BRIDGE_STALLED_LINE[] = "the cross-API fences made no progress for {} s while NR's work was outstanding";

inline constexpr char DESCRIPTORS_BUSY_LINE[] =
    "NR after DLSS: descriptors busy for over {} s in a row; the descriptor ring may be too small for this thread's submission timing";
inline constexpr char VK_RING_DRAIN_TIMEOUT_LINE[] =
    "Vulkan: Uplift's command buffers did not finish within {} s of leaving Present; NR stays off on this device until the game exits";
inline constexpr char VK_PRESENT_DEVICE_LOST_LINE[] =
    "Vulkan: the game's device was lost within {} s of native NR at Present: native NR is off from now on (VulkanNativeNr = 0)";
inline constexpr char VK_STALE_TOKEN_DROPPED_LINE[] = "Vulkan: a recording was never submitted; its token was dropped after {} s";
inline constexpr char VK_LATE_SUBMISSION_LINE[] =
    "Vulkan: a recording was submitted after its token was dropped as stale (more than {} s and {} frames after it was made)";

}  // namespace uplift::text
