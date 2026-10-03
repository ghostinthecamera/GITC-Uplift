#pragma once

#include "ngx_hooks/dlss_capture.hpp"
#include "nr/timeline.hpp"
#include "sources/after_dlss_source.hpp"
#include "sources/nr_pipeline.hpp"

namespace uplift::sources {

// v2 design §3.9: NR on the game's render-resolution colour before its DLSS-SR upscales it. The caller
// has chosen the main handle and ticked the Session. Never blocks. Not thread-safe: the device context's lock.
class PreSrSource {
 public:
  PreSrSource(NrPipeline& pipeline, nr::Timeline& timeline);
  // Before the original evaluate, on the game's list. With nr_applied, the pipeline's PrivateColor() is
  // what DLSS must read as Color for this evaluate.
  PipelineResult Run(DlssFrameHost& host, const ngx_hooks::DlssFrame& frame, bool reset_hint, const AfterDlssConfig& config);
  // Plan 18 (fix round 1, I-1): an evaluate Run never saw (the Direct3D 11 bridge skipped it before recording) did not apply NR either: the next one that
  // does starts its history afresh, as Run's own skips owe it.
  void OweReset() { reset_owed_ = true; }
  [[nodiscard]] bool ResetOwed() const { return reset_owed_; }

 private:
  NrPipeline& pipeline_;
  nr::Timeline& timeline_;
  bool reset_owed_ = false;  // a frame that did not apply NR: the next one that does starts its history afresh
};

}  // namespace uplift::sources
