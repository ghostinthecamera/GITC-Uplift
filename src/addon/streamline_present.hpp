#pragma once

#include <Windows.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>

namespace uplift::addon::streamline {

struct VkForward {
  VkPresentInfoKHR info;
  VkResult failure = VK_SUCCESS;
};

// The callbacks run before forwarding the application's call, outside the hook installation lock.
struct Observer {
  void (*dxgi_created)(IDXGISwapChain* outer, IUnknown* creation_device, void* reshade_chain);
  void (*dxgi_present)(IDXGISwapChain* outer, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);
  void (*vk_present)(VkQueue queue, const VkPresentInfoKHR* application, VkForward* forwarded);
  void (*dxgi_resize)(IDXGISwapChain*, UINT count, IUnknown* const* queues, void* inner) = nullptr;
  void (*dxgi_color)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE) = nullptr;
  void (*vk_created)(VkDevice, VkSwapchainKHR, const VkSwapchainCreateInfoKHR&, void* inner) = nullptr;
  void (*vk_images)(VkDevice, VkSwapchainKHR, uint32_t, const VkImage*) = nullptr;
  void (*vk_destroyed)(VkDevice, VkSwapchainKHR) = nullptr;
  void (*vk_acquired)(VkDevice, VkSwapchainKHR, uint32_t) = nullptr;
  VkImageUsageFlags (*vk_usage)(VkDevice, const VkSwapchainCreateInfoKHR&) = nullptr;
  void (*dxgi_retry)(IDXGISwapChain*) = nullptr;
};

bool Start(HMODULE self, const Observer& observer);
void Poll();
void NoteReshadeSwapchain(void* chain);
bool WatchGameFacing(IDXGISwapChain* chain);
bool InsideOuterPresent();
bool Installed();
enum class FgMode { UNKNOWN, OFF, ON, AUTO };
FgMode Mode();

}  // namespace uplift::addon::streamline
