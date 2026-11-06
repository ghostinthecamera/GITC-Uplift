#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "addon/before_fg_decision.hpp"
#include "addon/helper_front.hpp"
#include "ui/overlay.hpp"

namespace uplift::addon {

// A present's settings and facts, kept for the Before FG present that records them later (the outer present, or the native context's prelude).
struct BeforeFgFrame {
  FrameSettings settings;
  FrameFacts facts;
};

// Evidence and pending work for the current primary swapchain's Before FG handoff.
struct BeforeFgDevice {
  std::optional<BeforeFgFrame> outer_config;
  std::optional<BeforeFgFrame> native_config;
  std::optional<HelperFrame> helper_frame;
  const void* outer_inner = nullptr;
  IDXGISwapChain* early_dxgi = nullptr;
  VkSwapchainKHR early_vk_chain = VK_NULL_HANDLE;
  VkQueue early_vk_queue = VK_NULL_HANDLE;
  DWORD early_thread = 0u;
  bool early_confirmed = false;
  bool outer_holds_nr = false;
  bool outer_owns_frame = false;
  streamline::VkForward* outer_tick = nullptr;  // Borrowed only while the observed outer call is forwarding.
  uint32_t outer_present_passes = 0u;
  ui::OverlayView::Activity activity;
  text::BeforeFgMessage note;

  bool TakeOuterTick() {
    auto* const forward = std::exchange(outer_tick, nullptr);
    return forward && forward->TakeNativeTick();
  }

  void Reset() { *this = {}; }
};

void ClearBeforeFgHandoff(BeforeFgDevice& device);
// The sidecar snapshots the original stage policy before the inner frame is marked selected.
void PrepareBeforeFgNative(BeforeFgDevice& device, const FrameSettings& settings, FrameFacts& facts, bool active, bool holds_nr);
void ClearBeforeFgNativePrelude(BeforeFgDevice& device);
// At resolved bridge-route entry: discard the prior handoff even if this Present never reaches the bridge block.
void PrepareBeforeFgBridge(BeforeFgDevice& device, bool active);
void SelectBeforeFg(FrameFacts& facts, bool active);
void ApplyBeforeFgUnsupported(BeforeFgDevice& device, FrameFacts& facts, const BeforeFgDecision& decision);
void ApplyBeforeFgNative(BeforeFgDevice& device, FrameSettings& settings, FrameFacts& facts, const BeforeFgDecision& decision,
                         bool outer_matches_primary, bool holds_nr, bool& vk_native_present);
// PrepareBeforeFgBridge has already discarded the prior Present's handoff; this applies only the new decision.
void ApplyBeforeFgBridge(BeforeFgDevice& device, FrameSettings& settings, FrameFacts& facts, const BeforeFgDecision& decision);
void ApplyBeforeFgDx12(BeforeFgDevice& device, FrameSettings& settings, FrameFacts& facts, const BeforeFgDecision& decision, bool outer_matches_primary);
void ApplyBeforeFgHelper(BeforeFgDevice& device, HelperFrame& frame, const BeforeFgDecision& decision);
void DrainBeforeFgHelper(BeforeFgDevice& device, bool early_active);

}  // namespace uplift::addon
