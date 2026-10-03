#pragma once

#include <cstdint>

namespace uplift::addon {

// Plan 17 (1.0.1 design §3): the 2-strike rule for the private-device NR of one game device in one session (a bridge's private Direct3D 12 device, or the
// 64-bit helper's). Its first stop (the device removed or hung, or the CPU-ordered timeouts) offers Retry now, which builds everything afresh; the second
// is final until the game restarts. A stop that cannot be retried at all (a bridge on the adapter's shared device, without a device factory: no new device
// can be made while the abandoned NR runtime holds the removed one) is final at once. Pure, so the unit tests drive it.
class BridgeStrikes {
 public:
  static constexpr uint32_t LIMIT = 2u;

  // At every present: whether the current bridge (or helper) has stopped, and whether a retry could replace what stopped. Counts each one's stop once;
  // true when this call counted a new one.
  bool Note(bool stopped, bool retryable) {
    if (!stopped || counted_) return false;
    counted_ = true;
    ++stops_;
    unretryable_ = !retryable;
    return true;
  }
  // Retry now built (or is about to build) a fresh one: its own stop counts again. Refused (false) once the limit is reached, after a stop that cannot be
  // retried, or while nothing stopped.
  bool Retried() {
    if (!RetryAllowed()) return false;
    counted_ = false;
    return true;
  }
  [[nodiscard]] bool Stopped() const { return counted_; }  // the current one stopped and no retry has started
  [[nodiscard]] bool RetryAllowed() const { return counted_ && stops_ < LIMIT && !unretryable_; }
  [[nodiscard]] bool Exhausted() const { return stops_ >= LIMIT; }  // stopped twice
  [[nodiscard]] bool Unretryable() const { return unretryable_; }  // the stop counted cannot be retried: final at once
  [[nodiscard]] uint32_t Stops() const { return stops_; }

 private:
  uint32_t stops_ = 0u;
  bool counted_ = false;      // the current bridge's stop is counted
  bool unretryable_ = false;  // that stop cannot be retried
};

}  // namespace uplift::addon
