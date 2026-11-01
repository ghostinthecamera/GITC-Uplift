#include "vk/present_relay_host.hpp"
#include <algorithm>

namespace uplift::vk {
namespace {
VkImageLayout Layout(Usage usage) {
  switch (usage) {
    case Usage::PRESENT: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case Usage::COPY_SOURCE: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case Usage::COPY_DEST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case Usage::RENDER_TARGET: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case Usage::SHADER_RESOURCE: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case Usage::UNDEFINED: return VK_IMAGE_LAYOUT_UNDEFINED;
    default: return VK_IMAGE_LAYOUT_GENERAL;
  }
}
VkAccessFlags Access(Usage usage) {
  switch (usage) {
    case Usage::PRESENT: case Usage::UNDEFINED: return 0;
    case Usage::COPY_SOURCE: return VK_ACCESS_TRANSFER_READ_BIT;
    case Usage::COPY_DEST: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case Usage::RENDER_TARGET: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case Usage::SHADER_RESOURCE: return VK_ACCESS_SHADER_READ_BIT;
    default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  }
}
}

std::unique_ptr<PresentRelayHost> PresentRelayHost::Create(const NrFunctions& functions, VkDevice device, VkQueue queue, uint32_t family, std::string* error) {
  std::unique_ptr<PresentRelayHost> host(new PresentRelayHost(functions, device, queue));
  host->ring_ = CommandRing::Create(functions, device, family, error);
  return host->ring_ ? std::move(host) : nullptr;
}
bool PresentRelayHost::Note(VkResult result) {
  if (failure_ == VK_SUCCESS && result != VK_SUCCESS) failure_ = result;
  return result == VK_SUCCESS;
}
VkCommandBuffer PresentRelayHost::Commands() {
  if (!Valid()) return VK_NULL_HANDLE;
  if (!commands_) {
    const auto status = ring_->Begin(&commands_);
    if (status == CommandRing::Status::LOST) failure_ = VK_ERROR_DEVICE_LOST;
    else if (status == CommandRing::Status::FAILED) failure_ = VK_ERROR_INITIALIZATION_FAILED;
    else if (status != CommandRing::Status::READY) failure_ = VK_NOT_READY;
  }
  return commands_;
}
bool PresentRelayHost::Flush() {
  if (!commands_) return Valid();
  commands_ = VK_NULL_HANDLE;
  // A partial recording may have transitioned the game image without recording its restoration.
  return Valid() && Note(ring_->Submit(queue_));
}
bool PresentRelayHost::BeginFrame(std::span<const VkSemaphore> waits) {
  if (consumed_ || waits.empty()) return false;
  if (failure_ != VK_SUCCESS && failure_ != VK_NOT_READY) return false;
  failure_ = VK_SUCCESS;
  sizes_.clear();
  waits_.assign(waits.begin(), waits.end());
  if (!Commands()) return false;
  const std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = static_cast<uint32_t>(waits.size()),
                            .pWaitSemaphores = waits.data(), .pWaitDstStageMask = stages.data()};
  consumed_ = Note(functions_.vkQueueSubmit(queue_, 1u, &submit, VK_NULL_HANDLE));
  return consumed_;
}
VkResult PresentRelayHost::FinishFrame() {
  if (!consumed_) return VK_SUCCESS;
  Flush();
  const VkSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = static_cast<uint32_t>(waits_.size()), .pSignalSemaphores = waits_.data()};
  const VkResult result = functions_.vkQueueSubmit(queue_, 1u, &signal, VK_NULL_HANDLE);
  consumed_ = false;
  Note(result);
  return result;
}
void PresentRelayHost::Register(VkImage image, nr::Size size) { if (image) sizes_[image] = size; }
void PresentRelayHost::Barrier(VkImage image, Usage before, Usage after) {
  if (!image) {
    const auto buffer = Commands();
    if (!buffer) return;
    const VkMemoryBarrier global{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = Access(before), .dstAccessMask = Access(after)};
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &global, 0, nullptr, 0, nullptr);
    return;
  }
  BarrierWithGlobal(image, before, after, before, after);
}
void PresentRelayHost::BarrierWithGlobal(VkImage image, Usage before, Usage after, Usage global_before, Usage global_after) {
  const auto buffer = Commands();
  if (!buffer) return;
  const VkMemoryBarrier global{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = Access(global_before), .dstAccessMask = Access(global_after)};
  const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = Access(before), .dstAccessMask = Access(after),
      .oldLayout = Layout(before), .newLayout = Layout(after), .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &global, 0, nullptr, 1, &barrier);
}
void PresentRelayHost::Copy(VkImage source, VkImage destination) {
  const auto from = sizes_.find(source), to = sizes_.find(destination);
  if (from == sizes_.end() && to == sizes_.end()) { failure_ = VK_ERROR_INITIALIZATION_FAILED; return; }
  const auto size = from != sizes_.end() ? from->second : to->second;
  if (from != sizes_.end() && to != sizes_.end() && from->second != to->second) { failure_ = VK_ERROR_INITIALIZATION_FAILED; return; }
  const auto buffer = Commands();
  if (!buffer) return;
  const VkImageCopy copy{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .extent = {size.width, size.height, 1}};
  functions_.vkCmdCopyImage(buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}
bool PresentRelayHost::Signal(VkSemaphore timeline, uint64_t value) {
  if (!Flush()) return false;
  const VkTimelineSemaphoreSubmitInfo values{.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value};
  const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values, .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
  return Note(functions_.vkQueueSubmit(queue_, 1u, &submit, VK_NULL_HANDLE));
}
bool PresentRelayHost::Wait(VkSemaphore timeline, uint64_t value) {
  if (!Valid()) return false;
  const VkTimelineSemaphoreSubmitInfo values{.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value};
  const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values, .waitSemaphoreCount = 1, .pWaitSemaphores = &timeline, .pWaitDstStageMask = &stage};
  return Note(functions_.vkQueueSubmit(queue_, 1u, &submit, VK_NULL_HANDLE));
}
bool PresentRelayHost::FlushWithFence(VkFence fence) {
  if (!Flush()) return false;
  return Note(functions_.vkQueueSubmit(queue_, 0u, nullptr, fence));
}
void PresentRelayHost::DestroyImage(VkImage image) { sizes_.erase(image); functions_.vkDestroyImage(device_, image, nullptr); }
void PresentRelayHost::Destroy() {
  if (!ring_) return;
  Flush();
  if (ring_->WaitIdle(std::chrono::seconds(2)) || failure_ == VK_ERROR_DEVICE_LOST) ring_->Destroy();
  ring_.reset();
}
}  // namespace uplift::vk
