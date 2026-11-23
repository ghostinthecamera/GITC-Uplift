#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nr/vk_handles.hpp"
#include "vk/command_ring.hpp"
#include "vk/nr_functions.hpp"

namespace uplift::color {

// HUD-less (design §4), Vulkan: Begin copies the game's HUD-less image H into N, left in the present layout for NR's recording; EndAndClose
// puts the HUD back (back buffer = F + (1 - A) * (N - H)) when NR applied, and leaves N where DLSS-G reads it (N_REST_LAYOUT, the probe's).
// Two submissions of its own CommandRing per frame, each consuming and restoring the present's wait semaphores. Classic descriptor sets: a
// device on the bridge or helper route may lack push descriptors. Not thread-safe: the caller's lock. Freed only from the vkDestroyDevice detour.
class HudComposerVk {
 public:
  static constexpr uint32_t RING = 4u;  // HUD_RING 3 plus one: frame generation keeps up to three frames in flight
  static constexpr uint64_t RETIRE_PRESENTS = 16u;
  static constexpr VkImageLayout N_REST_LAYOUT = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // Begin and EndAndClose submit through a CommandRing each, so frame generation's frames in flight never exhaust one ring of both; both
  // rings' current slots must be free before Begin.

  HudComposerVk() = default;
  HudComposerVk(const HudComposerVk&) = delete;
  HudComposerVk& operator=(const HudComposerVk&) = delete;
  ~HudComposerVk() = default;  // frees nothing: Destroy, from the vkDestroyDevice detour

  bool Initialize(const vk::NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, uint32_t family,
                  std::span<const uint32_t> queue_families, std::string* error);

  struct Inputs {
    VkImage back_buffer = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;  // the swapchain's
    VkExtent2D size = {};
    VkImage hudless = VK_NULL_HANDLE;
    VkFormat hudless_format = VK_FORMAT_UNDEFINED;
    VkImageLayout hudless_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage opacity = VK_NULL_HANDLE;
    VkFormat opacity_format = VK_FORMAT_UNDEFINED;
    VkImageLayout opacity_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool opacity_from_ui_colour = false;
  };
  struct Begun {
    VkImage image = VK_NULL_HANDLE;  // N, in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    VkFormat format = VK_FORMAT_UNDEFINED;
    const NVSDK_NGX_Resource_VK* ngx = nullptr;  // N as DLSS-G is given it, alive with Owner()
  };
  // Nullopt, submitting nothing, when the format has no pass, fewer than SUBMISSIONS ring slots are free (never waited for), no N is free,
  // an image could not be made or the submission failed: the frame is hidden.
  std::optional<Begun> Begin(VkQueue queue, std::span<const VkSemaphore> present_waits, const Inputs& inputs, uint64_t present_number);
  // Always after a Begin that returned N. `applied`: NR recorded passes into N and the route's submission succeeded.
  VkResult EndAndClose(VkQueue queue, std::span<const VkSemaphore> present_waits, bool applied);
  [[nodiscard]] std::shared_ptr<const void> Owner() const;
  void Retire(uint64_t present_number);
  // No longer wanted: the ring retires as on a size change.
  void Release(uint64_t present_number);
  [[nodiscard]] size_t RetiringCount() const { return retiring_.size(); }
  void Destroy();

 private:
  struct Owned {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;      // the pass's read, in the image's UNORM or float format
    VkImageView ngx_view = VK_NULL_HANDLE;  // DLSS-G's, in the game's HUD-less format
    NVSDK_NGX_Resource_VK ngx = {};
  };
  struct Slot {
    std::shared_ptr<Owned> n;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::vector<VkImageView> transient;  // views of the game's images, kept until the slot's submission finished
    std::optional<uint32_t> ring_slot;   // the CommandRing slot of its last submission
    bool fresh = true;                   // never used: N starts undefined
  };
  struct Ring {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFormat hudless_format = VK_FORMAT_UNDEFINED;
    VkExtent2D size = {};
    std::array<Slot, RING> slots;
    Owned final_copy, hudless_copy, output;
    uint64_t retired_at = 0u;
  };
  bool EnsureRing(const Inputs& inputs, uint64_t present_number);
  bool MakeImage(VkFormat format, VkExtent2D size, VkImageUsageFlags usage, bool mutable_format, VkFormat view_format, Owned* out);
  void Free(Owned& owned);
  void FreeRing(Ring& ring);
  VkImageView MakeView(VkImage image, VkFormat format);
  [[nodiscard]] bool SlotDone(const Slot& slot) const;

  vk::NrFunctions functions_{};
  PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
  PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
  PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
  PFN_vkFreeDescriptorSets vkFreeDescriptorSets = nullptr;
  PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
  PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
  PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_ = {};
  std::vector<uint32_t> families_;
  std::unique_ptr<vk::CommandRing> begin_commands_;
  std::unique_ptr<vk::CommandRing> end_commands_;  // its slot marks when a slot's last frame finished
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkPipeline, 3> pipelines_ = {};  // rgba8, rgb10a2, rgba16f storage
  std::unique_ptr<Ring> ring_;
  std::vector<std::unique_ptr<Ring>> retiring_;
  bool allocation_failed_ = false;
  VkFormat failed_format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D failed_size_ = {};
  Inputs frame_;
  uint32_t slot_ = 0u;
  uint32_t next_slot_ = 0u;
  bool begun_ = false;
};

}  // namespace uplift::color
