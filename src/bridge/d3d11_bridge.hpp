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
  // `created` receives the device as D3D12CreateDevice returned it, whether or not this succeeds. Under ReShade that is
  // its proxy, whose last release raises destroy_device, so the caller releases it only outside its own lock.
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
  // First thing at every present. It runs the watchdog, frees retired surfaces the queue has passed, and describes
  // `back_buffer`. While `running` (NR's state is not OFF) it also returns the shared copy sized like it; otherwise,
  // and after a latch, it retires the shared surfaces (final review C-1 and Minor 2). `enabled` false (the user's
  // Enable) forgets failed shares, so re-enabling retries them. Plan 8: `back_buffer` null runs only the watchdog and
  // the retiring, and returns an empty frame (or the latch's): the D3D10 relay has no copy to describe while NR is off.
  BridgeFrame BeginFrame(ID3D11Resource* back_buffer, bool enabled, bool running,
                         std::chrono::steady_clock::time_point now);
  // One bridged recording (design §2), through RunBridgedFrame. True when NR's result reached `back_buffer`.
  // Nothing runs after the latch, while the ring is busy, or without a shared copy.
  // `motion`: this frame's UPLIFT_MV (RG16F), copied with the back buffer; null without LaunchPad.
  bool Run(ID3D11Resource* back_buffer, ID3D11Resource* motion, const Recorder& record);
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run's targets.mask.
  MaskCopy CopyMask(ID3D11Resource* mask);
  void ReleaseMask();
  // Final review I-2: stops the bridge for the session (no new wait is ever queued again) and signals both cross-API
  // fences from the CPU, up to the highest value any wait on them was queued for, so neither the game's queue nor the
  // private one can stay blocked. On a watchdog trip, and before DeviceContext::Teardown when the game's device goes.
  void Stop(std::string reason);
  [[nodiscard]] uint64_t SharedBytes() const;
  [[nodiscard]] std::string StatusLine() const;
  [[nodiscard]] bool Stopped() const { return !latch_.empty(); }    // Plan 8: the D3D10 bridge's one latch
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
  };
  // Final review Minor 3: a share that failed. It is retried only for another size or format, or after NR is
  // re-enabled, instead of making and freeing a full-size texture every frame.
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HRESULT result = S_OK;
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const {
      return size == other_size && format == other_format;
    }
  };
  // Creates `shared` in COMMON (the state D3D12 requires while the other API may touch it). False, leaving `shared`
  // untouched, on failure; `result` gets the failing call's HRESULT (S_OK on success), for the caller's diagnostic.
  bool CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, uint32_t bytes_per_pixel,
                    const wchar_t* name, Shared* shared, HRESULT* result);
  // Frees `shared` once the private queue has passed its last use (now, when it already has), and empties it.
  void Retire(Shared* shared);

  // Plan 9: the private device, queue, allocator ring, list, `progress` fence and retire list. Declared first, so
  // released last: its created device is the one D3D12CreateDevice returned (ReShade's proxy in a game).
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
