#pragma once

#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <span>

#include "color/encoding.hpp"
#include "ngx_hooks/dlss_capture.hpp"
#include "nr/timeline.hpp"
#include "nr/types.hpp"
#include "sources/nr_pipeline.hpp"
#include "state/compute_shadow.hpp"

namespace uplift::sources {

// What the add-on (Task 11) or a GPU test supplies for one After-DLSS recording on the game's list.
class DlssFrameHost {
 public:
  virtual ~DlssFrameHost() = default;
  // The native list the game recorded DLSS on; Uplift records on it natively.
  [[nodiscard]] virtual ID3D12GraphicsCommandList* NativeList() = 0;
  // v2 design §3.2: every compute-state change since the list's last Reset was seen. False as well
  // for a list Uplift does not track.
  [[nodiscard]] virtual bool StateKnown() = 0;
  // Replays the game's compute state after Uplift's recording.
  virtual void ReplayState(state::RestoreMode mode) = 0;
  // The recording's completion token travels with the list: submitted and stamped with it (Task 5).
  virtual void AttachToken(uint64_t token) = 0;
};

struct AfterDlssConfig {
  color::Encoding encoding = color::Encoding::AUTO;       // the Encoding setting; AUTO follows DLSS's IsHDR
  float diffuse_white_nits = 0.f;                         // 0: automatic for the resolved encoding
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  bool motion_vectors = true;                             // false: MotionVectors = None, the all-zero texture
  float motion_scale_x = 1.f;                             // MotionScaleX/Y (F23, F24), times DLSS's MV.Scale
  float motion_scale_y = 1.f;
  std::optional<bool> depth_inverted;                     // DepthDirection; nullopt = Game (DLSS's create flag)
  bool chained_history = true;                            // ChainedHistory (F25)
  state::RestoreMode restore = state::RestoreMode::FULL;  // StateRestore
  nr::Controls controls;
  WorkLayout layout;  // v2 design §3.8: set by DeviceContext; empty = Full
};

// The region of DLSS's Output NR runs on (v2 design §3.4): the Output subrect, or the whole Output
// when the create-time output size is unknown.
[[nodiscard]] nr::Rect ResolveOutputRegion(const ngx_hooks::DlssFrame& frame);

// What both DLSS placements take from one evaluate (v2 design §3.4): the target with the game's exposure
// and the resolved encoding, and NR's inputs with the game's motion vectors in place.
struct GameFrame {
  UavTarget target;
  nr::FrameInputs inputs;
};
[[nodiscard]] GameFrame ReadGameFrame(const ngx_hooks::DlssFrame& frame, ID3D12Resource* resource, nr::Rect region,
                                      bool reset_hint, const AfterDlssConfig& config);

// v2 design §3.2/§3.3: an Uplift recording on the game's list. Its completion token is issued first,
// attached to the list once anything was recorded (also when `record` throws), and the game's compute
// state is replayed after it. `record` returns the recording's PipelineResult.
template <class Record>
PipelineResult RecordOnGameList(DlssFrameHost& host, nr::Timeline& timeline, state::RestoreMode restore, Record&& record) {
  const uint64_t token = timeline.IssueToken();  // before recording, so every mark taken while recording includes it
  bool attached = false;
  try {
    const PipelineResult result = record();
    if (!result.recorded) {
      timeline.Drop(std::span<const uint64_t>(&token, 1u));
      return result;
    }
    host.AttachToken(token);
    attached = true;
    host.ReplayState(restore);
    return result;
  } catch (...) {
    // The list may already hold commands: an attached token completes at the list's reset or destroy,
    // or is submitted and stamped at its execute; it is never left RECORDED (Plan 3, Minor 1).
    if (!attached) {
      host.AttachToken(token);
    }
    throw;
  }
}

// v2 design §3.4: NR on DLSS's output inside the game's frame. The caller has chosen the main handle
// and ticked the Session. Never blocks. Not thread-safe: the device context's lock.
class AfterDlssSource {
 public:
  AfterDlssSource(NrPipeline& pipeline, nr::Timeline& timeline);

  PipelineResult Run(DlssFrameHost& host, const ngx_hooks::DlssFrame& frame, bool reset_hint, const AfterDlssConfig& config);
  // Auto is scene-linear (Linear BT.709) for an IsHDR feature and display-referred sRGB otherwise.
  [[nodiscard]] static color::Encoding ResolveEncoding(color::Encoding setting, bool is_hdr);

 private:
  NrPipeline& pipeline_;
  nr::Timeline& timeline_;
  bool reset_owed_ = false;  // a frame that did not apply NR: the next one that does starts its history afresh
};

}  // namespace uplift::sources
