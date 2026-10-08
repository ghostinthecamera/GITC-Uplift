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

// Plan 13 (design §3.4): one Vulkan command buffer as native NR sees it. The add-on keeps one per VkCommandBuffer (command_list_tracker's map,
// filled at init_command_list), so the NGX detour finds the buffer it was handed. ReShade-free: the device is opaque here.
struct VkListState {
  const void* device = nullptr;  // the ReShade device the buffer was allocated on
  std::vector<uint64_t> tokens;  // every Uplift token recorded since the last reset, in recording order: their events belong to this recording
  uint64_t first_token = 0u;     // the first of them; 0 = none
  uint64_t open_token = 0u;      // issued in the hooked evaluate in progress; its event is set at that evaluate's end (EndEvaluate)
  bool executed = false;         // submitted at least once since the last reset
};

// The user's decision 1 (2026-10-01): what the Vulkan DLSS placements' readout, Details line and README say about the one-time cost (design §1.2).
inline constexpr std::string_view VK_FIRST_USE_NOTE = "after the first use, about 0.6 GB stays with the game's device until the game exits";
// Batch 3 review I-1: how long the game's NGX shutdown waits for NR's last recordings to finish on the GPU before Uplift releases NR (else abandons it).
inline constexpr std::chrono::milliseconds VK_CORE_SHUTDOWN_WAIT{2000};

struct VkContextSetup {
  nr::SnippetConfig snippet;                      // initialize_core_for_device is Create's (batch 1 review, minor 4: off, the game set its core up)
  nr::VulkanBinding binding;                      // the instance, the physical device, the game's device and the loader's exports
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;  // the DXGI adapter with the device's LUID: the budget
  vk::QueueSignal signal;                         // the frame semaphore's signal at present (the add-on: ReShade's command_queue::signal)
  std::unique_ptr<nr::Host> host;                 // tests: an NR stand-in; null runs NVIDIA's runtime (nr::VkHost)
  // Plan 19: the family of ReShade's effect queue (DeviceRecord::graphics_family), where native NR at Present's own command buffers are allocated;
  // UINT32_MAX: unknown, and NR at Present cannot run natively here (CanRunPresent).
  uint32_t queue_family = UINT32_MAX;
  // T6 fix round (M-1): called right before the Session loads NR for native Present (the add-on writes its crash marker there), on the present thread.
  std::function<void()> before_present_load;
};

// Plan 19: the swap-chain image native NR at Present runs on, as the add-on describes it at the present event (ReShade's description: a mutable-format swap
// chain's image reads typeless), and the frame's trigger facts. A null image: none this frame.
struct VkPresentFrame {
  vk::ImageInfo back_buffer;
  color::ColorSpace color_space = color::ColorSpace::UNKNOWN;
  bool marker_expected = false;  // effects are on and an enabled Uplift technique exists (FrameTrigger::OnPresent)
};

// Plan 19: what one native Present recording needs from the add-on's queues, kept ReShade-free here.
struct VkPresentQueues {
  VkQueue queue = VK_NULL_HANDLE;  // the effect runtime's queue (native), where Uplift's command buffers are submitted
  // Submits ReShade's immediate command list of that queue (hard rule 2: the game's frame, ReShade's work so far, then Uplift's NR).
  std::function<void()> flush_effects;
  // The game presents from another queue: flushes that queue and has `fence` signalled there (the CPU waits for it before NR); empty otherwise.
  std::function<bool(VkFence)> flush_present_queue;
};

// Plan 19: this frame's UPLIFT_MV for native NR at Present (the Uplift technique's event, while Launchpad runs): a sampled view in SHADER_READ_ONLY_OPTIMAL.
struct VkLaunchpadMotion {
  VkImageView view = VK_NULL_HANDLE;
  nr::Size size;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

// Plan 13 (design §5): NR after DLSS and before upscaling natively on a Vulkan game's device: nr::VkHost, nr::Session, the Vulkan Timeline
// (vk::NrCompletion) and sources::VkNrPipeline. DeviceContext reduced to the DLSS placements; ReShade-free. Not thread-safe: the add-on serialises every call
// under its lock.
//
// The user's decision 1: on Vulkan, Source = Auto stays at Present (the bridge's); a DLSS placement runs only when Source is DLSS (the overlay's
// After DLSS and Before upscaling picks on a Vulkan device), on Present until the game's DLSS is seen.
//
// Plan 19: Present runs here too when the add-on chooses the native route (FrameConfig::vk_native_present): the swap-chain image, at the trigger point
// FrameTrigger picks (the present event, ReShade's begin-effects event, the Uplift technique or finish-effects), in a command buffer of Uplift's own
// (vk::CommandRing) submitted to the effect queue, never in ReShade's immediate command list.
class VkDlssContext {
 public:
  // nullptr with `error` when the device lacks a function native NR needs, cannot write NR's images as storage images, or a Vulkan object could not
  // be made. The snippet's initialize_core_for_device follows FrameConfig::vk_initialize_core at each present (Plan 19).
  static std::unique_ptr<VkDlssContext> Create(VkContextSetup setup, std::string* error);
  ~VkDlssContext();  // Abandon() unless torn down: a destructor never calls into Vulkan or NGX
  VkDlssContext(const VkDlssContext&) = delete;
  VkDlssContext& operator=(const VkDlssContext&) = delete;

  // Non-empty keeps NR off with this message (a conflicting host, a missing runtime, another NR producer).
  void SetBlockedReason(std::string reason);
  // Why the DLSS placements cannot run this session (the latch included); empty when they can.
  void SetDlssUnavailableReason(std::string reason);
  // The present event, on the effect runtime's queue (`present_queue`, where ReShade holds that queue's lock): device-lost check, the Timeline's
  // frame, stale tokens, the placement, the Session's settings and Tick, the idle drain, the release of the surfaces on OFF or FAILED, and the
  // output format's pipelines built when NR is about to load. `nr_allowed`: NrClaim lets this context load NR. `present_holds_nr`: the claim is
  // this device's Present path's (the bridge), so a context waiting for it is switching from Present (design R86), not losing NR to another device.
  // Plan 19: with FrameConfig::vk_native_present at Present, `present` is the frame NR runs on (its format, encoding and settled work size, which the
  // Session hears); the trigger point is returned, as DeviceContext::BeginFrame returns it (NONE otherwise).
  TriggerPoint BeginFrame(void* present_queue, const FrameConfig& config, bool nr_allowed, bool present_holds_nr, std::chrono::steady_clock::time_point now,
                          const VkPresentFrame& present = {});
  // Plan 19: the effect events' trigger points for native NR at Present (FrameTrigger's, as DeviceContext's).
  TriggerPoint OnBeginEffects(bool usable) { return trigger_.OnBeginEffects(usable); }
  TriggerPoint OnTechnique(bool is_marker) { return trigger_.OnTechnique(is_marker); }
  TriggerPoint OnFinishEffects() { return trigger_.OnFinishEffects(); }
  // Plan 19: BeginFrame readied a native Present recording that RunPresent has not made yet.
  [[nodiscard]] bool FrameReady() const { return frame_ready_; }
  // Plan 19: native NR at Present at `point`, once per frame: the second-queue CPU wait (2 s; a timeout skips the frame), a free slot of Uplift's own
  // command buffers (a busy one skips the frame, counted, never waited for), VkNrPipeline::RecordPresent on the swap-chain image in `usage` (PRESENT at the
  // present event, RENDER_TARGET inside the effects, as the bridge passes it), with DLSS's Present copy, else `launchpad`, else no vectors; then ReShade's
  // immediate list flushed and Uplift's buffer submitted to the effect queue with its fence. No token: the effect queue's next frame signal follows it.
  sources::PipelineResult RunPresent(TriggerPoint point, vk::Usage usage, const VkPresentQueues& queues, const VkLaunchpadMotion& launchpad,
                                     std::chrono::steady_clock::time_point now);
  // Plan 19: Uplift's own command buffers exist on this device (the effect queue's family was known), so NR at Present can run here.
  // In-game round 1: the ring is released while NR runs at a DLSS stage and made again for Present, so this asks whether it can be.
  [[nodiscard]] bool CanRunPresent() const { return queue_family_ != UINT32_MAX && !ring_failed_; }
  // Plan 19: NGX could not create NR's feature on this device (the Session's message, with NGX's result); empty otherwise. T6 (M-4): only a failure of
  // NR at Present (a DLSS placement's keeps the Session's own retry), and never an out-of-memory result. Kept once seen (the route's fact is sticky anyway).
  [[nodiscard]] std::string CreateFailure() const { return present_create_failure_; }
  // T6 (I-2, I-3): why NR at Present stopped here for good, so the route goes on along the chain: three second-queue timeouts in a row, or the game's NGX
  // shutdown while NR ran at Present (the context then drops its claim). Empty otherwise.
  [[nodiscard]] const std::string& PresentStopped() const { return present_stopped_; }
  // T6 (I-4): the device was lost within 10 s of a native Present submission: the add-on turns native NR off from the next start.
  [[nodiscard]] bool PresentLatchTripped() const { return present_latch_tripped_; }
  // T6 fix round (I-B): the game's device was lost: no route starts on it any more.
  [[nodiscard]] bool DeviceLost() const { return device_lost_; }
  // Review 97f99e3 (Important 1): NR was dropped here (a loss, a switch or a shutdown whose GPU work did not finish, an abandon): its runtime may still be
  // mapped, so no route ever starts on the device, and WantsNr stays true so the claim never goes to another one.
  [[nodiscard]] bool Abandoned() const { return dropped_; }
  // Review 97f99e3 (minor): Uplift's own command buffers could not be made again for Present (their own start error).
  [[nodiscard]] bool RingFailed() const { return ring_failed_; }
  // Plan 19: the game's vkQueueSubmit chain, for the add-on's flush of a second present queue (ReshadeVkHost).
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
  // Plan 14 (design §3.2): Uplift's copy of UPLIFT_MASK on the game's device (VkNrPipeline::PrepareMaskCopy), which the add-on copies the effect's texture into
  // at the end of ReShade's effects. VK_NULL_HANDLE, with the copy released, unless NR is ACTIVE at a DLSS placement with Mask = Auto and `format` is one the
  // decode samples. `*first`: the image's layout is still UNDEFINED. NoteMaskCopied after the copy is recorded; ReleaseMask when the effect stops.
  VkImage PrepareMaskCopy(VkFormat format, nr::Size size, bool* first);
  void NoteMaskCopied();
  void ReleaseMask();
  // Key decision c: the end of the hooked evaluate. vkCmdSetEvent(ALL_COMMANDS) for the token this call opened on `list`, if one is open.
  void EndEvaluate(VkCommandBuffer buffer, VkListState& list);
  // The command buffer's events (the add-on's lock): its first submission, a submission again without a reset (`tokens` in recording order), and its
  // reset or destroy (also a stale VkListState replaced at init_command_list: batch 1 review, minor 3).
  void Executed(std::span<const uint64_t> tokens);
  void Reexecuted(std::span<const uint64_t> tokens, uint64_t first_token, void* queue);
  void Recycle(std::span<const uint64_t> tokens);
  // Batch 3 review I-1: the game is about to shut the NGX core down for this device (NVSDK_NGX_VULKAN_Shutdown1, Streamline's slShutdown), which
  // comes before vkDestroyDevice. With NR not OFF: a wait of at most `cap` for NR's submitted recordings (their events only), then the NGX half of
  // Teardown (the features' release, DestroyParameters, the snippet's Shutdown1, the unload), so every NGX call of Uplift's precedes the core's;
  // when the wait runs out, NR is abandoned instead (no NGX call). Either way NR never loads on this device again, and Teardown then frees only
  // Uplift's own Vulkan objects. Final review C-1: the Present path's copy slots and the mask copy are never freed here (with NR OFF nothing is waited for
  // or freed at all): ReShade's queue may still read or write them after the game's last command buffer, which no token covers, and their marks carry a
  // present whose frame signal only a BeginFrame would send. They go at Teardown, after the game's own idle. Idempotent.
  void OnCoreShutdown(std::chrono::milliseconds cap);
  // The vkDestroyDevice detour (design §3.6): the game idled the device, so every token is complete (NrCompletion::DeviceIdle, first), then the
  // Session's flush and unload (unless OnCoreShutdown already ran them), the surfaces and pipelines, and the events and the semaphore. After a
  // device loss it abandons instead. Idempotent.
  void Teardown();
  // destroy_device without a teardown (R83), or a device loss: no NGX call and no Vulkan call; everything NR holds is left to the device's end.
  void Abandon();
  // Plan 6 (D11): "Retry now". Never after a loss or a teardown.
  void RetryNow() {
    if (torn_down_ || dropped_) return;
    session_->RetryNow();
  }

  [[nodiscard]] ContextStatus Status() const;
  [[nodiscard]] nr::SessionState NrState() const { return (session_ ? session_->State() : nr::SessionState::OFF); }
  [[nodiscard]] bool DlssSeen() const { return dlss_seen_; }
  // The placement is a DLSS one (DLSS seen, Source = DLSS, no reason against): NrClaim's "dlss_seen" for this context's identity. After the game's
  // NGX shutdown it still is (batch 3 review I-1), so the Present path never loads NR into a game that is shutting down; Setup then greys Present too, with
  // ui::VULKAN_NGX_SHUT_DOWN_PRESENT_REASON (Plan 14 final review I-1).
  [[nodiscard]] bool AtDlssPlacement() const;
  // Fix round 1 (I1): whether this present's BeginFrame, given `source`, will put the context at a DLSS stage (the choice BeginFrame makes, read before it),
  // so the owner moves in the same frame as the stage. False once the context stopped, was dropped or torn down.
  [[nodiscard]] bool WouldRunDlssStage(ui::PlacementSource source) const;
  // AtDlssPlacement, or (Plan 19) NR at Present runs natively here (FrameConfig::vk_native_present), with the same rule after the game's NGX shutdown.
  [[nodiscard]] bool WantsNr() const;
  [[nodiscard]] bool LatchTripped() const { return dlss_latch_tripped_; }
  // "Vulkan: NR runs inside the game's frame (native Vulkan NR, 412 MiB; after the first use, about 0.6 GB stays ...)"; Plan 19, at Present: "Vulkan: NR
  // runs natively on the game's Vulkan device (...)"; empty while NR neither runs here nor is wanted.
  [[nodiscard]] std::string DetailsLine() const;
  // Plan 14 (design §2.3): the copy of DLSS's motion vectors made in the game's frame for the present now running, in SHADER_READ_ONLY_OPTIMAL, or an empty
  // image (none was made: the Present path's recording takes UPLIFT_MV, or none; always empty once CanCopy is false). Reading it stamps the slot's last use
  // (the bridge's copy-in follows). Flicker fix: a present whose own copy was skipped (the GPU ran behind) gets the newest copy at most
  // sources::PRESENT_MOTION_MAX_AGE frames older, so the motion stays DLSS's; empty only with none that recent.
  [[nodiscard]] vk::NrImage PresentMotion();
  // Why that copy is missing, for Setup: GAME_PASSED_NONE (the latest evaluate seen carried no readable float vectors; kept while the copies are not asked
  // for), DLSS_PAUSED (DLSS was seen, the present has none, and no evaluate came for 250 ms: a menu or a loading screen), NO_COPY (an evaluate came but no copy
  // within sources::PRESENT_MOTION_MAX_AGE frames exists: a slot or the constants ring stayed busy), else NONE. NONE while the add-on is not asking for the copies (GAME_PASSED_NONE aside) and once CanCopy is false.
  [[nodiscard]] ui::MotionGap PresentMotionGap() const;
  // Whether the copies can be made at all: not after the game's NGX shutdown (OnCoreShutdown), a device loss or a teardown. The add-on asks for them only while true,
  // so the hooked evaluate is a pure passthrough again, and Setup greys the DLSS stages and DLSS's vectors with their true cause (batch 2 review I-1, final
  // review I-1).
  [[nodiscard]] bool CanCopy() const { return !torn_down_ && !dropped_ && !core_shut_down_; }
  // The bytes the copies hold on the game's device (four slots, or eight once the ring grew); 0 while there are none.
  [[nodiscard]] uint64_t HeldBytes() const { return (pipeline_ ? pipeline_->HeldBytes() : 0u); }

 private:
  VkDlssContext(vk::NrFunctions functions, VkDevice device, std::array<bool, color::VkColorPipeline::OUTPUT_VARIANTS> storage_formats);
  // Every frame's status reset, when nothing of this frame or evaluate is to report.
  void ResetFrameStatus();
  // The settled Session work both BeginFrame and the idle recompute need: SetEnabled, Tick, the release on OFF or FAILED.
  void UpdateSession(std::chrono::steady_clock::time_point now);
  void NotePlacementChange();
  void SetOutputProblem(std::string problem);
  // Plan 19: NR at Present runs natively here this frame (the placement is Present and the add-on chose the native route).
  [[nodiscard]] bool NativePresent() const { return placement_.placement == Placement::PRESENT && config_.vk_native_present; }
  // Plan 19: this present's frame for native NR at Present: why it cannot take NR (the copy access, the format, a blit the driver lacks), else its encoding
  // and settled work layout.
  void ResolvePresentFrame(std::chrono::steady_clock::time_point now);
  void SetPresentProblem(std::string problem);
  // Plan 19: `copy_format` and RGBA16F can be blitted between on this GPU (optimal tiling), asked of the driver once per format.
  bool Blittable(VkFormat copy_format);
  // Plan 19: a native Present frame that goes without NR, with `reason` (static text).
  sources::PipelineResult SkipPresent(std::string_view reason);
  // Review 97f99e3 (Important 2): whether a recording may hand NGX `shape` as its motion input (VkMotionCheck). Empty: yes; otherwise the skip reason (static
  // text), and after VK_MOTION_RELOAD_RECORDINGS of them the next present reloads the Session from OFF.
  std::string_view CheckMotion(const VkMotionShape& shape);
  // What both DLSS placements take from one evaluate: the target (`resource`, `region`, the resolved encoding, the strengths, the game's exposure) and NR's inputs
  // with the game's motion vectors in place.
  struct ReadFrame {
    sources::VkDlssTarget target;
    nr::FrameInputs inputs;
  };
  [[nodiscard]] ReadFrame ReadDlssFrame(const ngx_hooks::DlssFrame& frame, const ngx_hooks::VkDlssResources& copies, const NVSDK_NGX_Resource_VK* resource,
                                        nr::Rect region, bool reset_hint) const;
  // Key decision c: the token of a recording, issued before it and kept open on `list` until EndEvaluate. True when this call opened it (a token an earlier
  // recording of the same hooked call opened is reused).
  bool OpenToken(VkListState& list);
  // A recording that put nothing on the buffer: its token goes back unsubmitted.
  void DropOpenToken(VkListState& list);
  // The size the Session is told each frame: the canvas for Before upscaling, DLSS's output region otherwise.
  [[nodiscard]] nr::Size SessionFrameSize() const;
  // Plan 14: the look stage, the colour fixes and passes 2..10 for the recording about to be made, with the frame time since the last one (the stabiliser's).
  void ApplyLookConfig(std::chrono::steady_clock::time_point now);
  // Plan 14 Task 8, as DeviceContext::SettleLayout: the settled work image (and Before upscaling's floor canvas) for `output` at the Resolution mode; the
  // Session hears the canvas.
  sources::WorkLayout SettleLayout(nr::Size output, bool before_upscaling, std::chrono::steady_clock::time_point now);
  // VK_ERROR_DEVICE_LOST: the latch rule (within 10 s of a recording in a game command buffer), the card's text, then Drop(true).
  void NoteDeviceLost(std::chrono::steady_clock::time_point now);
  // Device loss (`device_lost`: Session::OnDeviceLost) or abandon (the runtime's patches restored, no NGX call): the Session, the host and the
  // pipeline are leaked, and the completion abandoned (no event or semaphore destroyed). Idempotent.
  void Drop(bool device_lost);
  [[nodiscard]] bool BeforeUpscaling() const { return config_.pre_upscale && main_feature_ == NVSDK_NGX_Feature_SuperSampling; }

  vk::NrFunctions functions_;
  VkDevice device_;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;  // Plan 19: the blit format checks
  std::array<bool, color::VkColorPipeline::OUTPUT_VARIANTS> storage_formats_;  // each output variant's format is a storage format here
  std::unique_ptr<vk::NrCompletion> completion_;
  std::unique_ptr<vk::CommandRing> ring_;  // Plan 19: native Present's own command buffers; null when the effect queue's family is unknown
  std::unique_ptr<nr::Host> host_;
  nr::VkHost* vk_host_ = nullptr;  // Plan 19: host_ as NVIDIA's runtime (null with a test's stand-in), for the per-present core initialisation
  std::unique_ptr<nr::Session> session_;
  std::unique_ptr<sources::VkNrPipeline> pipeline_;
  FrameConfig config_;
  nr::Controls controls_;
  PlacementChoice placement_;
  std::optional<Placement> logged_placement_;
  std::string blocked_reason_;
  std::string dlss_unavailable_reason_;
  std::string output_message_;  // a device loss, or why DLSS's output cannot take NR (format, size, pipelines)
  std::string_view skip_reason_;
  bool skip_from_session_ = false;
  std::optional<ngx_hooks::CreateSnapshot> main_snapshot_;
  NVSDK_NGX_Feature main_feature_ = NVSDK_NGX_Feature_Reserved_Unknown;
  ngx_hooks::ResetMerger reset_merger_;
  std::optional<std::chrono::steady_clock::time_point> last_main_evaluate_;
  std::optional<std::chrono::steady_clock::time_point> last_game_list_recording_;
  std::optional<std::chrono::steady_clock::time_point> last_look_recording_;  // Plan 14: the stabiliser's frame time
  nr::Size output_size_;                          // the latest evaluate's output region
  VkFormat output_format_ = VK_FORMAT_UNDEFINED;  // and its view format
  std::vector<VkFormat> unbuildable_formats_;     // formats whose pipelines could not be built: never tried again
  bool pre_sr_unbuildable_ = false;               // Before upscaling's pipelines could not be built: never tried again
  // Before upscaling (v2 design §3.8, §3.9): the render region the game's Color holds; both placements (Plan 14): the settled work image keyed on, and the
  // canvas NR runs at.
  sources::WorkSizeSettle settle_;
  nr::Size frame_size_;
  nr::Size work_size_;
  nr::Size canvas_size_;
  nr::SessionState logged_state_ = nr::SessionState::OFF;
  uint32_t passes_run_ = 0u;
  uint32_t trace_frames_ = 0u;
  sources::MotionSource motion_source_ = sources::MotionSource::NONE;
  float motion_scale_x_ = 1.f;
  float motion_scale_y_ = 1.f;
  bool nr_applied_ = false;
  bool last_motion_vectors_missing_ = false;
  bool reset_owed_ = false;  // a frame that did not apply NR: the next one that does starts NR's history afresh (Plan 3, Minor 4)
  bool dlss_seen_ = false;
  // Plan 18 Task 12: the game switched its DLSS off here (FrameConfig::dlss_released held long enough, DLSS seen, the DLSS placements available): Source =
  // DLSS runs at Present from that present until the next main evaluate shows DLSS on again. Frozen once the context stops (the game's NGX shutdown, a loss,
  // a teardown). `dlss_off_logged_`: the placement really fell back, so the off line was logged and the on line is owed (fix round 1, M-6).
  DlssOffLatch dlss_off_;
  bool dlss_off_logged_ = false;
  bool dlss_idle_ = false;
  bool dlss_paused_ = false;  // final review, minor 1: no evaluate for 250 ms, at any placement (PresentMotionGap's DLSS_PAUSED)
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
    uint64_t lag_min = UINT64_MAX;
    uint64_t lag_max = 0u;
    std::optional<std::chrono::steady_clock::time_point> since;
  };
  PresentMotionCounters present_motion_counters_;
  // What the latest present bound, for the per-frame trace (fix round 1, M1: formatted only there).
  enum class BoundMotion : uint8_t { NONE, LAUNCHPAD, OWN_COPY, OLDER_COPY, NO_COPY };
  BoundMotion present_motion_bound_ = BoundMotion::NONE;
  uint64_t present_motion_age_ = 0u;
  void NotePresentMotionSecond(std::chrono::steady_clock::time_point now);
  bool device_lost_ = false;
  bool torn_down_ = false;
  bool dropped_ = false;           // Drop ran: the Session, the host and the pipeline are leaked
  bool core_shut_down_ = false;    // OnCoreShutdown ran: no NGX call on this device any more, and NR never loads here again
  bool present_holds_nr_ = false;  // this frame's claim belongs to the device's Present path (the hand-off of design R86)
  uint64_t settings_generation_ = 0u;
  // Plan 19: native NR at Present.
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
  uint32_t present_queue_timeouts_ = 0u;              // T6 (I-2): second-queue timeouts in a row
  std::string present_stopped_;                       // T6: PresentStopped
  std::string present_create_failure_;                // T6 (M-4): CreateFailure
  std::optional<std::chrono::steady_clock::time_point> last_present_submission_;  // T6 (I-4): the latest native Present submission
  bool present_latch_tripped_ = false;                                             // T6 (I-4): PresentLatchTripped
  // T6 fix round (I-A): the game's NGX shutdown came while NR ran at Present and NR was released cleanly; it becomes PresentStopped once the game has gone on
  // presenting (CoreShutdownStopDue), counted in BeginFrame.
  std::optional<std::chrono::steady_clock::time_point> pending_stop_since_;
  uint32_t pending_stop_presents_ = 0u;
  std::function<void()> before_present_load_;  // T6 fix round (M-1): VkContextSetup::before_present_load
  // In-game round 1 (bug A): the command buffers the Session's NR was loaded for, and a reload through OFF in progress because the placement moved to the
  // other ones (VkStreamNeedsReload).
  VkNrStream session_stream_ = VkNrStream::NONE;
  bool stream_reload_ = false;
  bool own_drain_waited_ = false;  // transitions: Uplift's own command buffers were waited for in this drain from Present (once)
  // Review 97f99e3 (Important 2): the motion input this load's features were first evaluated with, different ones in a row since, and why the next present
  // reloads the Session (empty: no reason).
  std::optional<VkMotionShape> session_motion_;
  nr::Size dlss_motion_size_;  // stress round: the latest readable DLSS motion region (the Present copies' size)
  uint32_t motion_mismatches_ = 0u;
  std::string input_reload_reason_;
  uint32_t queue_family_ = UINT32_MAX;  // the effect queue's family, to make the ring again for Present
  bool ring_failed_ = false;            // the ring could not be made again: NR at Present cannot run here
};

}  // namespace uplift::addon
