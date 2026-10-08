#include "addon/live_facts.hpp"

#include "addon/placement_kind.hpp"
#include "sources/motion_source.hpp"

namespace uplift::addon {
namespace {

// What the decided placement and the latest recording's provider say, for both overloads.
void FillStageAndMotion(ui::SetupFacts* facts, Placement placement, sources::MotionSource motion_source, bool working) {
  facts->stage.reset();
  switch (placement) {
    case Placement::NONE:             break;
    case Placement::PRESENT:          facts->stage = ui::SourcePick::PRESENT; break;
    case Placement::AFTER_DLSS:       facts->stage = ui::SourcePick::AFTER_DLSS; break;
    case Placement::BEFORE_UPSCALING: facts->stage = ui::SourcePick::BEFORE_UPSCALING; break;
  }
  facts->motion.reset();
  facts->motion_copied = false;
  if (!working) return;
  switch (motion_source) {
    case sources::MotionSource::NONE:   facts->motion = ui::MotionPick::OFF; break;
    case sources::MotionSource::DIRECT: facts->motion = ui::MotionPick::DLSS; break;
    case sources::MotionSource::PRESENT_COPY:
      facts->motion = ui::MotionPick::DLSS;
      facts->motion_copied = true;
      break;
    case sources::MotionSource::LAUNCHPAD:  // UPLIFT_MV: Launchpad's, or (2026-10-08) Lumenite's while Uplift.fx is compiled for it
      facts->motion = (facts->uplift_mv_lumenite ? ui::MotionPick::LUMENITE : ui::MotionPick::LAUNCHPAD);
      break;
  }
}

}  // namespace

void FillPreference(ui::SetupFacts* facts, const ui::Settings& settings, bool explicit_dlss) {
  facts->preferred_stage = ui::SourcePickOf(settings, explicit_dlss);
  facts->preferred_motion = ui::MotionPickOf(settings);
  facts->motion_auto = (settings.motion_vectors == ui::MotionVectorSource::AUTO);
  facts->preferred_resolution = settings.resolution_mode;
  facts->custom_scale = settings.resolution_scale;
}

#if defined(_WIN64)
void FillRunning(ui::SetupFacts* facts, const ContextStatus& status, bool working) {
  FillStageAndMotion(facts, status.placement, status.motion_source, working);
  facts->game_passes_motion = status.game_passes_motion;
  facts->ray_reconstruction = facts->ray_reconstruction || status.ray_reconstruction;
  facts->dlss_motion_gap = status.dlss_motion_gap;
  facts->launchpad_gap = status.launchpad_gap;
  facts->resolution = status.resolution_applied;
  facts->upsampling = status.upsampling;
  facts->work = status.work;
  facts->canvas = status.canvas;
  facts->frame = status.frame;
  facts->passes_run = status.passes_run;
  facts->passes_requested = status.session.passes_requested;
}
#endif

void FillRunning(ui::SetupFacts* facts, const ipc::Status& status, bool working) {
  FillStageAndMotion(facts, static_cast<Placement>(status.placement), static_cast<sources::MotionSource>(status.motion_source), working);
  facts->dlss_motion_gap = static_cast<ui::MotionGap>(status.dlss_motion_gap);
  facts->launchpad_gap = static_cast<ui::MotionGap>(status.launchpad_gap);
  facts->resolution = (status.resolution_applied <= static_cast<uint32_t>(ui::ResolutionMode::CUSTOM) ? static_cast<ui::ResolutionMode>(status.resolution_applied)
                                                                                                      : ui::ResolutionMode::FULL);  // a block from the helper: never an index out of range
  facts->upsampling = static_cast<color::Upsampling>(status.upsampling);
  facts->work = {status.work_width, status.work_height};
  facts->canvas = {status.canvas_width, status.canvas_height};
  facts->frame = {status.frame_width, status.frame_height};
  facts->passes_run = status.passes_run;
  facts->passes_requested = status.passes_requested;
}

}  // namespace uplift::addon
