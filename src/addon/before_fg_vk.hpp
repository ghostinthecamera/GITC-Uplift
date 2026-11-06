#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "addon/streamline_present.hpp"
#include "addon/vk_dlss_context.hpp"
#include "vk/present_relay_host.hpp"

namespace uplift::addon {

// Accessed under the add-on state lock. The recording ring owns and retires the internal relay semaphore.
class BeforeFgVk {
 public:
  void Created(VkDevice device, VkSwapchainKHR chain, const VkSwapchainCreateInfoKHR& info, void* inner);
  void Images(VkDevice device, VkSwapchainKHR chain, uint32_t count, const VkImage* images);
  void Acquired(VkDevice device, VkSwapchainKHR chain, uint32_t index);
  void Destroyed(VkDevice device, VkSwapchainKHR chain);
  void DestroyDevice(VkDevice device);
  struct Binding { VkDevice device{}; void* inner = nullptr; };
  Binding Find(VkSwapchainKHR chain) const;
  struct Result { uint32_t passes = 0; std::string_view reason; };
  struct Frame {
    Binding binding;
    vk::ImageInfo image;
    color::ColorSpace space{};
    uint32_t family = 0;
    std::string_view reason;
  };
  Frame Prepare(VkQueue queue, const VkPresentInfoKHR& app, color::Encoding encoding);
  // The reason is owned by this device's queue entry, until its next attempt or destruction.
  struct HostResult { vk::PresentRelayHost* host = nullptr; std::string_view reason; };
  HostResult BridgeHost(VkQueue queue, const Frame& frame);
  Result Record(VkQueue queue, void* queue_identity, const VkPresentInfoKHR& app, VkDlssContext& context,
                const FrameSettings& settings, const FrameFacts& facts, bool holds_nr, bool& outer_ticked, std::chrono::steady_clock::time_point now);
 private:
  struct Image { VkImage resource{}; bool acquired = false; };
  struct Chain {
    Binding binding;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    color::ColorSpace space = color::ColorSpace::UNKNOWN;
    VkImageUsageFlags usage{};
    bool simple = false;
    VkQueue verified_queue{};
    uint32_t family = 0u;
    std::vector<Image> images;
  };
  std::unordered_map<VkSwapchainKHR, Chain> chains_;
  struct QueueHost {
    VkDevice device{};
    std::unique_ptr<vk::PresentRelayHost> host;
    std::string creation_error;
    bool warned = false;
  };
  std::unordered_map<VkQueue, QueueHost> hosts_;
};

}  // namespace uplift::addon
