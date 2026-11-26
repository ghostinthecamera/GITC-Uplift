#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace uplift::nr {

// The user-mode driver modules whose folder holds the running driver's NGX core.
inline constexpr wchar_t D3D_DRIVER_MODULE[] = L"nvwgf2umx.dll";    // loaded by a Direct3D game
inline constexpr wchar_t VULKAN_DRIVER_MODULE[] = L"nvoglv64.dll";  // loaded by a Vulkan or OpenGL game

// Resolves the NVIDIA NGX core DLL to force-load when no game has loaded one. Order:
//   1. `override_path` if it names a file;
//   2. the folder of the loaded D3D_DRIVER_MODULE;
//   3. the newest core in DriverStore\FileRepository\nv_dispi.inf_*\ (NVIDIA's generic package);
//   4. the folder of the loaded VULKAN_DRIVER_MODULE;
//   5. the newest core in any other NVIDIA package folder there (a laptop or OEM package has another name).
// Rungs 4 and 5 are reached only where 2 and 3 find nothing: a store that holds a generic package always resolves at 2 or 3, even when
// another package is newer or is the one running. Within a folder, `_nvngx.dll` wins over `nvngx.dll`. Returns empty when nothing is found.
[[nodiscard]] std::filesystem::path FindNgxCore(const std::filesystem::path& override_path);

// The folder a driver module was loaded from; empty when it is not loaded.
using DriverFolderLookup = std::function<std::filesystem::path(const wchar_t* module_name)>;

// FindNgxCore's ladder on a driver store and a module lookup of the caller's, so a test can give it its own. FindNgxCore passes the system's
// FileRepository and the process's loaded modules.
[[nodiscard]] std::filesystem::path FindNgxCoreIn(const std::filesystem::path& override_path, const std::filesystem::path& repository,
                                                  const DriverFolderLookup& driver_folder);

// Which of a driver store's package folders NewestStoreCore looks in.
enum class StorePackages : uint8_t {
  GENERIC,       // nv_dispi.inf_*, in exactly that case
  OTHER_NVIDIA,  // every other folder named nv*.inf_*, in any case
};

// The NGX core with the latest write time among `repository`'s package folders of one kind; empty when none holds a core. The ladder's rungs
// 3 and 5.
[[nodiscard]] std::filesystem::path NewestStoreCore(const std::filesystem::path& repository, StorePackages packages);

}  // namespace uplift::nr
