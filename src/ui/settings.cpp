#include "ui/settings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <system_error>
#include <utility>

#include "ui/settings_schema.hpp"

namespace uplift::ui {
namespace {

constexpr std::string_view BOOL_ONE = "1";
constexpr std::string_view BOOL_ZERO = "0";
constexpr std::string_view TEXT_TRUE = "true";
constexpr std::string_view TEXT_FALSE = "false";
constexpr std::string_view FG_PRESENT_KEY = "PresentWithFrameGen";
constexpr char UNREADABLE_SETTING_WARNING[] = "{}: ignored the unreadable value '{}'";
constexpr char OUT_OF_RANGE_SETTING_WARNING[] = "{}: '{}' is out of range; using the default {}";

// Shared by ApplySettingText (an ini value) and Sanitized (an in-memory value already parsed).
bool InRange(const SettingDescriptor& descriptor, double value) {
  if (descriptor.special.has_value() && value == *descriptor.special) return true;
  return value >= descriptor.min && value <= DescriptorMax(descriptor);
}

}  // namespace

std::optional<bool> ParseBoolSetting(std::string_view text) {
  if (text == BOOL_ONE || text == TEXT_TRUE) return true;
  if (text == BOOL_ZERO || text == TEXT_FALSE) return false;
  return std::nullopt;
}

std::errc ApplySettingText(Settings& settings, const SettingDescriptor& descriptor, std::string_view text) {
  if (descriptor.kind == SettingKind::TEXT) {
    descriptor.set_text(settings, text);
    return {};
  }
  double value{};
  if (descriptor.kind == SettingKind::BOOL || (descriptor.key == FG_PRESENT_KEY && (text == TEXT_TRUE || text == TEXT_FALSE))) {
    const auto parsed = ParseBoolSetting(text);
    if (!parsed) return std::errc::invalid_argument;
    value = static_cast<double>(*parsed);
  } else if (descriptor.kind == SettingKind::FLOAT) {
    float parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(parsed)) return std::errc::invalid_argument;
    value = parsed;
  } else {
    uint32_t parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) return std::errc::invalid_argument;
    value = parsed;
  }
  if (!InRange(descriptor, value)) return std::errc::result_out_of_range;
  descriptor.set(settings, value);
  return {};
}

Settings LoadSettings(const ConfigStore& store, std::vector<std::string>* warnings) {
  Settings settings;
  const auto warn = [warnings](std::string line) {
    if (warnings != nullptr) {
      warnings->push_back(std::move(line));
    }
  };
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    const std::optional<std::string> text = store.Get(descriptor.key);
    if (!text) continue;
    const auto error = ApplySettingText(settings, descriptor, *text);
    if (error == std::errc::invalid_argument) {
      warn(std::format(UNREADABLE_SETTING_WARNING, descriptor.key, *text));
    } else if (error == std::errc::result_out_of_range) {
      warn(std::format(OUT_OF_RANGE_SETTING_WARNING, descriptor.key, *text, FormatSettingValue(descriptor, DefaultSettings())));
    }
  }
  if (settings.use_d3d9ex) settings.startup_latches &= ~Settings::LATCH_D3D9EX;
  if (settings.adjust_vulkan_devices) settings.startup_latches &= ~Settings::LATCH_VK_DEVICE;
  if (settings.adjust_vulkan_devices_for_ngx) settings.startup_latches &= ~Settings::LATCH_VK_NGX;
  MigrateSettings(&settings);
  return settings;
}

void MigrateSettings(Settings* settings) {
  if (settings->config_version < 3u && settings->input_exposure == color::InputExposure::GAME) {
    settings->input_exposure = color::InputExposure::AUTO;
  }
}

void SaveSettings(const Settings& settings, ConfigStore* store) {
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    // A pass sharing its look still writes its look-sharing switch, stage and network selection (PASS_SWITCH rows).
    if (descriptor.pass != 0u && (descriptor.flags & setting_flags::PASS_SWITCH) == 0u
        && settings.passes[descriptor.pass - 2u].follow_pass1) {
      continue;
    }
    switch (descriptor.kind) {
      case SettingKind::TEXT:   store->Set(descriptor.key, descriptor.get_text(settings)); break;
      case SettingKind::BOOL:   store->Set(descriptor.key, (descriptor.get(settings) != 0.0 ? BOOL_ONE : BOOL_ZERO)); break;
      case SettingKind::FLOAT:  store->Set(descriptor.key, std::format("{}", static_cast<float>(descriptor.get(settings)))); break;
      case SettingKind::UINT:
      case SettingKind::CHOICE:
      case SettingKind::KEY:    store->Set(descriptor.key, std::format("{}", static_cast<uint32_t>(descriptor.get(settings)))); break;
    }
  }
}

Settings Sanitized(Settings settings) {
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    if (descriptor.kind == SettingKind::TEXT) continue;
    const double value = descriptor.get(settings);
    if (!std::isfinite(value)) {
      ResetSetting(&settings, descriptor);
    } else if (!InRange(descriptor, value)) {
      if (descriptor.kind == SettingKind::CHOICE || descriptor.kind == SettingKind::KEY) {
        ResetSetting(&settings, descriptor);
      } else {
        descriptor.set(settings, std::clamp(value, descriptor.min, DescriptorMax(descriptor)));
      }
    }
  }
  return settings;
}

SourcePick SourcePickOf(const Settings& settings, bool explicit_dlss) {
  if (settings.source == PlacementSource::PRESENT) return SourcePick::PRESENT;
  if (explicit_dlss && settings.source != PlacementSource::DLSS) return SourcePick::PRESENT;  // Auto stays at Present on Vulkan
  if (settings.pre_upscale) return SourcePick::BEFORE_UPSCALING;
  return SourcePick::AFTER_DLSS;
}

void SetSourcePick(Settings* settings, SourcePick pick, bool explicit_dlss) {
  // On Vulkan the DLSS picks are the explicit PlacementSource::DLSS, and Present is the default Auto.
  const PlacementSource dlss_source = (explicit_dlss ? PlacementSource::DLSS : PlacementSource::AUTO);
  switch (pick) {
    case SourcePick::BEFORE_UPSCALING:
      settings->source = dlss_source;
      settings->pre_upscale = true;
      break;
    case SourcePick::AFTER_DLSS:
      settings->source = dlss_source;
      settings->pre_upscale = false;
      break;
    case SourcePick::PRESENT:
      settings->source = (explicit_dlss ? PlacementSource::AUTO : PlacementSource::PRESENT);
      settings->pre_upscale = false;
      break;
  }
}

MotionPick MotionPickOf(const Settings& settings) {
  switch (settings.motion_vectors) {
    case MotionVectorSource::LAUNCHPAD: return MotionPick::LAUNCHPAD;
    case MotionVectorSource::LUMENITE:  return MotionPick::LUMENITE;
    case MotionVectorSource::NONE:      return MotionPick::OFF;
    case MotionVectorSource::AUTO:
    case MotionVectorSource::DLSS:      break;
  }
  return MotionPick::DLSS;
}

void SetMotionPick(Settings* settings, MotionPick pick) {
  switch (pick) {
    case MotionPick::OFF:       settings->motion_vectors = MotionVectorSource::NONE; break;
    case MotionPick::DLSS:      settings->motion_vectors = MotionVectorSource::AUTO; break;
    case MotionPick::LAUNCHPAD: settings->motion_vectors = MotionVectorSource::LAUNCHPAD; break;
    case MotionPick::LUMENITE:  settings->motion_vectors = MotionVectorSource::LUMENITE; break;
  }
}

nr::Controls ToControls(const Settings& settings) {
  return {
      .intensity = settings.intensity,
      .local_tone = settings.local_tone,
      .local_structure = settings.local_structure,
      .global_tone = settings.global_tone,
      .auto_mask = settings.auto_mask,
      .skin_structure = settings.skin_structure,
      .style = settings.style,
      .mask_blend_mode = settings.mask_blend_mode,
  };
}

SessionOptions ToSessionOptions(const Settings& settings) {
  std::array<uint32_t, MAX_PASSES> presets;
  presets[0] = settings.preset;
  for (size_t index = 0u; index < settings.passes.size(); ++index) {
    const uint32_t own = settings.passes[index].preset;
    presets[index + 1u] = (own == 0u ? settings.preset : own);
  }
  return {
      .enabled = settings.enabled,
      .pass_count = settings.pass_count,
      .presets = presets,
      .performance = settings.performance,
      .grace = std::chrono::milliseconds(std::lround(settings.grace_seconds * 1000.f)),
      .auto_resume = settings.auto_resume,
      .margin_override_bytes = (settings.budget_margin_mb == 0u ? std::nullopt
                                                                : std::optional<uint64_t>(uint64_t{settings.budget_margin_mb} << 20u)),
      .vram_check = settings.vram_check,
      .auto_retry = settings.auto_retry,
  };
}

std::optional<bool> EnabledPoller::Poll(std::chrono::steady_clock::time_point now,
                                        const std::function<std::optional<bool>()>& read) {
  if (last_poll_ && now - *last_poll_ < period_) return std::nullopt;
  last_poll_ = now;
  const std::optional<bool> value = read();
  if (!value || value == last_seen_) return std::nullopt;
  last_seen_ = value;
  return value;
}

}  // namespace uplift::ui
