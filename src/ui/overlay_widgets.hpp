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
  const float inner = std::max(1.f, total_width - static_cast<float>(count - 1u) * gap);
  const auto option_width = [&](size_t index) {
    if (label || labels_width <= 0.f) return inner / count;
    const float need = gui::CalcTextSize(options[index]).x + 2.f * style.FramePadding.x;
    return inner >= labels_width ? need + (inner - labels_width) / count : inner * need / labels_width;
  };
  float row_height = gui::GetFrameHeight();
  if (!label && inner < labels_width) {
    for (size_t i = 0; i < count; ++i) {
      const auto wrapped = WrapLabel(gui::Display(options[i]), option_width(i) - 2.f * style.FramePadding.x);
      row_height = std::max(row_height, ImGui::CalcTextSize(wrapped.c_str()).y + 2.f * style.FramePadding.y);
    }
  }
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
    const float width = option_width(index);
    const float text_width = width - 2.f * style.FramePadding.x;
    const std::string shown = gui::Display(options[index]);
    const bool wrap = !label && ImGui::CalcTextSize(shown.c_str()).x > text_width;
    const bool pressed = wrap ? ImGui::Button((WrapLabel(shown, text_width) + "###" + options[index]).c_str(), {width, row_height})
                              : gui::Button(options[index], {width, row_height});
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

inline void PanelBorder(ImVec2 start, float width, float bottom) {
  gui::GetWindowDrawList()->AddRect(start, {start.x + width, bottom}, gui::GetColorU32(ImGuiCol_Text, 0.10f), gui::GetStyle().FrameRounding);
}

inline bool Disclosure(const char* title, const char* hint = nullptr, const char* reset_id = nullptr, bool* reset = nullptr) {
  const ImVec2 start = gui::GetCursorScreenPos();
  const float right = start.x + gui::GetContentRegionAvail().x;
  const float pad_x = style::PanelInset() * 0.5f, height = style::Units(8.5f);
  const std::string label = gui::Label(title);
  const size_t suffix = label.find("###");
  const float text_end = start.x + gui::GetFontSize() + 3.f * pad_x + ImGui::CalcTextSize(label.c_str(), nullptr, true).x;
  const std::string reset_label = reset_id ? std::format("Reset##{}", reset_id) : std::string();
  const float reset_x = text_end + style::RowGap(), reset_width = reset ? gui::ResetButtonWidth(reset_label.c_str()) : 0.f;
  const bool inline_reset = reset && reset_x + reset_width <= right - style::LabelGap();
  const std::string note = hint ? gui::Display(hint) : std::string();
  const float hint_x = right - style::PanelInset() - ImGui::CalcTextSize(note.c_str()).x;
  const bool wrap_note = !note.empty() && hint_x < (inline_reset ? reset_x + reset_width : text_end) + style::RowGap();
  std::string header = label;
  if (wrap_note) header.insert(suffix, "\n ");
  gui::PushClipRect(start, {right, start.y + height + gui::GetTextLineHeight() + style::RowGap()}, true);
  gui::PushStyleColor(ImGuiCol_Header, style::Surface());
  gui::PushStyleVar(ImGuiStyleVar_FramePadding, {pad_x, (height - gui::GetFontSize()) * 0.5f});
  gui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
  const bool open = gui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_AllowOverlap);
  const float bottom = gui::GetItemRectMax().y;
  gui::PopStyleVar(2);
  gui::PopStyleColor();
  gui::PopClipRect();
  PanelBorder(start, right-start.x, bottom);
  const ImVec2 next = gui::GetCursorScreenPos();
  const float by = (height - gui::GetFrameHeight()) * 0.5f;
  if (inline_reset) {
    gui::SetCursorScreenPos({reset_x, start.y + by});
    *reset = gui::ResetButton(reset_label.c_str());
    gui::SetCursorScreenPos(next);
  } else if (reset) {
    *reset = gui::ResetButton(reset_label.c_str());
  }
  if (!note.empty()) {
    const float y = start.y + (height - gui::GetFontSize()) * 0.5f;
    const ImVec2 pos = wrap_note ? ImVec2(start.x + gui::GetFontSize() + 3.f * pad_x, y + gui::GetTextLineHeight()) : ImVec2(hint_x, y);
    gui::GetWindowDrawList()->AddText(pos, gui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());
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
void Inset(const char* id, ImVec2 padding, Contents contents, ImU32 surface = 0, float min_height = 0.f) {
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, padding);
  const bool visible = gui::BeginTable(id, 1, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings);
  if (visible) {
    gui::TableNextRow(ImGuiTableRowFlags_None, min_height);
    gui::TableSetColumnIndex(0);
  }
  gui::PopStyleVar();  // The nested controls and their tables use their own spacing.
  if (!visible) return;
  if (surface) gui::TableSetBgColor(ImGuiTableBgTarget_CellBg, surface);
  gui::PushItemWidth(-1.f);
  gui::PushTextWrapPos(0.f);
  contents();
  gui::PopTextWrapPos();
  gui::PopItemWidth();
  gui::EndTable();
}

template <typename Contents>
void Panel(const char* id, Contents contents, ImU32 fill = style::GroupSurface()) {
  const float inset = style::PanelInset(), width = gui::GetContentRegionAvail().x;
  const ImVec2 start = gui::GetCursorScreenPos();
  Inset(id, {inset, inset}, contents, fill);
  PanelBorder(start, width, gui::GetCursorScreenPos().y - gui::GetStyle().ItemSpacing.y);
}

template <typename Left, typename Right>
void PairedGroups(const char* id, Left left, Right right, ImU32 fill = style::GroupSurface()) {
  const float inset = style::PanelInset();
  if (!style::FitPairedGroups()) {
    gui::PushID(id);
    Panel("##left", left, fill);
    NextGap(inset);
    Panel("##right", right, fill);
    gui::PopID();
    return;
  }
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, 0.f});
  if (gui::BeginTable(id, 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoSavedSettings)) {
    gui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthStretch);
    gui::TableSetupColumn("##gutter", ImGuiTableColumnFlags_WidthFixed, inset);
    gui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthStretch);
    gui::TableNextRow();
    ImVec2 starts[2];
    float widths[2];
    const auto cell = [&](int index, const char* name, auto& contents) {
      gui::TableSetColumnIndex(index);
      starts[index / 2] = gui::GetCursorScreenPos();
      widths[index / 2] = gui::GetContentRegionAvail().x;
      gui::TableSetBgColor(ImGuiTableBgTarget_CellBg, fill);
      Inset(name, {inset, inset}, contents);
    };
    cell(0, "##left_content", left);
    cell(2, "##right_content", right);
    gui::EndTable();
    const float bottom = gui::GetCursorScreenPos().y - gui::GetStyle().ItemSpacing.y;
    for (int i = 0; i < 2; ++i) PanelBorder(starts[i], widths[i], bottom);
  }
  gui::PopStyleVar();
}

}  // namespace uplift::ui::widgets
