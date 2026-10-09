#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

// The overlay's sections (v2 design §5); resets and "(changed)" act per section.
enum class SettingSection : uint8_t {
  TOP,
  LOOK,
  RESULT,
  LIMITS,
  PLACEMENT,  // ui-review.md §3: "What NR works on" -- was PERFORMANCE
  PASSES,
  FIXES_COLOR,
  FIXES_GUIDES,
  MASK,  // ui-review.md §3: "Mask and HUD"
  ADVANCED,
  HOTKEYS,
  HIDDEN,  // written by Uplift, never drawn as a control
};

namespace setting_flags {
inline constexpr uint32_t RESTARTS_HISTORY = 1u << 0u;  // a change restarts NR's temporal history
inline constexpr uint32_t NEXT_START = 1u << 1u;        // applies from the next game start
inline constexpr uint32_t NEXT_DLSS_CREATE = 1u << 2u;  // applies when the game next creates its DLSS feature
inline constexpr uint32_t GREYED = 1u << 3u;            // shown read-only: no effect in NR 310.8
inline constexpr uint32_t PASS_SWITCH = 1u << 4u;       // a pass's "Same as pass 1": always written
inline constexpr uint32_t BOOKKEEPING = 1u << 5u;       // hidden state Uplift writes itself: Restore all keeps it
inline constexpr uint32_t APPLY_ON_RELEASE = 1u << 6u;  // a SliderInt row: shows its value while held, applies it once on release
}  // namespace setting_flags

// One persisted setting (v2 design §3.16): the single description load, save, sanitising, resets
// and the overlay rows all read.
struct SettingDescriptor {
  std::string_view key;
  SettingKind kind = SettingKind::BOOL;
  SettingSection section = SettingSection::TOP;
  std::string_view label;
  std::string_view tooltip;
  double min = 0.0;
  double max = 1.0;
  std::optional<double> special;              // one valid value outside [min, max]: 0 = automatic, -1 = same as Structure
  double ui_max = 0.0;                        // FLOAT: the slider's end when below `max` (Ctrl+click types up to `max`); 0 = max
  std::span<const std::string_view> choices;  // CHOICE: the labels, in stored-index order
  uint32_t flags = 0u;
  double (*get)(const Settings&) = nullptr;  // every kind but TEXT; BOOL is 0 or 1, CHOICE the index
  void (*set)(Settings&, double) = nullptr;
  std::string_view (*get_text)(const Settings&) = nullptr;  // TEXT only
  void (*set_text)(Settings&, std::string_view) = nullptr;
  uint8_t pass = 0u;  // 2-10: a per-pass row (Settings::passes[pass - 2]); 0: none
};

// Every persisted setting once, in save order (spec §12 as amended by the v2 design §5).
[[nodiscard]] std::span<const SettingDescriptor> SettingsSchema();
[[nodiscard]] const SettingDescriptor* FindSetting(std::string_view key);
[[nodiscard]] const Settings& DefaultSettings();
[[nodiscard]] bool IsDefault(const Settings& settings, const SettingDescriptor& descriptor);
void ResetSetting(Settings* settings, const SettingDescriptor& descriptor);
// True when a row of `section` differs from its default: the section header's "(changed)".
[[nodiscard]] bool SectionChanged(const Settings& settings, SettingSection section);
void ResetSection(Settings* settings, SettingSection section);
// Plan 6 (v2 design §3.16, D6): the look group the Defaults view swaps -- the whole of LOOK (Intensity, the
// Model rows, Pass Count and every pass's own rows included), RESULT, LIMITS and PASSES. Mask and UI
// correction are not part of it: ui-review.md §3 moved them out of LOOK into their own MASK section.
[[nodiscard]] bool InDefaultsView(const SettingDescriptor& descriptor);
// `mine` with the look group at its built-in values. A view only: nothing ever saves it.
[[nodiscard]] Settings DefaultsView(Settings mine);
// G2: every row back to its default, the hotkey and the hidden DLSS-SR overrides included, except Enable and BOOKKEEPING rows.
void RestoreAllDefaults(Settings* settings);
// The value as the overlay shows it: "1.00", "on", "Model A", "automatic", "none", "empty".
[[nodiscard]] std::string FormatSettingValue(const SettingDescriptor& descriptor, const Settings& settings);
// The largest valid value: `max`, or the last choice index for a CHOICE.
[[nodiscard]] double DescriptorMax(const SettingDescriptor& descriptor);
// The colour fixes' exposure rows the overlay hides (ui-review.md §3 rule 1) while `settings`' Input exposure makes them moot: Adaptation and its rates unless
// Metered or Auto; 2026-10-09: Auto exposure unless Auto, and Blend unless Auto's Blend.
[[nodiscard]] bool ExposureRowHidden(const SettingDescriptor& descriptor, const Settings& settings);

}  // namespace uplift::ui
