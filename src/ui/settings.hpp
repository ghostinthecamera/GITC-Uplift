#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "color/encoding.hpp"
#include "look/look_math.hpp"
#include "nr/log.hpp"
#include "nr/types.hpp"

namespace uplift::ui {

inline constexpr char CONFIG_SECTION[] = "Uplift";
inline constexpr char ENABLED_KEY[] = "Enabled";  // the one key the add-on polls every present
inline constexpr float SKIN_SAME_AS_STRUCTURE = -1.f;

inline constexpr uint32_t CONFIG_VERSION = 2u;  // v2 design §5: written from Plan 3; Plan 6 migrates older files

// v2 design §5. Every combo is stored as its index, in the order below.
enum class PlacementSource : uint32_t {
  AUTO = 0u,     // after DLSS once the game has used DLSS, else Present
  DLSS = 1u,     // after DLSS only
  PRESENT = 2u,  // the presented frame only
};

// v2 design §3.8 (T4, P15): the Resolution combo, in stored-index order.
enum class ResolutionMode : uint32_t {
  FULL = 0u,
  QUALITY = 1u,      // 67 %
  BALANCED = 2u,     // 58 %
  PERFORMANCE = 3u,  // 50 %
  MATCH_GAME = 4u,   // DLSS's create-time render size
  CUSTOM = 5u,       // ResolutionScale
};

enum class MotionVectorSource : uint32_t {
  AUTO = 0u,
  DLSS = 1u,
  LAUNCHPAD = 2u,  // Plan 6; treated as NONE until then
  NONE = 3u,
};

// ui-review.md §4.1: the Source toggle the overlay draws, in pipeline order. `SourcePickOf`/`SetSourcePick`
// translate to and from the persisted (PlacementSource, PreUpscale) pair; there are no new ini keys.
enum class SourcePick : uint32_t {
  BEFORE_UPSCALING,
  AFTER_DLSS,
  PRESENT,
};

// ui-review.md §4.2: the Motion vectors toggle, in pipeline order. `MotionPickOf`/`SetMotionPick` translate
// to and from the persisted MotionVectorSource.
enum class MotionPick : uint32_t {
  OFF,
  DLSS,
  LAUNCHPAD,
};

// The stored index is NVSDK_NGX_PerfQuality_Value + 1; GAME keeps the game's own mode.
enum class DlssQualityOverride : uint32_t {
  GAME = 0u,
  PERFORMANCE = 1u,
  BALANCED = 2u,
  QUALITY = 3u,
  ULTRA_PERFORMANCE = 4u,
  ULTRA_QUALITY = 5u,
  DLAA = 6u,
};

// GAME keeps the game's preset; J..M are DLSS render presets 10..13 (spec §10).
enum class DlssPresetOverride : uint32_t {
  GAME = 0u,
  J = 1u,
  K = 2u,
  L = 3u,
  M = 4u,
};

// The DLSS-SR modes with a preset key, in `DLSSPreset<Mode>` order; indexes `Settings::dlss_presets`.
enum class DlssPresetMode : uint32_t {
  DLAA = 0u,
  ULTRA_QUALITY = 1u,
  QUALITY = 2u,
  BALANCED = 3u,
  PERFORMANCE = 4u,
  ULTRA_PERFORMANCE = 5u,
  COUNT = 6u,
};

enum class AutoExposureOverride : uint32_t {
  GAME = 0u,
  OFF = 1u,
  ON = 2u,
};

// DLSSNR.DepthInverted: GAME forwards DLSS's DepthInverted create flag (no effect in NR 310.8, E8).
enum class DepthDirection : uint32_t {
  GAME = 0u,
  NORMAL = 1u,
  INVERTED = 2u,
};

// What Uplift replays after recording on a game list (v2 design §3.2).
enum class StateRestore : uint32_t {
  FULL = 0u,
  MINIMAL = 1u,  // the descriptor heaps and root signature only
};

enum class NgxHooksMode : uint32_t {
  AUTO = 0u,
  OFF = 1u,  // safe mode, from the next game start: only the Present path exists
};

enum class ForeignNrMode : uint32_t {
  YIELD = 0u,    // stand down while another NR producer runs
  OBSERVE = 1u,  // keep running, and log it
};

// Plan 5 (v2 design §5, key decision 8): Auto binds an effect's UPLIFT_MASK, when one runs.
enum class MaskMode : uint32_t {
  AUTO = 0u,
  OFF = 1u,
};

// Plan 5 (design D7 B, parked): forwards DLSSNR.UICorrection on the Present path only, when On.
enum class UiCorrection : uint32_t {
  AUTO = 0u,
  OFF = 1u,
  ON = 2u,
};

// Plan 5 (v2 design §3.14): Smooth adapts at AdaptBrighterStops/AdaptDarkerStops; Off applies the target every frame.
enum class ExposureAdapt : uint32_t {
  OFF = 0u,
  SMOOTH = 1u,
};

// Plan 5 (F11): greyed until E15 is checked in game.
enum class PedestalRemoval : uint32_t {
  OFF = 0u,
  AUTO = 1u,
  ALWAYS = 2u,
};

inline constexpr uint32_t MAX_PASSES = 10u;

// Pass n >= 2's own settings (v2 design §3.10, P4-P13), used only while it does not follow pass 1.
struct PassSettings {
  bool follow_pass1 = true;
  float intensity = 1.f;
  uint32_t style = 0u;
  float local_tone = 1.f;
  float local_structure = 1.f;
  float skin_structure = 1.f;
  bool auto_mask = true;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  friend bool operator==(const PassSettings&, const PassSettings&) = default;
};

// Every setting persisted in ReShade.ini [Uplift] (spec §12, v2 design §5), with their defaults.
// ui/settings_schema.cpp describes each field once; load, save, sanitising and the overlay read it.
struct Settings {
  bool enabled = false;
  uint32_t enable_key = 0u;  // Win32 virtual-key code; 0 = no hotkey
  float intensity = 1.f;     // slider 0-1; typed values up to 10 pass through
  uint32_t style = 0u;       // 0-2 = Model A/B/C
  float local_structure = 1.f;
  float local_tone = 1.f;
  bool auto_mask = true;
  float skin_structure = 1.f;  // SKIN_SAME_AS_STRUCTURE or 0-1
  uint32_t pass_count = 1u;
  color::Encoding encoding = color::Encoding::AUTO;
  float diffuse_white_nits = 0.f;  // 0 = automatic for the encoding, else 48-500
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  float global_tone = 1.f;    // forwarded; no effect in NR 310.8
  uint32_t preset = 1u;       // forwarded; no effect in NR 310.8
  uint32_t performance = 3u;  // forwarded; no effect in NR 310.8
  float grace_seconds = 5.f;
  uint32_t budget_margin_mb = 0u;  // 0 = automatic (spec §6.5)
  bool auto_resume = true;
  bool show_nv_indicator = false;
  nr::LogLevel log_level = nr::LogLevel::INFO;
  std::string snippet_path;  // UTF-8; empty = next to the add-on, then the game folder
  PlacementSource source = PlacementSource::AUTO;
  MotionVectorSource motion_vectors = MotionVectorSource::AUTO;
  DlssQualityOverride dlss_quality_mode = DlssQualityOverride::GAME;
  std::array<DlssPresetOverride, static_cast<size_t>(DlssPresetMode::COUNT)> dlss_presets = {};
  AutoExposureOverride dlss_auto_exposure = AutoExposureOverride::GAME;
  bool present_with_frame_gen = false;
  float motion_scale_x = 1.f;  // multiplies DLSSNR.MVecScaleX; negative flips the axis
  float motion_scale_y = 1.f;
  DepthDirection depth_direction = DepthDirection::GAME;
  bool chained_history = true;  // off resets passes 2+ every frame (a diagnostic)
  StateRestore state_restore = StateRestore::FULL;
  NgxHooksMode ngx_hooks = NgxHooksMode::AUTO;
  ForeignNrMode foreign_nr = ForeignNrMode::YIELD;
  ResolutionMode resolution_mode = ResolutionMode::FULL;  // user ruling: Full by default
  float resolution_scale = 100.f;                         // Custom: % of the output, 25-100
  color::Upsampling upsampling = color::Upsampling::EDGE_AWARE;
  bool pre_upscale = false;  // Before upscaling (pre-SR), off until E13
  std::array<PassSettings, MAX_PASSES - 1u> passes = {};  // passes 2..10
  look::LookSettings look;                                // Shape result and its rows
  MaskMode mask = MaskMode::AUTO;
  UiCorrection ui_correction = UiCorrection::AUTO;
  color::InputExposure input_exposure = color::InputExposure::GAME;  // key decision 1: Plan 4's exposure
  ExposureAdapt exposure_adapt = ExposureAdapt::SMOOTH;
  float adapt_brighter_stops = 2.f;
  float adapt_darker_stops = 0.7f;
  float linear_unit_nits = 0.f;  // 0 = automatic
  color::Primaries source_primaries = color::Primaries::AUTO;
  color::NeuralTransfer neural_transfer = color::NeuralTransfer::BOUNDED_RATIO;
  float chroma_clamp = 0.f;
  bool near_black_guard = false;  // key decision 1: off keeps Plan 4's output
  PedestalRemoval pedestal_removal = PedestalRemoval::OFF;
  bool dlss_placement_blocked = false;       // hidden: the device-removal latch (v2 design §3.2)
  uint32_t config_version = CONFIG_VERSION;  // hidden: the version the loaded file declared
  bool auto_retry = true;                    // Plan 6 (D11): retry a failed session on the backoff
  bool use_d3d9ex = false;                   // Plan 9 (design §2.9): ask ReShade for a Direct3D 9Ex device (Direct3D 9 games, 32- and 64-bit)
  bool adjust_vulkan_devices = true;         // Plan 11 (Vulkan design §2.3): hidden; 0 keeps the vkCreateDevice hook recording devices but adding nothing

  friend bool operator==(const Settings&, const Settings&) = default;
};

// ui-review.md §4.1's table: Present reads PlacementSource::PRESENT; Before upscaling reads PreUpscale on;
// After DLSS is the fallback, so it also reads a legacy PlacementSource::DLSS (the dropped strict "wait for
// DLSS" choice, question 3) as itself.
// Plan 13 (the user's decision 1): with `explicit_dlss` (a Vulkan device), Auto stays at Present, so it reads as Present, and the DLSS picks are
// PlacementSource::DLSS (Before upscaling with PreUpscale on).
[[nodiscard]] SourcePick SourcePickOf(const Settings& settings, bool explicit_dlss = false);
// Writes the toggle's (PlacementSource, PreUpscale) pair. Any click -- Present or After DLSS included --
// rewrites a legacy PlacementSource::DLSS back to Auto (question 3). With `explicit_dlss` the DLSS picks write
// PlacementSource::DLSS and Present writes Auto (the default, which stays at Present on Vulkan).
void SetSourcePick(Settings* settings, SourcePick pick, bool explicit_dlss = false);
// ui-review.md §4.2's table: Off and Launchpad read their own MotionVectorSource; DLSS is the fallback, so it
// also reads a legacy MotionVectorSource::DLSS (strict, never Launchpad) as itself.
[[nodiscard]] MotionPick MotionPickOf(const Settings& settings);
// Writes the toggle's MotionVectorSource. Any click rewrites a legacy MotionVectorSource::DLSS to Auto.
void SetMotionPick(Settings* settings, MotionPick pick);

struct SessionOptions {
  bool enabled = false;
  uint32_t pass_count = 1u;
  uint32_t preset = 1u;
  uint32_t performance = 3u;
  std::chrono::milliseconds grace{5000};
  bool auto_resume = true;
  std::optional<uint64_t> margin_override_bytes;
  bool auto_retry = true;
};

// The [Uplift] section of a config file; the add-on binds it to ReShade's config API.
class ConfigStore {
 public:
  virtual ~ConfigStore() = default;
  [[nodiscard]] virtual std::optional<std::string> Get(std::string_view key) const = 0;
  virtual void Set(std::string_view key, std::string_view value) = 0;
};

// The boolean forms ReShade's config API and this file use ("1"/"true", "0"/"false"); nullopt for
// anything else, including an empty string. Shared by `LoadSettings` and a caller that wants a
// single boolean key without loading the rest of the section (the add-on's `Enabled` poll).
[[nodiscard]] std::optional<bool> ParseBoolSetting(std::string_view text);

// Missing keys keep their defaults. An unreadable, non-finite or out-of-range value keeps its default
// and adds one line to `warnings` (v2 design §3.16, Plan 2 final review M3).
[[nodiscard]] Settings LoadSettings(const ConfigStore& store, std::vector<std::string>* warnings);
void SaveSettings(const Settings& settings, ConfigStore* store);
// After an overlay edit: a non-finite value returns to its default, a number is clamped into its
// range (a documented special value such as 0 = automatic is kept), an unknown combo index or key
// code returns to its default.
[[nodiscard]] Settings Sanitized(Settings settings);
[[nodiscard]] nr::Controls ToControls(const Settings& settings);
[[nodiscard]] SessionOptions ToSessionOptions(const Settings& settings);

// Re-reads `Enabled` at most once per period, so a tool that writes [Uplift] Enabled through
// ReShade's config API toggles NR (spec amendment 8; the end-to-end test relies on it).
class EnabledPoller {
 public:
  explicit EnabledPoller(std::chrono::milliseconds period = std::chrono::seconds(1)) : period_(period) {}
  // The config value when it differs from the last value seen, else nullopt.
  std::optional<bool> Poll(std::chrono::steady_clock::time_point now, const std::function<std::optional<bool>()>& read);
  // Records a value the add-on applied itself (loaded, UI, hotkey), so it is not reported back.
  void Seen(bool value) { last_seen_ = value; }

 private:
  std::chrono::milliseconds period_;
  std::optional<std::chrono::steady_clock::time_point> last_poll_;
  std::optional<bool> last_seen_;
};

}  // namespace uplift::ui
