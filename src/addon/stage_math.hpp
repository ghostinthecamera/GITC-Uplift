#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "addon/chain_plan.hpp"
#include "addon/frame_config.hpp"
#include "addon/placement_kind.hpp"
#include "ngx_hooks/feature_registry.hpp"
#include "nr/log.hpp"
#include "nr/session.hpp"
#include "nr/skip_reason.hpp"
#include "nr/types.hpp"
#include "sources/look_plan.hpp"
#include "sources/work_size.hpp"
#include "ui/settings.hpp"

// The stage and chain decisions both native contexts (DeviceContext, VkDlssContext) make alike. Pure: each takes the context's facts as arguments.
namespace uplift::addon {

// FrameSettings' per-later-stage arrays (stage_resolution, stage_keep_faces): After DLSS's slot, and Present's.
inline constexpr size_t LATER_AFTER_DLSS_SLOT = 0u;
inline constexpr size_t LATER_PRESENT_SLOT = 1u;
// Resolution = Custom's scale (FrameSettings::resolution_scale, StageResolution::scale) is a percentage of the output.
inline constexpr double CUSTOM_SCALE_PERCENT = 100.0;
// Resolution = Full's scale: the output itself.
inline constexpr double FULL_SCALE = 1.0;
// Why the DLSS stages cannot run now: no main evaluate has reached the context yet, or the game switched its DLSS off.
inline constexpr std::string_view DLSS_NOT_RUN_CAUSE = "DLSS has not run yet";
inline constexpr std::string_view DLSS_OFF_CAUSE = "the game's DLSS is off";
// Why Before upscaling cannot run while the DLSS stages can: PreUpscale is off, or the feature is not one NR may run before.
inline constexpr std::string_view BEFORE_UPSCALING_OFF_CAUSE = "Before upscaling is off";
// A stage with no problem of its own: the context learns of none before the chain is decided.
inline constexpr std::string_view NO_STAGE_PROBLEM = {};
// Why a later group cannot run at Present while frame generation is on: the same words as the Present path's skip (nr::SkipReason::FRAME_GENERATION_ON).
inline constexpr std::string_view FRAME_GENERATION_REASON = nr::SkipReasonText(nr::SkipReason::FRAME_GENERATION_ON);

// The stage group of `chain` at `stage` (its index in chain order; 0 is pass 1's), if one is.
[[nodiscard]] inline std::optional<uint32_t> GroupAt(const ChainPlan& chain, Stage stage) {
  for (uint32_t index = 0u; index < chain.group_count; ++index) {
    if (chain.groups[index].stage == stage) return index;
  }
  return std::nullopt;
}

// The route splits (`split_capable`: FrameFacts::split_capable) and some pass 2..N below the pass count is stored at another stage than Same.
[[nodiscard]] inline bool SplitStored(const FrameSettings& config, bool split_capable) {
  if (!split_capable) return false;
  const uint32_t later = std::min(config.session.pass_count, ui::MAX_PASSES) - std::min(config.session.pass_count, 1u);
  return std::any_of(config.pass_stages.begin(), config.pass_stages.begin() + later, [](ui::PassStage stage) { return stage != ui::PassStage::SAME; });
}

// Why a later group's `slice` the Session will not run skips, as Session::Evaluate says it: the Defaults view hides a pass without a reason; a budget
// cut is the Session's budget reason.
[[nodiscard]] inline nr::SkipReason SliceSkipReason(const FrameSettings& config, const nr::StageSlice& slice) {
  const bool hidden_by_view = (config.pass_view_limit != 0u && slice.first >= config.pass_view_limit);
  return (hidden_by_view ? nr::SkipReason::NONE : nr::SkipReason::BUDGET);
}

// "Keep faces here" for a later group at `stage`. Before upscaling is always pass 1's group, which Keep faces itself protects.
[[nodiscard]] inline bool LaterStageKeepsFaces(const FrameSettings& config, Stage stage) {
  switch (stage) {
    case Stage::AFTER_DLSS:       return config.stage_keep_faces[LATER_AFTER_DLSS_SLOT];
    case Stage::PRESENT:          return config.stage_keep_faces[LATER_PRESENT_SLOT];
    case Stage::BEFORE_UPSCALING: break;
  }
  return false;
}

// The DLSS stages fall back to Present: the game switched its DLSS off (DlssOffLatch::Off), unless a core-shutdown hold keeps priority (`core_held`: after
// the game's NGX shutdown NR waits for the game's DLSS to start again, and the shutdown ends the features too).
[[nodiscard]] inline bool DlssStagesOff(bool switched_off, bool core_held) {
  return (switched_off && !core_held);
}

// What a present tells DlssOffLatch::OnPresent: DLSS was seen here, the game released it (FrameFacts::dlss_released), the DLSS placements are available
// (`unavailable` empty), and no core-shutdown hold is in force.
[[nodiscard]] inline bool SwitchOffEvidence(bool dlss_seen, bool released, std::string_view unavailable, bool core_held) {
  return (dlss_seen && released && unavailable.empty() && !core_held);
}

// Why the DLSS stages cannot run now: the placements' unavailable reason, then DLSS not seen yet, then `dlss_off` (DlssStagesOff's); empty when they can.
// A DLSS pause is a wait, never a cause: its groups skip those frames.
[[nodiscard]] inline std::string_view DlssCause(std::string_view unavailable, bool dlss_seen, bool dlss_off) {
  if (!unavailable.empty()) return unavailable;
  if (!dlss_seen) return DLSS_NOT_RUN_CAUSE;
  if (dlss_off) return DLSS_OFF_CAUSE;
  return {};
}

// Why Before upscaling cannot run now: the DLSS stages' cause, then the stage's own `problem`, then Before upscaling being off (`before_upscaling`:
// BeforeUpscaling()); empty when it can.
[[nodiscard]] inline std::string_view BeforeUpscalingCause(std::string_view dlss_cause, std::string_view problem, bool before_upscaling) {
  if (!dlss_cause.empty()) return dlss_cause;
  if (!problem.empty()) return problem;
  return (before_upscaling ? std::string_view() : BEFORE_UPSCALING_OFF_CAUSE);
}

// Why After DLSS cannot run now: the DLSS stages' cause, then the stage's own `problem`; empty when it can.
[[nodiscard]] inline std::string_view AfterDlssCause(std::string_view dlss_cause, std::string_view problem) {
  return (dlss_cause.empty() ? problem : dlss_cause);
}

// The Present path skips a frame for frame generation (NR would also run on generated frames): `frame_generation_on`, each context's evidence of it, or
// Before FG selected, unless `path` lets NR record with frame generation.
[[nodiscard]] inline bool FrameGenerationSkipsPresent(bool frame_generation_on, FgPresentPath path) {
  return ((frame_generation_on || IsBeforeFg(path)) && !AllowsFrameGeneration(path));
}

// Frame generation keeps a later group off Present: on (`frame_generation_on`), not allowed by `path`, and Before FG not selected (an explicit Before FG
// selection keeps its stored Present group, LaterPresentCause).
[[nodiscard]] inline bool FrameGenerationKeepsLaterPresentOff(bool frame_generation_on, FgPresentPath path) {
  return (frame_generation_on && !AllowsFrameGeneration(path) && !IsBeforeFg(path));
}

// Why a later group cannot run at Present: `cause`, except that an explicit Before FG selection keeps its stored Present group even while the hand-off is
// unavailable.
[[nodiscard]] inline std::string_view LaterPresentCause(FgPresentPath path, std::string_view cause) {
  return (IsBeforeFg(path) ? std::string_view{} : cause);
}

// Whether the group at `stage` is pass 1's: it works at Setup's Resolution, a later group at its stage's own. Before upscaling is always pass 1's.
[[nodiscard]] inline bool Pass1Group(const ChainPlan& chain, Stage stage) {
  return (chain.group_count == 0u || chain.groups[0].stage == stage);
}

// What a stage group's work size settles toward, before the size rule (sources::WorkSize).
struct LayoutTarget {
  double width = 0.0;   // the Resolution mode's share of the output, or Match game's render size
  double height = 0.0;
  bool slider_held = false;  // its Custom slider is being dragged: the work size waits
};
// For an output region of `output` at `stage`; `pass1`: Pass1Group's. Match game takes DLSS's create-time render size from `snapshot`, stable under
// dynamic resolution, except before upscaling (NR already works at the render size), until DLSS is seen, and while the game's DLSS is off
// (`dlss_off`: the last create-time size is stale, the game renders at the output size again); Full in those cases.
[[nodiscard]] inline LayoutTarget StageLayoutTarget(const FrameSettings& config, nr::Size output, Stage stage, bool pass1,
                                                    const std::optional<ngx_hooks::CreateSnapshot>& snapshot, bool dlss_off) {
  ui::ResolutionMode mode = config.resolution;
  float custom_scale = config.resolution_scale;
  bool slider_held = config.resolution_slider_held;
  if (!pass1) {
    const FrameSettings::StageResolution& own = config.stage_resolution[(stage == Stage::AFTER_DLSS ? LATER_AFTER_DLSS_SLOT : LATER_PRESENT_SLOT)];
    mode = own.mode;
    custom_scale = own.scale;
    slider_held = own.slider_held;
  }
  const bool before_upscaling = (stage == Stage::BEFORE_UPSCALING);
  double target_width = output.width;
  double target_height = output.height;
  double scale = FULL_SCALE;
  switch (mode) {
    case ui::ResolutionMode::FULL:        break;
    case ui::ResolutionMode::QUALITY:     scale = sources::QUALITY_SCALE; break;
    case ui::ResolutionMode::BALANCED:    scale = sources::BALANCED_SCALE; break;
    case ui::ResolutionMode::PERFORMANCE: scale = sources::PERFORMANCE_SCALE; break;
    case ui::ResolutionMode::CUSTOM:      scale = custom_scale / CUSTOM_SCALE_PERCENT; break;
    case ui::ResolutionMode::MATCH_GAME:
      if (snapshot && !snapshot->render.Empty() && !before_upscaling && !dlss_off) {
        target_width = snapshot->render.width;
        target_height = snapshot->render.height;
      }
      break;
  }
  return {.width = target_width * scale, .height = target_height * scale, .slider_held = slider_held};
}

// The canvas NR runs a settled `image` at: Before upscaling pads an image below the NR floor into the floor canvas; every other stage runs at the image.
[[nodiscard]] inline nr::Size StageCanvas(nr::Size image, Stage stage) {
  return ((stage == Stage::BEFORE_UPSCALING && !nr::MeetsNrFloor(image)) ? sources::CanvasSize(image) : image);
}

// The time since a group's last recording (none for its first), which the stabiliser and the governor take as their frame time; `last` becomes `now`.
[[nodiscard]] inline std::optional<float> FrameSecondsSince(std::optional<std::chrono::steady_clock::time_point>& last,
                                                            std::chrono::steady_clock::time_point now) {
  std::optional<float> seconds;
  if (last) {
    seconds = std::chrono::duration<float>(now - *last).count();  // the pipeline clamps it
  }
  last = now;
  return seconds;
}

// The group whose change Result shaping acts on: the last of `chain` whose slice runs (`slice_runs(group)`), down to pass 1's. Shaping twice would double
// it, so an earlier group's change passes through as it is; a later group the budget cut (or the Defaults view hides) never takes the shaping away. A
// group that skips for the order cannot be known ahead: rare, accepted.
template <typename SliceRuns>
[[nodiscard]] uint32_t ShapedGroup(const ChainPlan& chain, SliceRuns&& slice_runs) {
  uint32_t shaped = (chain.group_count > 0u ? chain.group_count - 1u : 0u);
  while (shaped > 0u && !slice_runs(shaped)) {
    --shaped;
  }
  return shaped;
}

// FrameSettings::look for stage group `group`'s recording: the frame time when known, Result shaping on the `shaped` group only, and Keep faces where it
// protects the group (pass 1's whenever Keep faces is on, a later group only with its stage's "Keep faces here").
[[nodiscard]] inline sources::LookConfig GroupLookConfig(const FrameSettings& config, const ChainPlan& chain, uint32_t group, uint32_t shaped,
                                                         std::optional<float> frame_seconds) {
  sources::LookConfig look = config.look;
  if (frame_seconds) {
    look.frame_seconds = *frame_seconds;
  }
  if (group != shaped) {
    look.look.enabled = false;
  }
  if (group > 0u && group < chain.group_count) {
    look.keep_faces.enabled = (look.keep_faces.enabled && LaterStageKeepsFaces(config, chain.groups[group].stage));
  }
  return look;
}

// A placement the context has not logged yet: the very first one (`logged` starts unset, so NONE with its reason is logged too), or a change. `logged`
// takes it.
[[nodiscard]] inline bool PlacementChanged(std::optional<Placement>& logged, Placement placement) {
  if (logged && *logged == placement) return false;
  logged = placement;
  return true;
}

// The stage a placement runs pass 1 at, whose work size starts over when the placement changes to it; none for NONE.
[[nodiscard]] inline std::optional<Stage> PlacementStage(Placement placement) {
  switch (placement) {
    case Placement::BEFORE_UPSCALING: return Stage::BEFORE_UPSCALING;
    case Placement::AFTER_DLSS:       return Stage::AFTER_DLSS;
    case Placement::PRESENT:          return Stage::PRESENT;
    case Placement::NONE:             break;
  }
  return std::nullopt;
}

// The two chains have the same groups: each at the same stage, with the same passes.
[[nodiscard]] inline bool SameGroups(const ChainPlan& first, const ChainPlan& second) {
  if (first.group_count != second.group_count) return false;
  for (uint32_t index = 0u; index < first.group_count; ++index) {
    const StageGroup& a = first.groups[index];
    const StageGroup& b = second.groups[index];
    if (a.stage != b.stage || a.first != b.first || a.count != b.count) return false;
  }
  return true;
}

// The stages (Stage order) whose own state starts over when the chain moves from `current` to `next`: none while the groups stay the same, otherwise every
// stage either chain leaves out. A stage that joins the chain works at its own size at once, and reports nothing stale from an earlier time.
[[nodiscard]] inline std::array<bool, STAGE_COUNT> StagesToReset(const ChainPlan& next, const ChainPlan& current) {
  std::array<bool, STAGE_COUNT> reset = {};
  if (SameGroups(next, current)) return reset;
  for (size_t stage = 0u; stage < STAGE_COUNT; ++stage) {
    const auto uses = [stage](const ChainPlan& plan) {
      return std::ranges::any_of(std::span(plan.groups).first(plan.group_count),
                                 [stage](const StageGroup& group) { return static_cast<size_t>(group.stage) == stage; });
    };
    reset[stage] = (!uses(next) || !uses(current));
  }
  return reset;
}

// The Session's slices of `chain`, one per group in chain order: the last runs to the end of the passes, so a one-group chain is every pass, every surplus
// idle feature too. Entries past the chain's groups are left as they are.
inline void FillSlices(const ChainPlan& chain, std::array<nr::StageSlice, STAGE_COUNT>& slices) {
  for (uint32_t index = 0u; index < chain.group_count; ++index) {
    const StageGroup& group = chain.groups[index];
    const bool last = (index + 1u == chain.group_count);
    slices[index] = {.stage = static_cast<uint8_t>(group.stage), .first = group.first, .count = (last ? nr::MAX_PASSES - group.first : group.count)};
  }
}

// The chain's log lines for a move from `current` to `next`: the groups only around a split (so a one-group chain logs exactly what the placement line
// already says), then a new note.
inline void LogChainChange(const ChainPlan& next, const ChainPlan& current) {
  if (!SameGroups(next, current)) {
    if (next.Split()) {
      nr::Logf(nr::LogLevel::INFO, "NR chain: {}", FormatChainLine(next, {}));
    } else if (current.Split()) {
      nr::Log(nr::LogLevel::INFO, "NR chain: every pass at one stage");
    }
  }
  if (next.note != current.note && !next.note.empty()) {
    nr::Logf(nr::LogLevel::INFO, "NR chain: {}", next.note);
  }
}

// The settings the Session takes at every present, in this order, from `config` and the chain's later groups (each its own stage's "Keep faces here");
// Keep faces hears its off edge even on a frame NR then skips. `settings_generation` is the last FrameSettings::settings_generation seen: a new one restarts
// the retry backoff. A template over the Session so the unit test can record the calls; the contexts pass their nr::Session.
template <typename SessionSink>
void ApplySessionOptions(SessionSink& session, const FrameSettings& config, const ChainPlan& chain, uint64_t& settings_generation) {
  const ui::SessionOptions& options = config.session;
  session.SetGrace(options.grace);
  session.SetAutoResume(options.auto_resume);
  session.SetMarginOverride(options.margin_override_bytes);
  session.SetVramCheck(options.vram_check);
  session.SetCreateOptions(options.presets, options.performance);
  session.SetPassCount(options.pass_count);
  session.NoteFacesWanted(config.look.keep_faces.enabled);
  for (uint32_t group = 1u; group < chain.group_count; ++group) {
    const Stage stage = chain.groups[group].stage;
    session.NoteFacesWanted(static_cast<uint8_t>(stage), config.look.keep_faces.enabled && LaterStageKeepsFaces(config, stage));
  }
  session.SetAutoRetry(options.auto_retry);
  session.SetPassViewLimit(config.pass_view_limit);
  if (config.settings_generation != settings_generation) {
    settings_generation = config.settings_generation;
    session.ResetRetries();
  }
}

}  // namespace uplift::addon
