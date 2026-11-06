#pragma once

#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addon/device_context.hpp"
#include "addon/placement.hpp"
#include "addon/recording_status.hpp"
#include "addon/stage_math.hpp"
#include "addon/vk_route.hpp"
#include "addon/vk_stage_work.hpp"
#include "ngx_hooks/dlss_capture.hpp"
#include "nr/host.hpp"
#include "nr/session.hpp"
#include "nr/snippet.hpp"
#include "nr/vk_host.hpp"
#include "sources/vk_nr_pipeline.hpp"
#include "sources/work_size.hpp"
#include "vk/command_ring.hpp"
#include "vk/frame.hpp"
#include "vk/game_host.hpp"
#include "vk/nr_functions.hpp"
#include "vk/nr_timeline.hpp"

namespace uplift::addon {

// One Vulkan command buffer as native NR sees it. The add-on keeps one per VkCommandBuffer (command_list_tracker's map,
// filled at init_command_list), so the NGX detour finds the buffer it was handed. ReShade-free: the device is opaque here.
struct VkListState {
  const void* device = nullptr;  // the ReShade device the buffer was allocated on
  std::vector<uint64_t> tokens;  // every Uplift token recorded since the last reset, in recording order: their events belong to this recording
  uint64_t first_token = 0u;     // the first of them; 0 = none
  uint64_t open_token = 0u;      // issued in the hooked evaluate in progress; its event is set at that evaluate's end (EndEvaluate)
  bool executed = false;         // submitted at least once since the last reset
};

// What the Vulkan DLSS placements' readout, Details line and README say about the one-time cost.
inline constexpr std::string_view VK_FIRST_USE_NOTE = "after the first use, about 0.6 GB stays with the game's device until the game exits";
// How long the game's NGX shutdown waits for NR's last recordings to finish on the GPU before Uplift releases NR (else abandons it).
inline constexpr std::chrono::milliseconds VK_CORE_SHUTDOWN_WAIT{2000};

struct VkContextSetup {
  nr::SnippetConfig snippet;                      // initialize_core_for_device is Create's (off, the game set its core up)
  nr::VulkanBinding binding;                      // the instance, the physical device, the game's device and the loader's exports
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;  // the DXGI adapter with the device's LUID: the budget
  vk::QueueSignal signal;                         // the frame semaphore's signal at present (the add-on: ReShade's command_queue::signal)
  std::unique_ptr<nr::Host> host;                 // tests: an NR stand-in; null runs NVIDIA's runtime (nr::VkHost)
  // The family of ReShade's effect queue (DeviceRecord::graphics_family), where native NR at Present's own command buffers are allocated;
  // UINT32_MAX: unknown, and NR at Present cannot run natively here (CanRunPresent).
  uint32_t queue_family = UINT32_MAX;
  // Called right before the Session loads NR for native Present (the add-on writes its crash marker there), on the present thread.
  std::function<void()> before_present_load;
};

// The swap-chain image native NR at Present runs on, as the add-on describes it at the present event (ReShade's description: a mutable-format swap
// chain's image reads typeless), and the frame's trigger facts. A null image: none this frame.
struct VkPresentFrame {
  vk::ImageInfo back_buffer;
  color::ColorSpace color_space = color::ColorSpace::UNKNOWN;
  bool marker_expected = false;  // effects are on and an enabled Uplift technique exists (FrameTrigger::OnPresent)
};

// What one native Present recording needs from the add-on's queues, kept ReShade-free here.
struct VkPresentQueues {
  VkQueue queue = VK_NULL_HANDLE;  // the effect runtime's queue (native), where Uplift's command buffers are submitted
  // Submits ReShade's immediate command list of that queue (the order is the game's frame, ReShade's work so far, then Uplift's NR).
  std::function<void()> flush_effects;
  // The game presents from another queue: flushes that queue and has `fence` signalled there (the CPU waits for it before NR); empty otherwise.
  std::function<bool(VkFence)> flush_present_queue;
  std::span<const VkSemaphore> waits{};
};

// This frame's UPLIFT_MV for native NR at Present (the Uplift technique's event, while Launchpad runs): a sampled view in SHADER_READ_ONLY_OPTIMAL.
struct VkLaunchpadMotion {
  VkImageView view = VK_NULL_HANDLE;
  nr::Size size;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

// NR after DLSS and before upscaling natively on a Vulkan game's device: nr::VkHost, nr::Session, the Vulkan Timeline
// (vk::NrCompletion) and sources::VkNrPipeline. DeviceContext reduced to the DLSS placements; ReShade-free. Not thread-safe: the add-on serialises every call
// under its lock.
//
// On Vulkan, Source = Auto stays at Present (the bridge's); a DLSS placement runs only when Source is DLSS (the overlay's
// After DLSS and Before upscaling picks on a Vulkan device), on Present until the game's DLSS is seen.
//
// Present runs here too when the add-on chooses the native route (FrameFacts::vk_native_present): the swap-chain image, at the trigger point
// FrameTrigger picks (the present event, ReShade's begin-effects event, or the Uplift technique), in a command buffer of Uplift's own
// (vk::CommandRing) submitted to the effect queue, never in ReShade's immediate command list.
class VkDlssContext {
 public:
  // Device loss this soon after a native recording latches its route off.
  static constexpr std::chrono::seconds DLSS_LATCH_WINDOW{10};
  // The CPU wait for the game's present queue before native NR at Present (the bridge's WAIT_CAP_MS); a timeout skips the frame.
  static constexpr std::chrono::milliseconds PRESENT_QUEUE_WAIT{2000};
  // This many present-queue timeouts in a row stop NR at Present here for good, as the bridge's timeout rule does.
  static constexpr uint32_t PRESENT_QUEUE_TIMEOUT_LIMIT = 3u;

  // nullptr with `error` when the device lacks a function native NR needs, cannot write NR's images as storage images, or a Vulkan object could not
  // be made. The snippet's initialize_core_for_device follows FrameFacts::vk_initialize_core at each present.
  static std::unique_ptr<VkDlssContext> Create(VkContextSetup setup, std::string* error);
  ~VkDlssContext();  // Abandon() unless torn down: a destructor never calls into Vulkan or NGX
  VkDlssContext(const VkDlssContext&) = delete;
  VkDlssContext& operator=(const VkDlssContext&) = delete;

  // Non-empty keeps NR off with this message (a conflicting host, a missing runtime, another NR producer).
  void SetBlockedReason(std::string reason);
  // Why the DLSS placements cannot run this session (the latch included); empty when they can.
  void SetDlssUnavailableReason(std::string reason);
  // A standalone frame advances completion and prepares the current placement and session on the supplied queue.
  // At a ReShade inner present this is the effect-runtime queue, whose lock ReShade holds.
  // nr_allowed grants the context NR ownership. present_holds_nr means its Present path holds the claim during a handover.
  // For native Present, the image supplies the format, encoding and work size. The returned trigger is NONE when it cannot record.
  TriggerPoint BeginFrame(void* present_queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool present_holds_nr, std::chrono::steady_clock::time_point now,
                          const VkPresentFrame& present = {});
  // A verified outer records its tick before preparing the native frame or the bridge's native sidecar.
  TriggerPoint BeginOuterFrame(bool& outer_ticked, void* queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool holds_nr,
                               std::chrono::steady_clock::time_point now, const VkPresentFrame& present = {});
  // The inner present supplies any missing tick. A handoff delegated to the outer returns NONE here.
  TriggerPoint BeginInnerFrame(bool outer_owns_frame, bool outer_ticked, void* queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool holds_nr,
                               std::chrono::steady_clock::time_point now, const VkPresentFrame& present);
  // The effect events' trigger points for native NR at Present.
  TriggerPoint OnBeginEffects(bool usable) { return trigger_.OnBeginEffects(usable); }
  TriggerPoint OnTechnique(bool is_marker) { return trigger_.OnTechnique(is_marker); }
  void EndFrame() { trigger_.EndFrame(); }
  // BeginFrame readied a native Present recording that RunPresent has not made yet.
  [[nodiscard]] bool FrameReady() const { return frame_ready_; }
  // Native NR at Present at `point`, once per frame: the second-queue CPU wait (PRESENT_QUEUE_WAIT; a timeout skips the frame), a free slot of Uplift's own
  // command buffers (a busy one skips the frame, counted, never waited for), VkNrPipeline::RecordPresent on the swap-chain image in `usage` (PRESENT at the
  // present event, RENDER_TARGET inside the effects, as the bridge passes it), with DLSS's Present copy, else `launchpad`, else no vectors; then ReShade's
  // immediate list flushed and Uplift's buffer submitted to the effect queue with its fence. No token: the effect queue's next frame signal follows it.
  sources::PipelineResult RunPresent(TriggerPoint point, vk::Usage usage, const VkPresentQueues& queues, const VkLaunchpadMotion& launchpad,
                                     std::chrono::steady_clock::time_point now);
  // Uplift's own command buffers exist on this device (the effect queue's family was known), so NR at Present can run here.
  // The ring is released while NR runs at a DLSS stage and made again for Present, so this asks whether it can be.
  [[nodiscard]] bool CanRunPresent() const { return queue_family_ != UINT32_MAX && !ring_failed_; }
  // Why native NR at Present cannot run on this device, kept once seen: NGX could not create NR's feature here (CREATE_FAILURE, the Session's message
  // with NGX's result; only a failure of NR at Present, as a DLSS placement's keeps the Session's own retry, and never an out-of-memory result), else
  // Uplift's own command buffers cannot run at Present (PRESENT_BUFFERS, !CanRunPresent: why). nullopt: neither.
  [[nodiscard]] std::optional<VkNativeBlock> PresentBlock() const;
  // Why NR at Present stopped here for good, so the route goes on along the chain: PRESENT_QUEUE_TIMEOUT_LIMIT consecutive second-queue timeouts, or the game's NGX
  // shutdown while NR ran at Present (the context then drops its claim). nullopt otherwise.
  [[nodiscard]] const std::optional<std::string>& PresentStopped() const { return present_stopped_; }
  // Device loss within DLSS_LATCH_WINDOW of a native Present submission disables native NR from the next start.
  [[nodiscard]] bool PresentLatchTripped() const { return present_latch_tripped_; }
  // The game's device was lost: no route starts on it any more.
  [[nodiscard]] bool DeviceLost() const { return device_lost_; }
  // NR was dropped here (a loss, a switch or a shutdown whose GPU work did not finish, an abandon): its runtime may still be
  // mapped, so no route ever starts on the device, and WantsNr stays true so the claim never goes to another one.
  [[nodiscard]] bool Abandoned() const { return dropped_; }
  // The game's vkQueueSubmit chain, for the add-on's flush of a second present queue (ReshadeVkHost).
  [[nodiscard]] PFN_vkQueueSubmit QueueSubmit() const { return functions_.vkQueueSubmit; }
  // After the hooked evaluate of the main handle on `buffer` (whose state is `list`): NR after DLSS when that is the placement. The token is issued
  // before recording and left open on `list`; the caller calls EndEvaluate on every path (an exception included). `frame` points at `copies`.
  sources::PipelineResult OnDlssEvaluate(VkCommandBuffer buffer, const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies,
                                         VkListState& list, std::chrono::steady_clock::time_point now);
  // Before the hooked evaluate (Before upscaling): NR on the region of the game's Color, padded into the floor canvas, right before DLSS reads it. With NR
  // applied it returns the pipeline's private colour, which the NGX detour swaps in as DLSS's Color for this one call (the game's own pointer goes back
  // after); null when nothing changed (DLSS reads the game's Color). The token is issued before recording and left open on `list`, like OnDlssEvaluate's:
  // EndEvaluate sets its event after DLSS has read the private colour, on every path.
  const NVSDK_NGX_Resource_VK* BeforeDlssEvaluate(VkCommandBuffer buffer, const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies,
                                                  VkListState& list, std::chrono::steady_clock::time_point now);
  // The detour could not swap the private colour in (the game's block had no Color): DLSS read its own, so the status must not say NR applied.
  void OnColorSwapRejected();
  // Uplift's copy of UPLIFT_MASK on the game's device (VkNrPipeline::PrepareMaskCopy), which the add-on copies the effect's texture into
  // at the end of ReShade's effects. VK_NULL_HANDLE, with the copy released, unless NR is ACTIVE at a DLSS placement with Mask = Auto and `format` is one the
  // decode samples. `*first`: the image's layout is still UNDEFINED. NoteMaskCopied after the copy is recorded; ReleaseMask when the effect stops.
  VkImage PrepareMaskCopy(VkFormat format, nr::Size size, bool* first);
  void NoteMaskCopied();
  void ReleaseMask();
  // The end of the hooked evaluate. vkCmdSetEvent(ALL_COMMANDS) for the token this call opened on `list`, if one is open.
  void EndEvaluate(VkCommandBuffer buffer, VkListState& list);
  // The command buffer's events (the add-on's lock): its first submission, a submission again without a reset (`tokens` in recording order), and its
  // reset or destroy (also a stale VkListState replaced at init_command_list). Split chain: `queue` is where the buffer
  // goes, which decides whether a later Present group may run beside its DLSS-stage NR (no GPU waits).
  void Executed(std::span<const uint64_t> tokens, void* queue);
  void Reexecuted(std::span<const uint64_t> tokens, uint64_t first_token, void* queue);
  void Recycle(std::span<const uint64_t> tokens);
  // The game is about to shut the NGX core down for this device (NVSDK_NGX_VULKAN_Shutdown1, Streamline's slShutdown), which
  // comes before vkDestroyDevice. With NR not OFF: a wait of at most `cap` for NR's submitted recordings (their events only), then the NGX half of
  // Teardown (the features' release, DestroyParameters, the snippet's Shutdown1, the unload), so every NGX call of Uplift's precedes the core's;
  // when the wait runs out, NR is abandoned instead (no NGX call). Either way NR never loads on this device again, and Teardown then frees only
  // Uplift's own Vulkan objects. The Present path's copy slots and the mask copy are never freed here (with NR OFF nothing is waited for
  // or freed at all): ReShade's queue may still read or write them after the game's last command buffer, which no token covers, and their marks carry a
  // present whose frame signal only a BeginFrame would send. They go at Teardown, after the game's own idle. Idempotent.
  void OnCoreShutdown(std::chrono::milliseconds cap);
  // The vkDestroyDevice detour: the game idled the device, so every token is complete (NrCompletion::DeviceIdle, first), then the
  // Session's flush and unload (unless OnCoreShutdown already ran them), the surfaces and pipelines, and the events and the semaphore. After a
  // device loss it abandons instead. Idempotent.
  void Teardown();
  // destroy_device without a teardown, or a device loss: no NGX call and no Vulkan call; everything NR holds is left to the device's end.
  void Abandon();
  // "Retry now". Never after a loss or a teardown.
  void RetryNow() {
    if (torn_down_ || dropped_) return;
    session_->RetryNow();
  }

  [[nodiscard]] ContextStatus Status() const;
  [[nodiscard]] nr::SessionState NrState() const { return (session_ ? session_->State() : nr::SessionState::OFF); }
  // Split chain: the current chain (BeginFrame's) has a group at `stage`. With pass 1 at a DLSS stage, a group at Present makes the add-on ask for
  // the Present motion copies (PresentMotionGates::native_present_group).
  [[nodiscard]] bool RunsAt(Stage stage) const { return GroupAt(chain_, stage).has_value(); }
  // Split chain: the recordings that applied NR at each stage (Stage order) so far: the transitions stress test's per-group counts.
  [[nodiscard]] const std::array<uint64_t, STAGE_COUNT>& StageRecordings() const { return stage_recordings_; }
  [[nodiscard]] bool DlssSeen() const { return dlss_seen_; }
  // The observed main feature, including SR after RR; empty until an evaluate reached this context.
  [[nodiscard]] std::optional<NVSDK_NGX_Feature> ObservedDlssFeature() const { return dlss_seen_ ? std::optional{main_feature_} : std::nullopt; }
  // The placement is a DLSS one (DLSS seen, Source = DLSS, no reason against): NrClaim's "dlss_seen" for this context's identity. After the game's
  // NGX shutdown it still is, so the Present path never loads NR into a game that is shutting down; Setup then greys Present too, with
  // ui::VULKAN_NGX_SHUT_DOWN_PRESENT_REASON.
  [[nodiscard]] bool AtDlssPlacement() const;
  // Whether this present's BeginFrame, given `source`, will put the context at a DLSS stage (the choice BeginFrame makes, read before it),
  // so the owner moves in the same frame as the stage. False once the context stopped, was dropped or torn down.
  [[nodiscard]] bool WouldRunDlssStage(ui::PlacementSource source) const;
  // AtDlssPlacement, or NR at Present runs natively here (FrameFacts::vk_native_present), with the same rule after the game's NGX shutdown.
  [[nodiscard]] bool WantsNr() const;
  [[nodiscard]] bool LatchTripped() const { return dlss_latch_tripped_; }
  // "Vulkan: NR runs inside the game's frame (native Vulkan NR, 412 MiB; after the first use, about 0.6 GB stays ...)"; at Present: "Vulkan: NR
  // runs natively on the game's Vulkan device (...)"; empty while NR neither runs here nor is wanted.
  [[nodiscard]] std::string DetailsLine() const;
  // The copy of DLSS's motion vectors made in the game's frame for the present now running, in SHADER_READ_ONLY_OPTIMAL, or an empty
  // image (none was made: the Present path's recording takes UPLIFT_MV, or none; always empty once CanCopy is false). Reading it stamps the slot's last use
  // (the bridge's copy-in follows). Flicker fix: a present whose own copy was skipped (the GPU ran behind) gets the newest copy at most
  // sources::PRESENT_MOTION_MAX_AGE frames older, so the motion stays DLSS's; empty only with none that recent.
  [[nodiscard]] vk::NrImage PresentMotion();
  // Why that copy is missing, for Setup: GAME_PASSED_NONE (the latest evaluate seen carried no readable float vectors; kept while the copies are not asked
  // for), DLSS_PAUSED (DLSS was seen, the present has none, and no evaluate came for 250 ms: a menu or a loading screen), NO_COPY (an evaluate came but no copy
  // within sources::PRESENT_MOTION_MAX_AGE frames exists: a slot or the constants ring stayed busy), else NONE. NONE while the add-on is not asking for the copies (GAME_PASSED_NONE aside) and once CanCopy is false.
  [[nodiscard]] ui::MotionGap PresentMotionGap() const;
  // Whether the copies can be made at all: not after the game's NGX shutdown (OnCoreShutdown), a device loss or a teardown. The add-on asks for them only while true,
  // so the hooked evaluate is a pure passthrough again, and Setup greys the DLSS stages and DLSS's vectors with their true cause.
  [[nodiscard]] bool CanCopy() const { return !torn_down_ && !dropped_ && !core_shut_down_; }
  // The bytes the copies hold on the game's device (four slots, or eight once the ring grew); 0 while there are none.
  [[nodiscard]] uint64_t HeldBytes() const { return (pipeline_ ? pipeline_->HeldBytes() : 0u); }

 private:
  VkDlssContext(vk::NrFunctions functions, VkDevice device, std::array<bool, color::VkColorPipeline::OUTPUT_VARIANTS> storage_formats);
  bool BeginPresent(bool advance, void* queue, const FrameFacts& facts, std::chrono::steady_clock::time_point now);
  TriggerPoint PrepareFrame(void* queue, const FrameSettings& settings, const FrameFacts& facts, bool nr_allowed, bool holds_nr, std::chrono::steady_clock::time_point now,
                            const VkPresentFrame& present);
  void NoteDlssFeature(uint64_t serial);
  // The settled Session work both BeginFrame and the idle recompute need: SetEnabled, Tick, the release on OFF or FAILED.
  void UpdateSession(std::chrono::steady_clock::time_point now);
  void NotePlacementChange();
  void SetOutputProblem(std::string problem);
  // NR at Present runs natively here this frame (the placement is Present and the add-on chose the native route). Split chain: pass 1's group.
  [[nodiscard]] bool NativePresent() const { return placement_.placement == Placement::PRESENT && facts_.vk_native_present; }
  // Split chain: a group of the chain runs at Present natively here this frame: pass 1's (NativePresent), or a later one behind a DLSS stage.
  [[nodiscard]] bool PresentGroupNative() const { return facts_.vk_native_present && GroupAt(chain_, Stage::PRESENT).has_value(); }
  // This present's frame for native NR at Present: why it cannot take NR (the copy access, the format, a blit the driver lacks), else its encoding.
  // Split chain: read for pass 1 at Present, and for a later group only while some pass is stored at another stage (its "cannot run" fact).
  void ResolvePresentFrame();
  void SetPresentProblem(std::string problem);
  // `copy_format` and RGBA16F can be blitted between on this GPU (optimal tiling), asked of the driver once per format.
  bool Blittable(VkFormat copy_format);
  // A native Present frame that goes without NR, with `reason`. Split chain: the Present group `group`'s; a later group owes a reset.
  sources::PipelineResult SkipPresent(nr::SkipReason reason, uint32_t group = 0u);
  // Whether a recording may hand NGX `shape` as its motion input (VkMotionCheck). NONE: yes; otherwise the skip reason,
  // and after VK_MOTION_RELOAD_RECORDINGS of them the next present reloads the Session from OFF. Split chain: per stage group (`stage`'s).
  nr::SkipReason CheckMotion(Stage stage, const VkMotionShape& shape);
  // Split chain: the frame's chain from pass 1's placement, the stored "Runs at" and why each stage cannot run now (Present
  // whose route fell back off Native), right after the placement (BeginFrame): logged once when it splits or changes, the Session's groups set every frame,
  // and a set no group uses any more retired.
  void UpdateChain();
  // The command buffers the chain records into (a VkStreamBit per group: Present natively in Uplift's own, the DLSS stages in the game's).
  [[nodiscard]] VkStreamSet ChainStreams() const;
  // Split chain, no GPU waits: whether every earlier DLSS-stage recording of a split that went to another queue than the
  // effect queue has completed (VkStageWork::Done, a CPU check, never a wait). A later Present group skips its frame otherwise.
  [[nodiscard]] bool StageWorkDone() const;
  // A stage group's latest recording, for Status's groups, the card's pass count and StageRecordings, and for pass 1's group the status (`scale`: where
  // its motion scale comes from).
  void NoteResult(Stage stage, uint32_t group, const sources::PipelineResult& result, RecordingStatus::Scale scale);
  // What both DLSS placements take from one evaluate: the target (`resource`, `region`, the resolved encoding, the strengths, the game's exposure) and NR's inputs
  // with the game's motion vectors in place.
  struct ReadFrame {
    sources::VkDlssTarget target;
    nr::FrameInputs inputs;
  };
  [[nodiscard]] ReadFrame ReadDlssFrame(const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies, const NVSDK_NGX_Resource_VK* resource,
                                        nr::Rect region, bool reset_hint) const;
  // The token of a recording, issued before it and kept open on `list` until EndEvaluate. True when this call opened it (a token an earlier
  // recording of the same hooked call opened is reused).
  bool OpenToken(VkListState& list);
  // A recording that put nothing on the buffer: its token goes back unsubmitted.
  void DropOpenToken(VkListState& list);
  // The size the Session is told each frame for pass 1's group: the canvas for Before upscaling, DLSS's output region otherwise.
  [[nodiscard]] nr::Size SessionFrameSize() const;
  // The look stage, the colour fixes and passes 2..10 for the recording about to be made, with the frame time since the last one (the stabiliser's).
  // Split chain: of stage group `group`, with Result shaping on the last group that runs only and Keep faces where it protects that group.
  void ApplyLookConfig(std::chrono::steady_clock::time_point now, uint32_t group);
  // As DeviceContext::SettleLayout: the settled work image (and Before upscaling's floor canvas) for `output` at `stage`; the Session hears
  // the canvas. Split chain: pass 1's group at Setup's Resolution, a later group at its stage's own.
  sources::WorkLayout SettleLayout(nr::Size output, Stage stage, std::chrono::steady_clock::time_point now);
  // Apply DLSS_LATCH_WINDOW to the last recording, update the device-loss message, then Drop(true).
  void NoteDeviceLost(std::chrono::steady_clock::time_point now);
  // Device loss (`device_lost`: Session::OnDeviceLost) or abandon (the runtime's patches restored, no NGX call): the Session, the host and the
  // pipeline are leaked, and the completion abandoned (no event or semaphore destroyed). Idempotent.
  void Drop(bool device_lost);
  [[nodiscard]] bool BeforeUpscaling() const {
    return settings_.pre_upscale && PreSrFeatureAllowed(main_feature_, ngx_hooks::NgxApi::VULKAN);
  }

  vk::NrFunctions functions_;
  VkDevice device_;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;  // the blit format checks
  std::array<bool, color::VkColorPipeline::OUTPUT_VARIANTS> storage_formats_;  // each output variant's format is a storage format here
  std::unique_ptr<vk::NrCompletion> completion_;
  std::unique_ptr<vk::CommandRing> ring_;  // native Present's own command buffers; null when the effect queue's family is unknown
  std::unique_ptr<nr::Host> host_;
  nr::VkHost* vk_host_ = nullptr;  // host_ as NVIDIA's runtime (null with a test's stand-in), for the per-present core initialisation
  std::unique_ptr<nr::Session> session_;
  std::unique_ptr<sources::VkNrPipeline> pipeline_;
  FrameSettings settings_;
  FrameFacts facts_;
  nr::Controls controls_;
  PlacementChoice placement_;
  std::optional<Placement> logged_placement_;
  std::string blocked_reason_;
  std::string dlss_unavailable_reason_;
  std::string output_message_;  // a device loss, or why DLSS's output cannot take NR (format, size, pipelines)
  std::optional<ngx_hooks::CreateSnapshot> main_snapshot_;
  NVSDK_NGX_Feature main_feature_ = NVSDK_NGX_Feature_Reserved_Unknown;
  uint64_t dlss_feature_serial_ = 0u;
  ngx_hooks::ResetMerger reset_merger_;
  ngx_hooks::MotionJitterTracker present_jitter_;
  std::optional<std::chrono::steady_clock::time_point> last_main_evaluate_;
  std::optional<std::chrono::steady_clock::time_point> last_game_list_recording_;
  std::array<std::optional<std::chrono::steady_clock::time_point>, STAGE_COUNT> last_look_recordings_;  // the stabiliser's frame time, by group
  nr::Size output_size_;                          // the latest evaluate's output region
  VkFormat output_format_ = VK_FORMAT_UNDEFINED;  // and its view format
  std::vector<VkFormat> unbuildable_formats_;     // formats whose pipelines could not be built: never tried again
  bool pre_sr_unbuildable_ = false;               // Before upscaling's pipelines could not be built: never tried again
  // Before upscaling: the render region the game's Color holds; both placements: the settled work image keyed on, and the
  // canvas NR runs at (pass 1's group's).
  nr::Size frame_size_;
  nr::Size work_size_;
  nr::Size canvas_size_;
  // Split chain: what each stage (Stage order) keeps of its own: its settled work size, its latest recording's result (Status's
  // groups), its owed reset (a frame that did not apply NR: the next that does starts NR's history afresh), and
  // the motion input its features were first evaluated with in this load and the different ones in a row since.
  struct StageState {
    sources::WorkSizeSettle settle;
    nr::Size work;
    nr::Size canvas;
    bool nr_applied = false;
    uint32_t passes_run = 0u;
    nr::SkipReason skip_reason = nr::SkipReason::NONE;
    sources::MotionSource motion_source = sources::MotionSource::NONE;
    bool reset_owed = false;
    std::optional<VkMotionShape> motion;
    uint32_t motion_mismatches = 0u;
  };
  std::array<StageState, STAGE_COUNT> stages_;
  std::array<uint64_t, STAGE_COUNT> stage_recordings_ = {};  // StageRecordings
  // Split chain: UpdateChain's; groups[0] is pass 1's, at `placement_`. `slices_` are the Session's, per group: the last runs to the end of the
  // passes, so a one-group chain is exactly the single chain of before.
  ChainPlan chain_;
  std::array<nr::StageSlice, STAGE_COUNT> slices_ = {};
  bool chain_passes_moved_ = false;   // this frame's chain moved a pass to another stage while its streams stayed (a reload, as a stream change's)
  std::optional<bool> before_reset_;  // this evaluate's Before upscaling group's filtered reset, for the After DLSS group behind it
  bool present_reset_owed_ = false;   // a later Present group skipped a frame or the DLSS feature changed: the next successful Present recording starts afresh
  // The DLSS-stage recordings of a split and the queues they went to (VkStageWork), the effect
  // queue (BeginFrame's), and the other queue that keeps a later group off Present (logged once per change).
  VkStageWork stage_work_;
  void* effect_queue_ = nullptr;
  const void* other_dlss_queue_ = nullptr;
  nr::SessionState logged_state_ = nr::SessionState::OFF;
  uint32_t trace_frames_ = 0u;
  RecordingStatus status_;  // pass 1's group's latest recording; each group's own is in stages_
  bool last_motion_vectors_missing_ = false;
  bool dlss_seen_ = false;
  // The game switched its DLSS off here (FrameFacts::dlss_released held long enough, DLSS seen, the DLSS placements available): Source =
  // DLSS runs at Present from that present until the next main evaluate shows DLSS on again. Frozen once the context stops (the game's NGX shutdown, a loss,
  // a teardown). `dlss_off_logged_`: the placement really fell back, so the off line was logged and the on line is owed.
  DlssOffLatch dlss_off_;
  bool dlss_off_logged_ = false;
  bool dlss_idle_ = false;
  bool dlss_paused_ = false;  // no evaluate for DLSS_IDLE_MINIMUM, at any placement (PresentMotionGap's DLSS_PAUSED)
  bool dlss_latch_tripped_ = false;
  bool logged_after_dlss_ = false;  // "NR trigger: after DLSS (Vulkan)" was logged in this activation
  bool present_motion_logged_ = false;                      // "DLSS's motion vectors copied for Present" was logged since the add-on last asked for the copies
  ui::MotionGap present_motion_gap_ = ui::MotionGap::NONE;  // the latest evaluate seen: GAME_PASSED_NONE when it had no readable vectors (kept between them)
  // Flicker fix: one DEBUG line a second (LogLevel::TRACE, which the log bridge writes at ReShade's debug level) while the add-on asks for the Present copies
  // (NotePresentMotionSecond; reset when it stops asking, so a line never mixes in an old window): the evaluates and presents, the copies made,
  // the copies skipped by gate, what each present bound, and the GPU lag at the evaluates (CurrentFrame minus the frame the GPU has passed).
  struct PresentMotionCounters {
    uint64_t evaluates = 0u;
    uint64_t presents = 0u;
    uint64_t copies = 0u;
    uint64_t skipped_input = 0u;  // no readable vectors, or the copy could not be made
    uint64_t skipped_ring = 0u;   // the copy pipeline's constants ring slot was busy
    uint64_t skipped_slot = 0u;   // the copy slot's last reader was not complete
    uint64_t own = 0u;            // presents that bound their own frame's copy
    uint64_t older = 0u;          // presents that bound an older copy (PRESENT_MOTION_MAX_AGE)
    uint64_t missed = 0u;         // presents with no copy that recent
    float offset_x = 0.f;
    float offset_y = 0.f;
    uint64_t lag_min = UINT64_MAX;
    uint64_t lag_max = 0u;
    std::optional<std::chrono::steady_clock::time_point> since;
  };
  PresentMotionCounters present_motion_counters_;
  // What the latest present bound, for the per-frame trace (formatted only there).
  enum class BoundMotion : uint8_t { NONE, LAUNCHPAD, OWN_COPY, OLDER_COPY, NO_COPY };
  BoundMotion present_motion_bound_ = BoundMotion::NONE;
  uint64_t present_motion_age_ = 0u;
  void NotePresentMotionSecond(std::chrono::steady_clock::time_point now);
  bool device_lost_ = false;
  bool torn_down_ = false;
  bool dropped_ = false;           // Drop ran: the Session, the host and the pipeline are leaked
  bool core_shut_down_ = false;    // OnCoreShutdown ran: no NGX call on this device any more, and NR never loads here again
  bool present_holds_nr_ = false;  // this frame's claim belongs to the device's Present path
  uint64_t settings_generation_ = 0u;
  // Native NR at Present.
  FrameTrigger trigger_;
  bool frame_ready_ = false;
  VkPresentFrame present_frame_;
  VkFormat copy_format_ = VK_FORMAT_UNDEFINED;       // the swap-chain image's bits are copied raw into this (vk::SharedFormatOf's member)
  std::optional<color::Encoding> present_encoding_;  // resolved for copy_format_'s values; none while the frame cannot take NR
  float present_diffuse_white_nits_ = 0.f;
  std::string present_problem_;  // why this frame cannot take NR at Present
  sources::WorkLayout present_layout_;
  VkFormat blit_checked_format_ = VK_FORMAT_UNDEFINED;
  bool blit_supported_ = false;
  TriggerPoint trigger_used_ = TriggerPoint::NONE;
  TriggerPoint logged_trigger_ = TriggerPoint::NONE;  // "NR trigger: ... (Vulkan, native)" was logged for it in this activation
  uint64_t busy_skips_ = 0u;                          // frames without NR because every one of Uplift's command buffers was still on the GPU
  bool second_queue_ = false;                         // the game presents from another queue than the effect queue
  bool present_failure_logged_ = false;               // a failed begin, submission or present-queue wait was logged
  uint32_t present_queue_timeouts_ = 0u;              // second-queue timeouts in a row
  std::optional<std::string> present_stopped_;         // PresentStopped
  std::optional<std::string> present_create_failure_;  // PresentBlock's CREATE_FAILURE
  std::optional<std::chrono::steady_clock::time_point> last_present_submission_;  // the latest native Present submission
  bool present_latch_tripped_ = false;                                             // PresentLatchTripped
  // The game's NGX shutdown came while NR ran at Present and NR was released cleanly; it becomes PresentStopped once the game has gone on
  // presenting (CoreShutdownStopDue), counted in BeginFrame.
  std::optional<std::chrono::steady_clock::time_point> pending_stop_since_;
  uint32_t pending_stop_presents_ = 0u;
  std::function<void()> before_present_load_;  // VkContextSetup::before_present_load
  // The command buffers the Session's NR was loaded for, and a reload through OFF in progress because the placement moved to
  // other ones (VkStreamNeedsReload). Split chain: the set of them the chain uses; any change of it reloads (VkStreamsNeedReload).
  VkStreamSet session_streams_ = 0u;
  bool stream_reload_ = false;
  bool own_drain_waited_ = false;  // transitions: Uplift's own command buffers were waited for in this drain from Present (once)
  // Why the next present reloads the Session (empty: no reason); the motion each stage's features first saw is in stages_.
  nr::Size dlss_motion_size_;  // the latest readable DLSS motion region (the Present copies' size)
  std::string input_reload_reason_;
  uint32_t queue_family_ = UINT32_MAX;  // the effect queue's family, to make the ring again for Present
  bool ring_failed_ = false;            // the ring could not be made again: NR at Present cannot run here
};

}  // namespace uplift::addon
