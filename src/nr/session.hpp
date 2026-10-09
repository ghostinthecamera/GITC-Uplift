#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nr/budget.hpp"
#include "nr/feature.hpp"
#include "nr/host.hpp"
#include "nr/session_status.hpp"
#include "nr/timeline.hpp"

namespace uplift::nr {

struct SessionConfig {
  std::chrono::milliseconds grace{5000};
  bool auto_resume = true;
  BudgetConfig budget = {};
  bool auto_retry = true;  // Plan 6 (D11): a FAILED session retries on its own backoff
};

// Caller-owned model-domain textures (RGBA16F, SRV + UAV), both in
// NON_PIXEL_SHADER_RESOURCE on entry; `a` holds the encoded input.
struct PassChain {
  ID3D12Resource* a = nullptr;
  ID3D12Resource* b = nullptr;
  Size size;                              // the canvas: NR's network size and the features' capacity
  uint64_t surface_bytes = 0u;            // every surface the caller holds for this chain (v2 design §3.21)
  uint64_t held_intermediate_bytes = 0u;  // bytes the caller holds for this chain now
};

// Plan 5 (v2 design §3.10): records pass `index`'s (0-based, at least 1) own Transfer/Colour strength between NR
// passes. `given` is the pass's input and `returned` its raw output, rewritten in place; both are RGBA16F in
// NON_PIXEL_SHADER_RESOURCE on entry and on return.
class PassResolver {
 public:
  virtual ~PassResolver() = default;
  virtual void ResolvePass(ID3D12GraphicsCommandList* list, uint32_t index, ID3D12Resource* given, ID3D12Resource* returned) = 0;
  // Keep faces (2026-10-08): after pass 1 and its twin, records their recombination: `lighting` (pass 1's raw output, the user's settings) is rewritten
  // with pass 1's result, and `faces` (the twin's output) with the face mask the later passes' ResolvePass reads. Both RGBA16F in NON_PIXEL_SHADER_RESOURCE
  // on entry and on return.
  virtual void CombineFaces(ID3D12GraphicsCommandList* list, ID3D12Resource* lighting, ID3D12Resource* faces) = 0;
};

// Keep faces (2026-10-08): pass 1's twin, a second NR feature evaluated on pass 1's input with its own temporal history. `wanted` false: Keep faces is
// off, and no twin feature lives (one that did is released). Fix round 1: while Session::FacesPaused() the caller frees its surfaces and passes no `output`.
struct FaceTwin {
  bool wanted = false;
  // RGBA16F at the canvas, NON_PIXEL_SHADER_RESOURCE on entry and on return. Null while `wanted`: no twin this frame (paused, or its surfaces could not be
  // made), and a live twin feature stays.
  ID3D12Resource* output = nullptr;
  Controls controls;            // pass 1's, with the Character mask on and Skin structure at Face protection
  uint64_t surface_bytes = 0u;  // the caller's surfaces for the twin, made or not: part of what its resume after a pause needs
};

// Optional per-frame inputs forwarded to every pass.
struct FrameInputs {
  BoundResource motion;
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  BoundResource control_mask;
  BoundResource backbuffer;
  BoundResource ui;
  BoundResource ui_alpha;
  bool ui_correction = false;
  bool reset_hint = false;
  int32_t depth_inverted = 0;
  bool chained_history = true;  // off: every pass after the first resets its history each frame (F25)
  // Plan 5 (v2 design §3.10): pass n ≥ 2's own controls at [n - 2]; nullopt, or a pass past the end, follows `controls`.
  std::span<const std::optional<Controls>> later_passes;
  PassResolver* resolver = nullptr;  // records each later pass's own strengths; null: none
  FaceTwin faces;                    // Keep faces; needs `resolver`, which recombines the twin's output with pass 1's
};

struct EvaluateResult {
  ID3D12Resource* output = nullptr;  // a or b in NON_PIXEL_SHADER_RESOURCE; nullptr when nothing ran
  uint32_t passes_run = 0u;
  std::string_view reason;           // static text; empty when every requested pass ran
};

class Session {
 public:
  Session(Host& host, Timeline& timeline, SessionConfig config = {});
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void SetEnabled(bool enabled);
  void SetPassCount(uint32_t passes);
  void SetCreateOptions(uint32_t preset, uint32_t performance);
  // Spec §12 Advanced settings; they take effect from the next Tick or Evaluate.
  void SetGrace(std::chrono::milliseconds grace) { config_.grace = grace; }
  void SetAutoResume(bool auto_resume) { config_.auto_resume = auto_resume; }
  void SetMarginOverride(std::optional<uint64_t> margin_bytes) { budget_.SetMarginOverride(margin_bytes); }
  void SetVramCheck(VramCheck check) { budget_.SetVramCheck(check); }
  void SetAutoRetry(bool auto_retry) { config_.auto_retry = auto_retry; }
  // Plan 6 (D11): "Retry now". A FAILED session reloads at the next Tick once its failed teardown has finished; the
  // automatic backoff starts over.
  void RetryNow();
  // A settings change: the automatic backoff starts over.
  void ResetRetries() { retry_attempts_ = 0u; }
  // Plan 6 (v2 design §3.16): the Defaults view evaluates only the first `limit` live passes, without a rebuild; 0 = all.
  // A pass that did not run the previous frame restarts its history.
  void SetPassViewLimit(uint32_t limit) { pass_view_limit_ = limit; }
  // Records the current frame's size outside of Evaluate too: keeps resume_size_ current for
  // TryResume's budget check while NR sits off or suspended, and retries a FAILED session to
  // LOADING when the size changes (spec §6.3, §13). Evaluate calls this itself, first; a caller
  // that only reaches Evaluate while ACTIVE (spec §8.1's DeviceContext) should call this every
  // frame regardless of state, or the FAILED-retry and resume_size_ updates never happen.
  void NoteFrameSize(Size size);
  // Once per presented frame, after Timeline::BeginFrame. Never blocks.
  void Tick();
  // Records NR passes on `list`. Never blocks.
  EvaluateResult Evaluate(ID3D12GraphicsCommandList* list, const PassChain& chain,
                          const FrameInputs& inputs, const Controls& controls);
  // Device destruction: bounded synchronous teardown.
  void OnDeviceDestroyed();
  // Device removal: drops every feature and retired entry without releasing
  // them through NGX, abandons the runtime, and leaves the Session
  // permanently inert. Never blocks and never calls Flush.
  void OnDeviceLost();
  // Plan 15: the same drop on a live device, when the game's own NGX shutdown came while NR's last frames had not finished (a capped wait ran out):
  // no NGX call, the runtime abandoned (its patches restored, its module left mapped), and the Session inert for good. Logged as what it is.
  void Abandon();

  [[nodiscard]] SessionState State() const { return state_; }
  // Keep faces fix round 1 (I3): the twin is paused (the budget yielded it, a fit had no room for it, or it failed): its caller frees the twin's surfaces.
  [[nodiscard]] bool FacesPaused() const { return faces_pause_ != FacesPause::NONE; }
  // Keep faces fix round 2 (4): whether Keep faces is on, every frame, before any Evaluate (which a skipped frame never reaches). Off ends a pause and the
  // failure latch at once, so a quick off and on while NR skips frames starts the twin afresh.
  void NoteFacesWanted(bool wanted);
  [[nodiscard]] SessionStatus Status() const;

 private:
  struct Retired {
    Mark mark;
    std::unique_ptr<FeatureInterface> feature;
  };
  // Keep faces: why the twin does not run although it is wanted. BUDGET and UNFIT come back after the resume hold; the failures stay until Keep faces is
  // turned off and on again, or NR loads again from OFF.
  enum class FacesPause : uint8_t {
    NONE,
    BUDGET,           // the game needed VRAM: the twin yielded before any pass
    UNFIT,            // a fit had no room for the twin beside the passes
    FAILED_CREATE,    // its creation failed
    FAILED_EVALUATE,  // its evaluate failed
  };

  // Shared by SetEnabled's OFF->LOADING path, the FinishTeardown retry and
  // TryResume: the resets a fresh Load always needs before it runs.
  void BeginLoading();
  void Load();
  void EnterGrace();
  void BeginTeardown(SessionState next);
  void FinishTeardown();
  // Shared by FinishTeardown and OnDeviceDestroyed: retire every live feature,
  // release every retired feature, and unload the runtime if it is loaded.
  void ReleaseAllFeaturesAndUnload();
  void EnterFailed(std::string message);
  // OnDeviceLost's and Abandon's shared body; `message` is the ERR line it logs last.
  void Drop(std::string_view message);
  void SampleBudget(std::chrono::steady_clock::time_point now);
  void TryResume(std::chrono::steady_clock::time_point now);
  // `faces_wanted`: Keep faces asks for pass 1's twin this frame; `faces_ready`: its surfaces exist this frame.
  bool EnsureFeatures(ID3D12GraphicsCommandList* list, const PassChain& chain, bool faces_wanted, bool faces_ready, std::string_view* reason);
  void RetireFeature(size_t index);
  void RetireFaceFeature();
  void RetireAllFeatures();
  void ReleaseDueRetired();
  [[nodiscard]] uint64_t LiveFeatureBytes() const;

  Host& host_;
  Timeline& timeline_;
  SessionConfig config_;
  Budget budget_;
  SessionState state_ = SessionState::OFF;
  bool enabled_ = false;
  bool suspended_ = false;
  bool runtime_loaded_ = false;
  bool teardown_pending_ = false;
  bool draining_from_failure_ = false;  // this DRAINING must not shortcut back to ACTIVE
  bool device_lost_ = false;  // OnDeviceLost (or Plan 15's Abandon) ran: permanently inert, never loads again
  bool reset_pending_ = true;
  bool first_use_paid_ = false;  // the one-time per-device NR cost has been paid
  bool create_failed_ = false;  // Plan 19: SessionStatus::create_failed
  uint32_t create_result_ = 0u;  // Plan 19 T6: SessionStatus::create_result
  uint32_t pass_count_ = 1u;
  uint32_t passes_limit_ = 10u;
  uint32_t preset_ = 1u;
  uint32_t performance_ = 3u;
  uint32_t consecutive_failures_ = 0u;
  uint32_t retry_attempts_ = 0u;  // automatic retries since the last recovery, settings change or Retry now
  bool retry_requested_ = false;  // RetryNow: reload at the next Tick that may
  uint32_t pass_view_limit_ = 0u;
  size_t passes_evaluated_ = 0u;  // how many passes the last successful Evaluate ran
  std::optional<std::chrono::steady_clock::time_point> failed_at_;
  std::optional<std::chrono::steady_clock::time_point> active_since_;
  Mark last_use_mark_;
  Mark teardown_mark_;
  Size last_size_;
  Size resume_size_;  // latest non-empty frame size passed to Evaluate in any state
  uint64_t resume_surface_bytes_ = 0u;  // the latest chain's surfaces, for TryResume
  // Grace-regression round: summed at the clock's own resolution. Whole milliseconds per tick lost every sub-millisecond frame, so a game running faster
  // than 1000 fps (a native Vulkan game with NR off) never left GRACE.
  std::chrono::steady_clock::duration grace_elapsed_{0};
  std::optional<std::chrono::steady_clock::time_point> last_tick_;
  std::optional<std::chrono::steady_clock::time_point> last_sample_;
  std::optional<std::chrono::steady_clock::time_point> small_since_;
  std::vector<std::unique_ptr<FeatureInterface>> features_;
  // Keep faces (2026-10-08): pass 1's twin, live only while Keep faces asks for it. Under the budget it yields before any pass drops.
  std::unique_ptr<FeatureInterface> face_feature_;
  FacesPause faces_pause_ = FacesPause::NONE;
  bool faces_requested_ = false;       // the latest Evaluate wanted the twin (Status's faces_note)
  uint64_t faces_surface_bytes_ = 0u;  // the latest FaceTwin::surface_bytes
  bool faces_evaluated_ = false;       // the latest successful Evaluate ran the twin
  std::vector<Retired> retired_;
  std::optional<uint64_t> runtime_bytes_;
  std::string message_;
};

}  // namespace uplift::nr
