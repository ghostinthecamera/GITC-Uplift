#include "addon/launchpad_link.hpp"

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
  settling_ = true;
  return frame.wanted;
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
