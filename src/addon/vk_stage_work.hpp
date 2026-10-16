#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace uplift::addon {

// Split chain (Task 5; fix round 2, I3): the native Vulkan context's rule that NR never runs on two queues at once, without any GPU wait (fix round 1, I1, as
// Direct3D 12's Timeline::StageQueueOtherThan and StageWorkDone). It keeps a split's DLSS-stage recordings (their tokens) with the queue each one's command
// buffer went to (null: not submitted yet), and every queue they went to. A queue other than the effect queue, where a later Present group records, keeps
// Present off for later groups (OtherQueue: the caller's chain moves their passes to the DLSS stage); otherwise a later Present group runs only once every
// earlier DLSS-stage recording off the effect queue has completed (Done), else it skips that frame. Queues are opaque identities (ReShade's command queues:
// one per VkQueue). Pure; the add-on's lock.
class VkStageWork {
 public:
  // A split's DLSS-stage recording (its token, once).
  void Note(uint64_t token) {
    if (token == 0u || std::ranges::any_of(tokens_, [token](const Entry& entry) { return entry.token == token; })) return;
    tokens_.push_back({.token = token});
  }
  // The command buffer holding `tokens` was submitted to `queue` (its first execution, or again without a reset: the same queue rule applies).
  void Executed(std::span<const uint64_t> tokens, const void* queue) {
    for (Entry& entry : tokens_) {
      if (std::ranges::find(tokens, entry.token) == tokens.end()) continue;
      entry.queue = queue;
      if (std::ranges::find(queues_, queue) == queues_.end()) {
        queues_.push_back(queue);
      }
    }
  }
  // The command buffer was reset or destroyed: by Vulkan's rules it was not pending, so its recordings are complete or never ran.
  void Recycled(std::span<const uint64_t> tokens) {
    std::erase_if(tokens_, [tokens](const Entry& entry) { return std::ranges::find(tokens, entry.token) != tokens.end(); });
  }
  // Each present: the recordings on the effect queue (its own order keeps them apart) or complete (`complete(token)`) hold nothing back any more.
  template <typename Complete>
  void Prune(const void* effect_queue, Complete&& complete) {
    std::erase_if(tokens_, [&](const Entry& entry) { return (entry.queue != nullptr && entry.queue == effect_queue) || complete(entry.token); });
  }
  // No split: no later Present group waits for them. The queues seen stay (the latch never flaps).
  void Clear() { tokens_.clear(); }
  // The first queue other than `effect_queue` a DLSS-stage recording went to, ever; null when none did.
  [[nodiscard]] const void* OtherQueue(const void* effect_queue) const {
    const auto other = std::ranges::find_if(queues_, [effect_queue](const void* queue) { return queue != effect_queue; });
    return (other == queues_.end() ? nullptr : *other);
  }
  // Whether a later Present group may run now: every recording is on the effect queue, complete, or (once some went somewhere) not submitted yet. Until
  // some DLSS-stage buffer was seen submitted, where an unsubmitted one will go is unknown: wait for it.
  template <typename Complete>
  [[nodiscard]] bool Done(const void* effect_queue, Complete&& complete) const {
    for (const Entry& entry : tokens_) {
      if (entry.queue != nullptr && entry.queue == effect_queue) continue;
      if (complete(entry.token)) continue;
      if (entry.queue == nullptr && !queues_.empty()) continue;
      return false;
    }
    return true;
  }
  [[nodiscard]] size_t Pending() const { return tokens_.size(); }

 private:
  struct Entry {
    uint64_t token = 0u;
    const void* queue = nullptr;
  };
  std::vector<Entry> tokens_;
  std::vector<const void*> queues_;
};

}  // namespace uplift::addon
