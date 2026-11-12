#include "nr/process_modules.hpp"

#include <psapi.h>

#include <cstddef>

namespace uplift::nr {
namespace {

// Starting sizes for the process module list and one module's path.
constexpr size_t INITIAL_MODULE_CAPACITY = 512u;
constexpr size_t MODULE_PATH_CAPACITY = 32768u;

}  // namespace

std::vector<ProcessModule> EnumerateProcessModules() {
  const HANDLE process = GetCurrentProcess();
  std::vector<HMODULE> modules(INITIAL_MODULE_CAPACITY);
  while (true) {
    DWORD needed_bytes = 0u;
    if (K32EnumProcessModules(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed_bytes) == FALSE) {
      return {};
    }
    const size_t count = needed_bytes / sizeof(HMODULE);
    const bool complete = (count <= modules.size());
    modules.resize(count);
    if (complete) break;
  }
  std::vector<ProcessModule> result;
  std::wstring path(MODULE_PATH_CAPACITY, L'\0');
  for (const HMODULE module : modules) {
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0u) continue;
    result.push_back({.module = module, .path = path.substr(0u, length)});
  }
  return result;
}

}  // namespace uplift::nr
