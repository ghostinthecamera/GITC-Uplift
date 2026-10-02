#pragma once

namespace uplift::addon {

// One D3D12 device per process loads NR: the runtime patches one IAT slot (spec §6.1). Plan 2 final
// review M8: the claim is taken when a device's Session is about to load, not at its first present,
// and the device DLSS runs on takes it from one that only presents once that one has released NR.
// Plan 10: the same rule serves the helper, which runs NR for one Direct3D 9, 10 or 11 device at a time: addon32.cpp has one claim, and
// addon.cpp a second one for its Direct3D 9 devices (their NR is not in-process, so they never count against the first).
// Not thread-safe: the add-on's lock.
class NrClaim {
 public:
  // Every frame of `device`, before its Session is enabled. `wants_nr`: NR is enabled for it;
  // `dlss_seen`: a DLSS main handle evaluates on it; `nr_loaded`: its Session is not OFF. True when
  // `device` may enable its Session this frame.
  bool Update(const void* device, bool wants_nr, bool dlss_seen, bool nr_loaded);
  // The device is gone.
  void Forget(const void* device);
  [[nodiscard]] const void* Owner() const { return owner_; }

 private:
  const void* owner_ = nullptr;
  const void* dlss_device_ = nullptr;  // the device DLSS was last seen on
};

}  // namespace uplift::addon
