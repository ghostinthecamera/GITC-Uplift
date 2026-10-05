#pragma once

#include <cstdint>

namespace uplift::addon {

// Plan 11 (Vulkan design §3.5, R51): ReShade's Vulkan layer gives a swap chain's images COLOR_ATTACHMENT and TRANSFER_SRC, not TRANSFER_DST, which
// NR's copy back needs. create_swapchain is event 97 in ReShade 6.8, with (device_api, swapchain_desc&, void* hwnd) (reshade_events.hpp:164-169,
// 1842). The pinned 6.0.0 headers know it only as enum value 7 with (desc, hwnd), a signature with no API: a handler could not tell a DXGI swap
// chain from a Vulkan one. 6.8 leaves 7 unused and never raises it, and a ReShade without 97 ignores the id (addon_manager.cpp:558), so Uplift
// registers 97 raw and never 7.
inline constexpr uint32_t CREATE_SWAPCHAIN_EVENT = 97u;

// AddonInit, beside RegisterCreateDeviceEvent. The handler returns before doing anything for every API but Vulkan, takes no lock at all, and
// touches only desc.back_buffer.usage and, when `desc_has_fullscreen_state`, reads desc.fullscreen_state (their offsets are the same in both layouts
// of swapchain_desc, which is longer in 6.8; desc is never copied).
//
// Final review I-2: for a swap chain that asks for exclusive fullscreen (the app chained VkSurfaceFullScreenExclusiveInfoEXT with ALLOWED, which 6.8
// reports as desc.fullscreen_state) the handler returns false and adds nothing. Returning true would make ReShade rebuild the create info from its
// own copy of that struct, dropping everything the app chained before it (present modes, Reflex's latency struct, ReShade's own image format list
// with the mutable-format flag still set: VUID-VkSwapchainCreateInfoKHR-flags-03168), in a game that may never use NR. NR then reads "The game asks
// for exclusive fullscreen" on that swap chain (vk::EXCLUSIVE_FULLSCREEN_PROBLEM). `desc_has_fullscreen_state`: the ReShade is 6.8 or later
// (MIN_RESHADE_VULKAN_EVENTS); with an older one nothing is read and today's behaviour stays.
void RegisterCreateSwapchainEvent(bool desc_has_fullscreen_state);

// 1.1.5 (MudRunner): Uplift asked ReShade for a Direct3D 9Ex device (UseD3D9Ex). ReShade's upgrade calls CreateDeviceEx without a fullscreen display
// mode (d3d9.cpp:306), which Windows refuses for a game in exclusive fullscreen (D3DERR_INVALIDCALL), so from now on the handler turns a Direct3D 9
// swap chain's exclusive fullscreen into a window of the same size (borderless), with ReShade 6.8 or later; creation and every Reset.
void NoteD3D9ExRequested();

// A Vulkan swap chain went through the handler since the process started.
[[nodiscard]] bool CreateSwapchainEventSeen();

}  // namespace uplift::addon
