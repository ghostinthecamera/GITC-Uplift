#pragma once

// Plan 9: the Session's public state, apart from Session itself, so the 32-bit add-on's ui/ code compiles without the
// NGX headers session.hpp pulls in. Moved out of session.hpp unchanged.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "nr/types.hpp"

namespace uplift::nr {

enum class SessionState : uint8_t {
  OFF,
  LOADING,
  ACTIVE,
  GRACE,
  DRAINING,
  FAILED,
};

struct SessionStatus {
  SessionState state = SessionState::OFF;
  uint32_t passes_requested = 1u;
  uint32_t passes_live = 0u;
  Size capacity;
  std::optional<uint64_t> runtime_bytes;
  uint64_t bytes_per_megapixel = 0u;  // the budget's calibrated per-megapixel cost (k)
  std::string message;
  bool suspended = false;
  // Config grace minus elapsed, floored at 0; filled only while `state == GRACE`, else zero.
  std::chrono::milliseconds grace_remaining{0};
  // Plan 6 (D11): while FAILED with AutoRetry on and attempts left, the time to the next automatic retry.
  std::optional<std::chrono::milliseconds> retry_in;
  bool retries_exhausted = false;  // FAILED after the last automatic retry: waiting for a change or Retry now
};

// Upper-case state name for logs and the overlay.
[[nodiscard]] inline std::string_view SessionStateName(SessionState state) {
  switch (state) {
    case SessionState::OFF:      return "OFF";
    case SessionState::LOADING:  return "LOADING";
    case SessionState::ACTIVE:   return "ACTIVE";
    case SessionState::GRACE:    return "GRACE";
    case SessionState::DRAINING: return "DRAINING";
    case SessionState::FAILED:   return "FAILED";
  }
  return "OFF";
}

}  // namespace uplift::nr
