#include "addon/dlss_settings.hpp"

namespace uplift::addon {

// The hidden DLSS-SR settings never override the game, including keys loaded from a hand-edited INI.
// NgxBridge::BeforeCreate calls this unconditionally and receives no overrides.
ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& /*settings*/) {
  return {};
}

}  // namespace uplift::addon
