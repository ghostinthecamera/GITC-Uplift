#pragma once

namespace uplift::text {

inline constexpr char RUN_REPLY_TIMEOUT_LINE[] = "Uplift's 64-bit helper did not answer within {} s; frames go without NR until it does";
inline constexpr char START_TIMEOUT_LINE[] = "It did not start within {} s";
inline constexpr char EXIT_TIMEOUT_LINE[] = "the helper process was ended but had not exited after {} s";
inline constexpr char IDLE_EXIT_LINE[] = "no request for {} s; the helper exits";
inline constexpr char NR_DRAIN_TIMEOUT_LINE[] =
    "Vulkan: Uplift's helper process has not released NR {} s after NR moved away from it; NR stays off on this device until it does";

}  // namespace uplift::text
