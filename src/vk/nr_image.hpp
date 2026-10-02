#pragma once

#include <vulkan/vulkan.h>

#include <nvsdk_ngx_defs_vk.h>  // after Vulkan's types: a separate block keeps clang-format from sorting it first

#include <cstdint>

#include "vk/nr_functions.hpp"

namespace uplift::vk {

// Plan 13: an image Uplift owns on the game's device (the colour pipeline's models, the zero motion, the placeholders): one dedicated device-local
// allocation, a 2D view of the whole image, and the NVSDK_NGX_Resource_VK the snippet is handed for it (NR's A, B and zero motion travel as pointers
// to `ngx`, nr/vk_handles.hpp). Header-only on purpose: the colour pipeline (uplift_color) and the sources (uplift_sources) both make images, and
// neither links the Vulkan side's library.
struct NrImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  NVSDK_NGX_Resource_VK ngx = {};
};

// The first memory type in `bits` with all of `flags`, or UINT32_MAX.
[[nodiscard]] inline uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits, VkMemoryPropertyFlags flags) {
  for (uint32_t index = 0u; index < memory.memoryTypeCount; ++index) {
    if ((bits & (1u << index)) != 0u && (memory.memoryTypes[index].propertyFlags & flags) == flags) return index;
  }
  return UINT32_MAX;
}

// Frees whatever of `image` exists (a half-made one included) and resets it. The device must be idle for it (the caller's rule).
inline void DestroyNrImage(const NrFunctions& functions, VkDevice device, NrImage* image) {
  if (image->view != VK_NULL_HANDLE) {
    functions.vkDestroyImageView(device, image->view, nullptr);
  }
  if (image->image != VK_NULL_HANDLE) {
    functions.vkDestroyImage(device, image->image, nullptr);
  }
  if (image->memory != VK_NULL_HANDLE) {
    functions.vkFreeMemory(device, image->memory, nullptr);
  }
  *image = {};
}

// A 2D optimal-tiling image of `format` in UNDEFINED (every recording that uses it moves it to GENERAL first: design §3.5). `read_write` is the
// NVSDK_NGX_Resource_VK's ReadWrite. False when any call failed (nothing is left allocated then).
[[nodiscard]] inline bool CreateNrImage(const NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, VkFormat format,
                                        uint32_t width, uint32_t height, VkImageUsageFlags usage, bool read_write, NrImage* out) {
  const VkImageCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {width, height, 1u},
      .mipLevels = 1u,
      .arrayLayers = 1u,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  NrImage made;
  bool worked = (functions.vkCreateImage(device, &create, nullptr, &made.image) == VK_SUCCESS);
  if (worked) {
    VkMemoryRequirements requirements = {};
    functions.vkGetImageMemoryRequirements(device, made.image, &requirements);
    const uint32_t type = FindMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    worked = (type != UINT32_MAX && functions.vkAllocateMemory(device, &allocate, nullptr, &made.memory) == VK_SUCCESS
              && functions.vkBindImageMemory(device, made.image, made.memory, 0u) == VK_SUCCESS);
  }
  if (worked) {
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = made.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
    };
    worked = (functions.vkCreateImageView(device, &view, nullptr, &made.view) == VK_SUCCESS);
  }
  if (!worked) {
    DestroyNrImage(functions, device, &made);
    return false;
  }
  made.ngx.Resource.ImageViewInfo = {
      .ImageView = made.view,
      .Image = made.image,
      .SubresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
      .Format = format,
      .Width = width,
      .Height = height,
  };
  made.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
  made.ngx.ReadWrite = read_write;
  *out = made;
  return true;
}

}  // namespace uplift::vk
