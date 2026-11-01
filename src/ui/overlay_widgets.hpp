#pragma once

#include <format>
#include <span>
#include <string_view>
#include <type_traits>

#include "ui/overlay_style.hpp"

namespace uplift::ui::widgets {

// ImGui checkboxes and buttons do not wrap their own labels. Wrap the display text but keep the original widget ID.
inline std::string WrapLabel(std::string_view text, float width) {
  const char* line = text.data();
  const char* end = line + text.size();
  std::string result;
  while (line < end) {
    const char* at = line;
    const char* space = nullptr;
    while (at < end && *at != '\n') {
      const char* next = at + 1;
      while (next < end && (static_cast<unsigned char>(*next) & 0xc0) == 0x80) ++next;
      if (at > line && ImGui::CalcTextSize(line, next).x > width) break;
      if (*at == ' ') space = at;
      at = next;
    }
    if (at < end && *at != '\n' && *at != ' ' && space && space > line) at = space;
    result.append(line, at);
    line = at;
    while (line < end && *line == ' ') ++line;
    if (line < end) {
      if (*line == '\n') ++line;
      result += '\n';
    }
  }
  return result;
}

inline bool Checkbox(const char* label, bool* value, bool has_reset) {
  const float reserved = has_reset ? gui::ResetButtonWidth() + style::RowGap() : 0.f;
  const float width = gui::GetContentRegionAvail().x - gui::GetFrameHeight() - gui::GetStyle().ItemInnerSpacing.x - reserved;
  if (gui::CalcTextSize(label).x <= width) return gui::Checkbox(label, value);
  const std::string wrapped = WrapLabel(gui::Display(label), width) + "###" + label;
  return ImGui::Checkbox(wrapped.c_str(), value);
}

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
    const float text_width = width - 2.f * style.FramePadding.x;
    const std::string shown = gui::Display(options[index]);
    const bool wrap = !label && ImGui::CalcTextSize(shown.c_str()).x > text_width;
    const bool pressed = wrap ? ImGui::Button((WrapLabel(shown, text_width) + "###" + options[index]).c_str(), {width, 0.f})
                              : gui::Button(options[index], {width, 0.f});
    if (pressed) {
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
template <typename T>
bool RailSlider(const char* label, T& value, T low, T high, const char* format, ImGuiSliderFlags flags = 0) {
  const ImGuiID id = gui::GetID(gui::Label(label).c_str());
  auto* storage = gui::GetStateStorage();
  const int frame = gui::GetFrameCount();
  const bool typing = gui::GetIO().WantTextInput && storage->GetInt(id, -2) == frame - 1;
  if (!typing) gui::PushStyleVar(ImGuiStyleVar_Alpha, 0.f);
  bool edited;
  if constexpr (std::is_same_v<T, int>) edited = gui::SliderInt(label, &value, low, high, format, flags);
  else edited = gui::SliderFloat(label, &value, low, high, format, flags);
  if (gui::IsItemActive()) storage->SetInt(id, frame);
  if (!typing) {
    gui::PopStyleVar();
    const ImVec2 a = gui::GetItemRectMin(), b = gui::GetItemRectMax();
    const float radius = std::min(gui::GetFontSize() * 0.43f, (b.x - a.x) * 0.25f);
    const float y = (a.y + b.y) * 0.5f, start = a.x + radius, end = b.x - radius;
    const float t = static_cast<float>(value - low) / static_cast<float>(high - low);
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

inline bool Disclosure(const char* title, const char* hint = nullptr, const char* reset_id = nullptr, bool* reset = nullptr) {
  const auto base = gui::GetStyleColorVec4(ImGuiCol_WindowBg), text = gui::GetStyleColorVec4(ImGuiCol_Text);
  gui::PushStyleColor(ImGuiCol_Header, ImVec4(base.x+(text.x-base.x)*0.065f, base.y+(text.y-base.y)*0.065f, base.z+(text.z-base.z)*0.065f, base.w));
  gui::PushStyleVar(ImGuiStyleVar_FramePadding, {style::PanelInset(), style::RowGap()});
  const bool open = gui::CollapsingHeader(title, ImGuiTreeNodeFlags_AllowOverlap);
  const ImVec2 a = gui::GetItemRectMin(), b = gui::GetItemRectMax();
  gui::PopStyleVar();
  gui::PopStyleColor();
  const ImVec2 next = gui::GetCursorScreenPos();
  const std::string label = gui::Label(title);
  const float text_end = a.x + gui::GetFontSize() + 3.f * style::PanelInset() + ImGui::CalcTextSize(label.c_str(), nullptr, true).x;
  float free_from = text_end + style::RowGap();
  if (reset && reset_id) {
    const auto id = std::format("Reset##{}", reset_id);
    const float width = gui::ResetButtonWidth(id.c_str());
    if (free_from + width + style::PanelInset() <= b.x) {
      gui::SetCursorScreenPos({free_from, a.y + ((b.y - a.y) - gui::GetFrameHeight()) * 0.5f});
      *reset = gui::ResetButton(id.c_str());
      gui::SetCursorScreenPos(next);
      free_from += width + style::RowGap();
    } else {
      *reset = gui::ResetButton(id.c_str());
    }
  }
  if (hint) {
    const std::string shown = gui::Display(hint);
    const ImVec2 size = ImGui::CalcTextSize(shown.c_str());
    const float x = b.x - style::PanelInset() - size.x;
    if (x >= free_from) gui::GetWindowDrawList()->AddText({x, (a.y+b.y-size.y)*0.5f}, gui::GetColorU32(ImGuiCol_TextDisabled), shown.c_str());
  }
  return open;
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

// The preceding item has already advanced by ItemSpacing.y. Replace that gap instead of adding a second margin.
inline void NextGap(float gap) {
  gui::SetCursorPosY(gui::GetCursorPosY() + gap - gui::GetStyle().ItemSpacing.y);
}

template <typename Contents>
void Inset(const char* id, ImVec2 padding, Contents contents, bool surface = false, float min_height = 0.f) {
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, padding);
  if (gui::BeginTable(id, 1, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings)) {
    gui::TableNextRow(ImGuiTableRowFlags_None, min_height);
    gui::TableSetColumnIndex(0);
    if (surface) gui::TableSetBgColor(ImGuiTableBgTarget_CellBg, style::GroupSurface());
    gui::PushItemWidth(-1.f);
    gui::PushTextWrapPos(0.f);
    contents();
    gui::PopTextWrapPos();
    gui::PopItemWidth();
    gui::EndTable();
  }
  gui::PopStyleVar();
}

template <typename Contents>
void Panel(const char* id, Contents contents) {
  const float inset = style::PanelInset();
  Inset(id, {inset, inset}, contents, true);
}

template <typename Left, typename Right>
void PairedGroups(const char* id, Left left, Right right) {
  const float inset = style::PanelInset();
  if (!style::FitPairedGroups()) {
    gui::PushID(id);
    Panel("##left", left);
    NextGap(inset);
    Panel("##right", right);
    gui::PopID();
    return;
  }
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, 0.f});
  if (gui::BeginTable(id, 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoSavedSettings)) {
    gui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthStretch);
    gui::TableSetupColumn("##gutter", ImGuiTableColumnFlags_WidthFixed, inset);
    gui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthStretch);
    gui::TableNextRow();
    const auto cell = [inset](int index, const char* name, auto& contents) {
      gui::TableSetColumnIndex(index);
      gui::TableSetBgColor(ImGuiTableBgTarget_CellBg, style::GroupSurface());
      Inset(name, {inset, inset}, contents);
    };
    cell(0, "##left_content", left);
    cell(2, "##right_content", right);
    gui::EndTable();
  }
  gui::PopStyleVar();
}

}  // namespace uplift::ui::widgets
