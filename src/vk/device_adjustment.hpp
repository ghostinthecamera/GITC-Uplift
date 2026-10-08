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
  // Plan 19 (64-bit only; the 32-bit half passes false): NGX's four device extensions and bufferDeviceAddress, with `adjust`, for native NR.
  bool add_ngx = false;
  bool reshade_adds_push_descriptor = false;     // ReShade appends VK_KHR_push_descriptor itself (instance below 1.4, offered), never de-duplicated
  bool buffer_device_address_features = false;   // VkPhysicalDeviceBufferDeviceAddressFeatures (or the EXT one) in the pNext chain
  bool buffer_device_address_supported = false;  // vkGetPhysicalDeviceFeatures2 reports bufferDeviceAddress for the physical device
};

struct DeviceAdjustment {
  std::vector<const char*> added;        // static names to append; each offered and absent from `enabled`
  std::vector<const char*> ngx_added;    // Plan 19: NGX's names to append after `added`, offered, absent from `enabled`, not left to ReShade
  bool chain_timeline_features = false;  // prepend VkPhysicalDeviceTimelineSemaphoreFeatures{timelineSemaphore = TRUE}
  bool memory_win32 = false;             // what the device will have, whoever enabled it
  bool semaphore_win32 = false;
  bool timeline = false;
  bool presents = false;  // the app lists VK_KHR_swapchain: a device that can present at all (the pending marker waits only for those)
  // Plan 19: prepend VkPhysicalDeviceBufferDeviceAddressFeatures{bufferDeviceAddress = TRUE}, in front of the timeline struct when both are.
  bool chain_buffer_device_address = false;
};

// With `adjust`: appends whichever of VK_KHR_external_semaphore_win32 and VK_KHR_external_memory_win32 the driver offers and the app did
// not list (the memory extension is left to ReShade when it adds it itself, so it is not listed twice), and, when the app passes no
// Vulkan 1.2 or timeline feature struct and the driver offers VK_KHR_timeline_semaphore, that extension plus a chained feature struct of
// our own (a driver without it gets neither, and `timeline` stays false). With a struct present nothing is added for timeline semaphores:
// ReShade forces the feature on (vulkan_hooks_device.cpp:327-368).
// With `add_ngx` too (the 64-bit half, Plan 19): each of VK_NVX_binary_import, VK_NVX_image_view_handle, VK_KHR_buffer_device_address and VK_KHR_push_descriptor
// the driver offers and the app did not list, except VK_KHR_buffer_device_address next to the app's VK_EXT_buffer_device_address (the two exclude each
// other) and VK_KHR_push_descriptor when ReShade appends it (vulkan_hooks_device.cpp:249-253 adds it again without looking). bufferDeviceAddress is
// chained only when the app passes no Vulkan 1.2 or bufferDeviceAddress struct (one with the feature off is the app's memory, never written), the
// device supports it, and VK_KHR_buffer_device_address ends up on the list.
[[nodiscard]] DeviceAdjustment BuildDeviceAdjustment(const DeviceAdjustmentInput& input);

}  // namespace uplift::vk
