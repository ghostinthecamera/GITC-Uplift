#include "addon/swapchain_usage.hpp"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "addon/reshade_api.hpp"
#include "addon/uplift_catch.hpp"
#include "vk/frame.hpp"

namespace uplift::addon {
namespace {

namespace api = reshade::api;

std::atomic<bool> g_vulkan_swapchain_event{false};
std::atomic<bool> g_desc_has_fullscreen_state{false};
std::atomic<bool> g_fullscreen_skip_logged{false};

// ReShade 6.8's swapchain_desc (include/reshade_api_device.hpp of 6.8): the pinned 6.0.0 type's members, then fullscreen_state, the refresh rate and
// sync_interval. The handler gets a reference to ReShade's own object, so when ReShade is 6.8 or later these bytes are there to read.
struct SwapchainDesc68 {
  api::resource_desc back_buffer;
  uint32_t back_buffer_count;
  uint32_t present_mode;
  uint32_t present_flags;
  bool fullscreen_state;
  float fullscreen_refresh_rate;
  uint32_t sync_interval;
};
static_assert(offsetof(SwapchainDesc68, back_buffer) == offsetof(api::swapchain_desc, back_buffer));
static_assert(offsetof(SwapchainDesc68, back_buffer_count) == offsetof(api::swapchain_desc, back_buffer_count));
static_assert(offsetof(SwapchainDesc68, present_mode) == offsetof(api::swapchain_desc, present_mode));
static_assert(offsetof(SwapchainDesc68, present_flags) == offsetof(api::swapchain_desc, present_flags));
static_assert(sizeof(SwapchainDesc68) >= sizeof(api::swapchain_desc));

// Vulkan only, and no Uplift lock, ever (Plan 10 C-1): ReShade raises this inside vkCreateSwapchainKHR, and other APIs' swap chains pass through
// it too. Returning true makes ReShade rebuild the create info from `desc`; the round trip keeps every other field and every unmapped usage bit
// (vulkan_hooks_swapchain.cpp:220-255, vulkan_impl_type_convert.cpp:860-910), except for a swap chain that asks for exclusive fullscreen (below).
bool OnCreateSwapchain(api::device_api device_api, api::swapchain_desc& desc, void* /*hwnd*/) {
  try {
    if (device_api != api::device_api::vulkan) return false;
    if (g_desc_has_fullscreen_state.load(std::memory_order_relaxed) && reinterpret_cast<const SwapchainDesc68&>(desc).fullscreen_state) {
      // Final review I-2 (see the header): no rebuild. The swap chain keeps ReShade's own usage bits, without TRANSFER_DST, and the card says why.
      vk::NoteExclusiveFullscreenSwapchain();
      if (!g_fullscreen_skip_logged.exchange(true, std::memory_order_relaxed)) {
        nr::Log(nr::LogLevel::INFO,
                "Vulkan: swap-chain copy access not added (create_swapchain): the game asks for exclusive fullscreen, so NR cannot write back into "
                "this swap chain; use borderless or windowed mode for NR");
      }
      return false;
    }
    desc.back_buffer.usage |= api::resource_usage::copy_dest | api::resource_usage::copy_source;
    if (!g_vulkan_swapchain_event.exchange(true, std::memory_order_relaxed)) {
      nr::Log(nr::LogLevel::INFO, "Vulkan: swap-chain copy access added (create_swapchain)");
    }
    return true;
  }
  UPLIFT_CATCH("create_swapchain", false)
}

}  // namespace

void RegisterCreateSwapchainEvent(bool desc_has_fullscreen_state) {
  g_desc_has_fullscreen_state.store(desc_has_fullscreen_state, std::memory_order_relaxed);
  const auto register_event = reinterpret_cast<void (*)(reshade::addon_event, void*)>(
      GetProcAddress(reshade::internal::get_reshade_module_handle(), "ReShadeRegisterEvent"));
  if (register_event != nullptr) {
    register_event(static_cast<reshade::addon_event>(CREATE_SWAPCHAIN_EVENT), reinterpret_cast<void*>(&OnCreateSwapchain));
  }
}

bool CreateSwapchainEventSeen() {
  return g_vulkan_swapchain_event.load(std::memory_order_relaxed);
}

}  // namespace uplift::addon
