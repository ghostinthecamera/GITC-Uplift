#include "addon/launchpad_link.hpp"

#include <format>
#include <utility>

namespace uplift::addon {

std::optional<bool> LaunchPadLink::Update(const LaunchPadLinkFrame& frame) {
  if (!frame.ready) return std::nullopt;
  const bool changed = (seen_ != frame.wanted);  // true on the first ready frame too
  seen_ = frame.wanted;
  // Whatever the link's own reload changed is taken as it is; only a later change sets again.
  if (std::exchange(settling_, false)) return std::nullopt;
  if (!changed || set_ == frame.wanted) return std::nullopt;
  set_ = frame.wanted;
  // ReShade already has it (or, without any definition, Uplift.fx's own 0): no set, so no recompile. Nothing reloads, so nothing settles. An unknown value
  // never vetoes.
  if (frame.current.has_value() && *frame.current == frame.wanted) return std::nullopt;
  settling_ = true;
  return frame.wanted;
}

UpliftMvSources ChooseUpliftMvSources(ui::MotionVectorSource setting, bool launchpad_on, bool lumenite_on) {
  switch (setting) {
    case ui::MotionVectorSource::AUTO:      return {.launchpad = launchpad_on, .lumenite = (!launchpad_on && lumenite_on)};
    case ui::MotionVectorSource::LAUNCHPAD: return {.launchpad = launchpad_on};
    case ui::MotionVectorSource::LUMENITE:  return {.lumenite = lumenite_on};
    case ui::MotionVectorSource::DLSS:
    case ui::MotionVectorSource::NONE:      break;
  }
  return {};
}

bool MotionUsesUpliftMv(ui::MotionVectorSource setting) {
  return setting == ui::MotionVectorSource::AUTO || setting == ui::MotionVectorSource::LAUNCHPAD || setting == ui::MotionVectorSource::LUMENITE;
}

UpliftMvSource CompiledUpliftMv(bool launchpad_here, bool launchpad_now, bool lumenite_here, bool lumenite_now) {
  if (launchpad_here && launchpad_now) return UpliftMvSource::LAUNCHPAD;
  if (lumenite_here && lumenite_now) return UpliftMvSource::LUMENITE;
  return UpliftMvSource::NONE;
}

bool UpliftMvValid(UpliftMvSource compiled, bool launchpad_on, bool lumenite_on) {
  switch (compiled) {
    case UpliftMvSource::LAUNCHPAD: return launchpad_on;
    case UpliftMvSource::LUMENITE:  return lumenite_on;
    case UpliftMvSource::NONE:      break;
  }
  return false;
}

std::string LumeniteLinkLine(bool value) {
  return std::format("Lumenite link: UPLIFT_USE_LUMENITE = {}; ReShade recompiles Uplift.fx, and other add-ons that keep ReShade handles across a "
                     "recompile may be affected", (value ? 1 : 0));
}

std::string LaunchPadLinkLine(bool value) {
  return std::format("LaunchPad link: UPLIFT_USE_LAUNCHPAD = {}; ReShade recompiles Uplift.fx, and other add-ons that keep ReShade handles across a "
                     "recompile may be affected", (value ? 1 : 0));
}

std::optional<bool> LaunchPadDefinition(std::optional<std::string_view> effect_value, std::optional<std::string_view> outer_value) {
  const std::optional<std::string_view> value = (effect_value ? effect_value : outer_value);  // the first scope that has one wins
  if (!value) return false;
  if (*value == "0") return false;
  if (*value == "1") return true;
  return std::nullopt;
}

bool LaunchpadReadiness::Update(bool listed, bool ready, std::chrono::steady_clock::time_point now) {
  if (listed) {
    if (ready) {
      ready_at_ = now;
    } else {
      ready_at_.reset();
    }
    return ready;
  }
  return ready_at_.has_value() && now - *ready_at_ < HOLD;
}

}  // namespace uplift::addon
