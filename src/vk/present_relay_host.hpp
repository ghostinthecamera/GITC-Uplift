#pragma once

#include <span>
#include <unordered_map>
#include "vk/command_ring.hpp"
#include "vk/game_host.hpp"
#include "nr/types.hpp"

namespace uplift::vk {

// Uses private command buffers on the application's graphics queue. Bridge/helper processing remains in its existing owner.
class PresentRelayHost final : public GameHost {
 public:
  static std::unique_ptr<PresentRelayHost> Create(const NrFunctions& functions, VkDevice device, VkQueue queue, uint32_t family, std::string* error);
  bool BeginFrame(std::span<const VkSemaphore> waits);
  VkResult FinishFrame();
  void Register(VkImage image, nr::Size size);
  void Destroy();
  bool Valid() const override { return failure_ == VK_SUCCESS; }
  void Barrier(VkImage image, Usage before, Usage after) override;
  void BarrierWithGlobal(VkImage image, Usage before, Usage after, Usage global_before, Usage global_after) override;
  void Copy(VkImage source, VkImage destination) override;
  bool Signal(VkSemaphore timeline, uint64_t value) override;
  bool Wait(VkSemaphore timeline, uint64_t value) override;
  bool FlushWithFence(VkFence fence) override;
  void DestroyImage(VkImage image) override;
 private:
  PresentRelayHost(const NrFunctions& functions, VkDevice device, VkQueue queue) : functions_(functions), device_(device), queue_(queue) {}
  VkCommandBuffer Commands();
  bool Flush();
  bool Note(VkResult result);
  NrFunctions functions_;
  VkDevice device_;
  VkQueue queue_;
  std::unique_ptr<CommandRing> ring_;
  VkCommandBuffer commands_{};
  VkResult failure_ = VK_SUCCESS;
  bool consumed_ = false;
  std::vector<VkSemaphore> waits_;
  std::unordered_map<VkImage, nr::Size> sizes_;
};

}  // namespace uplift::vk
