#pragma once

#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addon/frame_trigger.hpp"
#include "bridge/bridge_sequence.hpp"
#include "client/nr_link.hpp"
#include "ipc/protocol.hpp"
#include "nr/types.hpp"

namespace uplift::client {

// ReShade's api::resource_usage values (the pinned reshade_api_resource.hpp), so this ReShade-free client can name the states its barriers move between. The
// add-on static_asserts them (reshade_d3d12_host.cpp). general is COMMON on D3D12 and present is PRESENT; `undefined` is never used (it asserts on D3D12).
enum class D3D12Usage : uint32_t {
  RENDER_TARGET = 0x4u,
  SHADER_RESOURCE = 0xC0u,
  COPY_DEST = 0x400u,
  COPY_SOURCE = 0x800u,
  GENERAL = 0x80000000u,
  PRESENT = 0x80000000u | 0x4u | 0x800u,
};

// What the client records with (design §6). The add-on's host is ReShade's API on the present queue's immediate command list; the smoke's is a native list
// that is closed and executed at Signal. Nothing here is recorded natively on the game's own lists. Not thread-safe: the caller's lock.
class D3D12Host {
 public:
  virtual ~D3D12Host() = default;
  // The queue has an immediate command list. False: nothing is recorded or signalled.
  [[nodiscard]] virtual bool Valid() const = 0;
  virtual void Barrier(ID3D12Resource* resource, D3D12Usage before, D3D12Usage after) = 0;
  virtual void Copy(ID3D12Resource* source, ID3D12Resource* destination) = 0;  // the whole resource (copy_resource)
  virtual bool Signal(ID3D12Fence* fence, uint64_t value) = 0;                 // command_queue::signal: flushes what was recorded, then signals
  virtual bool Wait(ID3D12Fence* fence, uint64_t value) = 0;                   // command_queue::wait: no flush; everything recorded or flushed later waits
};

// Plan 12 (design §6): a 32-bit Direct3D 12 game's frames to and from NR in gitc-uplift-helper64.exe through an NrLink. D3D11Client's FENCED half with Direct3D 12 calls:
// the helper's NT textures and both fences are opened on the game's native device (OpenSharedHandle, so no ReShade event is raised), RunBridgedFrame runs unchanged
// on this side, and every command is recorded through the D3D12Host. There is no CPU-ordered fallback: Direct3D 12 always has shared fences.
//
// R17 across processes: the game queue's wait is queued only after the helper's reply said it submitted that very signal. A fence reading UINT64_MAX means the
// helper or its device is gone. Frees: D3D12 frees at once, so an opened texture or fence the game's queue may still use goes on a retire list behind a client
// fence signalled on that queue (the retire fence, CreateFence on the native device); Describe releases what the fence has passed. DestroyDevice (no host:
// ReShade may already have released its queue) waits at most 2 s for the last retire value and leaks on a timeout, never releases under the GPU.
// Not thread-safe: the add-on's lock.
class D3D12Client {
 public:
  // nullptr + `error` when the device's adapter is not NVIDIA's or cannot be read, or the retire fence cannot be made. `device` is the game's native
  // ID3D12Device (device->get_native()); the client holds a reference until DestroyDevice (or its destructor), which must come inside destroy_device.
  static std::unique_ptr<D3D12Client> Create(ID3D12Device* device, std::string* error);
  ~D3D12Client();
  D3D12Client(const D3D12Client&) = delete;
  D3D12Client& operator=(const D3D12Client&) = delete;

  [[nodiscard]] ipc::Transport Kind() const { return ipc::Transport::FENCED; }
  [[nodiscard]] LUID Luid() const { return luid_; }
  // FRAME's target for a back buffer of this description: its size, format and why NR cannot take it (multisampled, not a 2D texture, an unsupported format, a
  // share that failed, the latch). Releases what the retire fence has passed. While NR is not `running`, or the image cannot go, the shared surfaces are
  // retired. `enabled` false forgets failed shares. The colour space is the caller's.
  ipc::Target Describe(D3D12Host& host, const D3D12_RESOURCE_DESC& back_buffer, bool enabled, bool running);
  // The FRAME reply's news: opens the helper's fences and shared textures (taking every NT handle in the reply), runs the fence watchdog, and retires the shared
  // surfaces when the helper's NR is not running.
  void Apply(const ipc::Reply& reply, NrLink& link, D3D12Host& host);
  // One RUN (design §6): the back buffer (in `entry_state`: present at PRESENT, render_target in the effects) and `motion` (this frame's UPLIFT_MV, in
  // shader_resource; null for none) out, NR, back. True when NR's result reached `back_buffer`.
  bool Run(D3D12Host& host, ID3D12Resource* back_buffer, D3D12Usage entry_state, ID3D12Resource* motion, addon::TriggerPoint point, NrLink& link);
  [[nodiscard]] bool LastRunSent() const { return run_sent_; }  // the last Run put a RUN on the wire
  // At the end of the effects: UPLIFT_MASK (in shader_resource) into the shared mask, for the next Run.
  MaskCopy CopyMask(D3D12Host& host, ID3D12Resource* mask, NrLink& link);
  void ReleaseMask(D3D12Host& host);
  // The helper is gone (or the transport is): every shared surface and opened fence goes behind the retire fence. True when it held any.
  bool ReleaseAll(D3D12Host& host);
  // Stops the transport for the session ("The Direct3D 12 bridge stopped: ..."): no new wait is ever queued again, the helper's side latches and CPU-signals
  // both fences up to the waited values, and the shared surfaces go.
  void Stop(std::string_view reason, NrLink& link, D3D12Host& host);
  // destroy_device, no host: waits up to 2 s (SetEventOnCompletion) for the retire fence to reach the last value it signalled, releases everything (or leaks it
  // on a timeout), and drops the native device reference either way. `wait` false (the destructor, for a game that ends with no destroy_device) never waits: it
  // releases only what the fence has already passed, and leaks the rest.
  void DestroyDevice(bool wait = true);
  [[nodiscard]] std::string Line() const;
  [[nodiscard]] std::string_view Latch() const { return latch_; }  // non-empty once the transport stopped for the session
  [[nodiscard]] uint64_t BusySkips() const { return busy_skips_; }
  // What this side still keeps alive of the helper's textures: the opened colour, mask and motion, and the ones on the retire lists that no fence has released.
  // Zero once NR is off and the retire fence has passed (the smoke checks it).
  [[nodiscard]] uint64_t HeldBytes() const;
  // The objects (textures and fences) waiting on the retire lists.
  [[nodiscard]] size_t RetiredCount() const { return retired_.size() + unsignalled_.size(); }
  // The FRAME Describe described was put on the wire (the caller read NrLink's FrameSent after the present): a colour handle that reply, or a late one stashed
  // from an earlier FRAME, brings is opened with the size and format of that FRAME's target.
  void NoteFrameSent();

 private:
  D3D12Client() = default;

  // A helper-made texture, opened here.
  struct Shared {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };
  // A share that failed: not retried for this size and format until NR is re-enabled (Plan 7's final review Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HRESULT result = S_OK;
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const { return size == other_size && format == other_format; }
  };
  // Something the game's queue may still use that this side no longer needs: released once the retire fence reaches `value`.
  struct Retired {
    Microsoft::WRL::ComPtr<IUnknown> object;
    uint64_t value = 0u;
  };

  // OpenSharedHandle on `handle` (which the caller closes) into `shared`, retiring what it held (a newer texture of this kind replaces the older, which the game's
  // queue may still use); false with `result` on failure, and `shared` is left as it was.
  bool Open(D3D12Host& host, HANDLE handle, Shared* shared, HRESULT* result);
  // Puts `objects` behind one new retire value, signalled on the queue now (or at the next valid host).
  void Retire(D3D12Host& host, std::vector<Microsoft::WRL::ComPtr<IUnknown>> objects);
  // Empties `surfaces` (and mask_fresh_) into one Retire.
  void RetireShared(D3D12Host& host, std::initializer_list<Shared*> surfaces);
  void FreeFinished(D3D12Host& host);
  bool RunFenced(D3D12Host& host, ID3D12Resource* back_buffer, D3D12Usage entry_state, ID3D12Resource* motion, ipc::Run run, NrLink& link);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  LUID luid_ = {};
  // The helper's two fences, opened on this device. to12 is signalled here (odd values) and waited on by the helper's queue; to11 is signalled by the helper's queue
  // and waited on here.
  Microsoft::WRL::ComPtr<ID3D12Fence> to12_;
  Microsoft::WRL::ComPtr<ID3D12Fence> to11_;
  Microsoft::WRL::ComPtr<ID3D12Fence> retire_fence_;  // signalled on the game's queue at every retire
  uint64_t retire_value_ = 0u;                        // the newest value handed out (signalled, unless the signal failed)
  std::vector<Retired> retired_;
  std::vector<Microsoft::WRL::ComPtr<IUnknown>> unsignalled_;  // retired while the host had no queue: signalled at the next valid host
  Shared color_;
  Shared mask_;
  Shared motion_;
  bool mask_fresh_ = false;  // CopyMask wrote mask_ since the last RUN
  bridge::BridgeWatchdog watchdog_;
  uint64_t last_value_ = 0u;   // the newest fence value handed out
  uint64_t waited11_ = 0u;     // the highest to11 value a queue wait was queued for
  uint64_t pending_out_ = 0u;  // a RUN whose reply never came: its `out`, until to11 reaches it (or the helper is gone)

  // What the last SHARE_MASK and SHARE_MOTION asked for: a reply that lands late is opened in Apply, and a failure there is recorded for this size and format
  // (the ones CopyMask and Run look up), not the back buffer's.
  struct ShareAsk {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };
  // The back buffer's size and format as the FRAME carries them, and whether that FRAME can make a colour at all (its target has no problem). NoteFrameSent copies
  // it into color_ask_ when the FRAME went out: a late reply's colour handle, stashed and delivered with a later FRAME's reply, is still the colour of the FRAME that
  // made it, and a failure to open it is recorded for that size and format (the ones Describe looks up), not for the present that delivers it.
  ShareAsk frame_ask_;
  bool frame_asks_colour_ = false;
  ShareAsk color_ask_;
  ShareAsk mask_ask_;
  nr::Size motion_ask_size_;
  std::optional<ShareFailure> color_failure_;
  std::optional<ShareFailure> mask_failure_;
  std::optional<ShareFailure> motion_failure_;
  std::string problem_;  // the back buffer's problem, logged once per change
  std::string latch_;
  bool in_flight_ = false;  // a request has no reply yet: no share is touched until a FRAME reply
  bool run_sent_ = false;
  uint64_t busy_skips_ = 0u;
};

}  // namespace uplift::client
