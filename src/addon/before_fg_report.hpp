#pragma once

#include <chrono>
#include <cstdint>

#include "addon/fg_input.hpp"
#include "ui/overlay.hpp"

namespace uplift::addon {

// HUD-less (design §1, §5): the one place a Present-stage recording reports. Every route calls these, so each Present drops
// its image's replacement before NR can note again, and only an image NR actually changed is noted.

// A native image handle that cannot be named (QueryInterface or GetBuffer failed, an unknown chain or index).
inline constexpr uint64_t UNNAMED_IMAGE = 0u;

// Adds the passes to Details' activity and notes `back_buffer` (the presented image, never N) when at least one pass ran.
void ReportPresentPasses(ui::OverlayView::Activity& activity, FgInput& input, uint64_t back_buffer, uint32_t passes,
                         std::chrono::steady_clock::time_point now, Replacement replacement = {}, HudInputs inputs = {});

// A game-facing Present of `back_buffer` drops its replacement; one that cannot be named drops every replacement. Any counted Present
// also drops replacements two Presents old (FgInput::REPLACEMENT_PRESENTS), so no N outlives a resize or a swapchain's images.
// `counts`: see FgInput::Drop.
void DropPresented(FgInput& input, uint64_t back_buffer, bool counts = true) noexcept;

}  // namespace uplift::addon
