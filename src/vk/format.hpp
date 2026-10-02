#pragma once

#include <vulkan/vulkan.h>

#include <dxgiformat.h>

namespace uplift::vk {

// Plan 11 (Vulkan design §3.6): the VkFormat that shares memory with a Direct3D 12 texture of `format`. Covers the display formats (BGRA8 and RGBA8,
// UNORM and sRGB, RGB10A2, RGBA16F) and the UPLIFT_MASK and UPLIFT_MV formats (color::DescribeMaskFormat's set). VK_FORMAT_UNDEFINED for anything else,
// typeless ones included: see SharedFormatOf.
[[nodiscard]] VkFormat VkFormatOf(DXGI_FORMAT format);

// The format the shared colour texture is made in for a back buffer that ReShade describes as `format`. ReShade reports a Vulkan swap chain's back buffer
// as its TYPELESS family (swapchain_impl's get_resource_desc, api::format_to_typeless), whichever of UNORM and sRGB the game's VkFormat is, so the
// shared texture takes the family's UNORM (or float) member: a vkCmdCopyImage between size-compatible formats is a raw copy, and the colour pipeline
// reads the values as stored either way. Any other format is its own.
[[nodiscard]] DXGI_FORMAT SharedFormatOf(DXGI_FORMAT format);

}  // namespace uplift::vk
