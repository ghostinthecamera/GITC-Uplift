#pragma once

#include <atomic>
#include <cstdint>

namespace uplift::addon {

// The epoch before any submission; a list that was never reset, or a thread that never submitted, holds it.
constexpr uint64_t NO_SUBMIT_EPOCH = 0u;
// What each submission adds to the shared counter.
constexpr uint64_t SUBMIT_EPOCH_STEP = 1u;

// Orders a thread's token submissions against later Resets of any Direct3D 12 command list, so an execute_command_list event can prove that
// an earlier ExecuteCommandLists call of its own thread has returned.
class SubmitEpochs {
 public:
  // reset_command_list (ReShade also raises it when a list is created): the value the list keeps until its next Reset.
  [[nodiscard]] uint64_t AtReset() const { return epoch_.load(); }
  // execute_command_list, when the calling thread submits tokens: the value that thread keeps.
  [[nodiscard]] uint64_t AtSubmit() { return epoch_.fetch_add(SUBMIT_EPOCH_STEP) + SUBMIT_EPOCH_STEP; }
  // A list last reset at `reset` was reset after the submission at `submit` (NO_SUBMIT_EPOCH: none): it belongs to a later ExecuteCommandLists call.
  [[nodiscard]] static constexpr bool LaterCall(uint64_t reset, uint64_t submit) { return submit != NO_SUBMIT_EPOCH && reset >= submit; }

 private:
  std::atomic<uint64_t> epoch_{NO_SUBMIT_EPOCH};
};

// execute_command_list: whether the calling thread stamps its submissions before handling the event. It must have submissions not yet stamped,
// and the list must have been reset after its latest submission, which proves a later call than the one those submissions belong to.
[[nodiscard]] constexpr bool StampsAtExecute(bool has_unstamped_submissions, uint64_t list_reset_epoch, uint64_t thread_submit_epoch) {
  return has_unstamped_submissions && SubmitEpochs::LaterCall(list_reset_epoch, thread_submit_epoch);
}

}  // namespace uplift::addon
