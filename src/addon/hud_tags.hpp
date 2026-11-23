#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace uplift::addon {

// HUD-less (design §2): Streamline's HUD-less and opacity tags, read only. The tag hooks forward every call unchanged
// and then report it here.

enum class TagApi : uint8_t { UNKNOWN, D3D12, VULKAN };

// Why a Present's HUD-less image can or cannot be used (design §3), in the order the conditions are tested.
enum class HudReason : uint8_t {
  USABLE,
  CORRECTION_OFF,
  NOT_READ,
  SETTLING,
  NOT_TAGGED,
  MORE_THAN_ONE_FRAME,
  NO_OPACITY,
  LIFECYCLE,
  EXTENT,
  SIZE,
  SAMPLES,
  FORMAT,
  STATE,
  USAGE,
  EFFECTS_ON,
  NO_FREE_IMAGE,  // the composer had no free N, or could not make one
};

// One tagged image. `native` is an ID3D12Resource* or a VkImage; 0 = none. The description fields are filled only
// when the resource's header is sl::Resource's, or by the Present for Direct3D 12 from the resource itself.
struct TaggedImage {
  uint64_t native = 0;
  uint32_t state = UINT32_MAX;  // D3D12_RESOURCE_STATES or VkImageLayout
  int lifecycle = 0;
  uint32_t extent_x = 0, extent_y = 0, extent_w = 0, extent_h = 0;
  bool described = false;
  uint32_t width = 0, height = 0, format = 0, mips = 0, layers = 0, samples = 0;
  uint32_t usage = 0;  // VkImageUsageFlags
  bool denies_shader_reads = false;  // D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
};

struct HudFrame {
  TaggedImage hudless;
  TaggedImage opacity;  // UI alpha, else UI colour and alpha
  bool opacity_is_ui_colour = false;
  uint32_t viewport = 0;
};

struct TakeResult {
  std::optional<HudFrame> frame;
  uint32_t waiting = 0;  // frames with a real HUD-less image
  HudReason reason = HudReason::NOT_TAGGED;
  std::shared_ptr<const void> hold;  // Direct3D 12: keeps the taken images referenced until the Present's list is submitted
};

class HudTags {
 public:
  HudTags() = default;
  HudTags(const HudTags&) = delete;
  HudTags& operator=(const HudTags&) = delete;
  ~HudTags();

  void SetApi(TagApi api) noexcept;
  // Off clears and releases: nothing is stored unless DLSS-G reads Uplift's output and UICorrection is On.
  void SetStoring(bool storing) noexcept;
  // slSetTag: no frame token. `viewport` is an sl::ViewportHandle*, `tags` an sl::ResourceTag array.
  void Record(const void* viewport, const void* tags, uint32_t count) noexcept;
  // slSetTagForFrame: the frame is identified by its token's address, never dereferenced; a null token records nothing.
  void RecordForFrame(const void* frame, const void* viewport, const void* tags, uint32_t count) noexcept;
  // A frame only when exactly one frame with a real HUD-less image is waiting. Clears everything either way.
  TakeResult Take() noexcept;
  // Streamline shutdown, a resize, a new swapchain, device loss.
  void Clear() noexcept;

  struct Counts {
    uint64_t hudless = 0, empty_hudless = 0, opacity = 0, ui_colour = 0, frames_waiting_max = 0;
  };
  Counts Totals() const noexcept;

 private:
  static constexpr size_t SLOT_HUDLESS = 0u, SLOT_ALPHA = 1u, SLOT_COLOUR = 2u, SLOTS = 3u;
  struct Frame {
    uint32_t viewport = 0;
    uintptr_t key = 0;  // the token's address; 0 for slSetTag
    uint64_t sequence = 0;
    std::array<TaggedImage, SLOTS> images = {};
  };
  void RecordLocked(uintptr_t key, const void* viewport, const void* tags, uint32_t count);
  void ReleaseLocked(TaggedImage& image) noexcept;
  void ClearLocked() noexcept;

  mutable std::mutex mutex_;
  TagApi api_ = TagApi::UNKNOWN;
  bool storing_ = false;
  uint64_t sequence_ = 0;
  std::vector<Frame> frames_;
  Counts counts_;
};

}  // namespace uplift::addon
