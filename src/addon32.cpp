// gitc-uplift.addon32: Uplift for 32-bit Direct3D 9, 10, 11, 12 (dgVoodoo2's D3D12 output), Vulkan (DXVK included) and OpenGL games. This add-on owns what only ReShade can give (its events,
// the settings and the panel, the frame's trigger, the capture) and lets gitc-uplift-helper64.exe -- started by itself, once NR is turned on -- run NR: no NVIDIA code ever loads in the
// game's process. It mirrors addon.cpp where it can, with the same names. The per-device logic lives in addon::HelperFront, which gitc-uplift.addon64 hosts for its Direct3D 9 devices too;
// this file is its thin host.
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
#include "addon/host_common.hpp"
#include "addon/launchpad_link.hpp"
#include "addon/live_facts.hpp"
#include "addon/motion_link_host.hpp"
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
constexpr char ADDON_FILE[] = "gitc-uplift.addon32";
// No instance API version is kept: its only reader is NGX's device additions, which the 32-bit half never makes (DeviceHook's add_ngx).
constexpr addon::host::InstanceApiVersionSink NOTE_INSTANCE_API_VERSION = nullptr;

struct DeviceEntry32 {
  addon::SwapchainSelector selector;
  // The frame in flight on the primary swap chain, set in the present event.
  api::command_queue* queue = nullptr;
  api::effect_runtime* runtime = nullptr;
  api::resource back_buffer = {0u};
  api::effect_technique marker = {0u};
  // The automatic UPLIFT_USE_LAUNCHPAD link and UPLIFT_USE_LUMENITE's, with Setup's "ready" for each, for
  // `link_runtime`.
  addon::MotionLinks motion_links;
  api::effect_runtime* link_runtime = nullptr;
  std::string link_preset_path;
  addon::HelperDevice helper;  // the clients and everything else about NR in the helper
  // On Vulkan and OpenGL `queue` is the effect runtime's, and this is the present event's.
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
  // Why the vkCreateDevice hook is not installed (a Vulkan device then runs CPU-ordered); empty when it is or was not tried. Written by AddonInit
  // and by create_device(vulkan), which takes no other lock of this add-on, so it has a lock of its own: a leaf, never held across a call.
  std::mutex vk_hook_error_mutex;
  std::string vk_hook_error;
};

// Created in AddonInit and never freed (as addon.cpp's).
AddonState32* g_state = nullptr;

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

// A Direct3D 9 Reset raises destroy_command_queue, not destroy_device (see HelperFront::DestroyQueue).
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

// The device is going away. Everything this add-on made on it is released and the helper detached (a Direct3D 9
// device already did both at destroy_command_queue).
void OnDestroyDevice(api::device* device) {
  try {
    // A Direct3D 10 device's client goes after the lock. It releases the relay ReShade returned (its proxy), whose
    // destroy_device comes back into this handler on this thread.
    std::unique_ptr<client::D3D10Client> d3d10;
    {
      const std::unique_lock lock(g_state->mutex);
      const auto found = g_state->devices.find(device);
      d3d10 = g_state->front->DestroyDevice((found != g_state->devices.end() ? &found->second.helper : nullptr), device);
      if (device->get_api() == api::device_api::vulkan) {
        // The pending marker's other end: every Vulkan device, also one that never presented. The hook's own lock, never this one's.
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
    const bool opengl = (device_api == api::device_api::opengl);  // the same queues as Vulkan, on a GL context
    const bool d3d12 = (device_api == api::device_api::d3d12);    // the present event's queue is the runtime's
    const auto now = std::chrono::steady_clock::now();
    // What D3D11CreateDevice returned for a new Direct3D 10 client's relay (ReShade's proxy in a game). Its last release
    // raises destroy_device, whose handler takes the add-on's lock. Declared before the lock, it is released after it.
    Microsoft::WRL::ComPtr<ID3D11Device> relay_after_unlock;
    const std::unique_lock lock(g_state->mutex);
    AddonState32& state = *g_state;

    addon::host::PollEnabled(state, now);
    const ui::Settings& settings = state.settings;

    DeviceEntry32& entry = state.devices[device];
    if (d3d9) {
      state.front->NotePresent(entry.helper, device, settings);
    }
    if (vulkan) {
      addon::host::CountVulkanPresent(entry, device);
    }
    if (!entry.selector.OnPresent(swapchain, now)) return;
    // This bookkeeping runs before the client exists, so a device waiting for its first enable still has `entry.runtime`,
    // which the Enable hotkey and the overlay need.
    addon::host::BindPresentRuntime(entry, state.runtimes, queue, swapchain);
    addon::host::BindPresentQueues(entry, queue, swapchain, vulkan || opengl);
    entry.helper.launchpad_ready = false;  // Setup's Launchpad option, set below while the effects are loaded
    entry.helper.lumenite_ready = false;   // and its Lumenite option
    if (const std::optional<addon::MotionLinkStep> step = addon::host::LinkMotion(entry, settings.motion_vectors, d3d9, now)) {
      addon::host::NoteMotionLinkStep(*step, &entry.helper);
    }
    const addon::RenderedMarker early_marker = addon::TakeEarlyMarker(entry.motion_links, entry.runtime, entry.marker);
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
                                           .early_marker = early_marker,
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
    if (addon::ObserveEarlyTechnique(entry->motion_links, entry->marker, runtime, technique, rtv)) return;
    g_state->front->Technique(entry->helper, runtime, technique.handle == entry->marker.handle, rtv.handle, g_state->settings);
  }
  UPLIFT_CATCH("render_technique", )
}

void OnFinishEffects(api::effect_runtime* runtime, api::command_list* cmd_list, api::resource_view /*rtv*/,
                     api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry32* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    if (entry == nullptr) return;
    g_state->front->FinishEffects(entry->helper, runtime, g_state->settings);
  }
  UPLIFT_CATCH("finish_effects", )
}

void OnReshadePresent(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    AddonState32& state = *g_state;
    const auto found = state.devices.find(runtime->get_device());
    if (found != state.devices.end() && found->second.runtime == runtime) {
      found->second.helper.trigger.EndFrame();
      found->second.motion_links.early.BeginObserving();
    }
    ui::ExpireKeyCapture(&state.overlay, std::chrono::steady_clock::now());
    if (state.overlay.key_captured_this_frame) {
      // The press that bound the hotkey is still in this frame's input: do not let it toggle Enable.
      state.overlay.key_captured_this_frame = false;
      return;
    }
    const uint32_t key = state.settings.enable_key;
    if (key == 0u || state.overlay.capturing_key || !runtime->is_key_pressed(key)) return;
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
    ui::OverlayView view = {.status_line = "OFF", .last_key_pressed = runtime->last_key_pressed()};
    view.dlss_unavailable = std::string(ui::BRIDGED_DLSS_REASON);  // no DLSS placement without a Direct3D 12 game
    const api::device_api overlay_api = runtime->get_device()->get_api();
    const auto found = state.devices.find(runtime->get_device());
    addon::HelperDevice* const helper_device = (found != state.devices.end() ? &found->second.helper : nullptr);
    if (overlay_api != api::device_api::d3d9) {
      view.d3d9ex_unavailable = "Only for 32-bit Direct3D 9 games";
    }
    // NR on a 32-bit Vulkan game always runs in Uplift's helper process (NGX's native Vulkan NR is 64-bit only).
    view.vulkan_nr_unavailable = (overlay_api == api::device_api::vulkan ? "A 32-bit game: NR always runs in Uplift's helper process" : "Only for Vulkan games");
    const bool supported = (overlay_api == api::device_api::d3d9 || overlay_api == api::device_api::d3d10 || overlay_api == api::device_api::d3d11
                            || overlay_api == api::device_api::d3d12 || overlay_api == api::device_api::vulkan || overlay_api == api::device_api::opengl);
    if (supported) {
      state.front->Overlay(runtime->get_device(), helper_device, overlay_api, state.settings, &view, &state.overlay, now);  // finishes Setup too
    } else {
      view.card = ui::BuildStatusCard({.device_problem = "Uplift supports 32-bit Direct3D 9, 10, 11 and 12, Vulkan and OpenGL games only"});
      view.keep_faces_unavailable = std::string(ui::KeepFacesUnavailable({.nr_runs = false}));
      ui::SetupFacts facts;
      addon::FillPreference(&facts, state.settings, false);
      facts.dlss_unavailable = view.dlss_unavailable;
      ui::FinishSetup(&view, &state.overlay, runtime->get_device(), facts, now);
    }
    view.before_fg_unavailable = "Before FG needs a game-facing 64-bit Streamline integration";
    view.AddStartupLocks(state.settings, overlay_api == api::device_api::vulkan, overlay_api == api::device_api::d3d9);
    if (ui::DrawOverlay(view, &state.settings, &state.overlay)) {
      SaveAndApply(&state);
    } else if (state.overlay.language_changed) {
      ui::SaveSettings(state.settings, &state.config);
    }
    // Retry now, decided by the card's button: a stopped helper starts afresh; otherwise the next FRAME carries it.
    if (std::exchange(state.overlay.retry_now, false)) {
      state.front->RetryNow(helper_device);
    }
  }
  UPLIFT_CATCH("draw_overlay", )
}

}  // namespace

// ReShade calls this after loading the add-on, outside the loader lock. A second call comes when ReShade reloads the add-on
// on a module that stayed loaded, after AddonUninit unregistered it: everything registers again.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
  // Fails when ReShade lacks this API version or ImGui table.
  if (!reshade::register_addon(addon_module, reshade_module)) return false;
  // A second call means ReShade unloaded its add-ons (the last instance or device went) and this module stayed loaded, pinned
  // by the vkCreateDevice detour. AddonUninit ended the helper and unregistered, every device is gone, and no handler of the first state is
  // registered any more, so it goes now instead of leaking (its helper front, settings and config store).
  delete std::exchange(g_state, nullptr);
  g_state = new AddonState32();
  std::vector<std::string> warnings;
  g_state->settings = ui::LoadSettings(g_state->config, &warnings);
  addon::InstallLogBridge(g_state->settings.log_level);
  addon::host::ApplyStartEnabled(*g_state);

  const std::filesystem::path game_exe_path = addon::host::ModulePath(nullptr);
  g_state->front.emplace(ADDON_FILE, addon::host::ModulePath(addon_module).parent_path(), game_exe_path.parent_path(), addon::UpliftStateDirectory(),
                         game_exe_path);

  // The pending marker sits next to the latch marker, per game exe, with its own extension. If it survived the
  // last start, that start asked for 9Ex and never got a 9Ex device to present or to destroy: turn the toggle off, once, and say so.
  if (std::optional<std::string> warning = g_state->front->CheckExMarker(&g_state->settings, &g_state->config)) {
    warnings.push_back(std::move(*warning));
  }
  g_state->front->SettingsChanged(g_state->settings);

  // ReShade's Vulkan layer runs this inside the game's vkCreateInstance, before any device exists, so a detour on
  // vkCreateDevice installed now is in time for every one. The pending marker (a start whose adjusted device never presented) turns the adjustment off,
  // as the 9Ex marker does; CPU-ordered NR is the fallback then.
  const std::optional<addon::ModuleVersion> reshade_version = addon::ReadModuleVersion(reshade_module);
  const std::filesystem::path vk_marker = addon::host::VkDevicePendingMarkerPath(addon::UpliftStateDirectory(), game_exe_path);
  if (vk::DeviceHook::CheckMarker(vk_marker) != vk::DeviceHook::MarkerVerdict::NONE) {  // the 32-bit half never adds NGX's: one tier
    const std::string_view warning = addon::host::TurnDeviceAdjustmentOff(*g_state);
    g_state->front->SettingsChanged(g_state->settings);  // the helper's copy of the settings, with the adjustment off
    warnings.emplace_back(warning);
  }
  const bool reshade_vulkan_events = (reshade_version && *reshade_version >= addon::MIN_RESHADE_VULKAN_EVENTS);
  vk::DeviceHook::Configure(g_state->settings.adjust_vulkan_devices, false, false, false, reshade_vulkan_events, vk_marker);  // NGX's additions are 64-bit only
  if (addon::host::InstallDeviceHookWithoutVulkanEvents(*g_state, reshade_vulkan_events, &warnings)) {
    addon::host::PinReshade(reshade_module);  // the detour pinned Uplift: ReShade stays loaded next to it
  }

  reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyCommandQueue);
  reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::register_event<reshade::addon_event::init_effect_runtime>(addon::host::OnInitEffectRuntime<g_state>);
  reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(addon::host::OnEffectsReloaded<g_state>);
  reshade::register_event<reshade::addon_event::destroy_effect_runtime>(addon::host::OnDestroyEffectRuntime<g_state>);
  reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(addon::host::OnSetCurrentPresetPath<g_state>);
  reshade::register_event<reshade::addon_event::present>(OnPresent);
  reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
  reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
  reshade::register_event<reshade::addon_event::reshade_open_overlay>(addon::host::OnOpenOverlay<g_state>);
  reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
  addon::RegisterCreateDeviceEvent(&addon::host::OnCreateDevice<g_state, NOTE_INSTANCE_API_VERSION>);
  addon::RegisterCreateSwapchainEvent(reshade_vulkan_events);  // copy access on a Vulkan swap chain's images (event 97, raw)
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
  // 9Ex device's second present event (its first Present returned) or its destruction, never here: a game that
  // quits cleanly after a refused CreateDeviceEx is the case the marker exists for.
  if (addon::host::RefusesLoad(reason)) return FALSE;
  // The process is ending (not a FreeLibrary): a clean exit clears the Vulkan pending marker. No lock, no allocation.
  if (addon::host::ProcessEnding(reason, reserved)) {
    vk::DeviceHook::NoteProcessExit();
  }
  return TRUE;
}
