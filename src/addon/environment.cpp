#include "addon/environment.hpp"

#include <Windows.h>

#include <bcrypt.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>

namespace uplift::addon {

std::optional<std::filesystem::path> LocateSnippet(const SnippetSearch& search) {
  std::error_code error;
  if (!search.configured.empty()) {
    // A relative SnippetPath resolves against the game's own folder, never the process's current
    // working directory: many launchers start the game with an unrelated CWD.
    const std::filesystem::path configured =
        (search.configured.is_absolute() || search.game_directory.empty() ? search.configured
                                                                          : search.game_directory / search.configured);
    if (std::filesystem::is_regular_file(configured, error)) return configured;
    return std::nullopt;
  }
  for (const std::filesystem::path& directory : {search.addon_directory, search.game_directory}) {
    if (directory.empty()) continue;
    std::filesystem::path candidate = directory / SNIPPET_FILE_NAME;
    if (std::filesystem::is_regular_file(candidate, error)) return candidate;
  }
  return std::nullopt;
}

std::string Sha256Hex(const std::filesystem::path& file) {
  std::ifstream stream(file, std::ios::binary);
  if (!stream) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0u))) return {};
  const auto close_algorithm = [](BCRYPT_ALG_HANDLE handle) { BCryptCloseAlgorithmProvider(handle, 0u); };
  const std::unique_ptr<void, decltype(close_algorithm)> algorithm_owner(algorithm, close_algorithm);
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0u, nullptr, 0u, 0u))) return {};
  const auto destroy_hash = [](BCRYPT_HASH_HANDLE handle) { BCryptDestroyHash(handle); };
  const std::unique_ptr<void, decltype(destroy_hash)> hash_owner(hash, destroy_hash);

  std::vector<char> buffer(1u << 20u);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = stream.gcount();
    if (count > 0 && !BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0u))) {
      return {};
    }
  }
  if (stream.bad()) return {};
  std::array<UCHAR, 32> digest = {};
  if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0u))) return {};
  std::string hex;
  for (const UCHAR byte : digest) {
    hex += std::format("{:02x}", byte);
  }
  return hex;
}

std::optional<std::wstring> FindConflictingHost(const std::vector<std::wstring>& module_file_names) {
  constexpr std::array<std::wstring_view, 4> NR_HOSTS = {L"renodx-dlss.addon64", L"renodx-dlss.addon",
                                                         L"renodx-dlss5.addon64", L"renodx-dlss5.addon"};
  for (const std::wstring& name : module_file_names) {
    std::wstring lower_name(name);
    std::ranges::transform(lower_name, lower_name.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    if (std::ranges::find(NR_HOSTS, std::wstring_view(lower_name)) != NR_HOSTS.end()) return name;
  }
  return std::nullopt;
}

std::vector<std::wstring> LoadedModuleFileNames() {
  const HANDLE process = GetCurrentProcess();
  std::vector<HMODULE> modules(512u);
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
  std::vector<std::wstring> names;
  std::wstring path(32768u, L'\0');
  for (const HMODULE module : modules) {
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0u) continue;
    names.push_back(std::filesystem::path(path.substr(0u, length)).filename().wstring());
  }
  return names;
}

bool IsNvidiaDevice(ID3D12Device* device) {
  if (device == nullptr) return false;
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory2(0u, IID_PPV_ARGS(&factory)))) return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) return false;
  DXGI_ADAPTER_DESC1 description = {};
  if (FAILED(adapter->GetDesc1(&description))) return false;
  return description.VendorId == NVIDIA_VENDOR_ID;
}

std::optional<std::filesystem::path> PathFromUtf8(std::string_view text) {
  if (text.empty()) return std::filesystem::path();
  if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return std::nullopt;
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return std::nullopt;
  std::wstring wide(static_cast<size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length) <= 0) {
    return std::nullopt;
  }
  return std::filesystem::path(wide);
}

std::string Utf8FromPath(const std::filesystem::path& path) {
  const std::wstring& wide = path.native();
  if (wide.empty()) return {};
  if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return "<path too long to log>";
  const int length =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) return "<path is not valid UTF-16>";
  std::string utf8(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), utf8.data(), length, nullptr,
                      nullptr);
  return utf8;
}

bool IsEnvironmentMarkerSet(const wchar_t* name) {
  SetLastError(ERROR_SUCCESS);
  wchar_t value = L'\0';
  const DWORD length = GetEnvironmentVariableW(name, &value, 1u);
  return length > 0u || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

std::optional<std::string> ForeignNrProducer(const ForeignNrSignals& signals) {
  if (signals.live_foreign_nr > 0u) {
    return std::format("another tool created {} NR feature(s) through NVIDIA NGX in this game", signals.live_foreign_nr);
  }
  if (signals.runtime_mapped_elsewhere) {
    std::string reason = "nvngx_dlssnr.dll is already mapped in this game by another component, or stayed mapped after Uplift unloaded it";
    if (!signals.runtime_mapped_path.empty()) {
      reason += std::format(" ({})", signals.runtime_mapped_path);
    }
    return reason;
  }
  if (signals.renodx_marker) return std::string("RenoDX DLSS5 is running NR in this game");
  return std::nullopt;
}

}  // namespace uplift::addon
