#include "addon/vk_route.hpp"

#include <format>

namespace uplift::addon {
namespace {

// DecideVkStages: why the DLSS stages are greyed while VulkanNr is not Native, with the two settings' names it gives.
constexpr char STAGES_NOT_NATIVE_LINE[] = "Only Vulkan NR: Native runs NR inside the game's DLSS (Vulkan NR is {})";
constexpr std::string_view DIRECT3D_12_MODE_NAME = "Direct3D 12";
constexpr std::string_view HELPER_MODE_NAME = "Helper";
// ChooseVkNrRoute: why each route cannot run (the panel greys it with this), the join of two reasons, and the reasons a pick gives.
constexpr char NATIVE_CREATE_FAILED_LINE[] = "NGX could not create NR's feature on the game's Vulkan device ({})";
constexpr char NATIVE_START_FAILED_LINE[] = "native Vulkan NR could not start ({})";
constexpr char NATIVE_NEEDS_LINE[] = "native Vulkan NR needs {}";
// VkNativeDlssProblem: why native NR cannot run inside the game's DLSS either.
constexpr char DLSS_NEEDS_LINE[] = "NR after DLSS on Vulkan needs {}";
constexpr char DLSS_START_FAILED_LINE[] = "NR after DLSS on Vulkan could not start: {}";
constexpr char D3D12_CREATE_FAILED_LINE[] = "NGX could not create NR's feature on the private Direct3D 12 device ({})";
constexpr char D3D12_START_FAILED_LINE[] = "the private Direct3D 12 device could not start ({})";
constexpr char ROUTE_REASONS_LINE[] = "{}; {}";
constexpr std::string_view D3D12_PICKED_REASON = "Vulkan NR at Present is set to Direct3D 12 in Advanced";
constexpr std::string_view HELPER_PICKED_REASON = "Vulkan NR at Present is set to Helper in Advanced";
// VkOwnerName: each owner as the owner-change line names it.
constexpr std::string_view OWNER_NATIVE_PRESENT_NAME = "native NR at Present";
constexpr std::string_view OWNER_NATIVE_DLSS_STAGE_NAME = "native NR at a DLSS stage";
constexpr std::string_view OWNER_BRIDGE_NAME = "the private Direct3D 12 device";
constexpr std::string_view OWNER_HELPER_NAME = "Uplift's helper process";
constexpr std::string_view OWNER_NONE_NAME = "nobody";
// VkOwnerChangeLine: the change's cause, and the line.
constexpr std::string_view STAGE_CHANGED_CAUSE = "the NR stage changed";
constexpr std::string_view SETTING_CHANGED_CAUSE = "Vulkan NR changed";
constexpr std::string_view FALLBACK_CAUSE = "an automatic fallback";
constexpr char FALLBACK_DETAIL_CAUSE_LINE[] = "an automatic fallback: {}";
constexpr char OWNER_CHANGE_LINE[] = "NR moves from {} to {} ({}): the old one drains now, the new one starts from off";
// VkRouteLine: the log's line for each route.
constexpr std::string_view NATIVE_ROUTE_LINE = "Vulkan: NR at Present runs natively on the game's Vulkan device";
constexpr char D3D12_ROUTE_LINE[] = "Vulkan: NR at Present runs on a private Direct3D 12 device: {}";
constexpr char HELPER_ROUTE_LINE[] = "Vulkan: NR at Present runs in Uplift's helper process: {}";

}  // namespace

bool PresentMotionWanted(const PresentMotionGates& gates) {
  bool nr_on = (gates.nr_state == nr::SessionState::LOADING || gates.nr_state == nr::SessionState::ACTIVE || gates.nr_state == nr::SessionState::GRACE);
  if (gates.native_present && gates.nr_state == nr::SessionState::OFF) {
    // Natively the copies are this NR load's motion input from its very first recording, so they are made while NR is about to load too (the
    // gates are read before the present's BeginFrame loads it); otherwise that load would start without them and reload for them.
    nr_on = true;
  }
  // Split chain: a native DLSS stage binds DLSS's vectors directly, unless its chain also has a later group at Present, which takes these copies.
  return gates.enabled && nr_on && (gates.bridge_gpu_ordered || gates.native_present) && gates.same_queue && (!gates.native_wants_nr || gates.native_present_group)
         && gates.native_can_copy && !gates.problem && gates.dlss_motion && gates.upscaler_created;
}

VkStageDecision DecideVkStages(const VkStageFacts& facts) {
  VkStageDecision decision;
  if (facts.setting == ui::VulkanNrMode::DIRECT3D_12 || facts.setting == ui::VulkanNrMode::HELPER) {
    decision.stages_off = std::format(STAGES_NOT_NATIVE_LINE,
                                      (facts.setting == ui::VulkanNrMode::DIRECT3D_12 ? DIRECT3D_12_MODE_NAME : HELPER_MODE_NAME));
  } else if (!facts.native_dlss_off.empty()) {
    decision.stages_off = std::string(facts.native_dlss_off);  // the failure's own words: native cannot run inside the game's DLSS either
  }
  decision.native_present_only = !decision.stages_off.empty();
  // A failure of native Present alone (the route fell back) leaves the DLSS stages native. The decision is this present's, so the owner
  // moves in the frame the stage does; a context stopped at a DLSS stage (the game's NGX shutdown) keeps it, and with it NR's claim.
  decision.native_at_dlss_stage = facts.stopped_at_dlss_stage || (!decision.native_present_only && facts.dlss_stage_wanted);
  return decision;
}

bool VkRoutePickMovesStage(ui::VulkanNrMode before, ui::VulkanNrMode after, ui::SourcePick stage) {
  return after != before && after != ui::VulkanNrMode::NATIVE && stage != ui::SourcePick::PRESENT;
}

std::string VkNativeUnavailable(const VkNativeBlock& block) {
  switch (block.cause) {
    case VkNativeBlock::Cause::CREATE_FAILURE:  return std::format(NATIVE_CREATE_FAILED_LINE, block.detail);
    case VkNativeBlock::Cause::START_FAILED:
    case VkNativeBlock::Cause::PRESENT_BUFFERS: return std::format(NATIVE_START_FAILED_LINE, block.detail);
    case VkNativeBlock::Cause::NEEDS:           break;
  }
  return std::format(NATIVE_NEEDS_LINE, block.detail);
}

std::string VkD3d12Unavailable(const VkD3d12Block& block) {
  return (block.cause == VkD3d12Block::Cause::CREATE_FAILURE ? std::format(D3D12_CREATE_FAILED_LINE, block.detail)
                                                             : std::format(D3D12_START_FAILED_LINE, block.detail));
}

bool VkNativeBlocksDlss(const std::optional<VkNativeBlock>& block) {
  return block && (block->cause == VkNativeBlock::Cause::NEEDS || block->cause == VkNativeBlock::Cause::START_FAILED);
}

std::string VkNativeDlssProblem(const std::optional<VkNativeBlock>& block) {
  if (!VkNativeBlocksDlss(block)) return {};
  return (block->cause == VkNativeBlock::Cause::NEEDS ? std::format(DLSS_NEEDS_LINE, block->detail) : std::format(DLSS_START_FAILED_LINE, block->detail));
}

std::string VkNativeLockReason(const std::optional<std::string>& stopped, const std::optional<VkNativeBlock>& block) {
  if (stopped) return *stopped;
  return ((block && block->cause != VkNativeBlock::Cause::NEEDS) ? block->detail : std::string());
}

VkRouteChoice ChooseVkNrRoute(const VkRouteFacts& facts) {
  VkRouteChoice choice;
  if (facts.native && facts.native->cause == VkNativeBlock::Cause::CREATE_FAILURE) {
    choice.native_unavailable = VkNativeUnavailable(*facts.native);
  } else if (!facts.native_off.empty()) {
    choice.native_unavailable = std::string(facts.native_off);
  } else if (facts.native) {
    choice.native_unavailable = VkNativeUnavailable(*facts.native);
  }
  if (facts.d3d12) {
    choice.d3d12_unavailable = VkD3d12Unavailable(*facts.d3d12);
  }
  switch (facts.setting) {
    case ui::VulkanNrMode::NATIVE:
      if (choice.native_unavailable.empty()) {
        choice.route = VkNrRoute::NATIVE;
      } else if (choice.d3d12_unavailable.empty()) {
        choice.route = VkNrRoute::DIRECT3D_12;
        choice.reason = choice.native_unavailable;
      } else {
        choice.route = VkNrRoute::HELPER;
        choice.reason = std::format(ROUTE_REASONS_LINE, choice.native_unavailable, choice.d3d12_unavailable);
      }
      break;
    case ui::VulkanNrMode::DIRECT3D_12:
      if (choice.d3d12_unavailable.empty()) {
        choice.route = VkNrRoute::DIRECT3D_12;
        choice.reason = (choice.native_unavailable.empty() ? std::string(D3D12_PICKED_REASON) : choice.native_unavailable);
      } else {
        choice.route = VkNrRoute::HELPER;
        choice.reason = choice.d3d12_unavailable;
      }
      break;
    case ui::VulkanNrMode::HELPER:
      choice.route = VkNrRoute::HELPER;
      choice.reason = HELPER_PICKED_REASON;
      break;
  }
  return choice;
}

VkNrOwner ChooseVkNrOwner(const VkOwnerFacts& facts) {
  if (facts.native_at_dlss_stage) return VkNrOwner::NATIVE_DLSS_STAGE;
  switch (facts.route) {
    case VkNrRoute::NATIVE:      return VkNrOwner::NATIVE_PRESENT;
    case VkNrRoute::DIRECT3D_12: return VkNrOwner::BRIDGE;
    case VkNrRoute::HELPER:      return VkNrOwner::HELPER;
  }
  return VkNrOwner::NONE;
}

bool VkHolderDrainsNow(VkNrOwner owner, VkNrHolder holder) {
  switch (holder) {
    case VkNrHolder::NATIVE: return owner != VkNrOwner::NATIVE_PRESENT && owner != VkNrOwner::NATIVE_DLSS_STAGE;
    case VkNrHolder::BRIDGE: return owner != VkNrOwner::BRIDGE;
    case VkNrHolder::HELPER: return owner != VkNrOwner::HELPER;
  }
  return true;
}

VkOwnerChange VkOwnerChangeOf(VkNrOwner previous, VkNrOwner now, bool stage_changed, bool setting_changed) {
  if (previous == VkNrOwner::NONE || now == VkNrOwner::NONE || previous == now) return {};
  // A route pick that also moves the stage names the route change.
  const VkOwnerCause cause = (setting_changed ? VkOwnerCause::SETTING : stage_changed ? VkOwnerCause::STAGE : VkOwnerCause::FALLBACK);
  return {.changed = true, .cause = cause};
}

std::string_view VkOwnerName(VkNrOwner owner) {
  switch (owner) {
    case VkNrOwner::NATIVE_PRESENT:    return OWNER_NATIVE_PRESENT_NAME;
    case VkNrOwner::NATIVE_DLSS_STAGE: return OWNER_NATIVE_DLSS_STAGE_NAME;
    case VkNrOwner::BRIDGE:            return OWNER_BRIDGE_NAME;
    case VkNrOwner::HELPER:            return OWNER_HELPER_NAME;
    case VkNrOwner::NONE:              break;
  }
  return OWNER_NONE_NAME;
}

std::string VkOwnerChangeLine(VkNrOwner previous, VkNrOwner now, VkOwnerCause cause, std::string_view detail) {
  std::string why;
  switch (cause) {
    case VkOwnerCause::STAGE:    why = STAGE_CHANGED_CAUSE; break;
    case VkOwnerCause::SETTING:  why = SETTING_CHANGED_CAUSE; break;
    case VkOwnerCause::FALLBACK: why = (detail.empty() ? std::string(FALLBACK_CAUSE) : std::format(FALLBACK_DETAIL_CAUSE_LINE, detail)); break;
  }
  return std::format(OWNER_CHANGE_LINE, VkOwnerName(previous), VkOwnerName(now), why);
}

std::string VkRouteLine(const VkRouteChoice& choice) {
  switch (choice.route) {
    case VkNrRoute::NATIVE:      return std::string(NATIVE_ROUTE_LINE);
    case VkNrRoute::DIRECT3D_12: return std::format(D3D12_ROUTE_LINE, choice.reason);
    case VkNrRoute::HELPER:      break;
  }
  return std::format(HELPER_ROUTE_LINE, choice.reason);
}

}  // namespace uplift::addon
