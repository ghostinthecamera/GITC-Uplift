#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "addon/frame_trigger.hpp"
#include "addon/placement.hpp"
#include "color/encoding.hpp"
#include "ngx_hooks/dlss_capture.hpp"
#include "ngx_hooks/frame_generation.hpp"
#include "nr/host.hpp"
#include "nr/real_host.hpp"
#include "nr/session.hpp"
#include "nr/snippet.hpp"
#include "nr/timeline.hpp"
#include "nr/types.hpp"
#include "sources/after_dlss_source.hpp"
#include "sources/pre_sr_source.hpp"
#include "sources/present_source.hpp"
#include "sources/work_size.hpp"
#include "state/compute_shadow.hpp"
#include "ui/settings.hpp"
#include "ui/status_text.hpp"

namespace uplift::addon {

// What the ReShade adapter supplies for one Present-path NR recording (spec §5.2, amendment 1).
class FrameHost {
 public:
  virtual ~FrameHost() = default;
  // Submits what ReShade recorded on the immediate list so far, which also empties its binding cache.
  virtual void FlushPending() = 0;
  // The native immediate list; valid after FlushPending.
  virtual ID3D12GraphicsCommandList* NativeList() = 0;
  // Moves the back buffer between states through ReShade's barrier API.
  virtual void TargetBarrier(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) = 0;
};

// The settings one frame needs, taken at the present and used by the evaluates until the next one.
struct FrameConfig {
  ui::SessionOptions session;
  nr::Controls controls;
  color::Encoding encoding = color::Encoding::AUTO;
  float diffuse_white_nits = 0.f;  // 0 = automatic for the resolved encoding
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  ui::PlacementSource source = ui::PlacementSource::AUTO;
  bool present_with_frame_gen = false;  // spec §11: the Present path runs with frame generation only when set
  bool motion_vectors = true;           // MotionVectors is not None, or must act like it is (ui-review.md Q1(a):
                                         // MotionVectors == Launchpad while placement is inside the game's frame)
  bool motion_vectors_want_launchpad = false;  // ui-review.md Q1(a): MotionVectors == Launchpad, for the motion
                                                // readout's "Launchpad feeds the presented image only" reason
  float motion_scale_x = 1.f;           // MotionScaleX/Y
  float motion_scale_y = 1.f;
  std::optional<bool> depth_inverted;                        // DepthDirection; nullopt = Game
  bool chained_history = true;                               // ChainedHistory
  state::RestoreMode restore = state::RestoreMode::FULL;     // StateRestore
  uint32_t ngx_frame_generation = 0u;                        // live NGX FrameGeneration features (Task 11's registry)
  bool nr_allowed = true;                                    // NrClaim lets this device load NR (Task 11)
  ui::ResolutionMode resolution = ui::ResolutionMode::FULL;  // v2 design §3.8
  float resolution_scale = 100.f;                            // Resolution = Custom: % of the output
  bool resolution_slider_held = false;                       // the Custom slider is being dragged: the work size waits
  color::Upsampling upsampling = color::Upsampling::EDGE_AWARE;
  bool pre_upscale = false;  // v2 design §3.9: NR before DLSS-SR upscales
  sources::LookConfig look;  // Plan 5: the look stage, the colour fixes, Mask, UI correction, passes 2..10
  bool launchpad_motion = true;       // Plan 6: MotionVectors Auto or LaunchPad: the Present path may bind UPLIFT_MV
  bool present_motion_copy = false;   // Plan 14 (design §2.2): a native Vulkan context copies DLSS's motion vectors for the Present path (set by the add-on only)
  bool vulkan = false;                // Plan 14 (batch 2 review, minor 2): a Vulkan device's bridged context, whose motion line must not name Direct3D 12 or Launchpad (set by the add-on only)
  uint64_t upscaler_creates = 0u;     // Plan 15: the game's DLSS creates on this device so far (FeatureRegistry::UpscalerCreates; set by the add-on only)
  uint32_t pass_view_limit = 0u;      // Plan 6 (D6): the Defaults view's passes on the live features; 0 = all
  uint64_t settings_generation = 0u;  // Plan 6 (D11): changes with every saved settings change; restarts the retry backoff
};

struct TargetInfo {
  ID3D12Resource* resource = nullptr;  // the primary swap chain's current back buffer
  color::ColorSpace color_space = color::ColorSpace::UNKNOWN;
  std::string_view problem;  // Plan 7: why the back buffer cannot reach NR (the D3D11 bridge); NR stays off and says it
  // Plan 7 (final review Minor 2): the back buffer's size and format while `resource` is null. The D3D11 bridge makes
  // its shared copy only while NR is not OFF, so the Session decides to load from these; NR runs from the next frame.
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct ContextStatus {
  nr::SessionStatus session;
  std::string message;                        // why NR is blocked or skipped; empty while it runs
  TriggerPoint trigger = TriggerPoint::NONE;  // where the Present path ran most recently
  bool nr_applied = false;                    // on the most recent frame of the chosen placement
  uint32_t passes_run = 0u;
  nr::Size frame;
  std::optional<color::Encoding> encoding;  // the Present path's, resolved; nullopt when unsupported
  float diffuse_white_nits = 0.f;           // effective value
  uint64_t intermediate_bytes = 0u;         // Minor 3: the live set; excludes sets awaiting the GPU
  bool device_lost = false;
  Placement placement = Placement::NONE;
  std::string placement_line;  // FormatPlacementLine
  ngx_hooks::FrameGenState frame_generation;
  bool dlss_seen = false;           // a main-handle evaluate reached this context
  bool dlss_latch_tripped = false;  // removed within 10 s of a game-list recording (v2 design §3.2)
  nr::Size work;                    // the settled work image of the chosen placement
  nr::Size canvas;                  // NR's network size: `work`, or the floor canvas it is padded into
  std::string work_line;            // "Working at 1920x1080 of 3840x2160 (Edge-aware)"; empty before a frame
  sources::MotionSource motion_source = sources::MotionSource::NONE;  // the latest recording's
  std::string motion_line;  // "Motion vectors: DLSS (the game's own, scale 1 x 1)"; never empty
  std::string ui_correction_note;  // Plan 5 (D7): why UI correction does nothing on this placement
  // Plan 6 (D1): the pieces of `message`, for the status card.
  std::string blocked;
  bool claimed_elsewhere = false;
  std::string output_problem;
  std::string placement_note;
  std::string_view skip_reason;
  bool skip_from_session = false;
  // Plan 14 (design §1.3, §1.6): what Setup and the working card need, from every path.
  bool ray_reconstruction = false;                                   // the latest main evaluate's feature is Ray Reconstruction (which keeps NR after DLSS)
  bool game_passes_motion = true;                                    // the latest main evaluate carried motion vectors (true before any)
  ui::MotionGap dlss_motion_gap = ui::MotionGap::NONE;               // why DLSS's vectors are not what NR used on the latest recording
  ui::MotionGap launchpad_gap = ui::MotionGap::NONE;                 // the same for Launchpad's UPLIFT_MV
  ui::ResolutionMode resolution_applied = ui::ResolutionMode::FULL;  // the stored mode, or Full where it cannot apply
  color::Upsampling upsampling = color::Upsampling::EDGE_AWARE;
  // Plan 15: NR is held off after the game's NGX shutdown until its DLSS starts again (ui::D3D12_NGX_SHUT_DOWN_REASON); empty otherwise, and once abandoned
  // (`abandoned`: the shutdown's wait ran out, so NR stays off for the session; the output problem says so).
  std::string_view held;
  bool abandoned = false;
};

// Plan 15: how long the game's Direct3D 12 NGX shutdown waits for NR's submitted work before Uplift releases NR on the device (else abandons it), as Vulkan's.
inline constexpr std::chrono::milliseconds D3D12_CORE_SHUTDOWN_WAIT{2000};

// Plan 5: FrameConfig::look from `settings`, with passes 2..10's `later_controls` (ControlsCoalescer::LaterPasses);
// the frame time is set per recording.
[[nodiscard]] sources::LookConfig LookConfigFrom(const ui::Settings& settings,
                                                 const std::array<std::optional<nr::Controls>, sources::LATER_PASSES>& later_controls);

// Plan 7: the DLSS placements' reason on a bridged context; Source After DLSS shows it on the placement line. Plan 9:
// defined in ui/status_text.hpp, so the 32-bit add-on has it without this header.
inline constexpr std::string_view BRIDGED_DLSS_REASON = ui::BRIDGED_DLSS_REASON;

// Plan 7: how a context runs.
struct ContextOptions {
  bool bridged = false;            // on the D3D11 bridge's private device (design §2): the DLSS placements are unavailable
  std::unique_ptr<nr::Host> host;  // tests: an NR stand-in; null runs NVIDIA's runtime (RealHost)
  // Plan 9 (design §2.10): the game process's DXGI local budget and usage, for RealHost::QueryMemory to combine with its
  // own (the 32-bit helper); nullopt or a zero budget = not measured. Called on the frame's thread, under the caller's lock.
  std::function<std::optional<nr::MemoryInfo>()> game_memory;
};

// Everything NR needs on one D3D12 device: RealHost, Timeline, Session, the shared NrPipeline with its
// Present and After-DLSS sources, the frame trigger, frame-generation detection and the placement.
// ReShade-free, so GPU tests drive it directly. Not thread-safe: the add-on serialises every call
// under its lock, except the three token methods, which are thread-safe.
class DeviceContext {
 public:
  // nullptr with `error` set when the frame fence or the colour pipeline cannot be created. A bridged context keeps
  // BRIDGED_DLSS_REASON whatever SetDlssUnavailableReason says, and initialises an adopted NGX core for its device.
  static std::unique_ptr<DeviceContext> Create(ID3D12Device* device, nr::SnippetConfig snippet_config,
                                               std::string* error, ContextOptions options = {});
  ~DeviceContext();
  DeviceContext(const DeviceContext&) = delete;
  DeviceContext& operator=(const DeviceContext&) = delete;

  // Non-empty keeps NR off with this message (a conflicting host, a missing runtime file).
  void SetBlockedReason(std::string reason);
  // Why the DLSS placements cannot run this session (hooks off or failed, ReShade older than 6.1,
  // the latch); empty when they can. The add-on decides it at start (Task 11).
  void SetDlssUnavailableReason(std::string reason);
  // First thing in the present event of the primary swap chain.
  TriggerPoint BeginFrame(ID3D12CommandQueue* queue, const FrameConfig& config, const TargetInfo& target,
                          bool marker_expected, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  TriggerPoint OnTechnique(bool is_marker) { return trigger_.OnTechnique(is_marker); }
  TriggerPoint OnFinishEffects() { return trigger_.OnFinishEffects(); }
  // Records Present-path NR for this frame. `entry_state`: the back buffer's state in the calling event.
  // `motion`: this frame's UPLIFT_MV, only from the Uplift technique's event; or, with `motion_is_dlss` (Plan 14: the Vulkan bridge's copy of DLSS's vectors,
  // made in the game's frame), those vectors, bound as DLSS's own copy for this present (no Launchpad then).
  // Returns the recording's result; an empty one when nothing ran.
  sources::PresentResult Run(FrameHost* host, D3D12_RESOURCE_STATES entry_state, TriggerPoint point,
                             ID3D12Resource* motion = nullptr, bool motion_is_dlss = false);
  // v2 design §3.1/§3.4: after the hooked evaluate of this device's main handle returned, under the
  // add-on's lock. Records NR after DLSS when that is the chosen placement; otherwise notes the evaluate.
  sources::PipelineResult OnDlssEvaluate(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                         std::chrono::steady_clock::time_point now);
  // v2 design §3.9: before the hooked evaluate of this device's main handle, under the add-on's lock.
  // Records NR before upscaling when that is the chosen placement, and returns the texture DLSS must read
  // as Color for this one call (null: the game's own).
  ID3D12Resource* BeforeDlssEvaluate(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                     std::chrono::steady_clock::time_point now);
  // Minor 9 (Plan 4 fix round 4): BeforeDlssEvaluate applied NR into the private colour (nr_applied_ is
  // already true), but the game's own NGX block had no readable Color for the detour to swap it into,
  // so DLSS reads its own Color unchanged this evaluate. Same call, same thread as BeforeDlssEvaluate;
  // corrects the status to skipped with a reason, instead of leaving it saying NR applied.
  void OnColorSwapRejected();
  // Completion tokens (v2 design §3.3): ReShade's execute_command_list submits; a reset or destroyed
  // list completes at once through DropTokens. Thread-safe.
  void SubmitTokens(std::span<const uint64_t> tokens, ID3D12CommandQueue* queue, uint32_t thread_id,
                    std::chrono::steady_clock::time_point now);
  // N12: a closed list whose Uplift work was first recorded with `first_token` was submitted again
  // without a Reset. Thread-safe, like SubmitTokens.
  void ResubmitTokens(uint64_t first_token, ID3D12CommandQueue* queue, uint32_t thread_id,
                      std::chrono::steady_clock::time_point now);
  void DropTokens(std::span<const uint64_t> tokens);
  // Minor 5: StampThread, at reset, evaluate or present; never from execute_command_list (Task 5's
  // rule: a Signal there could precede the native ExecuteCommandLists that submits the whole batch).
  // Thread-safe.
  void StampThread(uint32_t thread_id);
  // The game destroyed a queue other than the present queue (Timeline::ForgetQueue).
  void ForgetQueue(ID3D12CommandQueue* queue) { timeline_->ForgetQueue(queue); }
  // Device or present-queue destruction: synchronous unload, the only blocking call (2 s cap).
  // After a device removal it leaves the runtime allocated instead. Idempotent.
  void Teardown();
  // Plan 15 (design 2026-10-02): the game is about to shut the NGX core down for this device (NVSDK_NGX_D3D12_Shutdown1: a DLSS setting changed, or the game
  // quits), on the game's thread before the core's own runs. With NR loaded: a wait of at most `cap` for its submitted work (the frame fence and the
  // completion tokens), then the features' release, DestroyParameters, the snippet's Shutdown1 and the unload, so every NGX call of Uplift's precedes the
  // core's; when the wait runs out, NR is abandoned instead (no NGX call) and never loads here again. Either way NR is held off from now on: no NGX call on
  // this device until the game's DLSS starts there again (CoreShutdownHold: BeginFrame sees `upscaler_creates`, FeatureRegistry::UpscalerCreates now, rise,
  // or OnDlssEvaluate sees a feature newer than `serial`, FeatureRegistry::LastSerial now), and the teardown then makes none. A bridged context (a private
  // device) never gets this call. Not after a removal or a teardown.
  void OnCoreShutdown(std::chrono::milliseconds cap, uint64_t upscaler_creates, uint64_t serial);
  [[nodiscard]] bool CoreHeld() const { return core_hold_.Held(); }
  // Plan 15 fix round (minor 3): the hold outlives this context. The add-on keeps the device's copy when destroy_command_queue ends the context, and a new
  // context on the same game device starts from it (before its first BeginFrame), so it never loads NR into a core the game shut down, or into an abandoned
  // runtime.
  [[nodiscard]] const CoreShutdownHold& CoreHold() const { return core_hold_; }
  void SeedCoreHold(const CoreShutdownHold& hold) { core_hold_ = hold; }
  // Plan 5 (v2 design §3.13, key decision 8): the texture the add-on copies the effect's UPLIFT_MASK into at the end
  // of ReShade's effects. Null, with the copy released, unless NR is running with Mask = Auto and `mask` is readable.
  ID3D12Resource* PrepareMaskCopy(const D3D12_RESOURCE_DESC* mask);
  void NoteMaskCopied() { present_source_->Pipeline().NoteMaskCopied(); }
  // Plan 6 (D11): "Retry now". Never after a removal or a teardown.
  void RetryNow() {
    if (torn_down_ || device_lost_) return;  // never back into the snippet after a removal
    session_->RetryNow();
  }

  [[nodiscard]] ContextStatus Status() const;
  [[nodiscard]] ID3D12CommandQueue* PresentQueue() const { return present_queue_; }
  // Minor (fix round 2): the one bool NoteLatchIfTripped needs on every present, evaluate and
  // teardown, without building a full Status() (string formatting included) just to read it.
  [[nodiscard]] bool LatchTripped() const { return dlss_latch_tripped_; }
  // Plan 7: BeginFrame readied a Present-path recording that Run has not made yet. The D3D11 bridge starts its fence
  // hand-off only then.
  [[nodiscard]] bool FrameReady() const { return frame_ready_; }
  // Plan 7: the Session's state (OFF once torn down after a removal), for the bridge's surfaces and its mask copy.
  [[nodiscard]] nr::SessionState NrState() const { return (session_ ? session_->State() : nr::SessionState::OFF); }

 private:
  DeviceContext(ID3D12Device* device, nr::SnippetConfig snippet_config, std::unique_ptr<nr::Timeline> timeline,
               ContextOptions options);
  // Spec §13: true, with the context latched off, when the device was removed. BeginFrame and
  // OnDlssEvaluate both check, so no evaluate between a removal and the next present reaches the snippet.
  bool NoteDeviceRemoved(std::chrono::steady_clock::time_point now);
  // Fix round 1, Important 2: the per-frame Session work that both BeginFrame and OnDlssEvaluate's
  // present-starvation catch-up need -- the idle recompute, SetEnabled, Tick and the release on
  // OFF/FAILED -- so an evaluate that drives the Timeline while presents starve reloads NR exactly
  // as a present would, instead of finding a stale placement and an unticked Session.
  void UpdateSession(std::chrono::steady_clock::time_point now);
  // v2 design §3.8/§3.9: the settled work layout for an output region of `output` on the current
  // placement. Noted to the Session in every state (N10), so a FAILED session retries when it changes.
  sources::WorkLayout SettleLayout(nr::Size output, bool before_upscaling, std::chrono::steady_clock::time_point now);
  // Minor 2 (Plan 4 fix round 4): the bookkeeping a placement_ change needs wherever it is decided
  // (BeginFrame, and OnDlssEvaluate's present-starvation catch-up) -- logged once, and the settle
  // starts over so the new placement's work size applies at once. A no-op when the placement did not
  // change.
  void NotePlacementChange();
  // The per-evaluate settings both DLSS placements run with.
  [[nodiscard]] sources::AfterDlssConfig DlssConfig(const sources::WorkLayout& layout) const;
  // PreUpscale is on and the main handle is DLSS-SR (Ray Reconstruction keeps NR after DLSS).
  [[nodiscard]] bool BeforeUpscaling() const;
  // Plan 5: FrameConfig::look for the recording about to run, with Δt since this context's last recording (v2 design
  // §3.12: between main-handle evaluates on the DLSS placements, between presents on the Present path).
  void ApplyLookConfig(std::chrono::steady_clock::time_point now);
  std::optional<std::chrono::steady_clock::time_point> last_look_recording_;

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  std::unique_ptr<nr::Host> host_;
  std::unique_ptr<nr::Timeline> timeline_;
  std::unique_ptr<nr::Session> session_;
  std::unique_ptr<sources::PresentSource> present_source_;
  std::unique_ptr<sources::AfterDlssSource> after_dlss_;  // shares present_source_'s NrPipeline
  std::unique_ptr<sources::PreSrSource> pre_sr_;          // after after_dlss_; shares present_source_'s NrPipeline
  sources::WorkSizeSettle settle_;
  sources::WorkLayout present_layout_;
  nr::Size work_size_;
  nr::Size canvas_size_;
  NVSDK_NGX_Feature main_feature_ = NVSDK_NGX_Feature_Reserved_Unknown;
  FrameTrigger trigger_;
  ngx_hooks::FrameGenDetector frame_generation_;
  ngx_hooks::ResetMerger reset_merger_;
  ID3D12CommandQueue* present_queue_ = nullptr;
  FrameConfig config_;
  PlacementChoice placement_;
  std::string blocked_reason_;
  std::string dlss_unavailable_reason_;
  std::string output_message_;  // device removal, unsupported format or colour space
  std::string_view skip_reason_;
  bool skip_from_session_ = false;  // M1 fix round 1: skip_reason_ is Session::Evaluate's own
  sources::PresentTarget frame_target_;
  nr::Controls controls_;
  std::optional<color::Encoding> encoding_;
  std::optional<ngx_hooks::CreateSnapshot> main_snapshot_;
  std::optional<std::chrono::steady_clock::time_point> last_present_;
  std::optional<std::chrono::steady_clock::time_point> last_main_evaluate_;
  std::optional<std::chrono::steady_clock::time_point> last_game_list_recording_;
  nr::Size frame_size_;
  float diffuse_white_nits_ = 0.f;
  TriggerPoint trigger_used_ = TriggerPoint::NONE;
  TriggerPoint logged_trigger_ = TriggerPoint::NONE;
  std::optional<Placement> logged_placement_;  // Minor 8: nullopt logs the first placement, NONE included
  nr::SessionState logged_state_ = nr::SessionState::OFF;
  uint32_t passes_run_ = 0u;
  uint32_t trace_frames_ = 0u;
  bool nr_applied_ = false;
  sources::MotionSource motion_source_ = sources::MotionSource::NONE;  // the latest recording's
  float motion_scale_x_ = 1.f;
  float motion_scale_y_ = 1.f;
  // Set on every evaluate (After DLSS, Before upscaling and Source = Present's copy attempt alike):
  // whether the game passed motion vectors this evaluate, for the overlay's "the game passed no motion
  // vectors" reason. Stale between evaluates is harmless: it is read only while motion_source_ == NONE.
  bool last_motion_vectors_missing_ = false;
  bool frame_ready_ = false;
  std::atomic<bool> device_lost_{false};  // also read by StampThread on game threads
  bool torn_down_ = false;
  bool dlss_seen_ = false;
  bool dlss_idle_ = false;
  bool dlss_paused_ = false;  // final review, minor 1: no evaluate for 250 ms, at any placement (Status's DLSS_PAUSED)
  bool dlss_latch_tripped_ = false;
  bool logged_after_dlss_ = false;  // "NR trigger: after DLSS" was logged in this activation
  // Important 1 (fix round 2): a submit-only thread's tokens age out only after Timeline's own
  // AGED_SUBMISSION_TIME, so the descriptor ring can stay fully busy for a while by design on that
  // engine shape. Tracks how long "descriptors busy" has been the After-DLSS result, back to back, so
  // one WARN per context tells the difference from a genuinely undersized ring.
  std::optional<std::chrono::steady_clock::time_point> descriptors_busy_since_;
  bool warned_descriptors_busy_ = false;
  uint64_t settings_generation_ = 0u;  // Plan 6 (D11): the last FrameConfig::settings_generation seen
  bool bridged_ = false;               // Plan 7: ContextOptions::bridged
  CoreShutdownHold core_hold_;         // Plan 15: the game shut NGX down on this device; NR waits for its DLSS (OnCoreShutdown)
};

}  // namespace uplift::addon
