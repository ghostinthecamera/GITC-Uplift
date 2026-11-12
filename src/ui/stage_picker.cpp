#include "ui/overlay.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string>

#include "ui/overlay_style.hpp"
#include "ui/settings_schema.hpp"

namespace uplift::ui {
namespace {

// The pipeline diagram's measures, in font heights unless named otherwise. The six stages sit on one row while their names and the four
// stage buttons fit it; otherwise they fold into two rows of three.
constexpr float MARGIN_EM = 0.55f, MARGIN_SHARE = 0.025f;  // left and right of the diagram: the smaller of the two
constexpr float GAP_EM = 1.05f, GAP_SHARE = 0.04f;         // between stages: the smaller of the two
constexpr float NODE_HEIGHT_EM = 1.6f;
constexpr float NODE_TOP_EM = 0.2f;                // above the first row of stages
constexpr float NODE_ROUNDING_EM = 0.35f;
constexpr float NODE_LABEL_INSET_EM = 0.35f;       // the least space beside a stage's name inside its box
constexpr float HEX_LABEL_INSET_EM = 1.5f;         // the same inside the frame-generation hexagon
constexpr float HEX_INSET_EM = 0.75f, HEX_INSET_SHARE = 0.12f;  // the frame-generation hexagon's point: the smaller of the two
constexpr float FOLDED_ROW_PITCH_EM = 6.8f;        // from one row of stages to the next when folded
constexpr float BUTTON_TEXT_LINES = 1.f;           // a stage button's height, in text lines plus its padding
constexpr float WRAPPED_BUTTON_TEXT_LINES = 2.f;   // the same while Before Framegen's label wraps
constexpr float BUTTON_ROUNDING_SHARE = 0.2f;      // of the button's height: 0.5 draws a pill
constexpr float BUTTON_WIDTH_EM = 9.2f;
constexpr float BUTTON_SPACING_EM = 0.28f;         // the least space between two stage buttons
constexpr float HOOK_EM = 1.7f;                    // from a stage's centre down to its button
constexpr float BEND_EM = 0.5f;                    // the hook's elbow above the button
constexpr float ARROW_HEAD_EM = 0.17f;

// The six stages: each one's name in its box and full name in its tooltip, frame generation's name where its hexagon is too narrow for
// Frame Gen, the boxes the diagram treats apart, the boxes per row when folded, and the box each stage button hangs after.
constexpr std::array<const char*, 6> NODE_LABELS{"Scene", "Ray Recon", "DLSS", "PP/HUD", "Frame Gen", "Display"};
constexpr std::array<const char*, 6> NODE_TOOLTIPS{"Game\nscene", "Ray\nReconstruction", "DLSS\nupscaling", "Game PP\nand HUD", "Frame\ngeneration", "Present\nfor display"};
constexpr const char* NARROW_FRAME_GENERATION_LABEL = "FG";
constexpr size_t RAY_RECONSTRUCTION_NODE = 1u;
constexpr size_t FRAME_GENERATION_NODE = 4u;
constexpr size_t FOLDED_COLUMNS = 3u;
constexpr std::array<size_t, 4> STAGE_TAP_NODES{0u, 2u, 3u, 4u};

// The stage buttons, in NR_STAGE_NAMES's order, and Before Framegen's label where its button is too narrow for one line.
constexpr size_t PRE_DLSS_STAGE = 0u;
constexpr size_t BEFORE_FG_STAGE = 2u;
constexpr size_t ON_PRESENT_STAGE = 3u;
constexpr const char* WRAPPED_BEFORE_FG_LABEL = "Before\nFramegen";

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

}  // namespace

bool DrawStagePicker(const OverlayView& view, Settings* settings, OverlayState* state) {
  gui::PushID("Source");
  const bool can_reset = !IsDefault(*settings, *FindSetting("Source")) || !IsDefault(*settings, *FindSetting("PreUpscale"));
  const float width_needed = gui::CalcTextSize("Choose a stage for pass 1").x + (can_reset ? gui::ResetButtonWidth() + style::RowGap() : 0.f);
  gui::AlignTextToFramePadding();
  gui::TextUnformatted("NR stage");
  gui::SameLine();
  if (gui::GetContentRegionAvail().x < width_needed) gui::NewLine();
  gui::TextDisabled("Choose a stage for pass 1");
  bool changed = false;
  if (can_reset) {
    gui::SameLine();
    if (gui::GetContentRegionAvail().x < gui::ResetButtonWidth()) gui::NewLine();
    if (gui::ResetButton()) {
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
  const float margin = std::min(MARGIN_EM * em, width * MARGIN_SHARE);
  const float gap = std::min(GAP_EM * em, width * GAP_SHARE);
  const auto cell_for = [&](size_t count) { return std::max(1.f, (width - 2.f * margin - gap * static_cast<float>(count - 1u)) / static_cast<float>(count)); };
  const auto label_inset = [em](size_t node) { return em * (node == FRAME_GENERATION_NODE ? HEX_LABEL_INSET_EM : NODE_LABEL_INSET_EM); };
  // A stage's name in a box `cell_width` wide: frame generation's falls back to its narrow name.
  const auto node_label = [&](size_t node, float cell_width) {
    std::string label = gui::Display(NODE_LABELS[node]);
    if (node == FRAME_GENERATION_NODE && ImGui::CalcTextSize(label.c_str()).x > cell_width - label_inset(node)) label = gui::Display(NARROW_FRAME_GENERATION_LABEL);
    return label;
  };
  // One row while every name fits its box and every stage button its label (Before Framegen's on two lines).
  const auto fits_one_row = [&] {
    const float one_row_cell = cell_for(NODE_LABELS.size());
    for (size_t node = 0u; node < NODE_LABELS.size(); ++node) {
      if (ImGui::CalcTextSize(node_label(node, one_row_cell).c_str()).x > one_row_cell - label_inset(node)) return false;
    }
    const float one_row_button = std::min(BUTTON_WIDTH_EM * em, one_row_cell + gap - em * BUTTON_SPACING_EM);
    for (size_t stage = 0u; stage < NR_STAGE_NAMES.size(); ++stage) {
      const std::string shown = gui::Display(stage == BEFORE_FG_STAGE ? WRAPPED_BEFORE_FG_LABEL : NR_STAGE_NAMES[stage]);
      if (ImGui::CalcTextSize(shown.c_str()).x + 2.f * style::ControlPadding() > one_row_button) return false;
    }
    return true;
  };
  const bool folded = !fits_one_row();
  const size_t columns = folded ? FOLDED_COLUMNS : NODE_LABELS.size();
  const float cell = cell_for(columns);
  const float height = em * NODE_HEIGHT_EM, pitch = cell + gap, row = em * FOLDED_ROW_PITCH_EM;
  const float button_w = std::max(1.f, std::min(BUTTON_WIDTH_EM * em, pitch - em * BUTTON_SPACING_EM));
  const bool label_wraps = ImGui::CalcTextSize(gui::Display(NR_STAGE_NAMES[BEFORE_FG_STAGE]).c_str()).x + 2.f * style::ControlPadding() > button_w;
  const float button_h = (label_wraps ? WRAPPED_BUTTON_TEXT_LINES : BUTTON_TEXT_LINES) * em + 2.f * style::ControlPadding(), hook_y = em * HOOK_EM;
  float bottom = origin.y;
  ImDrawList& draw = *gui::GetWindowDrawList();
  const ImU32 wire = gui::GetColorU32(ImGuiCol_TextDisabled);
  const ImU32 ink = gui::GetColorU32(ImGuiCol_Text);
  const ImU32 surface = gui::GetColorU32(ImGuiCol_FrameBg);
  const float head = em * ARROW_HEAD_EM;
  std::array<ImVec2, NODE_LABELS.size()> centers{};
  for (size_t i = 0u; i < centers.size(); ++i) centers[i] = {origin.x + margin + cell * 0.5f + static_cast<float>(i % columns) * pitch,
                                                               origin.y + height * 0.5f + em * NODE_TOP_EM + static_cast<float>(i / columns) * row};

  for (size_t i = 0u; i + 1u < centers.size(); ++i) {
    const ImVec2 from{centers[i].x + cell * 0.5f, centers[i].y};
    const ImVec2 to{centers[i + 1].x - cell * 0.5f - 1.f, centers[i + 1].y};
    if (folded && i + 1u == FOLDED_COLUMNS) {
      const float right = origin.x + width - 1.f, left = origin.x + 1.f, mid = to.y - height * 0.59f;
      const ImVec2 turn[]{from, {right, from.y}, {right, mid}, {left, mid}, {left, to.y}};
      draw.AddPolyline(turn, 5, wire, ImDrawFlags_None, 1.f);
      Arrow(draw, turn[4], to, wire, head);
    } else {
      Arrow(draw, from, to, wire, head);
    }
  }

  for (size_t i = 0u; i < centers.size(); ++i) {
    const ImVec2 c = centers[i], lo{c.x - cell * 0.5f, c.y - height * 0.5f}, hi{c.x + cell * 0.5f, c.y + height * 0.5f};
    if (i == FRAME_GENERATION_NODE) {
      const float inset = std::min(cell * HEX_INSET_SHARE, em * HEX_INSET_EM);
      const ImVec2 hex[]{{lo.x + inset, lo.y}, {hi.x - inset, lo.y}, {hi.x, c.y}, {hi.x - inset, hi.y}, {lo.x + inset, hi.y}, {lo.x, c.y}};
      draw.AddConvexPolyFilled(hex, 6, surface);
      draw.AddPolyline(hex, 6, wire, ImDrawFlags_Closed, 1.f);
    } else {
      draw.AddRectFilled(lo, hi, surface, em * NODE_ROUNDING_EM);
      draw.AddRect(lo, hi, wire, em * NODE_ROUNDING_EM);
    }
    const bool greyed = (i == RAY_RECONSTRUCTION_NODE && !view.setup.before_upscaling_on_rr) || (i == FRAME_GENERATION_NODE && !view.frame_generation_active);
    CenterText(draw, c, greyed ? wire : ink, node_label(i, cell));
    if (gui::IsWindowHovered() && gui::IsMouseHoveringRect(lo, hi)) {
      if (i == RAY_RECONSTRUCTION_NODE) gui::SetTooltip("Ray Reconstruction denoises and upscales together when used in place of DLSS Super Resolution.");
      else gui::SetTooltip("%s", NODE_TOOLTIPS[i]);
    }
  }

  const auto preferred = SourcePickOf(*settings, view.dlss_explicit);
  const bool before_framegen = view.frame_generation_active && settings->fg_present == Settings::FgPresent::BEFORE;
  const size_t selected = preferred == SourcePick::PRESENT ? (before_framegen ? BEFORE_FG_STAGE : ON_PRESENT_STAGE) : static_cast<size_t>(preferred);
  for (size_t i = 0; i < NR_STAGE_NAMES.size(); ++i) {
    const size_t prior = STAGE_TAP_NODES[i];
    const ImVec2 c = centers[prior];
    const bool at_fold = folded && prior + 1u == FOLDED_COLUMNS;
    const float tap_x = at_fold ? origin.x + width - 1.f : c.x + pitch * 0.5f;
    const float x = std::clamp(tap_x, origin.x + margin + button_w * 0.5f, origin.x + width - margin - button_w * 0.5f);
    const float y = c.y + hook_y;
    const float bend_y = y - em * BEND_EM;
    if (!at_fold) draw.AddLine({tap_x, c.y}, {tap_x, bend_y}, wire);
    draw.AddLine({tap_x, bend_y}, {x, bend_y}, wire);
    draw.AddCircleFilled({tap_x, at_fold ? bend_y : c.y}, head, wire);
    Arrow(draw, {x, bend_y}, {x, y - 1.f}, wire, head);

    const auto source = static_cast<SourcePick>(std::min(i, static_cast<size_t>(SourcePick::PRESENT)));
    std::string_view why = view.setup.stage_options[static_cast<size_t>(source)].why;
    if (source == SourcePick::PRESENT && why == FG_PRESENT_OFF_REASON) why = {};
    if (i == BEFORE_FG_STAGE && !view.before_fg_unavailable.empty()) why = view.before_fg_unavailable;
    const auto run = view.stage_runs[i];
    const ImVec4 color = style::PassAccent(run.first, i);
    gui::PushID(static_cast<int>(i));
    gui::SetCursorScreenPos({x - button_w * 0.5f, y});
    gui::PushStyleVar(ImGuiStyleVar_FramePadding, {style::ControlPadding(), style::ControlPadding() * 0.5f});
    gui::PushStyleVar(ImGuiStyleVar_FrameRounding, button_h * BUTTON_ROUNDING_SHARE);
    gui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, selected == i ? 2.f : 1.f);
    gui::PushStyleColor(ImGuiCol_Border, run.count ? color : gui::GetStyleColorVec4(selected == i ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    if (run.count) {
      gui::PushStyleColor(ImGuiCol_Button, style::SelectedFill(color));
    }
    std::string label = gui::Display(NR_STAGE_NAMES[i]);
    if (i == BEFORE_FG_STAGE && label_wraps) label = gui::Display(WRAPPED_BEFORE_FG_LABEL);
    gui::BeginDisabled(view.setup.stopped || !why.empty());
    const bool clicked = ImGui::Button(label.c_str(), {button_w, button_h});
    gui::EndDisabled();
    if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || (gui::GetIO().NavVisible && gui::IsItemFocused())) {
      gui::SetTooltip("%s", why.empty() ? (i == PRE_DLSS_STAGE && view.setup.before_upscaling_on_rr ? "NR works on Ray Reconstruction's input." : TIPS[i]) : why.data());
    }
    gui::PopStyleColor(run.count ? 2 : 1);
    gui::PopStyleVar(3);
    gui::PopID();

    float stage_bottom = y + button_h;
    if (run.count) {
      const std::string passes = run.count == 1 ? std::format("P{}  x1", run.first + 1) : std::format("P{}-{}  x{}", run.first + 1, run.first + run.count, run.count);
      const float text_top = stage_bottom + gui::GetStyle().ItemInnerSpacing.y;
      CenterText(draw, {x, text_top + gui::GetTextLineHeight() * 0.5f}, gui::GetColorU32(color), passes);
      stage_bottom = text_top + gui::GetTextLineHeight();
    }
    bottom = std::max(bottom, stage_bottom);
    if (!clicked) continue;
    if (i == ON_PRESENT_STAGE && view.frame_generation_active && settings->fg_present != Settings::FgPresent::AFTER) {
      state->confirm_present_stage = true;
      state->enable_warning_key = "PresentWithFrameGen";
    } else {
      SetSourcePick(settings, source, view.dlss_explicit);
      if (i == BEFORE_FG_STAGE) settings->fg_present = Settings::FgPresent::BEFORE;
      state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(source));
      changed = true;
    }
  }
  gui::SetCursorScreenPos(origin);
  gui::Dummy({width, bottom - origin.y});
  gui::PopID();
  return changed;
}

}  // namespace uplift::ui
