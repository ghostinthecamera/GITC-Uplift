#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "bridge/bridge_sequence.hpp"
#include "bridge/d3d11_bridge.hpp"
#include "bridge/d3d12_side.hpp"
#include "gl/format.hpp"
#include "gl/functions.hpp"
#include "gl/game_host.hpp"
#include "gl/interop.hpp"
#include "nr/types.hpp"

namespace uplift::bridge {

// Plan 12 (OpenGL design §4): a private D3D12 device on an OpenGL context's NVIDIA adapter, the frame's shared copy, and the per-recording hand-off NR runs
// inside. VkBridge's shape with a GL game side (src/gl/), on the same D3D12Side, RunBridgedFrame, BridgeWatchdog and RecordNr.
// - GPU-ordered (GL_EXT_semaphore_win32): the two shared D3D12 fences are imported as GL semaphores and the frame is design §3.4's table. CPU-ordered (a
//   driver without it, or one that refused the import): the copy in, a capped GLsync wait, NR, a capped CPU wait, the copy out; no fence exists and nothing
//   waits on the GPU.
// - Every GL call goes through the GameHost (ReShade's API in the add-on) or the src/gl/ Interop, always with the runtime's context current (GameHost::Valid()).
//   Uplift makes no wgl* call and no GL call at destroy_device. Every CPU wait is capped at 2 s; a timeout skips that frame, and three in a row stop the bridge.
// - The frame is FB0 at the present event and ReShade's intermediate (the event's rtv resource) at the technique and after the effects (design §3.2).
// - Not thread-safe: the add-on's lock.
class GlBridge {
 public:
  // nullptr, with `error` set, when the adapter is not NVIDIA's or the D3D12 side cannot be made. `functions` are Functions::Load's (the context current now:
  // the fences are imported here); a driver without semaphores, or one that refuses the fence import, still gets a bridge that builds CPU-ordered and
  // StatusLine says why. `created` receives the device as D3D12CreateDevice returned it, whether or not this succeeds (ReShade's proxy under HookDirectX):
  // release it only outside the add-on's lock.
  static std::unique_ptr<GlBridge> Create(gl::Functions functions, LUID luid, Microsoft::WRL::ComPtr<ID3D12Device>* created, std::string* error);
  // Waits up to 2 s for the private queue, then releases the D3D12 side. It makes no GL call: ForgetGl first (destroy_device).
  ~GlBridge();
  GlBridge(const GlBridge&) = delete;
  GlBridge& operator=(const GlBridge&) = delete;

  [[nodiscard]] const gl::Functions& Gl() const { return interop_.Gl(); }
  [[nodiscard]] ID3D12Device* Device() const { return side_->Device(); }
  [[nodiscard]] ID3D12CommandQueue* Queue() const { return side_->Queue(); }
  // First thing at every present, as VkBridge::BeginFrame: the watchdog (GPU-ordered), the frees of what the queues have passed (both sides, and the GL names
  // that waited for a valid host), and the description of `back_buffer` (FB0); while `running` also the shared copy sized like it. Otherwise, and after a
  // latch, the shared surfaces are retired (Plan 7 C-1). `at_present`: NR would run at PRESENT (no marker), which is where MSAA is refused. `enabled` false
  // forgets failed shares. The problems are reported while NR is off too (R63's, with the runtime's queue but not its context, included). The shared colour is
  // in gl::SharedFormatOf's format, which is what the Session sees (design §3.5).
  BridgeFrame BeginFrame(gl::GameHost& host, const gl::ImageInfo& back_buffer, bool at_present, bool enabled, bool running,
                         std::chrono::steady_clock::time_point now);
  // One bridged recording. True when NR's result reached `frame`. `frame`: FB0 at PRESENT, the event's rtv resource at the technique and after the effects.
  // Nothing runs after the latch, while the ring is busy (which includes a CPU-ordered recording of an earlier frame still in flight after its 2 s wait ran out:
  // both count in BusySkips), without a shared copy, for a frame of another size than the shared copy, or without a valid host. `motion`: this frame's
  // UPLIFT_MV (RG16F; handle 0: none).
  bool Run(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, const Recorder& record);
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run's targets.mask.
  MaskCopy CopyMask(gl::GameHost& host, const gl::ImageInfo& mask);
  void ReleaseMask(gl::GameHost& host);  // deletes only while host.Valid() (key decision g): otherwise the names wait for the next valid frame
  // Deletes the two imported GL semaphores now (GL calls: the runtime's context must be current). For a bridge dropped before it ever ran, a failed
  // DeviceContext::Create right after Create, which no ForgetGl or retire reaches; every other teardown is destroy_device's ForgetGl, with no GL call.
  void ReleaseSemaphores();
  // As D3D11Bridge::Stop: no new wait is ever queued again, and both fences are signalled from the CPU up to the highest value a wait was queued for (which also
  // releases a GL wait on the imported semaphore, the same kernel object). Before DeviceContext::Teardown. Every latch ends here too.
  void Stop(std::string reason);
  // destroy_device, after Stop: every GL name is forgotten WITHOUT a GL call (they die with the share group, which ReShade has just made current for its own
  // teardown). The D3D12 resources stay for the destructor.
  void ForgetGl();
  [[nodiscard]] bool GpuOrdered() const { return gpu_ordered_; }
  [[nodiscard]] uint64_t SharedBytes() const;
  [[nodiscard]] std::string StatusLine() const;
  [[nodiscard]] bool Stopped() const { return !latch_.empty(); }
  [[nodiscard]] uint64_t BusySkips() const { return busy_skips_; }

 private:
  explicit GlBridge(gl::Functions functions) : interop_(std::move(functions)) {}

  // A texture on the private device, shared through an NT handle and imported into the game's GL context.
  struct Shared {
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
    gl::SharedTexture gl;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
    uint64_t last_use = 0u;  // the side's progress value after the newest submitted recording that used it
    // What host.Copy takes for the shared texture.
    [[nodiscard]] gl::ImageInfo Image() const { return {.handle = gl.Handle(), .size = size, .format = format, .samples = 1u}; }
  };
  // A share that failed: retried only for another size or format, or after NR is re-enabled (Plan 7 Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string what;  // the failing call: an HRESULT on the private device, or a GL call and its error
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const { return size == other_size && format == other_format; }
  };

  // Creates `shared` in COMMON on the private device and imports it. False, leaving `shared` untouched, on failure; `failure` says which call. The context is
  // current (the callers checked host.Valid()).
  bool CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, uint32_t bytes_per_pixel, const wchar_t* name, Shared* shared,
                    std::string* failure);
  // Latches the bridge for the session: "The OpenGL bridge stopped: <failure>. Restart the game to use NR again", and Stops it.
  void LatchGl(std::string_view failure);
  // A capped wait ran its full 2 s: counted in `consecutive` (reset by the caller when that kind of wait finishes). At the third in a row the bridge latches,
  // with `what` in its Details text, so a game whose queue work cannot finish does not pay 2 s for every frame.
  void NoteTimeout(uint32_t* consecutive, std::string_view what);
  // Retires both halves of `shared` (the D3D12 one by the side's progress, the GL one by a delete now or at the next valid frame) and empties it.
  void Retire(gl::GameHost& host, Shared* shared);
  // The private list's recording, shared by both orders. `motion12`: this frame's shared motion, or null. `wrote`: NR wrote the shared colour.
  // `used_mask`: the mask was read. True when the list was submitted.
  bool RecordAndExecute(const Recorder& record, ID3D12Resource* motion12, bool* wrote, bool* used_mask);
  // `motion`: the image to copy this frame, or an empty info; `motion12`: its shared D3D12 side, or null.
  bool RunGpuOrdered(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ID3D12Resource* motion12, const Recorder& record);
  bool RunCpuOrdered(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ID3D12Resource* motion12, const Recorder& record);
  // Stamps every surface this recording used with its progress value.
  void StampUsed(uint64_t value, bool used_mask, bool used_motion);

  // Declared first, so released last: its created device is the one D3D12CreateDevice returned (ReShade's proxy under HookDirectX).
  std::unique_ptr<D3D12Side> side_;
  gl::Interop interop_;
  bool gpu_ordered_ = false;
  std::string cpu_reason_;  // why CPU-ordered (design §7's reason), when it is
  // GPU-ordered only: one shared fence per direction, each signalled by one queue only, in increasing order (Plan 7 I-2), and each imported as a GL semaphore.
  Microsoft::WRL::ComPtr<ID3D12Fence> to12_;  // GL -> D3D12: odd values, the game's context signals, the private queue waits
  Microsoft::WRL::ComPtr<ID3D12Fence> to11_;  // D3D12 -> GL: even values, the private queue signals, the game's context waits
  GLuint to12_gl_ = 0u;
  GLuint to11_gl_ = 0u;
  Shared color_;
  Shared mask_;
  Shared motion_;
  bool mask_fresh_ = false;  // CopyMask wrote mask_ since the last recording
  std::optional<ShareFailure> color_failure_;
  std::optional<ShareFailure> mask_failure_;
  std::optional<ShareFailure> motion_failure_;
  BridgeWatchdog watchdog_;
  uint64_t last_value_ = 0u;  // the newest value handed out
  uint64_t waited12_ = 0u;    // the highest to12_ value the private queue was asked to wait for
  uint64_t waited11_ = 0u;    // the highest to11_ value a GL wait was queued for
  uint64_t busy_skips_ = 0u;
  uint32_t cpu_timeouts_ = 0u;       // consecutive 2 s timeouts of the CPU-ordered path's waits, reset by a frame that got through
  bool ran_logged_ = false;          // "NR ran on the game's frame" was logged since NR was last off
  HANDLE progress_event_ = nullptr;  // CPU-ordered: the private queue's completion wait
  std::string problem_;              // the back buffer's problem, logged once per change
  std::string latch_;                // set once: the bridge stopped for the session
};

}  // namespace uplift::bridge
