#include "ui/overlay.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string>

#include "ui/localized_imgui.hpp"
#include "ui/settings_schema.hpp"

namespace uplift::ui {
namespace {

constexpr std::array<const char*, 4> TIPS{
    "Run pass 1 on the game's render-resolution input before DLSS upscales it.",
    "Run pass 1 on the upscaled image before the game's remaining post-processing.",
    "Run Present passes on the finished real frame before Streamline frame generation.",
    "Run Present passes at ReShade's Present placement. With downstream ReShade this includes generated frames."};

void CenterText(ImDrawList& draw, ImVec2 center, ImU32 color, const std::string& label) {
  const ImVec2 size = ImGui::CalcTextSize(label.c_str());
  draw.AddText({center.x - size.x * 0.5f, center.y - size.y * 0.5f}, color, label.c_str());
}

void Arrow(ImDrawList& draw, ImVec2 from, ImVec2 to, ImU32 color, float head) {
  draw.AddLine(from, to, color);
  if (to.x == from.x) {
    draw.AddTriangleFilled(to, {to.x - head, to.y - head * 1.6f}, {to.x + head, to.y - head * 1.6f}, color);
  } else {
    draw.AddTriangleFilled(to, {to.x - head * 1.6f, to.y - head}, {to.x - head * 1.6f, to.y + head}, color);
  }
}

ImVec4 RunColor(uint32_t first, size_t stage) {
  if (first == 0) return {0.32f, 0.82f, 0.72f, 1.f};
  if (stage == 1) return {0.71f, 0.58f, 0.91f, 1.f};
  return {0.91f, 0.70f, 0.42f, 1.f};
}

}  // namespace

bool DrawStagePicker(const OverlayView& view, Settings* settings, OverlayState* state) {
  gui::PushID("Source");
  gui::TextUnformatted("NR stage");
  gui::SameLine();
  gui::TextDisabled("Choose a stage for pass 1");
  bool changed = false;
  if (!IsDefault(*settings, *FindSetting("Source")) || !IsDefault(*settings, *FindSetting("PreUpscale"))) {
    gui::SameLine();
    if (gui::SmallButton("<")) {
      const auto pick = view.dlss_explicit ? SourcePick::PRESENT : SourcePick::AFTER_DLSS;
      SetSourcePick(settings, pick, view.dlss_explicit);
      state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(pick), view.setup);
      changed = true;
    }
    if (gui::IsItemHovered()) gui::SetTooltip(view.dlss_explicit ? "Reset to default (Present on Vulkan)" : "Reset to default (After DLSS)");
  }
  const float em = gui::GetFontSize();
  const ImVec2 origin = gui::GetCursorScreenPos();
  const float width = std::max(1.f, gui::GetContentRegionAvail().x);
  const bool folded = width < em * 43.f;
  const int columns = folded ? 3 : 6;
  const float margin = std::min(0.55f * em, width * 0.025f);
  const float gap = std::min(1.05f * em, width * 0.04f);
  const float cell = std::max(1.f, (width - 2.f * margin - gap * (columns - 1)) / columns);
  const float height = em * 3.0f, pitch = cell + gap, row = em * 9.6f;
  const float button_h = em * 2.35f, hook_y = em * 3.f;
  const float button_w = std::max(1.f, std::min(9.2f * em, pitch - em * 0.28f));
  const float total_height = (folded ? row : 0.f) + height * 0.5f + hook_y + button_h + em * 2.55f;
  gui::Dummy({width, total_height});
  const ImVec2 end = gui::GetCursorScreenPos();
  ImDrawList& draw = *gui::GetWindowDrawList();
  const ImU32 wire = gui::GetColorU32(ImGuiCol_TextDisabled);
  const ImU32 ink = gui::GetColorU32(ImGuiCol_Text);
  const ImU32 surface = gui::GetColorU32(ImGuiCol_FrameBg);
  const float head = em * 0.17f;
  std::array<ImVec2, 6> centers{};
  for (int i = 0; i < 6; ++i) centers[i] = {origin.x + margin + cell * 0.5f + (i % columns) * pitch,
                                              origin.y + height * 0.5f + em * 0.3f + (i / columns) * row};

  for (int i = 0; i < 5; ++i) {
    const ImVec2 from{centers[i].x + cell * 0.5f, centers[i].y};
    const ImVec2 to{centers[i + 1].x - cell * 0.5f - 1.f, centers[i + 1].y};
    if (folded && i == 2) {
      const float right = origin.x + width - 1.f, left = origin.x + 1.f, mid = to.y - height * 0.59f;
      const ImVec2 turn[]{from, {right, from.y}, {right, mid}, {left, mid}, {left, to.y}};
      draw.AddPolyline(turn, 5, wire, ImDrawFlags_None, 1.f);
      Arrow(draw, turn[4], to, wire, head);
    } else {
      Arrow(draw, from, to, wire, head);
    }
  }

  const std::array<const char*, 6> nodes{"Game\nscene", "Ray\nReconstruction", "DLSS\nupscaling", "Game PP\nand HUD", "Frame\ngeneration", "Present\nfor display"};
  const std::array<const char*, 6> compact{"Game", "RR", "DLSS", "PP + HUD", "FG", "Display"};
  for (int i = 0; i < 6; ++i) {
    const ImVec2 c = centers[i], lo{c.x - cell * 0.5f, c.y - height * 0.5f}, hi{c.x + cell * 0.5f, c.y + height * 0.5f};
    if (i == 4) {
      const float inset = std::min(cell * 0.12f, em * 0.75f);
      const ImVec2 hex[]{{lo.x + inset, lo.y}, {hi.x - inset, lo.y}, {hi.x, c.y}, {hi.x - inset, hi.y}, {lo.x + inset, hi.y}, {lo.x, c.y}};
      draw.AddConvexPolyFilled(hex, 6, surface);
      draw.AddPolyline(hex, 6, wire, ImDrawFlags_Closed, 1.f);
    } else {
      draw.AddRectFilled(lo, hi, surface, em * 0.35f);
      draw.AddRect(lo, hi, wire, em * 0.35f);
    }
    std::string label = gui::Display(nodes[i]);
    if (ImGui::CalcTextSize(label.c_str()).x > cell - em * (i == 4 ? 1.5f : 0.35f)) label = gui::Display(compact[i]);
    CenterText(draw, c, (i == 1 && !view.setup.before_upscaling_on_rr) || (i == 4 && !view.frame_generation_active) ? wire : ink, label);
    if (gui::IsWindowHovered() && gui::IsMouseHoveringRect(lo, hi)) {
      if (i == 1) gui::SetTooltip("Ray Reconstruction denoises and upscales together when used in place of DLSS Super Resolution.");
      else gui::SetTooltip("%s", nodes[i]);
    }
  }

  const auto preferred = SourcePickOf(*settings, view.dlss_explicit);
  const size_t selected = preferred == SourcePick::PRESENT ? (settings->fg_present == Settings::FgPresent::BEFORE ? 2u : 3u) : static_cast<size_t>(preferred);
  constexpr std::array<int, 4> preceding{0, 2, 3, 4};
  for (size_t i = 0; i < NR_STAGE_NAMES.size(); ++i) {
    const int prior = preceding[i];
    const ImVec2 c = centers[prior];
    const bool at_fold = folded && prior == 2;
    const float tap_x = at_fold ? origin.x + width - 1.f : c.x + pitch * 0.5f;
    const float x = std::clamp(tap_x, origin.x + margin + button_w * 0.5f, origin.x + width - margin - button_w * 0.5f);
    const float y = c.y + hook_y;
    const float bend_y = y - em * 0.65f;
    if (!at_fold) draw.AddLine({tap_x, c.y}, {tap_x, bend_y}, wire);
    draw.AddLine({tap_x, bend_y}, {x, bend_y}, wire);
    draw.AddCircleFilled({tap_x, at_fold ? bend_y : c.y}, head, wire);
    Arrow(draw, {x, bend_y}, {x, y - 1.f}, wire, head);

    const auto source = static_cast<SourcePick>(std::min(i, size_t{2}));
    std::string_view why = view.setup.stage_options[static_cast<size_t>(source)].why;
    if (source == SourcePick::PRESENT && why == FG_PRESENT_OFF_REASON) why = {};
    if (i == 2 && !view.before_fg_unavailable.empty()) why = view.before_fg_unavailable;
    const auto run = view.stage_runs[i];
    const ImVec4 color = RunColor(run.first, i);
    gui::PushID(static_cast<int>(i));
    gui::SetCursorScreenPos({x - button_w * 0.5f, y});
    gui::PushStyleVar(ImGuiStyleVar_FrameRounding, button_h * 0.5f);
    gui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, selected == i ? 2.f : 1.f);
    gui::PushStyleColor(ImGuiCol_Border, run.count ? color : gui::GetStyleColorVec4(selected == i ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    if (run.count) {
      ImVec4 fill = gui::GetStyleColorVec4(ImGuiCol_WindowBg);
      fill.x = fill.x * 0.78f + color.x * 0.22f; fill.y = fill.y * 0.78f + color.y * 0.22f; fill.z = fill.z * 0.78f + color.z * 0.22f;
      gui::PushStyleColor(ImGuiCol_Button, fill);
    }
    std::string label = gui::Display(NR_STAGE_NAMES[i]);
    if (i == 2 && ImGui::CalcTextSize(label.c_str()).x + 2.f * gui::GetStyle().FramePadding.x > button_w) label = gui::Display("Before FG");
    gui::BeginDisabled(view.setup.stopped || !why.empty());
    const bool clicked = ImGui::Button(label.c_str(), {button_w, button_h});
    gui::EndDisabled();
    if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || (gui::GetIO().NavVisible && gui::IsItemFocused())) {
      gui::SetTooltip("%s", why.empty() ? (i == 0 && view.setup.before_upscaling_on_rr ? "NR works on Ray Reconstruction's input." : TIPS[i]) : why.data());
    }
    gui::PopStyleColor(run.count ? 2 : 1);
    gui::PopStyleVar(2);
    gui::PopID();

    if (run.count) {
      const std::string passes = run.count == 1 ? std::format("P{}  x1", run.first + 1) : std::format("P{}-{}  x{}", run.first + 1, run.first + run.count, run.count);
      CenterText(draw, {x, y + button_h + em * 0.7f}, gui::GetColorU32(color), passes);
    }
    if (!clicked) continue;
    if (i == 3 && settings->fg_present != Settings::FgPresent::AFTER) {
      state->confirm_present_stage = true;
      state->enable_warning_key = "PresentWithFrameGen";
    } else {
      SetSourcePick(settings, source, view.dlss_explicit);
      if (i == 2) settings->fg_present = Settings::FgPresent::BEFORE;
      state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(source));
      changed = true;
    }
  }
  gui::SetCursorScreenPos(end);
  gui::PopID();
  return changed;
}

}  // namespace uplift::ui
