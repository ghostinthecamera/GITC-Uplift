#pragma once

#include <chrono>

namespace uplift::addon {

// Spec §8.1 (amendment 7): NR runs on one swap chain per device, the first that presents, until
// it has not presented for `handover` (a launcher or splash window giving way to the game's).
//
// There is deliberately no destroyed-swap-chain callback. ReShade's v6.0.0 `destroy_swapchain`
// event fires from a resize (`ResizeBuffers`/`ResizeBuffers1`/`SetColorSpace1`) as well as a real
// teardown, and its single-argument signature cannot tell the two apart; reacting to it by
// clearing the primary would hand a resizing game's window over to some other swap chain that
// happens to present during the resize. A swap chain that is truly gone simply stops presenting,
// and `OnPresent`'s own idle rule below hands over to whatever presents next once `handover` has
// elapsed -- the only signal this class needs.
class SwapchainSelector {
 public:
  explicit SwapchainSelector(std::chrono::milliseconds handover = std::chrono::seconds(1)) : handover_(handover) {}

  // Every present on the device; true when `swapchain` is the primary one. `swapchain` is only
  // ever compared, never dereferenced, so a stale pointer from an already-destroyed swap chain is
  // safe to pass and to keep as `Primary()` until something else earns the handover.
  bool OnPresent(const void* swapchain, std::chrono::steady_clock::time_point now);
  [[nodiscard]] const void* Primary() const { return primary_; }

 private:
  std::chrono::milliseconds handover_;
  const void* primary_ = nullptr;
  std::chrono::steady_clock::time_point last_present_;
};

}  // namespace uplift::addon
