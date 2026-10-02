#include "addon/dlss_settings.hpp"

namespace uplift::addon {

// Minor (fix round 2, user decision): the DLSS-SR page's rows are hidden, and the user does not want
// a hand-edited ini key honoured either, so no override is ever returned -- whatever
// settings.dlss_quality_mode, dlss_presets or dlss_auto_exposure hold. Keeps ToSrOverrides's
// signature: NgxBridge::BeforeCreate calls it unconditionally, and a later plan may bring the page
// back, at which point the mapping this replaced (DlssQualityOverride/DlssPresetOverride onto
// NVSDK_NGX_PerfQuality_Value and the J-M render presets, spec §10) returns too.
ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& /*settings*/) {
  return {};
}

}  // namespace uplift::addon
