#pragma once

#include <cstdint>
#include <string_view>

#include "addon/hud_tags.hpp"

namespace uplift::addon {

// HUD-less (design §3): can this Present's HUD-less image be used. Pure: the Present fills the facts.

// Use starts only after this many usable Presents in a row (design §3, condition 2). Checked against the owner's
// Witcher 3 trace before release.
inline constexpr uint32_t HUD_SETTLE_PRESENTS = 8u;

struct HudFrameFacts {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;  // the backbuffer's DXGI_FORMAT or VkFormat
  TagApi api = TagApi::UNKNOWN;
  bool early = false;               // ReShade sits before frame generation
  bool effects_on = false;          // frame_facts.effects_on, read before BeginFrame
  bool technique_expected = false;  // an Uplift technique is expected this frame
};

// USABLE when every condition holds, else the first that fails. `use_wanted`: UICorrection On.
// `frame_generation_reads`: FgInput::FrameGenerationReads.
[[nodiscard]] HudReason DecideHudFrame(bool use_wanted, bool frame_generation_reads, const TakeResult& taken, const HudFrameFacts& back_buffer);

// The reason's English (text/hudless_messages.hpp), for Details and the log.
[[nodiscard]] std::string_view HudReasonText(HudReason reason);
// Logs the three formats behind a FORMAT reason, once for each new set of them: the reason alone does not say which one NR cannot use.
void NoteHudFormats(HudReason reason, const TakeResult& taken, const HudFrameFacts& back_buffer);

class HudSettle {
 public:
  // True once the last HUD_SETTLE_PRESENTS Presents, this one included, were all usable.
  bool Admit(HudReason reason) noexcept;

 private:
  uint32_t usable_ = 0;
};

}  // namespace uplift::addon
