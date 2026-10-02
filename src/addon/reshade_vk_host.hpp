#pragma once

#include <vulkan/vulkan.h>

#include <Windows.h>

#include <cstdint>
#include <string>
#include <type_traits>

#include "addon/reshade_api.hpp"
#include "vk/frame.hpp"
#include "vk/game_host.hpp"

namespace uplift::addon {

// ReShade's handles are 64 bits wide in every build. A Vulkan handle is a pointer, except a non-dispatchable one in a 32-bit build, where it is
// already a uint64_t (and a same-type reinterpret_cast does not compile there).
template <class VulkanHandle>
[[nodiscard]] uint64_t ReshadeHandleOf(VulkanHandle handle) {
  if constexpr (std::is_pointer_v<VulkanHandle>) {
    return reinterpret_cast<uintptr_t>(handle);
  } else {
    return static_cast<uint64_t>(handle);
  }
}
template <class VulkanHandle>
[[nodiscard]] VulkanHandle VulkanHandleOf(uint64_t handle) {
  if constexpr (std::is_pointer_v<VulkanHandle>) {
    return reinterpret_cast<VulkanHandle>(static_cast<uintptr_t>(handle));
  } else {
    return static_cast<VulkanHandle>(handle);
  }
}

// ReShade's description of a Vulkan image as the ReShade-free side wants it (api::format is DXGI-numbered). A null resource is an empty description.
[[nodiscard]] vk::ImageInfo VulkanImageInfo(reshade::api::device* device, reshade::api::resource resource);

// The LUID of the NVIDIA adapter `device` runs on: ReShade 6.8's adapter_luid property (raw id 9; the pinned headers stop at 8), else DXGI's NVIDIA adapter.
// False, with `error` set, when the device is not NVIDIA's or its adapter is unknown.
[[nodiscard]] bool VulkanAdapterLuid(reshade::api::device* device, LUID* luid, std::string* error);

// Plan 11 (Vulkan design §3.1): vk::GameHost on ReShade's API. The queue is the effect runtime's (its immediate command list is what the technique
// and finish-effects events hand out), and every command goes through command_list::barrier / copy_texture_region and command_queue::signal /
// wait / flush_immediate_command_list, never a raw vkCmd* on ReShade's command buffer. FlushWithFence is the one own submission: an empty batch
// with a VkFence, through `submit` (the game's vkQueueSubmit chain), after the immediate list's flush. The present thread already holds that
// queue's lock (vulkan_hooks_swapchain.cpp:428-432) and ReShade's mutex is recursive.
//
// With a null queue (destroy_device: ReShade has already deleted its queues) only DestroyImage works and Valid() is false. DestroyImage is
// ReShade's destroy_resource, which vmaDestroyImage's with no allocation (vulkan_impl_device.cpp:734-761): it needs only the device.
class ReshadeVkHost final : public vk::GameHost {
 public:
  ReshadeVkHost(reshade::api::device* device, reshade::api::command_queue* queue, PFN_vkQueueSubmit submit)
      : device_(device), queue_(queue), submit_(submit), list_(queue != nullptr ? queue->get_immediate_command_list() : nullptr) {}

  [[nodiscard]] bool Valid() const override { return list_ != nullptr; }
  void Barrier(VkImage image, vk::Usage before, vk::Usage after) override;
  void BarrierWithGlobal(VkImage image, vk::Usage before, vk::Usage after, vk::Usage global_before, vk::Usage global_after) override;
  void Copy(VkImage source, VkImage destination) override;
  bool Signal(VkSemaphore timeline, uint64_t value) override;
  bool Wait(VkSemaphore timeline, uint64_t value) override;
  bool FlushWithFence(VkFence fence) override;
  void DestroyImage(VkImage image) override;

 private:
  reshade::api::device* device_;
  reshade::api::command_queue* queue_;
  PFN_vkQueueSubmit submit_;
  reshade::api::command_list* list_;
};

}  // namespace uplift::addon
