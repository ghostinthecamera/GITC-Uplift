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

// Shared by LoadSettings (an ini value) and Sanitized (an in-memory value already parsed).
bool InRange(const SettingDescriptor& descriptor, double value) {
  if (descriptor.special.has_value() && value == *descriptor.special) return true;
  return value >= descriptor.min && value <= DescriptorMax(descriptor);
}

}  // namespace

std::optional<bool> ParseBoolSetting(std::string_view text) {
  if (text == "1" || text == "true") return true;
  if (text == "0" || text == "false") return false;
  return std::nullopt;
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
    if (descriptor.kind == SettingKind::TEXT) {
      descriptor.set_text(settings, *text);
      continue;
    }
    std::optional<double> value;
    if (descriptor.kind == SettingKind::BOOL) {
      if (const std::optional<bool> flag = ParseBoolSetting(*text); flag.has_value()) {
        value = (*flag ? 1.0 : 0.0);
      }
    } else if (descriptor.kind == SettingKind::FLOAT) {
      float parsed = 0.f;
      const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), parsed);
      if (error == std::errc{} && end == text->data() + text->size() && std::isfinite(parsed)) {
        value = static_cast<double>(parsed);
      }
    } else {
      uint32_t parsed = 0u;
      const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), parsed);
      if (error == std::errc{} && end == text->data() + text->size()) {
        value = static_cast<double>(parsed);
      }
    }
    if (!value) {
      warn(std::format("{}: ignored the unreadable value '{}'", descriptor.key, *text));
    } else if (!InRange(descriptor, *value)) {
      warn(std::format("{}: '{}' is out of range; using the default {}", descriptor.key, *text,
                       FormatSettingValue(descriptor, DefaultSettings())));
    } else {
      descriptor.set(settings, *value);
    }
  }
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
    // v2 design D6: a pass that follows pass 1 writes only its switch.
    if (descriptor.pass != 0u && (descriptor.flags & setting_flags::PASS_SWITCH) == 0u
        && settings.passes[descriptor.pass - 2u].follow_pass1) {
      continue;
    }
    switch (descriptor.kind) {
      case SettingKind::TEXT:   store->Set(descriptor.key, descriptor.get_text(settings)); break;
      case SettingKind::BOOL:   store->Set(descriptor.key, (descriptor.get(settings) != 0.0 ? "1" : "0")); break;
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
        descriptor.set(settings, std::clamp(value, descriptor.min, descriptor.max));
      }
    }
  }
  return settings;
}

SourcePick SourcePickOf(const Settings& settings, bool explicit_dlss) {
  if (settings.source == PlacementSource::PRESENT) return SourcePick::PRESENT;
  if (explicit_dlss && settings.source != PlacementSource::DLSS) return SourcePick::PRESENT;  // Plan 13: Auto stays at Present on Vulkan
  if (settings.pre_upscale) return SourcePick::BEFORE_UPSCALING;
  return SourcePick::AFTER_DLSS;
}

void SetSourcePick(Settings* settings, SourcePick pick, bool explicit_dlss) {
  // Plan 13: on Vulkan the DLSS picks are the explicit PlacementSource::DLSS, and Present is the default Auto.
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
  };
}

SessionOptions ToSessionOptions(const Settings& settings) {
  return {
      .enabled = settings.enabled,
      .pass_count = settings.pass_count,
      .preset = settings.preset,
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
