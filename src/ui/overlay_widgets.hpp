#pragma once

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include "ui/overlay_style.hpp"
#include "ui/slider_row_layout.hpp"

namespace uplift::ui::widgets {

// The rail slider's look, in font heights: a rectangular track inset from the control's frame, filled up to the value, and a
// narrow rectangular grab at the value.
constexpr float SLIDER_TRACK_INSET_EM = 0.24f;
constexpr float SLIDER_GRAB_WIDTH_EM = 0.5f;
constexpr float SLIDER_GRAB_INSET_EM = 0.05f;
constexpr float SLIDER_ROUNDING_EM = 0.1f;
constexpr float SLIDER_TRACK_ALPHA = 0.3f;  // of the muted text colour
constexpr float SLIDER_FILL_ALPHA = 0.45f;  // of the accent
// A grouped slider row on one line: the most of the row its caption column may take and the most lines a caption may wrap to there, and
// the slider's least width (style units). A caption needing more lines, or a slider narrower than that, stacks.
constexpr float SLIDER_ROW_CAPTION_SHARE = 0.42f;
constexpr size_t SLIDER_ROW_CAPTION_MAX_LINES = 2u;
constexpr float SLIDER_ROW_MIN_WIDTH_UNITS = 16.f;
// Between a slider row's caption column and its slider (style units): wider than the row's other gaps, so a caption never reads as part
// of the track.
constexpr float SLIDER_ROW_CAPTION_GAP_UNITS = 2.f;

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
    const float em = gui::GetFontSize();
    const float half_grab = std::min(em * SLIDER_GRAB_WIDTH_EM, b.x - a.x) * 0.5f, rounding = em * SLIDER_ROUNDING_EM;
    const float start = a.x + half_grab, end = b.x - half_grab;
    const float t = static_cast<float>(value - low) / static_cast<float>(high - low);
    const float x = start + (end - start) * std::clamp(t, 0.f, 1.f);
    const float track_top = a.y + em * SLIDER_TRACK_INSET_EM, track_bottom = b.y - em * SLIDER_TRACK_INSET_EM;
    auto* draw = gui::GetWindowDrawList();
    draw->AddRectFilled({a.x, track_top}, {b.x, track_bottom}, gui::GetColorU32(ImGuiCol_TextDisabled, SLIDER_TRACK_ALPHA), rounding);
    draw->AddRectFilled({a.x, track_top}, {x, track_bottom}, gui::GetColorU32(ImGuiCol_SliderGrab, SLIDER_FILL_ALPHA), rounding);
    draw->AddRectFilled({x - half_grab, a.y + em * SLIDER_GRAB_INSET_EM}, {x + half_grab, b.y - em * SLIDER_GRAB_INSET_EM}, gui::GetColorU32(ImGuiCol_SliderGrab), rounding);
    if (gui::IsItemFocused() && gui::GetIO().NavVisible) draw->AddRect(a, b, gui::GetColorU32(ImGuiCol_Text), gui::GetStyle().FrameRounding);
  }
  return edited;
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
  const float pad_x = style::PanelInset() * 0.5f, height = style::Units(style::DISCLOSURE_HEIGHT_UNITS);
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
  const float reserve = has_reset ? gui::ResetButtonWidth() + gui::GetStyle().ItemSpacing.x : 0.f;
  if (value.empty() && has_reset) gui::PushTextWrapPos(gui::GetCursorPosX() + std::max(gui::GetFontSize(), gui::GetContentRegionAvail().x - reserve));
  gui::TextUnformatted(label);
  if (value.empty() && has_reset) gui::PopTextWrapPos();
  if (value.empty()) return;
  gui::SameLine();
  const float needed = ImGui::CalcTextSize(value.c_str()).x + reserve;
  if (gui::GetContentRegionAvail().x < needed) gui::NewLine();
  gui::SetCursorPosX(gui::GetCursorPosX() + std::max(0.f, gui::GetContentRegionAvail().x - needed));
  gui::TextColored(gui::GetStyleColorVec4(ImGuiCol_SliderGrab), "%s", value.c_str());
}

// The caption and value columns one panel's slider rows share: as wide as the widest caption and value its rows measured on the previous
// frame, the way ImGui sizes a table column to its content, so the panel's sliders line up and start right after the captions. Inset
// opens one around each panel's contents; a row outside any sizes the columns to itself.
class SliderColumns {
 public:
  explicit SliderColumns(ImGuiID id) : id_(id), previous_(current_) {
    if (const auto found = Widths().find(id); found != Widths().end()) last_ = found->second;
    current_ = this;
  }
  ~SliderColumns() {
    Widths()[id_] = measured_;
    current_ = previous_;
  }
  SliderColumns(const SliderColumns&) = delete;
  SliderColumns& operator=(const SliderColumns&) = delete;

  // The open panel's columns, or nullptr.
  static SliderColumns* Current() { return current_; }
  // A row's caption (with its gap) and value widths: counted for the next frame, and returned widened to the panel's last frame.
  ImVec2 Fit(ImVec2 needed) {
    measured_ = {std::max(measured_.x, needed.x), std::max(measured_.y, needed.y)};
    return {std::max(last_.x, needed.x), std::max(last_.y, needed.y)};
  }

 private:
  static std::unordered_map<ImGuiID, ImVec2>& Widths() {
    static std::unordered_map<ImGuiID, ImVec2> widths;
    return widths;
  }
  inline static thread_local SliderColumns* current_ = nullptr;
  ImGuiID id_;
  SliderColumns* previous_;
  ImVec2 last_{0.f, 0.f};
  ImVec2 measured_{0.f, 0.f};
};

// Where a grouped slider row's parts go when it fits one line (window-local positions), else one_line is false and the row stacks.
struct SliderRowPlan {
  bool one_line = false;
  std::string caption;  // the shown caption, wrapped to its share of the row
  size_t caption_lines = 1u;
  float control_y = 0.f;  // the top of the slider and the reset button, centred on a wrapped caption
  float slider_x = 0.f, slider_width = 0.f, value_right = 0.f, reset_x = 0.f;
};

// Plans a row starting at the cursor: caption, slider, the value right-aligned, then the reset button's place. `bounds` are the value's
// text at both ends of the slider, so the value column never moves while it is dragged; the open panel's SliderColumns line its rows up.
inline SliderRowPlan PlanSliderRow(const char* caption, const std::string& value, std::span<const std::string> bounds) {
  const float row_x = gui::GetCursorPosX(), row_width = gui::GetContentRegionAvail().x, gap = style::RowGap();
  const float caption_gap = style::Units(SLIDER_ROW_CAPTION_GAP_UNITS);
  const std::string shown = gui::Display(caption);
  ImVec2 needed{ImGui::CalcTextSize(shown.c_str()).x + caption_gap, ImGui::CalcTextSize(value.c_str()).x};
  for (const std::string& bound : bounds) needed.y = std::max(needed.y, ImGui::CalcTextSize(bound.c_str()).x);
  if (SliderColumns* const columns = SliderColumns::Current()) needed = columns->Fit(needed);
  const float reset_width = gui::ResetButtonWidth();
  const SliderRowWidths widths = SliderRowLayout(row_width, needed.x, SLIDER_ROW_CAPTION_SHARE, needed.y, reset_width, gap);
  SliderRowPlan plan;
  plan.caption = WrapLabel(shown, widths.caption - caption_gap);
  plan.caption_lines = 1u + static_cast<size_t>(std::ranges::count(plan.caption, '\n'));
  const ImVec2 caption_size = ImGui::CalcTextSize(plan.caption.c_str());
  plan.slider_width = widths.slider;
  plan.one_line = plan.caption_lines <= SLIDER_ROW_CAPTION_MAX_LINES && caption_size.x + caption_gap <= widths.caption
                  && plan.slider_width >= style::Units(SLIDER_ROW_MIN_WIDTH_UNITS);
  plan.control_y = gui::GetCursorPosY() + std::max(0.f, (caption_size.y - gui::GetFrameHeight()) * 0.5f);
  plan.slider_x = row_x + widths.caption;
  plan.reset_x = row_x + row_width - reset_width;
  plan.value_right = plan.reset_x - gap;
  return plan;
}

// A one-line row's caption, and its value right-aligned before the reset button's place. The value is drawn, not an item, so the slider
// placed after it stays the last item for its tooltip and its active state.
inline void SliderRowCaption(const SliderRowPlan& plan, const std::string& value) {
  if (plan.caption_lines == 1u) gui::AlignTextToFramePadding();
  ImGui::TextUnformatted(plan.caption.c_str());
  const ImVec2 origin{gui::GetWindowPos().x - gui::GetScrollX(), gui::GetWindowPos().y - gui::GetScrollY()};
  const ImVec2 at{origin.x + plan.value_right - ImGui::CalcTextSize(value.c_str()).x, origin.y + plan.control_y + gui::GetStyle().FramePadding.y};
  gui::GetWindowDrawList()->AddText(at, gui::GetColorU32(ImGuiCol_SliderGrab), value.c_str());
}

// Moves the cursor back onto a one-line row's control line at `x` (window-local), for the reset button and then the slider.
inline void ToSliderRowControl(const SliderRowPlan& plan, float x) {
  gui::SameLine();
  gui::SetCursorPosX(x);
  gui::SetCursorPosY(plan.control_y);
}

// Percent marks under a slider: under a one-line row's slider, else across the whole row.
inline void PercentTicks(double low, double high, const SliderRowPlan& row) {
  const float offset = row.one_line ? row.slider_x - gui::GetCursorPosX() : 0.f;
  const ImVec2 pos{gui::GetCursorScreenPos().x + offset, gui::GetCursorScreenPos().y};
  const float width = row.one_line ? row.slider_width : gui::GetContentRegionAvail().x;
  for (int i = 0; i < 4; ++i) {
    const float t = i / 3.f;
    const auto label = std::format("{:.0f}%", low + t * (high - low));
    const float text_width = ImGui::CalcTextSize(label.c_str()).x;
    const float x = std::clamp(width * t - text_width * 0.5f, 0.f, std::max(0.f, width - text_width));
    gui::GetWindowDrawList()->AddText({pos.x + x, pos.y}, gui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
  }
  gui::Dummy({0.f, gui::GetTextLineHeight()});
}

// A rule inside a panel, between groups of rows or under a group's heading, and the gap under it that mirrors the gap above.
inline void GroupRule() {
  gui::Separator();
  gui::SetCursorPosY(gui::GetCursorPosY() + style::Units(style::GROUP_RULE_GAP_UNITS));
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
  GroupRule();
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
  const SliderColumns columns(gui::GetID(id));
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
