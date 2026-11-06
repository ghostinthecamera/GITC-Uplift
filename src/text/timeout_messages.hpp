#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace uplift::text {

// A capped wait ran out this many times in a row: the operation, the cap in seconds, then the count.
inline constexpr char CONSECUTIVE_TIMEOUTS_LINE[] = "{} did not finish within {} s, {} times in a row";
// One capped wait for the game's present queue ran out: the cap in seconds.
inline constexpr char PRESENT_QUEUE_TIMEOUT_LINE[] = "the game's present queue did not finish within {} s";
// The present-queue waits that stop native NR at Present: the cap in seconds, then the count in words.
inline constexpr char PRESENT_QUEUE_STOPPED_LINE[] = "the game's present queue did not finish within {} s {} times in a row";
// Counts a message spells in words, indexed by the count; each count formatted through it is static_asserted to be inside.
inline constexpr std::array<std::string_view, 4u> COUNT_WORDS = {"zero", "one", "two", "three"};

// A cap in seconds; std::format prints a whole number of seconds without a fraction.
[[nodiscard]] inline double CapSeconds(std::chrono::milliseconds cap) { return std::chrono::duration<double>(cap).count(); }

// `count` in words; the caller static_asserts that its count is inside COUNT_WORDS.
[[nodiscard]] constexpr std::string_view CountInWords(uint32_t count) { return COUNT_WORDS[count]; }

[[nodiscard]] inline std::string ConsecutiveTimeoutsText(std::string_view what, std::chrono::milliseconds cap, uint32_t count) {
  return std::format(CONSECUTIVE_TIMEOUTS_LINE, what, CapSeconds(cap), count);
}

[[nodiscard]] inline std::string PresentQueueTimeoutText(std::chrono::milliseconds cap) {
  return std::format(PRESENT_QUEUE_TIMEOUT_LINE, CapSeconds(cap));
}

[[nodiscard]] inline std::string PresentQueueStoppedText(std::chrono::milliseconds cap, uint32_t count) {
  return std::format(PRESENT_QUEUE_STOPPED_LINE, CapSeconds(cap), CountInWords(count));
}

}  // namespace uplift::text
