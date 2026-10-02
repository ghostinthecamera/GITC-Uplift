#pragma once

#include "nr/log.hpp"

namespace uplift::addon {

// reshade::log_level values (v6.0.0): error = 1, warning = 2, info = 3, debug = 4.
[[nodiscard]] constexpr int ToReshadeLogLevel(nr::LogLevel level) {
  return static_cast<int>(level) + 1;
}

// Routes nr::Log into ReShade's log up to `max_level`. Each line reads "[Uplift] ..." (not the
// add-on's NAME, "GITC Uplift", which ReShade would put there). Call only after reshade::register_addon succeeded.
void InstallLogBridge(nr::LogLevel max_level);
// Restores nr's default OutputDebugString sink. Call before reshade::unregister_addon.
void RemoveLogBridge();

}  // namespace uplift::addon
