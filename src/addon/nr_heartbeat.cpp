#include "addon/nr_heartbeat.hpp"

#include <psapi.h>

#include <format>

namespace uplift::addon {

std::optional<NrHeartbeat::Figures> NrHeartbeat::Note(uint64_t recordings, sources::MotionSource source, Clock::time_point now) {
  const uint64_t added = (recordings >= recordings_ ? recordings - recordings_ : recordings);  // fewer: a new context, counting from 0
  recordings_ = recordings;
  if (added == 0u) return std::nullopt;  // no NR this present: neither counts nor ends the run; a long gap ends it below
  if (!running_ || now - last_applied_ > GAP) {
    running_ = true;
    stop_reported_ = false;
    run_started_ = now;
    next_line_ = now + PERIOD;
    source_ = source;
    frames_ = 0u;
    started_ = now;
  } else if (source != source_) {
    // A new source restarts its own count only: the run, and its line a minute, go on.
    source_ = source;
    frames_ = 0u;
    started_ = now;
  }
  last_applied_ = now;
  frames_ += added;
  if (now < next_line_) return std::nullopt;
  next_line_ = now + PERIOD;
  return FiguresAt(now);
}

std::optional<NrHeartbeat::Figures> NrHeartbeat::Stop(Clock::time_point now) {
  if (!running_ || stop_reported_) return std::nullopt;
  stop_reported_ = true;
  running_ = false;  // the next recording starts a new run
  return FiguresAt(now);
}

NrHeartbeat::Figures NrHeartbeat::FiguresAt(Clock::time_point now) const {
  return {.frames = frames_,
          .source = source_,
          .since = std::chrono::duration_cast<std::chrono::seconds>(now - started_),
          .run = std::chrono::duration_cast<std::chrono::seconds>(now - run_started_)};
}

std::string_view MotionSourceWords(sources::MotionSource source) {
  switch (source) {
    case sources::MotionSource::NONE:         return "no motion vectors";
    case sources::MotionSource::DIRECT:       return "the game's DLSS motion vectors";
    case sources::MotionSource::PRESENT_COPY: return "DLSS's motion vectors (copied for Present)";
    case sources::MotionSource::LAUNCHPAD:    return "Launchpad's motion vectors";
  }
  return "no motion vectors";
}

std::string HeartbeatLine(std::string_view lead, const NrHeartbeat::Figures& figures, std::optional<uint64_t> private_mib,
                          std::optional<uint64_t> vram_mib) {
  const auto mib = [](std::optional<uint64_t> value) { return (value ? std::format("{} MiB", *value) : std::string("?")); };
  return std::format("{} {} frames with {} in {} s, of a {} s run; private {}, VRAM {}", lead, figures.frames, MotionSourceWords(figures.source),
                     figures.since.count(), figures.run.count(), mib(private_mib), mib(vram_mib));
}

std::optional<uint64_t> ProcessPrivateMiB() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) == FALSE) return std::nullopt;
  return static_cast<uint64_t>(counters.PrivateUsage) >> 20u;
}

std::optional<uint64_t> ProcessVram::MiB(LUID luid) {
  if (!luid_ || luid_->LowPart != luid.LowPart || luid_->HighPart != luid.HighPart) {
    luid_ = luid;
    adapter_.Reset();
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory2(0u, IID_PPV_ARGS(&factory)))) {
      factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter_));
    }
  }
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (!adapter_ || FAILED(adapter_->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return std::nullopt;
  return info.CurrentUsage >> 20u;
}

}  // namespace uplift::addon
