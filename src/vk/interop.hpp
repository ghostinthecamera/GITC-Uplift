#pragma once

#include <Windows.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "nr/types.hpp"
#include "vk/game_host.hpp"
#include "vk/loader.hpp"

namespace uplift::vk {

// A Direct3D 12 texture imported into a game's Vulkan device: a VkImage on dedicated memory, created through the device's own vkCreateImage (so
// ReShade's layer knows it and its API can barrier, copy and destroy it).
struct SharedImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct ImportResult {
  SharedImage image;  // valid only when `result` is VK_SUCCESS
  VkResult result = VK_SUCCESS;
  const char* step = "";  // the call that failed
};

enum class WaitSlot : size_t {
  CPU_ORDER,      // the CPU-ordered path's wait for the copy into the shared texture
  PRESENT_QUEUE,  // design §3.4 case 2: the game presents from a queue other than ReShade's
};
inline constexpr size_t WAIT_SLOTS = 2u;

enum class WaitResult {
  DONE,
  TIMEOUT,  // the cap ran out: skip this frame (Plan 9 M-2). A single one never latches; the bridge stops after a few in a row (batch 2 review, minor 4)
  BUSY,     // an earlier wait on this slot is still in flight and nothing was submitted or waited for: skip this frame, never latch
  FAILED,   // the submission or the wait failed; DeviceLost() says whether the device is gone
};

// Plan 11 (Vulkan design §3.1, §4, §6): one game device's imports and the objects that go with them. ReShade-free; every Vulkan call it makes
// is one ReShade's layer either does not intercept (memory, semaphores, fences) or one that is safe while the device exists (vkCreateImage,
// vkDestroyImage). Nothing in it waits on the GPU except FlushAndWait, which is capped.
//
// Frees: Vulkan has no deferred destruction (key decision f). Retire keeps an image until a VkFence submitted after its last use has signalled;
// FreeFinished frees what has. FreeAll and FreeNow are for destroy_device, where ReShade has already dropped the device from its dispatch map
// (R62): an image goes only through GameHost::DestroyImage, everything else through functions the layer does not intercept.
// Not thread-safe: the caller's lock.
class Interop {
 public:
  explicit Interop(Device device) : device_(std::move(device)) {}
  Interop(const Interop&) = delete;
  Interop& operator=(const Interop&) = delete;
  // Frees nothing: a caller that still holds objects runs FreeAll first (only it knows a host).
  ~Interop() = default;

  [[nodiscard]] const Device& Functions() const { return device_; }

  // `nt_handle`: a Direct3D 12 texture's NT handle (the caller closes it). Dedicated import, usage TRANSFER_SRC | TRANSFER_DST, memory type as
  // key decision e. On failure nothing is left behind and `result`, `step` say what failed.
  ImportResult ImportImage(HANDLE nt_handle, DXGI_FORMAT format, nr::Size size);
  // A Direct3D 12 fence's NT handle into a new timeline semaphore (the caller closes the handle). On failure nothing is left behind.
  VkResult ImportTimeline(HANDLE nt_handle, VkSemaphore* semaphore, const char** step);
  void DestroySemaphore(VkSemaphore* semaphore);

  // Puts `image` behind a fence submitted now (FlushWithFence, no wait) and empties it.
  void Retire(SharedImage* image, GameHost& host);
  // The same for an imported semaphore whose queued work may still wait on it (the 32-bit client replaces its fences at a new attach); emptied.
  void RetireSemaphore(VkSemaphore* semaphore, GameHost& host);
  // Frees the retired images and semaphores whose fence has signalled; true when any went. An image whose fence could not be submitted gets another try.
  bool FreeFinished(GameHost& host);
  // Plan 17: nothing is retired and no capped wait is still in flight (a timed-out one's fence unsignalled): FreeAll then frees nothing the GPU may use.
  [[nodiscard]] bool Idle();
  // destroy_device: `host` needs only the device. The retired images go, the wait fences too.
  void FreeAll(GameHost& host);
  // destroy_device, or a device known to be idle: frees `image` at once and empties it.
  void FreeNow(SharedImage* image, GameHost& host);

  // Flushes `host`'s queue, submits an empty batch with this slot's fence and waits for it, at most `timeout_ms`. A wait that timed out stays in
  // flight: the next call on the slot says BUSY until it has finished, instead of resetting a pending fence.
  WaitResult FlushAndWait(WaitSlot slot, GameHost& host, uint32_t timeout_ms);
  // The timeline semaphore's counter, UINT64_MAX when it cannot be read (VK_ERROR_DEVICE_LOST sets DeviceLost()).
  uint64_t Counter(VkSemaphore semaphore);
  [[nodiscard]] bool DeviceLost() const { return device_lost_; }

 private:
  struct Retired {
    SharedImage image;
    VkSemaphore semaphore = VK_NULL_HANDLE;  // RetireSemaphore's (no image then)
    VkFence fence = VK_NULL_HANDLE;          // null: its fence could not be submitted yet
  };
  struct WaitFence {
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;  // submitted, and not seen signalled yet
  };

  void Note(VkResult result);
  // Makes `retired`'s fence and submits it behind everything recorded so far; false leaves it without one.
  bool Seal(Retired* retired, GameHost& host);
  void FreeMemory(SharedImage* image);

  Device device_;
  std::vector<Retired> retired_;
  std::array<WaitFence, WAIT_SLOTS> waits_;
  bool device_lost_ = false;
};

}  // namespace uplift::vk
