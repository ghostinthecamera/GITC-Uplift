#include "nr/log.hpp"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace uplift::nr {
namespace {

void DefaultSink(LogLevel level, std::string_view message, void* /*user*/) {
  constexpr std::string_view PREFIXES[] = {
      "[Uplift] error: ",
      "[Uplift] warning: ",
      "[Uplift] ",
      "[Uplift] trace: ",
  };
  std::string line(PREFIXES[static_cast<size_t>(level)]);
  line.append(message);
  line.push_back('\n');
  OutputDebugStringA(line.c_str());
}

struct SinkState {
  std::mutex mutex;
  LogSink sink = &DefaultSink;
  void* user = nullptr;
  std::atomic<uint8_t> max_level = static_cast<uint8_t>(LogLevel::INFO);
};

SinkState& State() {
  static SinkState state;
  return state;
}

}  // namespace

void SetLogSink(LogSink sink, void* user, LogLevel max_level) {
  auto& state = State();
  const std::scoped_lock lock(state.mutex);
  state.sink = (sink != nullptr ? sink : &DefaultSink);
  state.user = user;
  state.max_level.store(static_cast<uint8_t>(max_level));
}

bool IsLogEnabled(LogLevel level) {
  return static_cast<uint8_t>(level) <= State().max_level.load();
}

void Log(LogLevel level, std::string_view message) {
  if (!IsLogEnabled(level)) return;
  auto& state = State();
  try {
    const std::scoped_lock lock(state.mutex);
    state.sink(level, message, state.user);
  } catch (...) {
    // A sink must not throw (log.hpp); discard rather than propagate into a
    // caller that may be an NVIDIA frame or mid-state-change. The lock is
    // already released here, so this block may safely log in future.
  }
}

}  // namespace uplift::nr
