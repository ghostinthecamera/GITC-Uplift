#include "ngx_hooks/minhook_once.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace uplift::hooks {
namespace {

#if defined(_WIN64)
constexpr std::array<uint8_t, 2> HOTPATCH_ENTRY = {0xEBu, 0xF9u};  // jmp short -7
constexpr uint8_t JUMP_REL8 = 0xEBu;
constexpr uint8_t JUMP_REL32 = 0xE9u;
constexpr size_t JUMP_OPCODE_BYTES = 1u;
constexpr size_t JUMP_REL8_BYTES = JUMP_OPCODE_BYTES + sizeof(int8_t);
constexpr size_t JUMP_REL32_BYTES = JUMP_OPCODE_BYTES + sizeof(int32_t);
constexpr std::array<uint8_t, 6> JUMP_ABSOLUTE = {0xFFu, 0x25u, 0x00u, 0x00u, 0x00u, 0x00u};  // jmp [rip+0]: the address follows
constexpr size_t JUMP_ABSOLUTE_BYTES = JUMP_ABSOLUTE.size() + sizeof(void*);
constexpr DWORD READABLE_CODE = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
#endif

std::atomic<bool> g_held{false};

}  // namespace

MH_STATUS EnsureMinHook() {
  const MH_STATUS status = MH_Initialize();
  return (status == MH_ERROR_ALREADY_INITIALIZED ? MH_OK : status);
}

void HoldMinHook() {
  g_held.store(true, std::memory_order_release);
}

MH_STATUS ReleaseMinHook() {
  if (g_held.load(std::memory_order_acquire)) return MH_OK;
  return MH_Uninitialize();
}

#if defined(_WIN64)
ToolPatch BehindPatch(const void* entry) {
  // The memory of `size` readable code bytes at `address`; its Type is 0 when they are not all readable code of one region.
  const auto code_at = [](const uint8_t* address, size_t size) {
    MEMORY_BASIC_INFORMATION memory = {};
    const bool readable = VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT
        && (memory.Protect & READABLE_CODE) != 0u && (memory.Protect & PAGE_GUARD) == 0u
        && static_cast<const uint8_t*>(memory.BaseAddress) + memory.RegionSize >= address + size;
    return (readable ? memory : MEMORY_BASIC_INFORMATION{});
  };
  // Where the unconditional jump at `code` leads; null when no such jump, or not all of it, can be read there.
  const auto jump_target = [&code_at](const uint8_t* code) -> const uint8_t* {
    if (code_at(code, JUMP_REL8_BYTES).Type == 0u) return nullptr;
    if (*code == JUMP_REL8) return code + JUMP_REL8_BYTES + static_cast<int8_t>(code[JUMP_OPCODE_BYTES]);
    if (*code == JUMP_REL32) {
      if (code_at(code, JUMP_REL32_BYTES).Type == 0u) return nullptr;
      int32_t displacement = 0;
      std::memcpy(&displacement, code + JUMP_OPCODE_BYTES, sizeof(displacement));
      return code + JUMP_REL32_BYTES + displacement;
    }
    if (code_at(code, JUMP_ABSOLUTE_BYTES).Type == 0u || std::memcmp(code, JUMP_ABSOLUTE.data(), JUMP_ABSOLUTE.size()) != 0) return nullptr;
    const uint8_t* target = nullptr;
    std::memcpy(&target, code + JUMP_ABSOLUTE.size(), sizeof(target));
    return target;
  };
  ToolPatch patch;
  size_t passed = 0u;  // the places named in patch.way so far
  const auto pass = [&patch, &passed](const uint8_t* place) {
    if (passed < patch.way.size()) patch.way[passed++] = const_cast<uint8_t*>(place);
  };
  const auto* const bytes = static_cast<const uint8_t*>(entry);
  if (bytes == nullptr || code_at(bytes, HOTPATCH_ENTRY.size()).Type == 0u) return patch;
  if (*bytes == JUMP_REL32) {
    const uint8_t* const stub = jump_target(bytes);
    if (stub != nullptr && code_at(stub, JUMP_REL32_BYTES).Type == MEM_PRIVATE) {
      patch.code.front() = const_cast<uint8_t*>(stub);
      for (const uint8_t* place = stub; place != nullptr && passed < patch.way.size(); place = jump_target(place)) {
        pass(place);
      }
    }
    return patch;
  }
  const uint8_t* const above = bytes - JUMP_REL32_BYTES;
  if (std::memcmp(bytes, HOTPATCH_ENTRY.data(), HOTPATCH_ENTRY.size()) != 0 || code_at(above, JUMP_REL32_BYTES).Type == 0u || *above != JUMP_REL32) {
    return patch;
  }
  const uint8_t* code = jump_target(above);
  if (code_at(code, JUMP_ABSOLUTE.size()).Type != 0u && std::memcmp(code, JUMP_ABSOLUTE.data(), JUMP_ABSOLUTE.size()) == 0) {
    patch.relay = const_cast<uint8_t*>(code);
    pass(code);
    code = jump_target(code);
  }
  for (void*& address : patch.code) {
    if (code == nullptr) break;
    address = const_cast<uint8_t*>(code);
    pass(code);
    code = jump_target(code);
  }
  return patch;
}
#endif

}  // namespace uplift::hooks
