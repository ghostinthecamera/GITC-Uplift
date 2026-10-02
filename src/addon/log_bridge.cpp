#include "addon/log_bridge.hpp"

#include <string>
#include <string_view>

#include "addon/reshade_api.hpp"

namespace uplift::addon {
namespace {

static_assert(ToReshadeLogLevel(nr::LogLevel::ERR) == static_cast<int>(reshade::log_level::error));
static_assert(ToReshadeLogLevel(nr::LogLevel::WARN) == static_cast<int>(reshade::log_level::warning));
static_assert(ToReshadeLogLevel(nr::LogLevel::INFO) == static_cast<int>(reshade::log_level::info));
static_assert(ToReshadeLogLevel(nr::LogLevel::TRACE) == static_cast<int>(reshade::log_level::debug));

// ReShade puts an add-on's registered NAME ("GITC Uplift") before each line the add-on logs. Uplift's lines keep their own "[Uplift]"
// prefix instead (what users and the tests search ReShade.log for): the line goes to ReShadeLogMessage with no
// module, as reshade.hpp itself does for a caller that is not an add-on, so ReShade adds no prefix, and the prefix is added here.
void ReshadeSink(nr::LogLevel level, std::string_view message, void* /*user*/) {
  using LogMessage = void(HMODULE, int, const char*);
  static LogMessage* const log_message =
      reinterpret_cast<LogMessage*>(GetProcAddress(reshade::internal::get_reshade_module_handle(), "ReShadeLogMessage"));
  if (log_message == nullptr) return;
  std::string line = "[Uplift] ";
  line.append(message);
  log_message(nullptr, ToReshadeLogLevel(level), line.c_str());
}

}  // namespace

void InstallLogBridge(nr::LogLevel max_level) {
  nr::SetLogSink(&ReshadeSink, nullptr, max_level);
}

void RemoveLogBridge() {
  nr::SetLogSink(nullptr, nullptr, nr::LogLevel::INFO);
}

}  // namespace uplift::addon
