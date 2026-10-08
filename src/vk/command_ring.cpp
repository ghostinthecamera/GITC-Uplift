#include "vk/command_ring.hpp"

#include <format>

namespace uplift::vk {
namespace {

CommandRing::Status StatusOf(VkResult result) {
  switch (result) {
    case VK_SUCCESS:           return CommandRing::Status::READY;
    case VK_NOT_READY:         return CommandRing::Status::BUSY;
    case VK_TIMEOUT:           return CommandRing::Status::TIMEOUT;
    case VK_ERROR_DEVICE_LOST: return CommandRing::Status::LOST;
    default:                   return CommandRing::Status::FAILED;
  }
}

}  // namespace

std::unique_ptr<CommandRing> CommandRing::Create(const NrFunctions& functions, VkDevice device, uint32_t family, std::string* error) {
  std::unique_ptr<CommandRing> ring(new CommandRing(functions, device));
  const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                             .queueFamilyIndex = family};
  const VkFenceCreateInfo signalled = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
  const VkFenceCreateInfo unsignalled = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  bool made = (functions.vkCreateFence(device, &unsignalled, nullptr, &ring->wait_fence_) == VK_SUCCESS);
  for (Slot& slot : ring->slots_) {
    if (!made) break;
    made = (functions.vkCreateCommandPool(device, &pool_info, nullptr, &slot.pool) == VK_SUCCESS);
    if (made) {
      const VkCommandBufferAllocateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = slot.pool,
                                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1u};
      made = (functions.vkAllocateCommandBuffers(device, &buffer_info, &slot.buffer) == VK_SUCCESS
              && functions.vkCreateFence(device, &signalled, nullptr, &slot.fence) == VK_SUCCESS);
    }
  }
  if (!made) {
    ring->Destroy();  // nothing was submitted yet
    *error = std::format("Uplift's own command buffers could not be made on the effect queue's family {}", family);
    return nullptr;
  }
  return ring;
}

CommandRing::Status CommandRing::Begin(VkCommandBuffer* buffer) {
  Slot& slot = slots_[current_];
  if (slot.fence == VK_NULL_HANDLE) return Status::FAILED;  // a failed submission's fence could not be made again
  if (const Status status = StatusOf(functions_.vkGetFenceStatus(device_, slot.fence)); status != Status::READY) return status;
  // The pool's reset returns its buffer to the initial state, a recording begun and never submitted included.
  if (const VkResult reset = functions_.vkResetCommandPool(device_, slot.pool, 0u); reset != VK_SUCCESS) return StatusOf(reset);
  const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  if (const VkResult begun = functions_.vkBeginCommandBuffer(slot.buffer, &begin); begun != VK_SUCCESS) return StatusOf(begun);
  begun_ = true;
  *buffer = slot.buffer;
  return Status::READY;
}

VkResult CommandRing::Submit(VkQueue queue) {
  if (!begun_) return VK_ERROR_INITIALIZATION_FAILED;
  begun_ = false;
  Slot& slot = slots_[current_];
  if (const VkResult ended = functions_.vkEndCommandBuffer(slot.buffer); ended != VK_SUCCESS) return ended;
  if (const VkResult reset = functions_.vkResetFences(device_, 1u, &slot.fence); reset != VK_SUCCESS) return reset;
  const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1u, .pCommandBuffers = &slot.buffer};
  const VkResult submitted = functions_.vkQueueSubmit(queue, 1u, &submit, slot.fence);
  if (submitted != VK_SUCCESS) {
    // Nothing reached the queue, so nothing will signal the fence: a fresh signalled one keeps the slot free (an unsignalled one would stay busy forever).
    functions_.vkDestroyFence(device_, slot.fence, nullptr);
    slot.fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo signalled = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (functions_.vkCreateFence(device_, &signalled, nullptr, &slot.fence) != VK_SUCCESS) {
      slot.fence = VK_NULL_HANDLE;
    }
    return submitted;
  }
  current_ = (current_ + 1u) % SLOTS;
  return VK_SUCCESS;
}

CommandRing::Status CommandRing::WaitForQueue(const std::function<bool(VkFence)>& flush, std::chrono::milliseconds cap) {
  if (wait_pending_) {
    const Status status = StatusOf(functions_.vkGetFenceStatus(device_, wait_fence_));
    if (status != Status::READY) return status;
    wait_pending_ = false;
  }
  if (const VkResult reset = functions_.vkResetFences(device_, 1u, &wait_fence_); reset != VK_SUCCESS) return StatusOf(reset);
  if (!flush(wait_fence_)) {
    // A lost device answers an unsignalled fence's status with VK_ERROR_DEVICE_LOST; a healthy one says VK_NOT_READY, which is a plain failure here.
    return (functions_.vkGetFenceStatus(device_, wait_fence_) == VK_ERROR_DEVICE_LOST ? Status::LOST : Status::FAILED);
  }
  wait_pending_ = true;
  const uint64_t timeout = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(cap).count());
  const Status waited = StatusOf(functions_.vkWaitForFences(device_, 1u, &wait_fence_, VK_TRUE, timeout));
  if (waited == Status::READY) {
    wait_pending_ = false;
  }
  return waited;
}

bool CommandRing::WaitIdle(std::chrono::milliseconds cap) {
  std::array<VkFence, SLOTS + 1u> fences = {};
  uint32_t count = 0u;
  for (const Slot& slot : slots_) {
    if (slot.fence != VK_NULL_HANDLE) {
      fences[count++] = slot.fence;
    }
  }
  if (wait_pending_) {
    fences[count++] = wait_fence_;
  }
  if (count == 0u) return true;
  const uint64_t timeout = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(cap).count());
  return functions_.vkWaitForFences(device_, count, fences.data(), VK_TRUE, timeout) == VK_SUCCESS;
}

void CommandRing::Destroy() {
  for (Slot& slot : slots_) {
    if (slot.pool != VK_NULL_HANDLE) {
      functions_.vkDestroyCommandPool(device_, slot.pool, nullptr);  // frees its buffer too
    }
    if (slot.fence != VK_NULL_HANDLE) {
      functions_.vkDestroyFence(device_, slot.fence, nullptr);
    }
    slot = {};
  }
  if (wait_fence_ != VK_NULL_HANDLE) {
    functions_.vkDestroyFence(device_, wait_fence_, nullptr);
    wait_fence_ = VK_NULL_HANDLE;
  }
  wait_pending_ = false;
  begun_ = false;
}

}  // namespace uplift::vk
