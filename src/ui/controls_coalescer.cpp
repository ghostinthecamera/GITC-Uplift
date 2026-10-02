#include "ui/controls_coalescer.hpp"

namespace uplift::ui {
namespace {

constexpr auto DRAG_PERIOD = std::chrono::milliseconds(100);

}  // namespace

float ControlsCoalescer::Slider::Update(float live, bool dragging, std::chrono::steady_clock::time_point now) {
  if (live == applied) return applied;
  if (dragging && last_apply && now - *last_apply < DRAG_PERIOD) return applied;
  applied = live;
  last_apply = now;
  return applied;
}

nr::Controls ControlsCoalescer::Update(const Settings& live, const DragState& drag,
                                       std::chrono::steady_clock::time_point now) {
  if (!initialized_) {
    local_structure_.applied = live.local_structure;
    local_tone_.applied = live.local_tone;
    skin_structure_.applied = live.skin_structure;
    initialized_ = true;
  }
  nr::Controls controls = ToControls(live);
  controls.local_structure = local_structure_.Update(live.local_structure, drag.local_structure, now);
  controls.local_tone = local_tone_.Update(live.local_tone, drag.local_tone, now);
  controls.skin_structure = skin_structure_.Update(live.skin_structure, drag.skin_structure, now);
  return controls;
}

std::array<std::optional<nr::Controls>, MAX_PASSES - 1u> ControlsCoalescer::LaterPasses(
    const Settings& live, bool dragging, std::chrono::steady_clock::time_point now) {
  if (dragging && later_applied_ && now - *later_applied_ < DRAG_PERIOD) return later_;
  for (size_t index = 0u; index < live.passes.size(); ++index) {
    const PassSettings& pass = live.passes[index];
    if (pass.follow_pass1) {
      later_[index].reset();
    } else {
      later_[index] = nr::Controls{
          .intensity = pass.intensity,
          .local_tone = pass.local_tone,
          .local_structure = pass.local_structure,
          .global_tone = live.global_tone,
          .auto_mask = pass.auto_mask,
          .skin_structure = pass.skin_structure,
          .style = pass.style,
      };
    }
  }
  later_applied_ = now;
  return later_;
}

}  // namespace uplift::ui
