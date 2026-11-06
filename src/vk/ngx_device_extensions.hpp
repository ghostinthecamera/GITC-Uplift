#pragma once

#include <vulkan/vulkan.h>

namespace uplift::vk {

// The snippet's NGX device-extension requirements for native NR; the game or Uplift must enable them on the device.
inline constexpr const char* NGX_DEVICE_EXTENSIONS[] = {
    VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
    VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};

}  // namespace uplift::vk
