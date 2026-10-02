#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "addon/device_context.hpp"
#include "bridge/d3d12_side.hpp"
#include "ipc/protocol.hpp"

namespace uplift::helper {

// Plan 9 (design §2.3, §2.4): how one game API's frame crosses into the helper and back, on the helper's D3D12Side. One
// transport per attach: BLOCK (plain D3D9, a shared-memory block opened as a D3D12 heap), KMT (D3D9Ex, and D3D11 without
// fences: a legacy shared texture, CPU-ordered by the add-on) and FENCED (D3D11.4: NT textures and two shared fences).
// Not thread-safe: the helper's one thread.
class Transport {
 public:
  virtual ~Transport() = default;

  // First thing at every FRAME: frees what the queue has passed, then describes this frame's target. While `running` (NR's
  // state is not OFF) it also makes the helper-side texture NR runs on, or opens the add-on's; otherwise, and after a
  // latch, it retires them (Plan 7 final review C-1 and Minor 2). New NT handles go into `handles`. `problem` in the result
  // views a string this transport owns until its next call. It is reported while NR is off too (the latch, the add-on's own
  // problem, then a share failure of this size), so the Session does not load for an image this side cannot take and unload
  // again; a share failure is forgotten only when the user's `enabled` is off (as D3D11Bridge does).
  virtual addon::TargetInfo Describe(const ipc::Target& target, bool enabled, bool running, ipc::Handles* handles) = 0;
  // One RUN (the point already replayed): records NR on the shared frame and fills `reply` (`waited`, `submitted`, `wrote`,
  // `signalled`, `busy`). `complete_at` stays 0 when the reply is final now. BLOCK and KMT set it to the `progress` value
  // their list signals: the caller sets the reply event when the GPU has passed it, and only after it has written the whole
  // reply, so the add-on never reads a reply that is still being written.
  virtual void Run(const ipc::Run& run, addon::DeviceContext& context, ipc::Reply* reply, uint64_t* complete_at) = 0;
  // UPLIFT_MASK went away (no enabled effect writes it), so the shared mask is retired (every transport, Plan 10).
  virtual void ReleaseMask() {}
  // SHARE_MASK and SHARE_MOTION (Plan 10: every transport): FENCED makes the shared texture and hands its NT handle back, BLOCK makes a
  // block (a file mapping opened as a heap) and hands its mapping, size and row pitch back, KMT opens the add-on's shared texture.
  virtual void Share(ipc::RequestKind kind, const ipc::Share& share, ipc::Reply* reply);
  // Plan 12: the two shared fences' completed values right now, for every reply (Reply::to12_completed and to11_completed). FENCED reads them
  // (UINT64_MAX once its device was removed); every other transport has no fences and answers 0 and 0.
  virtual void FenceValues(uint64_t* to12, uint64_t* to11) const {
    *to12 = 0u;
    *to11 = 0u;
  }
  // The add-on's watchdog tripped, or its device is going: latches this transport with `reason`. FENCED also signals both
  // shared fences from the CPU, up to the highest value a wait on them was queued for.
  virtual void Stop(uint64_t waited11, std::string_view reason) = 0;
  // DETACH: retires every surface and drops what was opened from the add-on. The next ATTACH makes a new transport.
  virtual void Detach() = 0;
  [[nodiscard]] virtual uint64_t SurfaceBytes() const = 0;
  [[nodiscard]] virtual std::string Line() const = 0;  // "frames cross through shared memory", ...
};

// The transport `kind` names, on `side`; nullptr with `error` set for NONE, or when its objects could not be made.
[[nodiscard]] std::unique_ptr<Transport> MakeTransport(ipc::Transport kind, bridge::D3D12Side& side, std::string* error);

}  // namespace uplift::helper
