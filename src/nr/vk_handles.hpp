#pragma once
// Plan 13 (design §3.2, key decision b): the Session, the Feature and the Host are typed for D3D12, and pass the command list and the pass
// textures through without touching them. On Vulkan they carry a VkCommandBuffer and a pointer to an Uplift-owned NVSDK_NGX_Resource_VK.
// These casts are the only place that pun is written; only VkHost, VkFeature, VkNrPipeline and VkDlssContext turn a handle back. The queue
// pair is the same pun for the Timeline's opaque present-queue identity (a ReShade command_queue* or a VkQueue, never dereferenced as a D3D12 queue).
#include <d3d12.h>
#include <vulkan/vulkan.h>

#include <nvsdk_ngx_defs_vk.h>  // after Vulkan's types: a separate block keeps clang-format from sorting it first

namespace uplift::nr {

[[nodiscard]] inline ID3D12GraphicsCommandList* AsList(VkCommandBuffer buffer) { return reinterpret_cast<ID3D12GraphicsCommandList*>(buffer); }
[[nodiscard]] inline VkCommandBuffer VkListOf(ID3D12GraphicsCommandList* list) { return reinterpret_cast<VkCommandBuffer>(list); }
[[nodiscard]] inline ID3D12Resource* AsResource(const NVSDK_NGX_Resource_VK* resource) {
  return reinterpret_cast<ID3D12Resource*>(const_cast<NVSDK_NGX_Resource_VK*>(resource));
}
[[nodiscard]] inline NVSDK_NGX_Resource_VK* VkResourceOf(ID3D12Resource* resource) { return reinterpret_cast<NVSDK_NGX_Resource_VK*>(resource); }
[[nodiscard]] inline ID3D12CommandQueue* AsQueue(void* queue) { return reinterpret_cast<ID3D12CommandQueue*>(queue); }
[[nodiscard]] inline void* VkQueueOf(ID3D12CommandQueue* queue) { return reinterpret_cast<void*>(queue); }

}  // namespace uplift::nr
