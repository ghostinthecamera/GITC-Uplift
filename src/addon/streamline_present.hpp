#pragma once

#include <Windows.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>

#include <atomic>

namespace uplift::addon::streamline {

// Borrowed pointers to VkForward are valid only while its forwarding scope is alive.
// An unclaimed scope must complete release_tick, which takes the add-on lock, before it may destroy this object.
// Takers dereference only under that same lock and disarm release_tick after their final read.
struct VkForward {
  VkPresentInfoKHR info;
  VkResult failure = VK_SUCCESS;
  bool native_tick = false;
  using ReleaseTick = void (*)(const VkForward*) noexcept;
  std::atomic<ReleaseTick> release_tick{};
  bool TakeNativeTick() noexcept {
    const bool ticked = native_tick;
    release_tick.store(nullptr, std::memory_order_release);  // Last access: the outer scope may now end on another thread.
    return ticked;
  }
  // A forwarded call that produces no accepted inner present must not leave its tick for a later frame.
  void ReleaseNativeTick() noexcept {
    if (const auto release = release_tick.load(std::memory_order_acquire)) release(this);
  }
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
  bool diagnostic_dxgi = false;
};

bool Start(HMODULE self, const Observer& observer);
void Poll();
void NoteReshadeSwapchain(void* chain);
bool WatchGameFacing(IDXGISwapChain* chain, bool trace_method = false);
bool InsideOuterPresent();
bool Installed();
// The game's Streamline interposer (sl.interposer.dll) is in the process, so frame generation can exist here; false until Poll or the game's first
// resolved Streamline export finds it, and for good in a game without Streamline (the overlay greys Before FG with why).
bool StreamlineLoaded();
enum class FgMode { UNKNOWN, OFF, ON, AUTO };
FgMode Mode();

[[nodiscard]] constexpr bool FrameGenerationActive(FgMode mode, bool detected) {
  return mode != FgMode::OFF && (mode == FgMode::ON || mode == FgMode::AUTO || detected);
}

// Whether the overlay shows the game's frame generation as running: the stage diagram's FG box and the Before FG choice read this one
// fact. `detected`: the device's frame generation detector or a running Before FG pass; `owns_frame`: Before FG's outer Present owns the frame.
[[nodiscard]] constexpr bool FrameGenerationShown(FgMode mode, bool detected, bool owns_frame) {
  return owns_frame || FrameGenerationActive(mode, detected);
}

}  // namespace uplift::addon::streamline
