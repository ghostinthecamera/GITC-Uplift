#include "addon/before_fg_report.hpp"

#include <utility>

namespace uplift::addon {
namespace {

// FgInput keys images by their native handle alone (see NgxBridge::DecideHudless).
constexpr uint64_t ANY_DEVICE = 0u;

}  // namespace

void ReportPresentPasses(ui::OverlayView::Activity& activity, FgInput& input, uint64_t back_buffer, uint32_t passes,
                         std::chrono::steady_clock::time_point now, Replacement replacement, HudInputs inputs) {
  if (passes == 0u) return;
  activity.passes += passes;
  if (back_buffer != UNNAMED_IMAGE) input.Note({.device = ANY_DEVICE, .image = back_buffer}, now, std::move(replacement), inputs);
}

void DropPresented(FgInput& input, uint64_t back_buffer, bool counts) noexcept {
  if (back_buffer == UNNAMED_IMAGE) {
    input.DropAll(counts);
  } else {
    input.Drop({.device = ANY_DEVICE, .image = back_buffer}, counts);
  }
}

}  // namespace uplift::addon
