#include "vk/marker_ledger.hpp"

namespace uplift::vk {

MarkerLedger::Reservation MarkerLedger::Reserve() {
  if (proven_) return {};
  ++reserved_;
  const bool write = !written_;
  written_ = true;
  return {.counted = true, .write = write};
}

bool MarkerLedger::Created(Reservation reservation, const void* device) {
  // A proof that came meanwhile has released everything already.
  if (!reservation.counted || proven_) return false;
  --reserved_;
  if (device != nullptr) {
    devices_.insert(device);
    return false;
  }
  if (reserved_ == 0u && devices_.empty()) {
    written_ = false;
    return true;
  }
  return false;
}

bool MarkerLedger::Proven() {
  if (proven_) return false;
  proven_ = true;
  reserved_ = 0u;
  devices_.clear();
  const bool delete_file = written_;
  written_ = false;
  return delete_file;
}

bool MarkerLedger::Destroyed(const void* device) {
  if (devices_.erase(device) == 0u) return false;
  if (reserved_ == 0u && devices_.empty()) {
    written_ = false;
    return true;
  }
  return false;
}

}  // namespace uplift::vk
