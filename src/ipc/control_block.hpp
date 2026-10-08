#pragma once

// Plan 9 (design §2.2): the small pieces around ipc::ControlBlock that both processes use -- bounded text fields, the
// helper's log ring, the header check, and the drag state's wire form.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

#include "ipc/protocol.hpp"
#include "ui/controls_coalescer.hpp"

namespace uplift::ipc {

// Copies `text` into a fixed field: cut at a UTF-8 character boundary when it does not fit, and always terminated.
void CopyTextBytes(char* out, size_t capacity, std::string_view text);

template <size_t N>
void CopyText(char (&out)[N], std::string_view text) {
  CopyTextBytes(out, N, text);
}

// The field's text: up to its first NUL, never past the array (a corrupt or unterminated field cannot run on).
template <size_t N>
[[nodiscard]] std::string_view TextOf(const char (&in)[N]) {
  const void* const end = std::memchr(in, 0, N);
  return {in, end == nullptr ? N : static_cast<size_t>(static_cast<const char*>(end) - in)};
}

// The log ring: one producer (the helper), one consumer (the add-on), both interlocked on the two byte counters. A line is
// written whole and read whole; when the ring is full the writer drops the oldest lines to make room. The first byte of a
// line is the record's own (the helper's log level), the rest its message.
// A newline inside `line` becomes a space, so one line is one record; a line longer than half the ring is cut.
void LogWrite(LogRing* ring, std::string_view line);
// Every complete line written since the last call, oldest first, without its newline.
void LogDrain(LogRing* ring, const std::function<void(std::string_view line)>& sink);

// The helper's check of the block the add-on made, before it touches D3D12: the magic, the protocol version, the block's
// size and the build id all match. On a mismatch `why` reads "gitc-uplift-helper64.exe (build X) does not match the Uplift add-on next to
// it (build Y): copy both files from one Uplift build" (X is `build_id`, this side's; Y is the header's).
// Only the header's first fields are read, so a block from another protocol version is safe to check.
[[nodiscard]] bool HeaderMatches(const ControlBlock& block, std::string_view build_id, std::string* why);

// ui::DragState, bit i = its i-th field in declaration order.
inline constexpr uint32_t DRAG_FIELDS = 6u;
static_assert(sizeof(ui::DragState) == DRAG_FIELDS, "ui::DragState changed: update DragBits and DragFrom");
[[nodiscard]] inline uint32_t DragBits(const ui::DragState& drag) {
  return (drag.local_structure ? 1u : 0u) | (drag.local_tone ? 2u : 0u) | (drag.skin_structure ? 4u : 0u)
         | (drag.resolution_scale ? 8u : 0u) | (drag.pass_slider ? 16u : 0u) | (drag.face_protection ? 32u : 0u);
}
[[nodiscard]] inline ui::DragState DragFrom(uint32_t bits) {
  return {
      .local_structure = (bits & 1u) != 0u,
      .local_tone = (bits & 2u) != 0u,
      .skin_structure = (bits & 4u) != 0u,
      .resolution_scale = (bits & 8u) != 0u,
      .pass_slider = (bits & 16u) != 0u,
      .face_protection = (bits & 32u) != 0u,
  };
}

}  // namespace uplift::ipc
