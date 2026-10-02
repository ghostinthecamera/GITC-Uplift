#pragma once

#include <cstdint>
#include <unordered_set>

namespace uplift::vk {

// Plan 11 (Vulkan design §2.3, batch 2 review I-3): who the pending marker is waiting on. The marker is one file for the whole process, so the
// hook counts the adjusted devices that are unproven (neither presented twice nor destroyed): the file is written when the count leaves 0 and
// deleted only when it returns to 0. The first adjusted device to present twice proves the adjustment for this game: the file goes, every device
// still counting is released, and nothing is written for a device created later. The methods only say when the file must be written or deleted;
// the hook does the file work. Pure (no Windows, no Vulkan) and not thread-safe: the hook's own lock.
class MarkerLedger {
 public:
  struct Reservation {
    bool counted = false;  // the creation counts toward the marker until Created or Proven says otherwise
    bool write = false;    // the file must be written now, before the device is created
  };

  // An adjusted device is about to be created. Not counted once the adjustment is proven.
  [[nodiscard]] Reservation Reserve();
  // The creation is over. `device` is the new adjusted device and is counted from now on (when the reservation was); null when nothing adjusted
  // exists (the creation failed, or went through without the additions), which releases the reservation. True when the file must be deleted.
  [[nodiscard]] bool Created(Reservation reservation, const void* device);
  // An adjusted device presented twice: the adjustment is proven, for this game. True, once, when the file must be deleted.
  [[nodiscard]] bool Proven();
  // A device is destroyed; only one that still counts matters. True when the file must be deleted.
  [[nodiscard]] bool Destroyed(const void* device);

  [[nodiscard]] bool IsProven() const { return proven_; }

 private:
  uint32_t reserved_ = 0u;                   // creations under way, counted
  std::unordered_set<const void*> devices_;  // adjusted devices that count
  bool proven_ = false;
  bool written_ = false;  // the file is on disk by this ledger's account
};

}  // namespace uplift::vk
