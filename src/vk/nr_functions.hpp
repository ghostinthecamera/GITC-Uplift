#pragma once

#include <vulkan/vulkan.h>

#include <optional>

namespace uplift::vk {

// Plan 13 (key decision e): the device functions native NR work needs (VkHost's barrier, NrCompletion's semaphore and events, and, in later
// batches, the colour pipeline's images, buffers, shaders and pipelines), resolved through the loader's vkGetDeviceProcAddr: the game's own chain,
// so ReShade's layer sits in front of the ones it intercepts and the rest go straight to the driver. The same list serves the add-on and the smokes.
// Plan 19: and native NR at Present's own command buffers (vk::CommandRing), its copies and blits, and its submissions to the effect queue.
#define UPLIFT_VK_NR_FUNCTIONS(X)  \
  X(vkCreateImage)                 \
  X(vkDestroyImage)                \
  X(vkGetImageMemoryRequirements)  \
  X(vkAllocateMemory)              \
  X(vkFreeMemory)                  \
  X(vkBindImageMemory)             \
  X(vkCreateImageView)             \
  X(vkDestroyImageView)            \
  X(vkCreateBuffer)                \
  X(vkDestroyBuffer)               \
  X(vkGetBufferMemoryRequirements) \
  X(vkBindBufferMemory)            \
  X(vkMapMemory)                   \
  X(vkUnmapMemory)                 \
  X(vkCreateShaderModule)          \
  X(vkDestroyShaderModule)         \
  X(vkCreateDescriptorSetLayout)   \
  X(vkDestroyDescriptorSetLayout)  \
  X(vkCreatePipelineLayout)        \
  X(vkDestroyPipelineLayout)       \
  X(vkCreateComputePipelines)      \
  X(vkDestroyPipeline)             \
  X(vkCmdBindPipeline)             \
  X(vkCmdDispatch)                 \
  X(vkCmdPipelineBarrier)          \
  X(vkCmdCopyImageToBuffer)        \
  X(vkCreateEvent)                 \
  X(vkDestroyEvent)                \
  X(vkCmdSetEvent)                 \
  X(vkGetEventStatus)              \
  X(vkResetEvent)                  \
  X(vkCreateSemaphore)             \
  X(vkDestroySemaphore)            \
  X(vkCreateCommandPool)           \
  X(vkDestroyCommandPool)          \
  X(vkResetCommandPool)            \
  X(vkAllocateCommandBuffers)      \
  X(vkBeginCommandBuffer)          \
  X(vkEndCommandBuffer)            \
  X(vkCmdCopyImage)                \
  X(vkCmdBlitImage)                \
  X(vkCmdClearColorImage)          \
  X(vkCreateFence)                 \
  X(vkDestroyFence)                \
  X(vkGetFenceStatus)              \
  X(vkResetFences)                 \
  X(vkWaitForFences)               \
  X(vkQueueSubmit)

// Core 1.2 functions whose KHR alias is the fallback (a device that enabled the extension, not 1.2), and the one extension function: NGX's own
// requirement (VK_KHR_push_descriptor), so a device without it cannot run native NR at all.
#define UPLIFT_VK_NR_FUNCTIONS_WITH_KHR_ALIAS(X) \
  X(vkGetSemaphoreCounterValue)                  \
  X(vkWaitSemaphores)

struct NrFunctions {
#define UPLIFT_VK_NR_MEMBER(name) PFN_##name name = nullptr;
  UPLIFT_VK_NR_FUNCTIONS(UPLIFT_VK_NR_MEMBER)
  UPLIFT_VK_NR_FUNCTIONS_WITH_KHR_ALIAS(UPLIFT_VK_NR_MEMBER)
#undef UPLIFT_VK_NR_MEMBER
  PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR = nullptr;

  // nullopt when the loader is missing or one function is not there (the push-descriptor one included).
  [[nodiscard]] static std::optional<NrFunctions> Open(VkDevice device);
};

}  // namespace uplift::vk
