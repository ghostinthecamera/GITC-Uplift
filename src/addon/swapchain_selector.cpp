#include "addon/swapchain_selector.hpp"

namespace uplift::addon {

bool SwapchainSelector::OnPresent(const void* swapchain, std::chrono::steady_clock::time_point now) {
  if (primary_ == nullptr || (swapchain != primary_ && now - last_present_ >= handover_)) {
    primary_ = swapchain;
  }
  if (swapchain != primary_) return false;
  last_present_ = now;
  return true;
}

}  // namespace uplift::addon
