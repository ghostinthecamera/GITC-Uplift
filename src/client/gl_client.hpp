#pragma once

#include <Windows.h>

#include <dxgiformat.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "addon/frame_trigger.hpp"
#include "bridge/bridge_sequence.hpp"
#include "client/nr_link.hpp"
#include "gl/format.hpp"
#include "gl/functions.hpp"
#include "gl/game_host.hpp"
#include "gl/interop.hpp"
#include "ipc/protocol.hpp"
#include "nr/types.hpp"

namespace uplift::client {

// Plan 12 (OpenGL design §5): an OpenGL context's frames to and from NR in gitc-uplift-helper64.exe through an NrLink. VkClient's logic on the GL game side (src/gl/):
// FENCED when the context offers GL_EXT_semaphore_win32 (the helper's two D3D12 fences imported as GL semaphores, RunBridgedFrame running unchanged on this
// side), SHARED_CPU otherwise or when the import is refused (the same NT textures with no fences: the copy in, a capped GLsync wait, RUN, whose reply comes at
// completion, then the copy out). Every copy goes through the GameHost (ReShade's copy_texture_region in the add-on, a native copy in the smokes); the
// imports, semaphores and frees through gl::Interop. Not thread-safe: the add-on's lock.
//
// What GL cannot do is read a semaphore (design §5, R70): where VkClient reads its imported fences, this client reads the helper's own view of them, which
// every reply carries (Reply::to12_completed and to11_completed; UINT64_MAX is the helper's device removed). R17 across processes is unchanged: a GL wait is
// queued only after the helper's reply said it submitted that very signal. Every CPU wait is capped at 2 s; a timeout skips that frame and three in a row stop
// the client for the session.
//
// Every GL call is made with the runtime's context current (GameHost::Valid()). While it is not, nothing is imported, and the NT handles a reply brought are
// still taken and closed once (the helper's two fence handles are kept, not closed, until a valid frame imports them: they are handed out only once). Frees wait
// for a valid frame too, and destroy_device is ForgetGl: no GL call at all (design §3.6).
class GlClient {
 public:
  // `functions`: gl::Functions::Load's (the runtime's context current); `allow_fences` false forces SHARED_CPU (the smoke uses it to test it on a context that
  // could do both).
  GlClient(gl::Functions functions, LUID luid, bool allow_fences);
  GlClient(const GlClient&) = delete;
  GlClient& operator=(const GlClient&) = delete;
  // Frees nothing, and makes no GL call: ForgetGl (destroy_device) or ReleaseAll (the helper is gone) first. Closes a fence handle it still holds.
  ~GlClient();

  [[nodiscard]] ipc::Transport Kind() const { return fenced_ ? ipc::Transport::FENCED : ipc::Transport::SHARED_CPU; }
  [[nodiscard]] LUID Luid() const { return luid_; }
  [[nodiscard]] const gl::Functions& Gl() const { return interop_.Gl(); }
  // FRAME's target for `back_buffer` (FB0 at PRESENT; ReShade's intermediate at the technique): its size, the shared format (gl::SharedFormatOf, never ReShade's
  // label) and why NR cannot take it (a multisampled FB0 at PRESENT, an unsupported format, another context than the runtime's, a share that failed, the
  // latch). Frees what can be freed. `at_present`: NR would run at PRESENT (no marker), where MSAA is refused. `enabled` false forgets failed shares.
  ipc::Target Describe(gl::GameHost& host, const gl::ImageInfo& back_buffer, bool at_present, bool enabled, bool running);
  // The FRAME reply's news: takes every NT handle in the reply, imports the helper's fences and shared textures while the host is valid, runs the fence
  // watchdog on the reply's fence values, and drops the shared surfaces when the helper's NR is not running. True when the transport changed: the driver
  // refused the fence import, so this client is SHARED_CPU now (Kind() says so) and the caller must DETACH and ATTACH afresh with it.
  bool Apply(const ipc::Reply& reply, NrLink& link, gl::GameHost& host);
  // The FRAME Describe described was put on the wire (the caller read NrLink's FrameSent after the present): a colour handle that reply, or a late one stashed
  // from an earlier FRAME, brings is imported with the size and format of that FRAME's target.
  void NoteFrameSent();
  // One RUN: `frame` (FB0 at PRESENT, the event's rtv resource at the technique and after the effects) and `motion` (this frame's UPLIFT_MV: an empty info for
  // none) out, NR, back. True when NR's result reached `frame`.
  bool Run(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, addon::TriggerPoint point, NrLink& link);
  [[nodiscard]] bool LastRunSent() const { return run_sent_; }  // the last Run put a RUN on the wire
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run.
  MaskCopy CopyMask(gl::GameHost& host, const gl::ImageInfo& mask, NrLink& link);
  void ReleaseMask(gl::GameHost& host);
  // The helper is gone (or this transport is): every shared surface and imported fence goes (deleted now, or at the next valid frame). True when it held any.
  bool ReleaseAll(gl::GameHost& host);
  // destroy_device (design §3.6): every GL name forgotten WITHOUT a GL call (they die with the share group).
  void ForgetGl();
  // What this side imported (the helper's transport counts the same textures as its own VRAM, so the overlay never adds it).
  [[nodiscard]] uint64_t ImportedBytes() const { return color_.bytes + mask_.bytes + motion_.bytes; }
  [[nodiscard]] std::string Line() const;
  [[nodiscard]] std::string_view Latch() const { return latch_; }  // non-empty once the client stopped for the session
  [[nodiscard]] uint64_t BusySkips() const { return busy_skips_; }

 private:
  // A helper-made texture, imported here.
  struct Shared {
    gl::SharedTexture gl;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
    // What host.Copy takes for the shared texture.
    [[nodiscard]] gl::ImageInfo Image() const { return {.handle = gl.Handle(), .size = size, .format = format, .samples = 1u}; }
  };
  // A share that failed: not retried for this size and format until NR is re-enabled (Plan 7's final review Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string what;  // the failing GL call and its error
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const { return size == other_size && format == other_format; }
  };
  // What the last SHARE_MASK and SHARE_MOTION asked for: a reply that lands late is imported in Apply, and a failure there is recorded for this size and
  // format (the ones CopyMask and Run look up), not the back buffer's.
  struct ShareAsk {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };

  // Imports `handle` (which the caller closes; `allocation_bytes` is the helper's GetResourceAllocationInfo size) into `shared`, retiring what it held. False,
  // with `failure` set, when the host is not valid or the import failed.
  bool Import(gl::GameHost& host, HANDLE handle, uint64_t allocation_bytes, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, Shared* shared,
              std::string* failure);
  void Retire(gl::GameHost& host, Shared* shared);
  void ReleaseShared(gl::GameHost& host);  // colour, mask and motion
  void CloseHeldFences();
  // Stops the client for the session ("The OpenGL bridge stopped: ..."): no new wait is ever queued again, the helper's side latches and CPU-signals both
  // fences up to the waited values, and the shared surfaces go.
  void Stop(std::string_view reason, NrLink& link, gl::GameHost& host);
  // A capped wait ran its full 2 s: counted in `consecutive`; the third in a row stops the client.
  void NoteTimeout(uint32_t* consecutive, std::string_view what, NrLink& link, gl::GameHost& host);
  bool RunFenced(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ipc::Run run, NrLink& link);
  bool RunCpuOrdered(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ipc::Run run, NrLink& link);
  // The UPLIFT_MV to copy in this frame: shares the motion texture for `motion` when it needs one (retired when a recording comes without one). The source to
  // copy, or an empty info (no motion, or a share that failed). A share whose reply is late sets `in_flight_`: the frame must not go on.
  gl::ImageInfo PrepareMotion(gl::GameHost& host, const gl::ImageInfo& motion, NrLink& link);

  gl::Interop interop_;
  LUID luid_ = {};
  bool fenced_ = false;
  std::string cpu_reason_;  // why SHARED_CPU, when it is (design §7's reason)
  // FENCED: the helper's two fences, imported. to12 is signalled here (odd values) and waited on by the helper's queue; to11 is signalled by the helper's queue
  // and waited on here. The NT handles are held (not closed) from the reply that brought them until a valid frame imported them.
  GLuint to12_gl_ = 0u;
  GLuint to11_gl_ = 0u;
  HANDLE held_to12_ = nullptr;
  HANDLE held_to11_ = nullptr;
  Shared color_;
  Shared mask_;
  Shared motion_;
  bool mask_fresh_ = false;  // CopyMask wrote mask_ since the last RUN
  bridge::BridgeWatchdog watchdog_;
  uint64_t last_value_ = 0u;           // the newest fence value handed out
  uint64_t waited11_ = 0u;             // the highest to11 value a GL wait was queued for
  uint64_t pending_out_ = 0u;          // a RUN whose reply never came: its `out`, until the helper's to11 reaches it (or the helper is gone)
  uint64_t helper_to11_ = 0u;          // the helper's to11 completed value in the newest reply (UINT64_MAX: its device was removed)

  // The back buffer's target as the FRAME carries it (size, and the format ReShade reports), and whether that FRAME can make a colour at all (its target has no
  // problem). NoteFrameSent copies it into color_ask_ when the FRAME went out: a late reply's colour handle, stashed and delivered with a later FRAME's reply, is
  // still the colour of the FRAME that made it, not of the present that delivers it.
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
  uint32_t cpu_timeouts_ = 0u;  // consecutive 2 s timeouts of SHARED_CPU's copy-in wait
  uint64_t busy_skips_ = 0u;
};

}  // namespace uplift::client
