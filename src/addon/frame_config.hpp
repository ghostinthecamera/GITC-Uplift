#pragma once

#include <chrono>
#include <cstdint>

#include "addon/device_context.hpp"
#include "ui/controls_coalescer.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// Plan 9: the FrameConfig one present runs with, for the 64-bit add-on and the helper alike (was OnPresent's).
struct FrameConfigInputs {
  const ui::Settings* settings = nullptr;
  ui::DragState drag;
  bool defaults_view = false;
  uint32_t ngx_frame_generation = 0u;
  bool nr_allowed = true;
  uint64_t settings_generation = 0u;
};

[[nodiscard]] FrameConfig BuildFrameConfig(const FrameConfigInputs& inputs, ui::ControlsCoalescer* coalescer,
                                           std::chrono::steady_clock::time_point now);

}  // namespace uplift::addon
