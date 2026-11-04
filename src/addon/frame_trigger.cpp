#include "addon/frame_trigger.hpp"

#include <utility>

#include "nr/log.hpp"

namespace uplift::addon {

std::string_view TriggerPointName(TriggerPoint point) {
  switch (point) {
    case TriggerPoint::NONE:          return "none";
    case TriggerPoint::PRESENT:       return "before effects";
    case TriggerPoint::MARKER:        return "Uplift technique";
    case TriggerPoint::EARLY_MARKER:  return "Present (Uplift technique rendered earlier)";
  }
  return "none";
}

TriggerPoint FrameTrigger::OnPresent(bool marker_expected, bool effects_on, bool marker_rendered_early) {
  const bool was_waiting = std::exchange(waiting_, marker_expected);
  frame_open_ = true;
  const bool defer = std::exchange(begin_seen_, false) && effects_on;
  waiting_begin_ = false;
  if (!marker_expected) {
    marker_missed_ = false;
    return BeforeEffects(defer);
  }
  if (marker_rendered_early) {
    waiting_ = false;
    marker_missed_ = false;
    return TriggerPoint::EARLY_MARKER;
  }
  if (was_waiting && !marker_missed_) {
    marker_missed_ = true;
    nr::Log(nr::LogLevel::WARN, "the Uplift technique did not render on ReShade's command list; NR runs before effects until the technique returns");
  }
  return marker_missed_ ? BeforeEffects(defer) : TriggerPoint::NONE;
}

TriggerPoint FrameTrigger::OnBeginEffects(bool usable) {
  if (!frame_open_) return TriggerPoint::NONE;
  begin_seen_ = usable;
  if (!waiting_begin_) return TriggerPoint::NONE;
  waiting_begin_ = false;
  return (usable ? TriggerPoint::PRESENT : TriggerPoint::NONE);
}

TriggerPoint FrameTrigger::BeforeEffects(bool deferred) {
  waiting_begin_ = deferred;
  return (deferred ? TriggerPoint::NONE : TriggerPoint::PRESENT);
}

TriggerPoint FrameTrigger::OnTechnique(bool is_marker) {
  if (!frame_open_ || !waiting_ || !is_marker) return TriggerPoint::NONE;
  waiting_ = false;
  // A returning marker rearms the next frame. This frame already selected the before-effects route.
  return std::exchange(marker_missed_, false) ? TriggerPoint::NONE : TriggerPoint::MARKER;
}

}  // namespace uplift::addon
