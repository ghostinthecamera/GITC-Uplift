#include "addon/before_fg_device.hpp"

namespace uplift::addon {
namespace {
using text::BeforeFgNote;

void ApplyNote(BeforeFgDevice& device, const BeforeFgDecision& decision) {
  if (decision.note) device.note = {*decision.note};
}
}  // namespace

void ClearBeforeFgHandoff(BeforeFgDevice& device) {
  device.outer_owns_frame = false;
  device.outer_config.reset();
  device.helper_frame.reset();
  device.note = {};
}

void PrepareBeforeFgNative(BeforeFgDevice& device, const FrameSettings& settings, FrameFacts& facts, bool active, bool holds_nr) {
  if (active) {
    device.native_config = BeforeFgFrame{settings, facts};
    device.native_config->facts.game_facing_present = true;
    device.outer_holds_nr = holds_nr;
    facts.fg_present_path = WithBeforeFg(facts.fg_present_path);
  } else {
    device.native_config.reset();
  }
}

void ClearBeforeFgNativePrelude(BeforeFgDevice& device) {
  device.outer_owns_frame = false;
  device.outer_config.reset();
  device.note = {};
}

void PrepareBeforeFgBridge(BeforeFgDevice& device, bool active) {
  device.outer_owns_frame = false;
  device.outer_config.reset();
  if (!active) device.note = {};
}

void SelectBeforeFg(FrameFacts& facts, bool active) {
  if (active) facts.fg_present_path = WithBeforeFg(facts.fg_present_path);
}

void ApplyBeforeFgUnsupported(BeforeFgDevice& device, FrameFacts& facts, const BeforeFgDecision& decision) {
  SelectBeforeFg(facts, decision.active);
  ApplyNote(device, decision);
}

void ApplyBeforeFgNative(BeforeFgDevice& device, FrameSettings& settings, FrameFacts& facts, const BeforeFgDecision& decision,
                         bool outer_matches_primary, bool holds_nr, bool& vk_native_present) {
  if (decision.state == BeforeFgState::EARLY) {
    facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, true);
    facts.game_facing_present = true;
    device.outer_owns_frame = false;
    ApplyNote(device, decision);
    return;
  }
  if (!decision.active) {
    ClearBeforeFgNativePrelude(device);
    return;
  }

  facts.fg_present_path = WithBeforeFg(facts.fg_present_path);
  device.outer_owns_frame = false;
  if (outer_matches_primary) {
    facts.game_facing_present = true;
    facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, true);
    ApplyOuterInputs(settings, facts, decision.inputs);
    ApplyNote(device, decision);
    if (decision.state == BeforeFgState::ARM_OUTER) {
      facts.effects_on = false;
      device.outer_config = BeforeFgFrame{settings, facts};
      vk_native_present = true;
      device.outer_owns_frame = true;
      device.outer_holds_nr = holds_nr;
    } else {
      facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, false);
      facts.game_facing_present = false;
    }
  } else {
    ApplyNote(device, decision);
  }
  if (decision.clear_pending) device.outer_config.reset();
}

void ApplyBeforeFgBridge(BeforeFgDevice& device, FrameSettings& settings, FrameFacts& facts, const BeforeFgDecision& decision) {
  if (decision.state == BeforeFgState::ORDINARY) return;
  if (decision.state == BeforeFgState::EARLY) {
    facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, true);
    device.outer_owns_frame = false;
    ApplyNote(device, decision);
    return;
  }
  facts.game_facing_present = true;
  facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, true);
  ApplyOuterInputs(settings, facts, decision.inputs);
  if (decision.state == BeforeFgState::ARM_OUTER) {
    facts.effects_on = false;
    device.outer_config = BeforeFgFrame{settings, facts};
    device.helper_frame.reset();
    device.outer_owns_frame = true;
    return;
  }
  ApplyNote(device, decision);
  device.outer_owns_frame = false;
  if (decision.clear_pending) device.outer_config.reset();
  facts.game_facing_present = false;
  facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, false);
}

void ApplyBeforeFgDx12(BeforeFgDevice& device, FrameSettings& settings, FrameFacts& facts, const BeforeFgDecision& decision, bool outer_matches_primary) {
  if (decision.active) {
    facts.fg_present_path = WithBeforeFg(facts.fg_present_path);
    device.outer_owns_frame = false;
    if (decision.state == BeforeFgState::EARLY) {
      facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, true);
      ApplyNote(device, decision);
    } else if (outer_matches_primary) {
      facts.game_facing_present = true;
      facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, true);
      ApplyOuterInputs(settings, facts, decision.inputs);
      ApplyNote(device, decision);
      if (decision.state == BeforeFgState::ARM_OUTER) {
        facts.effects_on = false;
        device.outer_config = BeforeFgFrame{settings, facts};
        device.outer_owns_frame = true;
      } else {
        facts.game_facing_present = false;
        facts.fg_present_path = WithFrameGeneration(facts.fg_present_path, false);
        if (decision.clear_pending) device.outer_config.reset();
      }
    } else {
      ApplyNote(device, decision);
    }
  } else {
    device.outer_owns_frame = false;
    if (decision.clear_pending) device.outer_config.reset();
    ApplyNote(device, decision);
  }
}

void ApplyBeforeFgHelper(BeforeFgDevice& device, HelperFrame& frame, const BeforeFgDecision& decision) {
  if (decision.state == BeforeFgState::ARM_OUTER) {
    frame.marker = {};
    frame.early_marker = {};
    ApplyNote(device, decision);
    device.helper_frame = frame;
    device.outer_config.reset();
    device.outer_owns_frame = true;
    return;
  }
  ApplyNote(device, decision);
  if (decision.state == BeforeFgState::UNAVAILABLE) frame.nr_allowed = false;
  if (decision.clear_pending) device.helper_frame.reset();
  device.outer_owns_frame = false;
}

void DrainBeforeFgHelper(BeforeFgDevice& device, bool early_active) {
  if (early_active) device.note = {BeforeFgNote::VK_RESHADE_PRESENT};
  device.helper_frame.reset();
  device.outer_owns_frame = false;
}

}  // namespace uplift::addon
