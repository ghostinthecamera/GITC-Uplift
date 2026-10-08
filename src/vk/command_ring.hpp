#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "vk/nr_functions.hpp"

namespace uplift::vk {

// Plan 19 (hard rule 1): native NR at Present records into command buffers of Uplift's own, never into ReShade's immediate command list (NGX's
// commands there reached every add-on's command-list events on a list no add-on had seen created, and crashed No Man's Sky in another add-on). A ring
// of SLOTS primary buffers, each in its own pool, allocated through the game's chain from the family of ReShade's effect queue
// (DeviceRecord::graphics_family), so ReShade and every add-on see them as ordinary command lists. Each slot has a fence its submission signals; a slot
// whose last submission has not finished is busy, and the frame goes without NR (never waited for). Not thread-safe: the caller's lock.
class CommandRing {
 public:
  static constexpr uint32_t SLOTS = 6u;  // review M-1: a pool, a buffer and a fence each; DXVK and others keep three frames in flight

  // nullptr with `error` when a pool, a buffer or a fence could not be made (nothing is left allocated then).
  static std::unique_ptr<CommandRing> Create(const NrFunctions& functions, VkDevice device, uint32_t family, std::string* error);
  ~CommandRing() = default;  // frees nothing: Destroy after the device is idle, or the device's end takes them (a loss, an abandon)
  CommandRing(const CommandRing&) = delete;
  CommandRing& operator=(const CommandRing&) = delete;

  enum class Status : uint8_t {
    READY,  // done
    BUSY,   // Begin: the slot's last submission is still on the GPU; WaitForQueue: the last wait's fence is still pending
    TIMEOUT,
    LOST,    // VK_ERROR_DEVICE_LOST
    FAILED,  // any other error
  };
  // The current slot's buffer, its pool reset and the buffer begun (one-time submit), in `*buffer`. A slot begun and never submitted is reset again by
  // the next Begin.
  Status Begin(VkCommandBuffer* buffer);
  // Ends the begun buffer and submits it to `queue` (the effect queue, through the game's chain: ReShade's hook flushes its immediate list first) with the
  // slot's fence; the ring moves on. The failing call's VkResult otherwise (the slot stays free).
  VkResult Submit(VkQueue queue);
  // Design §3.4 case 2, as the bridge's Interop::FlushAndWait: the game presents from another queue than the effect queue, so `flush` flushes that queue
  // and signals the ring's own wait fence there, and the CPU waits for it, at most `cap`. A timeout leaves the fence pending: the next call is BUSY until it
  // signals.
  Status WaitForQueue(const std::function<bool(VkFence)>& flush, std::chrono::milliseconds cap);
  // The game's NGX shutdown: every slot's last submission (and a pending wait) finished within `cap`. False on a timeout or a loss.
  bool WaitIdle(std::chrono::milliseconds cap);
  // The device is idle (the vkDestroyDevice detour): the pools (their buffers with them) and the fences go. Idempotent.
  void Destroy();

 private:
  CommandRing(const NrFunctions& functions, VkDevice device) : functions_(functions), device_(device) {}

  struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;  // created signalled: a slot never submitted is free
  };

  NrFunctions functions_;
  VkDevice device_;
  std::array<Slot, SLOTS> slots_ = {};
  VkFence wait_fence_ = VK_NULL_HANDLE;
  bool wait_pending_ = false;
  uint32_t current_ = 0u;
  bool begun_ = false;
};

}  // namespace uplift::vk
