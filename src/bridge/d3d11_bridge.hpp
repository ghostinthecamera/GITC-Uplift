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
#include "nr/types.hpp"

namespace uplift::bridge {

// Both shader-read states: how NrPipeline keeps its mask copy, and how Plan 6's UPLIFT_MV is read.
inline constexpr D3D12_RESOURCE_STATES SHADER_READ =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// A whole-resource transition barrier.
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after);

// Plan 7: the FrameHost of a bridged recording. The private list, with the shared back buffer as the target.
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
  ID3D12Resource* mask = nullptr;    // a fresh UPLIFT_MASK copy, COPY_SOURCE; null when none arrived since the last run
  ID3D12Resource* motion = nullptr;  // Plan 7 (decision 3): this frame's UPLIFT_MV, SHADER_READ; null without one
  bool motion_is_dlss = false;       // Plan 14: `motion` is DLSS's vectors copied in the game's frame (Vulkan), not Launchpad's; the helper's transports never set it
  // Plan 18: DLSS's vectors' scale with `motion_is_dlss` (Direct3D 11's ring keeps the raw vectors); 1 for Vulkan's scaled copies and Launchpad's.
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  nr::Rect motion_region;  // Plan 18 (fix round 1, M-4): `motion`'s subrect (a ring slot's region, at (0, 0)); empty: the whole texture
};

// Records NR on `list` and says whether it wrote targets.color: the copy back to D3D11 follows only then.
using Recorder = std::function<bool(ID3D12GraphicsCommandList* list, const BridgeTargets& targets)>;

struct BridgeFrame {
  ID3D12Resource* color = nullptr;  // the shared copy, DeviceContext::BeginFrame's target; null while unavailable
  // The back buffer's, also while `color` is null (NR off): the Session decides to load from them (final review Minor 2).
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::string_view problem;  // why the back buffer cannot reach NR; views the bridge's own text until the next call
};

// CopyMask's result.
struct MaskCopy {
  std::optional<nr::Size> size;  // copied: the next recording gets it
  bool unsupported = false;      // NR cannot read this format; any other failure is a share that failed
  bool relay_failed = false;     // Plan 8: that share was the D3D10 relay's own keyed one, not the private device's
};

// Plan 18 (design §3): which DLSS stage a hand-off runs.
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
};
// What the recorder gets on the private device. Every texture is COMMON on entry and must be COMMON again on return (the mask: COPY_SOURCE, as
// BridgeTargets::mask).
struct DlssHandoffTargets {
  ID3D12Resource* image = nullptr;     // After DLSS: the region's copy at (0, 0), where NR's result goes; Before upscaling: the colour's copy
  nr::Rect region;                     // NR's region inside `image`
  ID3D12Resource* motion = nullptr;    // the motion region's copy at (0, 0); null without one, or when it cannot be shared
  nr::Rect motion_region;              // Fix round 1 (M-4): the region inside `motion` (at (0, 0), the evaluate's size; the share is the vectors' whole size)
  ID3D12Resource* exposure = nullptr;  // the exposure texture's copy; null without one
  ID3D12Resource* mask = nullptr;      // a fresh UPLIFT_MASK copy (CopyMask), or null
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
// Plan 18: `region` inside `texture`, or its whole size when `region` is empty; empty when it is not a 2D texture or the region lies outside it.
[[nodiscard]] nr::Rect ResolveRegion11(ID3D11Resource* texture, nr::Rect region);

// Plan 7 (D3D11 design §2): a private D3D12 device on a D3D11 device's adapter, the back buffer's shared copy, one
// shared fence, and the per-recording hand-off NR runs inside.
// - ReShade-free: it takes any D3D11 device and resource, so a later relay device (D3D10, D3D9) drives it as it is.
// - It never waits on the CPU, except in its destructor.
// - Not thread-safe: the add-on's lock.
class D3D11Bridge {
 public:
  // nullptr, with `error` set, when:
  // - the adapter is not NVIDIA's;
  // - D3D11 fences are missing (Windows 10 before 1703);
  // - a D3D12 object cannot be created.
  // `created` receives the private device as D3D12Side made it, whether or not this succeeds: an independent one from the device
  // factory, or on the fallback D3D12CreateDevice's (under ReShade its proxy, whose last release raises destroy_device), so the caller releases it only
  // outside its own lock.
  // `label` names the game's API in the bridge's log and status texts (Plan 8: "Direct3D 10" for the D3D10 relay).
  static std::unique_ptr<D3D11Bridge> Create(ID3D11Device* device, Microsoft::WRL::ComPtr<ID3D12Device>* created,
                                             std::string* error, std::string_view label = "Direct3D 11");
  // Waits up to 2 s for the private queue, then releases everything, the created device last. Under ReShade, destroy
  // it only outside the add-on's lock. It never signals a fence: Stop does, before DeviceContext::Teardown.
  ~D3D11Bridge();
  D3D11Bridge(const D3D11Bridge&) = delete;
  D3D11Bridge& operator=(const D3D11Bridge&) = delete;

  [[nodiscard]] ID3D12Device* Device() const { return side_->Device(); }  // native: NGX and every Uplift object use it
  [[nodiscard]] ID3D12CommandQueue* Queue() const { return side_->Queue(); }
  [[nodiscard]] bool Independent() const { return side_->Independent(); }  // D3D12Side::Independent: whether Retry now can replace it
  // First thing at every present. It runs the watchdog, frees retired surfaces the queue has passed, and describes
  // `back_buffer`. While `running` (NR's state is not OFF) it also returns the shared copy sized like it; otherwise,
  // and after a latch, it retires the shared surfaces (final review C-1 and Minor 2). `enabled` false (the user's
  // Enable) forgets failed shares, so re-enabling retries them. Plan 8: `back_buffer` null runs only the watchdog and
  // the retiring, and returns an empty frame (or the latch's): the D3D10 relay has no copy to describe while NR is off.
  // Plan 18: `present_copy` false (NR runs at a DLSS stage, inside the game's frame) retires the back buffer's copy and makes none: the frame is described
  // only. NR off or a latch also releases the DLSS stages' shares.
  BridgeFrame BeginFrame(ID3D11Resource* back_buffer, bool enabled, bool running,
                         std::chrono::steady_clock::time_point now, bool present_copy = true);
  // One bridged recording (design §2), through RunBridgedFrame. True when NR's result reached `back_buffer`.
  // Nothing runs after the latch, while the ring is busy, or without a shared copy.
  // `motion`: this frame's UPLIFT_MV (RG16F), copied with the back buffer; null without LaunchPad.
  // Plan 18 (design §4): `dlss_motion` (NR at Present on a DLSS game): this frame's ring slot, when an evaluate copied one (CopyPresentMotion), is bound as
  // DLSS's vectors with its scale, ahead of Launchpad's (`motion` is then not copied).
  bool Run(ID3D11Resource* back_buffer, ID3D11Resource* motion, const Recorder& record, bool dlss_motion = false);
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run's targets.mask.
  MaskCopy CopyMask(ID3D11Resource* mask);
  void ReleaseMask();
  // Plan 18 (design §3, §6): one DLSS stage's mid-frame hand-off, inside the game's hooked evaluate on its immediate context, through RunBridgedFrame: the
  // image's region (After DLSS) or the whole colour (Before upscaling), with DLSS's vectors and exposure when they can be shared, copied into shared
  // textures; Signal; the private queue's Wait, `record`, Signal; the game's Wait; After DLSS's copy back. The watchdog runs first (a stalled or removed
  // private device stops the bridge, and Stop releases every queued wait from the CPU). Never waits on the CPU: a busy allocator ring skips the evaluate.
  // The shares count in SharedBytes and go with ReleaseDlss, NR off, or a latch.
  DlssHandoff RunDlss(const DlssHandoffInput& input, const DlssRecorder& record, std::chrono::steady_clock::time_point now);
  // Plan 18: the DLSS stages' shares go (NR runs at Present, or is off), each once the private queue has passed it.
  void ReleaseDlss();
  // Plan 18 (design §4): DLSS's vectors for the Present path, copied on the game's immediate context inside the hooked evaluate: the motion region into the
  // ring slot of the frame being rendered, with its scale (MV.Scale x MotionScale). Each slot is the vectors' whole size and own format (the region goes to
  // its (0, 0) and the slot keeps it, so a dynamic render size never remakes one inside the evaluate); the slots count in SharedBytes. A slot the private
  // queue may still read is skipped (false), never waited for.
  bool CopyPresentMotion(ID3D11Resource* motion, nr::Rect region, float scale_x, float scale_y);
  // Plan 18: the ring goes (its copies are not wanted: NR off, not at Present, or Motion vectors not DLSS's), each slot once the private queue has passed it.
  void ReleasePresentMotion();
  // Final review I-2: stops the bridge for good (no new wait is ever queued again; Plan 17's Retry now builds a new bridge) and signals both cross-API
  // fences from the CPU, up to the highest value any wait on them was queued for, so neither the game's queue nor the
  // private one can stay blocked. On a watchdog trip, and before DeviceContext::Teardown when the game's device goes.
  void Stop(std::string reason);
  // Test phase, DiagnosticRemoveDevice's mid-frame removal only (never on a normal frame): flushes the game's immediate context and waits on the CPU, up to
  // `cap_ms`, until the game's queue passed its last signal to the private queue (the copies in ahead of it are done, so the game's queue is at its wait).
  // True when it did.
  bool DiagnosticWaitForGameSignal(DWORD cap_ms);
  [[nodiscard]] uint64_t SharedBytes() const;
  // Plan 18 (design §5): the Details line names the stage NR runs at (`placement`, the context's).
  [[nodiscard]] std::string StatusLine(addon::Placement placement = addon::Placement::PRESENT) const;
  [[nodiscard]] bool Stopped() const { return !latch_.empty(); }    // Plan 8: the D3D10 bridge's one latch
  [[nodiscard]] std::string_view Latch() const { return latch_; }   // Plan 17: the card's reason; empty while it runs
  [[nodiscard]] uint64_t BusySkips() const { return busy_skips_; }  // Plan 8: the D3D10 bridge's status line
  // Plan 8: true once, after an object on the D3D11 device was released (a retired surface freed, or one dropped at
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
    // Plan 18: CreateShared's flags, so EnsureShared never hands After DLSS (a UAV) a texture Before upscaling made without one at the same size and format.
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
  };
  // Final review Minor 3: a share that failed. It is retried only for another size or format, or after NR is
  // re-enabled, instead of making and freeing a full-size texture every frame. Plan 18 (fix round 1, M-5): and for other flags, so one DLSS stage's
  // failure never blocks the other's texture at the same size and format.
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HRESULT result = S_OK;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format, D3D12_RESOURCE_FLAGS other_flags = D3D12_RESOURCE_FLAG_NONE) const {
      return size == other_size && format == other_format && flags == other_flags;
    }
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
  // Plan 18 (design §6): the watchdog of BeginFrame, also run at every hand-off: a removed private device, or no progress for 2 s while NR's work is
  // outstanding, stops the bridge (Stop signals both fences from the CPU, so the game's queue goes on).
  void CheckWatchdog(std::chrono::steady_clock::time_point now);
  // Plan 18: `shared` for (size, format, flags) unless it already matches; false, with `failure` set and one WARN naming `what`, when it cannot be made.
  // Not retried for that size and format (final review Minor 3) until NR is re-enabled.
  bool EnsureShared(Shared* shared, std::optional<ShareFailure>* failure, nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                    uint32_t bytes_per_pixel, std::string_view what);
  // Plan 18: the Present ring's shared bytes (in SharedBytes, and named on the Details line).
  [[nodiscard]] uint64_t RingBytes() const;

  // Plan 9: the private device, queue, allocator ring, list, `progress` fence and retire list. Declared first, so
  // released last: its created device is the one D3D12Side made (on the fallback, D3D12CreateDevice's: ReShade's proxy in a game).
  std::unique_ptr<D3D12Side> side_;
  // Final review I-2: one shared fence per direction, each signalled by one queue only, in increasing order. After
  // Stop signals one from the CPU, a later, lower signal still queued on its queue can only hold a wait back until
  // that same queue's next signals (which end at or above every value waited for), never for good; and no new wait is
  // queued once stopped. One fence for both directions would let such a lower signal re-block the other queue.
  Microsoft::WRL::ComPtr<ID3D12Fence> to12_;  // D3D11 -> D3D12: odd values, D3D11 signals, the private queue waits
  Microsoft::WRL::ComPtr<ID3D12Fence> to11_;  // D3D12 -> D3D11: even values, the private queue signals, D3D11 waits
  Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;  // the immediate context
  Microsoft::WRL::ComPtr<ID3D11Fence> to12_11_;             // to12_ and to11_, opened on the D3D11 device
  Microsoft::WRL::ComPtr<ID3D11Fence> to11_11_;
  Shared color_;
  Shared mask_;
  Shared motion_;            // Plan 7 (decision 3): LaunchPad's UPLIFT_MV, shared like the mask
  bool mask_fresh_ = false;  // CopyMask wrote mask_ since the last recording
  std::optional<ShareFailure> color_failure_;
  std::optional<ShareFailure> mask_failure_;
  std::optional<ShareFailure> motion_failure_;
  Shared dlss_image_;     // Plan 18: After DLSS's output region, or Before upscaling's whole colour
  Shared dlss_motion_;    // the motion region
  Shared dlss_exposure_;  // 1x1
  Shared dlss_swap_;      // Before upscaling: RGBA16F at the colour's size, what DLSS reads
  std::optional<ShareFailure> dlss_image_failure_;
  std::optional<ShareFailure> dlss_motion_failure_;
  std::optional<ShareFailure> dlss_exposure_failure_;
  std::optional<ShareFailure> dlss_swap_failure_;
  // Plan 18 (design §4): DLSS's vectors for the Present path, four shared slots by the frame they were copied for (as Plan 14's Vulkan copies).
  static constexpr size_t RING_SLOTS = 4u;
  struct RingSlot {
    Shared shared;        // fix round 1 (M-4): the vectors' whole size, so a dynamic render size never remakes it inside the evaluate
    nr::Rect region;      // the copied region, at (0, 0)
    uint64_t frame = 0u;  // the present it was copied for (presents_ + 1 at the copy); 0: none
    float scale_x = 1.f;  // MV.Scale x MotionScale: the slot keeps the raw vectors
    float scale_y = 1.f;
  };
  std::array<RingSlot, RING_SLOTS> ring_;
  std::optional<ShareFailure> ring_failure_;
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
  std::string problem_;                // the back buffer's problem, logged once per change
  std::string latch_;                  // set once: the bridge stopped for the session
  std::string label_ = "Direct3D 11";  // Plan 8: Create's label
};

}  // namespace uplift::bridge
