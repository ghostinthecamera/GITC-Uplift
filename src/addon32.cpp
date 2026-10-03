// gitc-uplift.addon32 (Plan 9, design docs/superpowers/specs/2026-09-29-uplift-32bit-helper-design.md): Uplift for 32-bit
// Direct3D 9, 10 (Plan 10), 11, 12 (Plan 12, dgVoodoo2's D3D12 output), Vulkan (Plan 11, DXVK included) and OpenGL (Plan 12) games. This add-on owns what only ReShade can give (its events, the settings and the panel, the frame's
// trigger, the capture) and lets gitc-uplift-helper64.exe -- started by itself, once NR is turned on -- run NR: no NVIDIA code ever
// loads in the game's process. It mirrors addon.cpp where it can, with the same names. Plan 10 (design §2.2): the per-device logic
// lives in addon::HelperFront, which gitc-uplift.addon64 hosts for its Direct3D 9 devices too; this file is its thin host.
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include "addon/helper_front.hpp"
#include "addon/launchpad_link.hpp"
#include "addon/live_facts.hpp"
#include "addon/log_bridge.hpp"
#include "addon/nr_claim.hpp"
#include "addon/removal_latch.hpp"
#include "addon/reshade_api.hpp"
#include "addon/reshade_config_store.hpp"
#include "addon/reshade_version.hpp"
#include "addon/reshade_vk_host.hpp"
#include "addon/swapchain_selector.hpp"
#include "addon/swapchain_usage.hpp"
#include "addon/uplift_catch.hpp"
#include "build_id.hpp"
#include "nr/log.hpp"
#include "ui/overlay.hpp"
#include "ui/settings.hpp"
#include "ui/settings_schema.hpp"
#include "ui/status_text.hpp"
#include "vk/device_hook.hpp"

// The name ReShade shows in its Add-ons list. The log lines keep their "[Uplift]" prefix (addon/log_bridge.cpp).
extern "C" __declspec(dllexport) const char* NAME = "GITC Uplift";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "A virtual photography focused version of the DLSS 5 mod: NVIDIA DLSS Neural Rendering (DLSS-NR) with greater "
    "flexibility, ease of use, stability and hassle-free compatibility across APIs.";

namespace {

namespace addon = uplift::addon;
namespace client = uplift::client;
namespace nr = uplift::nr;
namespace ui = uplift::ui;
namespace vk = uplift::vk;
namespace api = reshade::api;

constexpr std::string_view UPLIFT_VERSION = UPLIFT_VERSION_TEXT;  // project(VERSION) in CMakeLists.txt
constexpr char MARKER_TECHNIQUE[] = "Uplift";
constexpr char LAUNCHPAD_TECHNIQUE[] = "MartysMods_Launchpad";
constexpr char UPLIFT_FX_EFFECT_NAME[] = "Uplift.fx";
constexpr char UPLIFT_USE_LAUNCHPAD_DEFINE[] = "UPLIFT_USE_LAUNCHPAD";
constexpr char ADDON_FILE[] = "gitc-uplift.addon32";

struct DeviceEntry32 {
  addon::SwapchainSelector selector;
  // The frame in flight on the primary swap chain, set in the present event.
  api::command_queue* queue = nullptr;
  api::effect_runtime* runtime = nullptr;
  api::resource back_buffer = {0u};
  api::effect_technique marker = {0u};
  // Final review I-1 of Plan 6: the automatic UPLIFT_USE_LAUNCHPAD link (Direct3D 11 only), for `link_runtime`.
  addon::LaunchPadLink launchpad_link;
  addon::LaunchpadReadiness launchpad_readiness;  // Plan 14: Setup's "ready", held through the effect reload the link itself causes
  api::effect_runtime* link_runtime = nullptr;
  std::string link_preset_path;
  addon::HelperDevice helper;  // Plan 10: the clients and everything else about NR in the helper
  // Plans 11 and 12 (Vulkan design §3.1, key decision g): on Vulkan and OpenGL `queue` is the effect runtime's, and this is the present event's.
  api::command_queue* present_queue = nullptr;
  bool vk_presents_counted = false;  // DeviceHook::NotePresent has finished counting (the pending marker's second present)
};

struct AddonState32 {
  std::shared_mutex mutex;
  addon::ReshadeConfigStore config;
  ui::Settings settings;
  ui::EnabledPoller poller;
  ui::OverlayState overlay;
  std::unordered_map<api::device*, DeviceEntry32> devices;
  std::vector<api::effect_runtime*> runtimes;
  addon::NrClaim claim;  // at most one device runs NR
  // Made in AddonInit. One helper per process, made at the first present that needs it, for the device that owns the claim; its
  // Quit ends it at AddonUninit.
  std::optional<addon::HelperFront> front;
  // Plan 11: why the vkCreateDevice hook is not installed (a Vulkan device then runs CPU-ordered); empty when it is or was not tried. Written by AddonInit
  // and by create_device(vulkan), which takes no other lock of this add-on (Plan 10 C-1), so it has a lock of its own: a leaf, never held across a call.
  std::mutex vk_hook_error_mutex;
  std::string vk_hook_error;
};

// Created in AddonInit and never freed (as addon.cpp's).
AddonState32* g_state = nullptr;

// Batch 2 review I-4 (addon.cpp's twin): ReShade's module is pinned (once) whenever the vkCreateDevice detour pins Uplift, as ReShade's own
// [INSTALL] PreventUnloading does. The Vulkan loader closes ReShade's layer library with the last instance, and this module's cached ReShade function
// pointers (the log bridge among them, which the detour logs through) would point into the old image. Best effort: pinning a loaded module's own address
// does not fail in practice (addon.cpp has the details).
std::atomic<bool> g_reshade_pinned{false};

bool PinReshade(HMODULE reshade_module) {
  if (g_reshade_pinned.load(std::memory_order_acquire)) return true;
  HMODULE pinned = nullptr;
  if (reshade_module == nullptr
      || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(reshade_module), &pinned)
             == FALSE) {
    nr::Logf(nr::LogLevel::WARN, "could not pin ReShade's module (error {}): if ReShade unloads with the last Vulkan instance, Uplift is registered again at the next one",
             GetLastError());
    return false;
  }
  g_reshade_pinned.store(true, std::memory_order_release);
  return true;
}

// After the overlay or the hotkey changed a setting.
void SaveAndApply(AddonState32* state) {
  state->settings = ui::Sanitized(state->settings);
  ui::SaveSettings(state->settings, &state->config);
  state->poller.Seen(state->settings.enabled);
  addon::InstallLogBridge(state->settings.log_level);
  state->front->SettingsChanged(state->settings);
}

// The device entry whose primary swap chain presents through `runtime`, when `cmd_list` is that frame's immediate list.
DeviceEntry32* FindFrameEntry(AddonState32* state, api::effect_runtime* runtime, api::command_list* cmd_list) {
  const auto found = state->devices.find(runtime->get_device());
  if (found == state->devices.end()) return nullptr;
  [[maybe_unused]] auto& [device, entry] = *found;
  if (!entry.helper.HasClient() || entry.runtime != runtime || entry.queue == nullptr) return nullptr;
  if (cmd_list != entry.queue->get_immediate_command_list()) return nullptr;
  return &entry;
}

void OnInitEffectRuntime(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    g_state->runtimes.push_back(runtime);
  }
  UPLIFT_CATCH("init_effect_runtime", )
}

void OnDestroyEffectRuntime(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    std::erase(g_state->runtimes, runtime);
    for ([[maybe_unused]] auto& [device, entry] : g_state->devices) {
      if (entry.runtime == runtime) {
        entry.runtime = nullptr;
      }
      // ReShade reinitialises a runtime in place (a resize), under the same pointer: its link starts afresh.
      if (entry.link_runtime == runtime) {
        entry.launchpad_link.Reset();
        entry.launchpad_readiness.Reset();
        entry.link_runtime = nullptr;
        entry.link_preset_path.clear();
      }
    }
  }
  UPLIFT_CATCH("destroy_effect_runtime", )
}

// Final review I-1: a new preset is linked afresh once. ReShade raises this after every reload too (the link's own included),
// with the same path, which must not reset anything.
void OnSetCurrentPresetPath(api::effect_runtime* runtime, const char* path) {
  try {
    const std::unique_lock lock(g_state->mutex);
    for ([[maybe_unused]] auto& [device, entry] : g_state->devices) {
      if (entry.link_runtime == runtime && path != nullptr && entry.link_preset_path != path) {
        entry.link_preset_path = path;
        entry.launchpad_link.Reset();
      }
    }
  }
  UPLIFT_CATCH("set_current_preset_path", )
}

// Design §2.7: a Direct3D 9 Reset raises destroy_command_queue, not destroy_device (see HelperFront::DestroyQueue).
void OnDestroyCommandQueue(api::command_queue* queue) {
  try {
    api::device* const device = queue->get_device();
    if (device->get_api() != api::device_api::d3d9) return;
    const std::unique_lock lock(g_state->mutex);
    const auto found = g_state->devices.find(device);
    if (found == g_state->devices.end()) return;
    g_state->front->DestroyQueue(found->second.helper, device);
  }
  UPLIFT_CATCH("destroy_command_queue", )
}

// Design §2.7: the device is going away. Everything this add-on made on it is released and the helper detached (a Direct3D 9
// device already did both at destroy_command_queue).
void OnDestroyDevice(api::device* device) {
  try {
    // Plan 10 (design §3.1): a Direct3D 10 device's client goes after the lock. It releases the relay ReShade returned (its proxy), whose
    // destroy_device comes back into this handler on this thread.
    std::unique_ptr<client::D3D10Client> d3d10;
    {
      const std::unique_lock lock(g_state->mutex);
      const auto found = g_state->devices.find(device);
      d3d10 = g_state->front->DestroyDevice((found != g_state->devices.end() ? &found->second.helper : nullptr), device);
      if (device->get_api() == api::device_api::vulkan) {
        // The pending marker's other end (Vulkan design §2.3): every Vulkan device, also one that never presented. The hook's own lock, never this one's.
        vk::DeviceHook::NoteDestroyed(addon::VulkanHandleOf<VkDevice>(device->get_native()));
      }
      if (found != g_state->devices.end()) {
        g_state->devices.erase(found);
      }
      g_state->claim.Forget(device);
    }
  }
  UPLIFT_CATCH("destroy_device", )
}

bool OnOpenOverlay(api::effect_runtime* /*runtime*/, bool open, api::input_source /*source*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    if (!open) {
      // Plan 6 (D6): defaults_view is session-only and survives closing the overlay; a slider held as it closes must not stay
      // coalesced, so everything else about the panel still resets.
      const bool defaults_view = g_state->overlay.defaults_view;
      g_state->overlay = {};
      g_state->overlay.defaults_view = defaults_view;
    }
    return false;
  }
  UPLIFT_CATCH("open_overlay", false)
}

void OnPresent(api::command_queue* queue, api::swapchain* swapchain, const api::rect* /*source_rect*/,
               const api::rect* /*dest_rect*/, uint32_t /*dirty_rect_count*/, const api::rect* /*dirty_rects*/) {
  try {
    api::device* const device = swapchain->get_device();
    const api::device_api device_api = device->get_api();
    if (device_api != api::device_api::d3d9 && device_api != api::device_api::d3d10 && device_api != api::device_api::d3d11
        && device_api != api::device_api::d3d12 && device_api != api::device_api::vulkan && device_api != api::device_api::opengl) {
      return;  // the overlay says so
    }
    const bool d3d9 = (device_api == api::device_api::d3d9);
    const bool vulkan = (device_api == api::device_api::vulkan);
    const bool opengl = (device_api == api::device_api::opengl);  // Plan 12: the same queues as Vulkan, on a GL context
    const bool d3d12 = (device_api == api::device_api::d3d12);    // Plan 12: the present event's queue is the runtime's
    const auto now = std::chrono::steady_clock::now();
    // Plan 10 (design §3.1): what D3D11CreateDevice returned for a new Direct3D 10 client's relay (ReShade's proxy in a game). Its last release
    // raises destroy_device, whose handler takes the add-on's lock. Declared before the lock, it is released after it.
    Microsoft::WRL::ComPtr<ID3D11Device> relay_after_unlock;
    const std::unique_lock lock(g_state->mutex);
    AddonState32& state = *g_state;

    // Reads only the Enabled key: a missing or unparsable value must leave `settings.enabled` alone.
    const std::optional<bool> enabled_in_config = state.poller.Poll(now, [&state]() -> std::optional<bool> {
      const std::optional<std::string> text = state.config.Get(ui::ENABLED_KEY);
      if (!text) return std::nullopt;
      return ui::ParseBoolSetting(*text);
    });
    if (enabled_in_config) {
      state.settings.enabled = *enabled_in_config;
      nr::Logf(nr::LogLevel::INFO, "Enabled set to {} in ReShade.ini", (*enabled_in_config ? 1 : 0));
      state.front->SettingsChanged(state.settings);  // the helper's copy reads it too
    }
    const ui::Settings& settings = state.settings;

    DeviceEntry32& entry = state.devices[device];
    if (d3d9) {
      state.front->NotePresent(entry.helper, device, settings);
    }
    if (vulkan && !entry.vk_presents_counted) {
      // Before the selector, like the 9Ex marker: the second present event of an adjusted device deletes the pending marker (design §2.3).
      entry.vk_presents_counted = vk::DeviceHook::NotePresent(addon::VulkanHandleOf<VkDevice>(device->get_native()));
    }
    if (!entry.selector.OnPresent(swapchain, now)) return;
    // This bookkeeping runs before the client exists, so a device waiting for its first enable still has `entry.runtime`,
    // which the Enable hotkey and the overlay need.
    entry.queue = queue;
    entry.runtime = nullptr;
    for (api::effect_runtime* const runtime : state.runtimes) {
      if (runtime->get_native() == swapchain->get_native()) {
        entry.runtime = runtime;
        break;
      }
    }
    if (vulkan || opengl) {
      // Key decision g: Uplift runs on the effect runtime's queue, whose immediate command list the technique and finish-effects events hand out,
      // whatever queue presents. Without a runtime (effects not up yet) there is none, and nothing runs this frame. On OpenGL (key decision a) that queue is
      // the runtime's context: the front handles the present only when it is the one that presents, with that context current.
      entry.present_queue = queue;
      entry.queue = (entry.runtime != nullptr ? entry.runtime->get_command_queue() : nullptr);
    }
    entry.back_buffer = swapchain->get_current_back_buffer();
    entry.marker = {0u};
    entry.helper.launchpad_ready = false;  // Plan 14: Setup's Launchpad option, set below while the effects are loaded
    if (entry.runtime != nullptr && entry.runtime->get_effects_state()) {
      // find_technique returns 0 while effects are still loading: NR then runs before effects.
      const api::effect_technique technique = entry.runtime->find_technique(nullptr, MARKER_TECHNIQUE);
      if (technique.handle != 0u && entry.runtime->get_technique_state(technique)) {
        entry.marker = technique;
      }
      if (!d3d9 || addon::LAUNCHPAD_ON_D3D9) {
        // Uplift links itself to LaunchPad (Direct3D 10 and newer; Plan 10: Direct3D 9 too, while addon::LAUNCHPAD_ON_D3D9 holds, the one
        // switch in launchpad_link.hpp), so the user never edits a preprocessor definition by hand.
        // LaunchPadLink decides when (final review I-1 of Plan 6); see addon.cpp for the reasoning.
        if (entry.link_runtime != entry.runtime) {
          entry.launchpad_link.Reset();
          entry.launchpad_readiness.Reset();
          entry.link_runtime = entry.runtime;
          entry.link_preset_path.clear();
        }
        bool link_ready = false;
        if (technique.handle != 0u) {
          char effect_name[MAX_PATH] = {};
          entry.runtime->get_technique_effect_name(technique, effect_name);
          link_ready = (std::string_view(effect_name) == UPLIFT_FX_EFFECT_NAME);
        }
        char link_value[32] = {};
        const bool link_defined =
            entry.runtime->get_preprocessor_definition_for_effect(UPLIFT_FX_EFFECT_NAME, UPLIFT_USE_LAUNCHPAD_DEFINE, link_value);
        // 1.0.1 (review I-4): without one at the effect scope Uplift.fx compiles with the preset's or the global definition (a null effect name reads those).
        char outer_value[32] = {};
        const bool outer_defined = (!link_defined && entry.runtime->get_preprocessor_definition(UPLIFT_USE_LAUNCHPAD_DEFINE, outer_value));
        if (!link_ready && link_defined) {
          entry.runtime->enumerate_techniques(nullptr, [&link_ready](api::effect_runtime*, api::effect_technique) {
            link_ready = true;  // not loading
          });
        }
        const api::effect_technique launchpad_technique = entry.runtime->find_technique(nullptr, LAUNCHPAD_TECHNIQUE);
        // ReShade lists no technique while it reloads Uplift.fx, which choosing Launchpad does: the last ready value holds through that (LaunchpadReadiness).
        entry.helper.launchpad_ready = entry.launchpad_readiness.Update(
            (technique.handle != 0u && launchpad_technique.handle != 0u),
            (entry.marker.handle != 0u && launchpad_technique.handle != 0u && entry.runtime->get_technique_state(launchpad_technique)), now);
        const std::optional<bool> link = entry.launchpad_link.Update({
            .ready = link_ready,
            .wanted = (launchpad_technique.handle != 0u && entry.runtime->get_technique_state(launchpad_technique)
                       && (settings.motion_vectors == ui::MotionVectorSource::AUTO
                           || settings.motion_vectors == ui::MotionVectorSource::LAUNCHPAD)),
            // 1.0.1: a set that changes nothing is skipped.
            .current = addon::LaunchPadDefinition((link_defined ? std::optional<std::string_view>(link_value) : std::nullopt),
                                                  (outer_defined ? std::optional<std::string_view>(outer_value) : std::nullopt)),
        });
        if (link) {
          nr::Log(nr::LogLevel::INFO, addon::LaunchPadLinkLine(*link));
          entry.runtime->set_preprocessor_definition_for_effect(UPLIFT_FX_EFFECT_NAME, UPLIFT_USE_LAUNCHPAD_DEFINE,
                                                                (*link ? "1" : "0"));
        }
      }
    }
    if (entry.helper.rejected) return;

    // NrClaim, as in 64-bit games: one device per process runs NR.
    const bool nr_allowed = state.claim.Update(device, settings.enabled, false, state.front->NrLoaded(device));
    std::string vk_hook_error;  // an unseen Vulkan device's reason, read under its leaf lock
    if (vulkan && !entry.helper.HasClient()) {
      const std::scoped_lock error_lock(state.vk_hook_error_mutex);
      vk_hook_error = state.vk_hook_error;
    }
    state.front->Present(entry.helper, {
                                           .device = device,
                                           .swapchain = swapchain,
                                           .runtime = entry.runtime,
                                           .queue = ((vulkan || opengl || d3d12) ? entry.queue : nullptr),
                                           .present_queue = ((vulkan || opengl) ? entry.present_queue : nullptr),
                                           .vk_hook_error = vk_hook_error,
                                           .back_buffer = entry.back_buffer,
                                           .marker = entry.marker,
                                           .settings = &settings,
                                           .overlay = &state.overlay,
                                           .nr_allowed = nr_allowed,
                                           .claimed_elsewhere = (settings.enabled && !nr_allowed && state.claim.Owner() != device),
                                           .now = now,
                                       },
                         &relay_after_unlock);
  }
  UPLIFT_CATCH("present", )
}

void OnRenderTechnique(api::effect_runtime* runtime, api::effect_technique technique, api::command_list* cmd_list,
                       api::resource_view rtv, api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry32* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    if (entry == nullptr) return;
    g_state->front->Technique(entry->helper, runtime, technique.handle == entry->marker.handle, rtv.handle, g_state->settings);
  }
  UPLIFT_CATCH("render_technique", )
}

void OnFinishEffects(api::effect_runtime* runtime, api::command_list* cmd_list, api::resource_view rtv,
                     api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry32* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    if (entry == nullptr) return;
    g_state->front->FinishEffects(entry->helper, runtime, rtv.handle, g_state->settings);
  }
  UPLIFT_CATCH("finish_effects", )
}

void OnReshadePresent(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    AddonState32& state = *g_state;
    if (state.overlay.key_captured_this_frame) {
      // The press that bound the hotkey is still in this frame's input: do not let it toggle Enable.
      state.overlay.key_captured_this_frame = false;
      return;
    }
    const uint32_t key = state.settings.enable_key;
    if (key == 0u || state.overlay.capturing_key || !runtime->is_key_pressed(key)) return;
    const auto found = state.devices.find(runtime->get_device());
    if (found == state.devices.end()) return;
    [[maybe_unused]] const auto& [device, entry] = *found;
    if (entry.runtime != runtime) return;  // toggle once per frame: the primary swap chain's runtime only
    state.settings.enabled = !state.settings.enabled;
    nr::Logf(nr::LogLevel::INFO, "Hotkey: NR {}", (state.settings.enabled ? "enabled" : "disabled"));
    SaveAndApply(&state);
  }
  UPLIFT_CATCH("reshade_present", )
}

void OnDrawOverlay(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    AddonState32& state = *g_state;
    const auto now = std::chrono::steady_clock::now();
    ui::OverlayView view = {.status_line = "OFF", .last_key_pressed = runtime->last_key_pressed(), .placement_line = "Placement: NR is off", .motion_line = "Motion vectors: NR is off"};
    view.dlss_unavailable = std::string(ui::BRIDGED_DLSS_REASON);  // no DLSS placement without a Direct3D 12 game
    const api::device_api overlay_api = runtime->get_device()->get_api();
    const auto found = state.devices.find(runtime->get_device());
    addon::HelperDevice* const helper_device = (found != state.devices.end() ? &found->second.helper : nullptr);
    if (overlay_api != api::device_api::d3d9) {
      view.d3d9ex_unavailable = "Only for 32-bit Direct3D 9 games";
    }
    const bool supported = (overlay_api == api::device_api::d3d9 || overlay_api == api::device_api::d3d10 || overlay_api == api::device_api::d3d11
                            || overlay_api == api::device_api::d3d12 || overlay_api == api::device_api::vulkan || overlay_api == api::device_api::opengl);
    if (supported) {
      state.front->Overlay(runtime->get_device(), helper_device, overlay_api, state.settings, &view, &state.overlay, now);  // finishes Setup too
    } else {
      view.card = ui::BuildStatusCard({.device_problem = "Uplift supports 32-bit Direct3D 9, 10, 11 and 12, Vulkan and OpenGL games only"});
      ui::SetupFacts facts;
      addon::FillPreference(&facts, state.settings, false);
      facts.dlss_unavailable = view.dlss_unavailable;
      ui::FinishSetup(&view, &state.overlay, runtime->get_device(), facts, now);
    }
    if (ui::DrawOverlay(view, &state.settings, &state.overlay)) {
      SaveAndApply(&state);
    }
    // Retry now, decided by the card's button: a stopped helper starts afresh; otherwise the next FRAME carries it.
    if (std::exchange(state.overlay.retry_now, false)) {
      state.front->RetryNow(helper_device);
    }
  }
  UPLIFT_CATCH("draw_overlay", )
}

// Plan 9 (design §2.9): ReShade's create_device event. It must not take the lock for any other API than Direct3D 9 (batch 2 review C-1):
// ReShade raises it inside D3D11CreateDevice too, which OnPresent calls for a Direct3D 10 game's relay while it holds the lock.
bool OnCreateDevice(api::device_api device_api, uint32_t& api_version) {
  if (device_api == api::device_api::vulkan) {
    // Plan 11 (Vulkan design §2.1): a D3D game that loads Vulkan later; ReShade 6.8 raises this in every vkCreateInstance right after loading its add-ons.
    // Lock-free (Plan 10 C-1): the hook's state has its own lock, never this add-on's.
    try {
      if (const std::optional<std::string> error = vk::DeviceHook::Install()) {
        {
          const std::scoped_lock error_lock(g_state->vk_hook_error_mutex);
          g_state->vk_hook_error = *error;
        }
        static std::atomic<bool> logged = false;
        if (!logged.exchange(true)) {
          nr::Logf(nr::LogLevel::WARN, "Vulkan: the vkCreateDevice hook could not be installed: {}", *error);
        }
      } else {
        PinReshade(reshade::internal::get_reshade_module_handle());  // I-4: the detour pinned Uplift, so ReShade stays loaded next to it
      }
    }
    UPLIFT_CATCH("create_device", false)
    return false;
  }
  if (device_api != api::device_api::d3d9) return false;
  try {
    const std::unique_lock lock(g_state->mutex);
    return g_state->front->CreateDevice(device_api, api_version, g_state->settings.use_d3d9ex);
  }
  UPLIFT_CATCH("create_device", false)
}

}  // namespace

// ReShade calls this after loading the add-on, outside the loader lock. A second call comes when ReShade reloads the add-on
// on a module that stayed loaded, after AddonUninit unregistered it: everything registers again.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
  // Fails when ReShade lacks this API version or ImGui table.
  if (!reshade::register_addon(addon_module, reshade_module)) return false;
  // Batch 3 review, minor 4: a second call means ReShade unloaded its add-ons (the last instance or device went) and this module stayed loaded, pinned
  // by the vkCreateDevice detour. AddonUninit ended the helper and unregistered, every device is gone, and no handler of the first state is
  // registered any more, so it goes now instead of leaking (its helper front, settings and config store).
  delete std::exchange(g_state, nullptr);
  g_state = new AddonState32();
  std::vector<std::string> warnings;
  g_state->settings = ui::LoadSettings(g_state->config, &warnings);
  g_state->poller.Seen(g_state->settings.enabled);
  addon::InstallLogBridge(g_state->settings.log_level);

  const auto module_path = [](HMODULE handle) {
    std::wstring path(32768u, L'\0');
    path.resize(GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path);
  };
  const std::filesystem::path game_exe_path = module_path(nullptr);
  g_state->front.emplace(ADDON_FILE, module_path(addon_module).parent_path(), game_exe_path.parent_path(), addon::UpliftStateDirectory(),
                         game_exe_path);

  // Design §2.9: the pending marker sits next to the latch marker, per game exe, with its own extension. If it survived the
  // last start, that start asked for 9Ex and never got a 9Ex device to present or to destroy: turn the toggle off, once, and say so.
  if (std::optional<std::string> warning = g_state->front->CheckExMarker(&g_state->settings, &g_state->config)) {
    warnings.push_back(std::move(*warning));
  }
  g_state->front->SettingsChanged(g_state->settings);

  // Plan 11 (Vulkan design §2): ReShade's Vulkan layer runs this inside the game's vkCreateInstance, before any device exists, so a detour on
  // vkCreateDevice installed now is in time for every one. The pending marker (a start whose adjusted device never presented) turns the adjustment off,
  // as the 9Ex marker does; CPU-ordered NR is the fallback then.
  const std::optional<addon::ModuleVersion> reshade_version = addon::ReadModuleVersion(reshade_module);
  const std::filesystem::path vk_marker = addon::LatchMarkerPath(addon::UpliftStateDirectory(), game_exe_path).replace_extension(L".vkdevice-pending");
  if (!vk::DeviceHook::CheckMarker(vk_marker)) {
    g_state->settings.adjust_vulkan_devices = false;
    ui::SaveSettings(g_state->settings, &g_state->config);
    g_state->front->SettingsChanged(g_state->settings);
    warnings.push_back("Vulkan device adjustment turned off (AdjustVulkanDevices = 0): the last start with it did not reach its first frame");
  }
  const bool reshade_vulkan_events = (reshade_version && *reshade_version >= addon::MIN_RESHADE_VULKAN_EVENTS);
  vk::DeviceHook::Configure(g_state->settings.adjust_vulkan_devices, reshade_vulkan_events, vk_marker);
  // Final review, minor 2: only for a ReShade older than 6.8. 6.8 raises create_device(vulkan) first in every vkCreateInstance, so OnCreateDevice's
  // install is always in time, and installing here too would only bring the detour, the pins and MinHook (new in this add-on) into plain 32-bit
  // Direct3D 9, 10 and 11 games that happen to have vulkan-1.dll loaded.
  if (!reshade_vulkan_events && GetModuleHandleW(L"vulkan-1.dll") != nullptr) {
    if (const std::optional<std::string> error = vk::DeviceHook::Install()) {
      {
        const std::scoped_lock error_lock(g_state->vk_hook_error_mutex);
        g_state->vk_hook_error = *error;
      }
      warnings.push_back(std::format("Vulkan: the vkCreateDevice hook could not be installed: {}", *error));
    } else {
      PinReshade(reshade_module);  // the detour pinned Uplift: ReShade stays loaded next to it (batch 2 review I-4)
    }
  }

  reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyCommandQueue);
  reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
  reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
  reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(OnSetCurrentPresetPath);
  reshade::register_event<reshade::addon_event::present>(OnPresent);
  reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
  reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
  reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
  reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
  addon::RegisterCreateDeviceEvent(&OnCreateDevice);
  addon::RegisterCreateSwapchainEvent(reshade_vulkan_events);  // Plan 11: copy access on a Vulkan swap chain's images (event 97, raw)
  reshade::register_overlay("GITC Uplift", OnDrawOverlay);

  nr::Logf(nr::LogLevel::INFO, "Uplift {} (32-bit) registered (ReShade API {}, Dear ImGui {}, ReShade {})", UPLIFT_VERSION,
           RESHADE_API_VERSION, IMGUI_VERSION_NUM,
           (reshade_version ? addon::FormatModuleVersion(*reshade_version) : std::string("of unknown version")));
  nr::Logf(nr::LogLevel::INFO, "NR runs in gitc-uplift-helper64.exe next to this add-on (build {})", UPLIFT_BUILD_ID);
  for (const std::string& warning : warnings) {
    nr::Log(nr::LogLevel::WARN, warning);
  }
  return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module) {
  if (g_state == nullptr) return;
  g_state->front->Quit();  // QUIT, a capped wait for the exit, then the job closes: the helper never outlives the game
  addon::RemoveLogBridge();
  reshade::unregister_addon(addon_module, reshade_module);
}

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID reserved) {
  // Outside ReShade the add-on refuses to load. Inside it, ReShade calls AddonInit next. The Ex pending marker is deleted only by a
  // 9Ex device's second present event (its first Present returned) or its destruction (design §2.9), never here: a game that
  // quits cleanly after a refused CreateDeviceEx is the case the marker exists for.
  if (reason == DLL_PROCESS_ATTACH && reshade::internal::get_reshade_module_handle() == nullptr) return FALSE;
  // The process is ending (not a FreeLibrary): a clean exit clears the Vulkan pending marker (batch 2 review I-3). No lock, no allocation.
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
    vk::DeviceHook::NoteProcessExit();
  }
  return TRUE;
}
