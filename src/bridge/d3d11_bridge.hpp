#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "addon/device_context.hpp"
#include "bridge/bridge_sequence.hpp"
#include "bridge/d3d12_side.hpp"
#include "bridge/motion_share_keep.hpp"
#include "bridge/private_device_bridge.hpp"
#include "bridge/share_failure.hpp"
#include "nr/types.hpp"
#include "sources/present_motion_ring.hpp"

namespace uplift::bridge {

// Both shader-read states: how NrPipeline keeps its mask copy, and how UPLIFT_MV is read.
inline constexpr D3D12_RESOURCE_STATES SHADER_READ =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// A whole-resource transition barrier.
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after);

// The FrameHost of a bridged recording. The private list, with the shared back buffer as the target.
class ListFrameHost final : public addon::FrameHost {
 public:
  ListFrameHost(ID3D12GraphicsCommandList* list, ID3D12Resource* target) : list_(list), target_(target) {}
  void FlushPending() override {}  // No-op: the private list holds only this recording
  ID3D12GraphicsCommandList* NativeList() override { return list_; }
  void TargetBarrier(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override {
    Transition(list_, target_, before, after);
  }

 private:
  ID3D12GraphicsCommandList* list_;
  ID3D12Resource* target_;
};

// What one bridged recording gets on the private device.
struct BridgeTargets {
  ID3D12Resource* color = nullptr;   // the back buffer's shared copy: COMMON on entry, and COMMON again on return
  ID3D12Resource* mask = nullptr;    // A fresh UPLIFT_MASK copy, COPY_SOURCE; null when none arrived since the last run
  ID3D12Resource* motion = nullptr;  // this frame's UPLIFT_MV, SHADER_READ; null without one
  bool motion_is_dlss = false;       // `motion` is DLSS's vectors copied in the game's frame (Vulkan), not Launchpad's; the helper's transports never set it
  // DLSS's vectors' scale with `motion_is_dlss` (Direct3D 11's ring keeps the raw vectors); 1 for Vulkan's scaled copies and Launchpad's.
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  nr::Rect motion_region;  // `motion`'s subrect (a ring slot's region, at (0, 0)); empty: the whole texture
};

// Records NR on `list` and says whether it wrote targets.color: the copy back to D3D11 follows only then.
using Recorder = std::function<bool(ID3D12GraphicsCommandList* list, const BridgeTargets& targets)>;

struct BridgeFrame {
  ID3D12Resource* color = nullptr;  // the shared copy, DeviceContext::BeginFrame's target; null while unavailable
  // The back buffer's, also while `color` is null (NR off): the Session decides to load from them.
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::string_view problem;  // why the back buffer cannot reach NR; views the bridge's own text until the next call
};

// CopyMask's result.
struct MaskCopy {
  std::optional<nr::Size> size;  // copied: the next recording gets it
  bool unsupported = false;      // NR cannot read this format; any other failure is a share that failed
  bool relay_failed = false;     // that share was the D3D10 relay's own keyed one, not the private device's
};

// Which DLSS stage a hand-off runs.
enum class DlssStage : uint8_t {
  AFTER_DLSS,        // DLSS's output region crosses, NR writes it, and it comes back into the output
  BEFORE_UPSCALING,  // DLSS's colour crosses (into a copy of the whole texture, the region in its place); NR's result comes back in an RGBA16F texture DLSS reads
};
// One DLSS evaluate's game resources for a hand-off (the NGX block's, valid for this call), on the game's Direct3D 11 device.
struct DlssHandoffInput {
  DlssStage stage = DlssStage::AFTER_DLSS;
  ID3D11Resource* image = nullptr;     // After DLSS: DLSS's Output; Before upscaling: its Color
  nr::Rect region;                     // the region NR runs on, inside `image` (ResolveRegion11)
  ID3D11Resource* motion = nullptr;    // DLSS's motion vectors, or null
  nr::Rect motion_region;              // inside `motion` (ResolveRegion11)
  ID3D11Resource* exposure = nullptr;  // the 1x1 exposure texture, or null
  // Split chain: the chain runs both DLSS stages (a Before + After split), so the other stage's shares stay. Otherwise a hand-off frees them, as the
  // single share of before was re-made for whichever stage ran: one stage allocates exactly what it did.
  bool keep_other_stage = false;
};
// What the recorder gets on the private device. Every texture is COMMON on entry and must be COMMON again on return (the mask: COPY_SOURCE, as
// BridgeTargets::mask).
struct DlssHandoffTargets {
  ID3D12Resource* image = nullptr;     // After DLSS: the region's copy at (0, 0), where NR's result goes; Before upscaling: the colour's copy
  nr::Rect region;                     // NR's region inside `image`
  ID3D12Resource* motion = nullptr;    // the motion region's copy at (0, 0); null without one, or when it cannot be shared
  nr::Rect motion_region;              // the region inside `motion` (at (0, 0), the evaluate's size; the share is the vectors' whole size)
  ID3D12Resource* exposure = nullptr;  // the exposure texture's copy; null without one
  ID3D12Resource* mask = nullptr;      // A fresh UPLIFT_MASK copy (CopyMask), or null
  ID3D12Resource* swap = nullptr;      // Before upscaling: RGBA16F at the colour's size, for NR's result (the pipeline's private colour)
};
using DlssRecorder = std::function<bool(ID3D12GraphicsCommandList* list, const DlssHandoffTargets& targets)>;
enum class DlssSkip : uint8_t {
  NONE,
  STOPPED,             // the bridge latched (the watchdog, a removal, a refused step)
  BUSY,                // the allocator ring: this evaluate goes without NR
  NO_INPUT,            // the region is empty or outside its texture this evaluate
  UNSUPPORTED_FORMAT,  // the image cannot be shared (its format, MSAA, an array): the stage is greyed (DeviceContext::NoteStageProblem)
  SHARE_FAILED,        // a share could not be made for another reason (out of memory, Before upscaling's RGBA16F texture): this evaluate goes without NR
};
struct DlssHandoff {
  bool wrote = false;               // NR's result is back in the image (After DLSS), or in the swap texture (Before upscaling)
  bool executed = false;            // the private list was submitted: the recording's tokens go to the context, else they are dropped
  ID3D11Resource* color = nullptr;  // Before upscaling with `wrote`: the swap texture on the game's device, what DLSS reads as Color for this evaluate
  DlssSkip skipped = DlssSkip::NONE;
  std::string problem;              // UNSUPPORTED_FORMAT: why, for Setup and the log
};
// A 2D texture's description; nullopt for anything else (was d3d11_bridge.cpp's TextureDesc).
[[nodiscard]] std::optional<D3D11_TEXTURE2D_DESC> TextureDesc11(ID3D11Resource* resource);
// `region` inside `texture`, or its whole size when `region` is empty; empty when it is not a 2D texture or the region lies outside it.
[[nodiscard]] nr::Rect ResolveRegion11(ID3D11Resource* texture, nr::Rect region);

// A private D3D12 device on a D3D11 device's adapter, the back buffer's shared copy, one
// shared fence, and the per-recording hand-off NR runs inside.
// - ReShade-free: it takes any D3D11 device and resource, so a later relay device (D3D10, D3D9) drives it as it is.
// - It never waits on the CPU, except in its destructor.
// - Not thread-safe: the add-on's lock.
class D3D11Bridge final : public PrivateDeviceBridge {
 public:
  // nullptr, with `error` set, when:
  // - the adapter is not NVIDIA's;
  // - D3D11 fences are missing (Windows 10 before 1703);
  // - a D3D12 object cannot be created.
  // `created` receives the private device as D3D12Side made it, whether or not this succeeds: an independent one from the device
  // factory, or on the fallback D3D12CreateDevice's (under ReShade its proxy, whose last release raises destroy_device), so the caller releases it only
  // outside its own lock.
  // `label` names the game's API in the bridge's log and status texts ("Direct3D 10" for the D3D10 relay).
  static std::unique_ptr<D3D11Bridge> Create(ID3D11Device* device, Microsoft::WRL::ComPtr<ID3D12Device>* created,
                                             std::string* error, std::string_view label = "Direct3D 11");
  // Waits up to 2 s for the private queue, then releases everything, the created device last. Under ReShade, destroy
  // it only outside the add-on's lock. It never signals a fence: Stop does, before DeviceContext::Teardown.
  ~D3D11Bridge();
  D3D11Bridge(const D3D11Bridge&) = delete;
  D3D11Bridge& operator=(const D3D11Bridge&) = delete;

  [[nodiscard]] ID3D12Device* Device() const override { return side_->Device(); }  // native: NGX and every Uplift object use it
  [[nodiscard]] ID3D12CommandQueue* Queue() const override { return side_->Queue(); }
  [[nodiscard]] bool Independent() const override { return side_->Independent(); }  // D3D12Side::Independent: whether Retry now can replace it
  // First thing at every present. It runs the watchdog, frees retired surfaces the queue has passed, and describes
  // `back_buffer`. While `running` (NR's state is not OFF) it also returns the shared copy sized like it; otherwise,
  // and after a latch, it retires the shared surfaces. `enabled` false (the user's
  // Enable) forgets failed shares, so re-enabling retries them. `back_buffer` null runs only the watchdog and
  // the retiring, and returns an empty frame (or the latch's): the D3D10 relay has no copy to describe while NR is off.
  // `present_copy` false (NR runs at a DLSS stage, inside the game's frame) retires the back buffer's copy and makes none: the frame is described
  // only. NR off or a latch also releases the DLSS stages' shares.
  BridgeFrame BeginFrame(ID3D11Resource* back_buffer, bool enabled, bool running,
                         std::chrono::steady_clock::time_point now, bool present_copy = true);
  // One bridged recording, through RunBridgedFrame. True when NR's result reached `back_buffer`.
  // Nothing runs after the latch, while the ring is busy, or without a shared copy.
  // `motion`: this frame's UPLIFT_MV (RG16F), copied with the back buffer; null without LaunchPad.
  // `dlss_motion` (NR at Present on a DLSS game): this frame's ring slot, when an evaluate copied one (CopyPresentMotion), is bound as
  // DLSS's vectors with its scale, ahead of Launchpad's (`motion` is then not copied).
  bool Run(ID3D11Resource* back_buffer, ID3D11Resource* motion, const Recorder& record, bool dlss_motion = false);
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run's targets.mask.
  MaskCopy CopyMask(ID3D11Resource* mask);
  void ReleaseMask();
  // One DLSS stage's mid-frame hand-off, inside the game's hooked evaluate on its immediate context, through RunBridgedFrame: the
  // image's region (After DLSS) or the whole colour (Before upscaling), with DLSS's vectors and exposure when they can be shared, copied into shared
  // textures; Signal; the private queue's Wait, `record`, Signal; the game's Wait; After DLSS's copy back. The watchdog runs first (a stalled or removed
  // private device stops the bridge, and Stop releases every queued wait from the CPU). Never waits on the CPU: a busy allocator ring skips the evaluate.
  // The shares count in SharedBytes and go with ReleaseDlss, NR off, or a latch.
  DlssHandoff RunDlss(const DlssHandoffInput& input, const DlssRecorder& record, std::chrono::steady_clock::time_point now);
  // The DLSS stages' shares go (NR runs at Present, or is off), each once the private queue has passed it.
  void ReleaseDlss();
  // DLSS's vectors for the Present path, copied on the game's immediate context inside the hooked evaluate: the motion region into the
  // ring slot of the frame being rendered, with its scale (MV.Scale x MotionScale). Each slot is the vectors' whole size and own format (the region goes to
  // its (0, 0) and the slot keeps it, so a dynamic render size never remakes one inside the evaluate); the slots count in SharedBytes. A slot the private
  // queue may still read is skipped (false), never waited for.
  bool CopyPresentMotion(ID3D11Resource* motion, nr::Rect region, float scale_x, float scale_y);
  // The ring goes (its copies are not wanted: NR off, not at Present, or Motion vectors not DLSS's), each slot once the private queue has passed it.
  void ReleasePresentMotion();
  // Stops the bridge for good (no new wait is ever queued again; Retry now builds a new bridge) and signals both cross-API
  // fences from the CPU, up to the highest value any wait on them was queued for, so neither the game's queue nor the
  // private one can stay blocked. On a watchdog trip, and before DeviceContext::Teardown when the game's device goes.
  void Stop(std::string reason) override;
  // Diagnostic only, for DiagnosticRemoveDevice's mid-frame removal (never on a normal frame): flushes the game's immediate context and waits on the CPU, up to
  // `cap_ms`, until the game's queue passed its last signal to the private queue (the copies in ahead of it are done, so the game's queue is at its wait).
  // True when it did.
  bool DiagnosticWaitForGameSignal(DWORD cap_ms);
  [[nodiscard]] uint64_t SharedBytes() const override;
  // The Details line names the stage NR runs at (`placement`, the context's).
  [[nodiscard]] std::string StatusLine(addon::Placement placement = addon::Placement::PRESENT) const;
  [[nodiscard]] bool Stopped() const override { return !latch_.empty(); }    // the D3D10 bridge's one latch
  [[nodiscard]] std::string_view Latch() const override { return latch_; }   // the card's reason; empty while it runs
  [[nodiscard]] uint64_t BusySkips() const override { return busy_skips_; }  // the D3D10 bridge's status line
  [[nodiscard]] std::string_view Label() const override { return label_; }  // Create's label
  // Split chain: a three-stage chain makes three private lists per frame (two mid-frame hand-offs and the present's): the allocator ring
  // grows once from 8 to 12 (logged), and never shrinks. The default path keeps 8.
  void GrowRingForSplit();
  [[nodiscard]] size_t RingSize() const { return side_->RingSize(); }
  // Split chain: the shares the DLSS stages and the Present ring made so far (EnsureShared's), for the tests: a Before + After split makes none
  // per evaluate once both stages hold theirs.
  [[nodiscard]] uint64_t DlssSharesMade() const { return dlss_shares_made_; }
  // True once, after an object on the D3D11 device was released (a retired surface freed, or one dropped at
  // once) since the last call. D3D11 destroys a released object only at its context's next Flush, which a device with no
  // Present of its own (the D3D10 relay) must make itself.
  bool TakeReleased() { return std::exchange(released_, false); }

 private:
  D3D11Bridge() = default;

  // A texture on the private device, shared through an NT handle and opened on the D3D11 device.
  struct Shared {
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
    Microsoft::WRL::ComPtr<ID3D11Resource> d3d11;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
    uint64_t last_use = 0u;  // the side's progress value after the newest submitted recording that used it
    // CreateShared's flags, so EnsureShared never hands After DLSS (a UAV) a texture Before upscaling made without one at the same size and format.
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
  };
  // A failed share, with the failing call's HRESULT. Keyed by size, format and creation flags, so one stage's incompatible flags cannot block the
  // other stage at the same size and format.
  using ShareFailure = ShareFailureOf<HRESULT>;
  // A share and the failure that stops it being made again for the same key. Retire empties `shared` only: the failure outlives the texture it was for.
  struct ShareSlot {
    Shared shared;
    std::optional<ShareFailure> failure;
  };
  // Creates `shared` in COMMON (the state D3D12 requires while the other API may touch it). False, leaving `shared`
  // untouched, on failure; `result` gets the failing call's HRESULT (S_OK on success), for the caller's diagnostic.
  bool CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, uint32_t bytes_per_pixel,
                    const wchar_t* name, Shared* shared, HRESULT* result);
  // Frees `shared` once the game's D3D11 context and then the private queue have passed its last use, and empties it.
  void Retire(Shared* shared);
  // Hands each retired pair whose event query has completed to the side, which frees it now or after the private queue's last use.
  // True when one was released at once.
  bool HandOverFinished();
  // The watchdog of BeginFrame, also run at every hand-off: a removed private device, or no progress for BridgeWatchdog::LIMIT while NR's work is
  // outstanding, stops the bridge (Stop signals both fences from the CPU, so the game's queue goes on).
  void CheckWatchdog(std::chrono::steady_clock::time_point now);
  // `shared` for (size, format, flags) unless it already matches; false, with `failure` set and one WARN naming `what`, when it cannot be made.
  // Not retried for that size and format until NR is re-enabled.
  bool EnsureShared(Shared* shared, std::optional<ShareFailure>* failure, nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                    uint32_t bytes_per_pixel, std::string_view what);
  // The Present ring's shared bytes (in SharedBytes, and named on the Details line).
  [[nodiscard]] uint64_t RingBytes() const;

  // The private device, queue, allocator ring, list, `progress` fence and retire list. Declared first, so
  // released last: its created device is the one D3D12Side made (on the fallback, D3D12CreateDevice's: ReShade's proxy in a game).
  std::unique_ptr<D3D12Side> side_;
  // One shared fence per direction, each signalled by one queue only, in increasing order. After
  // Stop signals one from the CPU, a later, lower signal still queued on its queue can only hold a wait back until
  // that same queue's next signals (which end at or above every value waited for), never for good; and no new wait is
  // queued once stopped. One fence for both directions would let such a lower signal re-block the other queue.
  Microsoft::WRL::ComPtr<ID3D12Fence> to12_;  // D3D11 -> D3D12: odd values, D3D11 signals, the private queue waits
  Microsoft::WRL::ComPtr<ID3D12Fence> to11_;  // D3D12 -> D3D11: even values, the private queue signals, D3D11 waits
  Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;  // the immediate context
  Microsoft::WRL::ComPtr<ID3D11Fence> to12_11_;             // to12_ and to11_, opened on the D3D11 device
  Microsoft::WRL::ComPtr<ID3D11Fence> to11_11_;
  ShareSlot color_;
  ShareSlot mask_;
  ShareSlot motion_;         // LaunchPad's UPLIFT_MV, shared like the mask
  MotionShareKeep motion_keep_;  // the run of recordings without UPLIFT_MV while motion_ is held
  bool mask_fresh_ = false;  // CopyMask wrote mask_ since the last recording
  ShareSlot dlss_image_;      // After DLSS's output region
  ShareSlot dlss_pre_image_;  // split chain: Before upscaling's whole colour, its own share so a Before + After split re-makes neither per evaluate
  ShareSlot dlss_motion_;     // the motion region
  ShareSlot dlss_exposure_;   // 1x1
  ShareSlot dlss_swap_;       // Before upscaling: RGBA16F at the colour's size, what DLSS reads
  // Present-motion slots are keyed by their target present frame. The ring grows once when a slot is still read by the private
  // queue; new slots allocate on first use.
  // A present without its own copy binds the newest one within sources::PRESENT_MOTION_MAX_AGE frames.
  static constexpr size_t RING_SLOTS = sources::PRESENT_MOTION_SLOTS;  // room for the grown ring
  size_t ring_slots_ = sources::PRESENT_MOTION_MIN_SLOTS;              // the slots in use
  struct RingSlot {
    Shared shared;        // the vectors' whole size, so a dynamic render size never remakes it inside the evaluate
    nr::Rect region;      // the copied region, at (0, 0)
    uint64_t frame = 0u;  // the present it was copied for (presents_ + 1 at the copy); 0: none
    float scale_x = 1.f;  // MV.Scale x MotionScale: the slot keeps the raw vectors
    float scale_y = 1.f;
  };
  std::array<RingSlot, RING_SLOTS> ring_;
  std::optional<ShareFailure> ring_failure_;  // one for every slot: CopyPresentMotion makes each with the same key (the vectors' size and format, SHARE_FLAGS)
  // Every share's failure, the Present ring's included: BeginFrame forgets them all while NR is disabled, so re-enabling retries every share.
  [[nodiscard]] auto Failures() {
    return std::array{&color_.failure, &mask_.failure, &motion_.failure, &dlss_image_.failure, &dlss_pre_image_.failure,
                      &dlss_motion_.failure, &dlss_exposure_.failure, &dlss_swap_.failure, &ring_failure_};
  }
  // Retire's pairs until the game's D3D11 context has passed them. The context copies into and out of the shares (the Present ring,
  // the DLSS hand-off, the Present path's copies), and the private queue's progress says nothing about that work.
  struct Pending {
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
    Microsoft::WRL::ComPtr<ID3D11Resource> d3d11;
    Microsoft::WRL::ComPtr<ID3D11Query> done;  // an event query ended on context11_ when the pair was retired
    uint64_t last_use = 0u;
  };
  std::vector<Pending> pending_;
  uint64_t presents_ = 0u;  // BeginFrame calls: the frame being rendered is presents_ + 1
  bool released_ = false;  // an object on the D3D11 device was released since TakeReleased last said so
  BridgeWatchdog watchdog_;
  uint64_t last_value_ = 0u;  // the newest fence value handed out
  uint64_t waited12_ = 0u;    // the highest to12_ value the private queue was asked to wait for
  uint64_t waited11_ = 0u;    // the highest to11_ value a D3D11 wait was queued for
  uint64_t busy_skips_ = 0u;
  bool ring_growth_failed_ = false;  // logged once
  uint64_t dlss_shares_made_ = 0u;  // DlssSharesMade
  std::string problem_;                // the back buffer's problem, logged once per change
  std::string latch_;                  // set once: the bridge stopped for the session
  std::string label_ = "Direct3D 11";  // Create's label
};

}  // namespace uplift::bridge
