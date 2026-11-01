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
#include "ui/settings_schema.hpp"
#include "sources/work_size.hpp"
#include "ui/overlay_widgets.hpp"

namespace uplift::ui {
namespace {

// Plan 14 (R90): the tooltip line of a highlighted option while the highlight is not the stored preference (what runs differs from what ReShade.ini holds).
std::string SavedNote(std::string_view preference) {
  return std::format("Saved: {}; it runs again by itself when it can.", preference);
}

bool DrawReset(const SettingDescriptor& descriptor, Settings& settings, OverlayState& state, bool align_right, const SetupView& setup) {
  if (descriptor.section == SettingSection::TOP || IsDefault(settings, descriptor)) return false;
  gui::SameLine();
  if (align_right) {
    const float remaining = gui::GetContentRegionAvail().x - gui::ResetButtonWidth();
    if (remaining > 0.f) gui::SetCursorPosX(gui::GetCursorPosX() + remaining);
  }
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

// One schema row. Returns true when the value changed. Called both from DrawOverlay (the Enable
// row) and from its own draw_rows lambda (every other row), so it stays a free function.
bool DrawRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state, bool grouped = false) {
  if (descriptor.key == "BeforeFGMotion" && (settings->fg_present != Settings::FgPresent::BEFORE
      || (settings->motion_vectors != MotionVectorSource::LAUNCHPAD && settings->motion_vectors != MotionVectorSource::LUMENITE))) return false;
  std::string label(descriptor.label);
  if (descriptor.key == "ResolutionPresent") label = std::format("Resolution at {}", view.SharedPresentName(*settings));
  if (descriptor.key == "ResolutionScalePresent") label = std::format("Custom scale at {} (%)", view.SharedPresentName(*settings));
  // Plan 6 (D6): with Compare = Built-in, a look-group row shows its built-in value, read-only: edits never reach `settings`.
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
  const bool stabilize_off = ((descriptor.key == "StabilizeMs" || descriptor.key == "StabilizeDetail")
                              && settings->look.stabilize == look::StabilizeMode::OFF);
  // Plan 17: the Adaptation rows only where the meter can be used; 2026-10-09: Auto's own rows only under Auto.
  const bool exposure_hidden = ExposureRowHidden(descriptor, *settings);
  // Split chain (Task 3): a later stage group's Custom scale as Setup's, and Upsampling also while a later group works below Full.
  const bool resolution_scale_hidden =
      ((descriptor.key == "ResolutionScale" && settings->resolution_mode != ResolutionMode::CUSTOM)
       || (descriptor.key == "ResolutionScaleAfterDlss" && settings->resolution_mode_after_dlss != ResolutionMode::CUSTOM)
       || (descriptor.key == "ResolutionScalePresent" && settings->resolution_mode_present != ResolutionMode::CUSTOM));
  const auto reduced = [](ResolutionMode mode, float percent) {
    return mode != ResolutionMode::FULL && (mode != ResolutionMode::CUSTOM || percent < 100.f);
  };
  const auto later_group_scaled = [&](const PassStageRow& row) {
    return row.starts_group && ((row.runs == SourcePick::AFTER_DLSS && reduced(settings->resolution_mode_after_dlss, settings->resolution_scale_after_dlss))
                                || (row.runs == SourcePick::PRESENT && reduced(settings->resolution_mode_present, settings->resolution_scale_present)));
  };
  const bool upsampling_hidden = (descriptor.key == "Upsampling" && !reduced(settings->resolution_mode, settings->resolution_scale)
                                  && std::ranges::none_of(view.setup.pass_stages, later_group_scaled));
  // Keep faces' own rows, and (fix round 1 addendum) its mask's tuning in Advanced, only while it is on.
  const bool keep_faces_tuning = descriptor.key.starts_with("FaceMask") || descriptor.key.starts_with("FaceFill") || descriptor.key == "FaceEdgeFalloff"
                                 || descriptor.key == "FaceLightingDensity" || descriptor.key == "FaceSpeckSize";
  // Split chain (Task 3, decision Q5a): a later stage's Keep faces here too.
  const bool keep_faces_here = (descriptor.key == "KeepFacesAfterDlss" || descriptor.key == "KeepFacesPresent");
  const bool keep_faces_off =
      ((descriptor.key == "FaceProtection" || descriptor.key == "LightingScale" || keep_faces_tuning || keep_faces_here) && !settings->keep_faces);
  // Round 5: the fill's radius and tolerance only while it is on.
  const bool fill_off = ((descriptor.key == "FaceFillRadius" || descriptor.key == "FaceFillTolerance") && !settings->face_tuning.fill_skin);
  // ui-review.md §3 rule 1: these rows matter only after a choice in the row just above them, so they
  // are hidden -- not merely greyed -- until that choice is made.
  if (pass_follows || shaping_off || stabilize_off || exposure_hidden || resolution_scale_hidden || upsampling_hidden || keep_faces_off || fill_off) {
    return false;
  }

  gui::PushID(label.c_str());
  // ui-review.md §3 rule 1 / §2 finding 5: Game state after NR only matters once NR can run inside the
  // game's frame; while no DLSS placement can run this session, it is greyed and says why.
  // Plan 9: the Direct3D 9Ex row is greyed the same way where it does nothing (every game but a Direct3D 9 one).
  // Plan 19: the VulkanNr row likewise, everywhere but a 64-bit Vulkan game.
  // Keep faces (2026-10-08): its rows where it cannot run, and the face mask's view (in Advanced, far from the switch) while it is off.
  const bool keep_faces_row = (descriptor.key == "KeepFaces" || descriptor.key == "FaceProtection" || descriptor.key == "LightingScale"
                               || descriptor.key == "ShowFaceMask" || keep_faces_tuning || keep_faces_here);
  const std::string_view disabled_reason =
      ((descriptor.flags & setting_flags::NETWORK_PRESET) != 0u && view.legacy_runtime)                   ? std::string_view("NR 310.8 only provides the Legacy network.")
      : (descriptor.key == "MaskBlendMode" && view.legacy_runtime)            ? std::string_view("NR 310.8 does not support native mask blend mode.")
      : (descriptor.key == "StateRestore" && !view.dlss_unavailable.empty())   ? std::string_view(view.dlss_unavailable)
      : (descriptor.key == "UseD3D9Ex" && !view.d3d9ex_unavailable.empty())    ? std::string_view(view.d3d9ex_unavailable)
      : (descriptor.key == "VulkanNr" && !view.vulkan_nr_unavailable.empty()) ? std::string_view(view.vulkan_nr_unavailable)
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
  if (grouped) {
    const char* caption = descriptor.key == "ResolutionMode" ? "Working resolution" : descriptor.key == "ResolutionScale" ? "Custom scale" : label.c_str();
    std::string value;
    const bool inherited_skin = descriptor.key == "SkinStructure" && settings->skin_structure < 0.f;
    if (descriptor.key == "ResolutionScale") value = std::format("{:.0f}%", settings->resolution_scale);
    else if (descriptor.kind == SettingKind::FLOAT) value = std::format("{:.2f}", inherited_skin ? settings->local_structure : descriptor.get(*settings));
    gui::BeginDisabled(inherited_skin);
    widgets::SettingCaption(caption, value, !IsDefault(*settings, descriptor));
    gui::EndDisabled();
    changed |= DrawReset(descriptor, *settings, *state, true, view.setup);
    widgets::NextGap(style::LabelGap());
    label = "##value";
  }
  std::string saved_note;  // Plan 14 (R90): the Resolution combo's tooltip, while the mode that runs is not the stored one
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
    const bool edited = grouped ? widgets::RailSlider(label.c_str(), skin, static_cast<float>(descriptor.min), static_cast<float>(SliderMax(descriptor)), "%.2f")
                                : gui::SliderFloat(label.c_str(), &skin, 0.f, 1.f);
    if (edited) {
      settings->skin_structure = skin;
      changed = true;
    }
    state->drag.skin_structure = gui::IsItemActive();
    gui::EndDisabled();
  } else if (descriptor.key == "DiffuseWhiteNits") {
    // ui-review.md §3 rule 2 (the Skin pattern): an Automatic checkbox, the control greyed at the value
    // in use -- shown, not hidden, so the effective value always reads even while it is automatic.
    bool automatic = (settings->diffuse_white_nits <= 0.f);
    if (gui::Checkbox("Automatic diffuse white", &automatic)) {
      settings->diffuse_white_nits = (automatic ? 0.f : std::clamp(view.effective_diffuse_white_nits, 48.f, 500.f));
      changed = true;
    }
    gui::BeginDisabled(automatic);
    float nits = (automatic ? view.effective_diffuse_white_nits : settings->diffuse_white_nits);
    if (gui::SliderFloat(label.c_str(), &nits, 48.f, 500.f, "%.0f") && !automatic) {
      settings->diffuse_white_nits = nits;
      changed = true;
    }
    gui::EndDisabled();
  } else {
    switch (descriptor.kind) {
      case SettingKind::BOOL: {
        bool value = (descriptor.get(*settings) != 0.0);
        if (gui::Checkbox(label.c_str(), &value)) {
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
        const bool edited = (descriptor.max > 100.0 ? gui::InputInt(label.c_str(), &value, 64, 512)
                                                    : gui::SliderInt(label.c_str(), &value, static_cast<int>(descriptor.min),
                                                                       static_cast<int>(descriptor.max)));
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
        // 2026-10-09: AutoExposureBlend is stored 0..1 and shown as 0-100 %.
        const float shown_scale = (descriptor.key == "AutoExposureBlend" ? 100.f : 1.f);
        float value = static_cast<float>(descriptor.get(*settings)) * shown_scale;
        const float high = static_cast<float>(SliderMax(descriptor)) * shown_scale;  // Ctrl+click may type past it (DescriptorMax)
        const bool resolution_scale = (descriptor.key == "ResolutionScale" || descriptor.key == "ResolutionScaleAfterDlss"
                                       || descriptor.key == "ResolutionScalePresent");
        const char* const format = (descriptor.key == "GraceSeconds" ? "%.1f"
                                    : resolution_scale                      ? "%.0f %%"
                                    : descriptor.key == "AutoExposureBlend" ? "%.0f %%"
                                    : descriptor.key == "StabilizeMs"       ? "%.0f ms"
                                                                            : "%.2f");
        const bool edited = grouped
            ? widgets::RailSlider(label.c_str(), value, static_cast<float>(descriptor.min) * shown_scale, high, format, resolution_scale ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None)
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
          state->drag.resolution_scale_after_dlss = active;  // split chain (Task 3): the work size waits for the release, as Setup's
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
            // Plan 19 (the owner's UI rules): Native greyed where it cannot run, with why; the highlight is the route that runs. T5: Direct3D 12 too.
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
          // Plan 14 (the user's decision 1): the entries that cannot run now are greyed, each with its reason, and the preview is the mode that runs.
          // Split chain (Task 3): a later stage group's Resolution greys the same entries (Match game reads the same DLSS render size); its preview is its
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
              // Fix round 1 (M5): Setup's row greys every entry that cannot run, its highlight included (as before the split chain); a later stage's row
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
          if (gui::Combo(label.c_str(), &value, labels.data(), static_cast<int>(count))) {
            descriptor.set(*settings, static_cast<double>(value));
            changed = true;
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
  if (grouped && descriptor.key == "ResolutionScale") widgets::PercentTicks(descriptor.min, descriptor.max);
  if (!grouped) changed |= DrawReset(descriptor, *settings, *state, false, view.setup);
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
  gui::SetNextWindowSize(ImVec2(gui::GetFontSize() * 34.f, 0.f), ImGuiCond_Appearing);
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

// Split chain (Task 3, design 6.2): pass n's Runs at row, as the NR stage row but a combo (four long choices): a choice that cannot run is greyed with why,
// the preview is where the pass runs, and the stored pick is named while it runs elsewhere. Where every pass runs at one stage the whole row is greyed.
// Not folded into DrawRow: its choices' states come from Setup's pass rows, not from the schema.
bool DrawPassStageRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings) {
  const size_t later = descriptor.pass - 2u;
  const PassStageRow& row = view.setup.pass_stages[later];
  const std::string label(descriptor.label);
  gui::PushID(label.c_str());
  gui::BeginDisabled(!view.setup.split_capable || view.setup.stopped);  // fix round 1 (M5): greyed with Setup's other rows once NR stopped (Plan 15)
  const auto stored = static_cast<size_t>(settings->passes[later].stage);
  const auto shown = static_cast<size_t>(row.shown);
  const std::array names{descriptor.choices[0].data(), NR_STAGE_NAMES[0], NR_STAGE_NAMES[1], view.SharedPresentName(*settings)};
  bool changed = false;
  if (gui::BeginCombo(label.c_str(), names[shown])) {  // literals and static strings: NUL-terminated
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
  if (!IsDefault(*settings, descriptor)) {
    gui::SameLine();
    const bool reset_clicked = gui::ResetButton();
    if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      gui::SetTooltip("Reset to default (%s)", descriptor.choices[0].data());
    }
    if (reset_clicked) {
      ResetSetting(settings, descriptor);
      changed = true;
    }
  }
  gui::EndDisabled();
  gui::PopID();
  return changed;
}

// Plan 14: the Motion vectors row -- Off / DLSS / Launchpad / Lumenite (2026-10-08) -- from Setup, as the NR stage row.
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
  gui::Separator();
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
    const float indent = gui::GetContentRegionAvail().x - ImGui::CalcTextSize(output.c_str()).x;
    if (indent > 0.f) gui::SetCursorPosX(gui::GetCursorPosX() + indent);
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
      gui::TableSetupColumn("##choices", ImGuiTableColumnFlags_WidthFixed, std::max(choices_width, 12.f * gui::GetFontSize()));
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
    row("AutoMask", false);
    row("SkinStructure");
    gui::Separator();
    row("KeepFaces", false);
    row("FaceProtection");
    row("LightingScale");
  });
  return changed;
}

// The VRAM line of the Details header and of the working status card.
std::string VramLine(const OverlayView& view) {
  constexpr double MIB = 1024.0 * 1024.0;
  if (view.runtime_bytes) {
    return std::format("VRAM: NR runtime {:.0f} MiB, Uplift intermediates {:.0f} MiB", static_cast<double>(*view.runtime_bytes) / MIB,
                       static_cast<double>(view.intermediate_bytes) / MIB);
  }
  return std::format("VRAM: Uplift intermediates {:.0f} MiB", static_cast<double>(view.intermediate_bytes) / MIB);
}

// Cut UTF-8 text at a character boundary and append ASCII "..." to fit `width`.
std::string EllipsisToWidth(const std::string& text, float width) {
  if (gui::CalcTextSize(text.c_str()).x <= width) {
    return text;
  }
  constexpr std::string_view DOTS = "...";
  const float dots_width = gui::CalcTextSize(DOTS.data()).x;
  size_t fits = 0u;  // the longest prefix that fits together with the dots
  for (size_t low = 0u, high = text.size(); low < high;) {
    const size_t middle = (low + high + 1u) / 2u;
    if (gui::CalcTextSize(text.c_str(), text.c_str() + middle).x + dots_width <= width) {
      low = middle;
      fits = middle;
    } else {
      high = middle - 1u;
    }
  }
  while (fits > 0u && (static_cast<unsigned char>(text[fits]) & 0xC0u) == 0x80u) {
    --fits;  // never cut inside a UTF-8 sequence
  }
  while (fits > 0u && text[fits - 1u] == ' ') {
    --fits;
  }
  return text.substr(0u, fits) + std::string(DOTS);
}

// One line of the working status card, in one piece (cut with "..." at the card's right edge, the full text as a tooltip). `muted`: TextDisabled.
void CardLine(const std::string& source, bool muted) {
  const std::string text = gui::Display(source);
  const std::string shown = EllipsisToWidth(text, gui::GetContentRegionAvail().x);
  if (muted) {
    gui::TextDisabled("%s", shown.c_str());
  } else {
    gui::TextUnformatted(shown.c_str());
  }
  if (shown.size() != text.size() && gui::IsItemHovered()) {
    gui::BeginTooltip();
    gui::PushTextWrapPos(gui::GetFontSize() * 30.f);
    gui::TextUnformatted(text.c_str());
    gui::PopTextWrapPos();
    gui::EndTooltip();
  }
}

// One detail line of the working status card: muted so the title and rows stay prominent, and left out when the card's fixed height has no room left
// for it -- the card never grows or scrolls.
void CardDetailLine(const std::string& text) {
  if (text.empty() || gui::GetContentRegionAvail().y < gui::GetTextLineHeight()) {
    return;
  }
  CardLine(text, true);
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
  if (!view.details_backend.empty()) gui::TextUnformatted(view.details_backend.c_str());
  if (!view.output_size.Empty()) {
    const char* space = "";
    switch (view.output_space) {
      case color::ColorSpace::SRGB_NONLINEAR: space = "sRGB"; break;
      case color::ColorSpace::EXTENDED_SRGB_LINEAR: space = "scRGB"; break;
      case color::ColorSpace::HDR10_ST2084: space = "HDR10"; break;
      case color::ColorSpace::HDR10_HLG: space = "HLG"; break;
      default: break;
    }
    ImGui::Text("%s  %u x %u  %s", gui::Display("Display output").c_str(), view.output_size.width, view.output_size.height, space);
  }
  if (view.frame_generation_active) gui::TextUnformatted("Frame generation active");
  else if (!view.frame_generation_line.empty()) gui::TextUnformatted("Frame generation: off");

  const bool compact = gui::GetContentRegionAvail().x < gui::GetFontSize() * 38.f;
  if (gui::BeginTable("##executed_passes", compact ? 3 : 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    const auto column = [](const char* label, float weight) { gui::TableSetupColumn(gui::Display(label).c_str(), ImGuiTableColumnFlags_WidthStretch, weight); };
    column("Pass", 0.50f);
    column("Ran at", 1.15f);
    column(compact ? "NR evaluation" : "Network / style", 1.3f);
    if (!compact) { column("NR size", 1.f); column("Motion used", 1.25f); }
    gui::TableHeadersRow();
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
        gui::TableNextColumn(); gui::TextWrapped("%s", NR_STAGE_NAMES[i]);
        gui::TableNextColumn(); ImGui::TextWrapped("%s", name.c_str());
        if (!compact) gui::TableNextColumn();
        if (group.resolution.Empty()) gui::TextDisabled("Unknown"); else gui::Text("%u x %u", group.resolution.width, group.resolution.height);
        if (!compact) gui::TableNextColumn();
        gui::TextWrapped("%s", motion);
        any = true;
      }
    }
    gui::EndTable();
    if (!any) gui::TextDisabled("No pass recorded.");
  }
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
  const float language_width = std::min(gui::GetContentRegionAvail().x, std::max(min_language_width, 12.f * gui::GetFontSize()));
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
    gui::SetTooltip("English / Simplified Chinese. If Chinese appears as boxes, choose a Chinese-capable Global font in ReShade Settings (for example C:\\Windows\\Fonts\\msyh.ttc). No extra Uplift files are needed.");
  }

  widgets::NextGap(style::BlockGap());
  if (!view.safety_locks.empty()) {
    const ImVec4 red = style::SafetyRed();
    const ImVec2 warning_start = gui::GetCursorScreenPos();
    const float inset = style::PanelInset();
    gui::Indent(inset);
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
    gui::Unindent(inset);
    gui::GetWindowDrawList()->AddRectFilled(warning_start, {warning_start.x + 3.f, gui::GetCursorScreenPos().y}, gui::GetColorU32(red));
    gui::Separator();
  }

  const StatusCard& card = view.card;
  const float card_inset = style::PanelInset();
  gui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {style::RowGap(), style::LabelGap()});
  widgets::Inset("##status_card", {card_inset, card_inset}, [&] {
    if (card.working) {
      // Plan 14 (design §1): green while NR works, three rows that say what Setup highlights (keys muted on the left, values in normal text from one column),
      // one muted Why line for a temporary fallback (else the note), and the footer. The height above is unchanged; whatever does not fit is left out.
      gui::TextColored(gui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", card.title.c_str());
      const float value_x = gui::GetCursorPosX() + gui::CalcTextSize("Motion vectors").x + 2.f * gui::GetStyle().ItemSpacing.x;
      const auto row = [value_x](const char* key, const std::string& value) {
        gui::TextDisabled("%s", key);
        gui::SameLine(value_x);
        CardLine(value, false);
      };
      const char* recorded = view.RecordedStageName(0u);
      row("Running at", recorded ? recorded : view.live.stage);
      row("Motion vectors", view.live.motion);
      if (!view.details_backend.empty()) row("Backend", view.details_backend);
      row("Resolution", view.live.resolution);
      if (!view.setup.why.empty()) {
        // Muted and wrapped: two lines in the card's spare height; the child clips anything longer, and the full text is the tooltip.
        const std::string why = "Why: " + view.setup.why;
        gui::PushStyleColor(ImGuiCol_Text, gui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        gui::TextWrapped("%s", why.c_str());
        gui::PopStyleColor();
        if (gui::IsItemHovered()) {
          gui::SetTooltip("%s", why.c_str());
        }
      } else {
        CardDetailLine(view.live.note);
      }
      CardDetailLine(view.live.footer);
    } else {
      gui::TextColored(gui::GetStyleColorVec4(ImGuiCol_PlotHistogram), "%s", card.title.c_str());
      if (!card.reason.empty()) {
        gui::TextWrapped("%s", card.reason.c_str());
      }
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
  }, true, style::Units(27));
  gui::PopStyleVar();

  // Every row of one section. All its call sites are here in DrawOverlay, so it stays a local
  // lambda rather than a file-scope helper.
  const auto draw_rows = [&view, settings, state](SettingSection section) {
    bool section_changed = false;
    for (const SettingDescriptor& descriptor : SettingsSchema()) {
      if (descriptor.section == section && descriptor.kind != SettingKind::KEY) {
        section_changed |= DrawRow(descriptor, view, settings, state);
      }
    }
    return section_changed;
  };
  // A section title's Reset button (L1, P1, F1), shown only once the section has changed. All its
  // call sites are here in DrawOverlay too, so it is also a local lambda. Plan 17: on a CollapsingHeader's
  // line the header must carry HEADER_FLAGS, or it keeps the hover and the click (Reset opened or closed the section).
  const auto section_reset = [state](const char* id, bool section_has_changed) {
    // Plan 6 (D6): with Compare = Built-in, every row already reads read-only at its built-in value; a hidden
    // section reset must not rewrite Mine's own settings while they are not even shown.
    if (!section_has_changed || state->defaults_view) return false;
    gui::SameLine();
    return gui::ResetButton(std::format("Reset##{}", id).c_str());
  };

  // ui-review.md §2 finding 1 / §3: the NR stage and Motion vectors toggles, in pipeline order, at the top
  // of the panel -- the decisions that shape everything else, instead of about the 45th line. Plan 14: Setup.
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

  // ui-review.md §2 finding 7: Compare (Mine/Built-in) moves here, the first row of Look.
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
  // Plan 5: pass 2..pass_count, each its own collapsible tree (P4-P13), so the per-pass rows do not
  // flood the panel for the common one-pass case. ui-review.md §5: the tree's own label shows its state.
  // Split chain (Task 3, design 6.2): Runs at first; on the first pass of a later stage group, that stage's Resolution (and Custom scale) and, while Keep
  // faces is on, its Keep faces here; while the chain is split the tree's label names where the pass runs.
  for (uint32_t pass = 2u; pass <= settings->pass_count; ++pass) {
    const bool follows = settings->passes[pass - 2u].follow_pass1;
    const PassStageRow& stage_row = view.setup.pass_stages[pass - 2u];
    std::string suffix;
    if (view.setup.chain_split) {
      const char* stage = view.RecordedStageName(pass - 1u);
      if (!stage) stage = stage_row.runs == SourcePick::PRESENT ? view.SharedPresentName(*settings) : StageLabel(stage_row.runs);
      suffix = " - " + gui::Display(stage);
    }
    const std::string tree_label =
        std::format("{}{}{}###pass{}", gui::Display(std::format("Pass {}", pass)), gui::Display(follows ? " (same look as pass 1)" : ""), suffix, pass);
    if (gui::TreeNode(tree_label.c_str())) {
      const auto is_stage_row = [](const SettingDescriptor& descriptor) { return (descriptor.flags & setting_flags::PASS_STAGE) != 0u; };
      for (const SettingDescriptor& descriptor : SettingsSchema()) {
        if (descriptor.pass == pass && is_stage_row(descriptor)) {
          changed |= DrawPassStageRow(descriptor, view, settings);
        }
      }
      changed |= DrawRow(*FindSetting(std::format("Pass{}Preset", pass)), view, settings, state);
      if (stage_row.starts_group && stage_row.runs != SourcePick::BEFORE_UPSCALING) {  // Before upscaling is always pass 1's stage
        const bool after_dlss = (stage_row.runs == SourcePick::AFTER_DLSS);
        for (const std::string_view key : {(after_dlss ? "ResolutionAfterDlss" : "ResolutionPresent"), (after_dlss ? "ResolutionScaleAfterDlss" : "ResolutionScalePresent"),
                                           (after_dlss ? "KeepFacesAfterDlss" : "KeepFacesPresent")}) {
          changed |= DrawRow(*FindSetting(key), view, settings, state);
        }
      }
      for (const SettingDescriptor& descriptor : SettingsSchema()) {
        if (descriptor.pass == pass && !is_stage_row(descriptor) && (descriptor.flags & setting_flags::NETWORK_PRESET) == 0u) {
          changed |= DrawRow(descriptor, view, settings, state);
        }
      }
      gui::TreePop();
    }
  }

  // ui-review.md §2 finding 4: Result and Limits merge under one header, opened at start only for a
  // user who already shapes (SetNextItemOpen(look.enabled, ImGuiCond_Once) keeps the "stay usable" intent).
  // Plan 17: the headers below let their title's Reset button (drawn on the same line) take the hover and the click.
  constexpr ImGuiTreeNodeFlags HEADER_FLAGS = ImGuiTreeNodeFlags_AllowOverlap;
  const bool shaping_changed = SectionChanged(*settings, SettingSection::RESULT) || SectionChanged(*settings, SettingSection::LIMITS);
  gui::SetNextItemOpen(settings->look.enabled, ImGuiCond_Once);
  if (gui::CollapsingHeader(shaping_changed ? "Result shaping (changed)###shaping" : "Result shaping###shaping", HEADER_FLAGS)) {
    if (section_reset("shaping", shaping_changed)) {
      ResetSection(settings, SettingSection::RESULT);
      ResetSection(settings, SettingSection::LIMITS);
      changed = true;
    }
    changed |= draw_rows(SettingSection::RESULT);
    if (settings->look.enabled) {
      gui::SeparatorText("Limits and stability");
    }
    changed |= draw_rows(SettingSection::LIMITS);
  }

  // ui-review.md §2 finding 5: Mask and UI correction, which handle the HUD, get their own section.
  const bool mask_changed = SectionChanged(*settings, SettingSection::MASK);
  if (gui::CollapsingHeader(mask_changed ? "Mask and HUD (changed)###mask" : "Mask and HUD###mask", HEADER_FLAGS)) {
    if (section_reset("mask", mask_changed)) {
      ResetSection(settings, SettingSection::MASK);
      changed = true;
    }
    changed |= draw_rows(SettingSection::MASK);
  }

  const bool fixes_changed = SectionChanged(*settings, SettingSection::FIXES_COLOR)
                             || SectionChanged(*settings, SettingSection::FIXES_GUIDES);
  if (gui::CollapsingHeader(fixes_changed ? "Fixes (changed)###fixes" : "Fixes###fixes", HEADER_FLAGS)) {
    if (section_reset("fixes", fixes_changed)) {  // Plan 17: on the title's line, as Result shaping's and Mask and HUD's
      ResetSection(settings, SettingSection::FIXES_COLOR);
      ResetSection(settings, SettingSection::FIXES_GUIDES);
      changed = true;
    }
    gui::TextDisabled("Only change these if you know what you are doing.");
    gui::SeparatorText("Colour");
    changed |= draw_rows(SettingSection::FIXES_COLOR);
    gui::SeparatorText("Motion and game state");
    changed |= draw_rows(SettingSection::FIXES_GUIDES);
  }

  if (gui::CollapsingHeader("Advanced")) {
    changed |= draw_rows(SettingSection::ADVANCED);
  }

  if (gui::CollapsingHeader("Details")) DrawDetails(view, *settings, *state);

  // G2: Restore all defaults, confirmed once before it takes effect.
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
