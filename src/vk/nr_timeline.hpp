#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "nr/timeline.hpp"
#include "vk/nr_functions.hpp"

namespace uplift::vk {

// `signal`: signals `semaphore` to `value` on `queue` (an opaque present-queue identity the Timeline keeps): ReShade's command_queue::signal in
// the add-on (present event, ReShade holds the queue's lock), a native submit in the smokes. False when it could not be submitted.
using QueueSignal = std::function<bool(void* queue, VkSemaphore semaphore, uint64_t value)>;

// Plan 13 (design §3.3): how the Vulkan nr::Timeline learns that the GPU is done.
// - Frames: a timeline VkSemaphore of Uplift's, signalled at present (BeginFrame(frame n+1) signals n) through `signal`; Completed reads the
//   counter, Wait is vkWaitSemaphores (capped by the caller's timeout), and never waits for a value whose signal was not submitted.
// - Tokens: VkEvents. Every hooked evaluate that recorded anything ends with vkCmdSetEvent(EventFor(token), ALL_COMMANDS), whose first
//   synchronisation scope is everything earlier on the queue, so it covers DLSS, NR and Uplift's passes on whichever queue the buffer runs.
//   The GPU sets an event and the CPU polls it (vkGetEventStatus): a host cannot wait on one, so Flush polls. The token is complete when its
//   event is SET (or the device is lost, or idle: DeviceIdle). A token with no event yet is NOT complete.
//   An event belongs to its list's recording and returns to the pool (vkResetEvent) at the list's next reset or destroy: Recycle. A re-executed
//   list resets its events and gets a fresh token mapped to its last event (Reexecuted). A list that is never executed is dropped by DropStale.
// Locking: one small mutex of its own, a leaf: the probe takes it under the Timeline's token lock, so no method here ever calls the Timeline
// while holding it. Methods other than the Timeline's own probe are called under the add-on's lock (or by the smoke's one thread).
class NrCompletion {
 public:
  static std::unique_ptr<NrCompletion> Create(const NrFunctions& functions, VkDevice device, QueueSignal signal, std::string* error);
  ~NrCompletion();  // FreeAll when the owner did not (the device must still be alive and idle)
  NrCompletion(const NrCompletion&) = delete;
  NrCompletion& operator=(const NrCompletion&) = delete;

  [[nodiscard]] nr::Timeline& Timeline() { return *timeline_; }  // generic ctor: Completed = counter, Signal = signal_, Wait = vkWaitSemaphores
  // Key decision c. The event for `token`, which the caller records with vkCmdSetEvent(event, ALL_COMMANDS) at the end of the hooked evaluate.
  // VK_NULL_HANDLE when no event could be made (the token then stays outstanding until DropStale takes it, executed or not). Idempotent per
  // token (batch 1 review, minor 5): a second call returns the token's own event.
  VkEvent EventFor(uint64_t token, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  // The list holding `tokens` was submitted (execute_command_list, before the native submit): DropStale leaves them alone. A token DropStale
  // already took is the accepted R77 gap (a recording submitted more than 2 s and 8 frames after it was made): logged once.
  void Executed(std::span<const uint64_t> tokens);
  // The list was submitted again without a reset: vkResetEvent each, then Timeline::Resubmit(first_token, queue); the fresh token maps to the
  // last of `tokens`' events (set after everything earlier in that command buffer), so `tokens` must be in recording order. Batch 1 review,
  // minor 1: when a reset fails, the event stays SET, so the fresh token maps to no event (DropStale takes it after 2 s and 8 frames).
  void Reexecuted(std::span<const uint64_t> tokens, uint64_t first_token, void* queue);
  // The list was reset or destroyed: events back to the pool (vkResetEvent) and the tokens (and any fresh one a re-execution mapped to their
  // events) dropped from the Timeline: by Vulkan's rules the list was not pending, so each token is complete or never will be. An event
  // whose reset fails is destroyed instead of pooled (batch 1 review, minor 1).
  void Recycle(std::span<const uint64_t> tokens);
  // Never-executed recordings, 2 s and 8 frames after the token was issued: Timeline::Drop (complete). One INFO line the first time.
  void DropStale(std::chrono::steady_clock::time_point now, uint64_t frame);
  // The game idled the device (the vkDestroyDevice detour): nothing is in flight, so every token and every frame is complete, and the frame
  // signal is no longer submitted (no queue is touched during the device's destruction). Teardown only.
  void DeviceIdle();
  // Batch 3 review I-1 (the game's NGX shutdown, before the device goes): polls the events of every submitted token until all are SET, for at most
  // `cap`. Only events are read: no queue is signalled or waited on. A recording never submitted cannot run and is not waited for. True when all
  // are done (or the device is lost, or freed or idle already); false when the cap ran out first.
  bool WaitSubmitted(std::chrono::milliseconds cap);
  [[nodiscard]] bool DeviceLost() const { return lost_.load(std::memory_order_acquire); }  // a query returned VK_ERROR_DEVICE_LOST
  // Asks the device directly (the frame semaphore's counter): true once it is lost. The present's check, before anything else runs.
  [[nodiscard]] bool PollDeviceLost();
  // Device idle (the vkDestroyDevice detour): destroy the semaphore and every event. After it every query says complete.
  void FreeAll();
  // Batch 1 review, minor 2: a device loss, or a device ReShade already dropped (destroy_device without the detour, R83). Every query says
  // complete from now on, and nothing is destroyed: the game's recorded command buffers may still name these events, and no Vulkan call
  // may go through the layer any more. The events and the semaphore are left to the device's own end.
  void Abandon();

 private:
  NrCompletion(const NrFunctions& functions, VkDevice device, QueueSignal signal);

  struct Entry {
    VkEvent event = VK_NULL_HANDLE;
    std::chrono::steady_clock::time_point issued;
    uint64_t issue_frame = 0u;
    bool executed = false;
    bool dropped = false;
  };

  // The Timeline's probe.
  bool EventSet(uint64_t token);

  NrFunctions functions_;
  VkDevice device_;
  QueueSignal signal_;
  VkSemaphore semaphore_ = VK_NULL_HANDLE;
  std::atomic<uint64_t> submitted_{0u};  // the highest value whose signal was submitted: Wait never waits past it
  std::atomic<bool> lost_{false};
  std::atomic<bool> freed_{false};
  std::atomic<bool> signal_failed_logged_{false};
  std::unique_ptr<nr::Timeline> timeline_;

  std::mutex mutex_;
  std::map<uint64_t, Entry> entries_;  // by token
  std::vector<VkEvent> pool_;          // reset, unused
  std::atomic<bool> idle_{false};      // DeviceIdle: read by the frame functions without the mutex too
  bool stale_logged_ = false;
  bool late_submit_logged_ = false;
};

}  // namespace uplift::vk
