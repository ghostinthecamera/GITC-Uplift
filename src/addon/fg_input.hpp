#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace uplift::addon {

// HUD-less (design §1): which backbuffer images Uplift's NR changed before frame generation, and what DLSS-G's
// evaluate does with the game's HUD-less image for each. No graphics calls: images are compared by their native
// handles (ID3D12Resource* or VkImage) on their native device (ID3D12Device* or VkDevice).

struct ImageKey {
  uint64_t device = 0;
  uint64_t image = 0;
  friend bool operator==(const ImageKey&, const ImageKey&) = default;
};

enum class HudlessAction : uint8_t { LEAVE, HIDE, USE };

// N, the HUD-less image NR ran on, with what keeps it alive. `native` is what DLSS-G is given in place of the game's
// image: an ID3D12Resource* or an NVSDK_NGX_Resource_VK*.
struct Replacement {
  std::shared_ptr<const void> owner;
  void* native = nullptr;
};

// The game's images N was made from, by native handle. Opacity 0: none recorded.
struct HudInputs {
  uint64_t hudless = 0;
  uint64_t opacity = 0;
};

struct FgDecision {
  HudlessAction action = HudlessAction::LEAVE;
  Replacement replacement;
  std::string_view reason;
};

class FgInput {
 public:
  // A replacement lives at most this many counted Presents: long enough for DLSS-G to evaluate the last frame on its own present thread
  // after the game's next Present began (Vulkan), short enough that no N stays pinned by an image never presented again.
  static constexpr uint64_t REPLACEMENT_PRESENTS = 2u;
  // Every game-facing Present drops its image's replacement before NR can note a new one (design §1). Each counts one
  // Present on the retirement clock, unless `counts` is false: ReShade's own Present drops first on the early route, and the
  // Streamline callback for that same Present counts it (CountPresent), so each Present is counted once.
  void Drop(ImageKey image, bool counts = true) noexcept;
  void DropDevice(uint64_t device) noexcept;     // an image that cannot be named
  void DropAll(bool counts = true) noexcept;     // a Vulkan present that cannot be named
  void CountPresent() noexcept;
  // A Present-stage recording completed at least one pass into this image.
  void Note(ImageKey image, std::chrono::steady_clock::time_point now, Replacement replacement = {}, HudInputs inputs = {});
  // At DLSS-G's evaluate: `backbuffer` is DLSSG.Backbuffer, `game_hudless` and `game_opacity` the block's images.
  FgDecision Decide(ImageKey backbuffer, uint64_t game_hudless, uint64_t game_opacity, bool hide, std::chrono::steady_clock::time_point now);
  // DLSS-G was seen reading an image Uplift's NR changed within the limit.
  bool FrameGenerationReads(std::chrono::steady_clock::time_point now) const noexcept;
  // Any image noted within the limit: the evaluate's first, lock-free test.
  bool AnyRecent(std::chrono::steady_clock::time_point now) const noexcept;
  uint64_t Presents() const noexcept;
  // The early route drops in ReShade's Present; the Streamline callback for that same Present must not drop again.
  void MarkDroppedOnThisThread(ImageKey image) noexcept;
  bool ConsumeThreadMark(ImageKey image) noexcept;
  // Details and the trace (design §9): the latest evaluate's decision, while it is within the limit, and the counts so far.
  struct LastDecision {
    bool recent = false;
    HudlessAction action = HudlessAction::LEAVE;
    std::string_view reason;
  };
  LastDecision Last(std::chrono::steady_clock::time_point now) const noexcept;
  struct Counts {
    uint64_t used = 0, hidden = 0, left = 0;
  };
  Counts Totals() const noexcept;

 private:
  FgDecision Record(FgDecision decision, std::chrono::steady_clock::time_point now) noexcept;
  struct Entry {
    ImageKey image;
    std::chrono::steady_clock::time_point time;
    Replacement replacement;
    HudInputs inputs;
    uint64_t noted_present = 0;  // presents_ when the replacement was noted
  };
  // Releases replacements REPLACEMENT_PRESENTS old into `released`, outside the lock.
  void AgeLocked(std::vector<std::shared_ptr<const void>>* released) noexcept;
  void RefreshReplacementFlagLocked() noexcept;

  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::atomic<bool> any_replacement_{false};
  std::atomic<int64_t> last_note_{0};  // steady_clock ticks; 0 = never
  std::atomic<int64_t> read_at_{0};
  std::atomic<uint64_t> presents_{0};
  std::atomic<uint8_t> last_action_{0};
  std::atomic<uint8_t> last_reason_{0};  // an index into fg_input.cpp's REASONS, so a reader never pairs two reasons' halves
  std::atomic<int64_t> last_at_{0};
  std::atomic<uint64_t> used_{0}, hidden_{0}, left_{0};
};

// "used", "hidden (reason)" or "left as it is (reason)": Details' value and the INFO line's.
std::string HudlessLine(HudlessAction action, std::string_view reason);

}  // namespace uplift::addon
