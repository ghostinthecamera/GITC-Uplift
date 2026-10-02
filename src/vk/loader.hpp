#pragma once

#include <Windows.h>

#include <vulkan/vulkan.h>

#include <optional>

#include "vk/device_hook.hpp"

namespace uplift::vk {

// The system loader's instance-level exports (Vulkan design §2.2). vulkan-1.dll is only ever looked up with GetModuleHandleW, never loaded:
// Uplift acts only where the game loaded Vulkan itself. The exports are cached for good, which is sound because DeviceHook::Install pins the
// module before anything else uses it (DXVK would otherwise free it with its last instance, final review I-1).
struct Loader {
  HMODULE module = nullptr;
  // Plan 13: the instance-level entry points native NR passes to NGX (the game's own chain) and the device's LUID query. Optional: null when the
  // loader lacks the export (Get() still succeeds), and the NR users say so.
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 = nullptr;
  // Plan 13 batch 3, optional too: the 64-bit device hook's detours (the instance, the teardown) and native NR's format and feature checks.
  PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
  PFN_vkEnumeratePhysicalDeviceGroups enumerate_physical_device_groups = nullptr;
  PFN_vkDestroyDevice destroy_device = nullptr;
  PFN_vkGetPhysicalDeviceFeatures get_physical_device_features = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties get_physical_device_format_properties = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
  PFN_vkCreateDevice create_device = nullptr;  // the export the device hook detours

  // Null while the process has not loaded vulkan-1.dll. Thread-safe; the answer never changes once non-null.
  [[nodiscard]] static const Loader* Get();
};

// One Vulkan device's functions, resolved through the loader's vkGetDeviceProcAddr: the game's own chain, so ReShade's layer sits in front of
// the ones it intercepts (vkCreateImage, vkDestroyImage, vkQueueSubmit) and the rest go straight to the driver.
struct Device {
  VkDevice handle = VK_NULL_HANDLE;
  // What the hook recorded, or, for a device it never saw, a record that says so (design §2.5): memory_win32 then reads whether the loader
  // hands out vkGetMemoryWin32HandlePropertiesKHR for it.
  DeviceRecord record;
  bool seen = false;  // the hook recorded this device

  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
  PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkGetFenceStatus vkGetFenceStatus = nullptr;
  PFN_vkWaitForFences vkWaitForFences = nullptr;
  PFN_vkResetFences vkResetFences = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  // Optional: null when the device did not enable what provides them.
  PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
  PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR = nullptr;
  PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue = nullptr;  // core 1.2, or the KHR name

  // nullopt when the loader is missing or a required function is not there.
  [[nodiscard]] static std::optional<Device> Open(VkDevice device);
};

}  // namespace uplift::vk
