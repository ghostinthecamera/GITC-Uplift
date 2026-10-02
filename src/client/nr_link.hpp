#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string_view>

#include "ipc/protocol.hpp"
#include "nr/types.hpp"

namespace uplift::client {

// CopyMask's result, for every API's client (D3D11Bridge::MaskCopy's, without the D3D10 relay).
struct MaskCopy {
  std::optional<nr::Size> size;  // copied: the next RUN hands it to NR
  bool unsupported = false;      // NR cannot read this format, or this transport shares no mask; any other failure is a share that failed
  bool busy = false;             // the helper still works on an earlier frame: nothing was touched, try again next frame
  bool relay_failed = false;     // Plan 10: that share was the Direct3D 10 relay's own keyed one (D3D10Client), not the helper's
  uint32_t d3d9_format = 0u;     // Plan 10: the D3DFORMAT `unsupported` names (a Direct3D 9 mask); 0 elsewhere
};

// Plan 10 (design §5): the bitness of the add-on this client is built into, for the texts both add-ons show. gitc-uplift.addon32 keeps its
// Plan 9 texts byte for byte; gitc-uplift.addon64 runs the same clients for Direct3D 9 games.
inline constexpr bool IS_32_BIT = (sizeof(void*) == 4u);
inline constexpr std::string_view BITNESS = (IS_32_BIT ? "32-bit" : "64-bit");

// Plan 9 (design §2.5): the add-on side's one view of NR. It has one implementation, RemoteNr (the IPC to the 64-bit helper): a 64-bit
// Direct3D 9 game uses the helper too (Plan 10), so there is no in-process link. The clients never wait on anything but a link's own
// capped calls.
class NrLink {
 public:
  virtual ~NrLink() = default;
  // RUN at the trigger point: nullopt when the reply did not come within 2 s (the request stays pending) or the helper is
  // gone. The reply of a BLOCK or KMT RUN is set when the helper's GPU work is done, so its shared memory is free again.
  virtual std::optional<ipc::Reply> Run(const ipc::Run& run) = 0;
  // The last Run() put its request on the wire (the helper has seen its point, replied or not); false when it was refused
  // because another request is still outstanding or the helper is not attached.
  [[nodiscard]] virtual bool RunSent() const = 0;
  // `signalled` of a RUN whose reply landed after its 2 s cap, once (nullopt when none did). A FENCED client waits for the fence
  // value of a RUN it stopped waiting for; when the helper says it never signalled, nothing is outstanding.
  virtual std::optional<bool> TakeLateRunSignalled() = 0;
  // SHARE_MASK or SHARE_MOTION (FENCED): the helper makes the shared texture and hands its NT handle back in the reply.
  virtual std::optional<ipc::Reply> Share(ipc::RequestKind kind, const ipc::Share& share) = 0;
  // Takes over an NT handle by its value in the helper's table (the helper's copy closes in the same call). The caller
  // closes `local`.
  virtual bool Pull(uint64_t remote, HANDLE* local) = 0;
  // FENCED: the add-on's watchdog tripped or a transport failed: latch the helper's transport and CPU-signal its fences up to
  // `waited11`. Best effort: it is sent only when no request is outstanding.
  virtual void Stop(uint64_t waited11, std::string_view reason) = 0;
};

}  // namespace uplift::client
