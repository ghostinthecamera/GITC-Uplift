#pragma once

#include <Windows.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "addon/frame_trigger.hpp"
#include "bridge/bridge_sequence.hpp"
#include "client/nr_link.hpp"
#include "ipc/protocol.hpp"
#include "nr/types.hpp"
#include "vk/frame.hpp"
#include "vk/game_host.hpp"
#include "vk/interop.hpp"
#include "vk/loader.hpp"

namespace uplift::client {

// Plan 11 (Vulkan design §5): a Vulkan device's frames to and from NR in gitc-uplift-helper64.exe through an NrLink. D3D11Client's logic on the Vulkan game side
// (src/vk/): FENCED when the device has both Win32 extensions and timeline semaphores (the helper's two D3D12 fences imported as timeline
// semaphores, RunBridgedFrame running unchanged on this side), SHARED_CPU otherwise (the same NT textures with no fences: the copy in, a capped CPU
// wait, RUN, whose reply comes at completion, then the copy out). Every Vulkan command goes through the GameHost (ReShade's API in the add-on, a native
// command buffer in the smokes); the imports and frees through vk::Interop. Not thread-safe: the add-on's lock.
//
// R17 across processes: a Vulkan wait is queued only after the helper's reply said it submitted that very signal. A counter reading UINT64_MAX means the
// helper or its device is gone. Every CPU wait is capped at 2 s; a timeout skips that frame and three in a row stop the client for the session.
// destroy_device: FreeVulkan only (R62).
class VkClient {
 public:
  // `device`: the game's Vulkan device (vk::Device::Open), with its record in `device.record`; `allow_fences` false forces SHARED_CPU (the smoke uses it
  // to test it on a device that could do both).
  VkClient(vk::Device device, LUID luid, bool allow_fences);
  VkClient(const VkClient&) = delete;
  VkClient& operator=(const VkClient&) = delete;
  // Frees nothing: FreeVulkan first (destroy_device) or ReleaseAll (the helper is gone), which are the callers that know a host.
  ~VkClient() = default;

  [[nodiscard]] ipc::Transport Kind() const { return fenced_ ? ipc::Transport::FENCED : ipc::Transport::SHARED_CPU; }
  [[nodiscard]] LUID Luid() const { return luid_; }
  [[nodiscard]] const vk::Device& VulkanDevice() const { return interop_.Functions(); }
  // FRAME's target for `back_buffer`: its size, format and why NR cannot take it (no copy access, multisampled, an unsupported format, a share that
  // failed, the latch). While `running` and usable, it also records the opening global barrier into the present event's immediate list (batch 2 review
  // I-1 (a)). Frees what the queue has passed. `enabled` false forgets failed shares. The colour space is the caller's.
  ipc::Target Describe(vk::GameHost& host, const vk::ImageInfo& back_buffer, bool enabled, bool running);
  // The FRAME reply's news: imports the helper's fences and shared textures (taking every NT handle in the reply), runs the fence watchdog, and drops the
  // shared surfaces when the helper's NR is not running. True when the transport changed: the device could not import the helper's fences, so this client
  // is SHARED_CPU now (Kind() says so) and the caller must DETACH and ATTACH afresh with it.
  bool Apply(const ipc::Reply& reply, NrLink& link, vk::GameHost& host);
  // The FRAME Describe described was put on the wire (the caller read NrLink's FrameSent after the present): a colour handle that reply, or a late one
  // stashed from an earlier FRAME, brings is imported with the size and format of that FRAME's target (batch 3 review, minor 1).
  void NoteFrameSent();
  void NoteSecondQueue() { second_queue_ = true; }
  // One RUN: the back buffer (and `motion`, this frame's UPLIFT_MV: an empty info for none) out, NR, back. True when NR's result reached `back_buffer`.
  // `entry_state`: the back buffer's state in the calling event. `present_host`: design §3.4 case 2, when the game presents from a queue other than the
  // one Uplift records on: that queue is flushed and waited for on the CPU (2 s), once per frame (Describe starts a frame), after the skip checks and
  // before the first thing that could submit work (the copy in, or a motion share's retire fence, which flushes the immediate list); this frame goes
  // without NR when that timed out or failed, and three timeouts in a row stop the client.
  bool Run(vk::GameHost& host, const vk::ImageInfo& back_buffer, vk::Usage entry_state, const vk::ImageInfo& motion, addon::TriggerPoint point,
           NrLink& link, vk::GameHost* present_host = nullptr);
  [[nodiscard]] bool LastRunSent() const { return run_sent_; }  // the last Run put a RUN on the wire
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run.
  MaskCopy CopyMask(vk::GameHost& host, const vk::ImageInfo& mask, NrLink& link);
  void ReleaseMask(vk::GameHost& host);
  // The helper is gone (or this transport is): every shared surface and imported fence goes behind a fence of the queue. True when it held any.
  bool ReleaseAll(vk::GameHost& host);
  // destroy_device (R62): every Vulkan object, through ReShade's API for the images and the functions its layer does not intercept for the rest. `host`
  // needs only the device.
  void FreeVulkan(vk::GameHost& host);
  // What this side imported (the helper's transport counts the same textures as its own VRAM, so the overlay never adds it).
  [[nodiscard]] uint64_t ImportedBytes() const { return color_.bytes + mask_.bytes + motion_.bytes; }
  [[nodiscard]] std::string Line() const;
  [[nodiscard]] std::string_view Latch() const { return latch_; }  // non-empty once the client stopped for the session
  // Plan 17: Retry now after the helper stopped (the front's 2-strike rule allows it): a latch its stop caused (its fences read UINT64_MAX) goes with it.
  void ClearLatch() { latch_.clear(); }
  [[nodiscard]] uint64_t BusySkips() const { return busy_skips_; }

 private:
  // A helper-made texture, imported here.
  struct Shared {
    vk::SharedImage vulkan;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
  };
  // A share that failed: not retried for this size and format until NR is re-enabled (Plan 7's final review Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string what;  // the failing Vulkan step and its result
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const { return size == other_size && format == other_format; }
  };
  // What the last SHARE_MASK and SHARE_MOTION asked for: a reply that lands late is imported in Apply, and a failure there is recorded for this size and
  // format (the ones CopyMask and Run look up), not the back buffer's.
  struct ShareAsk {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };

  // Imports `handle` (which the caller closes) into `shared`, retiring what it held. False, with `failure` set, when the import failed.
  bool Import(vk::GameHost& host, HANDLE handle, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, Shared* shared, std::string* failure);
  void Retire(vk::GameHost& host, Shared* shared);
  void ReleaseShared(vk::GameHost& host);  // colour, mask and motion
  // Stops the client for the session ("The Vulkan bridge stopped: ..."): no new wait is ever queued again, the helper's side latches and CPU-signals both
  // fences up to the waited values, and the shared surfaces go.
  void Stop(std::string_view reason, NrLink& link, vk::GameHost& host);
  // A capped wait ran its full 2 s: counted in `consecutive`; the third in a row stops the client.
  void NoteTimeout(uint32_t* consecutive, std::string_view what, NrLink& link, vk::GameHost& host);
  // This frame's present-queue wait (design §3.4 case 2), once: true when the frame may go on (no second queue, or the wait is done).
  bool SyncPresentQueue(vk::GameHost* present_host, vk::GameHost& host, NrLink& link);
  bool RunFenced(vk::GameHost& host, const vk::ImageInfo& back_buffer, vk::Usage entry_state, const vk::ImageInfo& motion, ipc::Run run,
                 NrLink& link, vk::GameHost* present_host);
  bool RunCpuOrdered(vk::GameHost& host, const vk::ImageInfo& back_buffer, vk::Usage entry_state, const vk::ImageInfo& motion, ipc::Run run,
                     NrLink& link, vk::GameHost* present_host);
  // The UPLIFT_MV to copy in this frame: shares the motion texture for `motion` when it needs one (retired when a recording comes without one). The
  // source to copy, or an empty info (no motion, or a share that failed). A share whose reply is late sets `in_flight_`: the frame must not go on.
  vk::ImageInfo PrepareMotion(vk::GameHost& host, const vk::ImageInfo& motion, NrLink& link);

  vk::Interop interop_;
  LUID luid_ = {};
  bool fenced_ = false;
  std::string cpu_reason_;  // why SHARED_CPU, when it is (design §8's reason)
  // FENCED: the helper's two fences, imported. to12 is signalled here (odd values) and waited on by the helper's queue; to11 is signalled by the helper's
  // queue and waited on here.
  VkSemaphore to12_vk_ = VK_NULL_HANDLE;
  VkSemaphore to11_vk_ = VK_NULL_HANDLE;
  Shared color_;
  Shared mask_;
  Shared motion_;
  bool mask_fresh_ = false;  // CopyMask wrote mask_ since the last RUN
  bridge::BridgeWatchdog watchdog_;
  uint64_t last_value_ = 0u;   // the newest fence value handed out
  uint64_t waited11_ = 0u;     // the highest to11 value a Vulkan wait was queued for
  uint64_t pending_out_ = 0u;  // a RUN whose reply never came: its `out`, until to11 reaches it (or the helper is gone)

  // The back buffer's target as the FRAME carries it (size, and the format ReShade reports), and whether that FRAME can make a colour at all (its target
  // has no problem). NoteFrameSent copies it into color_ask_ when the FRAME went out: a late reply's colour handle, stashed and delivered with a later
  // FRAME's reply, is still the colour of the FRAME that made it, not of the present that delivers it (a FRAME with a problem never replaces the ask).
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
  bool floor_probed_ = false;  // SHARED_CPU: a frame below NR's floor already sent its one RUN
  bool second_queue_ = false;
  bool present_synced_ = false;           // this frame's present-queue wait is done; Describe clears it
  uint32_t present_queue_timeouts_ = 0u;  // consecutive 2 s timeouts of the present-queue wait
  uint32_t cpu_timeouts_ = 0u;            // consecutive 2 s timeouts of SHARED_CPU's copy-in wait
  uint64_t busy_skips_ = 0u;
};

}  // namespace uplift::client
