#include "nr/driver_store.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <string_view>
#include <system_error>

namespace uplift::nr {
namespace {

constexpr std::array<const wchar_t*, 2> CORE_NAMES = {L"_nvngx.dll", L"nvngx.dll"};  // in the order preferred
constexpr std::wstring_view GENERIC_PACKAGE_PREFIX = L"nv_dispi.inf_";
constexpr std::wstring_view NVIDIA_PACKAGE_PREFIX = L"nv";
constexpr std::wstring_view PACKAGE_INF_MARK = L".inf_";
constexpr wchar_t DRIVER_STORE_FOLDER[] = L"DriverStore";
constexpr wchar_t FILE_REPOSITORY_FOLDER[] = L"FileRepository";
constexpr size_t MODULE_PATH_CAPACITY = 32768u;

std::filesystem::path CoreInFolder(const std::filesystem::path& folder) {
  for (const wchar_t* name : CORE_NAMES) {
    std::error_code error;
    const auto candidate = folder / name;
    if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
  }
  return {};
}

std::filesystem::path LoadedDriverFolder(const wchar_t* module_name) {
  const HMODULE driver = GetModuleHandleW(module_name);
  if (driver == nullptr) return {};
  std::array<wchar_t, MODULE_PATH_CAPACITY> path = {};
  const DWORD length = GetModuleFileNameW(driver, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0u || length >= path.size()) return {};
  return std::filesystem::path(path.data(), path.data() + length).parent_path();
}

// Whether a package folder's name is of the kind asked for. The generic name is matched case-sensitively; the others without regard to case.
bool PackageMatches(std::wstring name, StorePackages packages) {
  const bool generic = name.starts_with(GENERIC_PACKAGE_PREFIX);
  if (packages == StorePackages::GENERIC) return generic;
  std::ranges::transform(name, name.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return (!generic && name.starts_with(NVIDIA_PACKAGE_PREFIX) && name.find(PACKAGE_INF_MARK) != std::wstring::npos);
}

std::filesystem::path DriverStoreRepository() {
  std::array<wchar_t, MAX_PATH> system_directory = {};
  const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
  if (length == 0u || length >= system_directory.size()) return {};
  return std::filesystem::path(system_directory.data()) / DRIVER_STORE_FOLDER / FILE_REPOSITORY_FOLDER;
}

}  // namespace

std::filesystem::path NewestStoreCore(const std::filesystem::path& repository, StorePackages packages) {
  if (repository.empty()) return {};
  std::error_code error;
  std::filesystem::path selected;
  std::filesystem::file_time_type selected_time = {};
  for (auto it = std::filesystem::directory_iterator(repository, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::directory_iterator();
       it.increment(error)) {
    if (!PackageMatches(it->path().filename().wstring(), packages)) continue;
    const auto candidate = CoreInFolder(it->path());
    if (candidate.empty()) continue;
    std::error_code time_error;
    const auto time = std::filesystem::last_write_time(candidate, time_error);
    if (!time_error && (selected.empty() || time > selected_time)) {
      selected = candidate;
      selected_time = time;
    }
  }
  return selected;
}

std::filesystem::path FindNgxCoreIn(const std::filesystem::path& override_path, const std::filesystem::path& repository,
                                    const DriverFolderLookup& driver_folder) {
  std::error_code error;
  if (!override_path.empty() && std::filesystem::is_regular_file(override_path, error)) return override_path;
  if (const auto folder = driver_folder(D3D_DRIVER_MODULE); !folder.empty()) {
    if (auto core = CoreInFolder(folder); !core.empty()) return core;
  }
  if (auto core = NewestStoreCore(repository, StorePackages::GENERIC); !core.empty()) return core;
  // Neither the Direct3D driver's folder nor a generic package holds a core: a Vulkan game on a driver package with another name.
  if (const auto folder = driver_folder(VULKAN_DRIVER_MODULE); !folder.empty()) {
    if (auto core = CoreInFolder(folder); !core.empty()) return core;
  }
  return NewestStoreCore(repository, StorePackages::OTHER_NVIDIA);
}

std::filesystem::path FindNgxCore(const std::filesystem::path& override_path) {
  return FindNgxCoreIn(override_path, DriverStoreRepository(), LoadedDriverFolder);
}

}  // namespace uplift::nr
