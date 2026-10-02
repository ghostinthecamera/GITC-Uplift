#include "addon/reshade_version.hpp"

#include <winver.h>

#include <cstddef>
#include <format>
#include <vector>

namespace uplift::addon {

std::optional<ModuleVersion> ReadModuleVersion(HMODULE module) {
  std::wstring path(32768u, L'\0');
  const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0u || length >= path.size()) return std::nullopt;
  path.resize(length);
  DWORD handle = 0u;
  const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &handle);
  if (size == 0u) return std::nullopt;
  std::vector<std::byte> data(size);
  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT fixed_size = 0u;
  if (GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0u, size, data.data()) == FALSE
      || VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_size) == FALSE || fixed == nullptr
      || fixed_size < sizeof(VS_FIXEDFILEINFO)) {
    return std::nullopt;
  }
  return ModuleVersion{
      .major = HIWORD(fixed->dwFileVersionMS),
      .minor = LOWORD(fixed->dwFileVersionMS),
      .patch = HIWORD(fixed->dwFileVersionLS),
      .build = LOWORD(fixed->dwFileVersionLS),
  };
}

bool SupportsDlssPlacement(const ModuleVersion& version) {
  return version >= MIN_DLSS_PLACEMENT_RESHADE;
}

std::string FormatModuleVersion(const ModuleVersion& version) {
  return std::format("{}.{}.{}.{}", version.major, version.minor, version.patch, version.build);
}

}  // namespace uplift::addon
