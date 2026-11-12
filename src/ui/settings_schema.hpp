#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "ui/settings.hpp"

namespace uplift::ui {

enum class SettingKind : uint8_t {
  BOOL,
  UINT,    // an integer in [min, max]
  FLOAT,   // a float in [min, max]
  CHOICE,  // an index into `choices`
  TEXT,    // UTF-8 text
  KEY,     // a Win32 virtual-key code in [min, max]; 0 = none
};

// The overlay's sections; resets and "(changed)" act per section.
enum class SettingSection : uint8_t {
  TOP,
  LOOK,
  RESULT,
  LIMITS,
  PLACEMENT,  // "What NR works on"
  PASSES,
  FIXES_COLOR,
  FIXES_GUIDES,
  MASK,  // "Mask and HUD"
  ADVANCED,
  HOTKEYS,
  HIDDEN,  // written by Uplift, never drawn as a control
};

namespace setting_flags {
inline constexpr uint32_t RESTARTS_HISTORY = 1u << 0u;  // a change restarts NR's temporal history
inline constexpr uint32_t NEXT_START = 1u << 1u;        // applies from the next game start
inline constexpr uint32_t NEXT_DLSS_CREATE = 1u << 2u;  // applies when the game next creates its DLSS feature
inline constexpr uint32_t GREYED = 1u << 3u;            // shown read-only: no effect in NR 310.8
inline constexpr uint32_t PASS_SWITCH = 1u << 4u;       // a pass's look-sharing, stage or network selection: always written, shown while it follows pass 1
inline constexpr uint32_t BOOKKEEPING = 1u << 5u;       // hidden state Uplift writes itself: Restore all keeps it
inline constexpr uint32_t APPLY_ON_RELEASE = 1u << 6u;  // a SliderInt row: shows its value while held, applies it once on release
// Split chain: a pass's "Runs at". Never part of the Defaults view: moving a pass to another stage rebuilds NR's features.
inline constexpr uint32_t PASS_STAGE = 1u << 7u;
// A FLOAT whose slider's range is its meaning (a share of the output, a blend): Ctrl+click cannot type past the slider's end.
inline constexpr uint32_t SLIDER_RANGE_ONLY = 1u << 8u;
inline constexpr uint32_t NETWORK_PRESET = 1u << 9u;  // creation configuration, independent of look sharing and Compare
// A FLOAT in the resolution scales' group: the grouped rail slider clamps typed values to the slider's range.
inline constexpr uint32_t SLIDER_ALWAYS_CLAMP = 1u << 10u;
// The Keep faces mask's tuning rows (Advanced): hidden while Keep faces is off, and greyed with it where it cannot run.
inline constexpr uint32_t KEEP_FACES_TUNING = 1u << 11u;
// A later stage's Keep faces here: hidden while Keep faces is off, and greyed with it where it cannot run.
inline constexpr uint32_t KEEP_FACES_HERE = 1u << 12u;
// KeepFaces and ShowFaceMask, the other rows greyed where Keep faces cannot run.
inline constexpr uint32_t KEEP_FACES_ROW = 1u << 13u;
// Keep faces' own sliders (Face protection, Lighting scale): hidden while Keep faces is off, and greyed with it where it cannot run.
inline constexpr uint32_t KEEP_FACES_OWN = 1u << 14u;
}  // namespace setting_flags

// How far past its slider's end a FLOAT accepts a typed value (Ctrl+click), as a multiple of that end.
inline constexpr double TYPED_PAST_SLIDER = 10.0;

// The longest a setting's tooltip may be, in bytes: about three lines once the overlay wraps it. Detail beyond that belongs in the Guide.
inline constexpr size_t TOOLTIP_MAX_CHARS = 200u;

// One persisted setting: the single description load, save, sanitising, resets
// and the overlay rows all read.
struct SettingDescriptor {
  std::string_view key;
  SettingKind kind = SettingKind::BOOL;
  SettingSection section = SettingSection::TOP;
  std::string_view label;
  std::string_view tooltip;
  std::string_view enable_warning;  // BOOL: confirmation text shown before a checkbox enables the setting
  double min = 0.0;
  double max = 1.0;
  std::optional<double> special;              // one valid value outside [min, max]: 0 = automatic, -1 = same as Structure
  double ui_max = 0.0;                        // FLOAT: the slider's end when below `max`; 0 = max. Ctrl+click types up to DescriptorMax
  std::span<const std::string_view> choices;  // CHOICE: the labels, in stored-index order
  uint32_t flags = 0u;
  double (*get)(const Settings&) = nullptr;  // every kind but TEXT; BOOL is 0 or 1, CHOICE the index
  void (*set)(Settings&, double) = nullptr;
  std::string_view (*get_text)(const Settings&) = nullptr;  // TEXT only
  void (*set_text)(Settings&, std::string_view) = nullptr;
  uint8_t pass = 0u;  // 2-10: a per-pass row (Settings::passes[pass - 2]); 0: none
  bool (*hidden)(const Settings&) = nullptr;  // a row the overlay hides while this holds (a choice above it not made yet); nullptr: always shown
  const char* slider_format = nullptr;        // FLOAT: the slider's printf format; nullptr: the overlay's default
  double display_scale = 1.0;                 // FLOAT: the shown value is the stored value times this (AutoExposureBlend: 100)
};

// Every persisted setting once, in save order.
[[nodiscard]] std::span<const SettingDescriptor> SettingsSchema();
[[nodiscard]] const SettingDescriptor* FindSetting(std::string_view key);
// Invalid text or an out-of-range value leaves the current setting intact.
std::errc ApplySettingText(Settings& settings, const SettingDescriptor& descriptor, std::string_view text);
[[nodiscard]] const Settings& DefaultSettings();
[[nodiscard]] bool IsDefault(const Settings& settings, const SettingDescriptor& descriptor);
void ResetSetting(Settings* settings, const SettingDescriptor& descriptor);
// True when a row of `section` differs from its default: the section header's "(changed)".
[[nodiscard]] bool SectionChanged(const Settings& settings, SettingSection section);
void ResetSection(Settings* settings, SettingSection section);
// The look group the Defaults view swaps -- the whole of LOOK (Intensity, the
// Model rows, Pass Count and every pass's own rows included), RESULT, LIMITS and PASSES. Mask and UI
// correction belong to MASK and are excluded, as are a pass's "Runs at" or network selection (PASS_STAGE / NETWORK_PRESET).
[[nodiscard]] bool InDefaultsView(const SettingDescriptor& descriptor);
// `mine` with the look group at its built-in values. A view only: nothing ever saves it.
[[nodiscard]] Settings DefaultsView(Settings mine);
// Every row back to its default, the hotkey and the hidden DLSS-SR overrides included, except Enable and BOOKKEEPING rows.
void RestoreAllDefaults(Settings* settings);
// The value as the overlay shows it: "1.00", "on", "Model A", "automatic", "none", "empty".
[[nodiscard]] std::string FormatSettingValue(const SettingDescriptor& descriptor, const Settings& settings);
// The largest valid value: `max`, the last choice index for a CHOICE, or for a FLOAT what Ctrl+click may type past its slider (TYPED_PAST_SLIDER).
[[nodiscard]] double DescriptorMax(const SettingDescriptor& descriptor);
// A FLOAT slider's end: `ui_max` when set, else `max`.
[[nodiscard]] double SliderMax(const SettingDescriptor& descriptor);
// The overlay hides this row while `settings` hold its `hidden` condition.
[[nodiscard]] bool RowHidden(const SettingDescriptor& descriptor, const Settings& settings);
// Manual exposure is Diffuse white alone, so the overlay shows the Diffuse white row in the Exposure group, under Input exposure, while
// `settings` says Manual, and in the Colour group otherwise.
[[nodiscard]] bool DiffuseWhiteInExposureGroup(const Settings& settings);

}  // namespace uplift::ui
