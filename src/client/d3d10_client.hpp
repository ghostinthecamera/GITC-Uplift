#pragma once

#include <d3d10_1.h>
#include <d3d11_4.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "addon/frame_trigger.hpp"
#include "bridge/keyed_relay.hpp"
#include "client/d3d11_client.hpp"
#include "client/nr_link.hpp"
#include "ipc/protocol.hpp"
#include "nr/types.hpp"

namespace uplift::client {

// Plan 10 (design §3). A 32-bit Direct3D 10 device's frames through Plan 8's relay (bridge::KeyedRelay) and Plan 9's D3D11Client on it:
// FENCED to the helper, GPU-ordered end to end. Only fences cross the process boundary. ReShade-free: native pointers, and on the
// game's device only CopyResource, Flush, OpenSharedResource and AcquireSync/ReleaseSync on the keyed textures. Not thread-safe: the
// add-on's lock. Destroy it outside that lock: the relay is ReShade's proxy, whose last release raises destroy_device.
//
// The hand-off is D3D10Bridge's (Plan 8, design §2 rule 3): key 0 is the game's side and key 1 the relay's, every acquire is capped at
// 2 s and only S_OK counts, every key taken is given back, and each side flushes before the other acquires. The relay has no Present
// (Plan 8's final review C-1), so this flushes it after every call that can release on it. R17 across the relay: the D3D11 client
// queues no wait for a value the helper's reply did not say it submitted, and the keys go back whatever it did.
class D3D10Client {
 public:
  // nullptr + `error` as bridge::KeyedRelay::Create or D3D11Client::Create (label "Direct3D 10"). `relay_created` receives the relay
  // as D3D11CreateDevice returned it, whether or not this succeeds (ReShade's proxy in a game): the caller releases it only outside
  // its own lock.
  static std::unique_ptr<D3D10Client> Create(ID3D10Device* device, Microsoft::WRL::ComPtr<ID3D11Device>* relay_created,
                                             std::string* error);
  // The keyed textures, then the D3D11 client, then the relay (its flush, then its proxy).
  ~D3D10Client() = default;
  D3D10Client(const D3D10Client&) = delete;
  D3D10Client& operator=(const D3D10Client&) = delete;

  [[nodiscard]] ipc::Transport Kind() const { return d3d11_->Kind(); }  // the relay's D3D11 client: FENCED, or KMT when it could not open the fences
  [[nodiscard]] LUID Luid() const { return relay_->Luid(); }
  // FRAME's target for `back_buffer`: D3D11Client::Describe on its description, with the D3D10 side's own problem (a keyed colour the
  // relay could not share, or a relay that stopped) over it. While `running` on a usable image and nothing stopped it also makes the
  // keyed colour (a failure is cached per size and format, and forgotten by `enabled` false); otherwise the keyed colour, mask and motion
  // are dropped. It flushes the relay when it dropped one.
  ipc::Target Describe(ID3D10Resource* back_buffer, bool enabled, bool running);
  // The FRAME reply's news, D3D11Client::Apply's: True when the transport changed (the relay device could not open the helper's fences:
  // the caller DETACHes and ATTACHes afresh with Kind()). Then flushes the relay.
  bool Apply(const ipc::Reply& reply, NrLink& link);
  // One RUN (design §3.1): the back buffer (and `motion`, this frame's UPLIFT_MV, FENCED only) into the keyed textures, D3D11Client::Run
  // on the relay's side, the keys back, and NR's result into `back_buffer` when it wrote. True when NR's result reached `back_buffer`.
  bool Run(ID3D10Resource* back_buffer, ID3D10Resource* motion, addon::TriggerPoint point, NrLink& link);
  [[nodiscard]] bool LastRunSent() const { return run_sent_; }  // the last Run put a RUN on the wire
  // At the end of the effects: UPLIFT_MASK through a keyed mask into the D3D11 client's shared mask (FENCED only). A mask the helper could
  // not share is not handed over again until NR is re-enabled.
  MaskCopy CopyMask(ID3D10Resource* mask, NrLink& link);
  void ReleaseMask();
  void ReleaseAll();  // destroy_device, the helper gone
  // VRAM this side made: the keyed textures (the helper's transport counts the shared ones) plus D3D11Client's own.
  [[nodiscard]] uint64_t SharedBytes() const { return color_.bytes + mask_.bytes + motion_.bytes + d3d11_->SharedBytes(); }
  [[nodiscard]] std::string Line() const;
  [[nodiscard]] std::string_view Latch() const { return d3d11_->Latch(); }  // non-empty once the transport stopped for the session
  // Plan 17: as D3D11Client::ClearLatch, the relay's latch too.
  void ClearLatch() {
    d3d11_->ClearLatch();
    relay_->ClearLatch();
  }

 private:
  D3D10Client() = default;

  using Keyed = bridge::KeyedRelay::Keyed;
  // A keyed texture that could not be made: retried only for another size or format, or after a re-enable (Plan 7 final review Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HRESULT result = S_OK;
    bool helper = false;  // the mask only: the helper could not share it (D3D11Client::CopyMask), not the relay's own keyed texture
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const {
      return size == other_size && format == other_format;
    }
  };

  [[nodiscard]] bool Stopped() const { return relay_->Stopped() || !d3d11_->Latch().empty(); }
  // The one latch is the D3D11 client's: a key that did not come in the relay stops it (the helper latches and CPU-signals its fences),
  // and its latch stops the relay's operations too. Called before the call that saw a latch returns.
  void SyncLatches(NrLink& link);

  // Declared first, so released last: the relay, then its proxy.
  std::unique_ptr<bridge::KeyedRelay> relay_;
  Microsoft::WRL::ComPtr<ID3D10Device> game_;  // the game's device, native
  std::unique_ptr<D3D11Client> d3d11_;         // on the relay's native device
  Keyed color_;
  Keyed mask_;
  Keyed motion_;
  std::optional<ShareFailure> color_failure_;
  std::optional<ShareFailure> mask_failure_;
  std::optional<ShareFailure> motion_failure_;
  std::string problem_;  // this side's problem with the back buffer, logged once per change
  bool run_sent_ = false;
};

}  // namespace uplift::client
