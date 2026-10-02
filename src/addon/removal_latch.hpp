#pragma once

#include <filesystem>
#include <string_view>

namespace uplift::addon {

// Fix round 1, Important 1 (v2 design's Safety latch, amended): the device-removal latch survives a
// game that crashes right after a removal, because ReShade's own ini cache only reaches disk on a
// later present or at shutdown. Uplift instead persists a marker file of its own, synchronously,
// the moment the latch trips.

// %LOCALAPPDATA%\Uplift, or the environment variable UPLIFT_STATE_DIR when it is set (a test
// override: tests must point it at a folder under build\, never the user's profile or any read-only
// location). Falls back to the temp directory if neither resolves.
[[nodiscard]] std::filesystem::path UpliftStateDirectory();

// `state_directory`\latch\<exe-stem>-<8-hex FNV-1a of the full exe path>.latch: one marker per game
// executable, named only from data Uplift itself already has (never user input).
[[nodiscard]] std::filesystem::path LatchMarkerPath(const std::filesystem::path& state_directory,
                                                    const std::filesystem::path& game_exe_path);

// Persists the trip: creates the marker's folder, writes one line (the local time and `message`),
// then FlushFileBuffers, so the write reaches disk before this call returns. Logs once and returns
// without throwing if any step fails.
void WriteLatchMarker(const std::filesystem::path& marker_path, std::string_view message);
// True when the marker exists: read once, at AddonInit, before DlssUnavailableReason decides whether
// a DLSS placement can run this session.
[[nodiscard]] bool LatchMarkerExists(const std::filesystem::path& marker_path);
// "Clear latch": removes the marker so the next game start tries the DLSS placements again.
void DeleteLatchMarker(const std::filesystem::path& marker_path);

}  // namespace uplift::addon
