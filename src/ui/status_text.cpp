#include "ui/status_text.hpp"

#include <cctype>
#include <format>
#include <utility>

namespace uplift::ui {

SetupView ResolveSetup(const SetupFacts& facts) {
  SetupView view;
  const auto at = [](auto& options, auto value) -> OptionState& { return options[static_cast<size_t>(value)]; };
  const auto selectable = [](const OptionState& option) { return option.why.empty(); };
  const auto grey_for_dlss = [&facts](OptionState* option) {
    if (!facts.dlss_unavailable.empty()) {
      *option = {.why = facts.dlss_unavailable};
    } else if (!facts.dlss_seen) {
      *option = {.why = DLSS_NOT_SEEN_REASON, .temporary = true};
    } else if (facts.dlss_off) {
      *option = {.why = DLSS_OFF_REASON, .temporary = true};  // Plan 18 Task 12
    }
  };

  // NR stage.
  grey_for_dlss(&at(view.stage_options, SourcePick::AFTER_DLSS));
  OptionState& before = at(view.stage_options, SourcePick::BEFORE_UPSCALING);
  grey_for_dlss(&before);
  if (selectable(before) && facts.ray_reconstruction) {
    before = {.why = "The game's DLSS is Ray Reconstruction, which keeps NR after DLSS", .temporary = true};
  }
  // Plan 18: a stage whose image the Direct3D 11 bridge cannot share is a fixed cause for that stage alone; it beats the temporary ones.
  const auto grey_fixed = [](OptionState* option, std::string_view fixed) {
    if (!fixed.empty() && (option->why.empty() || option->temporary)) {
      *option = {.why = fixed};
    }
  };
  grey_fixed(&at(view.stage_options, SourcePick::AFTER_DLSS), facts.after_dlss_fixed);
  grey_fixed(&before, facts.before_upscaling_fixed);
  if (!facts.present_fixed.empty()) {
    at(view.stage_options, SourcePick::PRESENT) = {.why = facts.present_fixed};  // a fixed cause beats the temporary one: it would not clear
  } else if (facts.frame_generation_blocks_present) {
    at(view.stage_options, SourcePick::PRESENT) = {.why = "Frame generation is on: turn on Present with frame generation (Advanced)", .temporary = true};
  }
  // Plan 15: held after the game's NGX shutdown on Direct3D 12, no stage can run until the game's DLSS starts again. That beats the other temporary causes
  // (it is why nothing runs now); a fixed cause keeps its own text, since it would not clear with the hold.
  const auto hold = [&facts](OptionState* option) {
    if (!facts.held.empty() && (option->why.empty() || option->temporary)) {
      *option = {.why = facts.held, .temporary = true};
    }
  };
  for (OptionState& option : view.stage_options) {
    hold(&option);
  }
  const OptionState stage_wish = at(view.stage_options, facts.preferred_stage);  // the preference's option, before the running one is un-greyed
  if (facts.stage) {
    view.stage = *facts.stage;
  } else if (selectable(stage_wish)) {
    view.stage = facts.preferred_stage;
  } else if (facts.preferred_stage == SourcePick::BEFORE_UPSCALING && selectable(at(view.stage_options, SourcePick::AFTER_DLSS))) {
    view.stage = SourcePick::AFTER_DLSS;  // Ray Reconstruction keeps NR after DLSS
  } else {
    view.stage = SourcePick::PRESENT;
  }

  // Motion vectors (Launchpad depends on the stage just decided).
  OptionState& dlss = at(view.motion_options, MotionPick::DLSS);
  grey_for_dlss(&dlss);
  if (dlss.why.empty() || dlss.temporary) {
    // A fixed reason beats "not seen yet": it would not clear when DLSS starts.
    if (!facts.dlss_motion_fixed.empty()) {
      dlss = {.why = facts.dlss_motion_fixed};
    } else if (selectable(dlss) && !facts.game_passes_motion) {
      dlss = {.why = "The game passes DLSS no motion vectors", .temporary = true};
    }
  }
  OptionState& launchpad = at(view.motion_options, MotionPick::LAUNCHPAD);
  if (!facts.launchpad_fixed.empty()) {
    launchpad = {.why = facts.launchpad_fixed};  // a fixed cause beats the temporary ones: it would not clear
  } else if (view.stage != SourcePick::PRESENT && !facts.present_fixed.empty()) {
    launchpad = {.why = facts.present_fixed};  // Launchpad runs at Present only, which cannot run again
  } else if (view.stage != SourcePick::PRESENT) {
    launchpad = {.why = "Launchpad feeds NR on the presented image only", .temporary = true};
  } else if (!facts.launchpad_ready) {
    launchpad = {.why = "Turn iMMERSE Launchpad on and enable Uplift.fx's Uplift technique below it", .temporary = true};
  }
  hold(&dlss);
  hold(&launchpad);
  const OptionState motion_wish = at(view.motion_options, facts.preferred_motion);
  MotionPick resolved_motion = MotionPick::OFF;
  if (selectable(motion_wish)) {
    resolved_motion = facts.preferred_motion;
  } else if (facts.motion_auto && selectable(launchpad)) {
    resolved_motion = MotionPick::LAUNCHPAD;
  } else if (facts.preferred_motion == MotionPick::LAUNCHPAD && view.stage != SourcePick::PRESENT && selectable(dlss)) {
    resolved_motion = MotionPick::DLSS;  // inside the game's frame a Launchpad preference runs on the game's own vectors (the engine's rule)
  }
  view.motion = facts.motion.value_or(resolved_motion);

  // Resolution.
  OptionState& match_game = at(view.resolution_options, ResolutionMode::MATCH_GAME);
  if (!facts.match_game_readable && facts.match_game_at_dlss_stages) {
    match_game = {.why = VULKAN_MATCH_GAME_REASON, .temporary = true};  // final review, minor 3: a DLSS stage here reads it
  } else if (!facts.match_game_readable) {
    match_game = {.why = "Match game needs the game's DLSS render size, which Uplift cannot read here"};
  } else if (!facts.dlss_seen) {
    match_game = {.why = DLSS_NOT_SEEN_REASON, .temporary = true};
  } else if (facts.dlss_off) {
    match_game = {.why = DLSS_OFF_REASON, .temporary = true};  // Plan 18 Task 12 (fix round 1, M-1): it reads DLSS's render size, which is off too
  }
  if (!facts.below_full_fixed.empty()) {
    for (size_t index = 1u; index < view.resolution_options.size(); ++index) {
      view.resolution_options[index] = {.why = facts.below_full_fixed};
    }
  }
  const OptionState resolution_wish = at(view.resolution_options, facts.preferred_resolution);
  view.resolution = facts.resolution.value_or(selectable(resolution_wish) ? facts.preferred_resolution : ResolutionMode::FULL);

  // What runs is possible by definition: its option is never greyed.
  at(view.stage_options, view.stage) = {};
  at(view.motion_options, view.motion) = {};
  at(view.resolution_options, view.resolution) = {};

  // Plan 15 fix round (minor 6): stopped for the session, nothing can run, the highlights included: every option of the three rows is greyed with the fixed
  // reason (the card says it too), and there is no Why line.
  if (!facts.stopped.empty()) {
    view.stopped = true;
    const auto stop = [&facts](auto& options) {
      for (OptionState& option : options) {
        option = {.why = facts.stopped};
      }
    };
    stop(view.stage_options);
    stop(view.motion_options);
    stop(view.resolution_options);
    return view;
  }
  // Plan 15: while held, the card says why NR waits (CardFacts::held), and a held context's card is never the working one, which alone draws the Why line.
  if (!facts.held.empty()) return view;

  // The Why line: temporary fallbacks from the preference only (a fixed cause has its tooltip). The first sentence as written (it starts with a name),
  // later ones with a capital.
  const auto add = [&view](std::string sentence) {
    if (!view.why.empty()) {
      sentence[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(sentence[0])));
      view.why += ' ';
    }
    view.why += sentence;
  };
  // A DLSS stage waits for the game's DLSS: not seen yet, or seen (a create) but not evaluated yet, or Vulkan's hand-off from Present.
  const bool stage_waits = (view.stage != facts.preferred_stage && facts.preferred_stage != SourcePick::PRESENT
                            && (selectable(stage_wish) || stage_wish.why == DLSS_NOT_SEEN_REASON));
  const bool motion_falls_back = (facts.preferred_motion == MotionPick::DLSS && view.motion != MotionPick::DLSS);
  const bool motion_waits = (motion_falls_back
                             && (motion_wish.why == DLSS_NOT_SEEN_REASON || (selectable(motion_wish) && facts.dlss_motion_gap == MotionGap::NONE)));
  const std::string_view stage_name = (facts.preferred_stage == SourcePick::BEFORE_UPSCALING ? "Before upscaling" : "After DLSS");
  // Plan 18 Task 12: the game switched its DLSS off: what fell back returns when it is on again.
  const bool stage_off = (view.stage != facts.preferred_stage && facts.preferred_stage != SourcePick::PRESENT && stage_wish.why == DLSS_OFF_REASON);
  const bool motion_off = (motion_falls_back && motion_wish.why == DLSS_OFF_REASON);
  if (stage_off) {
    add(motion_off ? std::format("{} and DLSS's motion vectors return when the game's DLSS is on again.", stage_name)
                   : std::format("{} returns when the game's DLSS is on again.", stage_name));
  } else if (stage_waits) {
    add(motion_waits ? std::format("{} and DLSS's motion vectors start when the game renders with DLSS.", stage_name)
                     : std::format("{} starts when the game renders with DLSS.", stage_name));
  } else if (view.stage != facts.preferred_stage && stage_wish.temporary && facts.preferred_stage == SourcePick::BEFORE_UPSCALING) {
    add("Before upscaling returns when the game's DLSS is not Ray Reconstruction.");
  }
  if (motion_off) {
    if (!stage_off) {
      add("DLSS's motion vectors return when the game's DLSS is on again.");  // else said with the stage above
    }
  } else if (motion_waits && !stage_waits) {
    add("DLSS's motion vectors start when the game renders with DLSS.");
  } else if (motion_falls_back && (facts.dlss_motion_gap == MotionGap::GAME_PASSED_NONE || (motion_wish.temporary && !motion_waits))) {
    add("the game passes DLSS no motion vectors.");
  } else if (motion_falls_back && selectable(motion_wish) && facts.dlss_motion_gap == MotionGap::NO_COPY) {
    add("no copy of DLSS's motion vectors reaches the Present path (the GPU runs too far behind).");
  } else if (motion_falls_back && selectable(motion_wish) && facts.dlss_motion_gap == MotionGap::DLSS_PAUSED) {
    add("DLSS's motion vectors return when the game's DLSS runs again (it is paused, as in a menu or a loading screen).");
  }
  if (facts.preferred_motion == MotionPick::LAUNCHPAD && view.motion != MotionPick::LAUNCHPAD) {
    if (motion_wish.temporary && view.stage != SourcePick::PRESENT) {
      add("Launchpad's vectors return when NR runs at Present; inside the game's frame NR uses the game's own.");
    } else if (motion_wish.temporary) {
      add("Launchpad's vectors return when Launchpad and Uplift.fx's Uplift technique are on.");
    } else if (selectable(motion_wish) && facts.launchpad_gap == MotionGap::NO_UPLIFT_MV) {
      add("no UPLIFT_MV arrives: put Uplift.fx's Uplift technique below Launchpad.");
    }
  }
  if (view.resolution != facts.preferred_resolution && facts.preferred_resolution == ResolutionMode::MATCH_GAME
      && (selectable(resolution_wish) || resolution_wish.why == DLSS_NOT_SEEN_REASON)) {
    add("Match game applies when the game renders with DLSS.");
  } else if (view.resolution != facts.preferred_resolution && facts.preferred_resolution == ResolutionMode::MATCH_GAME
             && resolution_wish.why == VULKAN_MATCH_GAME_REASON) {
    add("Match game applies when NR runs at After DLSS or Before upscaling.");
  } else if (view.resolution != facts.preferred_resolution && facts.preferred_resolution == ResolutionMode::MATCH_GAME
             && resolution_wish.why == DLSS_OFF_REASON) {
    add("Match game applies when the game's DLSS is on again.");  // Plan 18 Task 12 (fix round 1, M-1)
  }
  return view;
}

LiveCard BuildLiveCard(const SetupFacts& facts, const SetupView& shown) {
  LiveCard card;
  switch (shown.stage) {
    case SourcePick::BEFORE_UPSCALING: card.stage = "Before upscaling"; break;
    case SourcePick::AFTER_DLSS:       card.stage = "After DLSS"; break;
    case SourcePick::PRESENT:          card.stage = "Present"; break;
  }
  switch (shown.motion) {
    case MotionPick::OFF:       card.motion = "none"; break;
    case MotionPick::DLSS:      card.motion = (facts.motion_copied ? "DLSS (the game's, copied for Present)" : "DLSS (the game's)"); break;
    case MotionPick::LAUNCHPAD: card.motion = "Launchpad"; break;
  }
  std::string mode;
  switch (shown.resolution) {
    case ResolutionMode::FULL:        mode = "Full"; break;
    case ResolutionMode::QUALITY:     mode = "Quality"; break;
    case ResolutionMode::BALANCED:    mode = "Balanced"; break;
    case ResolutionMode::PERFORMANCE: mode = "Performance"; break;
    case ResolutionMode::MATCH_GAME:  mode = "Match game"; break;
    case ResolutionMode::CUSTOM:      mode = std::format("Custom {:.0f} %", facts.custom_scale); break;
  }
  const std::string_view upsampling = (facts.upsampling == color::Upsampling::CLASSIC ? "Classic" : "Edge-aware");
  const nr::Size work = facts.work;
  const bool full_size = (facts.frame.Empty() || work == facts.frame);
  if (work.Empty()) {
    card.resolution = mode;
  } else if (shown.stage == SourcePick::BEFORE_UPSCALING) {
    if (!facts.canvas.Empty() && facts.canvas != work) {
      card.resolution = std::format("{}x{} ({}, a {}x{} render padded)", facts.canvas.width, facts.canvas.height, mode, work.width, work.height);
    } else if (full_size) {
      card.resolution = std::format("{}x{} ({}, the render size)", work.width, work.height, mode);
    } else {
      card.resolution = std::format("{}x{} of the {}x{} render ({}, {})", work.width, work.height, facts.frame.width, facts.frame.height, mode, upsampling);
    }
  } else if (full_size) {
    card.resolution = std::format("{}x{} ({})", work.width, work.height, mode);
  } else {
    card.resolution = std::format("{}x{} of {}x{} ({}, {})", work.width, work.height, facts.frame.width, facts.frame.height, mode, upsampling);
  }
  for (const std::string_view note : {facts.latch_note, facts.passes_note, facts.frame_generation_line}) {
    if (!note.empty()) {
      card.note = std::string(note);
      break;
    }
  }
  card.footer = std::format("{} · {}/{} {}", facts.api, facts.passes_run, facts.passes_requested, (facts.passes_requested == 1u ? "pass" : "passes"));
  if (facts.vram_bytes > 0u) {
    card.footer += std::format(" · VRAM {:.0f} MiB", static_cast<double>(facts.vram_bytes) / (1024.0 * 1024.0));
  }
  return card;
}

SetupView SetupSettle::Update(SetupView resolved, std::chrono::steady_clock::time_point now, std::chrono::milliseconds settle) {
  if (!last_update_ || now - *last_update_ > 2 * settle) {
    rows_ = {};  // the first draw, or the overlay was closed for a while: what runs shows at once
  }
  last_update_ = now;
  const std::array<uint32_t, 3> wanted = {
      static_cast<uint32_t>(resolved.stage),
      static_cast<uint32_t>(resolved.motion),
      static_cast<uint32_t>(resolved.resolution),
  };
  std::array<uint32_t, 3> shown = {};
  bool settling = false;
  for (size_t index = 0u; index < rows_.size(); ++index) {
    Row& row = rows_[index];
    if (!row.shown || row.shown == wanted[index]) {
      row.shown = wanted[index];
      row.candidate.reset();
    } else {
      if (row.candidate != wanted[index]) {
        row.candidate = wanted[index];
        row.since = now;
      }
      if (now - row.since >= settle) {
        row.shown = wanted[index];
        row.candidate.reset();
      }
    }
    shown[index] = *row.shown;
    settling = settling || (*row.shown != wanted[index]);
  }
  resolved.stage = static_cast<SourcePick>(shown[0]);
  resolved.motion = static_cast<MotionPick>(shown[1]);
  resolved.resolution = static_cast<ResolutionMode>(shown[2]);
  // What is shown is what ran a moment ago: never greyed, and the Why belongs to the resolved highlights. Plan 15 fix round: unless nothing can run again.
  if (!resolved.stopped) {
    resolved.stage_options[shown[0]] = {};
    resolved.motion_options[shown[1]] = {};
    resolved.resolution_options[shown[2]] = {};
  }
  if (settling) {
    resolved.why.clear();
  }
  return resolved;
}

void SetupSettle::Clicked(SetupRow row, uint32_t index) {
  Row& clicked = rows_[static_cast<size_t>(row)];
  clicked.shown = index;
  clicked.candidate.reset();
}

void SetupSettle::ClickedBest(SetupRow row, uint32_t index, const SetupView& options, bool motion_auto) {
  const auto runs = [](const auto& states, auto pick) { return states[static_cast<size_t>(pick)].why.empty(); };
  uint32_t best = index;
  switch (row) {
    case SetupRow::STAGE:
      if (!runs(options.stage_options, index)) {
        const bool after_dlss = (index == static_cast<uint32_t>(SourcePick::BEFORE_UPSCALING) && runs(options.stage_options, SourcePick::AFTER_DLSS));
        best = static_cast<uint32_t>(after_dlss ? SourcePick::AFTER_DLSS : SourcePick::PRESENT);
      }
      break;
    case SetupRow::MOTION:
      if (!runs(options.motion_options, index)) {
        const std::optional<uint32_t>& stage = rows_[static_cast<size_t>(SetupRow::STAGE)].shown;
        const bool launchpad = (motion_auto && (!stage || *stage == static_cast<uint32_t>(SourcePick::PRESENT)) && runs(options.motion_options, MotionPick::LAUNCHPAD));
        best = static_cast<uint32_t>(launchpad ? MotionPick::LAUNCHPAD : MotionPick::OFF);
      }
      break;
    case SetupRow::RESOLUTION:
      if (!runs(options.resolution_options, index)) {
        best = static_cast<uint32_t>(ResolutionMode::FULL);
      }
      break;
  }
  Clicked(row, best);
}

std::string FormatStatusLine(const StatusView& view) {
  std::string line(nr::SessionStateName(view.state));
  if (view.suspended) {
    line += " (suspended: game needs VRAM)";
  }
  if (view.state == nr::SessionState::GRACE) {
    // Round up to whole seconds: a caller must never see "released in 0 s" while still counting down.
    const auto seconds_left = (view.grace_remaining.count() + 999) / 1000;
    line += std::format(": NR memory is released in {} s", seconds_left);
    return line;
  }
  if (view.state != nr::SessionState::ACTIVE) return line;
  line += std::format(" | {}/{} passes", view.passes_run, view.passes_requested);
  if (!view.frame.Empty()) {
    line += std::format(" | {}x{}", view.frame.width, view.frame.height);
  }
  if (!view.trigger.empty()) {
    line += std::format(" | {}", view.trigger);
  }
  if (view.encoding) {
    line += std::format(" | {}", color::EncodingName(*view.encoding));
    if (*view.encoding != color::Encoding::SRGB && view.diffuse_white_nits > 0.f) {
      line += std::format(", diffuse white {} nits", view.diffuse_white_nits);
    }
  }
  return line;
}

std::string FormatFrameGenerationLine(bool active, uint32_t multiplier, bool from_ngx) {
  if (!active) return "Frame generation: off";
  return std::format("Frame generation: on ({}x{})", multiplier, (from_ngx ? ", NGX" : ""));
}

std::optional<std::string> FrameGenerationWarning(bool active, uint32_t passes) {
  if (!active || passes <= 1u) return std::nullopt;
  return std::format("Frame generation is on: {} NR passes can push it past its frame budget; one pass is safest", passes);
}

StatusCard BuildStatusCard(const CardFacts& facts) {
  const auto card = [](CardStage stage, std::string title, std::string_view reason, std::vector<std::string> fixes,
                       CardButton button) {
    return StatusCard{.stage = stage, .working = false, .title = std::move(title), .reason = std::string(reason),
                      .fixes = std::move(fixes), .button = button};
  };
  if (!facts.device_problem.empty()) {
    return card(CardStage::DEVICE, "Uplift is not running on this device", facts.device_problem, {}, CardButton::NONE);
  }
  // Only while NR is on: a user who turned it off sees the normal off state, and the failure (remembered by the add-on) comes
  // back with Retry now when they turn it on again.
  // Plan 17: the 2-strike rule's second stop, for a bridge's private device and the helper alike.
  if (facts.private_stops_final) {
    return card(CardStage::DEVICE, "NR stopped twice", PRIVATE_DEVICE_STOPPED_TWICE_REASON, {"Restart the game to use NR again."}, CardButton::NONE);
  }
  // Test phase (review M-3): a stop on the adapter's shared device is final at once (no new device can be made while the game runs).
  if (facts.private_stop_unretryable && !facts.private_stopped.empty()) {
    return card(CardStage::DEVICE, (facts.device_lost ? "The GPU device was removed" : "NR stopped"), facts.private_stopped,
                {"Restart the game to use NR again."}, CardButton::NONE);
  }
  if (!facts.helper_problem.empty() && facts.enabled) {
    return card(CardStage::DEVICE, "Uplift's 64-bit helper stopped", facts.helper_problem, {"Retry now starts it again."},
                CardButton::RETRY_NOW);
  }
  // Plan 17: a bridge's private device stopped (the first time this session): Retry now builds a new one, as the helper's card restarts the helper.
  if (!facts.private_stopped.empty() && facts.enabled) {
    return card(CardStage::DEVICE, (facts.device_lost ? "The GPU device was removed" : "NR stopped"), facts.private_stopped,
                {"Retry now starts NR again on a new device."}, CardButton::RETRY_NOW);
  }
  if (facts.device_lost) {
    return card(CardStage::DEVICE, "The GPU device was removed", facts.output_problem, {"Restart the game to use NR again."},
                CardButton::NONE);
  }
  // Plan 15 fix round (minor 6): NR stopped on this device for the session (the game shut NVIDIA's NGX down while NR's work was still running).
  if (!facts.stopped.empty()) return card(CardStage::DEVICE, "NR stopped", facts.stopped, {"Restart the game to use NR again."}, CardButton::NONE);
  if (facts.claimed_elsewhere) {
    return card(CardStage::DEVICE, "NR runs on another device", "NR runs on another D3D12 device in this game.",
                {"One device per game runs NR; this one waits."}, CardButton::NONE);
  }
  if (!facts.blocked.empty()) {
    if (facts.blocked_stage == CardStage::RUNTIME) {
      return card(CardStage::RUNTIME, "The NR runtime was not found", facts.blocked,
                  {std::format("Copy nvngx_dlssnr.dll next to {} or the game, or set Runtime path (Advanced).", facts.addon_file)},
                  CardButton::NONE);
    }
    return card(CardStage::CONFLICTS, "Another NR tool is running", facts.blocked,
                {"Remove the other NR add-on, or set \"When another NR producer runs\" to Observe (Advanced)."},
                CardButton::NONE);
  }
  if (!facts.enabled) return card(CardStage::SWITCH, "NR is off", "Neural rendering is switched off.", {}, CardButton::TURN_ON);
  // Plan 15: after the game's NGX shutdown on Direct3D 12 NR waits for the game's DLSS, whatever the stage (a temporary cause; Setup greys with the same text).
  if (!facts.held.empty()) return card(CardStage::PLACEMENT, "Waiting for the game's DLSS", facts.held, {}, CardButton::NONE);
  const nr::SessionStatus& session = facts.session;
  if (session.state == nr::SessionState::FAILED) {
    std::string next = (session.retry_in ? std::format("Retrying automatically in {} s.", (session.retry_in->count() + 999) / 1000)
                        : session.retries_exhausted ? std::string("Automatic retries stopped: change a setting, or retry now.")
                                                    : std::string("Retry now, or change a setting."));
    return card(CardStage::SESSION, "NR failed", session.message, {std::move(next)}, CardButton::RETRY_NOW);
  }
  if (session.suspended) {
    return card(CardStage::SESSION, "Paused: the game needs video memory", session.message,
                {"NR resumes when memory frees up (Resume when VRAM frees up).", "Lower Resolution or Pass count to need less."},
                CardButton::NONE);
  }
  if (!facts.output_problem.empty()) {
    return card(CardStage::PLACEMENT, "NR cannot run on this image", facts.output_problem, {}, CardButton::NONE);
  }
  if (!facts.placement_note.empty()) {
    if (facts.dlss_latched) {
      return card(CardStage::PLACEMENT, "NR after DLSS is off", facts.placement_note,
                  {"The device was removed soon after NR ran inside the game's frame.",
                   "Clear the latch to try again from the next start."},
                  CardButton::CLEAR_LATCH);
    }
    return card(CardStage::PLACEMENT, "Waiting", facts.placement_note, {}, CardButton::NONE);
  }
  // FAILED already returned above: only ACTIVE falls through to the placement and skip checks below.
  switch (session.state) {
    case nr::SessionState::OFF:
    case nr::SessionState::LOADING:
      return card(CardStage::SESSION, "Starting NR", "NR loads with the next frame.", {}, CardButton::NONE);
    case nr::SessionState::GRACE:
    case nr::SessionState::DRAINING:
      return card(CardStage::SESSION, "NR is releasing its memory", "NR starts again once the release finishes.", {},
                  CardButton::NONE);
    case nr::SessionState::ACTIVE:
    default: break;
  }
  if (!facts.nr_applied) {
    const std::string_view reason =
        (facts.skip_from_session && !session.message.empty() ? std::string_view(session.message) : facts.skip_reason);
    if (facts.skip_reason == "budget") {
      return card(CardStage::BUDGET, "Not enough video memory for NR", reason,
                  {"Lower Resolution or Pass count, or close other GPU programs."}, CardButton::NONE);
    }
    if (facts.skip_reason == "frame too small" || facts.skip_reason == "resizing") {
      return card(CardStage::FRAME, "Waiting for a usable frame", reason, {"NR needs at least 1280x720."}, CardButton::NONE);
    }
    if (facts.skip_reason == "evaluate failed" || facts.skip_reason == "create failed") {
      return card(CardStage::EVALUATE, "NR's evaluate failed", reason, {}, CardButton::NONE);
    }
    if (facts.skip_reason == "frame generation is on") {
      return card(CardStage::PLACEMENT, "NR skipped this frame", reason,
                  {"Choose After DLSS, or turn on Present with frame generation (Advanced)."}, CardButton::NONE);
    }
    if (reason.empty()) return card(CardStage::PLACEMENT, "Waiting for a frame", "NR has not run yet.", {}, CardButton::NONE);
    return card(CardStage::PLACEMENT, "NR skipped this frame", reason, {}, CardButton::NONE);
  }
  StatusCard working = {.stage = CardStage::WORKING, .working = true, .title = "NR is working",
                        .reason = std::string(facts.working_line)};
  if (facts.passes_run < session.passes_requested) {
    working.fixes.push_back(session.message.empty()
                                ? std::format("Running {} of {} passes.", facts.passes_run, session.passes_requested)
                                : session.message);
  }
  return working;
}

}  // namespace uplift::ui
