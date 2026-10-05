#pragma once

#include <d3d10_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "bridge/d3d11_bridge.hpp"
#include "bridge/keyed_relay.hpp"
#include "nr/types.hpp"

namespace uplift::bridge {

// Plan 8 (D3D10 design §2): a D3D10 device's frames reach NR through a relay D3D11 device on its adapter and Plan 7's
// D3D11Bridge on that relay. The relay makes keyed-mutex textures the D3D10 device opens by their legacy (KMT) handles;
// D3D10 copies in and out under the keyed mutex (key 0: the game's side, key 1: the relay's), and the relay drives the
// D3D11 bridge's fences. Plan 10 (design §3.1): the relay and its hand-offs are KeyedRelay's, which the 32-bit D3D10 client shares.
// - ReShade-free, with D3D11Bridge's public face over the game's D3D10 resources.
// - The CPU never waits for the GPU: a keyed-mutex acquire only waits for a release this same thread made just before
//   (capped all the same), and only the D3D11 bridge's destructor waits (2 s cap per queue).
// - One latch: the D3D11 bridge's. A key that did not come (KeyedRelay's latch) is passed into it before the call that saw it
//   returns, and a stopped D3D11 bridge stops the relay's operations too. Not thread-safe: the add-on's lock.
class D3D10Bridge {
 public:
  // nullptr, with `error` set, when the adapter is not NVIDIA's, or the relay or the D3D11 bridge cannot be made.
  // `relay_created` and `created` receive the relay as D3D11CreateDevice returned it and the private D3D12 device as D3D12Side made it,
  // whether or not this succeeds. Under ReShade the relay is its proxy, and so is the private device on the fallback (D3D12CreateDevice's): their last
  // release raises destroy_device, so the caller releases them only outside its own lock.
  static std::unique_ptr<D3D10Bridge> Create(ID3D10Device* device, Microsoft::WRL::ComPtr<ID3D11Device>* relay_created,
                                             Microsoft::WRL::ComPtr<ID3D12Device>* created, std::string* error);
  // The keyed textures, then the D3D11 bridge (its 2 s wait), then the relay (one last flush, since its releases are destroyed only
  // by a Flush, then its proxy). Under ReShade, destroy it only outside the add-on's lock, and Stop it first, before
  // DeviceContext::Teardown.
  ~D3D10Bridge() = default;
  D3D10Bridge(const D3D10Bridge&) = delete;
  D3D10Bridge& operator=(const D3D10Bridge&) = delete;

  [[nodiscard]] ID3D12Device* Device() const { return bridge_->Device(); }  // native: NGX and every Uplift object use it
  [[nodiscard]] ID3D12CommandQueue* Queue() const { return bridge_->Queue(); }
  [[nodiscard]] bool Independent() const { return bridge_->Independent(); }  // D3D12Side::Independent: whether Retry now can replace it
  // As D3D11Bridge::BeginFrame, for the game's D3D10 back buffer. The keyed colour exists only while `running` on a
  // usable image and no bridge stopped; otherwise it, the keyed mask and the keyed motion are retired (Plan 7 C-1). The
  // relay has no Present, so a frame that released anything on it also flushes it, whether or not NR runs (Plan 8 final
  // review C-1: D3D11 destroys a released object only at its context's next Flush).
  BridgeFrame BeginFrame(ID3D10Resource* back_buffer, bool enabled, bool running, std::chrono::steady_clock::time_point now);
  // One bridged recording (design §2). True when NR's result reached `back_buffer`. `motion`: this frame's UPLIFT_MV
  // (RG16F); null without LaunchPad.
  bool Run(ID3D10Resource* back_buffer, ID3D10Resource* motion, const Recorder& record);
  // At the end of the effects: UPLIFT_MASK through a keyed mask into the D3D11 bridge's shared mask.
  MaskCopy CopyMask(ID3D10Resource* mask);
  void ReleaseMask();
  // Stops the bridge for the session and releases every queued fence wait (D3D11Bridge::Stop).
  void Stop(std::string reason);
  [[nodiscard]] uint64_t SharedBytes() const;
  [[nodiscard]] std::string StatusLine() const;
  [[nodiscard]] bool Stopped() const { return bridge_->Stopped() || relay_->Stopped(); }
  [[nodiscard]] std::string_view Latch() const { return bridge_->Latch(); }  // Plan 17: the card's reason (the relay's reaches it at the next BeginFrame)

 private:
  D3D10Bridge() = default;

  using Keyed = KeyedRelay::Keyed;
  // A keyed texture that could not be made or opened: retried only for another size or format, or after a re-enable
  // (Plan 7 final review Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HRESULT result = S_OK;
    bool bridge = false;  // the mask only: the D3D11 bridge's share with the private device failed, not the relay's own
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const {
      return size == other_size && format == other_format;
    }
  };

  // The D3D11 bridge's own verdict on the keyed colour (its share with the private device failed). It sees a back buffer
  // only while NR runs, so the verdict is kept here, per size and format, and reported while NR is off too (Plan 8
  // batch 2 review I-1); it goes with the other failure caches when NR is disabled.
  struct BridgeProblem {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string text;
  };

  // Passes the relay's latch (a key that did not come) into the D3D11 bridge, whose Stop also signals its fences, and the D3D11
  // bridge's latch into the relay, so a key released after either says nothing more. Called before the call that saw a latch returns.
  void SyncLatches();
  // Submits the relay's queued work, which is also what destroys what it released: the relay has no Present to do it. The D3D11
  // bridge's own releases are destroyed by the same flush.
  void FlushRelay();

  // Declared first, so released last (after the D3D11 bridge, which is on it): the relay and, last of all, its proxy.
  std::unique_ptr<KeyedRelay> relay_;
  Microsoft::WRL::ComPtr<ID3D10Device> game_;  // the game's device, native
  std::unique_ptr<D3D11Bridge> bridge_;        // on the relay's native device
  Keyed color_;
  Keyed mask_;
  Keyed motion_;
  std::optional<ShareFailure> color_failure_;
  std::optional<ShareFailure> mask_failure_;
  std::optional<ShareFailure> motion_failure_;
  std::optional<BridgeProblem> bridge_problem_;  // the D3D11 bridge's last verdict on the keyed colour
  bool shared_ready_ = false;                    // this frame's BeginFrame returned the D3D11 bridge's shared copy
  std::string problem_;                          // the back buffer's problem, logged once per change
};

}  // namespace uplift::bridge
