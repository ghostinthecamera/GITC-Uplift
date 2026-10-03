#pragma once

#include <Windows.h>

#include <d3d11_4.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "addon/frame_trigger.hpp"
#include "bridge/bridge_sequence.hpp"
#include "client/nr_link.hpp"
#include "ipc/protocol.hpp"
#include "nr/types.hpp"

namespace uplift::client {

// Plan 9 (design §2.3, §2.4): a D3D11 device's frames to and from NR through an NrLink. FENCED on a device with
// ID3D11Device5 and ID3D11DeviceContext4 (Plan 7's NT textures and two shared fences, split across the IPC: RunBridgedFrame
// runs unchanged on this side), KMT otherwise (a legacy shared texture, CPU-ordered). ReShade-free: it takes native
// pointers and calls only CopyResource, Flush, OpenSharedResource1, OpenSharedFence, Signal/Wait on the context 4,
// CreateTexture2D and queries. Not thread-safe: the add-on's lock.
//
// R17 across processes: a D3D11 GPU wait is queued only after the helper's reply said it submitted that very signal. A reply
// that never came (or a helper that died) means no wait is ever queued; a fence reading UINT64_MAX means its owner is gone.
// KMT's one CPU wait (an event query) is capped at 2 s: a frame whose GPU work outlasts it is skipped, and the frames after it
// are skipped until that work is done (nothing is latched).
class D3D11Client {
 public:
  // nullptr + `error` when the device's adapter is not NVIDIA's or cannot be read. `allow_fences` false forces KMT (the smoke
  // uses it once to test the fallback). Plan 10: `label` names the game's API in the latch and the multisample texts ("Direct3D 10"
  // when the device is D3D10Client's relay).
  static std::unique_ptr<D3D11Client> Create(ID3D11Device* device, bool allow_fences, std::string* error,
                                             std::string_view label = "Direct3D 11");
  ~D3D11Client();
  D3D11Client(const D3D11Client&) = delete;
  D3D11Client& operator=(const D3D11Client&) = delete;

  [[nodiscard]] ipc::Transport Kind() const { return fenced_ ? ipc::Transport::FENCED : ipc::Transport::KMT; }
  [[nodiscard]] LUID Luid() const { return luid_; }
  // FRAME's target for `back_buffer`: its size, format and why NR cannot take it (a multisampled or unsupported format, a share
  // that failed, the latch). On KMT, while `running`, it also makes the shared texture. `enabled` false forgets failed shares,
  // so re-enabling retries them. The colour space is the caller's.
  ipc::Target Describe(ID3D11Resource* back_buffer, bool enabled, bool running);
  // Plan 10: the same from a description (nullopt: not a 2D texture). D3D10Client has no D3D11 texture to describe while NR is off, but the
  // helper's Session still loads from the back buffer's size and format (Plan 8's Minor 2).
  ipc::Target Describe(const std::optional<D3D11_TEXTURE2D_DESC>& description, bool enabled, bool running);
  // The FRAME reply's news: opens the helper's fences and shared textures (taking every NT handle in the reply), runs the
  // fence watchdog, and drops the shared surfaces when the helper's NR is not running. True when the transport changed: the
  // device could not open the helper's fences, so this client is KMT now (Kind() says so) and the caller must DETACH and ATTACH
  // afresh with it.
  bool Apply(const ipc::Reply& reply, NrLink& link);
  // One RUN (design §2.4): the back buffer (and `motion`, this frame's UPLIFT_MV, FENCED only) out, NR, back. True when NR's
  // result reached `back_buffer`.
  bool Run(ID3D11Resource* back_buffer, ID3D11Resource* motion, addon::TriggerPoint point, NrLink& link);
  [[nodiscard]] bool LastRunSent() const { return run_sent_; }  // the last Run put a RUN on the wire
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run (FENCED only).
  MaskCopy CopyMask(ID3D11Resource* mask, NrLink& link);
  void ReleaseMask();
  // destroy_device, the helper gone. True when it released something (a shared texture, a fence or the query): the D3D11 context frees those at
  // its next Flush, which D3D10Client makes only then (batch 3 review, minor 4).
  bool ReleaseAll();
  // Stops the transport for the session ("The {label} bridge stopped: ..."): no new wait is ever queued again, the helper's side latches
  // and CPU-signals both fences up to the waited values, and the shared surfaces go. Plan 10: public, so a failure of the D3D10 relay
  // under this client (a key that did not come) latches it too.
  void Stop(std::string_view reason, NrLink& link);
  // VRAM this side made. FENCED: none (the helper's transport counts the shared textures both sides map).
  [[nodiscard]] uint64_t SharedBytes() const { return kmt_bytes_; }
  [[nodiscard]] std::string Line() const;
  [[nodiscard]] std::string_view Latch() const { return latch_; }  // non-empty once the transport stopped for the session
  // Plan 17: Retry now after the helper stopped (the front's 2-strike rule allows it): a latch its stop caused (its fences read UINT64_MAX) goes with it.
  void ClearLatch() { latch_.clear(); }
  [[nodiscard]] uint64_t BusySkips() const { return busy_skips_; }

 private:
  D3D11Client() = default;

  // A helper-made texture, opened here.
  struct Shared {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
  };
  // A share that failed: not retried for this size and format until NR is re-enabled (Plan 7's final review Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HRESULT result = S_OK;
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const {
      return size == other_size && format == other_format;
    }
  };

  // OpenSharedResource1 on `handle` (which the caller closes); false with `result` on failure.
  bool Open(HANDLE handle, Shared* shared, HRESULT* result);
  void ReleaseShared();  // color, mask and motion (the D3D11 side frees them at its next Flush)

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;  // immediate
  Microsoft::WRL::ComPtr<ID3D11Device5> device5_;        // FENCED only
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
  bool fenced_ = false;
  LUID luid_ = {};

  // FENCED: the helper's two fences, opened on this device. to12 is signalled here (odd values) and waited on by the helper's
  // queue; to11 is signalled by the helper's queue and waited on here.
  Microsoft::WRL::ComPtr<ID3D11Fence> to12_11_;
  Microsoft::WRL::ComPtr<ID3D11Fence> to11_11_;
  Shared color_;
  Shared mask_;
  Shared motion_;
  bool mask_fresh_ = false;  // CopyMask wrote mask_ since the last RUN
  bridge::BridgeWatchdog watchdog_;
  uint64_t last_value_ = 0u;   // the newest fence value handed out
  uint64_t waited11_ = 0u;     // the highest to11 value a D3D11 wait was queued for
  uint64_t pending_out_ = 0u;  // a RUN whose reply never came: its `out`, until to11 reaches it (or the helper is gone)

  // KMT: this side's shared texture, opened by the helper through its global handle.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> kmt_texture_;
  HANDLE kmt_handle_ = nullptr;
  uint32_t kmt_generation_ = 0u;
  nr::Size kmt_size_;
  DXGI_FORMAT kmt_format_ = DXGI_FORMAT_UNKNOWN;
  uint64_t kmt_bytes_ = 0u;
  Microsoft::WRL::ComPtr<ID3D11Query> query_;

  nr::Size described_size_;  // the last Describe's back buffer, for the failures Apply records
  DXGI_FORMAT described_format_ = DXGI_FORMAT_UNKNOWN;
  // What the last SHARE_MASK and SHARE_MOTION asked for: a reply that lands late is opened in Apply, and a failure there is recorded
  // for this size and format (the ones CopyMask and Run look up), not the back buffer's.
  struct ShareAsk {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };
  ShareAsk mask_ask_;
  nr::Size motion_ask_size_;
  std::optional<ShareFailure> color_failure_;
  std::optional<ShareFailure> mask_failure_;
  std::optional<ShareFailure> motion_failure_;
  std::string problem_;  // the back buffer's problem, logged once per change
  std::string latch_;
  std::string label_ = "Direct3D 11";  // Plan 10: Create's label
  bool in_flight_ = false;             // a request has no reply yet: no share is touched until a FRAME reply
  bool gpu_pending_ = false;           // KMT: the event query's 2 s ran out; frames skip until the copy into the shared texture is done
  bool run_sent_ = false;
  bool floor_probed_ = false;  // KMT: a frame below NR's floor already sent its one RUN
  uint64_t busy_skips_ = 0u;
};

}  // namespace uplift::client
