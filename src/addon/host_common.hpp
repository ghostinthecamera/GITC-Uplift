#pragma once

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "addon/launchpad_link.hpp"
#include "addon/motion_link_host.hpp"
#include "addon/reshade_api.hpp"
#include "addon/reshade_vk_host.hpp"
#include "addon/uplift_catch.hpp"
#include "nr/log.hpp"
#include "ui/settings.hpp"
#include "vk/device_hook.hpp"

// The ReShade-host code gitc-uplift.addon64 (addon.cpp) and gitc-uplift.addon32 (addon32.cpp) share.
//
// The two add-ons keep two state types (AddonState with DeviceEntry, AddonState32 with DeviceEntry32) whose shared members carry the same
// names. They have no common base: one would reorder both states' members, and so their destruction order. The shared code is therefore
// written once as templates over the state or entry type. A handler whose whole body is shared takes the add-on's own `g_state` as a reference
// template parameter (`OnInitEffectRuntime<g_state>`), so its `try` and UPLIFT_CATCH exist once too; each instantiation keeps its own
// UPLIFT_CATCH log-once flag, one per handler per module.
//
// Every handler here takes the add-on's lock (`mutex`) itself and holds it for its whole body, except OnCreateDevice's Vulkan branch.
namespace uplift::addon::host {

namespace api = reshade::api;

// The name ReShade shows in its Add-ons list (the NAME export of both add-ons).
inline constexpr char ADDON_NAME[] = "GITC Uplift";
// The description ReShade shows beside it (the DESCRIPTION export of both add-ons).
inline constexpr char ADDON_DESCRIPTION[] =
    "A virtual photography focused version of the DLSS 5 mod: NVIDIA DLSS Neural Rendering (DLSS-NR) with greater "
    "flexibility, ease of use, stability and hassle-free compatibility across APIs.";
// The title of the add-on's overlay tab.
inline constexpr char OVERLAY_TITLE[] = "GITC Uplift";

// Why the vkCreateDevice hook is not installed: logged once from create_device(vulkan), or a startup warning; the argument is the reason.
inline constexpr char VK_DEVICE_HOOK_FAILED_LINE[] = "Vulkan: the vkCreateDevice hook could not be installed: {}";
// The startup warning when the Vulkan device pending marker of the last start is still there.
inline constexpr std::string_view VK_ADJUSTMENT_OFF_WARNING =
    "Vulkan device adjustment turned off (AdjustVulkanDevices = 0): the last start with it did not reach its first frame";

// The technique a preset enables to place NR among its effects (fx/Uplift.fx).
inline constexpr char MARKER_TECHNIQUE[] = "Uplift";
// ReShade's handle of no technique.
inline constexpr api::effect_technique NO_TECHNIQUE{};
// ReShade.ini's Enabled changed under the add-on; the argument is the ini's spelling of the new value.
inline constexpr char ENABLED_FROM_INI_LINE[] = "Enabled set to {} in ReShade.ini";
inline constexpr uint32_t ENABLED_INI_ON = 1u;
inline constexpr uint32_t ENABLED_INI_OFF = 0u;
// ReShade.ini's StartEnabled: NR never starts enabled unless it is set, whatever Enabled the last session left, so a game's start-up (device and
// swap-chain creation, shader compilation) runs without NR. The add-on then writes Enabled=0 back, so the ini, the poller and the panel agree, and
// logs the one line. The e2e runner sets StartEnabled=1 in its own ini, since its scenarios run NR from the first frame on purpose.
inline constexpr char START_ENABLED_KEY[] = "StartEnabled";
inline constexpr bool START_ENABLED_DEFAULT = false;
inline constexpr char START_DISABLED_LINE[] = "NR starts disabled: Enabled was 1 in ReShade.ini and StartEnabled is off";

// Receives create_device(vulkan)'s instance API version (as ReShade codes it), or nothing: nullptr.
using InstanceApiVersionSink = void (*)(uint32_t api_version);

// Pins ReShade's module, once per add-on module, whenever Uplift is pinned (the NGX hooks, the vkCreateDevice detour, the Streamline hooks).
// Best effort: false, logged, when it could not be pinned.
bool PinReshade(HMODULE reshade_module);
// ReShade's module was pinned (a second AddonInit then finds Uplift still registered).
[[nodiscard]] bool ReshadePinned();
// The full path of `handle`'s module; the game's executable for nullptr.
[[nodiscard]] std::filesystem::path ModulePath(HMODULE handle);
// The Vulkan loader is loaded in this process.
[[nodiscard]] bool VulkanLoaderLoaded();
// The API's name in the texts.
[[nodiscard]] std::string_view ApiName(api::device_api device_api);

// DllMain: outside ReShade the add-on refuses to load.
[[nodiscard]] inline bool RefusesLoad(DWORD reason) {
  return reason == DLL_PROCESS_ATTACH && reshade::internal::get_reshade_module_handle() == nullptr;
}
// DllMain: the process is ending (DLL_PROCESS_DETACH with a non-null reserved), not a FreeLibrary. What runs then takes no lock and allocates
// nothing: ExitProcess may have ended a thread that held one.
[[nodiscard]] inline bool ProcessEnding(DWORD reason, LPVOID reserved) { return reason == DLL_PROCESS_DETACH && reserved != nullptr; }
// The Vulkan device pending marker: next to the removal-latch marker, per game executable, with its own extension.
[[nodiscard]] std::filesystem::path VkDevicePendingMarkerPath(const std::filesystem::path& state_directory, const std::filesystem::path& game_exe_path);

// The pending marker's verdict that the last start with an adjusted Vulkan device never reached its first frame: the adjustment goes
// (AdjustVulkanDevices = 0, with its startup latch), saved now. Returns the startup warning that says so.
template <class State>
std::string_view TurnDeviceAdjustmentOff(State& state) {
  state.settings.startup_latches |= ui::Settings::LATCH_VK_DEVICE;
  state.settings.adjust_vulkan_devices = false;
  ui::SaveSettings(state.settings, &state.config);
  return VK_ADJUSTMENT_OFF_WARNING;
}

// Installs the vkCreateDevice hook. On failure the reason is kept in `vk_hook_error` (an unseen device's Details line says why), under its
// leaf lock, never the add-on's, and returned.
template <class State>
std::optional<std::string> InstallDeviceHook(State& state) {
  std::optional<std::string> error = vk::DeviceHook::Install();
  if (error) {
    const std::scoped_lock error_lock(state.vk_hook_error_mutex);
    state.vk_hook_error = *error;
  }
  return error;
}

// AddonInit's install of the vkCreateDevice hook, only under a ReShade that raises no Vulkan create events (older than MIN_RESHADE_VULKAN_EVENTS) and only while
// the Vulkan loader is loaded: ReShade's Vulkan layer runs AddonInit inside the game's vkCreateInstance, before any device exists, so a detour
// installed now is in time for every one. A ReShade that raises the events raises create_device(vulkan) first in every vkCreateInstance, so
// OnCreateDevice's install is always in time, and installing here too would only bring the detour, the pins and MinHook (which suspends every
// thread) into Direct3D games that happen to have vulkan-1.dll loaded. True when it installed the hook now; a failure goes to `warnings`.
template <class State>
bool InstallDeviceHookWithoutVulkanEvents(State& state, bool reshade_vulkan_events, std::vector<std::string>* warnings) {
  if (reshade_vulkan_events || !VulkanLoaderLoaded()) return false;
  if (const std::optional<std::string> error = InstallDeviceHook(state)) {
    warnings->push_back(std::format(VK_DEVICE_HOOK_FAILED_LINE, *error));
    return false;
  }
  return true;
}

// ReShade's create_device event, registered raw in AddonInit (RegisterCreateDeviceEvent).
// Vulkan: a ReShade that raises the Vulkan create events raises it in every vkCreateInstance right after loading its add-ons and before any
// device can exist, so this is where the vkCreateDevice detour goes in (AddonInit installs it only for an older ReShade). Lock-free: the hook's state has its own lock, never this
// add-on's; the one thing touched of the add-on's state is the leaf lock of the hook's error text. `NOTE_INSTANCE_API_VERSION` receives the
// instance's API version first, when the add-on names one.
// Direct3D 9: the helper front answers for the 9Ex toggle (a device with UseD3D9Ex on).
// It must not take the add-on's lock for any other API: ReShade raises create_device inside D3D11CreateDevice (which OnPresent calls for a
// Direct3D 10 game's relay) and D3D12CreateDevice (the 64-bit bridges' private devices, on the fallback without a device factory) too, while
// OnPresent holds the lock exclusively, and the lock is not recursive. Nothing Uplift does creates a Direct3D 9 device under it (D3D9Client
// only calls Direct3DCreate9Ex, which raises no event).
template <auto& STATE, InstanceApiVersionSink NOTE_INSTANCE_API_VERSION>
bool OnCreateDevice(api::device_api device_api, uint32_t& api_version) {
  if (device_api == api::device_api::vulkan) {
    try {
      if constexpr (NOTE_INSTANCE_API_VERSION != nullptr) {
        NOTE_INSTANCE_API_VERSION(api_version);
      }
      if (const std::optional<std::string> error = InstallDeviceHook(*STATE)) {
        static std::atomic<bool> logged = false;
        if (!logged.exchange(true)) {
          nr::Logf(nr::LogLevel::WARN, VK_DEVICE_HOOK_FAILED_LINE, *error);
        }
      } else {
        PinReshade(reshade::internal::get_reshade_module_handle());  // the detour pinned Uplift, so ReShade stays loaded next to it
      }
    }
    UPLIFT_CATCH("create_device", false)
    return false;
  }
  if (device_api != api::device_api::d3d9) return false;
  try {
    const std::unique_lock lock(STATE->mutex);
    return STATE->front->CreateDevice(device_api, api_version, STATE->settings.use_d3d9ex);
  }
  UPLIFT_CATCH("create_device", false)
}

// Both add-ons' start, after the settings are loaded and the log bridge is up: Enabled off unless StartEnabled says otherwise, and the poller
// told what was applied, so it reports the ini's next change only.
template <class State>
void ApplyStartEnabled(State& state) {
  const std::optional<std::string> start_enabled = state.config.Get(START_ENABLED_KEY);
  const bool may_start_enabled = (start_enabled ? ui::ParseBoolSetting(*start_enabled).value_or(START_ENABLED_DEFAULT) : START_ENABLED_DEFAULT);
  if (state.settings.enabled && !may_start_enabled) {
    state.settings.enabled = false;
    state.config.Set(ui::ENABLED_KEY, std::to_string(ENABLED_INI_OFF));
    nr::Log(nr::LogLevel::INFO, START_DISABLED_LINE);
  }
  state.poller.Seen(state.settings.enabled);
}

// ReShade.ini's Enabled, re-read at most once per poll period, so a tool that writes [Uplift] Enabled through ReShade's config API toggles NR.
// Reads only that key: a missing or unparsable value leaves `settings.enabled` alone. A change goes to the helper, whose copy reads it too;
// it is not one of the add-on's own setting changes (the 64-bit settings generation stays).
template <class State>
void PollEnabled(State& state, std::chrono::steady_clock::time_point now) {
  const std::optional<bool> enabled_in_config = state.poller.Poll(now, [&state]() -> std::optional<bool> {
    const std::optional<std::string> text = state.config.Get(ui::ENABLED_KEY);
    if (!text) return std::nullopt;
    return ui::ParseBoolSetting(*text);
  });
  if (enabled_in_config) {
    state.settings.enabled = *enabled_in_config;
    nr::Logf(nr::LogLevel::INFO, ENABLED_FROM_INI_LINE, (*enabled_in_config ? ENABLED_INI_ON : ENABLED_INI_OFF));
    state.front->SettingsChanged(state.settings);
  }
}

// A Vulkan device's present until DeviceHook::NotePresent has finished counting. Before the selector, like the 9Ex marker: the second present
// event of an adjusted device deletes the pending marker.
template <class Entry>
void CountVulkanPresent(Entry& entry, api::device* device) {
  if (!entry.vk_presents_counted) {
    entry.vk_presents_counted = vk::DeviceHook::NotePresent(VulkanHandleOf<VkDevice>(device->get_native()));
  }
}

// The presenting queue, and the effect runtime of the presenting swap chain (null while it has none).
template <class Entry>
void BindPresentRuntime(Entry& entry, const std::vector<api::effect_runtime*>& runtimes, api::command_queue* queue, api::swapchain* swapchain) {
  entry.queue = queue;
  entry.runtime = nullptr;
  for (api::effect_runtime* const runtime : runtimes) {
    if (runtime->get_native() == swapchain->get_native()) {
      entry.runtime = runtime;
      break;
    }
  }
}

// `effect_queue` (Vulkan and OpenGL): Uplift runs on the effect runtime's queue, whose immediate command list the technique and finish-effects
// events hand out, whatever queue presents; `present_queue` keeps the present event's. Without a runtime (effects not up yet) there is none,
// and nothing runs this frame. On OpenGL that queue is the runtime's context: the present is handled only when it is the one that presents,
// with that context current. Then the frame's back buffer.
template <class Entry>
void BindPresentQueues(Entry& entry, api::command_queue* queue, api::swapchain* swapchain, bool effect_queue) {
  if (effect_queue) {
    entry.present_queue = queue;
    entry.queue = (entry.runtime != nullptr ? entry.runtime->get_command_queue() : nullptr);
  }
  entry.back_buffer = swapchain->get_current_back_buffer();
}

// This frame's Uplift technique, and the automatic motion links. Uplift links itself to LaunchPad (Direct3D 10 and newer; Direct3D 9 too, while
// LAUNCHPAD_ON_D3D9 holds) and to Lumenite's Kernel (Direct3D 10 and newer, LUMENITE_ON_D3D9 for Direct3D 9), so the user never edits a
// preprocessor definition by hand; LaunchPadLink decides when, UpdateMotionLinks runs both. A runtime the links did not see before starts them
// afresh. The step for Setup's readiness and the compiled source; nullopt when the links did not run (no runtime, effects off or loading, or
// neither link here).
template <class Entry>
std::optional<MotionLinkStep> LinkMotion(Entry& entry, ui::MotionVectorSource motion_vectors, bool d3d9, std::chrono::steady_clock::time_point now) {
  entry.marker = NO_TECHNIQUE;
  if (entry.runtime == nullptr || !entry.runtime->get_effects_state()) return std::nullopt;
  // find_technique finds nothing while effects are still loading: NR then runs before effects.
  const api::effect_technique technique = entry.runtime->find_technique(nullptr, MARKER_TECHNIQUE);
  if (technique != NO_TECHNIQUE && entry.runtime->get_technique_state(technique)) {
    entry.marker = technique;
  }
  const bool launchpad_here = (!d3d9 || LAUNCHPAD_ON_D3D9);
  const bool lumenite_here = (!d3d9 || LUMENITE_ON_D3D9);
  if (!launchpad_here && !lumenite_here) return std::nullopt;
  if (entry.link_runtime != entry.runtime) {
    entry.motion_links.Reset();
    entry.link_runtime = entry.runtime;
    entry.link_preset_path.clear();
  }
  return UpdateMotionLinks(&entry.motion_links, entry.runtime, technique, (entry.marker != NO_TECHNIQUE), motion_vectors, launchpad_here,
                           lumenite_here, now);
}

// Setup's Launchpad and Lumenite readiness and Uplift.fx's compiled source from one present's link step, into a device record (the 64-bit
// entry, a HelperDevice).
template <class Target>
void NoteMotionLinkStep(const MotionLinkStep& step, Target* target) {
  target->launchpad_ready = step.launchpad_ready;
  target->lumenite_ready = step.lumenite_ready;
  target->uplift_mv_lumenite = step.uplift_mv_lumenite;
  target->uplift_mv_source = step.compiled;
}

template <auto& STATE>
void OnInitEffectRuntime(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(STATE->mutex);
    STATE->runtimes.push_back(runtime);
  }
  UPLIFT_CATCH("init_effect_runtime", )
}

template <auto& STATE>
void OnEffectsReloaded(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(STATE->mutex);
    const auto found = STATE->devices.find(runtime->get_device());
    if (found != STATE->devices.end() && found->second.runtime == runtime) {
      found->second.motion_links.ResetEarly();
      found->second.marker = {};
    }
  }
  UPLIFT_CATCH("reloaded_effects", )
}

template <auto& STATE>
void OnDestroyEffectRuntime(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(STATE->mutex);
    std::erase(STATE->runtimes, runtime);
    for ([[maybe_unused]] auto& [device, entry] : STATE->devices) {
      if (entry.runtime == runtime) {
        entry.runtime = nullptr;
      }
      // ReShade reinitialises a runtime in place (a resize), under the same pointer: its link starts afresh.
      if (entry.link_runtime == runtime) {
        entry.motion_links.Reset();
        entry.link_runtime = nullptr;
        entry.link_preset_path.clear();
      }
    }
  }
  UPLIFT_CATCH("destroy_effect_runtime", )
}

// A new preset is linked afresh once. ReShade raises this after every reload too (the link's own
// included), with the same path, which must not reset anything.
template <auto& STATE>
void OnSetCurrentPresetPath(api::effect_runtime* runtime, const char* path) {
  try {
    const std::unique_lock lock(STATE->mutex);
    for ([[maybe_unused]] auto& [device, entry] : STATE->devices) {
      if (entry.link_runtime == runtime && path != nullptr && entry.link_preset_path != path) {
        entry.link_preset_path = path;
        entry.motion_links.ResetLinks();
      }
    }
  }
  UPLIFT_CATCH("set_current_preset_path", )
}

template <auto& STATE>
bool OnOpenOverlay(api::effect_runtime* /*runtime*/, bool open, api::input_source /*source*/) {
  try {
    const std::unique_lock lock(STATE->mutex);
    if (!open) {
      // defaults_view is session-only and survives closing the overlay; a slider held as
      // it closes must not stay coalesced, so everything else about the panel still resets.
      const bool defaults_view = STATE->overlay.defaults_view;
      STATE->overlay = {};
      STATE->overlay.defaults_view = defaults_view;
    }
    return false;
  }
  UPLIFT_CATCH("open_overlay", false)
}

}  // namespace uplift::addon::host
