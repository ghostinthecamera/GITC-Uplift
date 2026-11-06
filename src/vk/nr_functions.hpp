#pragma once

#include <vulkan/vulkan.h>

#include <optional>

namespace uplift::vk {

// Device functions for native NR barriers, completion, colour resources and pipelines, and CommandRing recording/submission.
// Resolved through the loader's vkGetDeviceProcAddr: intercepted calls pass through ReShade's layer; the rest reach the driver.
// The add-on and tests use the same function list.
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
  [[nodiscard]] static std::optional<NrFunctions> Open(VkDevice device, bool pipeline = true);
};

}  // namespace uplift::vk
