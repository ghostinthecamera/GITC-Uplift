#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace uplift::ui {

namespace text_wrap {
// The blanks ImGui drops where it wraps a line: none ends a line, none starts the next.
constexpr std::string_view BLANKS = " \t";
// UTF-8 continuation bytes are 10xxxxxx.
constexpr unsigned char CONTINUATION_MASK = 0xC0u;
constexpr unsigned char CONTINUATION_BITS = 0x80u;
// Bytes from here up are not ASCII: a line may break on either side of such a character.
constexpr unsigned char FIRST_NON_ASCII = 0x80u;

// Where the UTF-8 character at `at` ends.
inline const char* NextCharacter(const char* at, const char* end) {
  ++at;
  while (at < end && (static_cast<unsigned char>(*at) & CONTINUATION_MASK) == CONTINUATION_BITS) ++at;
  return at;
}

inline bool IsBlank(char c) { return BLANKS.find(c) != std::string_view::npos; }
inline bool IsNonAscii(char c) { return static_cast<unsigned char>(c) >= FIRST_NON_ASCII; }
}  // namespace text_wrap

// Where the line starting at `begin` ends so that it is no wider than `width`, as `measure(begin, end)` measures text: after the
// last word that fits, or between two characters where either is not ASCII (Chinese has no spaces). A first word wider than the
// line is broken where the width runs out.
template <typename Measure>
const char* NextBreakByWidth(const char* begin, const char* end, float width, Measure measure) {
  if (measure(begin, end) <= width) return end;
  const char* fit = nullptr;
  for (const char* at = begin; at < end;) {
    const char* const next = text_wrap::NextCharacter(at, end);
    const bool word_ends = next < end && !text_wrap::IsBlank(*at) &&
                           (text_wrap::IsBlank(*next) || text_wrap::IsNonAscii(*at) || text_wrap::IsNonAscii(*next));
    if (word_ends) {
      if (measure(begin, next) > width) break;
      fit = next;
    }
    at = next;
  }
  if (fit != nullptr) return fit;
  const char* cut = text_wrap::NextCharacter(begin, end);
  while (cut < end) {
    const char* const next = text_wrap::NextCharacter(cut, end);
    if (measure(begin, next) > width) break;
    cut = next;
  }
  return cut;
}

// Breaks text into lines no wider than `width`, keeping its own line breaks. `next_break(begin, end, width)` returns where the
// line starting at `begin` ends: the overlay passes NextBreakByWidth over its font, a test a fake one.
// A line that would take nothing takes one whole character, so the text always moves on.
template <typename NextBreak>
std::string WrapLines(std::string_view text, float width, NextBreak next_break) {
  std::string wrapped;
  wrapped.reserve(text.size() + text.size() / 8u);
  size_t start = 0u;
  while (true) {
    const size_t end = text.find('\n', start);
    const std::string_view paragraph = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    const char* at = paragraph.data();
    const char* const paragraph_end = paragraph.data() + paragraph.size();
    bool first = true;
    do {
      const char* line_end = next_break(at, paragraph_end, width);
      if (line_end <= at && at < paragraph_end) line_end = text_wrap::NextCharacter(at, paragraph_end);
      std::string_view line(at, static_cast<size_t>(line_end - at));
      const size_t last = line.find_last_not_of(text_wrap::BLANKS);
      line = (last == std::string_view::npos) ? std::string_view() : line.substr(0u, last + 1u);
      if (!first) wrapped += '\n';
      wrapped += line;
      first = false;
      at = line_end;
      while (at < paragraph_end && text_wrap::BLANKS.find(*at) != std::string_view::npos) ++at;
    } while (at < paragraph_end);
    if (end == std::string_view::npos) return wrapped;
    wrapped += '\n';
    start = end + 1u;
  }
}

}  // namespace uplift::ui
