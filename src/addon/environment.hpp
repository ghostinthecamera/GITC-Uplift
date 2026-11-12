#pragma once

#include <d3d12.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uplift::addon {

inline constexpr wchar_t SNIPPET_FILE_NAME[] = L"nvngx_dlssnr.dll";
// 1.1.4: the folder next to the add-on where nvngx_dlssnr.dll belongs. NVIDIA's DLSS loads every nvngx_*.dll next to the game's .exe on RTX 50 cards (and
// Uplift cannot run NR on that copy), but never looks in here.
inline constexpr wchar_t RUNTIME_FOLDER[] = L"GITC-Uplift";
inline constexpr uint32_t NVIDIA_VENDOR_ID = 0x10DEu;

// RenoDX DLSS5 v7 sets this while its NR runtime is loaded.
inline constexpr wchar_t RENODX_NR_MARKER[] = L"RENODX_DLSS5_NR_RUNTIME_LOADED";

// The game's NR counts as running while it evaluated within this long, so turning it off in the game's menu gives NR back to Uplift within a second.
inline constexpr std::chrono::milliseconds FOREIGN_NR_IDLE_MS{1000};

// Evidence that another NR producer runs in this game.
struct ForeignNrSignals {
  bool foreign_nr_evaluating = false;     // a NGX feature-18 that Uplift did not make evaluated within FOREIGN_NR_IDLE_MS
  bool renodx_marker = false;            // RENODX_NR_MARKER is set
};

struct SnippetSearch {
  std::filesystem::path configured;       // SnippetPath; used alone when set, resolved below
  std::filesystem::path addon_directory;  // the folder gitc-uplift.addon64 was loaded from
  std::filesystem::path game_directory;   // the folder of the game's executable
};

// A relative `configured` resolves against `game_directory`, never the
// process's current working directory. nullopt when no candidate exists.
[[nodiscard]] std::optional<std::filesystem::path> LocateSnippet(const SnippetSearch& search);
// Lower-case hex SHA-256; empty when the file cannot be read.
[[nodiscard]] std::string Sha256Hex(const std::filesystem::path& file);
// The loaded file name (any case) of an add-on that already hosts NR. Only the exact
// RenoDX NR hosts count: renodx-dlssfix, for example, overrides DLSS-SR and runs no NR.
[[nodiscard]] std::optional<std::wstring> FindConflictingHost(const std::vector<std::wstring>& module_file_names);
// File names (no folders) of every module loaded in this process.
[[nodiscard]] std::vector<std::wstring> LoadedModuleFileNames();
// True when the device's adapter is NVIDIA's. Init_Ext also succeeds on WARP, so this runs first.
[[nodiscard]] bool IsNvidiaDevice(ID3D12Device* device);

// Converts UTF-8 to a path without throwing. std::filesystem::path's std::u8string constructor
// converts with MB_ERR_INVALID_CHARS and throws std::system_error on invalid UTF-8: a
// `[Uplift] SnippetPath` value hand-edited into an ANSI code page can contain exactly that, and an
// exception escaping the present event would crash the game. nullopt when `text` is not valid
// UTF-8; an empty `text` returns an empty path.
[[nodiscard]] std::optional<std::filesystem::path> PathFromUtf8(std::string_view text);
// The inverse, for logging: a best-effort UTF-8 rendering of `path` that never throws.
// std::filesystem::path::u8string() throws on a lone surrogate, so a strange path still logs
// something instead of crashing.
[[nodiscard]] std::string Utf8FromPath(const std::filesystem::path& path);

// True when the environment variable exists, whatever its value.
[[nodiscard]] bool IsEnvironmentMarkerSet(const wchar_t* name);
// Why another NR producer is running, or nullopt. With ForeignNr = Yield the caller stands down with
// this text. A copy of the runtime mapped from another file is no evidence: Uplift loads its own beside it. One mapped from the
// file Uplift would load is refused by nr::Snippet::Load whatever ForeignNr says.
[[nodiscard]] std::optional<std::string> ForeignNrProducer(const ForeignNrSignals& signals);

}  // namespace uplift::addon
