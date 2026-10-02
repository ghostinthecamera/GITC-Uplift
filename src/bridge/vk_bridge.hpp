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
#include "nr/types.hpp"
#include "vk/frame.hpp"
#include "vk/game_host.hpp"
#include "vk/interop.hpp"
#include "vk/loader.hpp"

namespace uplift::bridge {

// One Vulkan image as the host describes it (vk::ImageInfo, which the 32-bit client shares).
using VkImageInfo = vk::ImageInfo;

// Plan 11 (Vulkan design §4): a private D3D12 device on a Vulkan game device's adapter, the back buffer's shared copy, and the per-recording
// hand-off NR runs inside. D3D11Bridge's shape with a Vulkan game side (src/vk/), on the same D3D12Side, RunBridgedFrame, BridgeWatchdog and
// RecordNr.
// - GPU-ordered (both Win32 extensions and timeline semaphores): the two shared D3D12 fences are imported as timeline semaphores and the frame is
//   design §3.2's. CPU-ordered (a device without the semaphore extension): the copy in, a capped CPU wait, NR, a capped CPU wait, the copy out
//   (§3.3); no fence exists and nothing waits on the GPU.
// - Every Vulkan call goes through the GameHost (ReShade's API in the add-on) or the src/vk/ Interop; every CPU wait is capped at 2 s, and a
//   timeout skips that frame. A single one never latches (Plan 9 M-2); three in a row stop the bridge for the session (batch 2 review, minor 4).
// - Not thread-safe: the add-on's lock.
class VkBridge {
 public:
  // nullptr, with `error` set, when the adapter is not NVIDIA's, the LUID is unknown or the D3D12 side cannot be made. A device that cannot be
  // ordered on the GPU still gets a bridge: it builds CPU-ordered and StatusLine says why. `created` receives the device as D3D12CreateDevice
  // returned it, whether or not this succeeds (ReShade's proxy under the Vulkan layer, R56): release it only outside the add-on's lock.
  static std::unique_ptr<VkBridge> Create(vk::Device device, LUID luid, Microsoft::WRL::ComPtr<ID3D12Device>* created, std::string* error);
  // Waits up to 2 s for the private queue, then releases the D3D12 side. It frees no Vulkan image: FreeVulkan first (destroy_device).
  ~VkBridge();
  VkBridge(const VkBridge&) = delete;
  VkBridge& operator=(const VkBridge&) = delete;

  [[nodiscard]] ID3D12Device* Device() const { return side_->Device(); }
  [[nodiscard]] ID3D12CommandQueue* Queue() const { return side_->Queue(); }
  [[nodiscard]] const vk::Device& VulkanDevice() const { return interop_.Functions(); }
  // First thing at every present, as D3D11Bridge::BeginFrame: the watchdog (GPU-ordered), the frees of what the queues have passed (both
  // sides), and the description of `back_buffer`; while `running` also the shared copy sized like it. Otherwise, and after a latch, the shared
  // surfaces are retired (Plan 7 C-1 and Minor 2). `enabled` false forgets failed shares.
  // Batch 2 review I-1 (a): while `running` and the frame has no problem, the opening global barrier is recorded here, into the runtime queue's
  // immediate list, in the present event and so before ReShade records its effects: everything the game submitted on that queue finishes before
  // the effects (which Uplift's early flushes submit without the game's present semaphores) and before the copy in.
  BridgeFrame BeginFrame(vk::GameHost& host, const VkImageInfo& back_buffer, bool enabled, bool running, std::chrono::steady_clock::time_point now);
  // One bridged recording. True when NR's result reached `back_buffer`. Nothing runs after the latch, while the ring is busy (which includes a
  // CPU-ordered recording of an earlier frame still in flight after its 2 s wait ran out: both count in BusySkips), without a shared copy, or without an
  // immediate command list. `entry_state`: the back buffer's state in the calling event. `motion`: this frame's UPLIFT_MV (RG16F; image null: none), or,
  // with `motion_is_dlss` (Plan 14, design §2.4), DLSS's vectors copied in the game's frame (RG16F at the motion region's own size, SHADER_READ_ONLY_OPTIMAL):
  // the private device binds them as DLSS's copied vectors (the full subrect, a scale of (1, 1)).
  // `present_host`: design §3.4 case 2, when the game presents from a queue other than the one Uplift records on: that queue is flushed and waited for
  // on the CPU (2 s), once per frame (BeginFrame starts a frame), after the skip checks above and before the first thing that could submit work (the
  // copy in, or a motion share's retire fence, which flushes the immediate list); false (this frame goes without NR) when that timed out or failed.
  bool Run(vk::GameHost& host, const VkImageInfo& back_buffer, vk::Usage entry_state, const VkImageInfo& motion, const Recorder& record,
           vk::GameHost* present_host = nullptr, bool motion_is_dlss = false);
  // At the end of the effects: UPLIFT_MASK into the shared mask, for the next Run's targets.mask.
  MaskCopy CopyMask(vk::GameHost& host, const VkImageInfo& mask);
  void ReleaseMask(vk::GameHost& host);
  // Final review I-2 as D3D11Bridge::Stop: no new wait is ever queued again, and both fences are signalled from the CPU up to the highest value a
  // wait was queued for (which also releases a Vulkan wait on the imported semaphore, the same kernel object). Before DeviceContext::Teardown.
  // Every latch ends here too (LatchVulkan), so a private queue never waits for a signal a lost device will not make.
  void Stop(std::string reason);
  // destroy_device, after Stop: every Vulkan object (key decision f, R62). `host` needs only the device; nothing here calls a Vulkan function that
  // ReShade's layer intercepts. The D3D12 resources stay for the destructor.
  void FreeVulkan(vk::GameHost& host);
  // The game presents from a second queue (logged and shown once by the caller).
  void NoteSecondQueue() { second_queue_ = true; }
  [[nodiscard]] bool GpuOrdered() const { return gpu_ordered_; }
  [[nodiscard]] uint64_t SharedBytes() const;
  [[nodiscard]] std::string StatusLine() const;
  [[nodiscard]] bool Stopped() const { return !latch_.empty(); }
  [[nodiscard]] uint64_t BusySkips() const { return busy_skips_; }

 private:
  explicit VkBridge(vk::Device device) : interop_(std::move(device)) {}

  // A texture on the private device, shared through an NT handle and imported into the game's Vulkan device.
  struct Shared {
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
    vk::SharedImage vulkan;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
    uint64_t last_use = 0u;  // the side's progress value after the newest submitted recording that used it
  };
  // A share that failed: retried only for another size or format, or after NR is re-enabled (Plan 7 Minor 3).
  struct ShareFailure {
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string what;  // the failing call: an HRESULT on the private device, or a Vulkan step and result
    [[nodiscard]] bool Matches(nr::Size other_size, DXGI_FORMAT other_format) const { return size == other_size && format == other_format; }
  };

  // Creates `shared` in COMMON on the private device and imports it. False, leaving `shared` untouched, on failure; `failure` says which call.
  bool CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, uint32_t bytes_per_pixel, const wchar_t* name, Shared* shared,
                    std::string* failure);
  // Latches the bridge for the session: "The Vulkan bridge stopped: <failure>. Restart the game to use NR again" (a lost device says so
  // instead), and Stops it (design §6: a device loss stops the bridge).
  void LatchVulkan(std::string_view failure);
  // A capped wait ran its full 2 s: counted in `consecutive` (reset by the caller when that kind of wait finishes). At the third in a row the
  // bridge latches, with `what` in its Details text, so a game whose queue work cannot finish does not pay 2 s for every frame.
  void NoteTimeout(uint32_t* consecutive, std::string_view what);
  // Retires both halves of `shared` (the D3D12 one by the side's progress, the Vulkan one behind a fence) and empties it.
  void Retire(vk::GameHost& host, Shared* shared);
  // The private list's recording, shared by both orders. `motion12`: this frame's shared motion, or null. `wrote`: NR wrote the shared colour.
  // `used_mask`: the mask was read. `motion_is_dlss`: `motion12` is DLSS's copied vectors. True when the list was submitted.
  bool RecordAndExecute(const Recorder& record, ID3D12Resource* motion12, bool motion_is_dlss, bool* wrote, bool* used_mask);
  // `motion`: the image to copy this frame, or an empty info; `motion12`: its shared D3D12 side, or null.
  bool RunGpuOrdered(vk::GameHost& host, const VkImageInfo& back_buffer, vk::Usage entry_state, const VkImageInfo& motion,
                     ID3D12Resource* motion12, bool motion_is_dlss, const Recorder& record);
  bool RunCpuOrdered(vk::GameHost& host, const VkImageInfo& back_buffer, vk::Usage entry_state, const VkImageInfo& motion,
                     ID3D12Resource* motion12, bool motion_is_dlss, const Recorder& record);
  // Stamps every surface this recording used with its progress value.
  void StampUsed(uint64_t value, bool used_mask, bool used_motion);

  // Declared first, so released last: its created device is the one D3D12CreateDevice returned (ReShade's proxy under the layer).
  std::unique_ptr<D3D12Side> side_;
  vk::Interop interop_;
  bool gpu_ordered_ = false;
  std::string cpu_reason_;  // why CPU-ordered (design §8's reason), when it is
  // GPU-ordered only: one shared fence per direction, each signalled by one queue only, in increasing order (Plan 7 I-2), and each imported as a
  // timeline semaphore in the game's device.
  Microsoft::WRL::ComPtr<ID3D12Fence> to12_;  // Vulkan -> D3D12: odd values, the game's queue signals, the private queue waits
  Microsoft::WRL::ComPtr<ID3D12Fence> to11_;  // D3D12 -> Vulkan: even values, the private queue signals, the game's queue waits
  VkSemaphore to12_vk_ = VK_NULL_HANDLE;
  VkSemaphore to11_vk_ = VK_NULL_HANDLE;
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
  uint64_t waited11_ = 0u;    // the highest to11_ value a Vulkan wait was queued for
  uint64_t busy_skips_ = 0u;
  uint32_t present_queue_timeouts_ = 0u;  // consecutive 2 s timeouts of the present-queue wait
  uint32_t cpu_timeouts_ = 0u;            // consecutive 2 s timeouts of the CPU-ordered path's waits, reset by a frame that got through
  bool second_queue_ = false;
  bool present_synced_ = false;      // this frame's present-queue wait (design §3.4 case 2) is done; BeginFrame clears it
  bool ran_logged_ = false;          // "NR ran on the game's frame" was logged since NR was last off
  HANDLE progress_event_ = nullptr;  // CPU-ordered: the private queue's completion wait
  std::string problem_;              // the back buffer's problem, logged once per change
  std::string latch_;                // set once: the bridge stopped for the session
};

}  // namespace uplift::bridge
