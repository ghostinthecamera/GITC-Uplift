#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace uplift::bridge {

// The cross-API steps of one bridged recording (D3D11 design §2). A bool step returns false when the API refused it.
class BridgeSteps {
 public:
  virtual ~BridgeSteps() = default;
  virtual void CopyIn() = 0;                       // D3D11: the back buffer (and motion) into the shared copies
  virtual bool SignalD3D11(uint64_t value) = 0;    // D3D11: Signal, then Flush
  virtual bool WaitD3D12(uint64_t value) = 0;      // the private queue waits for D3D11's copies
  virtual bool RecordAndExecute(bool* wrote) = 0;  // the private list: NR. False when nothing was submitted
  virtual bool SignalD3D12(uint64_t value) = 0;    // the private queue signals, after its list
  virtual bool WaitD3D11(uint64_t value) = 0;      // D3D11 waits for NR (a GPU wait)
  virtual void CopyOut() = 0;                      // D3D11: the shared copy back into the back buffer
};

struct BridgedFrame {
  bool wrote = false;            // NR's result reached the back buffer
  bool d3d12_signalled = false;  // the private queue will signal `out`: the ring and the watchdog track it
  std::string_view failure;      // static text; non-empty: the bridge stops for the session
};

// D3D11 design §1.3: runs the steps in the one order that cannot leave the game's GPU waiting on a value nobody
// signals. D3D11 waits only for a value whose D3D12 signal was already submitted, and the private queue always
// signals once it has waited, even when nothing ran.
BridgedFrame RunBridgedFrame(BridgeSteps& steps, uint64_t in, uint64_t out);

// D3D11 design §1.3 and §9 f, the second line of defence. At every present, a removed private device (its fence
// reads UINT64_MAX) stops the bridge, and so do fences that made no progress for LIMIT while a D3D12 signal was
// outstanding. Final review I-2: progress, not age, so slow frames that still progress (hotsampling at 1-4 fps, a
// paging stall, NGX creating a feature) never trip it. Pure, so the unit tests drive it.
class BridgeWatchdog {
 public:
  static constexpr std::chrono::seconds LIMIT{2};
  // After a D3D12 signal of `value` was submitted, at `now`: the clock starts at submission, not before recording.
  void Submitted(uint64_t value, std::chrono::steady_clock::time_point now);
  // `completed`: the D3D12 -> D3D11 fence's completed value. `progress`: a count that grows whenever either fence
  // completes a signal. nullopt while healthy, else why the bridge stops (static text).
  [[nodiscard]] std::optional<std::string_view> Check(uint64_t completed, uint64_t progress,
                                                      std::chrono::steady_clock::time_point now);

 private:
  uint64_t submitted_ = 0u;                      // the newest D3D12 signal submitted
  uint64_t progress_ = 0u;                       // at the last Check
  bool idle_ = true;                             // the last Check found nothing outstanding
  std::chrono::steady_clock::time_point since_;  // the last progress seen, or the submission that ended an idle spell
};

}  // namespace uplift::bridge
