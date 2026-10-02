#include "ipc/control_block.hpp"

#include <Windows.h>

#include <algorithm>
#include <format>

namespace uplift::ipc {
namespace {

// The ring's two counters as interlocked values: read with a compare-exchange that changes nothing.
uint32_t Load(volatile uint32_t* counter) {
  return static_cast<uint32_t>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(counter), 0, 0));
}

}  // namespace

void CopyTextBytes(char* out, size_t capacity, std::string_view text) {
  if (capacity == 0u) return;
  size_t length = std::min(text.size(), capacity - 1u);
  // A cut inside a multi-byte character would leave a broken one: back up to where the character starts.
  while (length > 0u && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0u) == 0x80u) {
    --length;
  }
  std::memcpy(out, text.data(), length);
  out[length] = '\0';
}

void LogWrite(LogRing* ring, std::string_view line) {
  const size_t size = std::min(line.size() + 1u, LOG_BYTES / 2u);  // the text and its newline
  char record[LOG_BYTES / 2u];
  for (size_t index = 0u; index + 1u < size; ++index) {
    record[index] = (line[index] == '\n' || line[index] == '\r' ? ' ' : line[index]);
  }
  record[size - 1u] = '\n';
  const uint32_t written = ring->written;  // only this thread writes it
  for (;;) {
    const uint32_t read = Load(&ring->read);
    if (LOG_BYTES - (written - read) >= size) break;
    // No room: drop the oldest line (everything, when none of its newline is left in the ring).
    uint32_t next = read;
    while (next != written && ring->bytes[next % LOG_BYTES] != '\n') {
      ++next;
    }
    if (next != written) ++next;
    InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&ring->read), static_cast<LONG>(next), static_cast<LONG>(read));
  }
  for (size_t index = 0u; index < size; ++index) {
    ring->bytes[(written + index) % LOG_BYTES] = record[index];
  }
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&ring->written), static_cast<LONG>(written + size));
}

void LogDrain(LogRing* ring, const std::function<void(std::string_view line)>& sink) {
  std::string line;
  for (;;) {
    const uint32_t read = Load(&ring->read);
    const uint32_t written = Load(&ring->written);
    if (read == written) return;
    uint32_t end = read;
    while (end != written && ring->bytes[end % LOG_BYTES] != '\n') {
      ++end;
    }
    if (end == written) return;  // no complete line yet
    line.clear();
    for (uint32_t index = read; index != end; ++index) {
      line.push_back(ring->bytes[index % LOG_BYTES]);
    }
    // The line counts only when the writer did not drop it (and overwrite it) meanwhile.
    if (static_cast<uint32_t>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&ring->read), static_cast<LONG>(end + 1u),
                                                         static_cast<LONG>(read)))
        == read) {
      sink(line);
    }
  }
}

bool HeaderMatches(const ControlBlock& block, std::string_view build_id, std::string* why) {
  const std::string_view theirs = TextOf(block.build_id);
  if (block.magic == MAGIC && block.protocol == PROTOCOL_VERSION && block.block_bytes == sizeof(ControlBlock)
      && theirs == build_id) {
    return true;
  }
  *why = std::format(
      "gitc-uplift-helper64.exe (build {}) does not match the Uplift add-on next to it (build {}): copy both files from one Uplift build",
      build_id, (block.magic == MAGIC ? theirs : std::string_view("unknown")));
  return false;
}

}  // namespace uplift::ipc
