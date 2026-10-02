#include "nr/driver_store.hpp"

#include <Windows.h>

#include <array>
#include <system_error>

namespace uplift::nr {
namespace {

std::filesystem::path CoreInFolder(const std::filesystem::path& folder) {
  for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
    std::error_code error;
    const auto candidate = folder / name;
    if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
  }
  return {};
}

std::filesystem::path ActiveDriverFolder() {
  const HMODULE driver = GetModuleHandleW(L"nvwgf2umx.dll");
  if (driver == nullptr) return {};
  std::array<wchar_t, 32768u> path = {};
  const DWORD length = GetModuleFileNameW(driver, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0u || length >= path.size()) return {};
  return std::filesystem::path(path.data(), path.data() + length).parent_path();
}

std::filesystem::path NewestDriverStoreCore() {
  std::array<wchar_t, MAX_PATH> system_directory = {};
  const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
  if (length == 0u || length >= system_directory.size()) return {};
  const auto repository = std::filesystem::path(system_directory.data()) / L"DriverStore" / L"FileRepository";
  std::error_code error;
  std::filesystem::path selected;
  std::filesystem::file_time_type selected_time = {};
  for (auto it = std::filesystem::directory_iterator(repository, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::directory_iterator();
       it.increment(error)) {
    if (!it->path().filename().wstring().starts_with(L"nv_dispi.inf_")) continue;
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

}  // namespace

std::filesystem::path FindNgxCore(const std::filesystem::path& override_path) {
  std::error_code error;
  if (!override_path.empty() && std::filesystem::is_regular_file(override_path, error)) return override_path;
  if (const auto folder = ActiveDriverFolder(); !folder.empty()) {
    if (auto core = CoreInFolder(folder); !core.empty()) return core;
  }
  return NewestDriverStoreCore();
}

}  // namespace uplift::nr
