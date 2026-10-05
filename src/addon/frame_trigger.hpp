#pragma once

#include <cstdint>
#include <string_view>

namespace uplift::addon {

enum class TriggerPoint : uint8_t {
  NONE,
  PRESENT,        // before ReShade's effects: their begin-effects event, or the present event (1.1.6, FrameTrigger::OnPresent)
  MARKER,         // the reshade_render_technique event of the Uplift technique
  AFTER_EFFECTS,  // reshade_finish_effects: the technique was expected but did not render
};

[[nodiscard]] std::string_view TriggerPointName(TriggerPoint point);

// Spec §8.1 (amendment 7): picks the one event per frame in which NR runs. The add-on calls it
// only for the primary swap chain, and only for events recorded on ReShade's immediate list.
class FrameTrigger {
 public:
  // First event of a frame. `marker_expected`: effects are on and an enabled Uplift technique exists. `effects_on`: ReShade's effects are on.
  // 1.1.6: with effects on, NR before effects waits for the begin-effects event, which comes after every add-on's present event (RenoDX draws its
  // swap chain proxy over the back buffer in its own, so NR's result in the present event was painted over). Only when the last frame had a usable
  // one: while effects load, or render on a game's list, none comes, and NR stays in the present event (one frame goes without NR when that starts).
  TriggerPoint OnPresent(bool marker_expected, bool effects_on = false);
  // reshade_begin_effects; `usable` when NR's result there reaches the screen (ReShade renders into the back buffer, or OpenGL's own copy of it).
  TriggerPoint OnBeginEffects(bool usable);
  // reshade_render_technique; `is_marker` when the technique is the Uplift technique.
  TriggerPoint OnTechnique(bool is_marker);
  // reshade_finish_effects.
  TriggerPoint OnFinishEffects();
  // True after a marker frame in which neither effect event came (another add-on rendered the
  // effects on a game command list, for example). NR then runs before effects until the marker
  // disappears, so it can never be skipped for good.
  [[nodiscard]] bool MarkerMissed() const { return marker_missed_; }

 private:
  // PRESENT now, or, when `deferred`, at this frame's begin-effects event.
  TriggerPoint BeforeEffects(bool deferred);

  bool waiting_ = false;
  bool waiting_begin_ = false;  // NR before effects waits for this frame's begin-effects event
  bool begin_seen_ = false;     // a usable begin-effects event came since the last present
  bool marker_missed_ = false;
};

}  // namespace uplift::addon
