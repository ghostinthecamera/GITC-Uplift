#pragma once

#include "ngx_hooks/dlss_overrides.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// The hidden DLSS-SR settings never override the game, including keys loaded from a hand-edited INI.
// NgxBridge::BeforeCreate calls this unconditionally and receives no overrides.
[[nodiscard]] ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& settings);

}  // namespace uplift::addon
