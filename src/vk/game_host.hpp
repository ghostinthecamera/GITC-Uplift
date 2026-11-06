#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace uplift::vk {

// ReShade's api::resource_usage values (the pinned reshade_api_resource.hpp), so the ReShade-free Vulkan side can name the states its barriers
// move between. The add-on static_asserts them (reshade_vk_host.cpp); the smokes map them the way ReShade's Vulkan backend does
// (vulkan_impl_type_convert.cpp: convert_usage_to_image_layout).
enum class Usage : uint32_t {
  UNDEFINED = 0u,
  RENDER_TARGET = 0x4u,
  SHADER_RESOURCE = 0xC0u,
  UNORDERED_ACCESS = 0x8u,
  COPY_DEST = 0x400u,
  COPY_SOURCE = 0x800u,
  GENERAL = 0x80000000u,
  PRESENT = 0x80000000u | 0x4u | 0x800u,
};
// Usages combine as ReShade's flags do (a global barrier names every access it orders).
[[nodiscard]] constexpr Usage operator|(Usage first, Usage second) {
  return static_cast<Usage>(static_cast<uint32_t>(first) | static_cast<uint32_t>(second));
}

// Host-owned Vulkan recording and queue operations. The ReShade host records through ReShade's command-list API.
// PresentRelayHost owns its command buffers and consumes/re-signals the application's present semaphores around that work.
// Raw Vulkan commands never target ReShade's immediate list. Not thread-safe: the caller serialises operations under its lock.
class GameHost {
 public:
  virtual ~GameHost() = default;
  // Reports a usable queue; the ReShade host is invalid when its immediate list could not be created or the queue was destroyed.
  [[nodiscard]] virtual bool Valid() const = 0;
  // A null `image` is a global memory barrier (ReShade: ALL_COMMANDS to ALL_COMMANDS, with the accesses `before` and `after` name): what the queue
  // ran earlier finishes, and its writes of that kind are visible, before anything after it.
  virtual void Barrier(VkImage image, Usage before, Usage after) = 0;
  // One barrier call with two entries: a global memory barrier (`global_before` to `global_after`) and `image`'s transition. ReShade ORs the
  // stage masks of a call's entries (vulkan_impl_command_list.cpp:26-90), so the image's layout change takes the global barrier's ALL_COMMANDS
  // scope too, where a transition from the present state alone has only TOP_OF_PIPE to start from.
  virtual void BarrierWithGlobal(VkImage image, Usage before, Usage after, Usage global_before, Usage global_after) = 0;
  virtual void Copy(VkImage source, VkImage destination) = 0;     // the whole image, equal extent
  virtual bool Signal(VkSemaphore timeline, uint64_t value) = 0;  // flushes what was recorded, then signals
  virtual bool Wait(VkSemaphore timeline, uint64_t value) = 0;    // everything recorded or submitted later waits for it, on the GPU
  virtual bool FlushWithFence(VkFence fence) = 0;                 // flushes, then an empty submission signals `fence`
  virtual void DestroyImage(VkImage image) = 0;                   // safe inside destroy_device: only the device is needed
};

}  // namespace uplift::vk
