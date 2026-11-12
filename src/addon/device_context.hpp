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

#include "addon/chain_plan.hpp"
#include "addon/frame_config.hpp"
#include "addon/frame_trigger.hpp"
#include "addon/placement.hpp"
#include "addon/recording_status.hpp"
#include "addon/stage_book.hpp"
#include "addon/stage_math.hpp"
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

// What the ReShade adapter supplies for one Present-path NR recording.
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

struct TargetInfo {
  ID3D12Resource* resource = nullptr;  // the primary swap chain's current back buffer
  color::ColorSpace color_space = color::ColorSpace::UNKNOWN;
  std::string_view problem;  // why the back buffer cannot reach NR (the D3D11 bridge); NR stays off and says it
  // The back buffer's size and format while `resource` is null. The D3D11 bridge makes
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
  uint64_t intermediate_bytes = 0u;         // the live set; excludes sets awaiting the GPU
  bool device_lost = false;
  Placement placement = Placement::NONE;
  std::string placement_line;  // FormatPlacementLine
  ngx_hooks::FrameGenState frame_generation;
  bool dlss_seen = false;           // a main-handle evaluate reached this context
  bool dlss_latch_tripped = false;  // removed within DLSS_LATCH_WINDOW of a game-list recording
  nr::Size work;                    // the settled work image of the chosen placement
  nr::Size canvas;                  // NR's network size: `work`, or the floor canvas it is padded into
  std::string work_line;            // "Working at 1920x1080 of 3840x2160 (Edge-aware)"; empty before a frame
  std::string exposure_line;        // "Input exposure: the game's" (sources::ExposureLine); empty unless NR ran on the latest frame and used one
  sources::MotionSource motion_source = sources::MotionSource::NONE;  // the latest recording's
  std::string motion_line;  // "Motion vectors: DLSS (the game's own, scale 1 x 1)"; never empty
  std::string ui_correction_note;  // why UI correction does nothing on this placement
  bool keep_faces_supported = true;  // Keep faces: the pipeline can recombine the extra run (ui::KeepFacesFacts::gpu_ready)
  // The pieces of `message`, for the status card.
  std::string blocked;
  bool claimed_elsewhere = false;
  std::string output_problem;
  std::string placement_note;
  nr::SkipReason skip_reason = nr::SkipReason::NONE;
  bool skip_from_session = false;
  // What Setup and the working card need, from every path.
  bool game_passes_motion = true;                                    // the latest main evaluate carried motion vectors (true before any)
  ui::MotionGap dlss_motion_gap = ui::MotionGap::NONE;               // why DLSS's vectors are not what NR used on the latest recording
  ui::MotionGap launchpad_gap = ui::MotionGap::NONE;                 // the same for Launchpad's UPLIFT_MV
  ui::ResolutionMode resolution_applied = ui::ResolutionMode::FULL;  // the stored mode, or Full where it cannot apply
  color::Upsampling upsampling = color::Upsampling::EDGE_AWARE;
  // NR is held off after the game's NGX shutdown until its DLSS starts again (ui::D3D12_NGX_SHUT_DOWN_REASON); empty otherwise, and once abandoned
  // (`abandoned`: the shutdown's wait ran out, so NR stays off for the session; the output problem says so).
  std::string_view held;
  bool abandoned = false;
  std::string after_dlss_problem;        // NoteStageProblem's, for Setup
  std::string before_upscaling_problem;
  // Split chain: Details' "Chain: passes 1-2 before upscaling (960x540), 3 at Present (1920x1080)" while the chain is split (empty
  // otherwise), the card's note when a stored "Runs at" does not run as stored (ChainPlan::note), and each stage group's latest recording. Every other
  // field above describes pass 1's group.
  std::string chain_line;
  std::string chain_note;
  struct Group {
    Stage stage = Stage::PRESENT;
    uint32_t first = 0u;  // 0-based
    uint32_t count = 0u;
    bool nr_applied = false;
    uint32_t passes_run = 0u;
    nr::SkipReason skip_reason = nr::SkipReason::NONE;
    sources::MotionSource motion_source = sources::MotionSource::NONE;
    nr::Size work;
    nr::Size canvas;
  };
  std::array<Group, STAGE_COUNT> groups = {};
  uint32_t group_count = 0u;
};

// How long the game's Direct3D 12 NGX shutdown waits for NR's submitted work before Uplift releases NR on the device (else abandons it), as Vulkan's.
inline constexpr std::chrono::milliseconds D3D12_CORE_SHUTDOWN_WAIT{2000};

// The DLSS placements' reason on a bridged context; Source After DLSS shows it on the placement line.
// defined in ui/status_text.hpp, so the 32-bit add-on has it without this header.
inline constexpr std::string_view BRIDGED_DLSS_REASON = ui::BRIDGED_DLSS_REASON;

// What a Direct3D 11 bridge's context wants done at one hooked DLSS evaluate of the main handle, decided before anything is copied.
enum class BridgedDlssWork : uint8_t {
  NONE,              // a passthrough: nothing crosses for this evaluate
  AFTER_DLSS,        // after the original evaluate: the hand-off around NR on DLSS's output
  BEFORE_UPSCALING,  // before the original evaluate: the hand-off around NR on DLSS's colour input
  PRESENT_MOTION,    // after the original evaluate: DLSS's vectors into the Present path's ring
};
struct BridgedDlssDecision {
  BridgedDlssWork work = BridgedDlssWork::NONE;
  // Split chain: pass 1 runs inside the game's frame and a later group at Present, so DLSS's vectors also go into the Present path's ring, before
  // `work` (After DLSS) runs. PRESENT_MOTION (pass 1 at Present) never sets it.
  bool present_motion = false;
  float motion_scale_x = 1.f;  // PRESENT_MOTION or `present_motion`: MV.Scale x MotionScaleX, the scale the ring slot carries to the Present path
  float motion_scale_y = 1.f;
};

// How a context runs.
struct ContextOptions {
  bool bridged = false;            // on the D3D11 bridge's private device: the DLSS placements are unavailable
  bool dlss_stages = false;        // A Direct3D 11 bridge's context: the DLSS stages run through the bridge's mid-frame hand-off (no BRIDGED_DLSS_REASON)
  std::unique_ptr<nr::Host> host;  // tests: an NR stand-in; null runs NVIDIA's runtime (RealHost)
  // The game process's DXGI local budget and usage, for RealHost::QueryMemory to combine with its
  // own (the 32-bit helper); nullopt or a zero budget = not measured. Called on the frame's thread, under the caller's lock.
  std::function<std::optional<nr::MemoryInfo>()> game_memory;
};

// Everything NR needs on one D3D12 device: RealHost, Timeline, Session, the shared NrPipeline with its
// Present and After-DLSS sources, the frame trigger, frame-generation detection and the placement.
// ReShade-free, so GPU tests drive it directly. Not thread-safe: the add-on serialises every call
// under its lock, except SubmitTokens, ResubmitTokens, DropTokens and StampThread, which are thread-safe.
class DeviceContext {
 public:
  // nullptr with `error` set when the frame fence or the colour pipeline cannot be created. A bridged context keeps
  // BRIDGED_DLSS_REASON whatever SetDlssUnavailableReason says (unless made with ContextOptions::dlss_stages), and initialises an adopted NGX
  // core for its device.
  static std::unique_ptr<DeviceContext> Create(ID3D12Device* device, nr::SnippetConfig snippet_config,
                                               std::string* error, ContextOptions options = {});
  ~DeviceContext();
  DeviceContext(const DeviceContext&) = delete;
  DeviceContext& operator=(const DeviceContext&) = delete;

  // Non-empty keeps NR off with this message (a conflicting host, a missing runtime file).
  void SetBlockedReason(std::string reason);
  // Why the DLSS placements cannot run this session (hooks off or failed, ReShade older than 6.1,
  // the latch); empty when they can. The add-on decides it at start.
  void SetDlssUnavailableReason(std::string reason);
  // First thing in the present event of the primary swap chain.
  [[nodiscard]] bool HasFrameGenerationCadence() const { return frame_generation_.HasCadence(); }
  void NoteDisplayedPresent(std::chrono::steady_clock::time_point now, uint32_t ngx_live) {
    frame_generation_.OnPresent(now);
    frame_generation_.Update(now, ngx_live);
  }
  TriggerPoint BeginFrame(ID3D12CommandQueue* queue, const FrameSettings& settings, const FrameFacts& facts, const TargetInfo& target,
                          bool marker_expected, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  TriggerPoint OnBeginEffects(bool usable) { return trigger_.OnBeginEffects(usable); }  // 1.1.6
  TriggerPoint OnTechnique(bool is_marker) { return trigger_.OnTechnique(is_marker); }
  void EndFrame() { trigger_.EndFrame(); }
  // Records Present-path NR for this frame. `entry_state`: the back buffer's state in the calling event.
  // `motion`: this frame's UPLIFT_MV, only from the Uplift technique's event; or, with `motion_is_dlss` (the Vulkan bridge's copy of DLSS's vectors,
  // made in the game's frame), those vectors, bound as DLSS's own copy for this present (no Launchpad then).
  // `dlss_scale_x/y`, with `motion_is_dlss`: the vectors' scale (MV.Scale x MotionScale), for Direct3D 11's ring, which keeps DLSS's raw vectors;
  // 1 for Vulkan's copies, which the copy already scaled. On a context made with ContextOptions::dlss_stages they are raw, so the
  // Present path filters and scales them through the motion copy first, from `dlss_motion_region` (empty: the whole texture).
  // Returns the recording's result; an empty one when nothing ran.
  sources::PresentResult Run(FrameHost* host, D3D12_RESOURCE_STATES entry_state, TriggerPoint point,
                             ID3D12Resource* motion = nullptr, bool motion_is_dlss = false, float dlss_scale_x = 1.f,
                             float dlss_scale_y = 1.f, nr::Rect dlss_motion_region = {});
  // After the hooked evaluate of this device's main handle returned, under the
  // add-on's lock. Records NR after DLSS when that is the chosen placement; otherwise notes the evaluate.
  sources::PipelineResult OnDlssEvaluate(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                         std::chrono::steady_clock::time_point now);
  // Before the hooked evaluate of this device's main handle, under the add-on's lock.
  // Records NR before upscaling when that is the chosen placement, and returns the texture DLSS must read
  // as Color for this one call (null: the game's own).
  ID3D12Resource* BeforeDlssEvaluate(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                     std::chrono::steady_clock::time_point now);
  // BeforeDlssEvaluate applied NR into the private colour (the status
  // already says so), but the game's own NGX block had no readable Color for the detour to swap it into,
  // so DLSS reads its own Color unchanged this evaluate. Same call, same thread as BeforeDlssEvaluate;
  // corrects the status to skipped with a reason, instead of leaving it saying NR applied.
  void OnColorSwapRejected();
  // Before the original evaluate of the main handle on a Direct3D 11 bridge's context: BEFORE_UPSCALING when NR runs before upscaling
  // for this evaluate (the placement, DLSS not idle, DLSS-SR, NR ACTIVE, Intensity above 0), after settling the work size on `render_region` (in every
  // state); NONE otherwise, with the status as BeforeDlssEvaluate leaves it. Only for a context made with ContextOptions::dlss_stages.
  BridgedDlssDecision DecideBeforeEvaluate(const ngx_hooks::DlssFrame& frame, nr::Size render_region, std::chrono::steady_clock::time_point now);
  // After the original evaluate: OnDlssEvaluate's bookkeeping (DLSS seen, the cadence, the present-starvation catch-up), then AFTER_DLSS when NR runs after
  // DLSS for this evaluate (the work size settled on `output_region` first, the NR floor, Intensity above 0), PRESENT_MOTION when Source = Present wants
  // DLSS's vectors (NR ACTIVE, the game passed vectors), else NONE. Split chain: `present_motion` too when a later group runs at Present.
  BridgedDlssDecision DecideAfterEvaluate(const ngx_hooks::DlssFrame& frame, nr::Size output_region, std::chrono::steady_clock::time_point now);
  // The decided AFTER_DLSS or BEFORE_UPSCALING on the private list `host` names, `frame` pointing at the bridge's shared copies; the status as OnDlssEvaluate
  // and BeforeDlssEvaluate set it. Before upscaling with nr_applied: PrivateColor() holds NR's result. Split chain: the stage's own group of the
  // chain (its slice and stage set), as Direct3D 12's evaluates record it.
  sources::PipelineResult RecordBridgedStage(BridgedDlssWork work, const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host,
                                             std::chrono::steady_clock::time_point now);
  // Split chain: the current chain has a group at `stage` (BeginFrame's). The Direct3D 11 bridge keeps both DLSS stages' shares while both run,
  // and makes its back-buffer copy for a later group at Present.
  [[nodiscard]] bool RunsAt(Stage stage) const { return GroupAt(chain_, stage).has_value(); }
  // Split chain: the Direct3D 11 bridge's ring of DLSS's vectors for the Present path is wanted: pass 1 at Present with
  // FrameFacts::present_motion_copy, or a later group at Present while Motion vectors is not None. The add-on also drops it while NR is OFF.
  [[nodiscard]] bool PresentRingWanted() const;
  // The pipeline's private colour (RGBA16F at the colour's size), where Before upscaling's recording leaves NR's result.
  [[nodiscard]] ID3D12Resource* PrivateColor() const { return present_source_->Pipeline().PrivateColor(); }
  // The bridge cannot share this stage's image: the stage is greyed with `problem` (Setup) and Auto runs at Present from the next
  // present (Before upscaling: NR runs after DLSS). Logged once per stage.
  void NoteStageProblem(BridgedDlssWork stage, std::string problem);
  // A decided stage's evaluate did not reach NR (the Decide skips, or the bridge's hand-off skipped before its recording or never
  // submitted it): the status says NR was not applied, with `reason`, and the stage's next recording starts NR's history afresh, as
  // Direct3D 12's sources owe it for their own skips.
  void NoteBridgedSkip(BridgedDlssWork stage, nr::SkipReason reason);
  // Whether the stage's next recording starts NR's history afresh. For the tests.
  [[nodiscard]] bool StageResetOwed(BridgedDlssWork stage) const;
  // The game's own Direct3D 11 device was removed (`reason`): within DLSS_LATCH_WINDOW of NR running inside its frame the DLSS placements latch off
  // (LatchTripped), as a Direct3D 12 game's removal does. A bridged context's own private device never latches them: its removal gets Retry now.
  void NoteGameDeviceRemoved(HRESULT reason, std::chrono::steady_clock::time_point now);
  // A main-handle evaluate reached this context (the add-on's foreign-device reason gives way to it), and the placement BeginFrame chose.
  [[nodiscard]] bool DlssSeen() const { return dlss_seen_; }
  // The observed main feature, including SR after RR; empty until an evaluate reached this context.
  [[nodiscard]] std::optional<NVSDK_NGX_Feature> ObservedDlssFeature() const { return dlss_seen_ ? std::optional{main_feature_} : std::nullopt; }
  [[nodiscard]] Placement CurrentPlacement() const { return placement_.placement; }
  // Completion tokens: ReShade's execute_command_list submits; a reset or destroyed
  // list completes at once through DropTokens. Thread-safe.
  void SubmitTokens(std::span<const uint64_t> tokens, ID3D12CommandQueue* queue, uint32_t thread_id,
                    std::chrono::steady_clock::time_point now);
  // A closed list whose Uplift work was first recorded with `first_token` was submitted again
  // without a Reset. Thread-safe, like SubmitTokens.
  void ResubmitTokens(uint64_t first_token, ID3D12CommandQueue* queue, uint32_t thread_id,
                      std::chrono::steady_clock::time_point now);
  void DropTokens(std::span<const uint64_t> tokens);
  // StampThread runs at reset, evaluate or present, and at execute_command_list only for a list reset after the thread's last token submission:
  // any other execute_command_list could precede the native ExecuteCommandLists of the batch its tokens were submitted in.
  // `later_call`: the stamp was made at an execute_command_list event for a list that proves a later ExecuteCommandLists call than the tokens'; the
  // tokens it stamps are counted apart for the diagnostic log. Thread-safe.
  void StampThread(uint32_t thread_id, bool later_call = false);
  // The game destroyed a queue other than the present queue (Timeline::ForgetQueue).
  void ForgetQueue(ID3D12CommandQueue* queue) { timeline_->ForgetQueue(queue); }
  // Device or present-queue destruction: synchronous unload, the only blocking call (capped at Session's TEARDOWN_WAIT).
  // After a device removal it leaves the runtime allocated instead. Idempotent.
  void Teardown();
  // The game is about to shut the NGX core down for this device (NVSDK_NGX_D3D12_Shutdown1: a DLSS setting changed, or the game
  // quits), on the game's thread before the core's own runs. With NR loaded: a wait of at most `cap` for its submitted work (the frame fence and the
  // completion tokens), then the features' release, DestroyParameters, the snippet's Shutdown1 and the unload, so every NGX call of Uplift's precedes the
  // core's; when the wait runs out, NR is abandoned instead (no NGX call) and never loads here again. Either way NR is held off from now on: no NGX call on
  // this device until the game's DLSS starts there again (CoreShutdownHold: BeginFrame sees `upscaler_creates`, FeatureRegistry::UpscalerCreates now, rise,
  // or OnDlssEvaluate sees a feature newer than `serial`, FeatureRegistry::LastSerial now), and the teardown then makes none. A bridged context (a private
  // device) never gets this call. Not after a removal or a teardown.
  void OnCoreShutdown(std::chrono::milliseconds cap, uint64_t upscaler_creates, uint64_t serial);
  [[nodiscard]] bool CoreHeld() const { return core_hold_.Held(); }
  // The hold outlives this context. The add-on keeps the device's copy when destroy_command_queue ends the context, and a new
  // context on the same game device starts from it (before its first BeginFrame), so it never loads NR into a core the game shut down, or into an abandoned
  // runtime.
  [[nodiscard]] const CoreShutdownHold& CoreHold() const { return core_hold_; }
  void SeedCoreHold(const CoreShutdownHold& hold) { core_hold_ = hold; }
  // The texture the add-on copies the effect's UPLIFT_MASK into at the end
  // of ReShade's effects. Null, with the copy released, unless NR is running with Mask = Auto and `mask` is readable.
  ID3D12Resource* PrepareMaskCopy(const D3D12_RESOURCE_DESC* mask);
  void NoteMaskCopied() { present_source_->Pipeline().NoteMaskCopied(); }
  // "Retry now". Never after a removal or a teardown.
  // The device was found removed (a bridged context's is the private device): Retry now's cue.
  [[nodiscard]] bool DeviceLost() const { return device_lost_; }
  void RetryNow() {
    if (torn_down_ || device_lost_) return;  // never back into the snippet after a removal
    session_->RetryNow();
  }

  [[nodiscard]] ContextStatus Status() const;
  // NGX could not create NR's feature on this context's device (the Session's message, with NGX's result), until a load works again; empty otherwise.
  // A Vulkan bridge's context: the Vulkan route chain goes on to the helper. Never for an out-of-memory result, which the Session's own retry
  // and budget handle (another device would most likely be short of memory too).
  [[nodiscard]] std::string CreateFailure() const;
  [[nodiscard]] ID3D12CommandQueue* PresentQueue() const { return present_queue_; }
  // The one bool NoteLatchIfTripped needs on every present, evaluate and
  // teardown, without building a full Status() (string formatting included) just to read it.
  [[nodiscard]] bool LatchTripped() const { return dlss_latch_tripped_; }
  // BeginFrame readied a Present-path recording that Run has not made yet. The D3D11 bridge starts its fence
  // hand-off only then.
  [[nodiscard]] bool FrameReady() const { return frame_ready_; }
  // The Session's state (OFF once torn down after a removal), for the bridge's surfaces and its mask copy.
  [[nodiscard]] nr::SessionState NrState() const { return (session_ ? session_->State() : nr::SessionState::OFF); }
  // The recordings that applied NR so far, at every placement (each Present-path run, each DLSS evaluate's), and the latest recording's
  // motion source. NR's heartbeat counts NR frames from these, never from presents: a DLSS placement keeps its status while the game's DLSS idles.
  [[nodiscard]] uint64_t NrRecordings() const { return nr_recordings_; }
  [[nodiscard]] sources::MotionSource LatestMotionSource() const { return status_.motion_source; }

 private:
  DeviceContext(ID3D12Device* device, nr::SnippetConfig snippet_config, std::unique_ptr<nr::Timeline> timeline,
               ContextOptions options);
  // True, with the context latched off, when the device was removed. BeginFrame and
  // OnDlssEvaluate both check, so no evaluate between a removal and the next present reaches the snippet.
  bool NoteDeviceRemoved(std::chrono::steady_clock::time_point now);
  // The per-frame Session work that both BeginFrame and OnDlssEvaluate's
  // present-starvation catch-up need -- the idle recompute, SetEnabled, Tick and the release on
  // OFF/FAILED -- so an evaluate that drives the Timeline while presents starve reloads NR exactly
  // as a present would, instead of finding a stale placement and an unticked Session.
  void UpdateSession(std::chrono::steady_clock::time_point now);
  // Split chain: the frame's chain from pass 1's placement, the stored "Runs at" and why each stage cannot run now, right after
  // ChoosePlacement (BeginFrame, and OnDlssEvaluate's present-starvation catch-up): logged once when it splits or changes, the Session's groups set, and a
  // set no group uses any more retired.
  void UpdateChain();
  // The settled work layout for an output region of `output` at `stage`. Noted to the Session in every state, so a FAILED
  // session retries when it changes. Split chain: pass 1's group's stage at Setup's Resolution, a later group's at its stage's own.
  sources::WorkLayout SettleLayout(nr::Size output, Stage stage, std::chrono::steady_clock::time_point now);
  // The bookkeeping a placement_ change needs wherever it is decided
  // (BeginFrame, and OnDlssEvaluate's present-starvation catch-up) -- logged once, and the settle
  // starts over so the new placement's work size applies at once. A no-op when the placement did not
  // change.
  void NotePlacementChange();
  // The per-evaluate settings both DLSS placements run with, for stage group `group`'s recording.
  [[nodiscard]] sources::AfterDlssConfig DlssConfig(const sources::WorkLayout& layout, uint32_t group) const;
  // OnDlssEvaluate's opening, shared with DecideAfterEvaluate: the removal check (false, with the status reset, after a removal), the core hold's
  // resume, DLSS seen, the cadence and the present-starvation catch-up.
  bool NoteMainEvaluate(const ngx_hooks::DlssFrame& frame, std::chrono::steady_clock::time_point now);
  void NoteDlssFeature(uint64_t serial);
  // The recordings of both DLSS placements on `host`'s list, with the status they set: shared by the Direct3D 12 paths (OnDlssEvaluate,
  // BeforeDlssEvaluate) and the Direct3D 11 bridge's (RecordBridgedStage). Split chain: After DLSS records stage group `group` (Before upscaling is always
  // group 0); `before_reset`, the game's reset as this evaluate's Before upscaling group already filtered it (ResetMerger passes one per window).
  sources::PipelineResult RecordAfterDlss(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host, const sources::WorkLayout& layout,
                                          std::chrono::steady_clock::time_point now, uint32_t group, std::optional<bool> before_reset);
  sources::PipelineResult RecordBeforeUpscaling(const ngx_hooks::DlssFrame& frame, sources::DlssFrameHost& host, const sources::WorkLayout& layout,
                                                std::chrono::steady_clock::time_point now);
  // PreUpscale is on and the main handle is DLSS-SR, or Ray Reconstruction on this native Direct3D 12 device (PreSrFeatureAllowed; the Direct3D 11 bridge
  // keeps DLSS-SR only).
  [[nodiscard]] bool BeforeUpscaling() const;
  // A stage group's recording: its StageState, and for pass 1's group the status and NR's heartbeat.
  void NoteResult(Stage stage, uint32_t group, const sources::PipelineResult& result);
  // FrameSettings::look for the recording about to run, with Δt since this context's last recording (between main-handle evaluates on
  // the DLSS placements, between presents on the Present path). Split chain: of stage group `group`, with Result shaping on the last
  // group only and Keep faces where it protects that group.
  void ApplyLookConfig(std::chrono::steady_clock::time_point now, uint32_t group);
  std::array<std::optional<std::chrono::steady_clock::time_point>, STAGE_COUNT> last_look_recordings_;  // by stage group

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  std::unique_ptr<nr::Host> host_;
  std::unique_ptr<nr::Timeline> timeline_;
  std::unique_ptr<nr::Session> session_;
  std::unique_ptr<sources::PresentSource> present_source_;
  std::unique_ptr<sources::AfterDlssSource> after_dlss_;  // shares present_source_'s NrPipeline
  std::unique_ptr<sources::PreSrSource> pre_sr_;          // after after_dlss_; shares present_source_'s NrPipeline
  // Split chain: what each stage (Stage order) keeps of its own: its settled work size, and its latest recording's result (Status's groups).
  using StageState = StageEntry<>;
  StageBook<> stages_;
  sources::WorkLayout present_layout_;
  nr::Size work_size_;  // pass 1's group's, as SettleLayout last settled it
  nr::Size canvas_size_;
  NVSDK_NGX_Feature main_feature_ = NVSDK_NGX_Feature_Reserved_Unknown;
  uint64_t dlss_feature_serial_ = 0u;
  FrameTrigger trigger_;
  ngx_hooks::FrameGenDetector frame_generation_;
  ngx_hooks::ResetMerger reset_merger_;
  ngx_hooks::MotionJitterTracker present_jitter_;
  ID3D12CommandQueue* present_queue_ = nullptr;
  FrameSettings settings_;
  FrameFacts facts_;
  PlacementChoice placement_;
  // Split chain: UpdateChain's; groups[0] is pass 1's, at `placement_`. `slices_` are the Session's, per group: the last runs to the end of
  // the passes, so a one-group chain is exactly the single chain of before (every pass, every surplus idle feature too).
  ChainPlan chain_;
  std::array<nr::StageSlice, STAGE_COUNT> slices_ = {};
  std::string present_cannot_;        // why a later group cannot run at Present: the latest BeginFrame's back buffer; empty: it can
  uint64_t dlss_stage_token_ = 0u;    // the newest DLSS-stage recording's completion token in a split: a later Present group waits for it to complete
  uint64_t last_dlss_token_ = 0u;     // the newest DLSS-stage recording's token, whatever the chain (marked when the chain splits)
  // A queue other than the current presents' that the game's DLSS-stage NR went to; while set, a later group never runs at Present.
  ID3D12CommandQueue* other_dlss_queue_ = nullptr;
  bool present_reset_owed_ = false;   // a later Present group skipped a frame: its next recording restarts its history
  std::optional<bool> before_reset_;  // this evaluate's Before upscaling group's filtered reset, for the After DLSS group behind it
  std::string blocked_reason_;
  std::string dlss_unavailable_reason_;
  std::string output_message_;  // device removal, unsupported format or colour space
  sources::PresentTarget frame_target_;
  nr::Controls controls_;
  std::optional<color::Encoding> encoding_;
  std::optional<ngx_hooks::CreateSnapshot> main_snapshot_;
  std::optional<std::chrono::steady_clock::time_point> last_present_;
  std::optional<std::chrono::steady_clock::time_point> last_main_evaluate_;
  std::optional<std::chrono::steady_clock::time_point> last_dlss_diagnostic_;
  uint64_t main_evaluates_ = 0u;
  std::optional<std::chrono::steady_clock::time_point> last_game_list_recording_;
  nr::Size frame_size_;
  float diffuse_white_nits_ = 0.f;
  TriggerPoint trigger_used_ = TriggerPoint::NONE;
  TriggerPoint logged_trigger_ = TriggerPoint::NONE;
  std::optional<Placement> logged_placement_;  // nullopt logs the first placement, NONE included
  nr::SessionState logged_state_ = nr::SessionState::OFF;
  uint32_t trace_frames_ = 0u;
  RecordingStatus status_;  // pass 1's group's latest recording; each group's own is in stages_
  uint64_t nr_recordings_ = 0u;  // recordings that applied NR, for NrRecordings
  // Set on every evaluate (After DLSS, Before upscaling and Source = Present's copy attempt alike):
  // whether the game passed motion vectors this evaluate, for the overlay's "the game passed no motion
  // vectors" reason. Stale between evaluates is harmless: it is read only while the status's motion source is NONE.
  bool last_motion_vectors_missing_ = false;
  bool frame_ready_ = false;
  // 1.2.4: a Present-path Run since the last BeginFrame. NR can run before the present itself, when an add-on renders ReShade's effects mid-frame
  // (S.T.A.L.K.E.R. Anomaly's, before the HUD): BeginFrame then keeps that frame's result for the card instead of clearing it.
  bool ran_since_present_ = false;
  std::atomic<bool> device_lost_{false};  // also read by StampThread on game threads
  std::atomic<uint64_t> later_call_stamps_{0u};  // tokens StampThread stamped for a later ExecuteCommandLists call
  std::atomic<uint64_t> dropped_tokens_{0u};     // tokens DropTokens completed: recorded on a list, never seen submitted
  bool torn_down_ = false;
  bool dlss_seen_ = false;
  // The game switched its DLSS off here (FrameFacts::dlss_released held long enough, DLSS seen, the DLSS placements available, not held):
  // the DLSS stages run at Present, and Match game at Full, until the next main evaluate shows DLSS on again (as on first sight). The core-shutdown hold keeps
  // priority while it lasts. `dlss_off_logged_`: a stage really fell back, so the off line was logged and the on line is owed.
  DlssOffLatch dlss_off_;
  bool dlss_off_logged_ = false;
  bool dlss_idle_ = false;
  bool dlss_paused_ = false;  // no evaluate for DLSS_IDLE_MINIMUM, at any placement (Status's DLSS_PAUSED)
  bool dlss_latch_tripped_ = false;
  bool logged_after_dlss_ = false;  // "NR trigger: after DLSS" was logged in this activation
  // A submit-only thread's tokens age out only after Timeline's own
  // AGED_SUBMISSION_TIME, so the descriptor ring can stay fully busy for a while by design on that
  // engine shape. Tracks how long nr::SkipReason::DESCRIPTORS_BUSY has been the After-DLSS result, back to back, so
  // one WARN per context tells the difference from a genuinely undersized ring.
  std::optional<std::chrono::steady_clock::time_point> descriptors_busy_since_;
  bool warned_descriptors_busy_ = false;
  uint64_t settings_generation_ = 0u;  // the last FrameSettings::settings_generation seen
  bool bridged_ = false;               // ContextOptions::bridged
  CoreShutdownHold core_hold_;         // the game shut NGX down on this device; NR waits for its DLSS (OnCoreShutdown)
  bool dlss_stages_ = false;           // ContextOptions::dlss_stages
  std::string after_dlss_problem_;     // NoteStageProblem's; empty while the stage can run
  std::string before_upscaling_problem_;
  // The layout the latest Decide*Evaluate settled, for RecordBridgedStage. Split chain: per DLSS stage ([0] Before upscaling, [1] After
  // DLSS), as both hand-offs run in one evaluate.
  std::array<sources::WorkLayout, 2> bridged_layouts_;
  // NoteGameDeviceRemoved's text, kept apart from output_message_ (which every BeginFrame rebuilds), so the card and the latch
  // marker keep it. Status prefers it as the output problem.
  std::string game_device_removed_;
};

}  // namespace uplift::addon
