#include "ui/overlay.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "addon/reshade_api.hpp"
#include "memory_units.hpp"
#include "ui/settings_schema.hpp"
#include "sources/work_size.hpp"
#include "ui/overlay_widgets.hpp"

namespace uplift::ui {
namespace {

// A FLOAT slider's printf format when its descriptor names none.
constexpr const char* DEFAULT_SLIDER_FORMAT = "%.2f";
// Layout: the status card's least height (style units; it keeps the card from jumping as its lines change), the Details table's
// narrowest five-column width (style units), and the widths of the warning popup, the language combo and the Compare choices (font heights).
constexpr float STATUS_CARD_MIN_HEIGHT_UNITS = 22.f;
constexpr float DETAILS_COLUMNS_MIN_WIDTH_UNITS = 112.f;
constexpr float WARNING_POPUP_WIDTH_EM = 34.f;
constexpr float LANGUAGE_COMBO_WIDTH_EM = 12.f;
constexpr float COMPARE_CHOICES_WIDTH_EM = 12.f;

// The tooltip line of a highlighted option while the highlight is not the stored preference (what runs differs from what ReShade.ini holds).
std::string SavedNote(std::string_view preference) {
  return std::format("Saved: {}; it runs again by itself when it can.", preference);
}

bool HasReset(const SettingDescriptor& descriptor, const Settings& settings) {
  return descriptor.section != SettingSection::TOP && !IsDefault(settings, descriptor);
}

// The reset button where the cursor is, its tooltip and its action.
bool ResetHere(const SettingDescriptor& descriptor, Settings& settings, OverlayState& state, const SetupView& setup) {
  const bool reset = gui::ResetButton();
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    const std::string value = descriptor.key == "MotionVectors" ? "DLSS" : FormatSettingValue(descriptor, DefaultSettings());
    gui::SetTooltip("Reset to default (%s)", value.c_str());
  }
  if (reset) {
    ResetSetting(&settings, descriptor);
    if (descriptor.key == "ResolutionMode") state.setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(DefaultSettings().resolution_mode), setup);
    if (descriptor.key == "MotionVectors") state.setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPick::DLSS), setup, true);
  }
  return reset;
}

bool DrawReset(const SettingDescriptor& descriptor, Settings& settings, OverlayState& state, bool align_right, const SetupView& setup) {
  if (!HasReset(descriptor, settings)) return false;
  gui::SameLine();
  if (align_right) {
    if (gui::GetContentRegionAvail().x + 1.f < gui::ResetButtonWidth()) gui::NewLine();
    const float remaining = gui::GetContentRegionAvail().x - gui::ResetButtonWidth();
    if (remaining > 0.f) gui::SetCursorPosX(gui::GetCursorPosX() + remaining);
  }
  return ResetHere(descriptor, settings, state, setup);
}

// Whether a grouped row draws `descriptor` with RailSlider: every FLOAT, and a UINT up to 100 but the native mask blend mode (four buttons).
// The dispatch in DrawRow and its caption read this one rule.
[[nodiscard]] bool UsesRailSlider(const SettingDescriptor& descriptor) {
  if (descriptor.kind == SettingKind::FLOAT) return true;
  return descriptor.kind == SettingKind::UINT && descriptor.max <= 100.0 && descriptor.key != "MaskBlendMode";
}

// A rail slider's value text at both ends of its range, as FormatSettingValue writes it: its row's value column fits the wider.
[[nodiscard]] std::array<std::string, 2> SliderBounds(const SettingDescriptor& descriptor) {
  Settings low = DefaultSettings(), high = DefaultSettings();
  descriptor.set(low, descriptor.min);
  descriptor.set(high, SliderMax(descriptor));
  return {FormatSettingValue(descriptor, low), FormatSettingValue(descriptor, high)};
}

// A grouped row's caption with its value and reset button: on one line with its rail slider where they fit (`row` says where the slider
// goes; the next item is the slider), else above the control.
bool DrawCaption(const SettingDescriptor& descriptor, const char* caption, const std::string& value, bool dimmed, Settings& settings,
                 OverlayState& state, const SetupView& setup, widgets::SliderRowPlan& row) {
  row = UsesRailSlider(descriptor) ? widgets::PlanSliderRow(caption, value, SliderBounds(descriptor)) : widgets::SliderRowPlan{};
  gui::BeginDisabled(dimmed);
  if (row.one_line) widgets::SliderRowCaption(row, value);
  else widgets::SettingCaption(caption, value, !IsDefault(settings, descriptor));
  gui::EndDisabled();
  if (!row.one_line) {
    const bool reset = DrawReset(descriptor, settings, state, true, setup);
    widgets::NextGap(style::LabelGap());
    return reset;
  }
  bool reset = false;
  if (HasReset(descriptor, settings)) {
    widgets::ToSliderRowControl(row, row.reset_x);
    reset = ResetHere(descriptor, settings, state, setup);
  }
  widgets::ToSliderRowControl(row, row.slider_x);
  gui::SetNextItemWidth(row.slider_width);
  return reset;
}

// Draw a settings-schema row with its normal availability, reset and commit rules.
bool DrawRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state, bool grouped = false) {
  std::string label(descriptor.label);
  if (descriptor.key == "ResolutionPresent") label = std::format("Resolution at {}", view.SharedPresentName(*settings));
  if (descriptor.key == "ResolutionScalePresent") label = std::format("Custom scale at {} (%)", view.SharedPresentName(*settings));
  // With Compare = Built-in, a look-group row shows its built-in value, read-only: edits never reach `settings`.
  const bool read_only = (state->defaults_view && InDefaultsView(descriptor));
  Settings built_in;
  if (read_only) {
    built_in = DefaultsView(*settings);
    settings = &built_in;
  }
  // Not `follows`: the SkinStructure branch below declares one (C4456).
  const bool pass_follows = (descriptor.pass != 0u && (descriptor.flags & setting_flags::PASS_SWITCH) == 0u
                             && settings->passes[descriptor.pass - 2u].follow_pass1);
  const bool shaping_off = ((descriptor.section == SettingSection::RESULT || descriptor.section == SettingSection::LIMITS)
                            && descriptor.key != "ShapeResult" && !settings->look.enabled);
  // Split chain: Upsampling is shown also while a later stage group works below Full.
  const auto reduced = [](ResolutionMode mode, float percent) {
    return mode != ResolutionMode::FULL && (mode != ResolutionMode::CUSTOM || percent < 100.f);
  };
  const auto later_group_scaled = [&](const PassStageRow& row) {
    return row.starts_group && ((row.runs == SourcePick::AFTER_DLSS && reduced(settings->resolution_mode_after_dlss, settings->resolution_scale_after_dlss))
                                || (row.runs == SourcePick::PRESENT && reduced(settings->resolution_mode_present, settings->resolution_scale_present)));
  };
  const bool upsampling_hidden = (descriptor.key == "Upsampling" && !reduced(settings->resolution_mode, settings->resolution_scale)
                                  && std::ranges::none_of(view.setup.pass_stages, later_group_scaled));
  // These rows matter only after a choice in the row just above them, so they
  // are hidden -- not merely greyed -- until that choice is made.
  if (pass_follows || shaping_off || RowHidden(descriptor, *settings) || upsampling_hidden) {
    return false;
  }

  gui::PushID(label.c_str());
  // Game state after NR only matters once NR can run inside the
  // game's frame; while no DLSS placement can run this session, it is greyed and says why.
  // The Direct3D 9Ex row is greyed the same way where it does nothing (every game but a Direct3D 9 one).
  // The VulkanNr row likewise, everywhere but a 64-bit Vulkan game.
  // The Diffuse white row on an sRGB image, where it does nothing (and the Manual exposure choice with it, below), and the Colour conversion rows
  // that do nothing on the shown encoding (ColorFixUnavailable).
  // Keep faces: its rows where it cannot run, and the face mask's view (in Advanced, far from the switch) while it is off.
  const bool keep_faces_row =
      ((descriptor.flags & (setting_flags::KEEP_FACES_ROW | setting_flags::KEEP_FACES_OWN | setting_flags::KEEP_FACES_TUNING | setting_flags::KEEP_FACES_HERE)) != 0u);
  const std::string_view white_unavailable = DiffuseWhiteUnavailable(view.encoding);
  const std::string_view fix_unavailable = ColorFixUnavailable(descriptor.key, view.encoding);
  const std::string_view disabled_reason =
      ((descriptor.flags & setting_flags::NETWORK_PRESET) != 0u && view.legacy_runtime)                   ? std::string_view("NR 310.8 only provides the Legacy network.")
      : (descriptor.key == "MaskBlendMode" && view.legacy_runtime)            ? std::string_view("NR 310.8 does not support native mask blend mode.")
      : (descriptor.key == "StateRestore" && !view.dlss_unavailable.empty())   ? std::string_view(view.dlss_unavailable)
      : (descriptor.key == "UseD3D9Ex" && !view.d3d9ex_unavailable.empty())    ? std::string_view(view.d3d9ex_unavailable)
      : (descriptor.key == "VulkanNr" && !view.vulkan_nr_unavailable.empty()) ? std::string_view(view.vulkan_nr_unavailable)
      : (descriptor.key == "DiffuseWhiteNits" && !white_unavailable.empty())  ? white_unavailable
      : (!fix_unavailable.empty())                                               ? fix_unavailable
      : (descriptor.key == "AllowCrossQueuePasses" && !view.cross_queue_passes_available) ? std::string_view("Only for 64-bit Direct3D 12 games")
      : (keep_faces_row && !view.keep_faces_unavailable.empty())               ? std::string_view(view.keep_faces_unavailable)
      : (descriptor.key == "ShowFaceMask" && !settings->keep_faces)            ? std::string_view("Turn on Keep faces (Look) first")
                                                                               : std::string_view();
  gui::BeginDisabled(!disabled_reason.empty() || read_only);
  bool changed = false;
  if (descriptor.key == "SkinStructure") {
    bool follows = (settings->skin_structure < 0.f);
    if (gui::Checkbox("Skin follows Structure", &follows)) {
      settings->skin_structure = (follows ? SKIN_SAME_AS_STRUCTURE : settings->local_structure);
      changed = true;
    }
  }
  if (descriptor.key == "DiffuseWhiteNits") {
    bool automatic = (settings->diffuse_white_nits <= 0.f);
    if (gui::Checkbox("Automatic diffuse white", &automatic)) {
      settings->diffuse_white_nits = (automatic ? 0.f : std::clamp(view.effective_diffuse_white_nits, 48.f, 500.f));
      changed = true;
    }
  }
  const bool captioned = grouped && descriptor.kind != SettingKind::BOOL;
  widgets::SliderRowPlan slider_row;  // where a one-line row put its slider
  if (captioned) {
    const char* caption = descriptor.key == "ResolutionMode" ? "Working resolution" : descriptor.key == "ResolutionScale" ? "Custom scale" : label.c_str();
    std::string value;
    const bool inherited_skin = descriptor.key == "SkinStructure" && settings->skin_structure < 0.f;
    if (descriptor.key == "ResolutionScale") value = std::format("{:.0f}%", settings->resolution_scale);
    else if (descriptor.key == "DiffuseWhiteNits") {
      const float nits = settings->diffuse_white_nits <= 0.f ? view.effective_diffuse_white_nits : settings->diffuse_white_nits;
      value = nits > 0.f ? std::format("{:.0f}", nits) : gui::Display("Unknown");
    }
    else if (descriptor.key == "AutoExposureBlend") value = std::format("{:.0f}%", settings->auto_exposure_blend * 100.f);
    else if (inherited_skin) value = std::format("{:.2f}", settings->local_structure);
    else if (descriptor.kind == SettingKind::FLOAT) value = FormatSettingValue(descriptor, *settings);
    else if (UsesRailSlider(descriptor))
      value = state->pending_key == descriptor.key && state->pending_release ? std::to_string(*state->pending_release) : FormatSettingValue(descriptor, *settings);
    changed |= DrawCaption(descriptor, caption, value, inherited_skin, *settings, *state, view.setup, slider_row);
    label = "##value";
  }
  std::string saved_note;  // the Resolution combo's tooltip, while the mode that runs is not the stored one
  if (grouped && descriptor.key == "MaskBlendMode") {
    static constexpr std::array<const char*, 4> modes{"0", "1", "2", "3"};
    const std::array<std::string_view, 4> why{disabled_reason, disabled_reason, disabled_reason, disabled_reason};
    const std::array<std::string_view, 4> tips{descriptor.tooltip, descriptor.tooltip, descriptor.tooltip, descriptor.tooltip};
    const int clicked = widgets::SegmentedControl(nullptr, modes, why, tips, static_cast<int>(settings->mask_blend_mode));
    if (clicked >= 0) {
      descriptor.set(*settings, clicked);
      changed = true;
    }
  } else if (descriptor.key == "SkinStructure") {
    const bool follows = settings->skin_structure < 0.f;
    gui::BeginDisabled(follows);
    float skin = (follows ? settings->local_structure : settings->skin_structure);
    const bool edited = grouped && UsesRailSlider(descriptor) ? widgets::RailSlider(label.c_str(), skin, static_cast<float>(descriptor.min), static_cast<float>(SliderMax(descriptor)), "%.2f")
                                : gui::SliderFloat(label.c_str(), &skin, 0.f, 1.f);
    if (edited) {
      settings->skin_structure = skin;
      changed = true;
    }
    state->drag.skin_structure = gui::IsItemActive();
    gui::EndDisabled();
  } else if (descriptor.key == "DiffuseWhiteNits") {
    // An Automatic checkbox, the control greyed at the value
    // in use -- shown, not hidden, so the effective value always reads even while it is automatic.
    const bool automatic = settings->diffuse_white_nits <= 0.f;
    gui::BeginDisabled(automatic);
    float nits = (automatic ? view.effective_diffuse_white_nits : settings->diffuse_white_nits);
    if (widgets::RailSlider(label.c_str(), nits, 48.f, 500.f, "%.0f") && !automatic) {
      settings->diffuse_white_nits = nits;
      changed = true;
    }
    gui::EndDisabled();
  } else {
    switch (descriptor.kind) {
      case SettingKind::BOOL: {
        bool value = (descriptor.get(*settings) != 0.0);
        if (widgets::Checkbox(label.c_str(), &value, !IsDefault(*settings, descriptor) && descriptor.section != SettingSection::TOP)) {
          if (value && !descriptor.enable_warning.empty()) {
            state->enable_warning_key = descriptor.key;
          } else {
            descriptor.set(*settings, (value ? 1.0 : 0.0));
            changed = true;
          }
        }
        break;
      }
      case SettingKind::UINT: {
        const int applied = static_cast<int>(descriptor.get(*settings));
        const bool on_release = ((descriptor.flags & setting_flags::APPLY_ON_RELEASE) != 0u);
        const bool held_here = (on_release && state->pending_release.has_value() && state->pending_key == descriptor.key);
        int value = (held_here ? *state->pending_release : applied);
        const int low = static_cast<int>(descriptor.min), high = static_cast<int>(descriptor.max);
        const bool edited = descriptor.max > 100.0 ? gui::InputInt(label.c_str(), &value, 64, 512)
            : grouped && UsesRailSlider(descriptor) ? widgets::RailSlider(label.c_str(), value, low, high, "%d")
                      : gui::SliderInt(label.c_str(), &value, low, high);
        if (!on_release) {
          if (edited) {
            descriptor.set(*settings, std::clamp(static_cast<double>(value), descriptor.min, descriptor.max));
            changed = true;
          }
        } else {
          // Pass Count: while held the slider shows the value under the mouse; the setting, NR's features and the
          // Pass 2+ rows below change once, on release (also after a Ctrl+click typed value is confirmed).
          if (edited) {
            state->pending_release = value;
            state->pending_key = descriptor.key;
          }
          if (gui::IsItemDeactivatedAfterEdit() && state->pending_key == descriptor.key && state->pending_release) {
            const int released = *std::exchange(state->pending_release, std::nullopt);
            if (released != applied) {
              descriptor.set(*settings, std::clamp(static_cast<double>(released), descriptor.min, descriptor.max));
              changed = true;
            }
          } else if (gui::IsItemDeactivated() && state->pending_key == descriptor.key) {
            state->pending_release.reset();  // released without an edit
          }
        }
        if (descriptor.key == "PassCount" && !view.frame_generation_warning.empty()) {
          gui::PushTextWrapPos(0.f);
          gui::TextDisabled("%s", view.frame_generation_warning.c_str());
          gui::PopTextWrapPos();
        }
        break;
      }
      case SettingKind::FLOAT: {
        const float shown_scale = static_cast<float>(descriptor.display_scale);
        float value = static_cast<float>(descriptor.get(*settings)) * shown_scale;
        const float high = static_cast<float>(SliderMax(descriptor)) * shown_scale;  // Ctrl+click may type past it (DescriptorMax)
        const bool always_clamp = ((descriptor.flags & setting_flags::SLIDER_ALWAYS_CLAMP) != 0u);
        const char* const format = (descriptor.slider_format != nullptr ? descriptor.slider_format : DEFAULT_SLIDER_FORMAT);
        const bool edited = grouped && UsesRailSlider(descriptor)
            ? widgets::RailSlider(label.c_str(), value, static_cast<float>(descriptor.min) * shown_scale, high, format, always_clamp ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None)
            : gui::SliderFloat(label.c_str(), &value, static_cast<float>(descriptor.min) * shown_scale, high, format);
        if (edited) {
          descriptor.set(*settings, static_cast<double>(value / shown_scale));
          changed = true;
        }
        const bool active = gui::IsItemActive();
        if (descriptor.key == "LocalStructure") {
          state->drag.local_structure = active;
        } else if (descriptor.key == "LocalTone") {
          state->drag.local_tone = active;
        } else if (descriptor.key == "ResolutionScale") {
          state->drag.resolution_scale = active;
        } else if (descriptor.key == "ResolutionScaleAfterDlss") {
          state->drag.resolution_scale_after_dlss = active;  // split chain: the work size waits for the release, as Setup's
        } else if (descriptor.key == "ResolutionScalePresent") {
          state->drag.resolution_scale_present = active;
        } else if (descriptor.key == "FaceProtection") {
          state->drag.face_protection = active;
        }
        if (descriptor.pass != 0u && (descriptor.flags & setting_flags::RESTARTS_HISTORY) != 0u) {
          state->drag.pass_slider |= active;
        }
        break;
      }
      case SettingKind::CHOICE: {
        int value = static_cast<int>(descriptor.get(*settings));
        if ((descriptor.flags & setting_flags::NETWORK_PRESET) != 0u && view.legacy_runtime) value = 1;
        if (descriptor.choices.size() <= 3u) {
          // User decision: at most three short choices draws as the RadioRow segmented control above,
          // never a Combo (Model, Upsampling, Stabilise, NR mask, UI correction and the like).
          std::array<const char*, 3> labels = {};
          std::array<std::string_view, 3> why = {};
          if (!disabled_reason.empty()) {
            why.fill(disabled_reason);  // e.g. Game state after NR, greyed while no DLSS placement can run
          }
          if (descriptor.key == "PresentWithFrameGen") why[static_cast<size_t>(Settings::FgPresent::BEFORE)] = view.before_fg_unavailable;
          if (descriptor.key == "VulkanNr" && disabled_reason.empty()) {
            // Native greyed where it cannot run, with why; the highlight is the route that runs. Direct3D 12 too.
            why[static_cast<size_t>(VulkanNrMode::NATIVE)] = view.vulkan_native_unavailable;
            why[static_cast<size_t>(VulkanNrMode::DIRECT3D_12)] = view.vulkan_d3d12_unavailable;
            if (view.vulkan_nr_running) {
              value = static_cast<int>(*view.vulkan_nr_running);
            }
          }
          std::array<std::string_view, 3> tips = {};
          tips.fill(descriptor.tooltip);
          for (size_t index = 0u; index < descriptor.choices.size(); ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          const std::span<const char* const> options(labels.data(), descriptor.choices.size());
          const std::span<const std::string_view> why_span(why.data(), descriptor.choices.size());
          const std::span<const std::string_view> tips_span(tips.data(), descriptor.choices.size());
          // Show FG placements in pipeline order while retaining the stored values used by existing presets.
          static constexpr size_t fg_order[]{static_cast<size_t>(Settings::FgPresent::OFF), static_cast<size_t>(Settings::FgPresent::BEFORE), static_cast<size_t>(Settings::FgPresent::AFTER)};
          const std::span<const size_t> order = descriptor.key == "PresentWithFrameGen" ? std::span<const size_t>{fg_order} : std::span<const size_t>{};
          if (const int clicked = widgets::SegmentedControl(grouped ? nullptr : label.c_str(), options, why_span, tips_span, value, order); clicked >= 0) {
            if (descriptor.key == "PresentWithFrameGen" && clicked == static_cast<int>(Settings::FgPresent::AFTER)) {
              state->confirm_present_stage = false;
              state->enable_warning_key = descriptor.key;
            } else {
              descriptor.set(*settings, static_cast<double>(clicked));
              changed = true;
            }
          }
        } else if (descriptor.key == "ResolutionMode" || descriptor.key == "ResolutionAfterDlss" || descriptor.key == "ResolutionPresent") {
          // The entries that cannot run now are greyed, each with its reason, and the preview is the mode that runs.
          // Split chain: a later stage group's Resolution greys the same entries (Match game reads the same DLSS render size); its preview is its
          // stored mode, since Setup's highlight is pass 1's.
          const bool setup_row = (descriptor.key == "ResolutionMode");
          std::array<const char*, 6> labels = {};
          for (size_t index = 0u; index < labels.size(); ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          const size_t stored_index = std::min(static_cast<size_t>(value), labels.size() - 1u);
          const size_t shown = (setup_row ? static_cast<size_t>(view.setup.resolution) : stored_index);
          const OptionState& stored = view.setup.resolution_options[stored_index];
          if (shown != stored_index && (stored.why.empty() || stored.temporary)) {  // a fixed cause never clears: no promise
            saved_note = SavedNote(labels[stored_index]);
          }
          if (gui::BeginCombo(label.c_str(), labels[shown])) {
            for (size_t index = 0u; index < labels.size(); ++index) {
              const OptionState& option = view.setup.resolution_options[index];
              // Setup's row greys every entry that cannot run, its highlight included (as before the split chain); a later stage's row
              // keeps its stored mode selectable, since its preview is that mode and not what runs.
              gui::BeginDisabled(!option.why.empty() && (setup_row || index != shown));
              if (gui::Selectable(labels[index], shown == index)) {
                descriptor.set(*settings, static_cast<double>(index));
                if (setup_row) {
                  state->setup_settle.Clicked(SetupRow::RESOLUTION, static_cast<uint32_t>(index));
                }
                changed = true;
              }
              gui::EndDisabled();
              if (!option.why.empty() && gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                gui::SetTooltip("Unavailable here: %s", std::string(option.why).c_str());
              }
            }
            gui::EndCombo();
          }
        } else {
          std::array<const char*, 8> labels = {};
          const size_t count = std::min(descriptor.choices.size(), labels.size());
          for (size_t index = 0u; index < count; ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          // A choice that does nothing on the shown image is greyed with why, the stored one included (as Setup's Resolution row does): Manual
          // exposure on an sRGB image. A row with no such choice keeps the plain combo.
          std::array<std::string_view, 8> why = {};
          if (descriptor.key == "InputExposure") why[static_cast<size_t>(color::InputExposure::MANUAL)] = white_unavailable;
          const size_t shown = std::min(static_cast<size_t>(std::max(value, 0)), count - 1u);
          if (std::ranges::all_of(why, [](std::string_view reason) { return reason.empty(); })) {
            if (gui::Combo(label.c_str(), &value, labels.data(), static_cast<int>(count))) {
              descriptor.set(*settings, static_cast<double>(value));
              changed = true;
            }
          } else if (gui::BeginCombo(label.c_str(), labels[shown])) {
            for (size_t index = 0u; index < count; ++index) {
              gui::BeginDisabled(!why[index].empty());
              if (gui::Selectable(labels[index], shown == index)) {
                descriptor.set(*settings, static_cast<double>(index));
                changed = true;
              }
              gui::EndDisabled();
              if (!why[index].empty() && gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                gui::SetTooltip("Unavailable here: %s", std::string(why[index]).c_str());
              }
            }
            gui::EndCombo();
          }
        }
        break;
      }
      case SettingKind::TEXT: {
        std::array<char, 1024> text = {};
        descriptor.get_text(*settings).copy(text.data(), text.size() - 1u);
        if (gui::InputText(label.c_str(), text.data(), text.size())) {
          descriptor.set_text(*settings, text.data());
          changed = true;
        }
        break;
      }
      case SettingKind::KEY: break;  // the Enable hotkey is drawn beside Enable
    }
  }
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && (!descriptor.tooltip.empty() || !disabled_reason.empty())) {
    std::string tooltip;
    if (!disabled_reason.empty()) {
      tooltip = std::format("Greyed: {}", disabled_reason);
      if (!descriptor.tooltip.empty()) {
        tooltip += "\n";
      }
    }
    tooltip += descriptor.tooltip;
    if (!saved_note.empty()) {
      tooltip += "\n" + saved_note;
    }
    gui::SetTooltip("%s", tooltip.c_str());
  }
  if (descriptor.key == "Mask" && !view.mask_note.empty()) {
    gui::TextDisabled("%s", view.mask_note.c_str());
  }
  if (descriptor.key == "UICorrection" && !view.ui_correction_note.empty()) {
    gui::TextDisabled("%s", view.ui_correction_note.c_str());
  }
  if (descriptor.key == "UseD3D9Ex") {
    const std::string& note = (view.d3d9ex_unavailable.empty() ? view.d3d9ex_readout : view.d3d9ex_unavailable);
    if (!note.empty()) {
      gui::TextDisabled("%s", note.c_str());
    }
  }
  if (descriptor.key == "VulkanNr" && view.vulkan_nr_unavailable.empty() && !view.vulkan_nr_note.empty()) {
    gui::TextDisabled("%s", view.vulkan_nr_note.c_str());
  }
  if (grouped && descriptor.key == "ResolutionScale") widgets::PercentTicks(descriptor.min, descriptor.max, slider_row);
  if (!captioned) changed |= DrawReset(descriptor, *settings, *state, grouped, view.setup);
  gui::EndDisabled();
  gui::PopID();
  return changed && !read_only;
}

bool DrawEnableWarning(const SettingDescriptor& descriptor, Settings* settings, OverlayState* state, bool explicit_dlss) {
  const std::string title = std::format("{} - performance warning", descriptor.label);
  if (state->enable_warning_key == descriptor.key) {
    state->enable_warning_key = {};
    gui::OpenPopup(title.c_str());
  }
  if (!gui::IsPopupOpen(title.c_str())) return false;
  gui::SetNextWindowSize(ImVec2(gui::GetFontSize() * WARNING_POPUP_WIDTH_EM, 0.f), ImGuiCond_Appearing);
  bool open = true;
  if (!gui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) return false;
  gui::TextWrapped("%s", descriptor.enable_warning.data());
  gui::Separator();
  const bool cancel = gui::Button(descriptor.key == "PresentWithFrameGen" ? "Keep current mode" : "Keep disabled");
  gui::SetItemDefaultFocus();
  gui::SameLine();
  const bool enable = gui::Button("Enable anyway");
  if (cancel || enable || gui::IsKeyPressed(ImGuiKey_Escape)) {
    gui::CloseCurrentPopup();
  }
  if (enable) {
    descriptor.set(*settings, 1.0);
    if (descriptor.key == "PresentWithFrameGen" && state->confirm_present_stage) {
      SetSourcePick(settings, SourcePick::PRESENT, explicit_dlss);
      state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(SourcePick::PRESENT));
    }
  }
  gui::EndPopup();
  return enable;
}

// Split chain: pass n's Runs at row, as the NR stage row but a combo (four long choices): a choice that cannot run is greyed with why,
// the preview is where the pass runs, and the stored pick is named while it runs elsewhere. Where every pass runs at one stage the whole row is greyed.
// Not folded into DrawRow: its choices' states come from Setup's pass rows, not from the schema.
bool DrawPassStageRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state) {
  const size_t later = descriptor.pass - 2u;
  const PassStageRow& row = view.setup.pass_stages[later];
  const std::string label(descriptor.label);
  gui::PushID(label.c_str());
  gui::BeginDisabled(!view.setup.split_capable || view.setup.stopped);  // greyed with Setup's other rows once NR stopped
  const auto stored = static_cast<size_t>(settings->passes[later].stage);
  const auto shown = static_cast<size_t>(row.shown);
  const std::array names{descriptor.choices[0].data(), NR_STAGE_NAMES[0], NR_STAGE_NAMES[1], view.SharedPresentName(*settings)};
  bool changed = false;
  gui::AlignTextToFramePadding();
  gui::TextUnformatted(label.c_str());
  changed |= DrawReset(descriptor, *settings, *state, true, view.setup);
  widgets::NextGap(style::LabelGap());
  if (gui::BeginCombo("##stage", names[shown])) {  // literals and static strings: NUL-terminated
    for (size_t index = 0u; index < row.options.size(); ++index) {
      const PassStageOption& option = row.options[index];
      gui::BeginDisabled(!option.why.empty());
      if (gui::Selectable(names[index], shown == index) && index != stored) {
        descriptor.set(*settings, static_cast<double>(index));
        changed = true;
      }
      gui::EndDisabled();
      if (!option.why.empty() && gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        gui::SetTooltip("Unavailable here: %s", option.why.c_str());
      }
    }
    gui::EndCombo();
  }
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    std::string tooltip(descriptor.tooltip);
    if (!view.setup.split_capable) {
      tooltip = std::format("Greyed: {}\n{}", ONE_STAGE_REASON, descriptor.tooltip);
    } else if (shown != stored && (row.options[stored].why.empty() || row.options[stored].temporary)) {  // a fixed cause never clears: no promise
      tooltip += "\n" + SavedNote(names[stored]);
    }
    gui::SetTooltip("%s", tooltip.c_str());
  }
  gui::EndDisabled();
  gui::PopID();
  return changed;
}

// The Motion vectors row -- Off / DLSS / Launchpad / Lumenite -- from Setup, as the NR stage row.
bool DrawMotionRow(const OverlayView& view, Settings* settings, OverlayState* state) {
  gui::PushID("MotionVectors");
  static constexpr std::array<const char*, 4> OPTIONS = {"Off", "DLSS", "Launchpad", "Lumenite"};
  std::array<std::string_view, 4> tips = {
      "No motion vectors, anywhere.",
      "The game's own DLSS motion vectors: bound directly inside the frame, and copied for Present when "
      "NR stage is Present.",
      "iMMERSE Launchpad on, and Uplift.fx's Uplift technique below it. The presented image only.",
      "LumeniteFX's Kernel on, and Uplift.fx's Uplift technique below it. The presented image only. Auto uses Launchpad when both are on.",
  };
  std::array<std::string_view, 4> why = {};
  for (size_t index = 0u; index < why.size(); ++index) {
    why[index] = view.setup.motion_options[index].why;
  }
  const MotionPick preferred = MotionPickOf(*settings);
  const int shown = static_cast<int>(view.setup.motion);
  std::string saved_tip;
  const OptionState& stored_motion = view.setup.motion_options[static_cast<size_t>(preferred)];
  if (view.setup.motion != preferred && (stored_motion.why.empty() || stored_motion.temporary)) {  // a fixed cause never clears: no promise
    const std::string_view stored = (settings->motion_vectors == MotionVectorSource::AUTO ? std::string_view("Auto") : std::string_view(OPTIONS[static_cast<size_t>(preferred)]));
    saved_tip = std::format("{}\n{}", tips[static_cast<size_t>(shown)], SavedNote(stored));
    tips[static_cast<size_t>(shown)] = saved_tip;
  }
  gui::AlignTextToFramePadding();
  gui::TextUnformatted("Motion vectors");
  bool changed = DrawReset(*FindSetting("MotionVectors"), *settings, *state, true, view.setup);
  widgets::NextGap(style::LabelGap());
  if (const int clicked = widgets::SegmentedControl(nullptr, OPTIONS, why, tips, shown); clicked >= 0) {
    SetMotionPick(settings, static_cast<MotionPick>(clicked));
    state->setup_settle.Clicked(SetupRow::MOTION, static_cast<uint32_t>(clicked));
    changed = true;
  }
  gui::PopID();
  return changed;
}

void DrawWorkingSize(const OverlayView& view, const Settings& settings) {
  nr::Size input = view.first_stage_input;
  if (input.Empty() && view.setup.stage == SourcePick::PRESENT) input = view.output_size;
  nr::Size size = view.first_stage_canvas;
  if (settings.resolution_mode == ResolutionMode::CUSTOM && !input.Empty()) {
    const double scale = settings.resolution_scale / 100.0;
    size = sources::WorkSize(input.width * scale, input.height * scale, input);
    if (view.setup.stage == SourcePick::BEFORE_UPSCALING) size = sources::CanvasSize(size);
  } else if (size.Empty() && settings.resolution_mode == ResolutionMode::FULL && view.setup.stage == SourcePick::PRESENT) {
    size = input;
  }
  const bool pending = !size.Empty() && !view.first_stage_canvas.Empty() && size != view.first_stage_canvas;
  widgets::GroupRule();
  gui::TextUnformatted(pending ? "Target NR size" : "NR working size");
  const std::string dimensions = size.Empty() ? gui::Display("Unknown") : std::format("{} × {}", size.width, size.height);
  const float remaining = gui::GetContentRegionAvail().x - ImGui::CalcTextSize(dimensions.c_str()).x;
  const float x = gui::GetCursorPosX() + remaining;
  if (remaining >= gui::CalcTextSize(pending ? "Target NR size" : "NR working size").x + gui::GetStyle().ItemSpacing.x) {
    gui::SameLine(); gui::SetCursorPosX(x);
  }
  ImGui::TextUnformatted(dimensions.c_str());
  if (!view.output_size.Empty()) {
    const std::string output = std::format("{}  {} × {}", gui::Display("Output"), view.output_size.width, view.output_size.height);
    widgets::NextGap(style::LabelGap());
    gui::TextDisabled("%s", output.c_str());
  }
}

bool DrawSetupGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  widgets::PairedGroups("##setup_groups", [&] {
    widgets::GroupHeading("Frame inputs");
    changed |= DrawMotionRow(view, settings, state);
    changed |= DrawRow(*FindSetting("MaskBlendMode"), view, settings, state, true);
    widgets::NextGap(style::BlockGap());
    widgets::GroupHeading("Passes");
    changed |= DrawRow(*FindSetting("PassCount"), view, settings, state, true);
  }, [&] {
    widgets::GroupHeading("NR processing", "Pass 1 defaults");
    changed |= DrawRow(*FindSetting("Preset"), view, settings, state, true);
    changed |= DrawRow(*FindSetting("ResolutionMode"), view, settings, state, true);
    changed |= DrawRow(*FindSetting("ResolutionScale"), view, settings, state, true);
    DrawWorkingSize(view, *settings);
    changed |= DrawRow(*FindSetting("Upsampling"), view, settings, state, true);
  });
  return changed;
}

bool DrawLookGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  static constexpr std::array<const char*, 2> options{"Mine", "Built-in"};
  static constexpr std::array<std::string_view, 2> why{}, tips{"", "The built-in look, read-only; your own settings are kept."};
  {
    if (gui::BeginTable("##compare", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
      gui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, gui::CalcTextSize("Compare").x);
      const float choices_width = gui::CalcTextSize(options[0]).x + gui::CalcTextSize(options[1]).x + 4.f * gui::GetStyle().FramePadding.x + gui::GetStyle().ItemInnerSpacing.x;
      gui::TableSetupColumn("##choices", ImGuiTableColumnFlags_WidthFixed, std::max(choices_width, COMPARE_CHOICES_WIDTH_EM * gui::GetFontSize()));
      gui::TableNextRow();
      gui::TableSetColumnIndex(0);
      gui::AlignTextToFramePadding();
      gui::TextUnformatted("Compare");
      gui::TableSetColumnIndex(1);
      gui::PushID("Compare");
      const int pick = widgets::SegmentedControl(nullptr, options, why, tips, state->defaults_view ? 1 : 0);
      if (pick >= 0) state->defaults_view = pick == 1;
      gui::PopID();
      gui::EndTable();
    }
  }

  bool changed = false;
  const auto row = [&](const char* key, bool grouped = true) { changed |= DrawRow(*FindSetting(key), view, settings, state, grouped); };
  widgets::NextGap(style::BlockGap());
  widgets::PairedGroups("##look_groups", [&] {
    widgets::GroupHeading("Image", "Pass 1");
    row("Style");
    row("Intensity");
    row("LocalStructure");
    row("LocalTone");
  }, [&] {
    widgets::GroupHeading("Characters");
    row("AutoMask");
    row("SkinStructure");
    widgets::GroupRule();
    row("KeepFaces");
    row("FaceProtection");
    row("LightingScale");
  });
  return changed;
}

// The VRAM line of the Details header and of the working status card.
std::string VramLine(const OverlayView& view) {
  if (view.runtime_bytes) {
    return std::format("VRAM: NR runtime {:.0f} MiB, Uplift intermediates {:.0f} MiB", static_cast<double>(*view.runtime_bytes) / BYTES_PER_MIB,
                       static_cast<double>(view.intermediate_bytes) / BYTES_PER_MIB);
  }
  return std::format("VRAM: Uplift intermediates {:.0f} MiB", static_cast<double>(view.intermediate_bytes) / BYTES_PER_MIB);
}

}  // namespace

void FinishSetup(OverlayView* view, OverlayState* state, const void* device, const SetupFacts& facts, std::chrono::steady_clock::time_point now) {
  if (state->setup_device != device) {
    state->setup_device = device;
    state->setup_settle.Reset();
    state->activity_since = {};
    state->activity_seconds = 0.0;
  }
  view->first_stage_input = facts.frame;
  view->first_stage_canvas = facts.canvas.Empty() ? facts.work : facts.canvas;
  view->setup = state->setup_settle.Update(ResolveSetup(facts), now);
  if (view->card.working) {
    view->live = BuildLiveCard(facts, view->setup);
  }
}

namespace {
void DrawDetails(const OverlayView& view, const Settings& settings, OverlayState& state) {
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, style::LabelGap() * 0.5f});
  const auto fact = [](const char* key, const std::string& value) {
    gui::TableNextRow();
    gui::TableSetColumnIndex(0); gui::TextDisabled("%s", gui::Display(key).c_str());
    gui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", value.c_str());
  };
  if (gui::BeginTable("##runtime_facts", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp)) {
    gui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, std::max({gui::CalcTextSize("Backend").x, gui::CalcTextSize("Display output").x, gui::CalcTextSize("Frame generation").x}) + style::BlockGap());
    gui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    if (!view.details_backend.empty()) fact("Backend", gui::Display(view.details_backend));
    if (!view.output_size.Empty()) {
      const char* space = "";
      switch (view.output_space) {
        case color::ColorSpace::SRGB_NONLINEAR: space = "sRGB"; break;
        case color::ColorSpace::EXTENDED_SRGB_LINEAR: space = "scRGB"; break;
        case color::ColorSpace::HDR10_ST2084: space = "HDR10"; break;
        case color::ColorSpace::HDR10_HLG: space = "HLG"; break;
        default: break;
      }
      fact("Display output", std::format("{} × {}  {}", view.output_size.width, view.output_size.height, space));
    }
    if (view.frame_generation_active || !view.frame_generation_line.empty())
      fact("Frame generation", gui::Display(view.frame_generation_active ? "On" : "Off"));
    gui::EndTable();
  }

  gui::PopStyleVar();
  widgets::NextGap(style::BlockGap());
  const bool compact = gui::GetContentRegionAvail().x < style::Units(DETAILS_COLUMNS_MIN_WIDTH_UNITS);
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, {style::RowGap(), style::LabelGap()});
  if (gui::BeginTable("##executed_passes", compact ? 2 : 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
    const auto column = [](const char* label, float weight) { gui::TableSetupColumn(gui::Display(label).c_str(), ImGuiTableColumnFlags_WidthStretch, weight); };
    column("Pass", 0.5f);
    if (compact) column("NR evaluation", 3.f);
    else {
      column("Ran at", 1.15f); column("Network / style", 1.3f); column("NR size", 1.f); column("Motion used", 1.25f);
    }
    if (!compact) gui::TableHeadersRow();
    bool any = false;
    for (size_t i = 0; i < view.stage_runs.size(); ++i) {
      const auto& group = view.stage_runs[i];
      for (uint32_t n = 0; n < group.count && group.first + n < view.pass_models.size(); ++n) {
        const uint32_t pass = group.first + n;
        const auto& model = view.pass_models[pass];
        const char* network = "Unconfirmed";
        switch (model.network) {
          case nr::RecordedNetwork::LEGACY: network = "Legacy"; break;
          case nr::RecordedNetwork::SMALL_FP8: network = "Small FP8"; break;
          case nr::RecordedNetwork::OTHER: network = "Other network"; break;
          default: break;
        }
        const std::string name = model.style < 3u ? std::format("{} / {}", gui::Display(network), static_cast<char>('A' + model.style)) : gui::Display(network);
        const char* motion = "None";
        switch (group.motion) {
          case sources::MotionSource::DIRECT: motion = "DLSS"; break;
          case sources::MotionSource::PRESENT_COPY: motion = "DLSS copy"; break;
          case sources::MotionSource::LAUNCHPAD: motion = view.lumenite_motion ? "Lumenite" : "Launchpad"; break;
          default: break;
        }
        gui::TableNextRow();
        gui::TableNextColumn(); gui::Text("P%u", pass + 1u);
        const auto record = [compact](const char* label, const std::string& value) {
          if (!compact) gui::TableNextColumn();
          if (compact) ImGui::TextWrapped("%s  %s", gui::Display(label).c_str(), value.c_str());
          else ImGui::TextWrapped("%s", value.c_str());
        };
        if (compact) gui::TableNextColumn();
        record("Ran at", gui::Display(NR_STAGE_NAMES[i]));
        record("Network / style", name);
        record("NR size", group.resolution.Empty() ? gui::Display("Unknown") : std::format("{} × {}", group.resolution.width, group.resolution.height));
        record("Motion used", gui::Display(motion));
        any = true;
      }
    }
    gui::EndTable();
    if (!any) gui::TextDisabled("No pass recorded.");
  }
  gui::PopStyleVar();
  if (!view.details_note.empty()) gui::TextWrapped("%s", view.details_note.c_str());
  for (const auto& group : view.stage_runs) {
    if (group.skip.empty() || group.planned <= group.count) continue;
    const uint32_t first = group.first + group.count + 1u, last = group.first + group.planned;
    const std::string passes = first == last ? std::format("P{}", first) : std::format("P{}-{}", first, last);
    ImGui::TextWrapped("%s: %s", passes.c_str(), gui::Display(group.skip).c_str());
  }
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - state.activity_since).count();
  if (view.activity) {
    const auto& v = *view.activity;
    const auto& base = state.activity_start;
    if (state.activity_mode != settings.fg_present || elapsed > 2.5 || v.real < base.real || v.reshade < base.reshade || v.passes < base.passes) {
      state.activity_since = now;
      state.activity_start = v;
      state.activity_seconds = 0.0;
      state.activity_mode = settings.fg_present;
    } else if (elapsed >= 1.0) {
      state.activity_delta = {v.real-base.real, v.reshade-base.reshade, v.passes-base.passes};
      state.activity_seconds = elapsed;
      state.activity_start = v;
      state.activity_since = now;
    }
    if (state.activity_seconds > 0 && state.activity_delta.real > 0) {
      const auto& delta = state.activity_delta;
      const auto line = std::format("{} {:.1f} s: {} {} / {} {} / {} {}", gui::Display("FG activity over"), state.activity_seconds,
          delta.real, gui::Display("real frames"), delta.reshade, gui::Display("ReShade Presents"), delta.passes, gui::Display("Present-pass runs"));
      ImGui::TextWrapped("%s", line.c_str());
    }
  } else {
    state.activity_since = {};
    state.activity_seconds = 0.0;
  }
  gui::TextWrapped("%s", VramLine(view).c_str());
}
}  // namespace

void OverlayView::AddStartupLocks(const Settings& settings, bool vulkan, bool d3d9) {
  if (d3d9 && (settings.startup_latches & Settings::LATCH_D3D9EX)) {
    safety_locks.push_back({"Direct3D 9Ex", "The previous 9Ex startup ended before the first frame.", &Settings::use_d3d9ex});
  }
  if (vulkan && (settings.startup_latches & Settings::LATCH_VK_DEVICE)) {
    safety_locks.push_back({"Vulkan device adjustments", "The previous startup ended before the adjusted device presented a frame.", &Settings::adjust_vulkan_devices});
  }
  if (vulkan && (settings.startup_latches & Settings::LATCH_VK_NGX)) {
    safety_locks.push_back({"Vulkan NGX device extensions", "The previous startup with these extensions ended before the first frame.", &Settings::adjust_vulkan_devices_for_ngx});
  }
}

static bool DrawOverlayContent(const OverlayView& view, Settings* settings, OverlayState* state) {
  state->drag.pass_slider = false;  // the rows OR into it
  state->language_changed = false;
  const gui::LanguageScope language_scope(settings->language);
  bool changed = false;

  changed |= DrawRow(*FindSetting(ENABLED_KEY), view, settings, state);
  gui::SameLine();

  // 1.1.0 (a player's report): Esc also closes ReShade's overlay, so it cannot be the way to clear the hotkey. A right-click on the button clears it
  // (capturing or not); Esc only cancels a capture. A capture whose overlay closed ends by itself (ExpireKeyCapture), so the hotkey never stays off.
  if (state->capturing_key) {
    state->capture_seen = std::chrono::steady_clock::now();
    if (gui::CalcTextSize("Press a key (right-click clears)").x + 2.f * gui::GetStyle().FramePadding.x > gui::GetContentRegionAvail().x) gui::NewLine();
    gui::Button("Press a key (right-click clears)");
    if (gui::IsItemClicked(ImGuiMouseButton_Right)) {
      settings->enable_key = 0u;
      state->capturing_key = false;
      changed = true;
    } else if (view.last_key_pressed != 0u) {
      if (view.last_key_pressed != VK_ESCAPE) {
        settings->enable_key = view.last_key_pressed;
        changed = true;
      }
      state->capturing_key = false;
      state->key_captured_this_frame = true;
    }
  } else {
    std::string key_label = "Hotkey: none";
    if (settings->enable_key >= VK_F1 && settings->enable_key <= VK_F24) {
      key_label = std::format("Hotkey: F{}", settings->enable_key - VK_F1 + 1u);
    } else if (settings->enable_key != 0u) {
      std::array<wchar_t, 64> key_name = {};
      const auto scan_code = static_cast<LONG>(MapVirtualKeyW(settings->enable_key, MAPVK_VK_TO_VSC) << 16u);
      const int length = GetKeyNameTextW(scan_code, key_name.data(), static_cast<int>(key_name.size()));
      if (length > 0) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, key_name.data(), length, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, key_name.data(), length, utf8.data(), bytes, nullptr, nullptr);
        key_label = std::format("Hotkey: {}", utf8);
      } else {
        key_label = std::format("Hotkey: key {:#04x}", settings->enable_key);
      }
    }
    if (gui::CalcTextSize(key_label.c_str()).x + 2.f * gui::GetStyle().FramePadding.x > gui::GetContentRegionAvail().x) gui::NewLine();
    if (gui::Button(key_label.c_str())) {
      state->capturing_key = true;
      state->capture_seen = std::chrono::steady_clock::now();
    } else if (gui::IsItemClicked(ImGuiMouseButton_Right) && settings->enable_key != 0u) {
      settings->enable_key = 0u;
      changed = true;
    }
    if (gui::IsItemHovered()) {
      gui::SetTooltip("Click, then press a key. Right-click clears it.");
    }
  }

  gui::SameLine();
  const float min_language_width = ImGui::CalcTextSize(settings->language == Language::ENGLISH ? "English" : "简体中文 / Chinese").x + gui::GetFrameHeight() + style::RowGap();
  if (gui::GetContentRegionAvail().x < min_language_width) gui::NewLine();
  const float language_width = std::min(gui::GetContentRegionAvail().x, std::max(min_language_width, LANGUAGE_COMBO_WIDTH_EM * gui::GetFontSize()));
  gui::SetCursorPosX(gui::GetCursorPosX() + gui::GetContentRegionAvail().x - language_width);
  gui::SetNextItemWidth(language_width);
  int selected_language = static_cast<int>(settings->language);
  const char* languages[] = {"English", "简体中文 / Chinese"};
  if (ImGui::Combo("##UpliftLanguage", &selected_language, languages, 2)) {
    FindSetting("Language")->set(*settings, selected_language);
    state->language_changed = true;
  }
  if (ImGui::IsItemHovered()) {
    const gui::LanguageScope tooltip_language(settings->language);
    gui::SetTooltip("Choose language. If Chinese not visible, choose a Chinese-capable Global font in ReShade Settings (for example C:\\Windows\\Fonts\\msyh.ttc)");
  }

  widgets::NextGap(style::BlockGap());
  if (!view.safety_locks.empty()) {
    const ImVec4 red = style::SafetyRed();
    const ImVec2 warning_start = gui::GetCursorScreenPos();
    const float inset = style::PanelInset();
    widgets::Inset("##safety_locks", {inset, inset}, [&] {
      gui::PushStyleColor(ImGuiCol_Text, red);
      gui::SeparatorText("SAFETY LATCH ACTIVE");
      gui::PopStyleColor();
      for (const auto& lock : view.safety_locks) {
        gui::PushID(lock.feature.data());
        gui::TextColored(red, "%s", lock.feature.data());
        gui::TextWrapped("%s", lock.reason.c_str());
        if (lock.setting != nullptr) {
          if (settings->*(lock.setting) == lock.restore_value) {
            gui::TextWrapped("Latch cleared for next launch. Restart the game to retry.");
          } else if (gui::Button("Clear latch for next launch")) {
            settings->*(lock.setting) = lock.restore_value;
            changed = true;
          }
        } else if (lock.retry_now) {
          if (gui::Button("Retry now")) state->retry_now = true;
        } else {
          gui::TextWrapped("Restart the game to retry this feature.");
        }
        gui::PopID();
      }
    }, gui::GetColorU32(style::Tint(red, 0.075f)));
    const float bottom = gui::GetCursorScreenPos().y - gui::GetStyle().ItemSpacing.y;
    gui::GetWindowDrawList()->AddRectFilled(warning_start, {warning_start.x + 0.75f * style::LabelGap(), bottom}, gui::GetColorU32(red));
    widgets::NextGap(style::BlockGap());
  }

  const StatusCard& card = view.card;
  const float card_inset = style::PanelInset();
  gui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {style::RowGap(), style::LabelGap()});
  widgets::Inset("##status_card", {card_inset, card_inset}, [&] {
    if (card.working) {
      // Green while NR works, three rows that say what Setup highlights (keys muted on the left, values in normal text from one column),
      // one muted Why line for a temporary fallback (else the note), and the footer. The height above is unchanged; whatever does not fit is left out.
      gui::TextColored(gui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", card.title.c_str());
      const float value_x = gui::GetCursorPosX() + gui::CalcTextSize("Motion vectors").x + 2.f * gui::GetStyle().ItemSpacing.x;
      const auto row = [value_x](const char* key, const std::string& value) {
        gui::TextDisabled("%s", key);
        gui::SameLine(value_x);
        gui::TextWrapped("%s", value.c_str());
      };
      std::string recorded;
      for (size_t i = 0; i < view.stage_runs.size(); ++i) {
        if (view.stage_runs[i].count == 0) continue;
        if (!recorded.empty()) recorded += " · ";
        recorded += gui::Display(NR_STAGE_NAMES[i]);
      }
      row("Running at", recorded.empty() ? view.live.stage : recorded);
      row("Motion vectors", view.live.motion);
      if (!view.details_backend.empty()) row("Backend", view.details_backend);
      row("Resolution", view.live.resolution);
      if (!view.setup.why.empty()) {
        const std::string why = "Why: " + view.setup.why;
        gui::PushStyleColor(ImGuiCol_Text, gui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        gui::TextWrapped("%s", why.c_str());
        gui::PopStyleColor();
        if (gui::IsItemHovered()) {
          gui::SetTooltip("%s", why.c_str());
        }
      } else {
        if (!view.live.note.empty()) gui::TextDisabled("%s", view.live.note.c_str());
        if (!card.before_fg_note.empty()) gui::TextDisabled("%s", card.before_fg_note.c_str());
      }
      if (!view.live.footer.empty()) gui::TextDisabled("%s", view.live.footer.c_str());
    } else {
      gui::TextColored(gui::GetStyleColorVec4(ImGuiCol_PlotHistogram), "%s", card.title.c_str());
      if (!card.reason.empty()) {
        gui::TextWrapped("%s", card.reason.c_str());
      }
      if (!card.before_fg_note.empty()) gui::TextDisabled("%s", card.before_fg_note.c_str());
      for (const std::string& fix : card.fixes) {
        gui::BulletText("%s", fix.c_str());
      }
    }
    switch (card.button) {
      case CardButton::NONE: break;
      case CardButton::TURN_ON:
        if (gui::Button("Turn on")) {
          settings->enabled = true;
          changed = true;
        }
        break;
      case CardButton::RETRY_NOW:
        if (gui::Button("Retry now")) state->retry_now = true;
        break;
      case CardButton::CLEAR_LATCH:  // Each blocked feature has its own explicit recovery action above the card.
        break;
    }
  }, style::GroupSurface(), style::Units(STATUS_CARD_MIN_HEIGHT_UNITS));
  gui::PopStyleVar();

  const auto rows = [&](std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) changed |= DrawRow(*FindSetting(key), view, settings, state, true);
  };
  // The NR stage and Motion vectors toggles, in pipeline order, at the top
  // of the panel -- the decisions that shape everything else, because they determine which later controls apply.
  widgets::NextGap(style::SectionGap());
  const bool placement_changed = SectionChanged(*settings, SettingSection::PLACEMENT);
  if (widgets::SectionHeading(placement_changed ? "Setup (changed)" : "Setup", "placement", placement_changed && !state->defaults_view)) {
    ResetSection(settings, SettingSection::PLACEMENT);
    const Settings defaults = DefaultSettings();  // the three highlights follow at once, each to the best option that can run (the stage first)
    state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(SourcePickOf(defaults, view.dlss_explicit)), view.setup);
    state->setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPickOf(defaults)), view.setup, true);
    state->setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(defaults.resolution_mode), view.setup);
    changed = true;
  }
  widgets::NextGap(style::BlockGap());
  changed |= DrawStagePicker(view, settings, state);
  widgets::NextGap(style::BlockGap());
  changed |= DrawSetupGroups(view, settings, state);

  // Compare (Mine/Built-in) is the first row of Look.
  widgets::NextGap(style::SectionGap());
  const bool look_changed = SectionChanged(*settings, SettingSection::LOOK);
  if (widgets::SectionHeading(look_changed ? "Look (changed)" : "Look", "look", look_changed && !state->defaults_view)) {
    ResetSection(settings, SettingSection::LOOK);
    changed = true;
  }
  widgets::NextGap(style::BlockGap());
  changed |= DrawLookGroups(view, settings, state);
  if (settings->pass_count > 1u) {
    widgets::NextGap(style::SectionGap());
    gui::SeparatorText("Passes");
    widgets::NextGap(style::BlockGap());
  }
  // Pass 2..pass_count, each its own collapsible tree, so the per-pass rows do not
  // flood the panel for the common one-pass case. The tree's own label shows its state.
  // Split chain: Runs at first; on the first pass of a later stage group, that stage's Resolution (and Custom scale) and, while Keep
  // faces is on, its Keep faces here; while the chain is split the tree's label names where the pass runs.
  for (uint32_t pass = 2u; pass <= settings->pass_count; ++pass) {
    const bool follows = settings->passes[pass - 2u].follow_pass1;
    const PassStageRow& stage_row = view.setup.pass_stages[pass - 2u];
    const char* stage = view.RecordedStageName(pass - 1u);
    if (!stage) stage = stage_row.runs == SourcePick::PRESENT ? view.SharedPresentName(*settings) : StageLabel(stage_row.runs);
    const std::string summary = gui::Display(stage) + " · " + gui::Display(follows ? "Same look as pass 1" : "Own look");
    const std::string label = std::format("{}###pass{}", gui::Display(std::format("Pass {}", pass)), pass);
    if (widgets::Disclosure(label.c_str(), summary.c_str())) {
      gui::PushID(static_cast<int>(pass));
      widgets::Inset("##pass_body", {style::PanelInset(), style::PanelInset()}, [&] {
        widgets::PairedGroups("##pass_groups", [&] {
          widgets::GroupHeading("Placement and network");
          changed |= DrawPassStageRow(*FindSetting(std::format("Pass{}Stage", pass)), view, settings, state);
          changed |= DrawRow(*FindSetting(std::format("Pass{}Preset", pass)), view, settings, state, true);
          if (stage_row.starts_group && stage_row.runs != SourcePick::BEFORE_UPSCALING) {
            if (stage_row.runs == SourcePick::AFTER_DLSS) rows({"ResolutionAfterDlss", "ResolutionScaleAfterDlss", "KeepFacesAfterDlss"});
            else rows({"ResolutionPresent", "ResolutionScalePresent", "KeepFacesPresent"});
          }
        }, [&] {
          widgets::GroupHeading("Pass look");
          for (const SettingDescriptor& descriptor : SettingsSchema()) {
            if (descriptor.pass == pass && (descriptor.flags & (setting_flags::PASS_STAGE | setting_flags::NETWORK_PRESET)) == 0u)
              changed |= DrawRow(descriptor, view, settings, state, true);
          }
        }, style::WellSurface());
      }, style::GroupSurface());
      gui::PopID();
    }
  }

  widgets::NextGap(style::SectionGap());
  bool reset = false;
  const bool shaping_changed = SectionChanged(*settings, SettingSection::RESULT) || SectionChanged(*settings, SettingSection::LIMITS);
  const bool shaping_enabled = state->defaults_view ? DefaultSettings().look.enabled : settings->look.enabled;
  gui::SetNextItemOpen(shaping_enabled, ImGuiCond_Once);
  const bool shaping = widgets::Disclosure(shaping_changed ? "Result shaping (changed)###shaping" : "Result shaping###shaping",
                                           shaping_enabled ? "On" : "Off", "shaping", shaping_changed && !state->defaults_view ? &reset : nullptr);
  if (reset) {
    ResetSection(settings, SettingSection::RESULT);
    ResetSection(settings, SettingSection::LIMITS);
    changed = true;
  }
  if (shaping) widgets::Panel("##shaping_body", [&] {
    rows({"ShapeResult"});
    if (shaping_enabled) {
      widgets::NextGap(style::BlockGap());
      widgets::PairedGroups("##result_groups", [&] {
        widgets::GroupHeading("Strength and colour");
        rows({"EditStrength", "Brighten", "Darken", "ResultColor", "HueShift"});
      }, [&] {
        widgets::GroupHeading("Tonal range");
        rows({"Shadows", "Midtones", "Highlights"});
      }, style::WellSurface());
      widgets::NextGap(style::BlockGap());
      widgets::PairedGroups("##limit_groups", [&] {
        widgets::GroupHeading("Limits");
        rows({"MaxBrighten", "MaxDarken", "MaxColorChange"});
      }, [&] {
        widgets::GroupHeading("Detail and stability");
        rows({"LargeScaleTone", "FineDetail", "DetailRadius", "HaloSuppression", "Stabilize", "StabilizeMs", "StabilizeDetail"});
      }, style::WellSurface());
    }
  });

  reset = false;
  const bool mask_changed = SectionChanged(*settings, SettingSection::MASK);
  const bool mask = widgets::Disclosure(mask_changed ? "Mask and HUD (changed)###mask" : "Mask and HUD###mask",
                                        nullptr, "mask", mask_changed && !state->defaults_view ? &reset : nullptr);
  if (reset) { ResetSection(settings, SettingSection::MASK); changed = true; }
  if (mask) widgets::Panel("##mask_body", [&] {
    widgets::PairedGroups("##mask_groups", [&] { widgets::GroupHeading("Mask"); rows({"Mask"}); },
                                         [&] { widgets::GroupHeading("HUD"); rows({"UICorrection"}); }, style::WellSurface());
  });

  reset = false;
  const bool fixes_changed = SectionChanged(*settings, SettingSection::FIXES_COLOR) || SectionChanged(*settings, SettingSection::FIXES_GUIDES);
  const bool fixes = widgets::Disclosure(fixes_changed ? "Fixes (changed)###fixes" : "Fixes###fixes", "Colour · Exposure · Motion",
                                         "fixes", fixes_changed && !state->defaults_view ? &reset : nullptr);
  if (reset) {
    ResetSection(settings, SettingSection::FIXES_COLOR);
    ResetSection(settings, SettingSection::FIXES_GUIDES);
    changed = true;
  }
  if (fixes) widgets::Panel("##fixes_body", [&] {
    gui::TextDisabled("Only change these if you know what you are doing.");
    // Manual exposure is Diffuse white alone: its row moves under Input exposure then (DiffuseWhiteInExposureGroup), so the control sits with the choice.
    widgets::PairedGroups("##colour_groups", [&] {
      widgets::GroupHeading("Colour");
      if (DiffuseWhiteInExposureGroup(*settings)) rows({"Encoding", "TransferStrength", "ColorStrength"});
      else rows({"Encoding", "DiffuseWhiteNits", "TransferStrength", "ColorStrength"});
    }, [&] {
      widgets::GroupHeading("Exposure");
      rows({"InputExposure"});
      if (DiffuseWhiteInExposureGroup(*settings)) rows({"DiffuseWhiteNits"});
      rows({"AutoExposureMode", "AutoExposureBlend", "ExposureAdapt", "AdaptBrighterStops", "AdaptDarkerStops"});
    }, style::WellSurface());
    widgets::NextGap(style::BlockGap());
    widgets::PairedGroups("##guide_groups", [&] {
      widgets::GroupHeading("Colour conversion");
      rows({"LinearUnitNits", "SourcePrimaries", "NeuralTransfer", "ChromaClamp", "NearBlackGuard"});
    }, [&] {
      widgets::GroupHeading("Motion and game state");
      rows({"MotionScaleX", "MotionScaleY", "ChainedHistory", "StateRestore"});
    }, style::WellSurface());
  });

  if (widgets::Disclosure("Advanced", "Routing · Runtime · Diagnostics")) widgets::Panel("##advanced_body", [&] {
    widgets::PairedGroups("##advanced_groups", [&] {
      widgets::GroupHeading("Routing");
      rows({"PresentWithFrameGen", "BeforeFGMotion", "VulkanNr", "AllowCrossQueuePasses", "UseD3D9Ex"});
    }, [&] {
      widgets::GroupHeading("Memory and recovery");
      rows({"VramCheck", "BudgetMarginMB", "GraceSeconds", "AutoResume", "AutoRetry"});
    }, style::WellSurface());
    widgets::NextGap(style::BlockGap());
    widgets::Panel("##runtime_group", [&] {
      widgets::GroupHeading("Runtime and logging");
      rows({"SnippetPath", "NgxHooks", "ForeignNr", "LogLevel", "ShowNvIndicator"});
    }, style::WellSurface());
    if (settings->keep_faces) {
      widgets::NextGap(style::BlockGap());
      if (widgets::Disclosure("Face mask tuning", "Keep faces")) widgets::Panel("##face_controls", [&] {
        rows({"ShowFaceMask", "FaceMaskThreshold", "FaceMaskFull", "FaceMaskStrength", "FaceMaskSoftness", "FaceEdgeFalloff",
              "FaceLightingDensity", "FaceSpeckSize", "FaceFillSkin", "FaceFillRadius", "FaceFillTolerance"});
      });
    }
  });
  if (widgets::Disclosure("Details", "Recorded passes")) widgets::Panel("##details_body", [&] { DrawDetails(view, *settings, *state); });
  widgets::NextGap(style::SectionGap());

  // Restore all defaults, confirmed once before it takes effect.
  gui::Separator();
  if (gui::Button("Restore all defaults")) {
    state->confirm_restore = true;
  }
  if (state->confirm_restore) {
    gui::TextWrapped("Every setting returns to its default, the hotkey included; Enable stays as it is.");
    if (gui::Button("Restore all")) {
      RestoreAllDefaults(settings);
      changed = true;
      state->confirm_restore = false;
    }
    gui::SameLine();
    if (gui::Button("Keep my settings")) {
      state->confirm_restore = false;
    }
  }

  changed |= DrawEnableWarning(*FindSetting("PresentWithFrameGen"), settings, state, view.dlss_explicit);
  changed |= DrawEnableWarning(*FindSetting("AllowCrossQueuePasses"), settings, state, view.dlss_explicit);
  return changed;
}

bool DrawOverlay(const OverlayView& view, Settings* settings, OverlayState* state) {
  const style::OverlayStyle theme;
  const auto host = gui::GetStyle().WindowPadding;
  const ImVec2 inset{std::max(0.f, style::BlockGap() - host.x), std::max(0.f, style::BlockGap() - host.y)};
  bool changed = false;
  widgets::Inset("##uplift_sheet", inset, [&] { changed = DrawOverlayContent(view, settings, state); });
  return changed;
}

}  // namespace uplift::ui
