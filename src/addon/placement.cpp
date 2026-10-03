#include "addon/placement.hpp"

#include <format>

namespace uplift::addon {

PlacementChoice ChoosePlacement(ui::PlacementSource source, std::string_view dlss_unavailable_reason, bool dlss_seen, bool before_upscaling,
                                const StageProblems& problems, bool dlss_off) {
  const bool dlss_available = dlss_unavailable_reason.empty();
  // Plan 18: the DLSS stage that can run: Before upscaling when asked and it can, else After DLSS when it can (Ray Reconstruction's own fallback), else none.
  std::optional<Placement> stage;
  if (before_upscaling && problems.before_upscaling.empty()) {
    stage = Placement::BEFORE_UPSCALING;
  } else if (problems.after_dlss.empty()) {
    stage = Placement::AFTER_DLSS;
  }
  switch (source) {
    case ui::PlacementSource::PRESENT:
      return {.placement = Placement::PRESENT};
    case ui::PlacementSource::DLSS:
      if (!dlss_available) return {.placement = Placement::NONE, .reason = std::string(dlss_unavailable_reason)};
      if (dlss_off) return {.placement = Placement::PRESENT};  // Plan 18 Task 12: never a wait for a DLSS the game switched off
      if (!dlss_seen) return {.placement = Placement::NONE, .reason = "Waiting for DLSS: the game has not run DLSS yet"};
      if (!stage) return {.placement = Placement::NONE, .reason = std::string(problems.after_dlss)};
      return {.placement = *stage};
    case ui::PlacementSource::AUTO:
      break;
  }
  return {.placement = ((dlss_available && dlss_seen && !dlss_off && stage) ? *stage : Placement::PRESENT)};
}

std::string DlssOnAgainLine(const PlacementChoice& resumed) {
  constexpr std::string_view LEAD = "DLSS is on again on this device: ";
  switch (resumed.placement) {
    case Placement::AFTER_DLSS:       return std::format("{}After DLSS resumes", LEAD);
    case Placement::BEFORE_UPSCALING: return std::format("{}Before upscaling resumes", LEAD);
    case Placement::PRESENT:          return std::format("{}NR stays at Present", LEAD);
    case Placement::NONE:             break;
  }
  return std::format("{}NR stays off ({})", LEAD, resumed.reason);
}

bool DlssReleased(const ngx_hooks::FeatureRegistry& registry, const void* device, ngx_hooks::NgxApi api, bool dlss_seen) {
  return dlss_seen && !registry.UpscalerLive(device, api);
}

bool SetupDlssSeen(bool vulkan, bool upscaler_created, bool context_evaluated) {
  return context_evaluated || (vulkan && upscaler_created);
}

bool PresentMotionWanted(const PresentMotionGates& gates) {
  const bool nr_on = (gates.nr_state == nr::SessionState::LOADING || gates.nr_state == nr::SessionState::ACTIVE || gates.nr_state == nr::SessionState::GRACE);
  return gates.enabled && nr_on && gates.bridge_gpu_ordered && gates.same_queue && !gates.native_wants_nr && gates.native_can_copy && !gates.problem
         && gates.dlss_motion && gates.upscaler_created;
}

std::string_view QualityName(int perf_quality) {
  switch (perf_quality) {
    case 0:  return "Performance";
    case 1:  return "Balanced";
    case 2:  return "Quality";
    case 3:  return "Ultra Performance";
    case 4:  return "Ultra Quality";
    case 5:  return "DLAA";
    default: return "custom";
  }
}

std::string FormatPlacementLine(const PlacementChoice& choice, const std::optional<ngx_hooks::CreateSnapshot>& main, bool vulkan) {
  switch (choice.placement) {
    case Placement::PRESENT:          return "Placement: Present";
    case Placement::NONE:             return std::format("Placement: none ({})", choice.reason);
    case Placement::AFTER_DLSS:
    case Placement::BEFORE_UPSCALING: break;
  }
  const std::string_view name = (choice.placement == Placement::AFTER_DLSS ? "after DLSS" : "before upscaling");
  const std::string_view api = (vulkan ? "Vulkan, " : "");
  if (!main) return (vulkan ? std::format("Placement: {} (Vulkan)", name) : std::format("Placement: {}", name));
  return std::format("Placement: {} ({}{}, {}x{} -> {}x{})", name, api, QualityName(main->perf_quality), main->render.width,
                     main->render.height, main->output.width, main->output.height);
}

std::string ContextMessage(const MessageInputs& inputs) {
  for (const std::string_view first : {inputs.blocked, inputs.output, inputs.placement}) {
    if (!first.empty()) return std::string(first);
  }
  if (inputs.skip_reason.empty()) return {};
  if (inputs.skip_from_session && !inputs.session_message.empty()) return std::string(inputs.session_message);
  return std::format("NR skipped: {}", inputs.skip_reason);
}

}  // namespace uplift::addon
