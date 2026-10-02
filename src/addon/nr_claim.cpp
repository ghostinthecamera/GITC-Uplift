#include "addon/nr_claim.hpp"

namespace uplift::addon {

bool NrClaim::Update(const void* device, bool wants_nr, bool dlss_seen, bool nr_loaded) {
  if (dlss_seen) {
    dlss_device_ = device;
  }
  if (owner_ == device) {
    const bool yielding = (dlss_device_ != nullptr && dlss_device_ != device);
    if (!nr_loaded && (yielding || !wants_nr)) {
      owner_ = nullptr;  // released: the claim is free for the next device that wants it
      return false;
    }
    return wants_nr && !yielding;  // a yielding owner drains first, so the runtime is never loaded twice
  }
  if (owner_ != nullptr || !wants_nr) return false;
  if (dlss_device_ != nullptr && dlss_device_ != device) return false;  // the DLSS device goes first
  owner_ = device;
  return true;
}

void NrClaim::Forget(const void* device) {
  if (owner_ == device) {
    owner_ = nullptr;
  }
  if (dlss_device_ == device) {
    dlss_device_ = nullptr;
  }
}

}  // namespace uplift::addon
