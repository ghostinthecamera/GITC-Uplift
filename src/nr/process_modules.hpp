#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace uplift::nr {

// One module of this process: its handle and full path.
struct ProcessModule {
  HMODULE module = nullptr;
  std::wstring path;
};

// Every module mapped in this process, with its full path; empty when the list cannot be read.
[[nodiscard]] std::vector<ProcessModule> EnumerateProcessModules();

}  // namespace uplift::nr
