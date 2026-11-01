#pragma once

#include <format>
#include <span>
#include <string_view>

#include "ui/overlay_style.hpp"

namespace uplift::ui::widgets {

inline int SegmentedControl(const char* label, std::span<const char* const> options, std::span<const std::string_view> why,
                            std::span<const std::string_view> tips, int selected, std::span<const size_t> display_order = {}) {
  const ImGuiStyle& style = gui::GetStyle();
  const float total_width = label ? gui::CalcItemWidth() : gui::GetContentRegionAvail().x;
  const float gap = style.ItemInnerSpacing.x;
  const size_t count = options.size();
  float labels_width = 0.f;
  if (!label) {
    for (const char* option : options) labels_width += gui::CalcTextSize(option).x + 2.f * style.FramePadding.x;
  }
  const float button_width = (total_width - static_cast<float>(count - 1u) * gap) / static_cast<float>(count);
  const float label_x = gui::GetCursorPosX() + total_width + gap;
  int clicked = -1;
  for (size_t column = 0u; column < count; ++column) {
    const size_t index = display_order.empty() ? column : display_order[column];
    if (column > 0u) {
      gui::SameLine(0.f, gap);
    }
    gui::PushID(static_cast<int>(index));
    const bool selected_option = (selected == static_cast<int>(index));
    if (selected_option) {
      gui::PushStyleColor(ImGuiCol_Button, gui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    gui::BeginDisabled(!why[index].empty());
    float width = button_width;
    if (!label && labels_width > 0.f) {
      const float inner = std::max(1.f, total_width - (count - 1u) * gap);
      const float need = gui::CalcTextSize(options[index]).x + 2.f * style.FramePadding.x;
      width = inner >= labels_width ? need + (inner - labels_width) / count : inner * need / labels_width;
    }
    if (gui::Button(options[index], ImVec2(width, 0.f))) {
      clicked = static_cast<int>(index);
    }
    if (!label && selected_option) {
      const auto a = gui::GetItemRectMin(), b = gui::GetItemRectMax();
      gui::GetWindowDrawList()->AddLine({a.x + 2.f, b.y - 1.f}, {b.x - 2.f, b.y - 1.f}, gui::GetColorU32(ImGuiCol_SliderGrab), 2.f);
    }
    gui::EndDisabled();
    if (selected_option) {
      gui::PopStyleColor();
    }
    if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      std::string tooltip;
      if (!why[index].empty()) {
        tooltip = std::format("Unavailable here: {}", why[index]);
        if (!tips[index].empty()) {
          tooltip += "\n";
        }
      }
      tooltip += tips[index];
      if (!tooltip.empty()) {
        gui::SetTooltip("%s", tooltip.c_str());
      }
    }
    gui::PopID();
  }
  if (label) {
    gui::SameLine();
    gui::SetCursorPosX(std::max(gui::GetCursorPosX(), label_x));
    gui::TextUnformatted(label);
  }
  return clicked;
}

// The native slider owns mouse, keyboard and Ctrl+click input. Keep the native text field visible during typed edits.
inline bool RailSlider(const char* label, float& value, float low, float high, const char* format, ImGuiSliderFlags flags = 0) {
  const ImGuiID id = gui::GetID(gui::Label(label).c_str());
  auto* storage = gui::GetStateStorage();
  const int frame = gui::GetFrameCount();
  const bool typing = gui::GetIO().WantTextInput && storage->GetInt(id, -2) == frame - 1;
  if (!typing) gui::PushStyleVar(ImGuiStyleVar_Alpha, 0.f);
  const bool edited = gui::SliderFloat(label, &value, low, high, format, flags);
  if (gui::IsItemActive()) storage->SetInt(id, frame);
  if (!typing) {
    gui::PopStyleVar();
    const ImVec2 a = gui::GetItemRectMin(), b = gui::GetItemRectMax();
    const float radius = std::min(gui::GetFontSize() * 0.43f, (b.x - a.x) * 0.25f);
    const float y = (a.y + b.y) * 0.5f, start = a.x + radius, end = b.x - radius;
    const float t = (value - low) / (high - low);
    const float x = start + (end - start) * std::clamp(t, 0.f, 1.f);
    auto* draw = gui::GetWindowDrawList();
    const ImU32 accent = gui::GetColorU32(ImGuiCol_SliderGrab);
    const float half = gui::GetFontSize() * 0.15f;
    draw->AddRectFilled({start, y - half}, {end, y + half}, gui::GetColorU32(ImGuiCol_TextDisabled, 0.5f), half);
    draw->AddRectFilled({start, y - half}, {x, y + half}, accent, half);
    draw->AddCircleFilled({x, y}, radius, accent);
    if (gui::IsItemFocused() && gui::GetIO().NavVisible) draw->AddRect(a, b, gui::GetColorU32(ImGuiCol_Text), radius);
  }
  return edited;
}

inline void PercentTicks(double low, double high) {
  const ImVec2 pos = gui::GetCursorScreenPos();
  const float width = gui::GetContentRegionAvail().x;
  for (int i = 0; i < 4; ++i) {
    const float t = i / 3.f;
    const auto label = std::format("{:.0f}%", low + t * (high - low));
    const float text_width = ImGui::CalcTextSize(label.c_str()).x;
    const float x = std::clamp(width * t - text_width * 0.5f, 0.f, std::max(0.f, width - text_width));
    gui::GetWindowDrawList()->AddText({pos.x + x, pos.y}, gui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
  }
  gui::Dummy({0.f, gui::GetTextLineHeight()});
}

inline bool SectionHeading(const char* title, const char* id, bool show_reset) {
  if (!show_reset) {
    gui::SeparatorText(title);
    return false;
  }
  const auto& theme = gui::GetStyle();
  const auto reset_label = std::format("Reset##{}", id);
  const ImVec2 start = gui::GetCursorScreenPos();
  const float width = gui::GetContentRegionAvail().x, gap = theme.ItemSpacing.x;
  const float content = gui::CalcTextSize(title).x + gap + gui::ResetButtonWidth(reset_label.c_str());
  const float spare = width - content - 2.f * theme.SeparatorTextPadding.x;
  if (spare < 0.f) {
    gui::SeparatorText(title);
    return gui::ResetButton(reset_label.c_str());
  }

  gui::SetCursorPosX(gui::GetCursorPosX() + theme.SeparatorTextPadding.x + spare * theme.SeparatorTextAlign.x);
  gui::AlignTextToFramePadding();
  gui::TextUnformatted(title);
  const float before_title = gui::GetItemRectMin().x - gap;
  gui::SameLine();
  const bool reset = gui::ResetButton(reset_label.c_str());
  const ImVec2 button_min = gui::GetItemRectMin(), button_max = gui::GetItemRectMax();
  const float y = (button_min.y + button_max.y) * 0.5f;
  const auto rule = [&](float from, float to) {
    if (to > from && theme.SeparatorTextBorderSize > 0.f)
      gui::GetWindowDrawList()->AddLine({from, y}, {to, y}, gui::GetColorU32(ImGuiCol_Separator), theme.SeparatorTextBorderSize);
  };
  rule(start.x, before_title);
  rule(button_max.x + gap, start.x + width);
  return reset;
}

inline void SettingCaption(const char* label, const std::string& value, bool has_reset) {
  gui::AlignTextToFramePadding();
  gui::TextUnformatted(label);
  if (value.empty()) return;
  gui::SameLine();
  const float reserve = has_reset ? gui::ResetButtonWidth() + gui::GetStyle().ItemSpacing.x : 0.f;
  const float needed = ImGui::CalcTextSize(value.c_str()).x + reserve;
  if (gui::GetContentRegionAvail().x < needed) gui::NewLine();
  gui::SetCursorPosX(gui::GetCursorPosX() + std::max(0.f, gui::GetContentRegionAvail().x - needed));
  gui::TextColored(gui::GetStyleColorVec4(ImGuiCol_SliderGrab), "%s", value.c_str());
}

inline void GroupHeading(const char* title, const char* note = nullptr) {
  gui::AlignTextToFramePadding();
  gui::TextUnformatted(title);
  if (note) {
    gui::SameLine();
    const float space = gui::GetContentRegionAvail().x - gui::CalcTextSize(note).x;
    if (space >= 0.f) gui::SetCursorPosX(gui::GetCursorPosX() + space);
    else gui::NewLine();
    gui::TextDisabled("%s", note);
  }
  gui::Separator();
}

template <typename Left, typename Right>
void PairedGroups(const char* id, Left left, Right right) {
  const bool paired = style::FitPairedGroups();
  const style::GroupStyle group_style;
  const auto flags = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings;
  if (!gui::BeginTable(id, paired ? 2 : 1, paired ? flags | ImGuiTableFlags_BordersInnerV : flags)) return;
  gui::TableNextRow();
  const auto cell = [](int column, auto& contents) {
    gui::TableSetColumnIndex(column);
    gui::TableSetBgColor(ImGuiTableBgTarget_CellBg, style::GroupSurface());
    gui::PushItemWidth(-1.f);
    contents();
    gui::PopItemWidth();
  };
  cell(0, left);
  if (!paired) {
    gui::PushStyleVar(ImGuiStyleVar_CellPadding, {gui::GetStyle().CellPadding.x, 0.f});
    gui::TableNextRow(ImGuiTableRowFlags_None, style::GroupGap());
    gui::PopStyleVar();
    gui::TableNextRow();
  }
  cell(paired ? 1 : 0, right);
  gui::EndTable();
}

}  // namespace uplift::ui::widgets
