#pragma once

#include <span>
#include <string>
#include <vector>

namespace uplift::vk {

// Plan 11 (Vulkan design §2.2): the pure part of Uplift's vkCreateDevice adjustment, shared by the hook-timing probe and, from Task 2,
// the product's detour. No Vulkan call and no Vulkan header here, so it compiles and tests in both halves.
struct DeviceAdjustmentInput {
  std::span<const char* const> enabled;    // the app's ppEnabledExtensionNames
  std::span<const std::string> offered;    // vkEnumerateDeviceExtensionProperties(physical, nullptr, ...)
  bool vulkan12_features = false;          // VkPhysicalDeviceVulkan12Features in the pNext chain
  bool timeline_features = false;          // VkPhysicalDeviceTimelineSemaphoreFeatures in the pNext chain
  bool adjust = true;                      // AdjustVulkanDevices, and not turned off by the pending marker
  bool reshade_adds_memory_win32 = false;  // ReShade >= 6.8 appends VK_KHR_external_memory_win32 to every device itself
};

struct DeviceAdjustment {
  std::vector<const char*> added;        // static names to append; each offered and absent from `enabled`
  bool chain_timeline_features = false;  // prepend VkPhysicalDeviceTimelineSemaphoreFeatures{timelineSemaphore = TRUE}
  bool memory_win32 = false;             // what the device will have, whoever enabled it
  bool semaphore_win32 = false;
  bool timeline = false;
  bool presents = false;  // the app lists VK_KHR_swapchain: a device that can present at all (the pending marker waits only for those)
};

// With `adjust`: appends whichever of VK_KHR_external_semaphore_win32 and VK_KHR_external_memory_win32 the driver offers and the app did
// not list (the memory extension is left to ReShade when it adds it itself, so it is not listed twice), and, when the app passes no
// Vulkan 1.2 or timeline feature struct and the driver offers VK_KHR_timeline_semaphore, that extension plus a chained feature struct of
// our own (a driver without it gets neither, and `timeline` stays false). With a struct present nothing is added for timeline semaphores:
// ReShade forces the feature on (vulkan_hooks_device.cpp:327-368).
[[nodiscard]] DeviceAdjustment BuildDeviceAdjustment(const DeviceAdjustmentInput& input);

}  // namespace uplift::vk
