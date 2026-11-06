#include "addon/before_fg_decision.hpp"

namespace uplift::addon {
namespace {
using text::BeforeFgNote;

bool Early(const BeforeFgInputs& inputs) {
  return inputs.early_confirmed
      && (inputs.route != BeforeFgRoute::DX12 || (inputs.early_dxgi_matches && !inputs.inside_outer_present));
}
}  // namespace

uint32_t BeforeFgConfigCount(const BeforeFgInputs& inputs) {
  return inputs.observed_fg_off ? uint32_t{} : inputs.ngx_live;
}

bool BeforeFgActive(const BeforeFgInputs& inputs) {
  const bool detected = inputs.ngx_live || (inputs.route == BeforeFgRoute::DX12 && inputs.measured_cadence);
  return inputs.enabled && inputs.requested == ui::Settings::FgPresent::BEFORE
      && streamline::FrameGenerationActive(inputs.mode, detected);
}

bool BeforeFgOuterMotionCandidate(const BeforeFgInputs& inputs) {
  // Motion preparation reads the registry evidence only; measured cadence is DX12 route evidence.
  BeforeFgInputs registry = inputs;
  registry.measured_cadence = false;
  return BeforeFgActive(registry) && inputs.outer_matches_primary && !inputs.early_confirmed;
}

bool BeforeFgChecksOuterInputs(const BeforeFgInputs& inputs) {
  if (!BeforeFgActive(inputs) || Early(inputs)) return false;
  switch (inputs.route) {
    case BeforeFgRoute::DX12:
    case BeforeFgRoute::VK_NATIVE: return inputs.outer_matches_primary;
    case BeforeFgRoute::VK_BRIDGE:
    case BeforeFgRoute::VK_HELPER: return true;
    case BeforeFgRoute::UNSUPPORTED_BRIDGE: return false;
  }
  return false;
}

OuterInputAction OuterMotionAction(ui::MotionVectorSource source, ui::Settings::FgMotion choice) {
  if (source != ui::MotionVectorSource::LAUNCHPAD && source != ui::MotionVectorSource::LUMENITE)
    return OuterInputAction::DISABLE_EFFECT_MOTION;
  return choice == ui::Settings::FgMotion::DLSS ? OuterInputAction::USE_DLSS : OuterInputAction::USE_NONE;
}

void ApplyOuterInputs(FrameSettings& settings, FrameFacts& facts, OuterInputAction action) {
  if (action == OuterInputAction::UNCHANGED) return;
  settings.launchpad_motion = false;
  if (action == OuterInputAction::DISABLE_EFFECT_MOTION) return;
  settings.motion_vectors_want_launchpad = false;
  if (!facts.motion_at_dlss_stage.has_value()) facts.motion_at_dlss_stage = settings.motion_vectors;
  settings.motion_vectors = action == OuterInputAction::USE_DLSS;
}

BeforeFgDecision DecideBeforeFg(const BeforeFgInputs& inputs) {
  BeforeFgDecision decision;
  decision.active = BeforeFgActive(inputs);
  decision.outer_motion_candidate = BeforeFgOuterMotionCandidate(inputs);

  if (inputs.route == BeforeFgRoute::UNSUPPORTED_BRIDGE) {
    decision.state = decision.active ? BeforeFgState::UNAVAILABLE : BeforeFgState::ORDINARY;
    if (decision.active) decision.note = BeforeFgNote::BRIDGE_HANDOFF_UNAVAILABLE;
    return decision;
  }

  if (!decision.active) {
    decision.clear_pending = true;
    if (inputs.route != BeforeFgRoute::VK_HELPER) decision.note = BeforeFgNote::NONE;
    return decision;
  }

  if (Early(inputs)) {
    decision.state = BeforeFgState::EARLY;
    decision.clear_pending = inputs.route == BeforeFgRoute::VK_HELPER;
    decision.note = inputs.route == BeforeFgRoute::DX12 ? BeforeFgNote::DX12_RESHADE_PRESENT : BeforeFgNote::VK_RESHADE_PRESENT;
    return decision;
  }
  if (!BeforeFgChecksOuterInputs(inputs)) {
    decision.state = BeforeFgState::UNAVAILABLE;
    decision.clear_pending = inputs.route == BeforeFgRoute::VK_NATIVE;
    decision.note = !inputs.hooks_installed ? BeforeFgNote::HOOKS_UNAVAILABLE
        : inputs.route == BeforeFgRoute::DX12 ? BeforeFgNote::NO_STREAMLINE_PRESENT : BeforeFgNote::NO_VK_PRESENT;
    return decision;
  }

  BeforeFgNote issue = BeforeFgNote::NONE;
  if (inputs.mask_conflicts) {
    issue = BeforeFgNote::MASK_UNAVAILABLE;
  } else {
    decision.inputs = OuterInputAction::DISABLE_EFFECT_MOTION;
    if (inputs.motion_source == ui::MotionVectorSource::LAUNCHPAD || inputs.motion_source == ui::MotionVectorSource::LUMENITE) {
      if (inputs.before_fg_motion == ui::Settings::FgMotion::SELECT) {
        issue = BeforeFgNote::MOTION_SELECTION_REQUIRED;
      } else {
        decision.inputs = OuterMotionAction(inputs.motion_source, inputs.before_fg_motion);
        if (decision.inputs == OuterInputAction::USE_DLSS && (inputs.route == BeforeFgRoute::VK_HELPER || !inputs.upscaler_live))
          issue = BeforeFgNote::WAITING_DLSS_MOTION;
      }
    }
  }
  // The helper cannot use explicit DLSS motion, even when the mask check already refused the frame.
  if (inputs.route == BeforeFgRoute::VK_HELPER && inputs.motion_source == ui::MotionVectorSource::DLSS)
    issue = BeforeFgNote::HELPER_DLSS_MOTION;

  if (issue != BeforeFgNote::NONE || !inputs.outer_matches_primary) {
    decision.state = BeforeFgState::UNAVAILABLE;
    decision.clear_pending = true;
    decision.note = issue == BeforeFgNote::NONE ? BeforeFgNote::NO_VK_PRESENT : issue;
    return decision;
  }

  decision.state = BeforeFgState::ARM_OUTER;
  if (!inputs.handoff_pending && inputs.route != BeforeFgRoute::VK_BRIDGE) {
    decision.note = inputs.route == BeforeFgRoute::DX12 ? BeforeFgNote::WAITING_DX12_FRAME
        : inputs.route == BeforeFgRoute::VK_NATIVE ? BeforeFgNote::WAITING_VK_FRAME : BeforeFgNote::WAITING_HELPER_FRAME;
  }
  return decision;
}

}  // namespace uplift::addon
