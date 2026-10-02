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
  [[nodiscard]] SessionStatus Status() const;

 private:
  struct Retired {
    Mark mark;
    std::unique_ptr<FeatureInterface> feature;
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
  bool EnsureFeatures(ID3D12GraphicsCommandList* list, const PassChain& chain, std::string_view* reason);
  void RetireFeature(size_t index);
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
  std::chrono::milliseconds grace_elapsed_{0};
  std::optional<std::chrono::steady_clock::time_point> last_tick_;
  std::optional<std::chrono::steady_clock::time_point> last_sample_;
  std::optional<std::chrono::steady_clock::time_point> small_since_;
  std::vector<std::unique_ptr<FeatureInterface>> features_;
  std::vector<Retired> retired_;
  std::optional<uint64_t> runtime_bytes_;
  std::string message_;
};

}  // namespace uplift::nr
