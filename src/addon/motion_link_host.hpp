#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "addon/launchpad_link.hpp"
#include "addon/reshade_api.hpp"
#include "nr/log.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// 2026-10-08 (Lumenite beside Launchpad): the step both add-ons run at every present while the runtime's effects are on, for the two motion sources
// Uplift.fx can compile into UPLIFT_MV. Each source keeps its own LaunchPadLink (its define, its last set value, its own-reload rule) and its own
// LaunchpadReadiness; ChooseUpliftMvSources picks at most one. Header-only: the ReShade calls are the hosts' own.
inline constexpr char UPLIFT_FX_EFFECT[] = "Uplift.fx";
inline constexpr char LAUNCHPAD_DEFINE[] = "UPLIFT_USE_LAUNCHPAD";
inline constexpr char LUMENITE_DEFINE[] = "UPLIFT_USE_LUMENITE";
inline constexpr char LAUNCHPAD_TECHNIQUE_NAME[] = "MartysMods_Launchpad";

struct MotionLinks {
  LaunchPadLink launchpad;
  LaunchPadLink lumenite;
  LaunchpadReadiness launchpad_readiness;  // Setup's "ready", held through the effect reload a link itself causes
  LaunchpadReadiness lumenite_readiness;
  // A new runtime, or its reinit: everything decides afresh.
  void Reset() { *this = {}; }
  // A new preset: the links decide afresh, once; readiness holds.
  void ResetLinks() {
    launchpad.Reset();
    lumenite.Reset();
  }
};

struct MotionLinkStep {
  bool launchpad_ready = false;     // MartysMods_Launchpad and the Uplift technique are enabled (held through a reload)
  bool lumenite_ready = false;      // Lumenite_Kernel and the Uplift technique are enabled (held through a reload)
  bool uplift_mv_lumenite = false;  // Uplift.fx is (or is about to be) compiled with Lumenite's pass: UPLIFT_MV holds Lumenite's vectors
  // 1.2.1: the source Uplift.fx is (or is about to be) compiled with; the readers take UPLIFT_MV only while its technique is on (UpliftMvValid).
  UpliftMvSource compiled = UpliftMvSource::NONE;
};

namespace motion_link_detail {

struct Definition {
  bool effect_scope = false;   // Uplift.fx has its own entry for the define (only an effect ReShade found gets one)
  std::optional<bool> current;  // LaunchPadDefinition: what Uplift.fx compiles with, when known
};

inline Definition Read(reshade::api::effect_runtime* runtime, const char* define) {
  char effect_value[32] = {};
  const bool effect_scope = runtime->get_preprocessor_definition_for_effect(UPLIFT_FX_EFFECT, define, effect_value);
  // 1.0.1 (review I-4): without one at the effect scope Uplift.fx compiles with the preset's or the global definition (a null effect name reads those).
  char outer_value[32] = {};
  const bool outer = (!effect_scope && runtime->get_preprocessor_definition(define, outer_value));
  return {.effect_scope = effect_scope,
          .current = LaunchPadDefinition((effect_scope ? std::optional<std::string_view>(effect_value) : std::nullopt),
                                         (outer ? std::optional<std::string_view>(outer_value) : std::nullopt))};
}

}  // namespace motion_link_detail

// `uplift`: the Uplift technique as found (0 while effects load); `marker`: it is enabled. `launchpad_here` / `lumenite_here`: the device's API can use
// that source (LAUNCHPAD_ON_D3D9, LUMENITE_ON_D3D9). Every set is logged (LaunchPadLinkLine, LumeniteLinkLine) and recompiles Uplift.fx.
inline MotionLinkStep UpdateMotionLinks(MotionLinks* links, reshade::api::effect_runtime* runtime, reshade::api::effect_technique uplift, bool marker,
                                        ui::MotionVectorSource setting, bool launchpad_here, bool lumenite_here, std::chrono::steady_clock::time_point now) {
  namespace api = reshade::api;
  MotionLinkStep step;
  // Ready only once ReShade has finished loading: find_technique and enumerate_techniques find nothing while it loads (reshade-main runtime_api.cpp),
  // which read as "LaunchPad is off" and flipped the value on every reload. And only while Uplift.fx is among its effects: for an effect it does not know,
  // ReShade's set_preprocessor_definition_for_effect falls back to reloading every effect without saving the value, forever.
  bool ready = false;
  if (uplift.handle != 0u) {
    char effect_name[260] = {};
    runtime->get_technique_effect_name(uplift, effect_name);
    ready = (std::string_view(effect_name) == UPLIFT_FX_EFFECT);
  }
  const motion_link_detail::Definition launchpad_define = motion_link_detail::Read(runtime, LAUNCHPAD_DEFINE);
  const motion_link_detail::Definition lumenite_define = motion_link_detail::Read(runtime, LUMENITE_DEFINE);
  // An Uplift.fx that failed to compile with "1" lists no technique. Its own entry for either define still means ReShade knows it, so "0" can go back.
  if (!ready && (launchpad_define.effect_scope || lumenite_define.effect_scope)) {
    runtime->enumerate_techniques(nullptr, [&ready](api::effect_runtime*, api::effect_technique) {
      ready = true;  // not loading
    });
  }
  const api::effect_technique launchpad_technique = (launchpad_here ? runtime->find_technique(nullptr, LAUNCHPAD_TECHNIQUE_NAME) : api::effect_technique{0u});
  const api::effect_technique lumenite_technique = (lumenite_here ? runtime->find_technique(nullptr, LUMENITE_TECHNIQUE) : api::effect_technique{0u});
  const bool launchpad_on = (launchpad_technique.handle != 0u && runtime->get_technique_state(launchpad_technique));
  const bool lumenite_on = (lumenite_technique.handle != 0u && runtime->get_technique_state(lumenite_technique));
  // Plan 14: Setup offers a source only while its technique and the Uplift technique are both enabled (the Uplift technique's own order shows only at run
  // time). ReShade lists no technique while it reloads Uplift.fx, which choosing a source does: the last ready value holds through that.
  step.launchpad_ready = links->launchpad_readiness.Update((uplift.handle != 0u && launchpad_technique.handle != 0u), (marker && launchpad_on), now);
  step.lumenite_ready = links->lumenite_readiness.Update((uplift.handle != 0u && lumenite_technique.handle != 0u), (marker && lumenite_on), now);
  const UpliftMvSources wanted = ChooseUpliftMvSources(setting, launchpad_on, lumenite_on);
  bool launchpad_now = launchpad_define.current.value_or(false);
  bool lumenite_now = lumenite_define.current.value_or(false);
  if (launchpad_here) {
    if (const std::optional<bool> link = links->launchpad.Update({.ready = ready, .wanted = wanted.launchpad, .current = launchpad_define.current})) {
      nr::Log(nr::LogLevel::INFO, LaunchPadLinkLine(*link));
      runtime->set_preprocessor_definition_for_effect(UPLIFT_FX_EFFECT, LAUNCHPAD_DEFINE, (*link ? "1" : "0"));
      launchpad_now = *link;
    }
  }
  if (lumenite_here) {
    if (const std::optional<bool> link = links->lumenite.Update({.ready = ready, .wanted = wanted.lumenite, .current = lumenite_define.current})) {
      nr::Log(nr::LogLevel::INFO, LumeniteLinkLine(*link));
      runtime->set_preprocessor_definition_for_effect(UPLIFT_FX_EFFECT, LUMENITE_DEFINE, (*link ? "1" : "0"));
      lumenite_now = *link;
    }
  }
  // Uplift.fx compiles Launchpad's pass when both are set (its #if order), so UPLIFT_MV is Lumenite's only without Launchpad's.
  step.compiled = CompiledUpliftMv(launchpad_here, launchpad_now, lumenite_here, lumenite_now);
  step.uplift_mv_lumenite = (step.compiled == UpliftMvSource::LUMENITE);
  return step;
}

}  // namespace uplift::addon
