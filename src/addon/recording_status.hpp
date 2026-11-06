#pragma once

#include <cstdint>

#include "nr/skip_reason.hpp"
#include "sources/motion_source.hpp"
#include "sources/nr_pipeline.hpp"

namespace uplift::addon {

// The motion scale of no vectors, and of vectors already in the work image's pixels.
inline constexpr float UNIT_MOTION_SCALE = 1.f;

// Pass 1's group's latest recording, as a native context's Status reports it; each stage group's own result is the context's StageState. A recording
// copies its result in (From); a skip says what of the last recording it resets (NoteSkip), so each caller keeps exactly the fields it changes.
struct RecordingStatus {
  // What NoteSkip resets besides setting the reason and clearing skip_from_session.
  enum class Reset : uint8_t {
    ALL,                 // nr_applied, passes_run, the motion source and its scale: nothing of the last recording stands
    ALL_BUT_MOTION,      // nr_applied and passes_run; the motion source and its scale stand
    APPLIED_AND_REASON,  // nr_applied only; the passes, the motion source and its scale stand
    REASON,              // nothing more
  };
  // Where a copied result's motion scale comes from: the result, or the unit scale (a Present copy already scaled).
  enum class Scale : uint8_t { RESULT, UNIT };

  bool nr_applied = false;
  uint32_t passes_run = 0u;
  nr::SkipReason skip_reason = nr::SkipReason::NONE;  // NONE while NR applied
  bool skip_from_session = false;  // skip_reason is Session::Evaluate's own
  sources::MotionSource motion_source = sources::MotionSource::NONE;
  float motion_scale_x = UNIT_MOTION_SCALE;  // with motion_source DIRECT: MV.Scale x MotionScale
  float motion_scale_y = UNIT_MOTION_SCALE;

  // Nothing of any recording.
  void Clear() { *this = {}; }
  // Nothing of any recording, but the motion scale stands.
  void ClearKeepScale() {
    nr_applied = false;
    passes_run = 0u;
    skip_reason = nr::SkipReason::NONE;
    skip_from_session = false;
    motion_source = sources::MotionSource::NONE;
  }
  void From(const sources::PipelineResult& result, Scale scale) {
    nr_applied = result.nr_applied;
    passes_run = result.passes_run;
    skip_reason = (result.nr_applied ? nr::SkipReason::NONE : result.reason);
    skip_from_session = (result.nr_applied ? false : result.from_session);
    motion_source = result.motion_source;
    motion_scale_x = (scale == Scale::RESULT ? result.motion_scale_x : UNIT_MOTION_SCALE);
    motion_scale_y = (scale == Scale::RESULT ? result.motion_scale_y : UNIT_MOTION_SCALE);
  }
  // A recording that did not apply NR, with `reason` (NONE: nothing to explain).
  void NoteSkip(nr::SkipReason reason, Reset reset) {
    if (reset != Reset::REASON) {
      nr_applied = false;
    }
    if (reset == Reset::ALL || reset == Reset::ALL_BUT_MOTION) {
      passes_run = 0u;
    }
    skip_reason = reason;
    skip_from_session = false;
    if (reset == Reset::ALL) {
      motion_source = sources::MotionSource::NONE;
      motion_scale_x = UNIT_MOTION_SCALE;
      motion_scale_y = UNIT_MOTION_SCALE;
    }
  }
};

}  // namespace uplift::addon
