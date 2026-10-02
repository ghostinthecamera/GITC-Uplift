#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace uplift::nr {

enum class LogLevel : uint8_t {
  ERR = 0u,
  WARN = 1u,
  INFO = 2u,
  TRACE = 3u,
};

// A sink must not throw: Log() calls it under a lock and from arbitrary
// threads, including NVIDIA's own (the snippet's log callback and worker
// threads route here). Log() catches and discards anything a sink throws, so
// a throwing sink loses log lines rather than crashing or corrupting state,
// but every sink should still be written as if nothing may throw.
using LogSink = void (*)(LogLevel level, std::string_view message, void* user);

// Routes library diagnostics. A null sink restores the default, which writes
// to OutputDebugStringA with an "[Uplift]" prefix.
void SetLogSink(LogSink sink, void* user, LogLevel max_level);
[[nodiscard]] bool IsLogEnabled(LogLevel level);
void Log(LogLevel level, std::string_view message);

template <class... Args>
void Logf(LogLevel level, std::format_string<Args...> format, Args&&... args) {
  if (!IsLogEnabled(level)) return;
  Log(level, std::format(format, std::forward<Args>(args)...));
}

}  // namespace uplift::nr
