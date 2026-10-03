#include <Windows.h>

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "addon/bridge_strikes.hpp"
#include "addon/command_list_tracker.hpp"
#include "addon/d3d11_identity.hpp"
#include "addon/device_context.hpp"
#include "addon/dlss_settings.hpp"
#include "addon/dlss_summary.hpp"
#include "addon/environment.hpp"
#include "addon/frame_config.hpp"
#include "addon/frame_trigger.hpp"
#include "addon/helper_front.hpp"
#include "addon/launchpad_link.hpp"
#include "addon/live_facts.hpp"
#include "addon/log_bridge.hpp"
#include "addon/ngx_bridge.hpp"
#include "addon/nr_claim.hpp"
#include "addon/nr_heartbeat.hpp"
#include "addon/placement.hpp"
#include "addon/removal_latch.hpp"
#include "addon/reshade_api.hpp"
#include "addon/reshade_config_store.hpp"
#include "addon/reshade_gl_host.hpp"
#include "addon/reshade_version.hpp"
#include "addon/reshade_vk_host.hpp"
#include "addon/swapchain_selector.hpp"
#include "addon/swapchain_usage.hpp"
#include "addon/uplift_catch.hpp"
#include "addon/vk_dlss_context.hpp"
#include "bridge/d3d10_bridge.hpp"
#include "bridge/d3d11_bridge.hpp"
#include "bridge/gl_bridge.hpp"
#include "bridge/record_nr.hpp"
#include "bridge/vk_bridge.hpp"
#include "color/encoding.hpp"
#include "gl/functions.hpp"
#include "ngx_hooks/hook_installer.hpp"
#include "nr/d3d11_handles.hpp"
#include "nr/log.hpp"
#include "nr/snippet.hpp"
#include "nr/vk_handles.hpp"
#include "state/compute_shadow.hpp"
#include "ui/controls_coalescer.hpp"
#include "ui/overlay.hpp"
#include "ui/settings.hpp"
#include "ui/settings_schema.hpp"
#include "ui/status_text.hpp"
#include "vk/device_hook.hpp"
#include "vk/format.hpp"
#include "vk/loader.hpp"

// The name ReShade shows in its Add-ons list. The log lines keep their "[Uplift]" prefix (addon/log_bridge.cpp).
extern "C" __declspec(dllexport) const char* NAME = "GITC Uplift";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "A virtual photography focused version of the DLSS 5 mod: NVIDIA DLSS Neural Rendering (DLSS-NR) with greater "
    "flexibility, ease of use, stability and hassle-free compatibility across APIs.";

namespace {

namespace addon = uplift::addon;
namespace bridge = uplift::bridge;
namespace color = uplift::color;
namespace gl = uplift::gl;
namespace ngx_hooks = uplift::ngx_hooks;
namespace nr = uplift::nr;
namespace state = uplift::state;
namespace ui = uplift::ui;
namespace vk = uplift::vk;
namespace api = reshade::api;

constexpr std::string_view UPLIFT_VERSION = UPLIFT_VERSION_TEXT;  // project(VERSION) in CMakeLists.txt
constexpr char MARKER_TECHNIQUE[] = "Uplift";
constexpr char MASK_TEXTURE[] = "UPLIFT_MASK";
constexpr char MOTION_TEXTURE[] = "UPLIFT_MV";
constexpr char LAUNCHPAD_TECHNIQUE[] = "MartysMods_Launchpad";
constexpr char UPLIFT_FX_EFFECT_NAME[] = "Uplift.fx";
// User-approved addition: Uplift sets this itself (OnPresent), so no preprocessor edit is needed.
constexpr char UPLIFT_USE_LAUNCHPAD_DEFINE[] = "UPLIFT_USE_LAUNCHPAD";

static_assert(static_cast<uint32_t>(api::color_space::unknown) == static_cast<uint32_t>(color::ColorSpace::UNKNOWN));
static_assert(static_cast<uint32_t>(api::color_space::srgb_nonlinear) == static_cast<uint32_t>(color::ColorSpace::SRGB_NONLINEAR));
static_assert(static_cast<uint32_t>(api::color_space::extended_srgb_linear) == static_cast<uint32_t>(color::ColorSpace::EXTENDED_SRGB_LINEAR));
static_assert(static_cast<uint32_t>(api::color_space::hdr10_st2084) == static_cast<uint32_t>(color::ColorSpace::HDR10_ST2084));
static_assert(static_cast<uint32_t>(api::color_space::hdr10_hlg) == static_cast<uint32_t>(color::ColorSpace::HDR10_HLG));
// The compute shadow compares ReShade's values as numbers (Task 6); these are the ones v6.0.0 names.
static_assert(static_cast<uint32_t>(api::shader_stage::compute) == state::SHADER_STAGE_COMPUTE);
static_assert(static_cast<uint32_t>(api::pipeline_stage::compute_shader) == state::PIPELINE_STAGE_COMPUTE);
static_assert(static_cast<uint32_t>(api::descriptor_type::constant_buffer) == state::DESCRIPTOR_CONSTANT_BUFFER);
static_assert(static_cast<uint32_t>(api::descriptor_type::acceleration_structure) == state::DESCRIPTOR_ACCELERATION_STRUCTURE_6_1);

// Plan 17: DiagnosticRemoveDevice presses Retry now itself about a second (at 60 Hz) after a stop that may still be retried.
constexpr uint32_t DIAGNOSTIC_RETRY_PRESENTS = 60u;

struct DeviceEntry {
  addon::SwapchainSelector selector;
  // Plan 7: a D3D11 device's bridge to its private D3D12 device. Made at the first present with NR on, and kept until
  // the device goes (D3D11 design, decision 2). OnDestroyDevice destroys it outside the add-on's lock.
  std::unique_ptr<bridge::D3D11Bridge> d3d11_bridge;
  // Plan 8: a D3D10 device's bridge (a relay D3D11 device into the D3D11 bridge). As d3d11_bridge: made at the first
  // present with NR on, kept until the device goes, destroyed outside the add-on's lock.
  std::unique_ptr<bridge::D3D10Bridge> d3d10_bridge;
  std::unique_ptr<addon::DeviceContext> context;
  std::string message;                                   // why this device has no context
  bool rejected = false;                                 // not NVIDIA, or the context failed to start: decided once
  std::filesystem::path snippet_path;                    // resolved once at context creation; empty when not found
  std::string static_block;                              // a conflicting host or a missing runtime: decided at creation
  ui::CardStage block_stage = ui::CardStage::CONFLICTS;  // which stage static_block fails
  nr::Size swapchain_size;                               // the primary swap chain's back buffer, for the main-handle choice
  // The frame in flight on the primary swap chain, set in the present event.
  api::command_queue* queue = nullptr;
  api::effect_runtime* runtime = nullptr;
  api::resource back_buffer = {0u};
  api::effect_technique marker = {0u};
  // Plan 14: Launchpad's technique and the Uplift technique are both enabled (Setup's Launchpad option); set at each present.
  bool launchpad_ready = false;
  std::string mask_note;  // Plan 5: the overlay's "NR mask" line
  bool d3d11 = false;     // Plan 7: a D3D11 device; its NR runs through d3d11_bridge
  // Final review I-1: the automatic UPLIFT_USE_LAUNCHPAD link, for `link_runtime`. Reset when that runtime is
  // destroyed (a reinit too) or changes, or when its preset changes.
  addon::LaunchPadLink launchpad_link;
  addon::LaunchpadReadiness launchpad_readiness;  // Plan 14: Setup's "ready", held through the effect reload the link itself causes
  api::effect_runtime* link_runtime = nullptr;
  std::string link_preset_path;
  bool d3d10 = false;  // Plan 8: a D3D10 device; its NR runs through d3d10_bridge
  // Plan 10 (design §2.2): a Direct3D 9 device's clients and helper-side state. Its NR runs in gitc-uplift-helper64.exe through the front
  // both add-ons share; no context and no bridge exist for it.
  addon::HelperDevice helper;
  // Plan 11 (Vulkan design §4): a Vulkan device's bridge to its private D3D12 device. Made at the first present with NR on, kept until the device
  // goes; OnDestroyDevice frees its Vulkan objects (through ReShade's API only) and destroys it outside the add-on's lock.
  std::unique_ptr<bridge::VkBridge> vk_bridge;
  bool vulkan = false;                          // a Vulkan device; its NR runs through vk_bridge
  api::command_queue* present_queue = nullptr;  // the present event's queue; on Vulkan `queue` is the effect runtime's (design §3.1, key decision g)
  bool vk_presents_counted = false;             // DeviceHook::NotePresent has finished counting (the pending marker's second present)
  bool vk_second_queue_logged = false;
  // Plan 12 (OpenGL design §4): an OpenGL context's bridge to its private D3D12 device. Made at the first present with NR on while the present is on the
  // effect runtime's context, kept until the device goes; OnDestroyDevice forgets its GL names with no GL call and destroys it outside the add-on's lock.
  // On OpenGL `queue` is the effect runtime's queue (its immediate list is the context itself) and `present_queue` the present event's, as on Vulkan.
  std::unique_ptr<bridge::GlBridge> gl_bridge;
  bool opengl = false;  // an OpenGL device; its NR runs through gl_bridge
  // Plan 13 (design §5): NR after DLSS natively on a 64-bit Vulkan device, next to vk_bridge's Present. The user's decision 1: made at a present once
  // Source is DLSS (the overlay's After DLSS or Before upscaling on Vulkan; Auto stays at Present), for a device whose record says ngx_ready with an
  // instance; kept until the device goes (the vkDestroyDevice detour tears it down; destroy_device abandons what is left, R83).
  std::unique_ptr<addon::VkDlssContext> vk_dlss;
  std::string vk_native_problem;   // why native NR cannot run on this device ("NR after DLSS on Vulkan needs ..."); empty when it can
  bool vk_native_checked = false;  // vk_native_problem was decided from the device's facts (they never change); a failed start sets it too
  bool vk_nr_counted = false;      // NgxBridge::NoteVkContextNr: the NGX hooks watch this device's Vulkan DLSS evaluates for its native context
  // Plan 15 fix round (minor 3): a Direct3D 12 device's hold after the game's NGX shutdown, kept across its contexts. While a context exists, its own copy is
  // the one that moves; destroy_command_queue keeps it here when it ends the context, a shutdown with no context records it here, and the next context
  // starts from it, so a new one never loads NR into a core the game shut down (or into an abandoned runtime).
  addon::CoreShutdownHold core_hold;
  // Plan 17 (1.0.1 design §3): Retry now after this device's private-device NR stopped (its bridge's device removed or hung, or the CPU-ordered timeouts),
  // with the 2-strike rule. `retry_pending`: the overlay asked; the next present tears the stopped bridge and its context down, and the one after builds
  // new ones. `retiring_vk`: Vulkan bridges a retry tore down, kept until the game's queue has passed the fences behind their imports (Plan 14's rule).
  addon::BridgeStrikes strikes;
  bool retry_pending = false;
  uint32_t diagnostic_retry_in = 0u;  // DiagnosticRemoveDevice: presents until it presses Retry now itself (0: not counting)
  uint32_t diagnostic_frames = 0u;    // DiagnosticRemoveDevice: presents with NR applied on the current private device
  std::vector<std::unique_ptr<bridge::VkBridge>> retiring_vk;
  // Plan 18 (design §2-§5): a Direct3D 11 device's DLSS stages. `d3d11_watch_counted`: NgxBridge::NoteD3D11Watch's flag (the hooks watch its evaluates while
  // its bridge's context exists and NR is on or not yet released). `foreign_dlss`: its game's DLSS ran on a Direct3D 12 device Uplift cannot reach (logged
  // once). `present_motion_logged`: "copied for Present" was logged since the ring was last released.
  bool d3d11_watch_counted = false;
  bool foreign_dlss = false;
  bool present_motion_logged = false;
  // 1.0.1 (F2): the once-a-minute "NR running" line and the "NR stopped after" one, with this process's private bytes and VRAM on `vram`'s adapter.
  addon::NrHeartbeat heartbeat;
  addon::ProcessVram vram;
};

// Plan 8: NR on this device runs on a private D3D12 device through a bridge (D3D11, or D3D10 through its relay; Plan 11: Vulkan; Plan 12: OpenGL).
bool Bridged(const DeviceEntry& entry) {
  return entry.d3d11 || entry.d3d10 || entry.vulkan || entry.opengl;
}

// Plan 17: the bridged device's private Direct3D 12 device (native), or null before its bridge exists.
ID3D12Device* PrivateDevice(const DeviceEntry& entry) {
  if (entry.d3d11_bridge) return entry.d3d11_bridge->Device();
  if (entry.d3d10_bridge) return entry.d3d10_bridge->Device();
  if (entry.vk_bridge) return entry.vk_bridge->Device();
  if (entry.gl_bridge) return entry.gl_bridge->Device();
  return nullptr;
}

// Test phase (review M-3): whether the bridge's private device can be replaced by Retry now (D3D12Side::Independent); true before a bridge exists.
bool PrivateDeviceIndependent(const DeviceEntry& entry) {
  if (entry.d3d11_bridge) return entry.d3d11_bridge->Independent();
  if (entry.d3d10_bridge) return entry.d3d10_bridge->Independent();
  if (entry.vk_bridge) return entry.vk_bridge->Independent();
  if (entry.gl_bridge) return entry.gl_bridge->Independent();
  return true;
}

// Plan 17: the bridge's latch text ("The OpenGL bridge stopped: ..."); empty while it runs.
std::string_view BridgeLatch(const DeviceEntry& entry) {
  if (entry.d3d11_bridge) return entry.d3d11_bridge->Latch();
  if (entry.d3d10_bridge) return entry.d3d10_bridge->Latch();
  if (entry.vk_bridge) return entry.vk_bridge->Latch();
  if (entry.gl_bridge) return entry.gl_bridge->Latch();
  return {};
}

// Plan 17: stops whichever bridge the device has (a no-op past the first reason).
void StopBridge(DeviceEntry& entry, std::string reason) {
  if (entry.d3d11_bridge) {
    entry.d3d11_bridge->Stop(std::move(reason));
  } else if (entry.d3d10_bridge) {
    entry.d3d10_bridge->Stop(std::move(reason));
  } else if (entry.vk_bridge) {
    entry.vk_bridge->Stop(std::move(reason));
  } else if (entry.gl_bridge) {
    entry.gl_bridge->Stop(std::move(reason));
  }
}

// Plan 17: the device's private-device NR has stopped: its bridge latched (a removed or hung private device, the CPU-ordered timeouts, a failed cross-API
// step), or its context found the private device removed.
bool PrivateDeviceStopped(const DeviceEntry& entry) {
  const bool latched = ((entry.d3d11_bridge && entry.d3d11_bridge->Stopped()) || (entry.d3d10_bridge && entry.d3d10_bridge->Stopped())
                        || (entry.vk_bridge && entry.vk_bridge->Stopped()) || (entry.gl_bridge && entry.gl_bridge->Stopped()));
  return latched || (entry.context && entry.context->DeviceLost());
}

// 1.0.1 (F2): one heartbeat line ("NR running: ..." or "NR stopped after ..."), with this process's private bytes and its VRAM on the adapter NR runs on:
// the private device's on a bridged device, the game's own on Direct3D 12.
void LogHeartbeat(DeviceEntry& entry, api::device* device, std::string_view lead, const addon::NrHeartbeat::Figures& figures) {
  std::optional<LUID> luid;
  if (ID3D12Device* const private_device = PrivateDevice(entry)) {
    luid = private_device->GetAdapterLuid();
  } else if (device->get_api() == api::device_api::d3d12) {
    luid = reinterpret_cast<ID3D12Device*>(device->get_native())->GetAdapterLuid();
  }
  nr::Log(nr::LogLevel::INFO, addon::HeartbeatLine(lead, figures, addon::ProcessPrivateMiB(), (luid ? entry.vram.MiB(*luid) : std::nullopt)));
}

// The bridge's view of the add-on (v2 design §3.1). Defined after AddonState.
class AddonRouting final : public addon::NgxRouting {
 public:
  ngx_hooks::SrOverrides Overrides() override;
  const void* DeviceOf(ID3D12GraphicsCommandList* list) override;
  nr::Size SwapchainSize(const void* device) override;
  bool IsOwnCode(const void* address) override;
  void OnMainEvaluate(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) override;
  ID3D12Resource* BeforeMainEvaluate(const void* device, ID3D12GraphicsCommandList* list,
                                     const ngx_hooks::DlssFrame& frame) override;
  void ColorSwapRejected(const void* device, ID3D12GraphicsCommandList* list) override;
  // Plan 13 (design §5): the Vulkan routing. `list` is a VkCommandBuffer (nr/vk_handles.hpp).
  const void* DeviceOfVk(ID3D12GraphicsCommandList* list, VkDevice vk_device) override;
  void OnMainEvaluateVk(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame,
                        const ngx_hooks::VkDlssResources& copies) override;
  ID3D12Resource* BeforeMainEvaluateVk(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame,
                                       const ngx_hooks::VkDlssResources& copies) override;
  void EndVulkanEvaluate(ID3D12GraphicsCommandList* list) override;
  void ColorSwapRejectedVk(const void* device, ID3D12GraphicsCommandList* list) override;
  void BeforeCoreShutdownVk(VkDevice vk_device) override;
  void BeforeCoreShutdownD3D12(ID3D12Device* device) override;  // Plan 15
  // Plan 18 (design §2-§5): the Direct3D 11 routing. `list` is an ID3D11DeviceContext (nr/d3d11_handles.hpp) and never reaches a Direct3D 12 call.
  const void* DeviceOfD3D11(ID3D12GraphicsCommandList* list) override;
  const void* DeviceOfD3D11Device(ID3D11Device* device) override;
  void OnMainEvaluateD3D11(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) override;
  ID3D12Resource* BeforeMainEvaluateD3D11(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) override;
  void ColorSwapRejectedD3D11(const void* device, ID3D12GraphicsCommandList* list) override;
  void LogFirstEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) override;
};

struct AddonState {
  std::shared_mutex mutex;
  addon::ReshadeConfigStore config;
  ui::Settings settings;
  ui::EnabledPoller poller;
  ui::ControlsCoalescer coalescer;
  ui::OverlayState overlay;
  std::unordered_map<api::device*, DeviceEntry> devices;
  std::vector<api::effect_runtime*> runtimes;
  addon::NrClaim claim;  // Plan 2 final review M8: at most one device loads NR
  // Plan 10 (batch 2 review, minor 3): a Direct3D 9 device's NR runs in the helper, not in this process, so it does not count against the one
  // in-process NR above. The single helper still serves one Direct3D 9 device at a time: this claim is theirs alone.
  addon::NrClaim d3d9_claim;
  AddonRouting routing;
  addon::NgxBridge bridge{routing};
  ngx_hooks::HookInstaller hooks;
  std::string dlss_unavailable_reason;  // decided at AddonInit; empty when a DLSS placement can run
  // Plan 11: why the vkCreateDevice hook is not installed (a Vulkan device then runs CPU-ordered); empty when it is or was not tried. Written by
  // AddonInit and by create_device(vulkan), which takes no other lock of this add-on (Plan 10 C-1), so it has a lock of its own: a leaf, never
  // held while taking another (batch 2 review, minor 5).
  std::mutex vk_hook_error_mutex;
  std::string vk_hook_error;
  // Minor (fix round 2): NoteLatchIfTripped may run on an evaluate or a teardown thread, where mirroring
  // the trip into ReShade.ini would race the present thread's own flush_cache()/save(). Set there,
  // cleared and acted on only by the next OnPresent, on the present thread.
  bool latch_save_pending = false;
  bool tracking_registered = false;
  HMODULE self_module = nullptr;
  std::filesystem::path addon_directory;
  std::filesystem::path game_exe_path;  // the game's own executable, for the latch marker's name
  std::filesystem::path game_directory;
  std::filesystem::path ngx_data_directory;
  std::filesystem::path latch_marker_path;   // fix round 1, Important 1: computed once, at AddonInit
  bool snippet_hashed = false;               // the ~166 MB runtime is hashed at most once per process
  bool warned_no_immediate_list = false;     // ReShade gave no immediate list for a presenting queue
  bool warned_invalid_snippet_path = false;  // SnippetPath was not valid UTF-8 (I1): logged once
  uint64_t settings_generation = 0u;         // Plan 6: every SaveAndApply; restarts the retry backoff
  // Plan 10: made in AddonInit. Hosts every Direct3D 9 device (and the helper they share); Direct3D 10, 11 and 12 never touch it.
  std::optional<addon::HelperFront> front;
  // Plan 13: ReShade raises the Vulkan create events Uplift's native NR relies on (6.8 or newer); decided at AddonInit.
  bool reshade_vulkan_events = false;
  // Plan 13 (batch 1 review, minor 3): the tokens of a stale VkListState that init_command_list replaced. init_command_list may run inside Uplift's
  // own work (a command buffer the NR runtime allocates under the add-on's lock), so it never takes that lock: the tokens wait here, under a leaf
  // lock of their own, and the device's next present recycles them.
  std::mutex vk_orphans_mutex;
  std::vector<addon::StaleVkList> vk_orphans;
};

// Created in AddonInit and never freed: with the NGX hooks running the module is pinned, and at
// process exit D3D12 objects must not be touched.
AddonState* g_state = nullptr;
// Amendment 1: the device whose tokens this thread submitted and has not stamped yet.
thread_local api::device* t_unstamped_device = nullptr;
// Plan 13: compute pipelines NVIDIA's DLSS bound on this thread inside its Vulkan evaluates since the last main-handle evaluate (OnVulkanBindPipeline).
thread_local uint32_t t_dlss_compute_binds = 0u;
// Batch 3 review, minor 7: OnVulkanBindPipeline is registered (AddonInit), so the count above means something.
std::atomic<bool> g_dlss_binds_watched{false};
// Plan 13 final review, minor 3: this thread holds the add-on's lock while it runs NR's runtime on a Vulkan device (a Session that may unload, a teardown, a
// core shutdown). The core's NVSDK_NGX_VULKAN_Shutdown1 reached from there (the snippet's own, or a helper module's that IsOwnCode does not know) must not
// take the lock again: std::shared_mutex is not recursive.
thread_local bool t_holds_lock_for_nr = false;
// The process is ending (DllMain's DLL_PROCESS_DETACH with a non-null reserved): ExitProcess may have ended a thread that held the lock, so a core
// Shutdown1 reached from a module's process detach never waits for it.
std::atomic<bool> g_process_exiting{false};
class HoldsLockForNr {
 public:
  HoldsLockForNr() : previous_(std::exchange(t_holds_lock_for_nr, true)) {}
  ~HoldsLockForNr() { t_holds_lock_for_nr = previous_; }
  HoldsLockForNr(const HoldsLockForNr&) = delete;
  HoldsLockForNr& operator=(const HoldsLockForNr&) = delete;

 private:
  bool previous_;
};

// Batch 2 review I-4: ReShade's module is pinned (once) whenever Uplift pins itself, the NGX hooks or the vkCreateDevice detour, as ReShade's own
// [INSTALL] PreventUnloading does (dll_main.cpp:268-271). The Vulkan loader closes ReShade's layer library with the last instance (a probe instance
// often comes first), and ReShade unloads it then. Uplift would stay, pinned, while the next ReShade starts empty: a second AddonInit that returns
// without registering gets no events at all, and this module's cached ReShade function pointers (the log bridge among them, which the detour logs
// through) would point into the old image. Pinned, ReShade keeps Uplift registered across unload_addons and load_addons (unload_addons marks it
// external and keeps it, addon_manager.cpp:357-368; the next load_addons calls its AddonInit again, :190-209), which is what the second AddonInit's
// early return assumes. The pin is best effort: GetModuleHandleExW on the address of a loaded module does not fail in practice. If it did, then
// while ReShade stays loaded a second register_addon is refused (two ERROR lines in ReShade.log) and the first registration keeps working.
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

// Settings that act outside the per-frame FrameConfig.
void ApplyProcessSettings(const ui::Settings& settings) {
  addon::InstallLogBridge(settings.log_level);
  // Spec §12: the NGX runtime reads it when the snippet next loads.
  SetEnvironmentVariableW(L"__NGX_SHOW_INDICATOR", (settings.show_nv_indicator ? L"1024" : nullptr));
}

// After the overlay or the hotkey changed a setting.
void SaveAndApply(AddonState* state) {
  state->settings = ui::Sanitized(state->settings);
  ui::SaveSettings(state->settings, &state->config);
  state->poller.Seen(state->settings.enabled);
  ApplyProcessSettings(state->settings);
  ++state->settings_generation;  // Plan 6 (D11): a settings change restarts the retry backoff
  state->front->SettingsChanged(state->settings);  // Plan 10: the helper of a Direct3D 9 game reads them too
}

// With the add-on's lock held.
addon::DeviceContext* ContextOf(api::device* device) {
  const auto found = g_state->devices.find(device);
  return (found == g_state->devices.end() ? nullptr : found->second.context.get());
}

// Spec §10 install timing: NGX cores loaded since the last entry point are hooked here, outside the
// loader lock. One atomic exchange when nothing loaded.
void PollHooks() {
  g_state->hooks.Poll();
}

// Amendment 1: this thread's earlier ExecuteCommandLists calls have returned by its next Uplift event,
// so one Signal per queue now follows them.
void StampThisThread() {
  api::device* const device = std::exchange(t_unstamped_device, nullptr);
  if (device == nullptr) return;
  const std::unique_lock lock(g_state->mutex);
  if (addon::DeviceContext* const context = ContextOf(device)) {
    context->StampThread(GetCurrentThreadId());
  }
}

void DropTokens(api::device* device, std::span<const uint64_t> tokens) {
  const std::unique_lock lock(g_state->mutex);
  if (addon::DeviceContext* const context = ContextOf(device)) {
    context->DropTokens(tokens);
  }
}

// Fix round 1, Important 1/2 (guard and cost fixed in fix round 2): the first time `context` reports a
// fresh trip -- from a present, an evaluate, or a teardown, wherever the removal is first seen --
// persists it synchronously (a marker file of Uplift's own, flushed to disk before this returns:
// ReShade's ini cache alone would not survive a crash right after the removal) and locks the DLSS
// placements off for the rest of this process, on every device, including one the game creates
// afterward. Call sites already hold the add-on's lock. The cheap accessor keeps the common (untripped)
// case from building a full Status() on every present, evaluate and teardown. No-op once
// `dlss_unavailable_reason` already reads the latch reason: a later, stale trip from an evaluate that
// raced the present which first decided it must not re-log or re-write the marker -- and Clear latch,
// which only ever resets `dlss_placement_blocked` (LATCH_UNAVAILABLE_REASON's own comment: this session
// stays off on purpose), must not reopen this by flipping that flag back.
// Plan 13: `Context` is a DeviceContext or a VkDlssContext (a Vulkan device lost soon after NR ran in the game's command buffer latches the same way).
template <class Context>
void NoteLatchIfTripped(AddonState* state, Context* context) {
  if (context == nullptr || !context->LatchTripped() || state->dlss_unavailable_reason == addon::LATCH_UNAVAILABLE_REASON) {
    return;
  }
  const addon::ContextStatus status = context->Status();
  state->settings.dlss_placement_blocked = true;
  state->dlss_unavailable_reason = std::string(addon::LATCH_UNAVAILABLE_REASON);
  nr::Log(nr::LogLevel::ERR, "DLSS placements latched off after a device removal (DlssPlacementBlocked = 1)");
  addon::WriteLatchMarker(state->latch_marker_path, status.message);
  for (auto& [device, entry] : state->devices) {
    if (entry.context) {
      entry.context->SetDlssUnavailableReason(state->dlss_unavailable_reason);
    }
    if (entry.vk_dlss) {
      entry.vk_dlss->SetDlssUnavailableReason(state->dlss_unavailable_reason);
    }
  }
  // Minor (fix round 2): the ini mirror runs only on the present thread now (see latch_save_pending).
  state->latch_save_pending = true;
}

ngx_hooks::SrOverrides AddonRouting::Overrides() {
  const std::unique_lock lock(g_state->mutex);
  return addon::ToSrOverrides(g_state->settings);
}

const void* AddonRouting::DeviceOf(ID3D12GraphicsCommandList* list) {
  const addon::ListState* const list_state = addon::FindListState(list);
  return (list_state == nullptr ? nullptr : list_state->device);
}

nr::Size AddonRouting::SwapchainSize(const void* device) {
  const std::unique_lock lock(g_state->mutex);
  const auto found = g_state->devices.find(static_cast<api::device*>(const_cast<void*>(device)));
  return (found == g_state->devices.end() ? nr::Size{} : found->second.swapchain_size);
}

bool AddonRouting::IsOwnCode(const void* address) {
  HMODULE module = nullptr;
  if (address == nullptr
      || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module)
             == FALSE) {
    return false;
  }
  return module == g_state->self_module || nr::IsUpliftRuntimeModule(module);
}

void AddonRouting::OnMainEvaluate(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) {
  StampThisThread();
  addon::ListState* const list_state = addon::FindListState(list);
  if (list_state == nullptr) return;
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;  // Plan 15: a starving present's catch-up may unload NR here, and the snippet's Shutdown1 may reach the core's
  addon::DeviceContext* const context = ContextOf(static_cast<api::device*>(const_cast<void*>(device)));
  if (context == nullptr) {
    g_state->bridge.NoteForeignD3D12Dlss();  // Plan 18: a device without a context (a mod's own, in a Direct3D 11 game)
    return;
  }
  addon::ReshadeDlssFrameHost host(list_state);
  context->OnDlssEvaluate(frame, host, std::chrono::steady_clock::now());
  // Important 1 (fix round 1): a removal first seen by an evaluate (presents may be starving) still
  // decides the latch here, under the lock this function already holds.
  NoteLatchIfTripped(g_state, context);
}

ID3D12Resource* AddonRouting::BeforeMainEvaluate(const void* device, ID3D12GraphicsCommandList* list,
                                                 const ngx_hooks::DlssFrame& frame) {
  StampThisThread();
  addon::ListState* const list_state = addon::FindListState(list);
  if (list_state == nullptr) return nullptr;
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;  // Plan 15: as OnMainEvaluate's
  addon::DeviceContext* const context = ContextOf(static_cast<api::device*>(const_cast<void*>(device)));
  if (context == nullptr) return nullptr;
  addon::ReshadeDlssFrameHost host(list_state);
  ID3D12Resource* const color = context->BeforeDlssEvaluate(frame, host, std::chrono::steady_clock::now());
  NoteLatchIfTripped(g_state, context);  // a removal first seen here still decides the latch
  return color;
}

void AddonRouting::ColorSwapRejected(const void* device, ID3D12GraphicsCommandList* /*list*/) {
  StampThisThread();
  const std::unique_lock lock(g_state->mutex);
  addon::DeviceContext* const context = ContextOf(static_cast<api::device*>(const_cast<void*>(device)));
  if (context == nullptr) return;
  context->OnColorSwapRejected();
}

// Plan 13 (design §5). A tracked command buffer names its device without the add-on's lock; only CreateFeature1's VkDevice for an untracked
// buffer looks the device up under it (a game's create, never Uplift's own: NgxBridge returns before this for those).
const void* AddonRouting::DeviceOfVk(ID3D12GraphicsCommandList* list, VkDevice vk_device) {
  if (const addon::VkListState* const list_state = addon::FindVkListState(nr::VkListOf(list))) return list_state->device;
  if (vk_device == VK_NULL_HANDLE) return nullptr;
  const std::unique_lock lock(g_state->mutex);
  for (const auto& [device, entry] : g_state->devices) {
    if (device->get_api() == api::device_api::vulkan && device->get_native() == addon::ReshadeHandleOf(vk_device)) return device;
  }
  return nullptr;
}

void AddonRouting::OnMainEvaluateVk(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame,
                                    const ngx_hooks::VkDlssResources& copies) {
  const VkCommandBuffer buffer = nr::VkListOf(list);
  addon::VkListState* const list_state = addon::FindVkListState(buffer);
  if (list_state == nullptr) return;
  const std::unique_lock lock(g_state->mutex);
  const auto found = g_state->devices.find(static_cast<api::device*>(const_cast<void*>(device)));
  if (found == g_state->devices.end() || !found->second.vk_dlss) return;
  addon::VkDlssContext* const context = found->second.vk_dlss.get();
  // Key decision c: the token's event closes this call on every path, an exception in the recording included, while the lock is still held.
  struct CloseEvaluate {
    addon::VkDlssContext* context;
    VkCommandBuffer buffer;
    addon::VkListState* list_state;
    ~CloseEvaluate() {
      try {
        context->EndEvaluate(buffer, *list_state);
      } catch (...) {
        // No-op: the hook's catch reports the recording's failure; a token left open here is dropped as stale after 2 s and 8 frames.
      }
    }
  } close{context, buffer, list_state};
  context->OnDlssEvaluate(buffer, frame, copies, *list_state, std::chrono::steady_clock::now());
  NoteLatchIfTripped(g_state, context);  // a loss first seen by an evaluate still decides the latch here
  static std::atomic<bool> binds_logged = false;
  if (const uint32_t binds = std::exchange(t_dlss_compute_binds, 0u); g_dlss_binds_watched.load(std::memory_order_relaxed) && !binds_logged.exchange(true)) {
    nr::Logf(nr::LogLevel::INFO, "Vulkan: the game's DLSS evaluate bound {} compute pipeline(s) of its own (the game binds its state again after DLSS; Uplift replays none on Vulkan)", binds);
  }
}

ID3D12Resource* AddonRouting::BeforeMainEvaluateVk(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame,
                                                   const ngx_hooks::VkDlssResources& copies) {
  const VkCommandBuffer buffer = nr::VkListOf(list);
  addon::VkListState* const list_state = addon::FindVkListState(buffer);
  if (list_state == nullptr) return nullptr;
  const std::unique_lock lock(g_state->mutex);
  const auto found = g_state->devices.find(static_cast<api::device*>(const_cast<void*>(device)));
  if (found == g_state->devices.end() || !found->second.vk_dlss) return nullptr;
  const NVSDK_NGX_Resource_VK* const color =
      found->second.vk_dlss->BeforeDlssEvaluate(buffer, frame, copies, *list_state, std::chrono::steady_clock::now());
  NoteLatchIfTripped(g_state, found->second.vk_dlss.get());
  return nr::AsResource(color);
}

void AddonRouting::ColorSwapRejectedVk(const void* device, ID3D12GraphicsCommandList* /*list*/) {
  const std::unique_lock lock(g_state->mutex);
  const auto found = g_state->devices.find(static_cast<api::device*>(const_cast<void*>(device)));
  if (found == g_state->devices.end() || !found->second.vk_dlss) return;
  found->second.vk_dlss->OnColorSwapRejected();
}

void AddonRouting::EndVulkanEvaluate(ID3D12GraphicsCommandList* list) {
  // Lock-free unless this call opened a token that OnMainEvaluateVk did not close (Before upscaling's recording, then a failed DLSS evaluate).
  const VkCommandBuffer buffer = nr::VkListOf(list);
  addon::VkListState* const list_state = addon::FindVkListState(buffer);
  if (list_state == nullptr || list_state->open_token == 0u) return;
  const std::unique_lock lock(g_state->mutex);
  const auto found = g_state->devices.find(static_cast<api::device*>(const_cast<void*>(list_state->device)));
  if (found == g_state->devices.end() || !found->second.vk_dlss) {
    list_state->open_token = 0u;  // its context is gone, and its timeline with it
    return;
  }
  found->second.vk_dlss->EndEvaluate(buffer, *list_state);
}

// Batch 3 review I-1: the game's NVSDK_NGX_VULKAN_Shutdown1 for `vk_device` (Streamline's slShutdown, before the game destroys the device), on the game's
// thread before the core's own runs. Native NR on that device makes its last NGX calls here (after a wait of at most 2 s for its recordings, events only),
// so none comes after the core's; the lock is released when this returns, before the original runs. The vkDestroyDevice detour then frees only Uplift's
// own Vulkan objects.
void AddonRouting::BeforeCoreShutdownVk(VkDevice vk_device) {
  // Plan 13 final review, minor 3: never when Uplift's own locked code reached the core's Shutdown1 on this thread (it would wait for itself), nor once the
  // process is ending (a thread ExitProcess ended may have held the lock). The NR work such a path does is its own (a teardown, a Session's unload).
  if (t_holds_lock_for_nr || g_process_exiting.load(std::memory_order_acquire)) return;
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;
  for (auto& [device, entry] : g_state->devices) {
    if (!entry.vk_dlss || device->get_native() != addon::ReshadeHandleOf(vk_device)) continue;
    entry.vk_dlss->OnCoreShutdown(addon::VK_CORE_SHUTDOWN_WAIT);
    NoteLatchIfTripped(g_state, entry.vk_dlss.get());  // a device loss first seen here still decides the latch
    break;
  }
}

// Plan 15 (design 2026-10-02): the game's NVSDK_NGX_D3D12_Shutdown1 for `device` (a DLSS settings change that re-initialises NGX, or Streamline's slShutdown
// at exit), on the game's thread before the core's own runs. Native NR on the game's device makes its last NGX calls here (after a wait of at most 2 s for
// its submitted work, fences only) and is held off until the game's DLSS is created there again; the lock is released when this returns, before the
// original runs. A device without Uplift's mark (a bridge's private one, any other) returns before the lock. The same guards as the Vulkan twin's.
void AddonRouting::BeforeCoreShutdownD3D12(ID3D12Device* device) {
  if (t_holds_lock_for_nr || g_process_exiting.load(std::memory_order_acquire)) return;
  api::device* owner = nullptr;
  UINT size = sizeof(owner);
  if (FAILED(device->GetPrivateData(addon::UPLIFT_RESHADE_DEVICE_GUID, &size, &owner)) || size != sizeof(owner) || owner == nullptr) return;
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;  // the Session's unload reaches the snippet's Shutdown1 on this thread
  const auto found = g_state->devices.find(owner);
  if (found == g_state->devices.end() || Bridged(found->second)) return;
  DeviceEntry& entry = found->second;
  const uint64_t creates = g_state->bridge.Registry().UpscalerCreates(owner);
  const uint64_t serial = g_state->bridge.Registry().LastSerial();
  if (!entry.context) {
    // Fix round, minor 3: between two contexts (the present queue went, and no present has made the next one yet) nothing of NGX's is held here; the hold
    // waits on the entry for the next context.
    if (!entry.core_hold.Abandoned()) {
      entry.core_hold.Hold(creates, serial);
      nr::Log(nr::LogLevel::INFO, "Direct3D 12: the game shut NGX down on its device; NR was off there");
    }
    return;
  }
  addon::DeviceContext* const context = entry.context.get();
  context->OnCoreShutdown(addon::D3D12_CORE_SHUTDOWN_WAIT, creates, serial);
  NoteLatchIfTripped(g_state, context);  // a removal first seen here still decides the latch
}

// Plan 18: the NGX API the game's DLSS on this device goes through, for the registry's per-API questions.
ngx_hooks::NgxApi DlssApiOf(const DeviceEntry& entry) {
  return (entry.vulkan ? ngx_hooks::NgxApi::VULKAN : entry.d3d11 ? ngx_hooks::NgxApi::D3D11 : ngx_hooks::NgxApi::D3D12);
}

// Plan 18 (design §5): why the DLSS stages cannot run on this Direct3D 11 device, or empty. With the add-on's lock held.
std::string_view D3D11DlssReason(const AddonState& state, const DeviceEntry& entry) {
  if (!state.dlss_unavailable_reason.empty()) return state.dlss_unavailable_reason;  // hooks off or failed, ReShade, the latch
  if (entry.foreign_dlss && !(entry.context && entry.context->DlssSeen())) return ui::FOREIGN_D3D12_DLSS_REASON;
  return {};
}

// Plan 18 (design §7): DiagnosticRemoveDevice at a Direct3D 11 DLSS stage removes Uplift's private device inside the game's frame, right after a hand-off
// queued the game's wait on it, so the e2e case proves the game's queue is released. The present-time removal skips such a device.
// Test phase (design §6, the owner's criterion correction): the removal happens with both queues at rest, only waiting, as the never-hang proof means it. On
// the removal frame the private queue is first held by a gate fence nobody signals, queued ahead of the hand-off's own work, so NR's kernels do not run, and
// the removal waits (DiagnosticWaitForGameSignal, up to 500 ms) until the game's queue passed its copies into the shares and sits at its wait. Removing the
// device while GPU work touching its allocations runs (NR's kernels, or the game's copies in) faults every SM (MMU fault) and the GPU's recovery hangs the
// game's device too, whatever the fences do: measured DEVICE_HUNG and 384 nvlddmkm events with no gate, and the same with the gate alone; with both queues
// waiting the removal releases the game's wait at once (probes P1-P4, and this case: 0 events). The gate exists only on that one frame of the hidden key.
Microsoft::WRL::ComPtr<ID3D12Fence> DiagnosticGateMidFrame(const AddonState& state, const DeviceEntry& entry) {
  if (state.settings.diagnostic_remove_device == 0u || entry.strikes.Stopped() || !entry.d3d11_bridge
      || entry.diagnostic_frames + 1u != state.settings.diagnostic_remove_device) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D12Fence> gate;
  if (FAILED(entry.d3d11_bridge->Device()->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))
      || FAILED(entry.d3d11_bridge->Queue()->Wait(gate.Get(), 1u))) {
    nr::Log(nr::LogLevel::WARN, "Diagnostic (DiagnosticRemoveDevice): the private queue could not be held before NR's work; no removal on this frame");
    return nullptr;
  }
  return gate;
}

// After the hand-off of a frame DiagnosticGateMidFrame held (`gate`): the removal, from the CPU, while the private queue waits on the gate. A held frame is
// always removed, even when its hand-off skipped, so the queue never stays held; a held frame that cannot be removed releases its gate instead.
void DiagnosticRemoveMidFrame(const AddonState& state, DeviceEntry& entry, const bridge::DlssHandoff& handoff, ID3D12Fence* gate) {
  if (state.settings.diagnostic_remove_device == 0u) return;
  if (handoff.wrote && !entry.strikes.Stopped()) {
    ++entry.diagnostic_frames;
  }
  if (gate == nullptr) return;
  Microsoft::WRL::ComPtr<ID3D12Device5> removable;
  if (ID3D12Device* const private_device = PrivateDevice(entry);
      !entry.strikes.Stopped() && private_device != nullptr && SUCCEEDED(private_device->QueryInterface(IID_PPV_ARGS(&removable)))) {
    const bool game_waiting = entry.d3d11_bridge->DiagnosticWaitForGameSignal(500u);
    nr::Logf(nr::LogLevel::WARN,
             "Diagnostic (DiagnosticRemoveDevice = {}): removing Uplift's own private Direct3D 12 device inside the game's frame (its queue held ahead of "
             "NR's work; the game's queue {})",
             state.settings.diagnostic_remove_device, (game_waiting ? "at its wait" : "not yet at its wait after 500 ms"));
    removable->RemoveDevice();
    return;
  }
  gate->Signal(1u);
  nr::Log(nr::LogLevel::WARN, "Diagnostic (DiagnosticRemoveDevice): Uplift's private device could not be removed on this frame; its queue is released");
}

const void* AddonRouting::DeviceOfD3D11(ID3D12GraphicsCommandList* list) {
  bool deferred = false;
  const void* const owner = addon::D3D11ContextOwner(nr::D3D11ContextOf(list), &deferred);
  if (deferred) {
    static std::atomic<bool> logged = false;
    if (!logged.exchange(true)) {
      nr::Log(nr::LogLevel::INFO, "Direct3D 11: a DLSS call on a deferred context is passed through untouched (NGX's Direct3D 11 backend refuses them)");
    }
  }
  return owner;
}

const void* AddonRouting::DeviceOfD3D11Device(ID3D11Device* device) {
  return addon::D3D11DeviceMark(device);
}

// Plan 18 (design §3, §4): after the original evaluate of a Direct3D 11 main handle, on the game's thread and immediate context: the context decides first
// (nothing crosses unless NR runs), then the bridge's hand-off (After DLSS) or the ring copy (Source = Present). The lock is held for the decision, the copies
// and the submit only; nothing waits on the CPU.
void AddonRouting::OnMainEvaluateD3D11(const void* device, ID3D12GraphicsCommandList* /*list*/, const ngx_hooks::DlssFrame& frame) {
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;  // as OnMainEvaluate's: a starving present's catch-up may unload NR here
  AddonState& state = *g_state;
  auto* const owner = static_cast<api::device*>(const_cast<void*>(device));
  const auto found = state.devices.find(owner);
  if (found == state.devices.end() || !found->second.d3d11_bridge || !found->second.context) return;
  DeviceEntry& entry = found->second;
  const auto now = std::chrono::steady_clock::now();
  if (const HRESULT removed = reinterpret_cast<ID3D11Device*>(owner->get_native())->GetDeviceRemovedReason(); FAILED(removed)) {
    entry.context->NoteGameDeviceRemoved(removed, now);  // design §6: the latch window decides; nothing crosses again
    NoteLatchIfTripped(&state, entry.context.get());
    return;
  }
  const nr::Rect output = bridge::ResolveRegion11(nr::D3D11ResourceOf(frame.output), frame.output_region);
  const addon::BridgedDlssDecision decision = entry.context->DecideAfterEvaluate(frame, {output.width, output.height}, now);
  if (decision.work == addon::BridgedDlssWork::PRESENT_MOTION) {
    ID3D11Resource* const motion = nr::D3D11ResourceOf(frame.motion_vectors);
    if (entry.d3d11_bridge->CopyPresentMotion(motion, frame.motion_region, decision.motion_scale_x, decision.motion_scale_y) && !entry.present_motion_logged) {
      entry.present_motion_logged = true;
      const nr::Rect region = bridge::ResolveRegion11(motion, frame.motion_region);
      nr::Logf(nr::LogLevel::INFO, "Direct3D 11: DLSS's motion vectors copied for Present ({}x{})", region.width, region.height);
    }
  } else if (decision.work == addon::BridgedDlssWork::AFTER_DLSS) {
    const Microsoft::WRL::ComPtr<ID3D12Fence> gate = DiagnosticGateMidFrame(state, entry);  // null but on the hidden key's removal frame
    DiagnosticRemoveMidFrame(state, entry, bridge::RunDlssStage(*entry.d3d11_bridge, *entry.context, decision.work, frame, now), gate.Get());
  }
  NoteLatchIfTripped(&state, entry.context.get());
}

// Plan 18 (design §3): before the original evaluate: NR before upscaling through the hand-off; DLSS reads the bridge's shared RGBA16F texture as Color for this
// one evaluate (the detour swaps it in and puts the game's own back).
ID3D12Resource* AddonRouting::BeforeMainEvaluateD3D11(const void* device, ID3D12GraphicsCommandList* /*list*/, const ngx_hooks::DlssFrame& frame) {
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;  // as OnMainEvaluate's: a starving present's catch-up may unload NR here
  AddonState& state = *g_state;
  auto* const owner = static_cast<api::device*>(const_cast<void*>(device));
  const auto found = state.devices.find(owner);
  if (found == state.devices.end() || !found->second.d3d11_bridge || !found->second.context) return nullptr;
  DeviceEntry& entry = found->second;
  const auto now = std::chrono::steady_clock::now();
  if (const HRESULT removed = reinterpret_cast<ID3D11Device*>(owner->get_native())->GetDeviceRemovedReason(); FAILED(removed)) {
    entry.context->NoteGameDeviceRemoved(removed, now);  // design §6: the latch window decides; nothing crosses again
    NoteLatchIfTripped(&state, entry.context.get());
    return nullptr;
  }
  const nr::Rect color = bridge::ResolveRegion11(nr::D3D11ResourceOf(frame.color), frame.color_region);
  const addon::BridgedDlssDecision decision = entry.context->DecideBeforeEvaluate(frame, {color.width, color.height}, now);
  if (decision.work != addon::BridgedDlssWork::BEFORE_UPSCALING) return nullptr;
  const Microsoft::WRL::ComPtr<ID3D12Fence> gate = DiagnosticGateMidFrame(state, entry);  // null but on the hidden key's removal frame
  const bridge::DlssHandoff handoff = bridge::RunDlssStage(*entry.d3d11_bridge, *entry.context, decision.work, frame, now);
  DiagnosticRemoveMidFrame(state, entry, handoff, gate.Get());
  NoteLatchIfTripped(&state, entry.context.get());
  return (handoff.color != nullptr ? nr::AsResource(handoff.color) : nullptr);
}

void AddonRouting::ColorSwapRejectedD3D11(const void* device, ID3D12GraphicsCommandList* /*list*/) {
  const std::unique_lock lock(g_state->mutex);
  const auto found = g_state->devices.find(static_cast<api::device*>(const_cast<void*>(device)));
  if (found != g_state->devices.end() && found->second.context) {
    found->second.context->OnColorSwapRejected();
  }
}

// Plan 18 (design §5): the process's first game DLSS evaluate, once, at INFO, for remote testers. Lock-free: the device by its mark or its list's state.
// Batch 1 review M3: NgxBridge calls this ahead of Vulkan's close guard, so it never throws; a failure logs nothing.
void AddonRouting::LogFirstEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) {
  try {
    std::string_view device = "a Vulkan device";
    if (api == ngx_hooks::NgxApi::D3D11) {
      bool deferred = false;
      const void* const owner = addon::D3D11ContextOwner(nr::D3D11ContextOf(list), &deferred);
      device = (deferred ? "a deferred context (passed through)"
                : owner != nullptr ? "the game's device"
                                   : "an immediate context (Uplift has not marked its device yet: NR has not been on)");
    } else if (api == ngx_hooks::NgxApi::D3D12) {
      device = (addon::FindListState(list) != nullptr ? "a device ReShade tracks" : "a device ReShade does not track (a mod's own?)");
    }
    nr::Log(nr::LogLevel::INFO, addon::FormatDlssSummary(addon::SummarizeEvaluate(api, frame, device)));
  } catch (...) {
    // No-op: the summary is for the log alone, and the evaluate's own path must go on.
  }
}

// NR's view of ReShade's immediate command list on the presenting queue. ReShade drops a queue's
// immediate list when creating it fails, so the list is fetched once here and every use is
// null-checked: `Valid()` tells the caller (RunOrWarnOnce below) whether to run NR at all this frame.
class ReshadeFrameHost final : public addon::FrameHost {
 public:
  ReshadeFrameHost(api::command_queue* queue, api::resource back_buffer)
      : queue_(queue), back_buffer_(back_buffer), list_(queue->get_immediate_command_list()) {}

  [[nodiscard]] bool Valid() const { return list_ != nullptr; }

  void FlushPending() override { queue_->flush_immediate_command_list(); }
  ID3D12GraphicsCommandList* NativeList() override {
    if (list_ == nullptr) return nullptr;
    return reinterpret_cast<ID3D12GraphicsCommandList*>(list_->get_native());
  }
  // Through ReShade's API, which also marks the list as holding commands so ReShade submits it.
  void TargetBarrier(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) override {
    if (list_ == nullptr) return;
    const auto usage = [](D3D12_RESOURCE_STATES state) {
      switch (state) {
        case D3D12_RESOURCE_STATE_RENDER_TARGET: return api::resource_usage::render_target;
        case D3D12_RESOURCE_STATE_COPY_SOURCE:   return api::resource_usage::copy_source;
        default:                                 return api::resource_usage::present;
      }
    };
    list_->barrier(back_buffer_, usage(before), usage(after));
  }

 private:
  api::command_queue* queue_;
  api::resource back_buffer_;
  api::command_list* list_;
};

// The device entry whose primary swap chain presents through `runtime`, when `cmd_list` is that
// frame's immediate list. Effects another add-on renders on a game command list are ignored.
DeviceEntry* FindFrameEntry(AddonState* state, api::effect_runtime* runtime, api::command_list* cmd_list) {
  const auto found = state->devices.find(runtime->get_device());
  if (found == state->devices.end()) return nullptr;
  [[maybe_unused]] auto& [device, entry] = *found;
  if ((!entry.context && !entry.helper.HasClient()) || entry.runtime != runtime || entry.queue == nullptr) return nullptr;
  if (cmd_list != entry.queue->get_immediate_command_list()) return nullptr;
  return &entry;
}

// Runs NR through `host` for this frame, or skips it with a one-time WARN if ReShade could not
// give the presenting queue an immediate command list.
void RunOrWarnOnce(AddonState* state, addon::DeviceContext* context, ReshadeFrameHost* host,
                   D3D12_RESOURCE_STATES entry_state, addon::TriggerPoint point,
                   ID3D12Resource* launchpad_motion = nullptr) {
  if (host->Valid()) {
    context->Run(host, entry_state, point, launchpad_motion);
    return;
  }
  if (!state->warned_no_immediate_list) {
    state->warned_no_immediate_list = true;
    nr::Log(nr::LogLevel::WARN, "ReShade gave no immediate command list for this queue; NR is skipped until it does");
  }
}

// Plan 11 (Vulkan design §3.6): one Vulkan image as the bridge describes it, from ReShade's description (shared with the helper front).
using addon::VulkanImageInfo;

// The bridge's shared UPLIFT_MASK goes (the effect stopped, or NR is off). Called with the add-on's lock held.
void ReleaseBridgeMask(DeviceEntry& entry, api::device* device) {
  if (entry.d3d11_bridge) {
    entry.d3d11_bridge->ReleaseMask();
  }
  if (entry.d3d10_bridge) {
    entry.d3d10_bridge->ReleaseMask();
  }
  if (entry.vk_bridge) {
    addon::ReshadeVkHost host(device, entry.queue, entry.vk_bridge->VulkanDevice().vkQueueSubmit);
    entry.vk_bridge->ReleaseMask(host);
  }
  if (entry.gl_bridge) {
    // A delete needs the runtime's context current (key decision g): while it is not, the names wait for the next valid frame.
    addon::ReshadeGlHost host(entry.gl_bridge->Gl(), entry.queue);
    entry.gl_bridge->ReleaseMask(host);
  }
}

// Plan 7 (D3D11 design §2), Plan 8 (D3D10 design §2), Plan 11 (Vulkan design §3): NR at `point` for a bridged device. The bridge's copies and
// hand-offs wrap DeviceContext::Run on the private device. With the add-on's lock held; nothing here waits on the CPU
// for the GPU, except a Vulkan device's capped waits (2 s: the present queue, and the CPU-ordered path's). `launchpad_motion`: this
// frame's UPLIFT_MV handle (Task 7, decision 3), from the Uplift technique's event; 0 elsewhere. `entry_state`: the Vulkan back buffer's
// state in the calling event (PRESENT at the present point, RENDER_TARGET in the effects); unused elsewhere. `frame`: an OpenGL device's frame, FB0 at the
// present point and ReShade's intermediate (the event's rtv resource) at the technique and after the effects (design §3.2); unused elsewhere.
void RunBridged(DeviceEntry& entry, api::device* device, addon::TriggerPoint point, uint64_t launchpad_motion, vk::Usage entry_state,
                api::resource frame = {0u}) {
  if (point == addon::TriggerPoint::NONE || !entry.context->FrameReady()) return;
  const bridge::Recorder record = [&entry, point](ID3D12GraphicsCommandList* list, const bridge::BridgeTargets& targets) {
    return bridge::RecordNr(*entry.context, point, list, targets);
  };
  if (entry.gl_bridge) {
    // The present event checks that its queue is the runtime's (R63); the effect events have no present queue, only the current context to check.
    addon::ReshadeGlHost host(entry.gl_bridge->Gl(), entry.queue, (entry_state == vk::Usage::PRESENT ? entry.present_queue : nullptr));
    entry.gl_bridge->Run(host, addon::GlImageInfo(device, frame), addon::GlImageInfo(device, {launchpad_motion}), record);
    return;
  }
  if (entry.vk_bridge) {
    const PFN_vkQueueSubmit submit = entry.vk_bridge->VulkanDevice().vkQueueSubmit;
    addon::ReshadeVkHost host(device, entry.queue, submit);
    // Design §3.4 case 2: the game presents from a queue other than the effect runtime's. Uplift's submissions are ordered after the game's frame
    // only by submission order on the runtime's queue, so the bridge flushes the present queue and waits for it on the CPU (2 s; a timeout skips this
    // frame's NR) inside Run, once per frame, after its own skip checks and before the copy in (final review, minor 4).
    std::optional<addon::ReshadeVkHost> present_host;
    if (entry.present_queue != nullptr && entry.queue != nullptr && entry.present_queue != entry.queue) {
      if (!entry.vk_second_queue_logged) {
        entry.vk_second_queue_logged = true;
        nr::Log(nr::LogLevel::INFO,
                "Vulkan: the game presents from another queue than ReShade's effects; Uplift waits for it on the CPU before NR");
      }
      entry.vk_bridge->NoteSecondQueue();
      present_host.emplace(device, entry.present_queue, submit);
    }
    // Plan 14 (design §2.4): DLSS's motion vectors copied in the game's frame for this present, when the native context made one (Auto's order: DLSS first);
    // otherwise the Uplift technique's UPLIFT_MV, as before.
    const vk::NrImage dlss_motion = (entry.vk_dlss ? entry.vk_dlss->PresentMotion() : vk::NrImage{});
    const bool motion_is_dlss = (dlss_motion.image != VK_NULL_HANDLE);
    const vk::ImageInfo motion = (motion_is_dlss ? vk::ImageInfo{.image = dlss_motion.image,
                                                                 .size = {dlss_motion.ngx.Resource.ImageViewInfo.Width, dlss_motion.ngx.Resource.ImageViewInfo.Height},
                                                                 .format = DXGI_FORMAT_R16G16_FLOAT,
                                                                 .samples = 1u}
                                                 : VulkanImageInfo(device, {launchpad_motion}));
    entry.vk_bridge->Run(host, VulkanImageInfo(device, entry.back_buffer), entry_state, motion, record, (present_host ? &*present_host : nullptr), motion_is_dlss);
    return;
  }
  if (entry.d3d10_bridge) {
    entry.d3d10_bridge->Run(reinterpret_cast<ID3D10Resource*>(entry.back_buffer.handle),
                            reinterpret_cast<ID3D10Resource*>(launchpad_motion), record);
  } else {
    // Plan 18 (design §4): at Present, this frame's ring slot of DLSS's vectors (copied in the game's evaluate) comes before Launchpad's.
    entry.d3d11_bridge->Run(reinterpret_cast<ID3D11Resource*>(entry.back_buffer.handle),
                            reinterpret_cast<ID3D11Resource*>(launchpad_motion), record,
                            /*dlss_motion=*/entry.context->CurrentPlacement() == addon::Placement::PRESENT);
  }
}

// Final review I-1 (addon::MaskEffectRunning, shared with the helper front since Plan 10): a found UPLIFT_MASK variable does not mean
// an effect still writes it.
using addon::MaskEffectRunning;

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

// Final review I-1: a new preset is linked afresh once. ReShade raises this after every reload too (the link's own
// included), with the same path, which must not reset anything.
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

void OnDestroyCommandQueue(api::command_queue* queue) {
  try {
    api::device* const queue_device = queue->get_device();
    if (queue_device->get_api() == api::device_api::d3d9) {
      // Plan 10 (design §2.2): a Direct3D 9 Reset raises this, not destroy_device. The front releases the client's DEFAULT-pool
      // objects (a live one fails the game's Reset) and detaches the helper.
      const std::unique_lock lock(g_state->mutex);
      const auto found = g_state->devices.find(queue_device);
      if (found != g_state->devices.end()) {
        g_state->front->DestroyQueue(found->second.helper, queue_device);
      }
      return;
    }
    if (queue_device->get_api() != api::device_api::d3d12) return;
    const std::unique_lock lock(g_state->mutex);
    const HoldsLockForNr holds;  // Plan 15: the teardown's unload reaches the snippet's Shutdown1 on this thread, under the lock
    auto* const native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    for (auto& [device, entry] : g_state->devices) {
      if (!entry.context) continue;
      if (entry.context->PresentQueue() != native_queue) {
        entry.context->ForgetQueue(native_queue);  // tokens submitted there are never signalled on it again
        continue;
      }
      // The frame fence is signalled on this queue: unload synchronously while it still exists.
      entry.context->Teardown();
      // Important 1 (fix round 1): a removal first seen only at teardown still decides the latch,
      // checked before the context that just decided it is destroyed below.
      NoteLatchIfTripped(g_state, entry.context.get());
      entry.core_hold = entry.context->CoreHold();  // Plan 15 fix round (minor 3): the next context on this device starts from it
      entry.context.reset();
      entry.queue = nullptr;
      g_state->claim.Forget(device);
    }
  }
  UPLIFT_CATCH("destroy_command_queue", )
}

void OnDestroyDevice(api::device* device) {
  try {
    // Plan 7 (key decision a): a D3D11 device's bridge goes after the lock. It releases the private device ReShade
    // returned, whose destroy_device comes back into this handler on this thread.
    std::unique_ptr<bridge::D3D11Bridge> d3d11_bridge;
    std::unique_ptr<bridge::D3D10Bridge> d3d10_bridge;  // Plan 8: goes after the lock too (its relay's proxy)
    std::unique_ptr<bridge::VkBridge> vk_bridge;        // Plan 11: the same, for a Vulkan device's private D3D12 device
    std::unique_ptr<bridge::GlBridge> gl_bridge;        // Plan 12: and for an OpenGL context's
    std::vector<std::unique_ptr<bridge::VkBridge>> retiring_vk;  // Plan 17: Vulkan bridges a retry tore down, still behind their fences
    std::unique_ptr<addon::VkDlssContext> vk_dlss;      // Plan 13: abandoned under the lock, destroyed after it (no Vulkan or NGX call)
    {
      const std::unique_lock lock(g_state->mutex);
      const HoldsLockForNr holds;  // Plan 15: as destroy_command_queue's
      const auto found = g_state->devices.find(device);
      if (found != g_state->devices.end() && found->second.vk_dlss) {
        // Plan 13 (design R83): the vkDestroyDevice detour did not tear the native context down (it moves it out when it does). ReShade has already
        // dropped the device from its dispatch map: no NGX call and no Vulkan call, everything NR holds stays with the device's end.
        found->second.vk_dlss->Abandon();
        NoteLatchIfTripped(g_state, found->second.vk_dlss.get());
        g_state->claim.Forget(found->second.vk_dlss.get());
        vk_dlss = std::move(found->second.vk_dlss);
        g_state->bridge.AddVkContexts(-1);
        g_state->bridge.NoteVkContextNr(found->second.vk_nr_counted, false);
      }
      if (device->get_api() == api::device_api::vulkan) {
        const std::scoped_lock orphans_lock(g_state->vk_orphans_mutex);
        std::erase_if(g_state->vk_orphans, [device](const addon::StaleVkList& orphan) { return orphan.device == device; });
        // Batch 3 review, minor 3: vkDestroyDevice frees the device's pools without destroy_command_list, so its buffers' states (and their tokens) go
        // here; a later device at this address, reusing a buffer handle, never inherits them.
        addon::ForgetVkListStates(device);
      }
      if (device->get_api() == api::device_api::d3d9) {
        // Plan 10: every Direct3D 9 device, also one that never presented (the 9Ex marker's rule); an entry's clients are released. It hands
        // back a Direct3D 10 client to destroy after the lock; a Direct3D 9 device has none (D3D10 devices here go through D3D10Bridge).
        static_cast<void>(g_state->front->DestroyDevice((found != g_state->devices.end() ? &found->second.helper : nullptr), device));
      }
      if (found != g_state->devices.end() && found->second.d3d11_bridge) {
        // Final review I-2: before Teardown, so a wedged private queue drains during Teardown's own wait instead of
        // running lists later over NR resources Teardown freed. Never in the bridge's destructor, which runs after it.
        found->second.d3d11_bridge->Stop("The Direct3D 11 bridge stopped: the game's Direct3D 11 device was destroyed");
      }
      if (found != g_state->devices.end() && found->second.d3d10_bridge) {
        found->second.d3d10_bridge->Stop("The Direct3D 10 bridge stopped: the game's Direct3D 10 device was destroyed");
      }
      if (found != g_state->devices.end() && found->second.vk_bridge) {
        found->second.vk_bridge->Stop("The Vulkan bridge stopped: the game's Vulkan device was destroyed");
      }
      if (found != g_state->devices.end() && found->second.gl_bridge) {
        found->second.gl_bridge->Stop("The OpenGL bridge stopped: the game's OpenGL context was destroyed");
      }
      if (found != g_state->devices.end() && found->second.context) {
        // Important 1 (fix round 1): Teardown() first, so a removal first seen only here still decides
        // the latch while Status() can still be read -- the erase below would otherwise destroy this
        // context (its destructor tears it down the same way, but Status() is gone with it by then).
        found->second.context->Teardown();
        NoteLatchIfTripped(g_state, found->second.context.get());
      }
      if (device->get_api() == api::device_api::d3d12) {
        // Plan 15: Uplift's mark goes with the ReShade device (ReShade removes its own the same way, after this event); a no-op on a device never marked.
        reinterpret_cast<ID3D12Device*>(device->get_native())->SetPrivateData(addon::UPLIFT_RESHADE_DEVICE_GUID, 0u, nullptr);
      }
      if (device->get_api() == api::device_api::d3d11) {
        addon::MarkD3D11Device(reinterpret_cast<ID3D11Device*>(device->get_native()), nullptr);  // Plan 18: the mark goes with the ReShade device
      }
      if (found != g_state->devices.end()) {
        g_state->bridge.NoteD3D11Watch(found->second.d3d11_watch_counted, false);
      }
      if (found != g_state->devices.end() && found->second.vk_bridge) {
        // Vulkan design §6, R62: ReShade has already deleted its queues and dropped this device from its dispatch map, so no Vulkan call through
        // its layer may follow. An image goes only through ReShade's destroy_resource (the host, with no queue, does exactly that); memory,
        // semaphores and fences through functions the layer does not intercept. After Stop and Teardown, as the D3D bridges.
        addon::ReshadeVkHost host(device, nullptr, found->second.vk_bridge->VulkanDevice().vkQueueSubmit);
        found->second.vk_bridge->FreeVulkan(host);
      }
      if (found != g_state->devices.end() && found->second.gl_bridge) {
        // OpenGL design §3.6: ReShade makes the share group's last context current only for its own teardown, so Uplift makes no GL call here at all: its
        // names die with the share group. After Stop and Teardown, as the D3D bridges.
        found->second.gl_bridge->ForgetGl();
      }
      if (found != g_state->devices.end()) {
        // Plan 17: a retry's torn-down Vulkan bridges free what is left of their imports as the live one does (R62: no queue, the device is idle).
        for (std::unique_ptr<bridge::VkBridge>& retiring : found->second.retiring_vk) {
          addon::ReshadeVkHost host(device, nullptr, retiring->VulkanDevice().vkQueueSubmit);
          retiring->FreeVulkan(host);
        }
        retiring_vk = std::move(found->second.retiring_vk);
      }
      if (device->get_api() == api::device_api::vulkan) {
        // The pending marker's other end (design §2.3): every Vulkan device, also one that never presented. The hook's own lock, never this one's.
        vk::DeviceHook::NoteDestroyed(reinterpret_cast<VkDevice>(device->get_native()));
      }
      if (found != g_state->devices.end()) {
        d3d11_bridge = std::move(found->second.d3d11_bridge);  // the context, erased below, goes first
        d3d10_bridge = std::move(found->second.d3d10_bridge);
        vk_bridge = std::move(found->second.vk_bridge);
        gl_bridge = std::move(found->second.gl_bridge);
      }
      // Erasing destroys the entry's DeviceContext (Teardown() above already made that a no-op).
      g_state->devices.erase(device);
      g_state->claim.Forget(device);
      g_state->d3d9_claim.Forget(device);
      g_state->bridge.DeviceDestroyed(device);  // Plan 14: a device recreated at this address does not start as "DLSS seen"
    }
  }
  UPLIFT_CATCH("destroy_device", )
}

// Plan 13 (design §3.4): the add-on's lock is taken by a Vulkan command-list event only for a buffer that holds Uplift tokens, which only a game
// buffer the NGX detour recorded in can (Uplift's own work and the NR runtime's never get one), so no event Uplift's own Vulkan calls raise under
// that lock ever waits for it. init_command_list never takes it.
addon::VkDlssContext* VkContextOf(const void* device) {
  const auto found = g_state->devices.find(static_cast<api::device*>(const_cast<void*>(device)));
  return (found == g_state->devices.end() ? nullptr : found->second.vk_dlss.get());
}

// Identity and completion-token bookkeeping: registered whenever the NGX hooks run.
void OnInitCommandList(api::command_list* cmd_list) {
  try {
    const api::device_api device_api = cmd_list->get_device()->get_api();
    if (device_api == api::device_api::vulkan) {
      PollHooks();
      // Batch 1 review, minor 3: a stale state for a reused handle (its pool was destroyed unannounced) hands its tokens to the device's next present.
      if (std::optional<addon::StaleVkList> stale = addon::CreateVkListState(cmd_list)) {
        const std::scoped_lock orphans_lock(g_state->vk_orphans_mutex);
        g_state->vk_orphans.push_back(std::move(*stale));
      }
      return;
    }
    if (device_api != api::device_api::d3d12) return;
    PollHooks();
    addon::CreateListState(cmd_list);
  }
  UPLIFT_CATCH("init_command_list", )
}

void OnDestroyCommandList(api::command_list* cmd_list) {
  try {
    if (cmd_list->get_device()->get_api() == api::device_api::vulkan) {
      const std::optional<addon::VkListState> list_state = addon::DestroyVkListState(cmd_list);
      if (!list_state || list_state->tokens.empty()) return;
      const std::unique_lock lock(g_state->mutex);
      if (addon::VkDlssContext* const context = VkContextOf(list_state->device)) {
        context->Recycle(list_state->tokens);  // its events back to the pool; the buffer is not pending (Vulkan's rule)
      }
      return;
    }
    if (addon::FindListState(cmd_list) == nullptr) return;
    api::device* const device = cmd_list->get_device();
    const std::vector<uint64_t> unsubmitted = addon::DestroyListState(cmd_list);
    if (!unsubmitted.empty()) {
      DropTokens(device, unsubmitted);
    }
  }
  UPLIFT_CATCH("destroy_command_list", )
}

void OnResetCommandList(api::command_list* cmd_list) {
  try {
    if (cmd_list->get_device()->get_api() == api::device_api::vulkan) {
      addon::VkListState* const vk_list = addon::FindVkListState(reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cmd_list->get_native())));
      if (vk_list == nullptr) return;
      PollHooks();
      if (!vk_list->tokens.empty()) {
        const std::unique_lock lock(g_state->mutex);
        if (addon::VkDlssContext* const context = VkContextOf(vk_list->device)) {
          context->Recycle(vk_list->tokens);  // a new recording: the last one's events back to the pool
        }
      }
      vk_list->tokens.clear();
      vk_list->first_token = 0u;
      vk_list->open_token = 0u;
      vk_list->executed = false;
      return;
    }
    addon::ListState* const list_state = addon::FindListState(cmd_list);
    if (list_state == nullptr) return;
    PollHooks();
    StampThisThread();
    if (!list_state->tokens.empty()) {
      DropTokens(list_state->device, list_state->tokens);  // recorded, never submitted
      list_state->tokens.clear();
    }
    list_state->first_token = 0u;
    list_state->executed = false;
    list_state->shadow.OnReset(addon::TrackingEpoch());  // 0 while tracking is off: unknown until the next Reset
  }
  UPLIFT_CATCH("reset_command_list", )
}

void OnExecuteCommandList(api::command_queue* queue, api::command_list* cmd_list) {
  try {
    if (cmd_list->get_device()->get_api() == api::device_api::vulkan) {
      // Before the native submit, under the queue's lock in ReShade. Never a queue signal here (design §3.3): the tokens are the buffer's events.
      addon::VkListState* const vk_list = addon::FindVkListState(reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cmd_list->get_native())));
      if (vk_list == nullptr) return;
      PollHooks();
      if (!vk_list->tokens.empty()) {
        const std::unique_lock lock(g_state->mutex);
        if (addon::VkDlssContext* const context = VkContextOf(vk_list->device)) {
          if (vk_list->executed) {
            context->Reexecuted(vk_list->tokens, vk_list->first_token, queue);  // N12 on Vulkan: submitted again without a new recording
          } else {
            context->Executed(vk_list->tokens);
          }
        }
      }
      vk_list->executed = true;
      return;
    }
    addon::ListState* const list_state = addon::FindListState(cmd_list);
    if (list_state == nullptr) return;
    PollHooks();
    // No StampThisThread() here (Task 5 review of Plan 3): ReShade raises this event for every list of one
    // ExecuteCommandLists call before the single native call, so a Signal now could precede the submission
    // of an earlier list in the same batch. Reset, evaluate and present stamp instead.
    const bool executed_again = (list_state->tokens.empty() && list_state->executed && list_state->first_token != 0u);
    if (list_state->tokens.empty() && !executed_again) return;
    const std::unique_lock lock(g_state->mutex);
    if (addon::DeviceContext* const context = ContextOf(list_state->device)) {
      auto* const native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
      const auto now = std::chrono::steady_clock::now();
      if (executed_again) {
        context->ResubmitTokens(list_state->first_token, native_queue, GetCurrentThreadId(), now);  // N12
      } else {
        context->SubmitTokens(list_state->tokens, native_queue, GetCurrentThreadId(), now);
      }
      t_unstamped_device = list_state->device;  // this thread stamps it at its next event
    }
    list_state->tokens.clear();
    list_state->executed = true;
  }
  UPLIFT_CATCH("execute_command_list", )
}

bool OnOpenOverlay(api::effect_runtime* /*runtime*/, bool open, api::input_source /*source*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    if (!open) {
      // Plan 6 (D6): defaults_view is session-only and survives closing the overlay; a slider held as
      // it closes must not stay coalesced, so everything else about the panel still resets.
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
    if (device_api != api::device_api::d3d12 && device_api != api::device_api::d3d11 && device_api != api::device_api::d3d10
        && device_api != api::device_api::d3d9 && device_api != api::device_api::vulkan && device_api != api::device_api::opengl) {
      return;
    }
    const bool d3d9 = (device_api == api::device_api::d3d9);  // Plan 10: routed to the helper front after the common prefix
    const bool vulkan = (device_api == api::device_api::vulkan);
    const bool opengl = (device_api == api::device_api::opengl);  // Plan 12: the same shape as Vulkan, on a GL context
    PollHooks();
    StampThisThread();
    const auto now = std::chrono::steady_clock::now();
    // Plan 7 (key decision a): the private device D3D12Side made for a new bridge. On the fallback it is D3D12CreateDevice's (ReShade's proxy in a game),
    // whose last release raises destroy_device, whose handler takes the add-on's lock; the device factory's raises nothing, and goes the same way.
    // Declared before the lock, it is released after it.
    Microsoft::WRL::ComPtr<ID3D12Device> release_after_unlock;
    // Plan 8: what D3D11CreateDevice returned for a new D3D10 bridge's relay (ReShade's proxy in a game); released after
    // the lock, like release_after_unlock.
    Microsoft::WRL::ComPtr<ID3D11Device> relay_after_unlock;
    // Plan 17: what Retry now tears down, destroyed after the lock as destroy_device does (a private device's last release raises destroy_device).
    std::unique_ptr<bridge::D3D11Bridge> d3d11_after_unlock;
    std::unique_ptr<bridge::D3D10Bridge> d3d10_after_unlock;
    std::unique_ptr<bridge::GlBridge> gl_after_unlock;
    std::vector<std::unique_ptr<bridge::VkBridge>> vk_after_unlock;
    const std::unique_lock lock(g_state->mutex);
    // A Session may unload NR here (BeginFrame: the native Vulkan context's, Plan 13, and the Direct3D 12 context's, Plan 15), and its snippet may reach the
    // core's Shutdown1 on this thread, under the lock.
    const HoldsLockForNr holds;
    AddonState& state = *g_state;

    // Reads only the Enabled key: a missing or unparsable value must leave `settings.enabled` alone.
    const std::optional<bool> enabled_in_config = state.poller.Poll(now, [&state]() -> std::optional<bool> {
      const std::optional<std::string> text = state.config.Get(ui::ENABLED_KEY);
      if (!text) return std::nullopt;
      return ui::ParseBoolSetting(*text);
    });
    if (enabled_in_config) {
      state.settings.enabled = *enabled_in_config;
      nr::Logf(nr::LogLevel::INFO, "Enabled set to {} in ReShade.ini", (*enabled_in_config ? 1 : 0));
      state.front->SettingsChanged(state.settings);  // Plan 10: this does not bump Plan 6's generation, and the helper must see Enabled change
    }

    DeviceEntry& entry = state.devices[device];
    if (d3d9) {
      state.front->NotePresent(entry.helper, device, state.settings);  // before the selector, as gitc-uplift.addon32 does
    }
    if (opengl) {
      entry.opengl = true;
    }
    if (vulkan) {
      entry.vulkan = true;
      if (!entry.vk_presents_counted) {
        // Before the selector, like the 9Ex marker: the second present event of an adjusted device deletes the pending marker (design §2.3).
        entry.vk_presents_counted = vk::DeviceHook::NotePresent(reinterpret_cast<VkDevice>(device->get_native()));
      }
    }
    if (!entry.selector.OnPresent(swapchain, now)) return;
    // Plan 7: this bookkeeping runs before the context (and, on D3D11 and D3D10, the bridge) exist, so a bridged device
    // waiting for its first enable still has `entry.runtime`, which the Enable hotkey and the overlay need.
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
      // whatever queue presents. Without a runtime (effects not up yet) there is none, and nothing runs this frame. On OpenGL (key decision a) the queue is
      // the runtime's context: the present is handled only when it is the one that presents, with that context current.
      entry.present_queue = queue;
      entry.queue = (entry.runtime != nullptr ? entry.runtime->get_command_queue() : nullptr);
    }
    entry.back_buffer = swapchain->get_current_back_buffer();
    if (!d3d9) {
      const api::resource_desc back_buffer_desc = device->get_resource_desc(entry.back_buffer);
      entry.swapchain_size = {back_buffer_desc.texture.width, back_buffer_desc.texture.height};
    }
    entry.marker = {0u};
    entry.launchpad_ready = false;
    entry.helper.launchpad_ready = false;
    if (entry.runtime != nullptr && entry.runtime->get_effects_state()) {
      // find_technique returns 0 while effects are still loading: NR then runs before effects.
      const api::effect_technique technique = entry.runtime->find_technique(nullptr, MARKER_TECHNIQUE);
      if (technique.handle != 0u && entry.runtime->get_technique_state(technique)) {
        entry.marker = technique;
      }
      if (!d3d9 || addon::LAUNCHPAD_ON_D3D9) {
        // Plan 10: Direct3D 9 too, while addon::LAUNCHPAD_ON_D3D9 holds (the one switch, see launchpad_link.hpp).
        // User-approved addition (applies to D3D9, D3D10, D3D11 and D3D12): Uplift links itself to LaunchPad, so the user never
        // edits a preprocessor definition by hand. LaunchPadLink decides when (final review I-1).
        if (entry.link_runtime != entry.runtime) {
          entry.launchpad_link.Reset();
          entry.launchpad_readiness.Reset();
          entry.link_runtime = entry.runtime;
          entry.link_preset_path.clear();
        }
        // Ready only once ReShade has finished loading: find_technique and enumerate_techniques find nothing while it
        // loads (reshade-main runtime_api.cpp), which read as "LaunchPad is off" and flipped the value on every reload.
        // And only while Uplift.fx is among its effects: for an effect it does not know, ReShade's
        // set_preprocessor_definition_for_effect falls back to reloading every effect without saving the value, forever.
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
        // An Uplift.fx that failed to compile with "1" lists no technique. Its own UPLIFT_USE_LAUNCHPAD entry still
        // means ReShade knows it (only an effect it found gets one), so "0" can go back.
        if (!link_ready && link_defined) {
          entry.runtime->enumerate_techniques(nullptr, [&link_ready](api::effect_runtime*, api::effect_technique) {
            link_ready = true;  // not loading
          });
        }
        const api::effect_technique launchpad_technique = entry.runtime->find_technique(nullptr, LAUNCHPAD_TECHNIQUE);
        // Plan 14: Setup offers Launchpad's vectors only while both techniques are enabled (the Uplift technique's own order shows only at run time). ReShade
        // lists no technique while it reloads Uplift.fx, which choosing Launchpad does: the last ready value holds through that (LaunchpadReadiness).
        entry.launchpad_ready = entry.launchpad_readiness.Update(
            (technique.handle != 0u && launchpad_technique.handle != 0u),
            (entry.marker.handle != 0u && launchpad_technique.handle != 0u && entry.runtime->get_technique_state(launchpad_technique)), now);
        entry.helper.launchpad_ready = entry.launchpad_ready;
        const std::optional<bool> link = entry.launchpad_link.Update({
            .ready = link_ready,
            .wanted = (launchpad_technique.handle != 0u && entry.runtime->get_technique_state(launchpad_technique)
                       && (state.settings.motion_vectors == ui::MotionVectorSource::AUTO
                           || state.settings.motion_vectors == ui::MotionVectorSource::LAUNCHPAD)),
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
    if (d3d9) {
      // Plan 10 (design §2.2): a Direct3D 9 device's NR runs in gitc-uplift-helper64.exe (next to this add-on), through the front both
      // add-ons share: its client, the helper's FRAME, and NR at the present point. No context and no bridge exist for it.
      if (entry.helper.rejected) return;
      const bool nr_allowed = state.d3d9_claim.Update(device, state.settings.enabled, false, state.front->NrLoaded(device));
      state.front->Present(entry.helper, {
                                             .device = device,
                                             .swapchain = swapchain,
                                             .runtime = entry.runtime,
                                             .back_buffer = entry.back_buffer,
                                             .marker = entry.marker,
                                             .settings = &state.settings,
                                             .overlay = &state.overlay,
                                             .nr_allowed = nr_allowed,
                                             .claimed_elsewhere = (state.settings.enabled && !nr_allowed && state.d3d9_claim.Owner() != device),
                                             .now = now,
                                         });
      return;
    }

    // The NR runtime both of a device's contexts load (Plan 13: the bridge's and the native Vulkan one), and what keeps it from loading at all.
    const auto locate_snippet = [&state]() -> std::optional<std::filesystem::path> {
      const std::string& configured = state.settings.snippet_path;
      // I1: a hand-edited SnippetPath can be invalid UTF-8; it is treated like an unset one.
      const std::optional<std::filesystem::path> configured_path = addon::PathFromUtf8(configured);
      if (!configured_path && !configured.empty() && !state.warned_invalid_snippet_path) {
        state.warned_invalid_snippet_path = true;
        nr::Log(nr::LogLevel::WARN,
                "SnippetPath is not valid UTF-8; searching next to gitc-uplift.addon64 and the game instead");
      }
      return addon::LocateSnippet({
          .configured = configured_path.value_or(std::filesystem::path()),
          .addon_directory = state.addon_directory,
          .game_directory = state.game_directory,
      });
    };
    const auto note_static_block = [&state, &entry](const std::optional<std::filesystem::path>& snippet) {
      const std::string& configured = state.settings.snippet_path;
      if (const std::optional<std::wstring> host = addon::FindConflictingHost(addon::LoadedModuleFileNames())) {
        entry.static_block = std::format("{} is loaded and runs NR too; remove it from the add-on folder to use Uplift",
                                         addon::Utf8FromPath(*host));
        entry.block_stage = ui::CardStage::CONFLICTS;
        // NR can never load while the other host stays loaded, so the deferred SHA-256 below must not
        // hash 166 MB for a runtime that never loads.
        entry.snippet_path.clear();
      } else if (!snippet) {
        entry.static_block =
            (configured.empty() ? "nvngx_dlssnr.dll not found next to gitc-uplift.addon64 or the game; copy it there or set Runtime path"
                                : std::format("Runtime path not found: {}", configured));
        entry.block_stage = ui::CardStage::RUNTIME;
      }
    };
    const auto hash_runtime_once = [&state, &entry] {
      if (state.settings.enabled && !state.snippet_hashed && !entry.snippet_path.empty()) {
        state.snippet_hashed = true;
        nr::Logf(nr::LogLevel::INFO, "NR runtime file {} (SHA-256 {})", addon::Utf8FromPath(entry.snippet_path),
                 addon::Sha256Hex(entry.snippet_path));
      }
    };
    // Why NR stays off on this device whatever the placement: a static block, or (v2 design §3.19, ForeignNr = Yield) another NR producer.
    const auto blocked_reason = [&state, &entry]() -> std::string {
      std::optional<std::string> foreign;
      if (state.settings.foreign_nr == ui::ForeignNrMode::YIELD) {
        foreign = addon::ForeignNrProducer({
            .live_foreign_nr = state.bridge.Registry().LiveCount(ngx_hooks::FeatureKind::NEURAL_RENDERING),
            .runtime_mapped_elsewhere = (!entry.snippet_path.empty() && nr::IsRuntimeMappedElsewhere(entry.snippet_path)),
            .renodx_marker = addon::IsEnvironmentMarkerSet(addon::RENODX_NR_MARKER),
        });
      }
      return (!entry.static_block.empty() ? entry.static_block : (foreign ? std::format("{} (ForeignNr = Yield)", *foreign) : std::string()));
    };

    // Plan 5 (key decision 8) and Plan 14: whether an UPLIFT_MASK is running on this device, and why not (the note). Looked up once per present, by the first path
    // that needs it. Final review I-1: a declared UPLIFT_MASK variable also needs a technique of its effect still enabled.
    std::optional<std::pair<bool, std::string_view>> mask_lookup;
    const auto mask_state = [&state, &entry, &mask_lookup]() {
      if (!mask_lookup) {
        const api::effect_texture_variable variable =
            (state.settings.mask == ui::MaskMode::AUTO && entry.runtime != nullptr && entry.runtime->get_effects_state())
                ? entry.runtime->find_texture_variable(nullptr, MASK_TEXTURE)
                : api::effect_texture_variable{0u};
        const bool declared = variable.handle != 0u;
        mask_lookup.emplace(declared && MaskEffectRunning(entry.runtime, variable),
                            (state.settings.mask != ui::MaskMode::AUTO) ? std::string_view()
                            : declared                                  ? std::string_view("NR mask: the UPLIFT_MASK effect is off")
                                                                        : std::string_view("NR mask: no effect writes UPLIFT_MASK"));
      }
      return *mask_lookup;
    };
    bool present_motion_wanted = false;  // Plan 14: whether the native Vulkan context copies DLSS's vectors this present (decided in the Vulkan block below)
    if (vulkan) {
      // Plan 13 (design §5): NR after DLSS natively on the game's Vulkan device, next to the bridge's Present. The user's decision 1: Source = Auto stays
      // at Present on Vulkan, so the native context is made only once Source is DLSS (the overlay's After DLSS or Before upscaling here), and it takes
      // NR from the bridge (NrClaim) only once the game's DLSS is seen.
      const auto vk_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device->get_native()));
      if (!entry.vk_native_checked) {
        entry.vk_native_checked = true;
        const std::string needs = [&]() -> std::string {
          if (!state.hooks.Started()) return "NGX hooks";
          if (!state.reshade_vulkan_events) return "ReShade 6.8";
          if (const std::string error = vk::DeviceHook::NativeHooksError(); !error.empty()) return std::format("Uplift's Vulkan hooks ({})", error);
          const std::optional<vk::DeviceRecord> record = vk::DeviceHook::Find(vk_device);
          if (!record) return "a device Uplift saw being created";
          if (!record->ngx_ready) return std::format("a device with NGX's extensions (this one lacks {})", record->ngx_missing);
          if (record->instance == VK_NULL_HANDLE) return "the game's Vulkan instance, which Uplift did not see";
          if (!record->storage_extended_supported) return "a driver whose storage images include RG16F (shaderStorageImageExtendedFormats)";
          return {};
        }();
        entry.vk_native_problem = (needs.empty() ? std::string() : std::format("NR after DLSS on Vulkan needs {}", needs));
      }
      // Plan 14 (design §2.2): DLSS's motion vectors for the Present path, copied in the game's frame by the native context, whose Session never loads. Only
      // while NR runs at Present on a GPU-ordered bridge (a CPU-ordered one holds this lock across capped CPU waits, which the hooked evaluate must not wait
      // on) and the game presents on the effect runtime's queue (else the bridge waits a GPU frame inside the present event), the native context can still
      // copy (the game has not shut NGX down), the native placement is not a DLSS one, the motion preference is DLSS (MotionVectors Auto or DLSS), and the
      // game has created DLSS on this device (final review, minor 5).
      present_motion_wanted = addon::PresentMotionWanted({
          .enabled = state.settings.enabled,
          .nr_state = (entry.context ? entry.context->NrState() : nr::SessionState::OFF),
          .bridge_gpu_ordered = (entry.vk_bridge && entry.vk_bridge->GpuOrdered() && !entry.vk_bridge->Stopped()),
          .same_queue = (entry.present_queue == entry.queue),
          .native_wants_nr = (entry.vk_dlss && entry.vk_dlss->WantsNr()),
          .native_can_copy = (!entry.vk_dlss || entry.vk_dlss->CanCopy()),
          .problem = (!entry.vk_native_problem.empty() || !state.dlss_unavailable_reason.empty()),
          .dlss_motion = (ui::MotionPickOf(state.settings) == ui::MotionPick::DLSS),
          .upscaler_created = state.bridge.Registry().UpscalerCreated(device),
      });
      if (!entry.vk_dlss && entry.vk_native_problem.empty() && state.dlss_unavailable_reason.empty()
          && (state.settings.source == ui::PlacementSource::DLSS || present_motion_wanted)) {
        const vk::Loader* const loader = vk::Loader::Get();
        const std::optional<vk::DeviceRecord> record = vk::DeviceHook::Find(vk_device);
        std::string error = "the device's record or the loader is gone";
        LUID luid = {};
        if (loader != nullptr && loader->get_instance_proc_addr != nullptr && record && addon::VulkanAdapterLuid(device, &luid, &error)) {
          Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
          Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
          if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));  // none: the budget reads unlimited headroom (RealHost's rule)
          }
          const std::optional<std::filesystem::path> snippet = locate_snippet();
          entry.snippet_path = snippet.value_or(std::filesystem::path());
          entry.vk_dlss = addon::VkDlssContext::Create(
              {
                  .snippet = {.snippet_path = entry.snippet_path, .application_data_path = state.ngx_data_directory},
                  .binding = {.instance = record->instance,
                              .physical = record->physical,
                              .device = vk_device,
                              .get_instance_proc_addr = loader->get_instance_proc_addr,
                              .get_device_proc_addr = loader->get_device_proc_addr},
                  .adapter = adapter,
                  // Design §3.3: the frame semaphore is signalled through ReShade's queue, in the present event, where ReShade holds its lock.
                  .signal = [](void* signal_queue, VkSemaphore semaphore, uint64_t value) {
                    return signal_queue != nullptr && static_cast<api::command_queue*>(signal_queue)->signal(api::fence{addon::ReshadeHandleOf(semaphore)}, value);
                  },
              },
              &error);
          if (!entry.context) {
            entry.static_block.clear();
            note_static_block(snippet);  // the bridge's context has not decided it yet
          }
        }
        if (entry.vk_dlss) {
          state.bridge.AddVkContexts(1);  // batch 3 review I-2: the NGX hooks' Vulkan paths stop being a passthrough
          entry.vk_dlss->SetDlssUnavailableReason(state.dlss_unavailable_reason);
          nr::Log(nr::LogLevel::INFO, (state.settings.source == ui::PlacementSource::DLSS ? "Vulkan: NR after DLSS available on this device"
                                                                                          : "Vulkan: DLSS's motion vectors can reach the Present path on this device"));
        } else {
          entry.vk_native_problem = std::format("NR after DLSS on Vulkan could not start: {}", error);
          nr::Log(nr::LogLevel::ERR, entry.vk_native_problem);
        }
      }
      if (entry.vk_dlss) {
        addon::VkDlssContext& native = *entry.vk_dlss;
        // Batch 1 review, minor 3: tokens of stale command-buffer states, handed over by init_command_list.
        std::vector<addon::StaleVkList> orphans;
        {
          const std::scoped_lock orphans_lock(state.vk_orphans_mutex);
          for (addon::StaleVkList& orphan : state.vk_orphans) {
            if (orphan.device == device) {
              orphans.push_back(std::move(orphan));
            }
          }
          std::erase_if(state.vk_orphans, [device](const addon::StaleVkList& orphan) { return orphan.device == device; });
        }
        for (const addon::StaleVkList& orphan : orphans) {
          native.Recycle(orphan.tokens);
        }
        native.SetBlockedReason(blocked_reason());
        // NrClaim, with the native context's own identity: it wants NR when its placement is a DLSS one (DLSS seen); the bridge (the device's identity)
        // drains first. Once it neither wants NR nor holds it, it is forgotten, so the bridge can take NR back.
        const bool native_wants = native.WantsNr();
        const bool native_allowed = state.claim.Update(&native, state.settings.enabled && native_wants, native_wants, native.NrState() != nr::SessionState::OFF);
        if (!native_wants && native.NrState() == nr::SessionState::OFF) {
          state.claim.Forget(&native);
        }
        addon::FrameConfig native_config = addon::BuildFrameConfig(
            {
                .settings = &state.settings,
                .drag = state.overlay.drag,
                .defaults_view = state.overlay.defaults_view,
                .ngx_frame_generation = state.bridge.Registry().LiveCount(ngx_hooks::FeatureKind::FRAME_GENERATION),
                .nr_allowed = native_allowed,
                .settings_generation = state.settings_generation,
            },
            &state.coalescer, now);
        native_config.present_motion_copy = present_motion_wanted;
        // Plan 18 Task 12: the game released its Vulkan DLSS here (asked of the registry once the context saw DLSS: fix round 1, M-5).
        native_config.dlss_released = addon::DlssReleased(state.bridge.Registry(), device, ngx_hooks::NgxApi::VULKAN, native.DlssSeen());
        // Key decision g: the effect runtime's queue (none until the runtime is up: the frame is then not signalled).
        native.BeginFrame(entry.queue, native_config, native_allowed, state.claim.Owner() == device, now);
        NoteLatchIfTripped(&state, &native);
        if (const auto [mask_running, mask_idle_note] = mask_state(); !mask_running) {
          // Batch 3 review: the native mask copy goes here too. The bridged context's branch below, which releases it as well, is never reached while that
          // context (or its bridge) could not start, nor when native NR alone runs the device.
          native.ReleaseMask();
          entry.mask_note = mask_idle_note;
        }
        // Plan 13 final review, minor 1: the NGX hooks watch this device's Vulkan DLSS evaluates only while Source is DLSS (the context learns the game's
        // DLSS from them) or the context wants or holds NR; after a pick back to Auto or Present, once NR is released, the evaluate is a passthrough again.
        const bool watching = (state.settings.source == ui::PlacementSource::DLSS || native.WantsNr() || native.NrState() != nr::SessionState::OFF || present_motion_wanted);
        if (watching != entry.vk_nr_counted) {
          state.bridge.NoteVkContextNr(entry.vk_nr_counted, watching);
          nr::Log(nr::LogLevel::INFO, (watching ? "Vulkan: the game's DLSS evaluates are watched for NR on this device"
                                                : "Vulkan: NR is off here and no DLSS placement is chosen: the game's DLSS evaluates are a pure passthrough again"));
        }
        if (native.WantsNr() && !entry.context) {
          // NR runs here natively and the Present path has nothing to do: no bridge is made for it.
          hash_runtime_once();
          if (state.latch_save_pending) {
            state.latch_save_pending = false;
            SaveAndApply(&state);
          }
          state.bridge.SetPreSr(state.settings.enabled && state.settings.pre_upscale && state.settings.source != ui::PlacementSource::PRESENT
                                && state.dlss_unavailable_reason.empty());
          return;
        }
      }
    }
    if (Bridged(entry)) {
      // Plan 17: Vulkan bridges a retry tore down go once the game's queue has passed the fences behind their imports.
      for (auto retiring = entry.retiring_vk.begin(); retiring != entry.retiring_vk.end();) {
        addon::ReshadeVkHost host(device, entry.queue, (*retiring)->VulkanDevice().vkQueueSubmit);
        if (entry.queue != nullptr && (*retiring)->FreeRetired(host)) {
          vk_after_unlock.push_back(std::move(*retiring));
          retiring = entry.retiring_vk.erase(retiring);
        } else {
          ++retiring;
        }
      }
      // Plan 17 (1.0.1 design §3): a stop is counted once for the 2-strike rule. A private device the context found removed latches its bridge too, so the
      // card and the log name it.
      if (entry.context && entry.strikes.Note(PrivateDeviceStopped(entry), PrivateDeviceIndependent(entry))) {
        if (const std::optional<addon::NrHeartbeat::Figures> figures = entry.heartbeat.Stop(now)) {
          LogHeartbeat(entry, device, "NR stopped after", *figures);  // 1.0.1 (F2): how long NR ran, with what, and the memory then
        }
        if (BridgeLatch(entry).empty()) {
          const std::string reason = std::format("The {} bridge stopped: its private Direct3D 12 device was removed",
                                                 (entry.d3d11 ? "Direct3D 11" : entry.d3d10 ? "Direct3D 10" : entry.vulkan ? "Vulkan" : "OpenGL"));
          nr::Log(nr::LogLevel::ERR, reason);
          StopBridge(entry, reason);
        }
        if (entry.strikes.Exhausted()) {
          nr::Logf(nr::LogLevel::WARN, "NR stopped twice this session on this device: it stays off until the game restarts");
        } else if (entry.strikes.Unretryable()) {
          // Test phase (review M-3): the fallback's private device is the adapter's shared one, which the abandoned NR runtime keeps removed: no new device
          // can be made in this process, so there is no Retry now.
          nr::Log(nr::LogLevel::WARN, std::string(ui::PRIVATE_DEVICE_STOPPED_FINAL_REASON));
        } else {
          nr::Logf(nr::LogLevel::INFO, "NR stopped on this device ({} of {} this session): Retry now in Uplift's panel starts it again on a new private "
                   "Direct3D 12 device", entry.strikes.Stops(), addon::BridgeStrikes::LIMIT);
          if (state.settings.diagnostic_remove_device != 0u) {
            entry.diagnostic_retry_in = DIAGNOSTIC_RETRY_PRESENTS;
          }
        }
      }
      if (entry.diagnostic_retry_in != 0u && --entry.diagnostic_retry_in == 0u && entry.strikes.RetryAllowed()) {
        nr::Log(nr::LogLevel::WARN, "Diagnostic (DiagnosticRemoveDevice): pressing Retry now");
        entry.retry_pending = true;
      }
      if (entry.retry_pending) {
        // Plan 17: Retry now. As the game's own device going (Plan 7/11/12): Stop before Teardown, so a wedged private queue drains during Teardown's wait;
        // the private device is only released (Teardown abandons a removed one's NR without an NGX call, spec §13). The game's side of the bridge goes by
        // its API's own rules: GL names now, on the present thread with the runtime's context current (else at a later present); Vulkan imports behind
        // fences (retiring_vk); D3D11 and D3D10 objects with the bridge, after the lock. The next present builds a new bridge and context.
        const bool gl_ready = (!entry.gl_bridge || addon::ReshadeGlHost(entry.gl_bridge->Gl(), entry.queue, entry.present_queue).Valid());
        if (gl_ready) {
          entry.retry_pending = false;
          if (entry.strikes.Retried()) {
            StopBridge(entry, "Retry now");
            if (entry.context) {
              entry.context->Teardown();
              entry.context.reset();
            }
            state.claim.Forget(device);
            if (entry.vk_bridge) {
              addon::ReshadeVkHost host(device, entry.queue, entry.vk_bridge->VulkanDevice().vkQueueSubmit);
              entry.vk_bridge->RetireAll(host);
              entry.retiring_vk.push_back(std::move(entry.vk_bridge));
            }
            if (entry.gl_bridge) {
              addon::ReshadeGlHost host(entry.gl_bridge->Gl(), entry.queue, entry.present_queue);
              entry.gl_bridge->ReleaseGl(host);
              entry.gl_bridge->ForgetGl();  // nothing is left to delete; the destructor makes no GL call
              gl_after_unlock = std::move(entry.gl_bridge);
            }
            state.bridge.NoteD3D11Watch(entry.d3d11_watch_counted, false);  // Plan 18: the next context counts again
            entry.present_motion_logged = false;
            d3d11_after_unlock = std::move(entry.d3d11_bridge);
            d3d10_after_unlock = std::move(entry.d3d10_bridge);
            entry.rejected = false;
            entry.message.clear();
            entry.diagnostic_frames = 0u;
            nr::Log(nr::LogLevel::INFO, "Retry now: the stopped bridge and its NR are gone; a new private Direct3D 12 device starts with the next frame");
          }
          return;  // this frame goes without NR
        }
      }
    }
    if (!entry.context) {
      if (entry.rejected) return;
      entry.static_block.clear();  // Minor 1 (fix round 1): a rebuild must not keep a stale reason
      ID3D12Device* native_device = nullptr;
      if (vulkan) {
        // Vulkan design §4: the private device is made at the first present with NR on, then kept, as on D3D11. The vendor, the LUID and the
        // device's functions (with what the vkCreateDevice hook recorded) decide it.
        if (!state.settings.enabled) return;
        std::string error;
        LUID luid = {};
        if (addon::VulkanAdapterLuid(device, &luid, &error)) {  // else `error` is the card's text: not NVIDIA's, or its adapter is unknown
          std::optional<vk::Device> functions = vk::Device::Open(reinterpret_cast<VkDevice>(device->get_native()));
          if (!functions) {
            error = "the Vulkan loader, or a Vulkan function every device has, is missing";
          } else {
            if (!functions->seen) {
              const std::scoped_lock error_lock(state.vk_hook_error_mutex);
              if (!state.vk_hook_error.empty()) {
                functions->record.not_adjusted = std::format("Uplift's vkCreateDevice hook could not be installed ({})", state.vk_hook_error);
              }
            }
            entry.vk_bridge = bridge::VkBridge::Create(*functions, luid, &release_after_unlock, &error);
          }
        }
        native_device = (entry.vk_bridge ? entry.vk_bridge->Device() : nullptr);
        if (native_device == nullptr) {
          entry.rejected = true;
          entry.message = std::format("Uplift could not start on this Vulkan device: {}", error);
          nr::Log(nr::LogLevel::ERR, entry.message);
          return;
        }
      } else if (opengl) {
        // OpenGL design §4: the private device is made at the first present with NR on, then kept, as on Vulkan. The context is checked FIRST (key decision a,
        // batch 2 review I-1): from the queues and the system wglGetCurrentContext alone, with no GL call. A context of the game's own, a present with no context
        // current, or a runtime that is not up yet gets nothing this frame, is asked again at the next one, and is never rejected for good; only the game's own
        // context gets the card's R63 text (the others say nothing). gl::Functions::Load, which does make GL calls, then runs once, on the runtime's own context, and the bridge keeps its table (every path after it
        // makes the bridge or rejects the device, so Load never runs twice). The device's gaps (no memory-object extension, a foreign GPU) are decided once.
        if (!state.settings.enabled) {
          entry.message.clear();  // only R63's text can be here (a rejected device returned above): NR is off, so there is nothing to say
          return;
        }
        if (const addon::RuntimeContext context = addon::CheckRuntimeContext(entry.queue, entry.present_queue); context != addon::RuntimeContext::CURRENT) {
          entry.message = (context == addon::RuntimeContext::OTHER ? std::string(gl::WRONG_CONTEXT_PROBLEM) : std::string());
          return;
        }
        std::string error;
        std::optional<gl::Functions> functions = gl::Functions::Load(&error);
        LUID luid = {};
        if (functions && addon::GlAdapterLuid(*functions, &luid, &error)) {  // else `error` is the card's text: not NVIDIA's, or its adapter is unknown
          entry.gl_bridge = bridge::GlBridge::Create(std::move(*functions), luid, &release_after_unlock, &error);
        }
        native_device = (entry.gl_bridge ? entry.gl_bridge->Device() : nullptr);
        if (native_device == nullptr) {
          entry.rejected = true;
          entry.message = std::format("Uplift could not start on this OpenGL context: {}", error);
          nr::Log(nr::LogLevel::ERR, entry.message);
          return;
        }
      } else if (device_api == api::device_api::d3d11 || device_api == api::device_api::d3d10) {
        // D3D11 design, decision 2 (D3D10 too): the private devices are made at the first present with NR on, then
        // kept. A game that never enables NR pays nothing.
        entry.d3d11 = (device_api == api::device_api::d3d11);
        entry.d3d10 = (device_api == api::device_api::d3d10);
        if (!state.settings.enabled) return;
        std::string error;
        if (entry.d3d10) {
          entry.d3d10_bridge = bridge::D3D10Bridge::Create(reinterpret_cast<ID3D10Device*>(device->get_native()),
                                                           &relay_after_unlock, &release_after_unlock, &error);
          native_device = (entry.d3d10_bridge ? entry.d3d10_bridge->Device() : nullptr);
        } else {
          entry.d3d11_bridge = bridge::D3D11Bridge::Create(reinterpret_cast<ID3D11Device*>(device->get_native()),
                                                           &release_after_unlock, &error);
          native_device = (entry.d3d11_bridge ? entry.d3d11_bridge->Device() : nullptr);
        }
        if (native_device == nullptr) {
          entry.rejected = true;
          entry.message = std::format("Uplift could not start on this Direct3D {} device: {}", (entry.d3d10 ? 10 : 11), error);
          nr::Log(nr::LogLevel::ERR, entry.message);
          return;
        }
      } else {
        native_device = reinterpret_cast<ID3D12Device*>(device->get_native());
        if (!addon::IsNvidiaDevice(native_device)) {
          entry.rejected = true;
          entry.message = "Uplift needs an NVIDIA GPU; this game renders on another adapter";
          nr::Log(nr::LogLevel::WARN, entry.message);
          return;
        }
      }
      const std::optional<std::filesystem::path> snippet = locate_snippet();
      entry.snippet_path = snippet.value_or(std::filesystem::path());
      std::string error;
      entry.context = addon::DeviceContext::Create(
          native_device, {.snippet_path = entry.snippet_path, .application_data_path = state.ngx_data_directory}, &error,
          {.bridged = Bridged(entry), .dlss_stages = entry.d3d11});  // Plan 18: a Direct3D 11 bridge's context runs the DLSS stages
      if (!entry.context) {
        entry.rejected = true;
        // Safe under the lock: release_after_unlock and relay_after_unlock still hold the proxies.
        entry.d3d11_bridge.reset();
        entry.d3d10_bridge.reset();
        entry.vk_bridge.reset();
        if (entry.gl_bridge) {
          // No ForgetGl or retire ever reaches a bridge that never ran. The context is still the runtime's (checked a few lines up, in this same present), so the
          // two imported semaphores go now instead of living until the share group dies.
          entry.gl_bridge->ReleaseSemaphores();
        }
        entry.gl_bridge.reset();
        entry.message = std::format("Uplift could not start on this device: {}", error);
        nr::Log(nr::LogLevel::ERR, entry.message);
        return;
      }
      entry.message.clear();
      entry.context->SetDlssUnavailableReason(state.dlss_unavailable_reason);
      note_static_block(snippet);
      if (entry.d3d11) {
        // Plan 18 (design §2): the mark the hooked Direct3D 11 calls find this device by, whichever pointer the game hands NGX (ReShade's proxy forwards it).
        addon::MarkD3D11Device(reinterpret_cast<ID3D11Device*>(device->get_native()), device);
      }
      if (!Bridged(entry)) {
        // Plan 15: the mark the game's NGX shutdown finds this device by (BeforeCoreShutdownD3D12), whichever pointer the game hands NGX.
        api::device* const owner = device;
        native_device->SetPrivateData(addon::UPLIFT_RESHADE_DEVICE_GUID, sizeof(owner), &owner);
        // Fix round, minor 3: a context made after the game shut NGX down here (its last one ended with its present queue) starts held, or abandoned.
        entry.context->SeedCoreHold(entry.core_hold);
      }
    }

    const ui::Settings& settings = state.settings;
    hash_runtime_once();
    entry.context->SetBlockedReason(blocked_reason());
    if (entry.d3d11) {
      // Plan 18 (design §5, §6): a foreign Direct3D 12 DLSS device can appear at any time; the game's own device removed decides the latch window; the hooks
      // watch this device's evaluates while NR is on or not yet released (Plan 13 I-2's rule).
      if (!entry.foreign_dlss && state.bridge.ForeignD3D12Dlss() && !entry.context->DlssSeen()) {
        entry.foreign_dlss = true;
        nr::Logf(nr::LogLevel::INFO, "Direct3D 11: {}", ui::FOREIGN_D3D12_DLSS_REASON);
      }
      entry.context->SetDlssUnavailableReason(std::string(D3D11DlssReason(state, entry)));
      if (const HRESULT removed = reinterpret_cast<ID3D11Device*>(device->get_native())->GetDeviceRemovedReason(); FAILED(removed)) {
        entry.context->NoteGameDeviceRemoved(removed, now);
        NoteLatchIfTripped(&state, entry.context.get());  // fix round 1 (M-2): now, as the evaluate path does, with the removal's own text
      }
      state.bridge.NoteD3D11Watch(entry.d3d11_watch_counted, state.settings.enabled || entry.context->NrState() != nr::SessionState::OFF);
    }
    const addon::ContextStatus before = entry.context->Status();
    // 1.0.1 (F2): a line a minute while NR runs, from the recordings that applied NR (any placement, any thread) and the latest one's motion source. A
    // native Direct3D 12 device's removal is reported right after BeginFrame below, which is where it is found; a bridged device's stop where its strike
    // is counted, above.
    if (const std::optional<addon::NrHeartbeat::Figures> figures =
            entry.heartbeat.Note(entry.context->NrRecordings(), entry.context->LatestMotionSource(), now)) {
      LogHeartbeat(entry, device, "NR running:", *figures);
    }
    // Plan 17: DiagnosticRemoveDevice (hidden, diagnostic only): Uplift's own private device is removed once NR has run that many frames on it (never the
    // game's: Bridged only), so Retry now and the 2-strike rule can be tested without a real hang.
    if (state.settings.diagnostic_remove_device != 0u && Bridged(entry) && before.nr_applied && !entry.strikes.Stopped()
        && !(entry.d3d11 && addon::IsDlssStage(entry.context->CurrentPlacement()))  // Plan 18: DiagnosticRemoveMidFrame's
        && ++entry.diagnostic_frames == state.settings.diagnostic_remove_device) {
      Microsoft::WRL::ComPtr<ID3D12Device5> removable;
      if (ID3D12Device* const private_device = PrivateDevice(entry);
          private_device != nullptr && SUCCEEDED(private_device->QueryInterface(IID_PPV_ARGS(&removable)))) {
        nr::Logf(nr::LogLevel::WARN, "Diagnostic (DiagnosticRemoveDevice = {}): removing Uplift's own private Direct3D 12 device",
                 state.settings.diagnostic_remove_device);
        removable->RemoveDevice();
      }
    }
    const bool nr_allowed =
        state.claim.Update(device, settings.enabled, before.dlss_seen, before.session.state != nr::SessionState::OFF);
    // Plan 5 (key decision 8): without a running UPLIFT_MASK there is no mask to bind; the copy is released (and the native context's too, in the Vulkan block
    // above, which a device whose bridge never started reaches alone).
    const auto [mask_running, mask_idle_note] = mask_state();
    if (!mask_running) {
      entry.context->PrepareMaskCopy(nullptr);
      ReleaseBridgeMask(entry, device);
      if (entry.vk_dlss) {
        entry.vk_dlss->ReleaseMask();  // Plan 14: the native context's copy on the game's device
      }
      entry.mask_note = mask_idle_note;
    }
    // Plan 9: BuildFrameConfig is shared with the 32-bit helper; Plan 6's Defaults view is applied inside it.
    addon::FrameConfig frame_config = addon::BuildFrameConfig(
        {
            .settings = &settings,
            .drag = state.overlay.drag,
            .defaults_view = state.overlay.defaults_view,
            .ngx_frame_generation = state.bridge.Registry().LiveCount(ngx_hooks::FeatureKind::FRAME_GENERATION),
            .nr_allowed = nr_allowed,
            .settings_generation = state.settings_generation,
        },
        &state.coalescer, now);
    if (entry.vk_bridge && frame_config.source == ui::PlacementSource::DLSS) {
      // Plan 13: on Vulkan, Source = DLSS is the explicit native choice, which runs on Present (the bridge's) until the game's DLSS is seen, and NrClaim
      // hands NR to the native context then. The bridged context itself knows Auto (Present) only.
      frame_config.source = ui::PlacementSource::AUTO;
    }
    // Plan 14 (batch 2 review, minor 2): the bridged context's Details motion line says what is true on Vulkan, where DLSS's vectors come as copies.
    frame_config.vulkan = entry.vulkan;
    frame_config.present_motion_copy = present_motion_wanted;
    if (entry.d3d11) {
      // Plan 18 (design §4): Direct3D 11's ring, as Direct3D 12's amendment 7: Source = Present.
      frame_config.present_motion_copy = (settings.source == ui::PlacementSource::PRESENT && frame_config.motion_vectors);
    }
    // Plan 15: a Direct3D 12 context held after the game's NGX shutdown resumes once this rises (the game created its DLSS again); bridged ones never hold.
    frame_config.upscaler_creates = state.bridge.Registry().UpscalerCreates(device);
    // Plan 18 Task 12: the game released its DLSS on this device (or on Direct3D 11 shut NGX down): once that holds, its DLSS stages fall back to Present.
    // Fix round 1 (M-5): the registry is asked once the context saw DLSS.
    frame_config.dlss_released = addon::DlssReleased(state.bridge.Registry(), device, DlssApiOf(entry), entry.context->DlssSeen());
    addon::TargetInfo target = {
        .resource = reinterpret_cast<ID3D12Resource*>(entry.back_buffer.handle),
        .color_space = static_cast<color::ColorSpace>(swapchain->get_color_space()),
    };
    ID3D12CommandQueue* frame_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    // Owns a copy of the bridge's problem text: BridgeFrame::problem only views the bridge's own string until its
    // next call (RunBridged below calls into it again), so `target.problem` must not keep pointing into it.
    std::string bridge_problem;
    if (Bridged(entry)) {
      // Plan 7: the back buffer reaches NR through the bridge's shared copy, on the private device's queue. Final
      // review Minor 2: the copy exists only while NR's state is not OFF; until then the Session loads from the
      // described size and format, and NR runs from the next frame.
      const bool running = (entry.context->NrState() != nr::SessionState::OFF);
      bridge::BridgeFrame frame;
      if (entry.vk_bridge) {
        // Plan 11: the retire fences of a bridge that just went off are submitted here, on the effect runtime's queue.
        addon::ReshadeVkHost host(device, entry.queue, entry.vk_bridge->VulkanDevice().vkQueueSubmit);
        frame = entry.vk_bridge->BeginFrame(host, VulkanImageInfo(device, entry.back_buffer), settings.enabled, running, now);
        frame_queue = entry.vk_bridge->Queue();
      } else if (entry.gl_bridge) {
        // Plan 12: FB0 at the present event is what NR runs on unless the Uplift technique is in the preset, where it is ReShade's single-sampled intermediate
        // (and MSAA is no problem: `at_present` decides). The GL names that waited for a valid host are deleted here, with the runtime's context current.
        addon::ReshadeGlHost host(entry.gl_bridge->Gl(), entry.queue, entry.present_queue);
        frame = entry.gl_bridge->BeginFrame(host, addon::GlImageInfo(device, entry.back_buffer), entry.marker.handle == 0u, settings.enabled, running, now);
        frame_queue = entry.gl_bridge->Queue();
      } else if (entry.d3d10_bridge) {
        frame = entry.d3d10_bridge->BeginFrame(reinterpret_cast<ID3D10Resource*>(entry.back_buffer.handle), settings.enabled, running, now);
        frame_queue = entry.d3d10_bridge->Queue();
      } else {
        // Plan 18: at a DLSS stage NR runs inside the game's frame, so the back buffer is only described.
        frame = entry.d3d11_bridge->BeginFrame(reinterpret_cast<ID3D11Resource*>(entry.back_buffer.handle), settings.enabled, running, now,
                                               /*present_copy=*/!addon::IsDlssStage(entry.context->CurrentPlacement()));
        frame_queue = entry.d3d11_bridge->Queue();
      }
      target.resource = frame.color;
      bridge_problem = std::string(frame.problem);
      target.problem = bridge_problem;
      target.size = frame.size;
      target.format = frame.format;
    }
    const addon::TriggerPoint point =
        entry.context->BeginFrame(frame_queue, frame_config, target, entry.marker.handle != 0u, now);
    if (entry.d3d11_bridge) {
      // Plan 18: the shares of what no longer runs go now: the DLSS stages' when NR runs at Present, the ring when its copies are not wanted (NR off
      // included, so "copied for Present" is logged once per activation).
      const addon::Placement placement = entry.context->CurrentPlacement();
      if (!addon::IsDlssStage(placement)) {
        entry.d3d11_bridge->ReleaseDlss();
      }
      if (!frame_config.present_motion_copy || placement != addon::Placement::PRESENT || entry.context->NrState() == nr::SessionState::OFF) {
        entry.d3d11_bridge->ReleasePresentMotion();
        entry.present_motion_logged = false;
      }
    }
    if (!Bridged(entry) && entry.context->DeviceLost()) {
      // 1.0.1 (F2): the game's own Direct3D 12 device, found removed by this very BeginFrame (or earlier): the heartbeat's figures, once.
      if (const std::optional<addon::NrHeartbeat::Figures> figures = entry.heartbeat.Stop(now)) {
        LogHeartbeat(entry, device, "NR stopped after", *figures);
      }
    }
    if (Bridged(entry)) {
      RunBridged(entry, device, point, 0u, vk::Usage::PRESENT, entry.back_buffer);
    } else {
      ReshadeFrameHost host(queue, entry.back_buffer);
      RunOrWarnOnce(&state, entry.context.get(), &host, D3D12_RESOURCE_STATE_PRESENT, point);
    }

    // v2 design §3.2's safety latch, persisted so the next start keeps the DLSS placements off, and
    // held for the rest of this session on every device (Important 1/2, fix round 1).
    NoteLatchIfTripped(&state, entry.context.get());
    NoteLatchIfTripped(&state, entry.vk_dlss.get());  // Plan 13: a no-op without a native Vulkan context
    // Minor (fix round 2): the ini mirror runs here, on the present thread, whether this present's own
    // call just set the flag or an earlier evaluate or teardown call (on another thread) did.
    if (state.latch_save_pending) {
      state.latch_save_pending = false;
      SaveAndApply(&state);
    }
    // Key decisions: tracking runs only while a DLSS placement can use it, so a game without DLSS (or before
    // its first evaluate) pays one relaxed load per tracking event. The first list reset after DLSS appears
    // is tracked; an evaluate on a list reset earlier skips once with "list state unknown". Important 2:
    // also gated on the latch (or any other reason DLSS placements stopped being available mid-session).
    // Key decision 6: tracking runs whatever Source says now, so a live Source switch applies at once; a
    // Source = Present game still needs it once MotionVectors wants DLSS's motion vectors there too.
    // Plan 18: never for a bridged device: a bridge never records on a game's Direct3D 12 list, and a Direct3D 11 DLSS game must not turn state tracking
    // on for a mod's device.
    addon::SetTracking(state.tracking_registered && state.settings.enabled && before.dlss_seen && !Bridged(entry)
                       && state.dlss_unavailable_reason.empty()
                       && (state.settings.source != ui::PlacementSource::PRESENT
                           || state.settings.motion_vectors == ui::MotionVectorSource::AUTO
                           || state.settings.motion_vectors == ui::MotionVectorSource::DLSS));
    state.bridge.SetPreSr(state.settings.enabled && state.settings.pre_upscale
                          && state.settings.source != ui::PlacementSource::PRESENT
                          && state.dlss_unavailable_reason.empty());
  }
  UPLIFT_CATCH("present", )
}

void OnRenderTechnique(api::effect_runtime* runtime, api::effect_technique technique, api::command_list* cmd_list,
                       api::resource_view rtv, api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    if (entry == nullptr) return;
    const bool is_marker = (technique.handle == entry->marker.handle);
    if (entry->helper.HasClient()) {
      g_state->front->Technique(entry->helper, runtime, is_marker, rtv.handle, g_state->settings);  // Plan 10: a Direct3D 9 device
      return;
    }
    // Plan 6 (v2 design §3.20): at the Uplift technique, the UPLIFT_MV its pass just wrote, while LaunchPad runs.
    // Plan 7: the same lookup, API-neutral, so the bridged branch below (D3D11, and D3D10 in Plan 8) can use it too.
    api::resource launchpad = {0u};
    if (is_marker) {
      const api::effect_technique launchpad_technique = runtime->find_technique(nullptr, LAUNCHPAD_TECHNIQUE);
      const api::effect_texture_variable motion = runtime->find_texture_variable(nullptr, MOTION_TEXTURE);
      if (launchpad_technique.handle != 0u && runtime->get_technique_state(launchpad_technique) && motion.handle != 0u) {
        api::resource_view view = {0u};
        api::resource_view view_srgb = {0u};
        runtime->get_texture_binding(motion, &view, &view_srgb);
        if (view.handle != 0u) {
          launchpad = runtime->get_device()->get_resource_from_view(view);
        }
      }
    }
    if (Bridged(*entry)) {
      // Plan 7 (decision 3): UPLIFT_MV crosses only when MotionVectors can use it (a 32 MiB copy at 4K otherwise).
      const ui::MotionVectorSource source = g_state->settings.motion_vectors;
      const bool launchpad_wanted =
          (source == ui::MotionVectorSource::AUTO || source == ui::MotionVectorSource::LAUNCHPAD);
      // Plan 12 (design §3.2): an OpenGL device's frame here is ReShade's intermediate, the event's rtv, not FB0 (which still holds the game's pre-effects frame).
      RunBridged(*entry, runtime->get_device(), entry->context->OnTechnique(is_marker), (launchpad_wanted ? launchpad.handle : 0u),
                 vk::Usage::RENDER_TARGET, (entry->opengl ? runtime->get_device()->get_resource_from_view(rtv) : api::resource{0u}));
      return;
    }
    ReshadeFrameHost host(entry->queue, entry->back_buffer);
    RunOrWarnOnce(g_state, entry->context.get(), &host, D3D12_RESOURCE_STATE_RENDER_TARGET, entry->context->OnTechnique(is_marker),
                  reinterpret_cast<ID3D12Resource*>(launchpad.handle));
  }
  UPLIFT_CATCH("render_technique", )
}

void OnFinishEffects(api::effect_runtime* runtime, api::command_list* cmd_list, api::resource_view rtv,
                     api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    // Plan 14 (design §3.2): a Vulkan device whose native context runs NR at a DLSS placement takes UPLIFT_MASK on the game's own device. FindFrameEntry keeps
    // requiring the bridged context (the technique and RunBridged use it), so the entry is looked up here, for this copy alone.
    DeviceEntry* native = nullptr;
    if (const auto found = g_state->devices.find(runtime->get_device()); found != g_state->devices.end()) {
      DeviceEntry& candidate = found->second;
      const bool native_view = (candidate.vk_dlss && (candidate.vk_dlss->WantsNr() || candidate.vk_dlss->NrState() != nr::SessionState::OFF));
      if (native_view && candidate.runtime == runtime && candidate.queue != nullptr && cmd_list == candidate.queue->get_immediate_command_list()) {
        native = &candidate;
      }
    }
    if (entry == nullptr && native == nullptr) return;
    if (entry != nullptr && entry->helper.HasClient()) {
      g_state->front->FinishEffects(entry->helper, runtime, rtv.handle, g_state->settings);  // Plan 10: a Direct3D 9 device
      return;
    }
    if (entry != nullptr && Bridged(*entry)) {
      RunBridged(*entry, runtime->get_device(), entry->context->OnFinishEffects(), 0u, vk::Usage::RENDER_TARGET,
                 (entry->opengl ? runtime->get_device()->get_resource_from_view(rtv) : api::resource{0u}));
    } else if (entry != nullptr) {
      ReshadeFrameHost host(entry->queue, entry->back_buffer);
      RunOrWarnOnce(g_state, entry->context.get(), &host, D3D12_RESOURCE_STATE_RENDER_TARGET, entry->context->OnFinishEffects());
    }
    // Plan 5 (key decision 8): this frame's UPLIFT_MASK, copied now that the effects have run, on this same list,
    // into Uplift's own texture; every placement binds that copy from the next recording on.
    if (g_state->settings.mask != ui::MaskMode::AUTO) return;
    const api::effect_texture_variable variable = runtime->find_texture_variable(nullptr, MASK_TEXTURE);
    if (variable.handle == 0u) return;
    DeviceEntry& masked = (native != nullptr ? *native : *entry);
    // Final review I-1: the variable can still exist, with its last content, after every technique of its
    // effect has been disabled; only a still-enabled technique means the effect keeps writing it.
    if (!MaskEffectRunning(runtime, variable)) {
      if (masked.context) {
        masked.context->PrepareMaskCopy(nullptr);
      }
      if (masked.vk_dlss) {
        masked.vk_dlss->ReleaseMask();
      }
      ReleaseBridgeMask(masked, runtime->get_device());
      masked.mask_note = "NR mask: the UPLIFT_MASK effect is off";
      return;
    }
    api::resource_view view = {0u};
    api::resource_view view_srgb = {0u};
    runtime->get_texture_binding(variable, &view, &view_srgb);
    if (view.handle == 0u) return;
    const api::resource mask = runtime->get_device()->get_resource_from_view(view);
    if (mask.handle == 0u) return;
    if (native != nullptr) {
      // Plan 14: into the native context's image on the game's device, through ReShade's command list (as Direct3D 12's copy below). ReShade keeps effect
      // textures in shader_resource between techniques; the copy rests there between copies, and is UNDEFINED before its first (R95).
      const api::resource_desc description = runtime->get_device()->get_resource_desc(mask);
      const std::optional<color::MaskFormatInfo> format = color::DescribeMaskFormat(static_cast<DXGI_FORMAT>(description.texture.format));
      const bool readable = (format.has_value() && description.type == api::resource_type::texture_2d && description.texture.samples == 1u);
      bool first = false;
      const VkImage copy = native->vk_dlss->PrepareMaskCopy((readable ? vk::VkFormatOf(format->view_format) : VK_FORMAT_UNDEFINED),
                                                            {description.texture.width, description.texture.height}, &first);
      if (copy == VK_NULL_HANDLE) {
        native->mask_note = (readable ? "NR mask: waiting for NR to run" : "NR mask: UPLIFT_MASK's format is not supported");
        return;
      }
      const api::resource target = {addon::ReshadeHandleOf(copy)};
      cmd_list->barrier(mask, api::resource_usage::shader_resource, api::resource_usage::copy_source);
      cmd_list->barrier(target, (first ? api::resource_usage::undefined : api::resource_usage::shader_resource), api::resource_usage::copy_dest);
      cmd_list->copy_texture_region(mask, 0u, nullptr, target, 0u, nullptr);
      cmd_list->barrier(target, api::resource_usage::copy_dest, api::resource_usage::shader_resource);
      cmd_list->barrier(mask, api::resource_usage::copy_source, api::resource_usage::shader_resource);
      native->vk_dlss->NoteMaskCopied();
      native->mask_note = std::format("NR mask: UPLIFT_MASK ({}x{})", description.texture.width, description.texture.height);
      return;
    }
    if (Bridged(*entry)) {
      // Plan 7 (key decision b): UPLIFT_MASK crosses in the bridge's shared mask; the next bridged recording copies it
      // into NR's own. Only while NR runs, as PrepareMaskCopy gates the D3D12 copy.
      if (entry->context->NrState() != nr::SessionState::ACTIVE) {
        entry->mask_note = "NR mask: waiting for NR to run";
        return;
      }
      bridge::MaskCopy copied;
      if (entry->vk_bridge) {
        addon::ReshadeVkHost host(runtime->get_device(), entry->queue, entry->vk_bridge->VulkanDevice().vkQueueSubmit);
        copied = entry->vk_bridge->CopyMask(host, VulkanImageInfo(runtime->get_device(), mask));
      } else if (entry->gl_bridge) {
        addon::ReshadeGlHost host(entry->gl_bridge->Gl(), entry->queue);
        copied = entry->gl_bridge->CopyMask(host, addon::GlImageInfo(runtime->get_device(), mask));
      } else if (entry->d3d10_bridge) {
        copied = entry->d3d10_bridge->CopyMask(reinterpret_cast<ID3D10Resource*>(mask.handle));
      } else {
        copied = entry->d3d11_bridge->CopyMask(reinterpret_cast<ID3D11Resource*>(mask.handle));
      }
      // Final review Minor 4: "not supported" only for a format NR cannot read; any other failure is the share's.
      if (copied.size) {
        entry->mask_note = std::format("NR mask: UPLIFT_MASK ({}x{})", copied.size->width, copied.size->height);
      } else if (copied.unsupported) {
        entry->mask_note = "NR mask: UPLIFT_MASK's format is not supported";
      } else if (copied.relay_failed) {
        entry->mask_note = "NR mask: UPLIFT_MASK could not be shared with Uplift's relay device";  // Plan 8: D3D10's keyed share
      } else {
        entry->mask_note = "NR mask: UPLIFT_MASK could not be shared with the private Direct3D 12 device";
      }
      return;
    }
    const D3D12_RESOURCE_DESC description = reinterpret_cast<ID3D12Resource*>(mask.handle)->GetDesc();
    ID3D12Resource* const copy = entry->context->PrepareMaskCopy(&description);
    if (copy == nullptr) {
      // B2 (folded into I-1): split the two causes PrepareMaskCopy's nullptr can mean. The format check
      // mirrors NrPipeline::PrepareMaskCopy's own gate (src/sources/nr_pipeline.cpp).
      const bool format_supported = color::DescribeMaskFormat(description.Format).has_value()
                                    && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
                                    && description.SampleDesc.Count == 1u;
      entry->mask_note =
          (format_supported ? "NR mask: waiting for NR to run" : "NR mask: UPLIFT_MASK's format is not supported");
      return;
    }
    const api::resource target = {reinterpret_cast<uint64_t>(copy)};
    cmd_list->barrier(mask, api::resource_usage::shader_resource, api::resource_usage::copy_source);
    cmd_list->barrier(target, api::resource_usage::shader_resource, api::resource_usage::copy_dest);
    cmd_list->copy_texture_region(mask, 0u, nullptr, target, 0u, nullptr);
    cmd_list->barrier(target, api::resource_usage::copy_dest, api::resource_usage::shader_resource);
    cmd_list->barrier(mask, api::resource_usage::copy_source, api::resource_usage::shader_resource);
    entry->context->NoteMaskCopied();
    entry->mask_note = std::format("NR mask: UPLIFT_MASK ({}x{})", description.Width, description.Height);
    // ReShade keeps effect textures in shader_resource between techniques (its runtime.cpp, read-only), so the
    // barriers above name its real state.
  }
  UPLIFT_CATCH("finish_effects", )
}

void OnReshadePresent(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    AddonState& state = *g_state;
    ui::ExpireKeyCapture(&state.overlay, std::chrono::steady_clock::now());
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

// Plan 6 (v2 design §3.18, D1): `entry`'s status card, from its Status() facts, for the overlay.
// `status` is null when the device has no context. Called with the add-on's lock held.
ui::StatusCard CardFor(const AddonState& state, const DeviceEntry& entry, const addon::ContextStatus* status,
                       std::string_view working_line) {
  if (status == nullptr) {
    // Plan 7: a D3D11 device makes its private D3D12 device only at the first present with NR on (Plan 8: D3D10 too). Plan 12: an OpenGL context that presents
    // from another context than the runtime's has no bridge yet and says why (R63).
    if (Bridged(entry) && !entry.rejected && entry.message.empty()) return ui::BuildStatusCard({.enabled = state.settings.enabled});
    return ui::BuildStatusCard({
        .device_problem = (entry.message.empty() ? std::string_view("Waiting for the game's first frame")
                                                 : std::string_view(entry.message)),
    });
  }
  // Plan 17: a stopped private-device bridge is its own card (Retry now, or the second time the restart); its latch text names what stopped.
  const bool private_stopped = (Bridged(entry) && (entry.strikes.Stopped() || PrivateDeviceStopped(entry)));  // the present counts it next
  std::string_view private_reason = BridgeLatch(entry);
  if (private_reason.empty()) {
    private_reason = status->output_problem;
  }
  return ui::BuildStatusCard({
      .private_stopped = (private_stopped ? private_reason : std::string_view()),
      .private_stops_final = (Bridged(entry) && entry.strikes.Exhausted()),
      .private_stop_unretryable = (Bridged(entry) && entry.strikes.Unretryable()),
      .device_lost = status->device_lost,
      .stopped = (status->abandoned ? ui::D3D12_NGX_ABANDONED_REASON : std::string_view()),  // Plan 15 fix round (minor 6)
      .blocked = status->blocked,
      .blocked_stage = (entry.static_block.empty() ? ui::CardStage::CONFLICTS : entry.block_stage),
      .claimed_elsewhere = status->claimed_elsewhere,
      .enabled = state.settings.enabled,
      .held = status->held,  // Plan 15
      .session = status->session,
      .output_problem = status->output_problem,
      .placement_note = status->placement_note,
      .dlss_latched = state.settings.dlss_placement_blocked,
      .nr_applied = status->nr_applied,
      .passes_run = status->passes_run,
      .skip_reason = status->skip_reason,
      .skip_from_session = status->skip_from_session,
      .working_line = working_line,
  });
}

void OnDrawOverlay(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    AddonState& state = *g_state;
    const auto now = std::chrono::steady_clock::now();
    ui::OverlayView view = {.status_line = "OFF", .last_key_pressed = runtime->last_key_pressed(),
                            .placement_line = "Placement: NR is off", .motion_line = "Motion vectors: NR is off"};
    const auto found = state.devices.find(runtime->get_device());
    const api::device_api overlay_device_api = runtime->get_device()->get_api();
    // Plan 13 (the user's decision 1): on Vulkan, Before upscaling and After DLSS are explicit choices, offered where native NR can run.
    view.dlss_explicit = (overlay_device_api == api::device_api::vulkan);
    // Plan 14 (design §1): Setup's facts. The stored preference here; what is possible and what runs below, per path; the draw resolves them.
    ui::SetupFacts facts;
    std::string after_dlss_problem;  // Plan 18: Setup's facts view these; they outlive FinishSetup
    std::string before_upscaling_problem;
    addon::FillPreference(&facts, state.settings, view.dlss_explicit);
    if (overlay_device_api != api::device_api::d3d9) {
      view.d3d9ex_unavailable = "Only for Direct3D 9 games";  // Plan 10: the 9Ex toggle works in 64-bit Direct3D 9 games too
    }
    if (overlay_device_api != api::device_api::d3d12 && overlay_device_api != api::device_api::d3d11
        && overlay_device_api != api::device_api::d3d10 && overlay_device_api != api::device_api::d3d9
        && overlay_device_api != api::device_api::vulkan && overlay_device_api != api::device_api::opengl) {
      view.card = ui::BuildStatusCard({.device_problem = "Uplift supports Direct3D 9, 10, 11 and 12, Vulkan and OpenGL games only"});
      view.dlss_unavailable = state.dlss_unavailable_reason;
    } else if (overlay_device_api == api::device_api::d3d9) {
      // Plan 10: the helper front's lines and card; no DLSS placement without a Direct3D 12 game.
      view.dlss_unavailable = std::string(addon::BRIDGED_DLSS_REASON);
      state.front->Overlay(runtime->get_device(), (found != state.devices.end() ? &found->second.helper : nullptr), overlay_device_api,
                           state.settings, &view, &state.overlay, now);  // finishes Setup too
    } else if (found == state.devices.end()) {
      view.card = ui::BuildStatusCard({.device_problem = "Waiting for the game's first frame"});
      view.dlss_unavailable = state.dlss_unavailable_reason;
    } else {
      [[maybe_unused]] const auto& [device, entry] = *found;
      // ui-review.md §4.1: known before a context exists, so a D3D11 game greys the DLSS toggles from
      // its first frame.
      // Plan 18 (design §5): a Direct3D 11 device runs the DLSS stages through its bridge; its own reasons (a foreign Direct3D 12 DLSS device) replace the
      // bridged one.
      view.dlss_unavailable = (entry.d3d11   ? std::string(D3D11DlssReason(state, entry))
                               : Bridged(entry) ? std::string(addon::BRIDGED_DLSS_REASON)
                                                : state.dlss_unavailable_reason);
      // Final review I-1: the native Vulkan context stopped for good (the game shut NGX down on the device, or it was lost): nothing inside the game's frame
      // runs again this session, so the DLSS stages and DLSS's vectors are a fixed cause.
      const bool native_stopped = (entry.vk_dlss && !entry.vk_dlss->CanCopy());
      if (entry.vulkan) {
        view.dlss_unavailable = (!state.dlss_unavailable_reason.empty() ? state.dlss_unavailable_reason
                                 : !entry.vk_native_problem.empty()     ? entry.vk_native_problem
                                 : native_stopped                       ? std::string(ui::VULKAN_NGX_SHUT_DOWN_REASON)
                                                                        : std::string());
      }
      // Plan 13: while the native Vulkan context wants or holds NR, the panel shows it; the bridge's Present otherwise.
      const bool native_view = (entry.vk_dlss && (entry.vk_dlss->WantsNr() || entry.vk_dlss->NrState() != nr::SessionState::OFF));
      // Final review I-1: stopped at a DLSS stage, the native context keeps NR's claim (Plan 13's exit protection: the Present path never loads NR into a game
      // that may be quitting), so Present cannot run either; what is highlighted is the stage it stopped at, where the card says NR stays off.
      if (native_view && native_stopped) {
        facts.present_fixed = ui::VULKAN_NGX_SHUT_DOWN_PRESENT_REASON;
      }
      // Plan 14 (design §1.2): what is possible here. The game created a DLSS feature on this device (sticky; the NGX hooks feed it in every mode), and
      // whether the main one is Ray Reconstruction.
      // Batch 1 review I-1: on Direct3D 12 only a real evaluate counts (added below from the context's status); a create counts on Vulkan, where the passthrough
      // context sees no evaluate until a DLSS pick makes it watch.
      facts.dlss_seen = addon::SetupDlssSeen(entry.vulkan, state.bridge.Registry().UpscalerCreated(device), (entry.vk_dlss && entry.vk_dlss->DlssSeen()));
      if (const NVSDK_NGX_Handle* const main_handle = state.bridge.Registry().MainHandle(device, entry.swapchain_size, DlssApiOf(entry))) {
        const std::optional<ngx_hooks::FeatureRecord> main_record = state.bridge.Registry().Find(main_handle);
        facts.ray_reconstruction = main_record && main_record->feature == NVSDK_NGX_Feature_RayReconstruction;
      }
      facts.launchpad_ready = entry.launchpad_ready;
      facts.match_game_readable = (!Bridged(entry) || entry.d3d11 || native_view);  // the bridges never see the game's DLSS create (Plan 18: Direct3D 11's context reads it)
      // Final review, minor 3: at Present on Vulkan a DLSS stage reads it when it can run here, so Match game waits for one (temporary).
      facts.match_game_at_dlss_stages = (entry.vulkan && !native_view && view.dlss_unavailable.empty());
      if (entry.vulkan && !native_view) {
        // At Present on Vulkan DLSS's vectors reach NR through the copies the native context makes in the game's frame (design §2.2): on a GPU-ordered bridge only,
        // a game that presents on the effect runtime's queue, and while the native context can still copy. Batch 2 review I-2: the bridge's order is known from the
        // device's record before the bridge exists (VkBridge::Create's rule); only a refused fence import is unknowable until it does.
        const std::optional<vk::DeviceRecord> record =
            (entry.vk_bridge ? std::nullopt : vk::DeviceHook::Find(reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device->get_native()))));
        const bool cpu_ordered = (entry.vk_bridge ? !entry.vk_bridge->GpuOrdered() : (record && !(record->semaphore_win32 && record->timeline && record->memory_win32)));
        if (cpu_ordered) {
          facts.dlss_motion_fixed = ui::VULKAN_CPU_ORDERED_MOTION;
        } else if (entry.present_queue != nullptr && entry.queue != nullptr && entry.present_queue != entry.queue) {
          facts.dlss_motion_fixed = ui::VULKAN_SECOND_QUEUE_MOTION;
        }  // a stopped native context: dlss_unavailable above (final review I-1)
      }
      facts.api = "Direct3D 12";
      if (entry.d3d11) {
        facts.api = "Direct3D 11";
      } else if (entry.d3d10) {
        facts.api = "Direct3D 10";
      } else if (entry.opengl) {
        facts.api = "OpenGL";
      } else if (entry.vulkan) {
        facts.api = (native_view ? "Vulkan (native NR)" : "Vulkan");
      }
      if (native_view || entry.context) {
        const addon::ContextStatus status = (native_view ? entry.vk_dlss->Status() : entry.context->Status());
        view.status_line = ui::FormatStatusLine({
            .state = status.session.state,
            .suspended = status.session.suspended,
            .passes_requested = status.session.passes_requested,
            .passes_run = status.passes_run,
            .frame = status.frame,
            .trigger = (status.trigger == addon::TriggerPoint::NONE ? std::string_view() : addon::TriggerPointName(status.trigger)),
            .encoding = status.encoding,
            .diffuse_white_nits = status.diffuse_white_nits,
            .grace_remaining = status.session.grace_remaining,
        });
        view.placement_line = status.placement_line;
        view.motion_line = status.motion_line;
        view.work_line = status.work_line;  // P17, D4: the "Working at" readout
        view.exposure_line = status.exposure_line;  // Plan 17: which input exposure NR used
        view.mask_note = entry.mask_note;
        view.ui_correction_note = status.ui_correction_note;
        view.frame_generation_line = ui::FormatFrameGenerationLine(
            status.frame_generation.active, status.frame_generation.multiplier, status.frame_generation.from_ngx);
        view.frame_generation_active = status.frame_generation.active;
        if (const std::optional<std::string> warning =
                ui::FrameGenerationWarning(status.frame_generation.active, state.settings.pass_count)) {
          view.frame_generation_warning = *warning;
        }
        if (native_view) {
          view.api_line = entry.vk_dlss->DetailsLine();  // the one-time cost's note lives here (and in the tips)
        }
        view.intermediate_bytes = status.intermediate_bytes;
        if (entry.d3d11_bridge) {
          // Final review Minor 1: the bridge's shared textures count as Uplift's, and its busy skips show here.
          view.intermediate_bytes += entry.d3d11_bridge->SharedBytes();
          view.api_line = entry.d3d11_bridge->StatusLine(status.placement);  // Plan 18: names the DLSS stage NR runs at
        }
        if (entry.d3d10_bridge) {
          // Plan 8: the relay's keyed textures and the bridge's shared ones count as Uplift's; its busy skips show here.
          view.intermediate_bytes += entry.d3d10_bridge->SharedBytes();
          view.api_line = entry.d3d10_bridge->StatusLine();
        }
        if (entry.vk_bridge && !native_view) {
          // Plan 11: the shared surfaces count as Uplift's, and the Details line names the path (GPU- or CPU-ordered) and why.
          view.intermediate_bytes += entry.vk_bridge->SharedBytes();
          view.api_line = entry.vk_bridge->StatusLine();
          // Plan 14: and so do the four copies of DLSS's motion vectors the native context holds on the game's device while it makes them.
          if (const uint64_t copies = (entry.vk_dlss ? entry.vk_dlss->HeldBytes() : 0u); copies > 0u) {
            view.intermediate_bytes += copies;
            view.api_line += (native_stopped ? std::format("; DLSS's motion vectors' last copies, kept until the game's device goes ({:.1f} MiB)",
                                                           static_cast<double>(copies) / (1024.0 * 1024.0))
                                             : std::format("; DLSS's motion vectors copied in the game's frame ({:.1f} MiB)",
                                                           static_cast<double>(copies) / (1024.0 * 1024.0)));
          }
        }
        if (entry.gl_bridge) {
          // Plan 12: the same for an OpenGL context: the shared surfaces count as Uplift's, and the Details line names the path (GPU- or CPU-ordered) and why.
          view.intermediate_bytes += entry.gl_bridge->SharedBytes();
          view.api_line = entry.gl_bridge->StatusLine();
        }
        view.runtime_bytes = status.session.runtime_bytes;
        view.effective_diffuse_white_nits = status.diffuse_white_nits;
        view.card = CardFor(state, entry, &status, view.status_line);
        // Plan 14: what runs, and the card's detail and notes (the working card's fixes hold only the passes note).
        addon::FillRunning(&facts, status, view.card.working);
        after_dlss_problem = status.after_dlss_problem;  // Plan 18 (design §3): an image the bridge cannot share greys its stage
        before_upscaling_problem = status.before_upscaling_problem;
        facts.after_dlss_fixed = after_dlss_problem;
        facts.before_upscaling_fixed = before_upscaling_problem;
        if (entry.vulkan && !native_view && entry.vk_dlss) {
          // Plan 14 (design §2.6): at Present on Vulkan the bridged context never sees the game's evaluate; the native context made (or missed) the copies.
          facts.dlss_motion_gap = entry.vk_dlss->PresentMotionGap();
          facts.game_passes_motion = (facts.dlss_motion_gap != ui::MotionGap::GAME_PASSED_NONE);
        }
        facts.dlss_seen = facts.dlss_seen || status.dlss_seen;
        // Plan 15: after the game's NGX shutdown on Direct3D 12, every option waits for the game's DLSS (temporary); abandoned there, nothing runs again this
        // session (fix round, minor 6: every option greyed with the fixed reason, the highlights included).
        facts.held = status.held;
        if (status.abandoned) {
          view.dlss_unavailable = std::string(ui::D3D12_NGX_ABANDONED_REASON);
          facts.stopped = ui::D3D12_NGX_ABANDONED_REASON;
        }
        if (Bridged(entry) && entry.strikes.Exhausted()) {
          facts.stopped = ui::PRIVATE_DEVICE_STOPPED_TWICE_REASON;  // Plan 17: the 2-strike rule's second stop is a fixed cause
        } else if (Bridged(entry) && entry.strikes.Unretryable()) {
          facts.stopped = ui::PRIVATE_DEVICE_STOPPED_FINAL_REASON;  // review M-3: a stop on the shared device, final at once
        }
        facts.frame_generation_blocks_present = (status.frame_generation.active && !state.settings.present_with_frame_gen);
        facts.vram_bytes = view.runtime_bytes.value_or(0u) + view.intermediate_bytes;
        facts.latch_note = (state.settings.dlss_placement_blocked ? addon::LATCH_UNAVAILABLE_REASON : std::string_view());
        facts.passes_note = ((view.card.working && !view.card.fixes.empty()) ? std::string_view(view.card.fixes.front()) : std::string_view());
        facts.frame_generation_line = (view.frame_generation_active ? std::string_view(view.frame_generation_line) : std::string_view());
      } else {
        view.card = CardFor(state, entry, nullptr, {});
      }
      // Plan 18 Task 12: DLSS seen here, and the game switched it off since (a release, or on Direct3D 11 an NGX shutdown): the DLSS stages, DLSS's vectors
      // and Match game wait for it (temporary), Present runs. Plan 15's hold, the stopped Vulkan context and the fixed causes keep their own text (ResolveSetup).
      facts.dlss_off = addon::DlssReleased(state.bridge.Registry(), device, DlssApiOf(entry), facts.dlss_seen);
    }
    view.dlss_latched = state.settings.dlss_placement_blocked;
    if (overlay_device_api != api::device_api::d3d9) {
      facts.dlss_unavailable = view.dlss_unavailable;
      ui::FinishSetup(&view, &state.overlay, runtime->get_device(), facts, now);
    }
    if (ui::DrawOverlay(view, &state.settings, &state.overlay)) {
      if (view.dlss_latched && !state.settings.dlss_placement_blocked) {
        // Clear latch: only the persisted record goes away here (Important 2 keeps the placements off
        // for the rest of THIS session on purpose, as the message itself says: restart to try again).
        addon::DeleteLatchMarker(state.latch_marker_path);
      }
      SaveAndApply(&state);
    }
    // Plan 6 (D11): Retry now, decided by the card's button; never after a removal or a teardown
    // (DeviceContext::RetryNow's own guard).
    if (std::exchange(state.overlay.retry_now, false)) {
      if (overlay_device_api == api::device_api::d3d9) {
        state.front->RetryNow(found != state.devices.end() ? &found->second.helper : nullptr);  // Plan 10
      } else if (found != state.devices.end() && Bridged(found->second) && found->second.strikes.RetryAllowed()) {
        found->second.retry_pending = true;  // Plan 17: the card shown was the stopped bridge's; the next present tears it down
      } else if (found != state.devices.end() && found->second.vk_dlss
                 && (found->second.vk_dlss->WantsNr() || found->second.vk_dlss->NrState() != nr::SessionState::OFF)) {
        found->second.vk_dlss->RetryNow();  // Plan 13: the card shown was the native context's
      } else if (found != state.devices.end() && found->second.context) {
        found->second.context->RetryNow();
      }
    }
  }
  UPLIFT_CATCH("draw_overlay", )
}

// Plan 13 (design §3.6): DeviceHook's vkDestroyDevice callback, on the game's thread before ReShade's own vkDestroyDevice (inside the detour's try), so
// NR on the device is released while ReShade still dispatches it. The game has idled the device. The context goes after the lock.
void OnVulkanDeviceDestroyed(VkDevice vk_device) {
  std::unique_ptr<addon::VkDlssContext> torn_down;
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;  // the Session's unload may reach the core's Shutdown1 on this thread, under the lock
  for (auto& [device, entry] : g_state->devices) {
    if (!entry.vk_dlss || device->get_native() != addon::ReshadeHandleOf(vk_device)) continue;
    entry.vk_dlss->Teardown();  // NrCompletion::DeviceIdle first, then the Session's flush and unload, then the frees
    NoteLatchIfTripped(g_state, entry.vk_dlss.get());
    g_state->claim.Forget(entry.vk_dlss.get());
    torn_down = std::move(entry.vk_dlss);
    g_state->bridge.AddVkContexts(-1);
    g_state->bridge.NoteVkContextNr(entry.vk_nr_counted, false);
    break;
  }
}

// Plan 13 (design §3.5 and R80, batch 2 carry: the "violator"): compute pipelines NVIDIA's DLSS binds inside its own Vulkan evaluate, seen through
// ReShade's layer. NGX's and Streamline's contract makes the game bind its own state again after DLSS, and Uplift replays none on Vulkan (decision 3):
// this records, once in the log, whether DLSS-SR itself disturbs the compute bind point. Lock-free: one TLS read for every other bind.
void OnVulkanBindPipeline(api::command_list* /*cmd_list*/, api::pipeline_stage stages, api::pipeline /*pipeline*/) {
  if (!ngx_hooks::InsideVulkanEvaluate() || (static_cast<uint32_t>(stages) & state::PIPELINE_STAGE_COMPUTE) == 0u) return;
  ++t_dlss_compute_binds;
}

// Plan 10 (design §2.1, §2.9): ReShade's create_device event (id 96), for the 9Ex toggle of 64-bit Direct3D 9 games. Registered raw in
// AddonInit; the front answers only for a Direct3D 9 device with UseD3D9Ex on.
// It must not take the lock for any other API (batch 2 review C-1): ReShade raises create_device inside D3D12CreateDevice and
// D3D11CreateDevice too, which OnPresent calls for the D3D10 relay, and on the fallback without a device factory for the bridges' private devices,
// while it holds the lock exclusively, and the lock is not recursive. Nothing Uplift does creates a Direct3D 9 device under it (D3D9Client only calls Direct3DCreate9Ex,
// which raises no event).
bool OnCreateDevice(api::device_api device_api, uint32_t& api_version) {
  if (device_api == api::device_api::vulkan) {
    // Plan 11 (Vulkan design §2.1): ReShade 6.8 raises this in every vkCreateInstance right after loading its add-ons and before any device can exist,
    // so this is where the detour goes in on 6.8 (AddonInit installs it only for an older ReShade). Lock-free (Plan 10 C-1): the hook's state has its
    // own lock, never this add-on's; the one thing touched of g_state is the leaf mutex of the hook's error text.
    try {
      if (const std::optional<std::string> error = vk::DeviceHook::Install()) {
        {
          // Minor 5: an unseen device's Details line says why, as with AddonInit's own install. Only this leaf lock, never the add-on's.
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

// ReShade calls this after loading the add-on, outside the loader lock (v6.0.0's add-on loader), so
// MinHook and the module enumeration can run here. A second call comes when ReShade reloads a pinned
// Uplift after the last device went away: everything is still registered, so it only reports success.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
  // Minor 2 (fix round 1): with the hooks running the module is pinned, so a second call here only
  // means "still registered, report success" (below). Without them, AddonUninit fully unregisters
  // once the last device goes away, so a later call on the same loaded module must register again --
  // g_state != nullptr alone must not short-circuit that.
  // Plan 11: the Vulkan device hook pins the module too, whatever the NGX hooks did.
  // Batch 2 review I-4: that holds only while ReShade is pinned as well; it then still lists Uplift with every event and overlay, and registering
  // the module a second time would even be refused (ReShadeRegisterAddon takes a module once). Without the pin (best effort: it does not fail in
  // practice) this call registers again below, as a first one: a ReShade that stayed loaded refuses that with two ERROR lines and keeps the first
  // registration, and one that really unloaded starts empty and takes it.
  if (g_state != nullptr && g_reshade_pinned.load(std::memory_order_acquire) && (g_state->hooks.Started() || vk::DeviceHook::Installed())) return true;
  // Fails when ReShade lacks this API version or ImGui table.
  if (!reshade::register_addon(addon_module, reshade_module)) return false;
  g_state = new AddonState();
  g_state->self_module = addon_module;
  std::vector<std::string> warnings;
  g_state->settings = ui::LoadSettings(g_state->config, &warnings);
  g_state->poller.Seen(g_state->settings.enabled);
  ApplyProcessSettings(g_state->settings);

  const auto module_path = [](HMODULE handle) {
    std::wstring path(32768u, L'\0');
    path.resize(GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path);
  };
  g_state->addon_directory = module_path(addon_module).parent_path();
  g_state->game_exe_path = module_path(nullptr);
  g_state->game_directory = g_state->game_exe_path.parent_path();
  // Important 3 (fix round 2): one resolution (UPLIFT_STATE_DIR, else LOCALAPPDATA, else temp) shared
  // by NGX's own app data and the latch marker below, instead of repeating LOCALAPPDATA-then-temp for
  // the former -- so a test's UPLIFT_STATE_DIR override moves NGX's app data out of the user's profile
  // too, not just the latch marker.
  const std::filesystem::path state_directory = addon::UpliftStateDirectory();
  g_state->ngx_data_directory = state_directory / L"ngx";

  // Important 1 (fix round 1): read before DlssUnavailableReason decides whether a DLSS placement
  // can run this session, so a marker from a crash last session blocks them from this session's very
  // first present -- DlssPlacementBlocked stays a mirror of it, for the overlay.
  g_state->latch_marker_path = addon::LatchMarkerPath(state_directory, g_state->game_exe_path);
  if (addon::LatchMarkerExists(g_state->latch_marker_path)) {
    g_state->settings.dlss_placement_blocked = true;
  }

  // Plan 10 (design §2.1, §2.2): Direct3D 9 devices run NR in gitc-uplift-helper64.exe, which lies next to this add-on; the front is the one
  // gitc-uplift.addon32 uses. The 9Ex pending marker is checked here, before the settings text the helper reads is made.
  g_state->front.emplace("gitc-uplift.addon64", g_state->addon_directory, g_state->game_directory, state_directory, g_state->game_exe_path);
  if (std::optional<std::string> warning = g_state->front->CheckExMarker(&g_state->settings, &g_state->config)) {
    warnings.push_back(std::move(*warning));
  }
  g_state->front->SettingsChanged(g_state->settings);

  // Spec §10 and v2 design §3.1: the NGX hooks, unless NgxHooks = Off (Plan 2's add-on exactly).
  std::string hook_error;
  const bool hooks_started =
      (g_state->settings.ngx_hooks == ui::NgxHooksMode::AUTO && g_state->hooks.Start(&g_state->bridge, &hook_error));
  const std::optional<addon::ModuleVersion> reshade_version = addon::ReadModuleVersion(reshade_module);
  g_state->dlss_unavailable_reason =
      addon::DlssUnavailableReason(g_state->settings, reshade_version, hooks_started, hook_error);

  // Plan 11 (Vulkan design §2): ReShade's Vulkan layer runs this inside the game's vkCreateInstance, before any device exists, so a detour on
  // vkCreateDevice installed now is in time for every one. The pending marker (a start whose adjusted device never presented) turns the adjustment
  // off, as the 9Ex marker does; CPU-ordered NR is the fallback then.
  const std::filesystem::path vk_marker = addon::LatchMarkerPath(state_directory, g_state->game_exe_path).replace_extension(L".vkdevice-pending");
  if (!vk::DeviceHook::CheckMarker(vk_marker)) {
    g_state->settings.adjust_vulkan_devices = false;
    ui::SaveSettings(g_state->settings, &g_state->config);
    warnings.push_back("Vulkan device adjustment turned off (AdjustVulkanDevices = 0): the last start with it did not reach its first frame");
  }
  const bool reshade_vulkan_events = (reshade_version && *reshade_version >= addon::MIN_RESHADE_VULKAN_EVENTS);
  g_state->reshade_vulkan_events = reshade_vulkan_events;
  vk::DeviceHook::Configure(g_state->settings.adjust_vulkan_devices, reshade_vulkan_events, vk_marker);
  // Plan 13 (design §3.6): native Vulkan NR is torn down in the vkDestroyDevice detour, before ReShade drops the device.
  vk::DeviceHook::SetDestroyCallback(&OnVulkanDeviceDestroyed);
  // Final review, minor 2: only for a ReShade older than 6.8. 6.8 raises create_device(vulkan) first in every vkCreateInstance, so OnCreateDevice's
  // install is always in time, and installing here too would only spread the detour, the pins and MinHook (which suspends every thread) into
  // D3D games that happen to have vulkan-1.dll loaded, plain 32-bit Direct3D 9 ones included.
  if (!reshade_vulkan_events && GetModuleHandleW(L"vulkan-1.dll") != nullptr) {
    if (const std::optional<std::string> error = vk::DeviceHook::Install()) {
      {
        const std::scoped_lock error_lock(g_state->vk_hook_error_mutex);
        g_state->vk_hook_error = *error;
      }
      warnings.push_back(std::format("Vulkan: the vkCreateDevice hook could not be installed: {}", *error));
    }
  }
  // Batch 2 review I-4: Uplift is pinned now (the NGX hooks, the detour), so ReShade's module is too.
  if (hooks_started || vk::DeviceHook::Installed()) {
    PinReshade(reshade_module);
  }

  reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyCommandQueue);
  // No destroy_swapchain handler: see the comment on SwapchainSelector for why (resize safety).
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
  if (hooks_started) {
    // Identity and completion-token bookkeeping, a few calls per list per frame (amendment 2).
    reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
    reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
    reshade::register_event<reshade::addon_event::execute_command_list>(OnExecuteCommandList);
    if (reshade_vulkan_events && g_state->settings.source == ui::PlacementSource::DLSS && g_state->dlss_unavailable_reason.empty()
        && GetModuleHandleW(L"vulkan-1.dll") != nullptr) {
      // Plan 13 (R80): the "violator" observation, lock-free. Batch 3 review, minor 7: only where it can matter, a start with Source already on a DLSS
      // placement (native NR runs only then, decision 1) under a ReShade that raises the Vulkan events native NR needs (ReShade's layer runs AddonInit
      // inside its vkCreateInstance). Never registered later: ReShade's event lists take no lock against a dispatch on another thread.
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnVulkanBindPipeline);
      g_dlss_binds_watched.store(true, std::memory_order_relaxed);
    }
  }
  if (g_state->dlss_unavailable_reason.empty()) {
    addon::RegisterTrackingEvents();  // once, and never unregistered while the game runs (amendment 2)
    g_state->tracking_registered = true;
  }
  reshade::register_overlay("GITC Uplift", OnDrawOverlay);

  nr::Logf(nr::LogLevel::INFO, "Uplift {} registered (ReShade API {}, Dear ImGui {}, ReShade {})", UPLIFT_VERSION,
           RESHADE_API_VERSION, IMGUI_VERSION_NUM,
           (reshade_version ? addon::FormatModuleVersion(*reshade_version) : std::string("of unknown version")));
  if (hooks_started) {
    for (const ngx_hooks::HookedModule& module : g_state->hooks.Modules()) {
      // Plan 18: every API the module's hooks cover, Direct3D 11 included (ngx_hooks::HookedApis).
      nr::Logf(nr::LogLevel::INFO, "NGX hooks on {} ({} entry points){}", addon::Utf8FromPath(module.file_name), ngx_hooks::HookedApis(module),
               (module.evaluate_c ? " (with EvaluateFeature_C)" : ""));
    }
  }
  nr::Logf(nr::LogLevel::INFO, "DLSS placements: {}",
           (g_state->dlss_unavailable_reason.empty() ? std::string("available") : g_state->dlss_unavailable_reason));
  for (const std::string& warning : warnings) {
    nr::Log(nr::LogLevel::WARN, warning);
  }
  return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module) {
  // With the NGX hooks running Uplift is pinned: its detours and events live as long as the process,
  // so it stays registered (ReShade 6.5.1+ keeps it; older versions keep its events, not its tab).
  // Plan 11: pinned by the Vulkan device hook too; its detour and the events stay for the process.
  if (g_state == nullptr || g_state->hooks.Started() || vk::DeviceHook::Installed()) return;
  // Plan 10: not pinned, so the Direct3D 9 helper (if one runs) ends now. Pinned, QUIT at NR off, the helper's 30 s idle exit and the
  // kill-on-close job when the game exits do it (design §2.1).
  g_state->front->Quit();
  addon::RemoveLogBridge();
  reshade::unregister_addon(addon_module, reshade_module);
}

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID reserved) {
  // Outside ReShade the add-on refuses to load. Inside it, ReShade calls AddonInit next.
  if (reason == DLL_PROCESS_ATTACH && reshade::internal::get_reshade_module_handle() == nullptr) return FALSE;
  // The process is ending (not a FreeLibrary): a clean exit clears the Vulkan pending marker (batch 2 review I-3). No lock, no allocation.
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
    g_process_exiting.store(true, std::memory_order_release);  // plan 13 final review, minor 3: no core Shutdown1 waits for the add-on's lock from here on
    vk::DeviceHook::NoteProcessExit();
  }
  return TRUE;
}
