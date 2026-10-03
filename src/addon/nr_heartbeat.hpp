#pragma once

#include <Windows.h>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sources/motion_source.hpp"

namespace uplift::addon {

// 1.0.1 (the Launchpad investigation, F2): three failures in two games came about 217 s after NR began running with Launchpad's motion vectors, and the
// logs could not say how long NR had run, with what, or how much memory the game held then. While NR runs, a device logs one INFO line a minute: the NR
// frames since the current motion source started, that source, how long NR has been running, and the process's private bytes and VRAM; the same figures
// once more when its bridge stops or its device is removed. The counting is pure (unit-tested); the memory figures are read by the caller.
class NrHeartbeat {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds PERIOD{60};
  // No NR recording for longer than this ends a run (NR off, or a long stop); a shorter gap (a busy frame, a reload) keeps it going.
  static constexpr std::chrono::seconds GAP{10};

  struct Figures {
    uint64_t frames = 0u;  // NR frames with `source` since it started
    sources::MotionSource source = sources::MotionSource::NONE;
    std::chrono::seconds since{0};  // how long ago `source` started
    std::chrono::seconds run{0};    // how long ago this run of NR started (a source change does not start a new run)
  };

  // At every present, with DeviceContext::NrRecordings (the recordings that applied NR so far: a count, so a DLSS placement whose game's DLSS idles
  // counts nothing) and the latest recording's motion source. A new context (Retry now) counts from 0 again. The figures when this minute's line is
  // due: a minute after the run started, then every minute, whatever the source does meanwhile.
  [[nodiscard]] std::optional<Figures> Note(uint64_t recordings, sources::MotionSource source, Clock::time_point now);
  // A bridge stop or a device removal: the figures, once per run; nullopt when NR has not run since the last report.
  [[nodiscard]] std::optional<Figures> Stop(Clock::time_point now);

 private:
  [[nodiscard]] Figures FiguresAt(Clock::time_point now) const;

  bool running_ = false;
  bool stop_reported_ = false;
  uint64_t recordings_ = 0u;  // the count at the last Note
  sources::MotionSource source_ = sources::MotionSource::NONE;
  uint64_t frames_ = 0u;
  Clock::time_point started_;      // the current source's start
  Clock::time_point run_started_;  // the run's start
  Clock::time_point last_applied_;
  Clock::time_point next_line_;
};

// "Launchpad's motion vectors", "no motion vectors", ...
[[nodiscard]] std::string_view MotionSourceWords(sources::MotionSource source);

// One line: "<lead> 13080 frames with Launchpad's motion vectors in 218 s, of a 300 s run; private 3472 MiB, VRAM 6210 MiB" ("?" for a figure Windows
// did not give).
[[nodiscard]] std::string HeartbeatLine(std::string_view lead, const NrHeartbeat::Figures& figures, std::optional<uint64_t> private_mib,
                                        std::optional<uint64_t> vram_mib);

// This process's private bytes (its commit charge, what Windows' leak detection watches), in MiB.
[[nodiscard]] std::optional<uint64_t> ProcessPrivateMiB();

// This process's own local VRAM on one adapter (DXGI's CurrentUsage), in MiB. The adapter is looked up again only when the LUID changes: under ReShade
// every CreateDXGIFactory2 is logged, which a lookup a minute would repeat.
class ProcessVram {
 public:
  [[nodiscard]] std::optional<uint64_t> MiB(LUID luid);

 private:
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
  std::optional<LUID> luid_;  // the LUID `adapter_` was looked up for (a failed lookup included)
};

}  // namespace uplift::addon
