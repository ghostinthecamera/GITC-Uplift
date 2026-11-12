#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ui/localized_imgui.hpp"

namespace uplift::ui::style {

// The overlay's layout measures. One unit is 4 px at ReShade's 13 px reference font, so every inset and gap follows the live font size.
constexpr float REFERENCE_FONT_PX = 13.f;
constexpr float UNIT_PX = 4.f;
constexpr float CONTROL_PADDING_UNITS = 1.f;      // a control's inner left and right inset
constexpr float CONTROL_HEIGHT_UNITS = 4.5f;      // buttons, combos, checkboxes and slider rows
constexpr float LABEL_GAP_UNITS = 0.5f;           // a caption to its control, and inside a control
constexpr float ROW_GAP_UNITS = 0.75f;            // between rows
constexpr float PANEL_INSET_UNITS = 1.5f;         // a panel's padding and the gutter between paired panels
constexpr float BLOCK_GAP_UNITS = 2.f;            // between blocks inside a section
constexpr float SECTION_GAP_UNITS = 3.f;          // between sections
constexpr float SEPARATOR_UNITS = 0.25f;          // a section heading's rule
constexpr float DISCLOSURE_HEIGHT_UNITS = 5.5f;   // a collapsible section's header
constexpr float GROUP_RULE_GAP_UNITS = 1.f;       // under a group heading's rule, before the group's first row
constexpr float PAIRED_GROUPS_MIN_WIDTH_UNITS = 112.f;  // narrower than this, paired panels stack

inline ImVec4 PassAccent(uint32_t first, size_t stage) {
  const auto accent = gui::GetStyleColorVec4(ImGuiCol_CheckMark);
  if (first == 0) return accent;
  // ReShade themes can assign the same colour to every plot and control. Derive distinct pass colours from the active accent.
  float h, s, v;
  ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, h, s, v);
  h += stage == 1 ? 1.f / 6.f : 0.5f;
  ImVec4 result = accent;
  ImGui::ColorConvertHSVtoRGB(h >= 1.f ? h - 1.f : h, std::max(s, 0.35f), v, result.x, result.y, result.z);
  return result;
}

// Safety locks always use red, with the shade chosen for the host background.
inline ImVec4 SafetyRed() {
  const ImVec4 bg = gui::GetStyleColorVec4(ImGuiCol_WindowBg);
  return bg.x + bg.y + bg.z > 1.5f ? ImVec4(0.72f, 0.08f, 0.08f, 1.f) : ImVec4(1.f, 0.42f, 0.39f, 1.f);
}

inline ImVec4 Tint(ImVec4 accent, float tint) {
  const ImVec4 base = gui::GetStyleColorVec4(ImGuiCol_WindowBg);
  return {base.x + tint * (accent.x - base.x), base.y + tint * (accent.y - base.y), base.z + tint * (accent.z - base.z), base.w};
}

inline ImVec4 SelectedFill(ImVec4 accent) { return Tint(accent, 0.28f); }

// ReShade's reference UI font is 13 px. Resolve every inset and gap from the live font size.
inline float Units(float count) { return gui::GetFontSize() * (UNIT_PX / REFERENCE_FONT_PX) * count; }
inline float ControlPadding() { return Units(CONTROL_PADDING_UNITS); }
inline float ControlHeight() { return Units(CONTROL_HEIGHT_UNITS); }
inline float LabelGap() { return Units(LABEL_GAP_UNITS); }
inline float RowGap() { return Units(ROW_GAP_UNITS); }
inline float PanelInset() { return Units(PANEL_INSET_UNITS); }
inline float BlockGap() { return Units(BLOCK_GAP_UNITS); }
inline float SectionGap() { return Units(SECTION_GAP_UNITS); }
inline bool FitPairedGroups() { return gui::GetContentRegionAvail().x >= Units(PAIRED_GROUPS_MIN_WIDTH_UNITS); }
inline ImVec4 Surface(float tint = 0.065f) {
  return Tint(gui::GetStyleColorVec4(ImGuiCol_Text), tint);
}
inline ImU32 GroupSurface() { return gui::GetColorU32(Surface()); }
inline ImU32 WellSurface() { return gui::GetColorU32(Surface(0.025f)); }

class OverlayStyle {
 public:
  OverlayStyle() {
    const auto& theme = gui::GetStyle();
    const auto base = theme.Colors[ImGuiCol_WindowBg], accent = theme.Colors[ImGuiCol_CheckMark];
    gui::PushStyleColor(ImGuiCol_Button, base);
    gui::PushStyleColor(ImGuiCol_FrameBg, base);
    gui::PushStyleColor(ImGuiCol_ButtonActive, SelectedFill(accent));
    gui::PushStyleColor(ImGuiCol_SliderGrab, accent);
    gui::PushStyleColor(ImGuiCol_SliderGrabActive, accent);
    gui::PushStyleVar(ImGuiStyleVar_FramePadding, {ControlPadding(), (ControlHeight() - gui::GetFontSize()) * 0.5f});
    gui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {LabelGap(), LabelGap()});
    gui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {RowGap(), RowGap()});
    gui::PushStyleVar(ImGuiStyleVar_SeparatorTextBorderSize, Units(SEPARATOR_UNITS));
    gui::PushStyleVar(ImGuiStyleVar_FrameRounding, std::max(theme.FrameRounding, LabelGap()));
    gui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(theme.FrameBorderSize, 1.f));
  }
  ~OverlayStyle() {
    gui::PopStyleVar(6);
    gui::PopStyleColor(5);
  }
  OverlayStyle(const OverlayStyle&) = delete;
  OverlayStyle& operator=(const OverlayStyle&) = delete;
};

}  // namespace uplift::ui::style
