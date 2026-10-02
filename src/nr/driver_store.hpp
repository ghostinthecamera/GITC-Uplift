#pragma once

#include <filesystem>

namespace uplift::nr {

// Resolves the NVIDIA NGX core DLL to force-load when no game has loaded one.
// Order: `override_path` if it names a file; the folder of the loaded
// `nvwgf2umx.dll` (the active D3D12 driver); the newest
// DriverStore\FileRepository\nv_dispi.inf_*\ entry. Within a folder,
// `_nvngx.dll` wins over `nvngx.dll`. Returns empty when nothing is found.
[[nodiscard]] std::filesystem::path FindNgxCore(const std::filesystem::path& override_path);

}  // namespace uplift::nr
