#include "vk/loader.hpp"

#include <atomic>
#include <type_traits>
#include <utility>

namespace uplift::vk {

const Loader* Loader::Get() {
  static std::atomic<const Loader*> cached{nullptr};
  if (const Loader* const found = cached.load(std::memory_order_acquire)) return found;
  const HMODULE module = GetModuleHandleW(L"vulkan-1.dll");
  if (module == nullptr) return nullptr;
  auto* const loader = new Loader();
  loader->module = module;
  loader->get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
  loader->get_physical_device_properties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(GetProcAddress(module, "vkGetPhysicalDeviceProperties2"));
  loader->enumerate_physical_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetProcAddress(module, "vkEnumeratePhysicalDevices"));
  loader->enumerate_physical_device_groups =
      reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(GetProcAddress(module, "vkEnumeratePhysicalDeviceGroups"));
  loader->destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(GetProcAddress(module, "vkDestroyDevice"));
  loader->get_physical_device_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(GetProcAddress(module, "vkGetPhysicalDeviceFeatures"));
  loader->get_physical_device_format_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(GetProcAddress(module, "vkGetPhysicalDeviceFormatProperties"));
  loader->get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(module, "vkGetDeviceProcAddr"));
  loader->enumerate_device_extensions =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(GetProcAddress(module, "vkEnumerateDeviceExtensionProperties"));
  loader->get_memory_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(GetProcAddress(module, "vkGetPhysicalDeviceMemoryProperties"));
  loader->create_device = reinterpret_cast<PFN_vkCreateDevice>(GetProcAddress(module, "vkCreateDevice"));
  if (loader->get_device_proc_addr == nullptr || loader->enumerate_device_extensions == nullptr || loader->get_memory_properties == nullptr
      || loader->create_device == nullptr) {
    delete loader;
    return nullptr;
  }
  const Loader* expected = nullptr;
  if (!cached.compare_exchange_strong(expected, loader, std::memory_order_acq_rel)) {
    delete loader;  // another thread got there first
    return expected;
  }
  return loader;
}

std::optional<Device> Device::Open(VkDevice device) {
  const Loader* const loader = Loader::Get();
  if (loader == nullptr || device == VK_NULL_HANDLE) return std::nullopt;
  Device opened;
  opened.handle = device;
  bool complete = true;
  const auto resolve = [&](const char* name) { return loader->get_device_proc_addr(device, name); };
  const auto required = [&](auto* function, const char* name) {
    *function = reinterpret_cast<std::remove_reference_t<decltype(*function)>>(resolve(name));
    complete = complete && (*function != nullptr);
  };
  required(&opened.vkCreateImage, "vkCreateImage");
  required(&opened.vkDestroyImage, "vkDestroyImage");
  required(&opened.vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
  required(&opened.vkAllocateMemory, "vkAllocateMemory");
  required(&opened.vkFreeMemory, "vkFreeMemory");
  required(&opened.vkBindImageMemory, "vkBindImageMemory");
  required(&opened.vkCreateSemaphore, "vkCreateSemaphore");
  required(&opened.vkDestroySemaphore, "vkDestroySemaphore");
  required(&opened.vkCreateFence, "vkCreateFence");
  required(&opened.vkDestroyFence, "vkDestroyFence");
  required(&opened.vkGetFenceStatus, "vkGetFenceStatus");
  required(&opened.vkWaitForFences, "vkWaitForFences");
  required(&opened.vkResetFences, "vkResetFences");
  required(&opened.vkQueueSubmit, "vkQueueSubmit");
  if (!complete) return std::nullopt;
  opened.vkGetMemoryWin32HandlePropertiesKHR =
      reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(resolve("vkGetMemoryWin32HandlePropertiesKHR"));
  opened.vkImportSemaphoreWin32HandleKHR =
      reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(resolve("vkImportSemaphoreWin32HandleKHR"));
  opened.vkGetSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(resolve("vkGetSemaphoreCounterValue"));
  if (opened.vkGetSemaphoreCounterValue == nullptr) {
    opened.vkGetSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(resolve("vkGetSemaphoreCounterValueKHR"));
  }
  if (std::optional<DeviceRecord> record = DeviceHook::Find(device)) {
    opened.record = std::move(*record);
    opened.seen = true;
  } else {
    // Design §2.5: a device the hook never saw can only run CPU-ordered, and only when it can import memory at all.
    opened.record = {
        .memory_win32 = (opened.vkGetMemoryWin32HandlePropertiesKHR != nullptr),
        .not_adjusted = "Uplift did not see this device being created",
    };
  }
  return opened;
}

}  // namespace uplift::vk
