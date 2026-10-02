#pragma once

#include <Windows.h>

namespace uplift::nr {

// Returns the address of `module`'s import-address-table slot for the named
// import (by-name imports only), or nullptr.
[[nodiscard]] void** FindImportAddress(HMODULE module, const char* import_name);

}  // namespace uplift::nr
