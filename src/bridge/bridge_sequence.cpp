#include "bridge/bridge_sequence.hpp"

#include <limits>

namespace uplift::bridge {

BridgedFrame RunBridgedFrame(BridgeSteps& steps, uint64_t in, uint64_t out) {
  steps.CopyIn();
  // Nothing is queued on D3D12 yet, so a refused signal leaves nothing waiting.
  if (!steps.SignalD3D11(in)) return {.failure = "the Direct3D 11 fence signal failed"};
  // The queue did not wait, so running the list could read the shared copy before D3D11 wrote it.
  if (!steps.WaitD3D12(in)) return {.failure = "the private Direct3D 12 queue refused to wait"};
  bool wrote = false;
  const bool submitted = steps.RecordAndExecute(&wrote);
  // Always signal once the queue waited: `out` is the only value D3D11 ever waits for.
  if (!steps.SignalD3D12(out)) return {.failure = "the private Direct3D 12 queue refused to signal"};
  // Without the wait, a copy back could read the shared copy while NR still writes it.
  if (!steps.WaitD3D11(out)) return {.d3d12_signalled = true, .failure = "the Direct3D 11 fence wait failed"};
  if (submitted && wrote) {
    steps.CopyOut();
  }
  return {.wrote = (submitted && wrote), .d3d12_signalled = true};
}

void BridgeWatchdog::Submitted(uint64_t value, std::chrono::steady_clock::time_point now) {
  if (idle_) {
    since_ = now;
    idle_ = false;
  }
  submitted_ = value;
}

std::optional<std::string_view> BridgeWatchdog::Check(uint64_t completed, uint64_t progress,
                                                      std::chrono::steady_clock::time_point now) {
  if (completed == std::numeric_limits<uint64_t>::max()) return "the private Direct3D 12 device was removed";
  if (progress != progress_) {
    progress_ = progress;
    since_ = now;
  }
  idle_ = (completed >= submitted_);
  if (!idle_ && now - since_ >= LIMIT) {
    return "the cross-API fences made no progress for 2 s while NR's work was outstanding";
  }
  return std::nullopt;
}

}  // namespace uplift::bridge
