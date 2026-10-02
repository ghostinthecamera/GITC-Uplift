#pragma once

#include <cstdint>
#include <string_view>

namespace uplift::addon {

enum class TriggerPoint : uint8_t {
  NONE,
  PRESENT,        // the present event, before ReShade's effects
  MARKER,         // the reshade_render_technique event of the Uplift technique
  AFTER_EFFECTS,  // reshade_finish_effects: the technique was expected but did not render
};

[[nodiscard]] std::string_view TriggerPointName(TriggerPoint point);

// Spec §8.1 (amendment 7): picks the one event per frame in which NR runs. The add-on calls it
// only for the primary swap chain, and only for events recorded on ReShade's immediate list.
class FrameTrigger {
 public:
  // First event of a frame. `marker_expected`: effects are on and an enabled Uplift technique exists.
  TriggerPoint OnPresent(bool marker_expected);
  // reshade_render_technique; `is_marker` when the technique is the Uplift technique.
  TriggerPoint OnTechnique(bool is_marker);
  // reshade_finish_effects.
  TriggerPoint OnFinishEffects();
  // True after a marker frame in which neither effect event came (another add-on rendered the
  // effects on a game command list, for example). NR then runs before effects until the marker
  // disappears, so it can never be skipped for good.
  [[nodiscard]] bool MarkerMissed() const { return marker_missed_; }

 private:
  bool waiting_ = false;
  bool marker_missed_ = false;
};

}  // namespace uplift::addon
