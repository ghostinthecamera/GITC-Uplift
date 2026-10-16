#pragma once

#include <array>
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

inline constexpr uint32_t MAX_PASSES = 10u;

// Split chain (2026-10-10, design 3.2): the passes [first, first + count), 0-based and global, that one stage of the frame runs. `stage` (below
// SESSION_STAGES; addon::Stage's order: Before upscaling, After DLSS, Present) keys that stage's own sizes, resets and Keep faces twin. The default is every
// pass at one stage: the single chain of before the split.
struct StageSlice {
  uint8_t stage = 0u;
  uint32_t first = 0u;
  uint32_t count = MAX_PASSES;
};

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
// Split chain (decision Q5a): one twin per stage, run on the first pass of the slice it comes with; `wanted` says whether that stage is protected.
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
  // Split chain (design 3.2): each stage keeps its own size; this form is pass 1's stage.
  void NoteFrameSize(Size size);
  void NoteFrameSize(uint8_t stage, Size size);
  // Split chain (design 3.2): the frame's stage groups, in chain order (groups[0] holds pass 1), once per frame before any Evaluate. A pass whose stage
  // changed restarts its history when it next runs; no pass's feature is retired here (a feature that no longer fits its stage rebuilds them all in
  // Evaluate). Pass 1's group moving to a stage the chain did not use (a placement switch) takes its stage's state along; a stage that leaves the chain
  // retires its Keep faces twin. The budget reads it: a pass drop removes the last group's passes first, a restored pass and a resume cost each pass at its
  // stage's size, and a fit reserves the earlier groups' missing passes. Without a call, every pass is at stage 0.
  void SetChain(std::span<const StageSlice> groups);
  // Whether any pass of `slice` runs: false when the budget cut its group to 0 (or the Defaults view shows fewer passes), so its stage is skipped.
  [[nodiscard]] bool SliceRuns(const StageSlice& slice) const;
  // Once per presented frame, after Timeline::BeginFrame. Never blocks.
  void Tick();
  // Records `slice`'s NR passes on `list`, on `chain` (that stage's canvas and surfaces). Never blocks. The default slice is every pass.
  EvaluateResult Evaluate(ID3D12GraphicsCommandList* list, const PassChain& chain,
                          const FrameInputs& inputs, const Controls& controls, StageSlice slice = {});
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
  // Split chain: per stage; this form is pass 1's stage.
  [[nodiscard]] bool FacesPaused() const;
  [[nodiscard]] bool FacesPaused(uint8_t stage) const;
  // Keep faces fix round 2 (4): whether Keep faces is on, every frame, before any Evaluate (which a skipped frame never reaches). Off ends a pause and the
  // failure latch at once, so a quick off and on while NR skips frames starts the twin afresh. Split chain: this form is Keep faces itself, for every
  // stage; the two-argument form is one stage's own protection (decision Q5a).
  void NoteFacesWanted(bool wanted);
  void NoteFacesWanted(uint8_t stage, bool wanted);
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
  // Split chain (design 3.2): one stage's own state. With every pass at one stage only that stage's is used, exactly as the single chain's was.
  struct StageState {
    Size last_size;
    Size resume_size;             // latest non-empty frame size noted for this stage, in any state
    uint64_t surface_bytes = 0u;  // the latest chain's surfaces, for TryResume
    std::optional<std::chrono::steady_clock::time_point> small_since;
    uint32_t evaluated_end = 0u;  // the end (global) of the passes the last successful Evaluate here ran: a pass at or past it restarts its history
    bool reset_pending = true;
    // Keep faces (2026-10-08): the stage's twin, live only while Keep faces asks for it here. Under the budget it yields before any pass drops.
    std::unique_ptr<FeatureInterface> face_feature;
    FacesPause faces_pause = FacesPause::NONE;
    bool faces_requested = false;       // the latest Evaluate wanted the twin (Status's faces_note)
    uint64_t faces_surface_bytes = 0u;  // the latest FaceTwin::surface_bytes
    bool faces_evaluated = false;       // the latest successful Evaluate ran the twin
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
  // `faces_wanted`: Keep faces asks for the slice's twin this frame; `faces_ready`: its surfaces exist this frame.
  bool EnsureFeatures(ID3D12GraphicsCommandList* list, const PassChain& chain, const StageSlice& slice, bool faces_wanted, bool faces_ready,
                      std::string_view* reason);
  void RetireFeature(size_t index);
  void RetireFaceFeature(StageState* stage);
  void RetireAllFeatures();
  void ReleaseDueRetired();
  // The live features of `slice`'s passes, and its stage's twin.
  [[nodiscard]] uint64_t LiveFeatureBytes(const StageSlice& slice) const;
  [[nodiscard]] uint32_t LiveFeatureCount() const;
  // Every stage restarts its history: a load, a re-enable, a rebuild.
  void ResetAllStages();
  // Pass 1's stage: the first group's.
  [[nodiscard]] uint8_t FirstStage() const { return groups_[0].stage; }

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
  std::optional<std::chrono::steady_clock::time_point> failed_at_;
  std::optional<std::chrono::steady_clock::time_point> active_since_;
  Mark last_use_mark_;
  Mark teardown_mark_;
  // Grace-regression round: summed at the clock's own resolution. Whole milliseconds per tick lost every sub-millisecond frame, so a game running faster
  // than 1000 fps (a native Vulkan game with NR off) never left GRACE.
  std::chrono::steady_clock::duration grace_elapsed_{0};
  std::optional<std::chrono::steady_clock::time_point> last_tick_;
  std::optional<std::chrono::steady_clock::time_point> last_sample_;
  // Split chain (design 3.2): indexed by pass, global; a pass whose stage has not run yet leaves a hole. Each live feature's capacity is its stage's size.
  std::array<std::unique_ptr<FeatureInterface>, MAX_PASSES> features_;
  std::array<StageState, SESSION_STAGES> stages_;
  std::array<StageSlice, SESSION_STAGES> groups_ = {StageSlice{}};  // SetChain's, in chain order
  uint32_t group_count_ = 1u;
  std::array<uint8_t, MAX_PASSES> pass_stages_ = {};       // each pass's stage in the latest SetChain
  std::array<bool, MAX_PASSES> stage_reset_owed_ = {};     // the pass's stage changed since it last ran
  std::vector<Retired> retired_;
  std::optional<uint64_t> runtime_bytes_;
  std::string message_;
};

}  // namespace uplift::nr
