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
#include "color/encoding.hpp"
#include "ui/settings_schema.hpp"

namespace uplift::ui {
namespace {

// ui-review.md §3 (user decision) and §4.3: every row with three or fewer short choices -- Source,
// Motion vectors, Compare, Model, Upsampling, Stabilise, NR mask, UI correction and any other CHOICE row
// this short -- draws as a segmented control instead of RadioButton (which sizes to its text) or a
// Combo, so every option is equally sized and the row's total width matches CalcItemWidth(), lining up
// with the sliders and combos above and below it. `why[i]` non-empty greys option `i`; its tooltip
// starts "Unavailable here: <why[i]>", then `tips[i]`. The row label sits on the right, at the same x a
// slider's label would sit at (ui-review.md §4.3's RadioRow sketch, adapted to draw buttons). Returns
// the clicked option, or -1.
int RadioRow(const char* label, std::span<const char* const> options, std::span<const std::string_view> why,
             std::span<const std::string_view> tips, int selected) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float total_width = ImGui::CalcItemWidth();
  const float gap = style.ItemInnerSpacing.x;
  const size_t count = options.size();
  const float button_width = (total_width - static_cast<float>(count - 1u) * gap) / static_cast<float>(count);
  const float label_x = ImGui::GetCursorPosX() + total_width + gap;
  int clicked = -1;
  for (size_t index = 0u; index < count; ++index) {
    if (index > 0u) {
      ImGui::SameLine(0.f, gap);
    }
    ImGui::PushID(static_cast<int>(index));
    const bool selected_option = (selected == static_cast<int>(index));
    if (selected_option) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    ImGui::BeginDisabled(!why[index].empty());
    if (ImGui::Button(options[index], ImVec2(button_width, 0.f))) {
      clicked = static_cast<int>(index);
    }
    ImGui::EndDisabled();
    if (selected_option) {
      ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      std::string tooltip;
      if (!why[index].empty()) {
        tooltip = std::format("Unavailable here: {}", why[index]);
        if (!tips[index].empty()) {
          tooltip += "\n";
        }
      }
      tooltip += tips[index];
      if (!tooltip.empty()) {
        ImGui::SetTooltip("%s", tooltip.c_str());
      }
    }
    ImGui::PopID();
  }
  ImGui::SameLine();
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), label_x));
  ImGui::TextUnformatted(label);
  return clicked;
}

// Plan 14 (R90): the tooltip line of a highlighted option while the highlight is not the stored preference (what runs differs from what ReShade.ini holds).
std::string SavedNote(std::string_view preference) {
  return std::format("Saved: {}; it runs again by itself when it can.", preference);
}

// One schema row. Returns true when the value changed. Called both from DrawOverlay (the Enable
// row) and from its own draw_rows lambda (every other row), so it stays a free function.
bool DrawRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state) {
  const std::string label(descriptor.label);
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
  // Plan 17: the Adaptation rows matter wherever the meter can be used: Metered, and Auto (the default), which meters when the game's exposure is missing or off.
  const bool not_metered = ((descriptor.key == "ExposureAdapt" || descriptor.key == "AdaptBrighterStops"
                             || descriptor.key == "AdaptDarkerStops")
                            && settings->input_exposure != color::InputExposure::AUTO
                            && settings->input_exposure != color::InputExposure::METERED);
  const bool resolution_scale_hidden =
      (descriptor.key == "ResolutionScale" && settings->resolution_mode != ResolutionMode::CUSTOM);
  const bool upsampling_hidden = (descriptor.key == "Upsampling" && settings->resolution_mode == ResolutionMode::FULL);
  // ui-review.md §3 rule 1: these rows matter only after a choice in the row just above them, so they
  // are hidden -- not merely greyed -- until that choice is made.
  if (pass_follows || shaping_off || stabilize_off || not_metered || resolution_scale_hidden || upsampling_hidden) {
    return false;
  }

  ImGui::PushID(label.c_str());
  // ui-review.md §3 rule 1 / §2 finding 5: Game state after NR only matters once NR can run inside the
  // game's frame; while no DLSS placement can run this session, it is greyed and says why.
  // Plan 9: the Direct3D 9Ex row is greyed the same way where it does nothing (every game but a Direct3D 9 one).
  const std::string_view disabled_reason =
      (descriptor.key == "StateRestore" && !view.dlss_unavailable.empty())  ? std::string_view(view.dlss_unavailable)
      : (descriptor.key == "UseD3D9Ex" && !view.d3d9ex_unavailable.empty()) ? std::string_view(view.d3d9ex_unavailable)
                                                                            : std::string_view();
  ImGui::BeginDisabled(!disabled_reason.empty() || read_only);
  bool changed = false;
  std::string saved_note;  // Plan 14 (R90): the Resolution combo's tooltip, while the mode that runs is not the stored one
  if (descriptor.key == "SkinStructure") {
    bool follows = (settings->skin_structure < 0.f);
    if (ImGui::Checkbox("Skin follows Structure", &follows)) {
      settings->skin_structure = (follows ? SKIN_SAME_AS_STRUCTURE : settings->local_structure);
      changed = true;
    }
    ImGui::BeginDisabled(follows);
    float skin = (follows ? settings->local_structure : settings->skin_structure);
    if (ImGui::SliderFloat(label.c_str(), &skin, 0.f, 1.f)) {
      settings->skin_structure = skin;
      changed = true;
    }
    state->drag.skin_structure = ImGui::IsItemActive();
    ImGui::EndDisabled();
  } else if (descriptor.key == "DiffuseWhiteNits") {
    // ui-review.md §3 rule 2 (the Skin pattern): an Automatic checkbox, the control greyed at the value
    // in use -- shown, not hidden, so the effective value always reads even while it is automatic.
    bool automatic = (settings->diffuse_white_nits <= 0.f);
    if (ImGui::Checkbox("Automatic diffuse white", &automatic)) {
      settings->diffuse_white_nits = (automatic ? 0.f : std::clamp(view.effective_diffuse_white_nits, 48.f, 500.f));
      changed = true;
    }
    ImGui::BeginDisabled(automatic);
    float nits = (automatic ? view.effective_diffuse_white_nits : settings->diffuse_white_nits);
    if (ImGui::SliderFloat(label.c_str(), &nits, 48.f, 500.f, "%.0f") && !automatic) {
      settings->diffuse_white_nits = nits;
      changed = true;
    }
    ImGui::EndDisabled();
  } else {
    switch (descriptor.kind) {
      case SettingKind::BOOL: {
        bool value = (descriptor.get(*settings) != 0.0);
        if (ImGui::Checkbox(label.c_str(), &value)) {
          descriptor.set(*settings, (value ? 1.0 : 0.0));
          changed = true;
        }
        break;
      }
      case SettingKind::UINT: {
        const int applied = static_cast<int>(descriptor.get(*settings));
        const bool on_release = ((descriptor.flags & setting_flags::APPLY_ON_RELEASE) != 0u);
        const bool held_here = (on_release && state->pending_release.has_value() && state->pending_key == descriptor.key);
        int value = (held_here ? *state->pending_release : applied);
        const bool edited = (descriptor.max > 100.0 ? ImGui::InputInt(label.c_str(), &value, 64, 512)
                                                    : ImGui::SliderInt(label.c_str(), &value, static_cast<int>(descriptor.min),
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
          if (ImGui::IsItemDeactivatedAfterEdit() && state->pending_key == descriptor.key && state->pending_release) {
            const int released = *std::exchange(state->pending_release, std::nullopt);
            if (released != applied) {
              descriptor.set(*settings, std::clamp(static_cast<double>(released), descriptor.min, descriptor.max));
              changed = true;
            }
          } else if (ImGui::IsItemDeactivated() && state->pending_key == descriptor.key) {
            state->pending_release.reset();  // released without an edit
          }
        }
        if (descriptor.key == "PassCount" && !view.frame_generation_warning.empty()) {
          ImGui::TextDisabled("%s", view.frame_generation_warning.c_str());
        }
        break;
      }
      case SettingKind::FLOAT: {
        float value = static_cast<float>(descriptor.get(*settings));
        const float high = static_cast<float>(descriptor.ui_max > 0.0 ? descriptor.ui_max : descriptor.max);
        const char* const format = (descriptor.key == "GraceSeconds" ? "%.1f"
                                    : descriptor.key == "ResolutionScale" ? "%.0f %%"
                                    : descriptor.key == "StabilizeMs"     ? "%.0f ms"
                                                                          : "%.2f");
        if (ImGui::SliderFloat(label.c_str(), &value, static_cast<float>(descriptor.min), high, format)) {
          descriptor.set(*settings, static_cast<double>(value));
          changed = true;
        }
        const bool active = ImGui::IsItemActive();
        if (descriptor.key == "LocalStructure") {
          state->drag.local_structure = active;
        } else if (descriptor.key == "LocalTone") {
          state->drag.local_tone = active;
        } else if (descriptor.key == "ResolutionScale") {
          state->drag.resolution_scale = active;
        }
        if (descriptor.pass != 0u && (descriptor.flags & setting_flags::RESTARTS_HISTORY) != 0u) {
          state->drag.pass_slider |= active;
        }
        break;
      }
      case SettingKind::CHOICE: {
        int value = static_cast<int>(descriptor.get(*settings));
        if (descriptor.choices.size() <= 3u) {
          // User decision: at most three short choices draws as the RadioRow segmented control above,
          // never a Combo (Model, Upsampling, Stabilise, NR mask, UI correction and the like).
          std::array<const char*, 3> labels = {};
          std::array<std::string_view, 3> why = {};
          if (!disabled_reason.empty()) {
            why.fill(disabled_reason);  // e.g. Game state after NR, greyed while no DLSS placement can run
          }
          std::array<std::string_view, 3> tips = {};
          tips.fill(descriptor.tooltip);
          for (size_t index = 0u; index < descriptor.choices.size(); ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          const std::span<const char* const> options(labels.data(), descriptor.choices.size());
          const std::span<const std::string_view> why_span(why.data(), descriptor.choices.size());
          const std::span<const std::string_view> tips_span(tips.data(), descriptor.choices.size());
          if (const int clicked = RadioRow(label.c_str(), options, why_span, tips_span, value); clicked >= 0) {
            descriptor.set(*settings, static_cast<double>(clicked));
            changed = true;
          }
        } else if (descriptor.key == "ResolutionMode") {
          // Plan 14 (the user's decision 1): the entries that cannot run now are greyed, each with its reason, and the preview is the mode that runs.
          std::array<const char*, 6> labels = {};
          for (size_t index = 0u; index < labels.size(); ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          const size_t shown = static_cast<size_t>(view.setup.resolution);
          const size_t stored_index = std::min(static_cast<size_t>(settings->resolution_mode), labels.size() - 1u);
          const OptionState& stored = view.setup.resolution_options[stored_index];
          if (view.setup.resolution != settings->resolution_mode && (stored.why.empty() || stored.temporary)) {  // a fixed cause never clears: no promise
            saved_note = SavedNote(labels[stored_index]);
          }
          if (ImGui::BeginCombo(label.c_str(), labels[shown])) {
            for (size_t index = 0u; index < labels.size(); ++index) {
              const OptionState& option = view.setup.resolution_options[index];
              ImGui::BeginDisabled(!option.why.empty());
              if (ImGui::Selectable(labels[index], shown == index)) {
                descriptor.set(*settings, static_cast<double>(index));
                state->setup_settle.Clicked(SetupRow::RESOLUTION, static_cast<uint32_t>(index));
                changed = true;
              }
              ImGui::EndDisabled();
              if (!option.why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Unavailable here: %s", std::string(option.why).c_str());
              }
            }
            ImGui::EndCombo();
          }
        } else {
          std::array<const char*, 8> labels = {};
          const size_t count = std::min(descriptor.choices.size(), labels.size());
          for (size_t index = 0u; index < count; ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          if (ImGui::Combo(label.c_str(), &value, labels.data(), static_cast<int>(count))) {
            descriptor.set(*settings, static_cast<double>(value));
            changed = true;
          }
        }
        break;
      }
      case SettingKind::TEXT: {
        std::array<char, 1024> text = {};
        descriptor.get_text(*settings).copy(text.data(), text.size() - 1u);
        if (ImGui::InputText(label.c_str(), text.data(), text.size())) {
          descriptor.set_text(*settings, text.data());
          changed = true;
        }
        break;
      }
      case SettingKind::KEY: break;  // the Enable hotkey is drawn beside Enable
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && (!descriptor.tooltip.empty() || !disabled_reason.empty())) {
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
    ImGui::SetTooltip("%s", tooltip.c_str());
  }
  if (descriptor.key == "Mask" && !view.mask_note.empty()) {
    ImGui::TextDisabled("%s", view.mask_note.c_str());
  }
  if (descriptor.key == "UICorrection" && !view.ui_correction_note.empty()) {
    ImGui::TextDisabled("%s", view.ui_correction_note.c_str());
  }
  if (descriptor.key == "UseD3D9Ex") {
    const std::string& note = (view.d3d9ex_unavailable.empty() ? view.d3d9ex_readout : view.d3d9ex_unavailable);
    if (!note.empty()) {
      ImGui::TextDisabled("%s", note.c_str());
    }
  }
  // The reset arrow after a row that differs from its default (design §3.16, G1). The Enable
  // switch itself is the one row without an arrow: "reset" would only mean "switch NR off".
  if (descriptor.section != SettingSection::TOP && !IsDefault(*settings, descriptor)) {
    ImGui::SameLine();
    const bool reset_clicked = ImGui::SmallButton("<");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Reset to default (%s)", FormatSettingValue(descriptor, DefaultSettings()).c_str());
    }
    if (reset_clicked) {
      ResetSetting(settings, descriptor);
      if (descriptor.key == "ResolutionMode") {
        // The highlight follows at once, to the default when it can run and else to the best that can.
        state->setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(DefaultSettings().resolution_mode), view.setup);
      }
      changed = true;
    }
  }
  ImGui::EndDisabled();
  ImGui::PopID();
  return changed && !read_only;
}

// Plan 14: the NR stage row -- Before upscaling / After DLSS / Present -- from Setup: an option that cannot run now is greyed (its reason on hover), the
// highlight is what runs, and a click writes the stored preference as before. Not folded into DrawRow: it reads and writes two schema keys (Source,
// PreUpscale) as one composite choice, and it carries the DLSS-placement latch's Clear latch button.
bool DrawSourceRow(const OverlayView& view, Settings* settings, OverlayState* state) {
  ImGui::PushID("Source");
  static constexpr std::array<const char*, 3> OPTIONS = {"Before upscaling", "After DLSS", "Present"};
  const std::array<std::string_view, 3> direct3d_tips = {
      "NR works on the game's render-size image, and the game's DLSS upscales the result. Cheaper. "
      "Experimental. With Ray Reconstruction NR stays after DLSS.",
      "NR works on DLSS's output inside the game's frame, before the HUD, post-processing and frame "
      "generation. Runs on Present until the game runs DLSS.",
      "NR works on the finished frame, HUD included. Works in every game. Uplift.fx's Uplift technique "
      "sets where among your effects.",
  };
  // Plan 13 (the user's decision 1): on Vulkan the DLSS placements are explicit choices, and their tips name what stays after the first use.
  const std::array<std::string_view, 3> vulkan_tips = {
      "NR works on the game's render-size image, natively on the game's Vulkan device, and the game's DLSS upscales the result. Experimental. With Ray "
      "Reconstruction NR stays after DLSS. About 0.6 GB of VRAM stays with the game's device after the first use, until the game exits.",
      "NR works on DLSS's output inside the game's frame, natively on the game's Vulkan device, before the HUD and post-processing. Runs on "
      "Present until the game runs DLSS. About 0.6 GB of VRAM stays with the game's device after the first use, until the game exits.",
      "NR works on the finished frame, HUD included. The default on Vulkan. Uplift.fx's Uplift technique sets where among your effects.",
  };
  std::array<std::string_view, 3> tips = (view.dlss_explicit ? vulkan_tips : direct3d_tips);
  std::array<std::string_view, 3> why = {};
  for (size_t index = 0u; index < why.size(); ++index) {
    why[index] = view.setup.stage_options[index].why;
  }
  const SourcePick preferred = SourcePickOf(*settings, view.dlss_explicit);
  const int shown = static_cast<int>(view.setup.stage);
  std::string saved_tip;  // the highlighted option's tip, with the stored preference when it is not the one that runs
  const OptionState& stored_stage = view.setup.stage_options[static_cast<size_t>(preferred)];
  if (view.setup.stage != preferred && (stored_stage.why.empty() || stored_stage.temporary)) {  // a fixed cause never clears: no promise
    saved_tip = std::format("{}\n{}", tips[static_cast<size_t>(shown)], SavedNote(OPTIONS[static_cast<size_t>(preferred)]));
    tips[static_cast<size_t>(shown)] = saved_tip;
  }
  bool changed = false;
  if (const int clicked = RadioRow("NR stage", OPTIONS, why, tips, shown); clicked >= 0) {
    SetSourcePick(settings, static_cast<SourcePick>(clicked), view.dlss_explicit);
    state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(clicked));
    changed = true;
  }
  if (!IsDefault(*settings, *FindSetting("Source")) || !IsDefault(*settings, *FindSetting("PreUpscale"))) {
    ImGui::SameLine();
    const bool reset_clicked = ImGui::SmallButton("<");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(view.dlss_explicit ? "Reset to default (Present on Vulkan)" : "Reset to default (After DLSS)");
    }
    if (reset_clicked) {
      // The defaults themselves (Auto, PreUpscale off): After DLSS on Direct3D 12, Present on Vulkan.
      const SourcePick default_pick = (view.dlss_explicit ? SourcePick::PRESENT : SourcePick::AFTER_DLSS);
      SetSourcePick(settings, default_pick, view.dlss_explicit);
      state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(default_pick), view.setup);  // at once, to the best that can run
      changed = true;
    }
  }
  // ui-review.md §4.1: the latch's one surviving Clear latch button, unless the status card already shows it.
  if (view.dlss_latched && view.card.button != CardButton::CLEAR_LATCH) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear latch")) {
      settings->dlss_placement_blocked = false;
      changed = true;
    }
  }
  ImGui::PopID();
  return changed;
}

// Plan 14: the Motion vectors row -- Off / DLSS / Launchpad -- from Setup, as the NR stage row.
bool DrawMotionRow(const OverlayView& view, Settings* settings, OverlayState* state) {
  ImGui::PushID("MotionVectors");
  static constexpr std::array<const char*, 3> OPTIONS = {"Off", "DLSS", "Launchpad"};
  std::array<std::string_view, 3> tips = {
      "No motion vectors, anywhere.",
      "The game's own DLSS motion vectors: bound directly inside the frame, and copied for Present when "
      "NR stage is Present.",
      "iMMERSE Launchpad on, and Uplift.fx's Uplift technique below it. The presented image only.",
  };
  std::array<std::string_view, 3> why = {};
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
  bool changed = false;
  if (const int clicked = RadioRow("Motion vectors", OPTIONS, why, tips, shown); clicked >= 0) {
    SetMotionPick(settings, static_cast<MotionPick>(clicked));
    state->setup_settle.Clicked(SetupRow::MOTION, static_cast<uint32_t>(clicked));
    changed = true;
  }
  if (!IsDefault(*settings, *FindSetting("MotionVectors"))) {
    ImGui::SameLine();
    const bool reset_clicked = ImGui::SmallButton("<");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Reset to default (DLSS)");
    }
    if (reset_clicked) {
      SetMotionPick(settings, MotionPick::DLSS);
      state->setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPick::DLSS), view.setup, true);  // DLSS is stored as Auto
      changed = true;
    }
  }
  ImGui::PopID();
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

// `text` cut with "..." (ASCII, so the strings stay Latin-1) to fit `width`; unchanged when it already fits.
std::string EllipsisToWidth(const std::string& text, float width) {
  if (ImGui::CalcTextSize(text.c_str()).x <= width) {
    return text;
  }
  constexpr std::string_view DOTS = "...";
  const float dots_width = ImGui::CalcTextSize(DOTS.data()).x;
  size_t fits = 0u;  // the longest prefix that fits together with the dots
  for (size_t low = 0u, high = text.size(); low < high;) {
    const size_t middle = (low + high + 1u) / 2u;
    if (ImGui::CalcTextSize(text.c_str(), text.c_str() + middle).x + dots_width <= width) {
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
void CardLine(const std::string& text, bool muted) {
  const std::string shown = EllipsisToWidth(text, ImGui::GetContentRegionAvail().x);
  if (muted) {
    ImGui::TextDisabled("%s", shown.c_str());
  } else {
    ImGui::TextUnformatted(shown.c_str());
  }
  if (shown.size() != text.size() && ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// One detail line of the working status card: muted so the title and rows stay prominent, and left out when the card's fixed height has no room left
// for it -- the card never grows or scrolls.
void CardDetailLine(const std::string& text) {
  if (text.empty() || ImGui::GetContentRegionAvail().y < ImGui::GetTextLineHeight()) {
    return;
  }
  CardLine(text, true);
}

}  // namespace

void FinishSetup(OverlayView* view, OverlayState* state, const void* device, const SetupFacts& facts, std::chrono::steady_clock::time_point now) {
  if (state->setup_device != device) {
    state->setup_device = device;
    state->setup_settle.Reset();
  }
  view->setup = state->setup_settle.Update(ResolveSetup(facts), now);
  if (view->card.working) {
    view->live = BuildLiveCard(facts, view->setup);
  }
}

bool DrawOverlay(const OverlayView& view, Settings* settings, OverlayState* state) {
  state->drag.pass_slider = false;  // the rows OR into it
  bool changed = false;

  changed |= DrawRow(*FindSetting(ENABLED_KEY), view, settings, state);
  ImGui::SameLine();
  if (state->capturing_key) {
    ImGui::Button("Press a key (Esc clears)");
    if (view.last_key_pressed != 0u) {
      settings->enable_key = (view.last_key_pressed == VK_ESCAPE ? 0u : view.last_key_pressed);
      state->capturing_key = false;
      state->key_captured_this_frame = true;
      changed = true;
    }
  } else {
    std::string key_label = "Hotkey: none";
    if (settings->enable_key >= VK_F1 && settings->enable_key <= VK_F24) {
      key_label = std::format("Hotkey: F{}", settings->enable_key - VK_F1 + 1u);
    } else if (settings->enable_key != 0u) {
      std::array<char, 64> key_name = {};
      const auto scan_code = static_cast<LONG>(MapVirtualKeyA(settings->enable_key, MAPVK_VK_TO_VSC) << 16u);
      key_label = (GetKeyNameTextA(scan_code, key_name.data(), static_cast<int>(key_name.size())) > 0
                       ? std::format("Hotkey: {}", key_name.data())
                       : std::format("Hotkey: key {:#04x}", settings->enable_key));
    }
    if (ImGui::Button(key_label.c_str())) {
      state->capturing_key = true;
    }
  }

  // Plan 6 (v2 design §3.18, D1): the status card -- the first failing stage, its reason and its one fix/button.
  // Plan 7 (the controller's addition): a fixed-height region, sized for the card's worst case (a title, a
  // two-line wrapped reason, at most two fix bullets, and one button row), so a transient reason or bullet --
  // e.g. the "resizing" and "Running k of k+1 passes." lines a Pass Count change used to flash -- never
  // shifts the rows below it. Same in D3D12 and D3D11 (bridged): this is ordinary ImGui, backend-agnostic.
  constexpr float CARD_TEXT_LINES = 5.f;  // title + up to 2 reason lines + up to 2 fix bullets
  const StatusCard& card = view.card;
  ImGui::BeginChild("##status_card", ImVec2(0.f, ImGui::GetTextLineHeightWithSpacing() * CARD_TEXT_LINES + ImGui::GetFrameHeightWithSpacing()),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (card.working) {
    // Plan 14 (design §1): green while NR works, three rows that say what Setup highlights (keys muted on the left, values in normal text from one column),
    // one muted Why line for a temporary fallback (else the note), and the footer. The height above is unchanged; whatever does not fit is left out.
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.f), "%s", card.title.c_str());
    const float value_x = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Motion vectors").x + 2.f * ImGui::GetStyle().ItemSpacing.x;
    const auto row = [value_x](const char* key, const std::string& value) {
      ImGui::TextDisabled("%s", key);
      ImGui::SameLine(value_x);
      CardLine(value, false);
    };
    row("Running at", view.live.stage);
    row("Motion vectors", view.live.motion);
    row("Resolution", view.live.resolution);
    if (!view.setup.why.empty()) {
      // Muted and wrapped: two lines in the card's spare height; the child clips anything longer, and the full text is the tooltip.
      const std::string why = "Why: " + view.setup.why;
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("%s", why.c_str());
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", why.c_str());
      }
    } else {
      CardDetailLine(view.live.note);
    }
    CardDetailLine(view.live.footer);
  } else {
    ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "%s", card.title.c_str());
    if (!card.reason.empty()) {
      ImGui::TextWrapped("%s", card.reason.c_str());
    }
    for (const std::string& fix : card.fixes) {
      ImGui::BulletText("%s", fix.c_str());
    }
  }
  switch (card.button) {
    case CardButton::NONE: break;
    case CardButton::TURN_ON:
      if (ImGui::Button("Turn on")) {
        settings->enabled = true;
        changed = true;
      }
      break;
    case CardButton::RETRY_NOW:
      if (ImGui::Button(view.dlss_latched ? "Retry now and clear the latch" : "Retry now")) {
        state->retry_now = true;
        if (view.dlss_latched) {
          settings->dlss_placement_blocked = false;
          changed = true;
        }
      }
      break;
    case CardButton::CLEAR_LATCH:
      if (ImGui::Button("Clear latch##card")) {
        settings->dlss_placement_blocked = false;
        changed = true;
      }
      break;
  }
  ImGui::EndChild();

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
    ImGui::SameLine();
    return ImGui::SmallButton(std::format("Reset##{}", id).c_str());
  };

  // ui-review.md §2 finding 1 / §3: the NR stage and Motion vectors toggles, in pipeline order, at the top
  // of the panel -- the decisions that shape everything else, instead of about the 45th line. Plan 14: Setup.
  const bool placement_changed = SectionChanged(*settings, SettingSection::PLACEMENT);
  ImGui::SeparatorText(placement_changed ? "Setup (changed)" : "Setup");
  if (section_reset("placement", placement_changed)) {
    ResetSection(settings, SettingSection::PLACEMENT);
    const Settings defaults = DefaultSettings();  // the three highlights follow at once, each to the best option that can run (the stage first)
    state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(SourcePickOf(defaults, view.dlss_explicit)), view.setup);
    state->setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPickOf(defaults)), view.setup, true);
    state->setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(defaults.resolution_mode), view.setup);
    changed = true;
  }
  changed |= DrawSourceRow(view, settings, state);
  changed |= DrawMotionRow(view, settings, state);
  changed |= DrawRow(*FindSetting("ResolutionMode"), view, settings, state);
  changed |= DrawRow(*FindSetting("ResolutionScale"), view, settings, state);
  changed |= DrawRow(*FindSetting("Upsampling"), view, settings, state);

  // ui-review.md §2 finding 7: Compare (Mine/Built-in) moves here, the first row of Look.
  const bool look_changed = SectionChanged(*settings, SettingSection::LOOK);
  ImGui::SeparatorText(look_changed ? "Look (changed)" : "Look");
  if (section_reset("look", look_changed)) {
    ResetSection(settings, SettingSection::LOOK);
    changed = true;
  }
  ImGui::PushID("Compare");
  static constexpr std::array<const char*, 2> COMPARE_OPTIONS = {"Mine", "Built-in"};
  const std::array<std::string_view, 2> compare_why = {};
  const std::array<std::string_view, 2> compare_tips = {
      std::string_view(), "The built-in look, read-only; your own settings are kept."};
  if (const int clicked =
          RadioRow("Compare", COMPARE_OPTIONS, compare_why, compare_tips, (state->defaults_view ? 1 : 0));
      clicked >= 0) {
    state->defaults_view = (clicked == 1);
  }
  ImGui::PopID();
  changed |= draw_rows(SettingSection::LOOK);
  // Plan 5: pass 2..pass_count, each its own collapsible tree (P4-P13), so the per-pass rows do not
  // flood the panel for the common one-pass case. ui-review.md §5: the tree's own label shows its state.
  for (uint32_t pass = 2u; pass <= settings->pass_count; ++pass) {
    const bool follows = settings->passes[pass - 2u].follow_pass1;
    const std::string tree_label =
        std::format("Pass {}{}###pass{}", pass, (follows ? " (same as pass 1)" : ""), pass);
    if (ImGui::TreeNode(tree_label.c_str())) {
      for (const SettingDescriptor& descriptor : SettingsSchema()) {
        if (descriptor.pass == pass) {
          changed |= DrawRow(descriptor, view, settings, state);
        }
      }
      ImGui::TreePop();
    }
  }

  // ui-review.md §2 finding 4: Result and Limits merge under one header, opened at start only for a
  // user who already shapes (SetNextItemOpen(look.enabled, ImGuiCond_Once) keeps the "stay usable" intent).
  // Plan 17: the headers below let their title's Reset button (drawn on the same line) take the hover and the click.
  constexpr ImGuiTreeNodeFlags HEADER_FLAGS = ImGuiTreeNodeFlags_AllowOverlap;
  const bool shaping_changed = SectionChanged(*settings, SettingSection::RESULT) || SectionChanged(*settings, SettingSection::LIMITS);
  ImGui::SetNextItemOpen(settings->look.enabled, ImGuiCond_Once);
  if (ImGui::CollapsingHeader(shaping_changed ? "Result shaping (changed)###shaping" : "Result shaping###shaping", HEADER_FLAGS)) {
    if (section_reset("shaping", shaping_changed)) {
      ResetSection(settings, SettingSection::RESULT);
      ResetSection(settings, SettingSection::LIMITS);
      changed = true;
    }
    changed |= draw_rows(SettingSection::RESULT);
    if (settings->look.enabled) {
      ImGui::SeparatorText("Limits and stability");
    }
    changed |= draw_rows(SettingSection::LIMITS);
  }

  // ui-review.md §2 finding 5: Mask and UI correction, which handle the HUD, get their own section.
  const bool mask_changed = SectionChanged(*settings, SettingSection::MASK);
  if (ImGui::CollapsingHeader(mask_changed ? "Mask and HUD (changed)###mask" : "Mask and HUD###mask", HEADER_FLAGS)) {
    if (section_reset("mask", mask_changed)) {
      ResetSection(settings, SettingSection::MASK);
      changed = true;
    }
    changed |= draw_rows(SettingSection::MASK);
  }

  const bool fixes_changed = SectionChanged(*settings, SettingSection::FIXES_COLOR)
                             || SectionChanged(*settings, SettingSection::FIXES_GUIDES);
  if (ImGui::CollapsingHeader(fixes_changed ? "Fixes (changed)###fixes" : "Fixes###fixes", HEADER_FLAGS)) {
    if (section_reset("fixes", fixes_changed)) {  // Plan 17: on the title's line, as Result shaping's and Mask and HUD's
      ResetSection(settings, SettingSection::FIXES_COLOR);
      ResetSection(settings, SettingSection::FIXES_GUIDES);
      changed = true;
    }
    ImGui::TextDisabled("Only change these if you know what you are doing.");
    ImGui::SeparatorText("Colour");
    changed |= draw_rows(SettingSection::FIXES_COLOR);
    ImGui::SeparatorText("Motion and game state");
    changed |= draw_rows(SettingSection::FIXES_GUIDES);
  }

  if (ImGui::CollapsingHeader("Advanced")) {
    changed |= draw_rows(SettingSection::ADVANCED);
  }

  // ui-review.md §2 finding 2: the wall of status text moves into one closed Details header. It always lists
  // everything, so its content does not depend on the card's state; a working card repeats its details lines.
  if (ImGui::CollapsingHeader("Details")) {
    ImGui::TextUnformatted(view.status_line.c_str());
    // Plan 14: the engine's own lines, which the rows no longer show under themselves.
    for (const std::string* line : {&view.placement_line, &view.motion_line, &view.work_line, &view.exposure_line}) {
      if (!line->empty()) {
        ImGui::TextUnformatted(line->c_str());
      }
    }
    if (!view.frame_generation_line.empty()) {
      ImGui::TextUnformatted(view.frame_generation_line.c_str());
    }
    if (!view.api_line.empty()) {
      ImGui::TextUnformatted(view.api_line.c_str());
    }
    ImGui::TextDisabled("%s", VramLine(view).c_str());
  }

  // G2: Restore all defaults, confirmed once before it takes effect.
  ImGui::Separator();
  if (ImGui::Button("Restore all defaults")) {
    state->confirm_restore = true;
  }
  if (state->confirm_restore) {
    ImGui::TextWrapped("Every setting returns to its default, the hotkey included; Enable stays as it is.");
    if (ImGui::Button("Restore all")) {
      RestoreAllDefaults(settings);
      changed = true;
      state->confirm_restore = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep my settings")) {
      state->confirm_restore = false;
    }
  }

  return changed;
}

}  // namespace uplift::ui
