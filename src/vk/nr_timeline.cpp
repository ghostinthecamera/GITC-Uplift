#include "vk/nr_timeline.hpp"

#include <Windows.h>

#include <algorithm>
#include <format>
#include <utility>

#include "nr/log.hpp"
#include "nr/vk_handles.hpp"

namespace uplift::vk {
namespace {

// A recording that was never submitted cannot run; one still unsubmitted this long (and this many presents) after its token was issued is dropped.
constexpr auto STALE_AGE = std::chrono::seconds(2);
constexpr uint64_t STALE_FRAMES = 8u;

}  // namespace

NrCompletion::NrCompletion(const NrFunctions& functions, VkDevice device, QueueSignal signal)
    : functions_(functions), device_(device), signal_(std::move(signal)) {}

NrCompletion::~NrCompletion() {
  FreeAll();
}

std::unique_ptr<NrCompletion> NrCompletion::Create(const NrFunctions& functions, VkDevice device, QueueSignal signal, std::string* error) {
  if (device == VK_NULL_HANDLE || !signal) {
    *error = "NrCompletion needs a device and a signal function";
    return nullptr;
  }
  std::unique_ptr<NrCompletion> completion(new NrCompletion(functions, device, std::move(signal)));
  const VkSemaphoreTypeCreateInfo type = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0u,
  };
  const VkSemaphoreCreateInfo create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
  if (const VkResult result = functions.vkCreateSemaphore(device, &create, nullptr, &completion->semaphore_); result != VK_SUCCESS) {
    *error = std::format("vkCreateSemaphore (timeline) returned VkResult {}", static_cast<int>(result));
    return nullptr;
  }
  NrCompletion* const self = completion.get();
  completion->timeline_ = std::make_unique<nr::Timeline>(
      [self]() -> uint64_t {
        // Freed or idle: nothing is in flight any more. Lost: batch 1 review, minor 2, D3D12's reading of a removed device's fence, so no mark
        // ever waits on a semaphore the lost device can no longer signal.
        if (self->freed_.load(std::memory_order_acquire) || self->idle_.load(std::memory_order_acquire) || self->lost_.load(std::memory_order_acquire)) {
          return UINT64_MAX;
        }
        uint64_t value = 0u;
        const VkResult result = self->functions_.vkGetSemaphoreCounterValue(self->device_, self->semaphore_, &value);
        if (result == VK_ERROR_DEVICE_LOST) {
          self->lost_.store(true, std::memory_order_release);
          return UINT64_MAX;
        }
        return (result == VK_SUCCESS ? value : 0u);
      },
      [self](ID3D12CommandQueue* queue, uint64_t value) {
        if (self->freed_.load(std::memory_order_acquire) || self->lost_.load(std::memory_order_acquire) || self->idle_.load(std::memory_order_acquire)) return;
        if (!self->signal_(nr::VkQueueOf(queue), self->semaphore_, value)) {
          if (!self->signal_failed_logged_.exchange(true)) {
            Logf(nr::LogLevel::WARN, "the Vulkan frame semaphore's signal could not be submitted (frame {}); marks wait for the next one", value);
          }
          return;
        }
        // The highest value whose signal was submitted: Wait never waits past it (no wait on a signal that was never submitted).
        uint64_t seen = self->submitted_.load();
        while (seen < value && !self->submitted_.compare_exchange_weak(seen, value)) {
        }
      },
      [self](uint64_t value, std::chrono::milliseconds timeout) {
        if (self->freed_.load(std::memory_order_acquire) || self->idle_.load(std::memory_order_acquire) || self->lost_.load(std::memory_order_acquire)) {
          return true;
        }
        if (value > self->submitted_.load()) return false;
        const VkSemaphoreWaitInfo wait = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1u,
            .pSemaphores = &self->semaphore_,
            .pValues = &value,
        };
        const auto nanoseconds = static_cast<uint64_t>(std::max<int64_t>(timeout.count(), 0)) * 1'000'000ull;
        const VkResult result = self->functions_.vkWaitSemaphores(self->device_, &wait, nanoseconds);
        if (result == VK_ERROR_DEVICE_LOST) {
          self->lost_.store(true, std::memory_order_release);
        }
        return result == VK_SUCCESS;
      });
  completion->timeline_->SetTokenProbe([self](uint64_t token) { return self->EventSet(token); });
  return completion;
}

bool NrCompletion::EventSet(uint64_t token) {
  if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire) || idle_.load(std::memory_order_acquire)) return true;
  const std::scoped_lock lock(mutex_);
  const auto found = entries_.find(token);
  // No entry: the event was not made yet (the recording is still going on) or could not be made. Not complete: DropStale decides.
  if (found == entries_.end() || found->second.event == VK_NULL_HANDLE) return false;
  const VkResult status = functions_.vkGetEventStatus(device_, found->second.event);
  if (status == VK_ERROR_DEVICE_LOST) {
    lost_.store(true, std::memory_order_release);
    return true;
  }
  return status == VK_EVENT_SET;
}

bool NrCompletion::PollDeviceLost() {
  if (lost_.load(std::memory_order_acquire)) return true;
  if (freed_.load(std::memory_order_acquire)) return false;
  uint64_t value = 0u;
  if (functions_.vkGetSemaphoreCounterValue(device_, semaphore_, &value) == VK_ERROR_DEVICE_LOST) {
    lost_.store(true, std::memory_order_release);
  }
  return lost_.load(std::memory_order_acquire);
}

VkEvent NrCompletion::EventFor(uint64_t token, std::chrono::steady_clock::time_point now) {
  const std::scoped_lock lock(mutex_);
  if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
  // Batch 1 review, minor 5: one event per token, whoever asks twice.
  if (const auto found = entries_.find(token); found != entries_.end() && found->second.event != VK_NULL_HANDLE) return found->second.event;
  VkEvent event = VK_NULL_HANDLE;
  if (!pool_.empty()) {
    event = pool_.back();
    pool_.pop_back();
  } else {
    const VkEventCreateInfo create = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    if (const VkResult result = functions_.vkCreateEvent(device_, &create, nullptr, &event); result != VK_SUCCESS) {
      Logf(nr::LogLevel::WARN, "vkCreateEvent returned VkResult {}; the token stays outstanding until it is dropped as stale", static_cast<int>(result));
      event = VK_NULL_HANDLE;
    }
  }
  entries_[token] = {.event = event, .issued = now, .issue_frame = timeline_->CurrentFrame()};
  return event;
}

void NrCompletion::Executed(std::span<const uint64_t> tokens) {
  const std::scoped_lock lock(mutex_);
  for (const uint64_t token : tokens) {
    if (const auto found = entries_.find(token); found != entries_.end()) {
      if (found->second.dropped && !late_submit_logged_) {
        // Batch 1 review, minor 6: R77's accepted gap. The token completed when it was dropped, so a release may have run before this submission:
        // a device removal soon after is traced here.
        late_submit_logged_ = true;
        Log(nr::LogLevel::WARN, "Vulkan: a recording was submitted after its token was dropped as stale (more than 2 s and 8 frames after it was made)");
      }
      found->second.executed = true;
    }
  }
}

void NrCompletion::Reexecuted(std::span<const uint64_t> tokens, uint64_t first_token, void* queue) {
  VkEvent last = VK_NULL_HANDLE;
  bool reset_failed = false;
  {
    const std::scoped_lock lock(mutex_);
    if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire)) return;
    for (const uint64_t token : tokens) {
      const auto found = entries_.find(token);
      if (found == entries_.end() || found->second.event == VK_NULL_HANDLE) continue;
      // The list cannot be pending here (a command buffer without SIMULTANEOUS_USE is submitted again only after its last run finished), so
      // the reset cannot race the earlier execution that set the event.
      if (functions_.vkResetEvent(device_, found->second.event) != VK_SUCCESS) {
        reset_failed = true;  // the event stays SET: it cannot tell this execution's end
      }
      found->second.executed = true;
      found->second.dropped = false;
      last = found->second.event;
    }
  }
  if (last == VK_NULL_HANDLE) return;
  // Without the lock: the Timeline's probe takes it under the Timeline's own.
  const uint64_t fresh = timeline_->Resubmit(first_token, nr::AsQueue(queue), GetCurrentThreadId(), std::chrono::steady_clock::now());
  const std::scoped_lock lock(mutex_);
  if (!freed_.load(std::memory_order_acquire)) {
    // A failed reset maps the fresh token to no event and leaves it unexecuted: DropStale completes it after 2 s and 8 frames, long after the GPU
    // has run this submission, instead of the SET event completing it at once.
    entries_[fresh] = {
        .event = (reset_failed ? VK_NULL_HANDLE : last),
        .issued = std::chrono::steady_clock::now(),
        .issue_frame = timeline_->CurrentFrame(),
        .executed = !reset_failed,
    };
  }
}

void NrCompletion::Recycle(std::span<const uint64_t> tokens) {
  std::vector<uint64_t> dropped(tokens.begin(), tokens.end());
  {
    const std::scoped_lock lock(mutex_);
    if (!freed_.load(std::memory_order_acquire)) {
      std::vector<VkEvent> events;
      for (const uint64_t token : tokens) {
        const auto found = entries_.find(token);
        if (found != entries_.end() && found->second.event != VK_NULL_HANDLE && std::ranges::find(events, found->second.event) == events.end()) {
          events.push_back(found->second.event);
        }
      }
      // Every entry on one of these events goes with them: the listed tokens and the fresh one a re-execution mapped to a list's event.
      std::erase_if(entries_, [&](const auto& entry) {
        const bool listed = std::ranges::find(tokens, entry.first) != tokens.end();
        const bool aliased = entry.second.event != VK_NULL_HANDLE && std::ranges::find(events, entry.second.event) != events.end();
        if (!listed && !aliased) return false;
        if (!listed) {
          dropped.push_back(entry.first);
        }
        return true;
      });
      const bool lost = lost_.load(std::memory_order_acquire);
      for (const VkEvent event : events) {
        if (lost) {
          pool_.push_back(event);  // never used again after a loss: Abandon leaves the pool to the device
        } else if (functions_.vkResetEvent(device_, event) == VK_SUCCESS) {
          pool_.push_back(event);
        } else {
          functions_.vkDestroyEvent(device_, event, nullptr);  // batch 1 review, minor 1: a SET event must never serve a later token
        }
      }
    }
  }
  // Without the lock: Drop takes the Timeline's token lock, under which the probe takes this one.
  timeline_->Drop(dropped);
}

void NrCompletion::DropStale(std::chrono::steady_clock::time_point now, uint64_t frame) {
  std::vector<uint64_t> stale;
  {
    const std::scoped_lock lock(mutex_);
    for (auto& [token, entry] : entries_) {
      if (entry.dropped || (entry.executed && entry.event != VK_NULL_HANDLE)) continue;
      if (now - entry.issued < STALE_AGE || frame < entry.issue_frame + STALE_FRAMES) continue;
      entry.dropped = true;
      stale.push_back(token);
    }
  }
  if (stale.empty()) return;
  timeline_->Drop(stale);
  if (!stale_logged_) {
    stale_logged_ = true;
    Log(nr::LogLevel::INFO, "Vulkan: a recording was never submitted; its token was dropped after 2 s");
  }
}

void NrCompletion::DeviceIdle() {
  idle_.store(true, std::memory_order_release);
}

bool NrCompletion::WaitSubmitted(std::chrono::milliseconds cap) {
  const auto deadline = std::chrono::steady_clock::now() + cap;
  while (true) {
    bool pending = false;
    {
      const std::scoped_lock lock(mutex_);
      if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire) || idle_.load(std::memory_order_acquire)) return true;
      for (const auto& [token, entry] : entries_) {
        if (!entry.executed || entry.dropped) continue;  // never submitted: it cannot be running
        // A submitted recording whose event could not be made cannot tell its end: only the cap ends the wait for it.
        const VkResult status = (entry.event == VK_NULL_HANDLE ? VK_EVENT_RESET : functions_.vkGetEventStatus(device_, entry.event));
        if (status == VK_ERROR_DEVICE_LOST) {
          lost_.store(true, std::memory_order_release);
          return true;
        }
        if (status != VK_EVENT_SET) {
          pending = true;
          break;
        }
      }
    }
    if (!pending) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    Sleep(1u);
  }
}

void NrCompletion::FreeAll() {
  const std::scoped_lock lock(mutex_);
  if (freed_.exchange(true)) return;
  std::vector<VkEvent> events = std::move(pool_);
  for (const auto& [token, entry] : entries_) {
    if (entry.event != VK_NULL_HANDLE) {
      events.push_back(entry.event);
    }
  }
  entries_.clear();
  std::ranges::sort(events);
  const auto duplicates = std::ranges::unique(events);
  events.erase(duplicates.begin(), duplicates.end());
  for (const VkEvent event : events) {
    functions_.vkDestroyEvent(device_, event, nullptr);
  }
  if (semaphore_ != VK_NULL_HANDLE) {
    functions_.vkDestroySemaphore(device_, semaphore_, nullptr);
    semaphore_ = VK_NULL_HANDLE;
  }
}

void NrCompletion::Abandon() {
  const std::scoped_lock lock(mutex_);
  if (freed_.exchange(true)) return;  // from here every query says complete, and FreeAll (the destructor's too) destroys nothing
  entries_.clear();
  pool_.clear();
  semaphore_ = VK_NULL_HANDLE;
}

}  // namespace uplift::vk
