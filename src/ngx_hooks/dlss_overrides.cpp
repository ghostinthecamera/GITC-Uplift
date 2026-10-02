#include "ngx_hooks/dlss_overrides.hpp"

#include <algorithm>

#include "ngx_hooks/ngx_parameters.hpp"

namespace uplift::ngx_hooks {

bool SrOverrides::Any() const {
  return perf_quality >= 0 || auto_exposure.has_value()
         || std::ranges::any_of(presets, [](unsigned int preset) { return preset != 0u; });
}

void ApplySrOverrides(NVSDK_NGX_Parameter* parameters, const SrOverrides& overrides, std::vector<SavedParameter>* saved) {
  if (overrides.perf_quality >= 0) {
    saved->push_back({.key = NVSDK_NGX_Parameter_PerfQualityValue,
                      .type = SavedParameter::Type::INT,
                      .value = ReadInt(*parameters, NVSDK_NGX_Parameter_PerfQualityValue).value_or(0)});
    parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, overrides.perf_quality);
  }
  for (size_t mode = 0u; mode < PRESET_KEYS.size(); ++mode) {
    if (overrides.presets[mode] == 0u) continue;
    saved->push_back({.key = PRESET_KEYS[mode],
                      .type = SavedParameter::Type::UINT,
                      .value = ReadUint(*parameters, PRESET_KEYS[mode]).value_or(0u)});
    parameters->Set(PRESET_KEYS[mode], overrides.presets[mode]);
  }
  if (overrides.auto_exposure.has_value()) {
    const int flags = ReadInt(*parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags).value_or(0);
    const int auto_exposure = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const int wanted = (*overrides.auto_exposure ? (flags | auto_exposure) : (flags & ~auto_exposure));
    if (wanted != flags) {
      saved->push_back({.key = NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, .type = SavedParameter::Type::INT, .value = flags});
      parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, wanted);
    }
  }
}

}  // namespace uplift::ngx_hooks
