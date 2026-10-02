#include "vk/device_adjustment.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <string_view>

namespace uplift::vk {

DeviceAdjustment BuildDeviceAdjustment(const DeviceAdjustmentInput& input) {
  const auto listed = [&input](std::string_view name) {
    return std::ranges::any_of(input.enabled, [name](const char* enabled) { return name == enabled; });
  };
  const auto offered = [&input](std::string_view name) {
    return std::ranges::any_of(input.offered, [name](const std::string& candidate) { return candidate == name; });
  };
  DeviceAdjustment adjustment;
  const auto append = [&](const char* name) {
    if (!input.adjust || listed(name) || !offered(name)) return false;
    adjustment.added.push_back(name);
    return true;
  };

  const bool semaphore_added = append(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
  const bool memory_added = !input.reshade_adds_memory_win32 && append(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
  // Only for a driver that offers the extension (batch 2 review, minor 7): a feature struct for an extension that is not enabled, on a device
  // below 1.2, is invalid, and the device would then be reported as having timeline semaphores it does not have.
  if (input.adjust && !input.vulkan12_features && !input.timeline_features && offered(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
    append(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    adjustment.chain_timeline_features = true;
  }

  adjustment.presents = listed(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  adjustment.timeline = input.vulkan12_features || input.timeline_features || adjustment.chain_timeline_features;
  adjustment.semaphore_win32 = semaphore_added || listed(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
  adjustment.memory_win32 = memory_added || listed(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)
                            || (input.reshade_adds_memory_win32 && offered(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME));
  return adjustment;
}

}  // namespace uplift::vk
