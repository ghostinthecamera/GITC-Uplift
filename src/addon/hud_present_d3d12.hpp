#pragma once

#include <d3d12.h>

#include <chrono>
#include <cstdint>
#include <memory>

#include "addon/fg_input.hpp"
#include "addon/hud_frame.hpp"
#include "addon/hud_tags.hpp"
#include "color/hud_composer_d3d12.hpp"

namespace uplift::addon {

// HUD-less (design §3, §5): what one Direct3D 12 Present needs to use the game's HUD-less image. Both routes (outer, and ReShade
// before frame generation at the present point) call BeginHudD3D12 once, at the point where NR's target is chosen.
struct HudPresentD3D12 {
  color::HudComposerD3D12* composer = nullptr;
  HudTags* tags = nullptr;
  FgInput* input = nullptr;
  HudSettle* settle = nullptr;
  bool use_wanted = false;          // UICorrection On
  bool early = false;               // ReShade sits before frame generation
  bool effects_on = false;          // ReShade's effects (read before BeginFrame)
  bool technique_expected = false;  // an Uplift technique is expected this frame
};

struct HudBegun {
  ID3D12Resource* n = nullptr;  // non-null: NR records into N, then the composer's End (if NR applied) and Close follow
  HudReason reason = HudReason::CORRECTION_OFF;
  HudInputs inputs;              // the game's images N was made from, for FgInput::Note
  std::shared_ptr<const void> hold;  // the taken images' references, until the Present's list is submitted
};

// Takes this Present's tags (always, so none outlive it), decides, and begins the composer on `list` when the frame is usable and
// settled. Records nothing when it returns no N.
HudBegun BeginHudD3D12(const HudPresentD3D12& present, ID3D12GraphicsCommandList* list, ID3D12Resource* back_buffer, uint64_t present_number,
                       std::chrono::steady_clock::time_point now);

}  // namespace uplift::addon
