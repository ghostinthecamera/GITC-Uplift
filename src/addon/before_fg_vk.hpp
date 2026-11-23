#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <vector>

#include "addon/fg_input.hpp"
#include "addon/hud_frame.hpp"
#include "addon/hud_tags.hpp"
#include "color/hud_composer_vk.hpp"
#include "addon/streamline_present.hpp"
#include "addon/vk_dlss_context.hpp"
#include "vk/present_relay_host.hpp"

namespace uplift::addon {

// Accessed under the add-on state lock. The recording ring owns and retires the internal relay semaphore.
class BeforeFgVk {
 public:
  void Created(VkDevice device, VkSwapchainKHR chain, const VkSwapchainCreateInfoKHR& info, void* inner);
  void Images(VkDevice device, VkSwapchainKHR chain, uint32_t count, const VkImage* images);
  // HUD-less (design §1): the image `chain` presents at `index`, as a native handle; 0 when the chain or index is unknown.
  [[nodiscard]] uint64_t ImageOf(VkSwapchainKHR chain, uint32_t index) const;
  void Acquired(VkDevice device, VkSwapchainKHR chain, uint32_t index);
  void Destroyed(VkDevice device, VkSwapchainKHR chain);
  void DestroyDevice(VkDevice device);
  struct Binding { VkDevice device{}; void* inner = nullptr; };
  Binding Find(VkSwapchainKHR chain) const;
  // `image`: the presented VkImage NR recorded for (HUD-less: the image noted for DLSS-G's evaluate); 0 when none.
  struct Result { uint32_t passes = 0; std::string_view reason; uint64_t image = 0; };
  // HUD-less (design §5): what Prepare needs to use the game's HUD-less image. Null or not wanted: nothing is taken or made.
  struct HudPresentVk {
    HudTags* tags = nullptr;
    FgInput* input = nullptr;
    HudSettle* settle = nullptr;
    bool use_wanted = false;
    std::chrono::steady_clock::time_point now;
  };
  struct HudOutcome {
    bool wanted = false;
    HudReason reason = HudReason::CORRECTION_OFF;
    Replacement replacement;  // N, for the back buffer's note, when NR ran into it and the pass was submitted
    HudInputs inputs;
  };
  struct Frame {
    Binding binding;
    vk::ImageInfo image;
    vk::ImageInfo record_image;  // what the route records into: N when the game's HUD-less image is used, else `image`
    color::ColorSpace space{};
    uint32_t family = 0;
    std::string_view reason;
  };
  Frame Prepare(VkQueue queue, const VkPresentInfoKHR& app, color::Encoding encoding, const HudPresentVk* hud = nullptr);
  // Always after the route that used a Prepare with `hud`: puts the HUD back when `applied` (NR recorded passes into N and the present's
  // forwarding is intact) and leaves N where DLSS-G reads it.
  // `forwarding_failed`: the relay may have consumed the present's semaphores without signalling them again, so the composer's last
  // submission waits on none of them.
  HudOutcome FinishHud(bool applied, bool forwarding_failed = false);
  // No longer wanted (UICorrection left On, or DLSS-G stopped reading): every composer's ring retires.
  void ReleaseHud(uint64_t present_number);
  // The reason is owned by this device's queue entry, until its next attempt or destruction.
  struct HostResult { vk::PresentRelayHost* host = nullptr; std::string_view reason; };
  HostResult BridgeHost(VkQueue queue, const Frame& frame);
  Result Record(VkQueue queue, void* queue_identity, const VkPresentInfoKHR& app, VkDlssContext& context,
                const FrameSettings& settings, const FrameFacts& facts, bool holds_nr, bool& outer_ticked, std::chrono::steady_clock::time_point now,
                const HudPresentVk* hud = nullptr);
 private:
  struct Image { VkImage resource{}; bool acquired = false; };
  struct Chain {
    Binding binding;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;  // HUD-less: the swapchain's own, sRGB kept
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
  // HUD-less: each device's composer (null after a failed start, not retried), freed in DestroyDevice; the frame in progress.
  color::HudComposerVk* ComposerFor(const Chain& chain);
  void BeginHud(const HudPresentVk& hud, VkQueue queue, const VkPresentInfoKHR& app, const Chain& chain, TakeResult taken, Frame* frame);
  std::unordered_map<VkDevice, std::unique_ptr<color::HudComposerVk>> composers_;
  struct HudFrame {
    bool wanted = false;
    HudReason reason = HudReason::CORRECTION_OFF;
    color::HudComposerVk* composer = nullptr;  // non-null: begun, FinishHud owes EndAndClose
    VkQueue queue = VK_NULL_HANDLE;
    std::vector<VkSemaphore> waits;
    HudInputs inputs;
    const NVSDK_NGX_Resource_VK* ngx = nullptr;
  };
  HudFrame hud_;
};

}  // namespace uplift::addon
