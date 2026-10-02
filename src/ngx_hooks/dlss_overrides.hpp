#pragma once

#include <array>
#include <optional>
#include <vector>

#include "ngx_hooks/hook_installer.hpp"
#include "nr/ngx.hpp"

namespace uplift::ngx_hooks {

// The DLSS-SR page's create overrides (spec §10). The key mapping follows the NGX SDK header.
struct SrOverrides {
  int perf_quality = -1;                     // an NVSDK_NGX_PerfQuality_Value to force; -1 keeps the game's
  std::array<unsigned int, 6> presets = {};  // per PRESET_KEYS entry: a render preset to force; 0 keeps the game's
  std::optional<bool> auto_exposure;         // force NVSDK_NGX_DLSS_Feature_Flags_AutoExposure on or off

  [[nodiscard]] bool Any() const;
};

// DLSS.Hint.Render.Preset.<Mode>, in SrOverrides::presets and ui::DlssPresetMode order.
inline constexpr std::array<const char*, 6> PRESET_KEYS = {
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
};

// Applies `overrides` to a SuperSampling create's parameter block. The game's value of every key it
// changes goes into `saved` first, so the create detour can put it back (Task 1). The create helper
// writes PerfQualityValue and the create flags as int; games write presets as unsigned int.
void ApplySrOverrides(NVSDK_NGX_Parameter* parameters, const SrOverrides& overrides, std::vector<SavedParameter>* saved);

}  // namespace uplift::ngx_hooks
