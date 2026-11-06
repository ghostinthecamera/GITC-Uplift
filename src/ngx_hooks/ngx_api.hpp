#pragma once

#include <cstdint>

namespace uplift::ngx_hooks {

// Which of the core's APIs a hooked call came through (here so the registry can name it). On Vulkan the command list
// is a VkCommandBuffer and every resource pointer an NVSDK_NGX_Resource_VK* (nr/vk_handles.hpp); on Direct3D 11 the command list is the
// evaluate's ID3D11DeviceContext and every resource an ID3D11Resource (nr/d3d11_handles.hpp). Both ride in the Direct3D 12-typed pointers; never
// dereference one as a Direct3D 12 object.
enum class NgxApi : uint8_t {
  D3D12,
  VULKAN,
  D3D11,
};

}  // namespace uplift::ngx_hooks
