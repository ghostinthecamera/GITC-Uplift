#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ui/localized_imgui.hpp"

namespace uplift::ui::style {

inline ImVec4 PassAccent(uint32_t first, size_t stage) {
  return gui::GetStyleColorVec4(first == 0 ? ImGuiCol_CheckMark : stage == 1 ? ImGuiCol_PlotHistogram : ImGuiCol_PlotLinesHovered);
}

// Safety locks always use red, with the shade chosen for the host background.
inline ImVec4 SafetyRed() {
  const ImVec4 bg = gui::GetStyleColorVec4(ImGuiCol_WindowBg);
  return bg.x + bg.y + bg.z > 1.5f ? ImVec4(0.72f, 0.08f, 0.08f, 1.f) : ImVec4(1.f, 0.42f, 0.39f, 1.f);
}

inline ImVec4 SelectedFill(ImVec4 accent) {
  const ImVec4 base = gui::GetStyleColorVec4(ImGuiCol_WindowBg);
  constexpr float tint = 0.28f;
  return {base.x + tint * (accent.x - base.x), base.y + tint * (accent.y - base.y), base.z + tint * (accent.z - base.z), base.w};
}

class GroupStyle {
 public:
  GroupStyle() {
    const auto& theme = gui::GetStyle();
    const float em = gui::GetFontSize(), pad = em * 0.12f;
    const auto base = theme.Colors[ImGuiCol_WindowBg], accent = theme.Colors[ImGuiCol_CheckMark];
    gui::PushStyleColor(ImGuiCol_Button, base);
    gui::PushStyleColor(ImGuiCol_FrameBg, base);
    gui::PushStyleColor(ImGuiCol_ButtonActive, SelectedFill(accent));
    gui::PushStyleColor(ImGuiCol_SliderGrab, accent);
    gui::PushStyleColor(ImGuiCol_SliderGrabActive, accent);
    gui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.75f * em, 0.55f * em});
    gui::PushStyleVar(ImGuiStyleVar_FramePadding, {theme.FramePadding.x + pad, theme.FramePadding.y + pad});
    gui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {theme.ItemInnerSpacing.x + pad, theme.ItemInnerSpacing.y});
    gui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {theme.ItemSpacing.x, theme.ItemSpacing.y + pad});
    gui::PushStyleVar(ImGuiStyleVar_FrameRounding, std::max(theme.FrameRounding, em * 0.28f));
    gui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(theme.FrameBorderSize, 1.f));
  }
  ~GroupStyle() {
    gui::PopStyleVar(6);
    gui::PopStyleColor(5);
  }
  GroupStyle(const GroupStyle&) = delete;
  GroupStyle& operator=(const GroupStyle&) = delete;
};

inline ImU32 GroupSurface() { return gui::GetColorU32(ImGuiCol_Text, 0.065f); }
inline bool FitPairedGroups() { return gui::GetContentRegionAvail().x >= gui::GetFontSize() * 44.f; }

}  // namespace uplift::ui::style
