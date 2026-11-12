#include <Windows.h>

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
#include "addon/streamline_present.hpp"
#include "addon/before_fg_device.hpp"
#include "addon/before_fg_dx12.hpp"
#include "addon/before_fg_vk.hpp"
#include "addon/command_list_tracker.hpp"
#include "addon/d3d11_identity.hpp"
#include "addon/device_context.hpp"
#include "addon/dlss_settings.hpp"
#include "addon/dlss_summary.hpp"
#include "addon/e2e_harness.hpp"
#include "addon/environment.hpp"
#include "addon/frame_config.hpp"
#include "addon/frame_trigger.hpp"
#include "addon/helper_front.hpp"
#include "addon/host_common.hpp"
#include "addon/launchpad_link.hpp"
#include "addon/live_facts.hpp"
#include "addon/motion_link_host.hpp"
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
#include "addon/vk_route.hpp"
#include "bridge/d3d10_bridge.hpp"
#include "bridge/d3d11_bridge.hpp"
#include "bridge/gl_bridge.hpp"
#include "bridge/private_device_bridge.hpp"
#include "bridge/record_nr.hpp"
#include "bridge/vk_bridge.hpp"
#include "build_id.hpp"
#include "client/vk_client.hpp"
#include "color/encoding.hpp"
#include "gl/functions.hpp"
#include "ngx_hooks/hook_installer.hpp"
#include "nr/d3d11_handles.hpp"
#include "nr/log.hpp"
#include "nr/snippet.hpp"
#include "nr/vk_handles.hpp"
#include "state/compute_shadow.hpp"
#include "text/before_fg_messages.hpp"
#include "text/effect_names.hpp"
#include "text/helper_messages.hpp"
#include "text/nr_messages.hpp"
#include "ui/controls_coalescer.hpp"
#include "ui/overlay.hpp"
#include "ui/settings.hpp"
#include "ui/settings_schema.hpp"
#include "ui/status_text.hpp"
#include "vk/device_hook.hpp"
#include "vk/format.hpp"
#include "vk/loader.hpp"

// The name ReShade shows in its Add-ons list. The log lines keep their "[Uplift]" prefix (addon/log_bridge.cpp).
extern "C" __declspec(dllexport) const char* NAME = uplift::addon::host::ADDON_NAME;
extern "C" __declspec(dllexport) const char* DESCRIPTION = uplift::addon::host::ADDON_DESCRIPTION;

namespace {

namespace addon = uplift::addon;
namespace bridge = uplift::bridge;
namespace client = uplift::client;
namespace color = uplift::color;
namespace gl = uplift::gl;
namespace ngx_hooks = uplift::ngx_hooks;
namespace nr = uplift::nr;
namespace state = uplift::state;
namespace ui = uplift::ui;
namespace vk = uplift::vk;
namespace api = reshade::api;
using uplift::text::BeforeFgNote;

// create_device(vulkan) hands each instance's API version to the device hook: ReShade appends VK_KHR_push_descriptor itself to the
// devices of an instance older than Vulkan's core push descriptors, and NGX's device additions must know whether it will.
constexpr addon::host::InstanceApiVersionSink NOTE_INSTANCE_API_VERSION = &vk::DeviceHook::NoteInstanceApiVersion;
constexpr std::chrono::milliseconds DIAGNOSTIC_GAME_WAIT{500};
// How often BlockedReason looks at the process's modules for foreign copies of the runtime: listing every module and its path each frame is too
// costly, and a copy mapped mid-session is seen within a second.
constexpr std::chrono::milliseconds MAPPED_RUNTIME_RECHECK_MS{1000};
constexpr uint64_t BEFORE_FG_TRACE_INITIAL_PRESENTS = 2u;
constexpr uint64_t BEFORE_FG_TRACE_INTERVAL = 240u;

constexpr std::string_view UPLIFT_VERSION = UPLIFT_VERSION_TEXT;  // project(VERSION) in CMakeLists.txt
// 1.1.6: ReShade's own definition for games whose depth is upside down (ReShade.fxh). DLSS works on the same camera images as that depth, so its motion
// vectors are upside down against the back buffer too: the Present path flips them while it is set.
constexpr char DEPTH_UPSIDE_DOWN_DEFINE[] = "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN";

static_assert(static_cast<uint32_t>(api::color_space::unknown) == static_cast<uint32_t>(color::ColorSpace::UNKNOWN));
static_assert(static_cast<uint32_t>(api::color_space::srgb_nonlinear) == static_cast<uint32_t>(color::ColorSpace::SRGB_NONLINEAR));
static_assert(static_cast<uint32_t>(api::color_space::extended_srgb_linear) == static_cast<uint32_t>(color::ColorSpace::EXTENDED_SRGB_LINEAR));
static_assert(static_cast<uint32_t>(api::color_space::hdr10_st2084) == static_cast<uint32_t>(color::ColorSpace::HDR10_ST2084));
static_assert(static_cast<uint32_t>(api::color_space::hdr10_hlg) == static_cast<uint32_t>(color::ColorSpace::HDR10_HLG));
// The compute shadow compares ReShade's values as numbers; these are the ones v6.0.0 names.
static_assert(static_cast<uint32_t>(api::shader_stage::compute) == state::SHADER_STAGE_COMPUTE);
static_assert(static_cast<uint32_t>(api::pipeline_stage::compute_shader) == state::PIPELINE_STAGE_COMPUTE);
static_assert(static_cast<uint32_t>(api::descriptor_type::constant_buffer) == state::DESCRIPTOR_CONSTANT_BUFFER);
static_assert(static_cast<uint32_t>(api::descriptor_type::acceleration_structure) == state::DESCRIPTOR_ACCELERATION_STRUCTURE_6_1);

// DiagnosticRemoveDevice presses Retry now itself this many presents (about a second at 60 Hz) after a stop that may still be retried.
constexpr uint32_t DIAGNOSTIC_RETRY_PRESENTS = 60u;
// Native NR at Present has proved itself on this start after this many frames with NR applied (a few seconds): its pending marker goes.
constexpr uint32_t NATIVE_MARKER_FRAMES = 180u;
// The helper's NR not known OFF this long after NR moved away from it is reported (once); NR still waits for it.
constexpr std::chrono::seconds HELPER_DRAIN_WARN{5};
// A bridged device's stop when its context found the private device removed and the bridge had not latched; the argument is the bridge's Label().
constexpr char PRIVATE_DEVICE_REMOVED_LINE[] = "The {} bridge stopped: its private Direct3D 12 device was removed";
// The native route's sentence once native NR at Present stopped on a device (the context's PresentStopped), and the three lines a route fact logs once.
constexpr char VULKAN_NATIVE_STOPPED_LINE[] = "native Vulkan NR stopped: {}";
constexpr char VULKAN_NATIVE_STOPPED_WARN_LINE[] =
    "Vulkan: native NR at Present stopped on this device ({}); NR at Present moves along the route chain for the rest of this session";
constexpr char VULKAN_NATIVE_CREATE_FAILED_WARN_LINE[] =
    "Vulkan: NGX could not create NR's feature on the game's device ({}); NR at Present uses the Direct3D 12 route for the rest of this session";
constexpr char VULKAN_D3D12_CREATE_FAILED_WARN_LINE[] =
    "Vulkan: NGX could not create NR's feature on the private Direct3D 12 device ({}); NR at Present uses Uplift's helper process for the rest of this "
    "session";
// The native route's reason while VulkanNativeNr is 0.
constexpr std::string_view VULKAN_NATIVE_OFF_REASON =
    "native Vulkan NR is off (VulkanNativeNr = 0: Uplift sets it after a start or a device loss with native NR went wrong; press Clear latch, or set "
    "VulkanNativeNr=1 under [Uplift], and restart the game to try again)";

struct DeviceEntry {
  addon::SwapchainSelector selector;
  // A D3D11 device's bridge to its private D3D12 device. Made at the first present with NR on, and kept until
  // the device goes. OnDestroyDevice destroys it outside the add-on's lock.
  std::unique_ptr<bridge::D3D11Bridge> d3d11_bridge;
  // A D3D10 device's bridge (a relay D3D11 device into the D3D11 bridge). As d3d11_bridge: made at the first
  // present with NR on, kept until the device goes, destroyed outside the add-on's lock.
  std::unique_ptr<bridge::D3D10Bridge> d3d10_bridge;
  std::unique_ptr<addon::DeviceContext> context;
  addon::BeforeFgDevice before_fg;
  color::ColorSpace output_space = color::ColorSpace::UNKNOWN;
  std::string message;                                   // why this device has no context
  bool rejected = false;                                 // not NVIDIA, or the context failed to start: decided once
  std::filesystem::path snippet_path;                    // resolved once at context creation; empty when not found
  // The foreign copies of the runtime mapped in the process, for snippet_path; looked up at most once per MAPPED_RUNTIME_RECHECK_MS.
  nr::MappedRuntimeDecision mapped_runtime;
  std::filesystem::path mapped_runtime_for;  // the snippet_path that mapped_runtime was decided for
  std::chrono::steady_clock::time_point mapped_runtime_checked;
  uint64_t mapped_runtime_load_count = 0u;  // nr::SnippetLoadCount() when mapped_runtime was decided
  std::string static_block;                              // a conflicting host or a missing runtime: decided at creation
  bool runtime_in_game_folder = false;                   // 1.1.4: the runtime is mapped from the game's own folder (NVIDIA's DLSS loaded it)
  ui::CardStage block_stage = ui::CardStage::CONFLICTS;  // which stage static_block fails
  nr::Size swapchain_size;                               // the primary swap chain's back buffer, for the main-handle choice
  // The frame in flight on the primary swap chain, set in the present event.
  api::command_queue* queue = nullptr;
  api::effect_runtime* runtime = nullptr;
  api::resource back_buffer = {0u};
  api::effect_technique marker = {0u};
  // Launchpad's technique and the Uplift technique are both enabled (Setup's Launchpad option); set at each present.
  bool launchpad_ready = false;
  bool lumenite_ready = false;      // the same for Lumenite's Kernel (Setup's Lumenite option)
  bool uplift_mv_lumenite = false;  // Uplift.fx writes Lumenite's vectors into UPLIFT_MV (the links' state; the readouts name it)
  addon::UpliftMvSource uplift_mv_source = addon::UpliftMvSource::NONE;  // 1.2.1: the source Uplift.fx is compiled with (MotionLinkStep::compiled)
  std::string mask_note;  // the overlay's "NR mask" line
  // The device's API: d3d11, d3d10, vulkan and opengl are set together at every present, from the device's first one on.
  bool d3d11 = false;     // A D3D11 device; its NR runs through d3d11_bridge
  // The automatic UPLIFT_USE_LAUNCHPAD link and UPLIFT_USE_LUMENITE's, with Setup's "ready" for each, for `link_runtime`.
  // Reset when that runtime is destroyed (a reinit too) or changes, or (the links) when its preset changes.
  addon::MotionLinks motion_links;
  api::effect_runtime* link_runtime = nullptr;
  std::string link_preset_path;
  bool d3d10 = false;  // A D3D10 device; its NR runs through d3d10_bridge
  // A Direct3D 9 device's clients and helper-side state. Its NR runs in gitc-uplift-helper64.exe through the front
  // both add-ons share; no context and no bridge exist for it. A Vulkan device's on the helper route (its bridge and bridged context are torn down
  // first; its native context may still run a DLSS placement).
  addon::HelperDevice helper;
  // A Vulkan device's bridge to its private D3D12 device. Made at the first present with NR on, kept until the device
  // goes; OnDestroyDevice frees its Vulkan objects (through ReShade's API only) and destroys it outside the add-on's lock.
  std::unique_ptr<bridge::VkBridge> vk_bridge;
  bool vulkan = false;                          // a Vulkan device; its NR runs through vk_bridge
  api::command_queue* present_queue = nullptr;  // the present event's queue; on Vulkan `queue` is the effect runtime's
  bool vk_presents_counted = false;             // DeviceHook::NotePresent has finished counting (the pending marker's second present)
  bool vk_second_queue_logged = false;
  // An OpenGL context's bridge to its private D3D12 device. Made at the first present with NR on while the present is on the
  // effect runtime's context, kept until the device goes; OnDestroyDevice forgets its GL names with no GL call and destroys it outside the add-on's lock.
  // On OpenGL `queue` is the effect runtime's queue (its immediate list is the context itself) and `present_queue` the present event's, as on Vulkan.
  std::unique_ptr<bridge::GlBridge> gl_bridge;
  bool opengl = false;  // an OpenGL device; its NR runs through gl_bridge
  // NR after DLSS natively on a 64-bit Vulkan device, next to vk_bridge's Present. Made at a present once
  // Source is DLSS (the overlay's After DLSS or Before upscaling on Vulkan; Auto stays at Present), for a device whose record says ngx_ready with an
  // instance; kept until the device goes (the vkDestroyDevice detour tears it down; destroy_device abandons what is left).
  std::unique_ptr<addon::VkDlssContext> vk_dlss;
  bool vk_native_checked = false;  // what the device lacks for native NR was decided from its facts (they never change)
  bool vk_nr_counted = false;      // NgxBridge::NoteVkContextNr: the NGX hooks watch this device's Vulkan DLSS evaluates for its native context
  // Where NR at Present runs on this device (addon::ChooseVkNrRoute, at each present), from VulkanNr and these facts, each kept for the session.
  // Why native NR cannot run here (addon::VkNativeBlock: what the device lacks, a context that could not be made, Present's command buffers, or NGX's
  // creation failure, logged once); its causes exclude one another, so the first is kept. nullopt: nothing.
  std::optional<addon::VkNativeBlock> vk_native_block;
  std::optional<std::string> vk_native_stopped;  // native NR at Present stopped on the device for the session: the route's sentence (logged once)
  bool vk_native_latch_noted = false;    // the device loss's VulkanNativeNr latch was applied (once per device)
  std::optional<addon::VkRouteChoice> vk_route;  // the latest choice: logged when it changes, shown by the panel
  bool vk_native_present = false;        // NR at Present runs natively this frame: the effect events reach the native context (RunNative)
  // Transitions: NR's owner on this device at the last present (addon::ChooseVkNrOwner), and the facts its change's cause is read from.
  addon::VkNrOwner vk_owner = addon::VkNrOwner::NONE;
  bool vk_owner_stage = false;
  ui::VulkanNrMode vk_owner_setting = ui::VulkanNrMode::NATIVE;
  // The Direct3D 12 route's own facts, kept for the session as the native ones are: the chain goes on to the helper (`helper`, through the front).
  // Why the bridge route cannot run here: a creation failure (logged once) replaces a start failure and is never replaced; a later start failure replaces an
  // earlier one. nullopt: nothing.
  std::optional<addon::VkD3d12Block> vk_d3d12_block;
  // Helper clients a route change left (their shares retired behind the effect queue's fences), freed once the queue has passed them.
  std::vector<std::unique_ptr<client::VkClient>> retiring_clients;
  // Since when the route has been waiting for the helper's NR to be known OFF (warned once after HELPER_DRAIN_WARN), and the
  // one-time notes of a claim that would have let NR load next to NR in the other process.
  std::optional<std::chrono::steady_clock::time_point> helper_drain_since;
  bool helper_drain_warned = false;
  // Why After DLSS and Before upscaling are greyed on this Vulkan device (addon::DecideVkStages, at each present); empty while they can run.
  std::string vk_stages_off;
  bool helper_guard_noted = false;
  bool native_guard_noted = false;
  // A Direct3D 12 device's hold after the game's NGX shutdown, kept across its contexts. While a context exists, its own copy is
  // the one that moves; destroy_command_queue keeps it here when it ends the context, a shutdown with no context records it here, and the next context
  // starts from it, so a new one never loads NR into a core the game shut down (or into an abandoned runtime).
  addon::CoreShutdownHold core_hold;
  // Retry now after this device's private-device NR stopped (its bridge's device removed or hung, or the CPU-ordered timeouts),
  // with the 2-strike rule. `retry_pending`: the overlay asked; the next present tears the stopped bridge and its context down, and the one after builds
  // new ones. `retiring_vk`: Vulkan bridges a retry tore down, kept until the game's queue has passed the fences behind their imports.
  addon::BridgeStrikes strikes;
  bool retry_pending = false;
  uint32_t diagnostic_retry_in = 0u;  // DiagnosticRemoveDevice: presents until it presses Retry now itself (0: not counting)
  uint32_t diagnostic_frames = 0u;    // DiagnosticRemoveDevice: presents with NR applied on the current private device
  std::vector<std::unique_ptr<bridge::VkBridge>> retiring_vk;
  // A Direct3D 11 device's DLSS stages. `d3d11_watch_counted`: NgxBridge::NoteD3D11Watch's flag (the hooks watch its evaluates while
  // its bridge's context exists and NR is on or not yet released). `foreign_dlss`: its game's DLSS ran on a Direct3D 12 device Uplift cannot reach (logged
  // once). `present_motion_logged`: "copied for Present" was logged since the ring was last released.
  bool d3d11_watch_counted = false;
  bool foreign_dlss = false;
  bool present_motion_logged = false;
  std::optional<bool> depth_upside_down;  // 1.1.6: RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN as last read (logged when it changes)
  // The "NR running" line every NrHeartbeat::PERIOD and the "NR stopped after" one, with this process's private bytes and VRAM on `vram`'s adapter.
  addon::NrHeartbeat heartbeat;
  addon::ProcessVram vram;

  // The private-device bridge allocated for this device, or null (none yet, or torn down by Retry now, a Vulkan route change or a failed
  // context). At most one of the four is ever set: each is made only in its own API's branch of OnPresent's context creation, and a device's
  // API never changes. It says nothing about the device's API (Bridged) or about who runs NR now (the Vulkan owner, the claims).
  [[nodiscard]] bridge::PrivateDeviceBridge* ActiveBridge() { return ActiveOf<bridge::PrivateDeviceBridge>(*this); }
  [[nodiscard]] const bridge::PrivateDeviceBridge* ActiveBridge() const { return ActiveOf<const bridge::PrivateDeviceBridge>(*this); }

 private:
  template <class Bridge, class Self>
  static Bridge* ActiveOf(Self& self) {
    if (self.d3d11_bridge) return self.d3d11_bridge.get();
    if (self.d3d10_bridge) return self.d3d10_bridge.get();
    if (self.vk_bridge) return self.vk_bridge.get();
    if (self.gl_bridge) return self.gl_bridge.get();
    return nullptr;
  }
};

// NR on this device runs on a private D3D12 device through a bridge (D3D11, or D3D10 through its relay; Vulkan; OpenGL).
bool Bridged(const DeviceEntry& entry) {
  return entry.d3d11 || entry.d3d10 || entry.vulkan || entry.opengl;
}

// The bridged device's private Direct3D 12 device (native), or null before its bridge exists.
ID3D12Device* PrivateDevice(const DeviceEntry& entry) {
  const bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge();
  return (private_bridge != nullptr ? private_bridge->Device() : nullptr);
}

// Whether the bridge's private device can be replaced by Retry now (D3D12Side::Independent); true before a bridge exists.
bool PrivateDeviceIndependent(const DeviceEntry& entry) {
  const bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge();
  return (private_bridge == nullptr || private_bridge->Independent());
}

// The bridge's latch text ("The OpenGL bridge stopped: ..."); empty while it runs.
std::string_view BridgeLatch(const DeviceEntry& entry) {
  const bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge();
  return (private_bridge != nullptr ? private_bridge->Latch() : std::string_view());
}

// Stops whichever bridge the device has (a no-op past the first reason).
void StopBridge(DeviceEntry& entry, std::string reason) {
  if (bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge()) {
    private_bridge->Stop(std::move(reason));
  }
}

// 1.1.6: ReShade's RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN, the preset's or the global one, as ReShade.fxh reads it (any non-zero number is set).
bool DepthUpsideDown(api::effect_runtime* runtime) {
  char value[32] = {};
  if (runtime == nullptr || !runtime->get_preprocessor_definition(DEPTH_UPSIDE_DOWN_DEFINE, value)) return false;
  return std::strtol(value, nullptr, 0) != 0;
}

// The device's private-device NR has stopped: its bridge latched (a removed or hung private device, the CPU-ordered timeouts, a failed cross-API
// step), or its context found the private device removed.
bool PrivateDeviceStopped(const DeviceEntry& entry) {
  const bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge();
  const bool latched = (private_bridge != nullptr && private_bridge->Stopped());
  return latched || (entry.context && entry.context->DeviceLost());
}

// One heartbeat line ("NR running: ..." or "NR stopped after ..."), with this process's private bytes and its VRAM on the adapter NR runs on:
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

// The bridge's view of the add-on. Defined after AddonState.
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
  // The Vulkan routing. `list` is a VkCommandBuffer (nr/vk_handles.hpp).
  const void* DeviceOfVk(ID3D12GraphicsCommandList* list, VkDevice vk_device) override;
  void OnMainEvaluateVk(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame,
                        const ngx_hooks::VkDlssResources& copies) override;
  ID3D12Resource* BeforeMainEvaluateVk(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame,
                                       const ngx_hooks::VkDlssResources& copies) override;
  void EndVulkanEvaluate(ID3D12GraphicsCommandList* list) override;
  void ColorSwapRejectedVk(const void* device, ID3D12GraphicsCommandList* list) override;
  void BeforeCoreShutdownVk(VkDevice vk_device) override;
  void BeforeCoreShutdownD3D12(ID3D12Device* device) override;
  // The Direct3D 11 routing. `list` is an ID3D11DeviceContext (nr/d3d11_handles.hpp) and never reaches a Direct3D 12 call.
  const void* DeviceOfD3D11(ID3D12GraphicsCommandList* list) override;
  const void* DeviceOfD3D11Device(ID3D11Device* device) override;
  void OnMainEvaluateD3D11(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) override;
  ID3D12Resource* BeforeMainEvaluateD3D11(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) override;
  void ColorSwapRejectedD3D11(const void* device, ID3D12GraphicsCommandList* list) override;
  void LogFirstEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) override;
};

struct AddonState {
  std::shared_mutex mutex;
  std::shared_mutex queue_mutex;
  struct QueueRecord { api::command_queue* queue; api::device* device; };
  std::unordered_map<uint64_t, QueueRecord> queues;
  addon::ReshadeConfigStore config;
  ui::Settings settings;
  ui::EnabledPoller poller;
  ui::ControlsCoalescer coalescer;
  ui::OverlayState overlay;
  std::unordered_map<api::device*, DeviceEntry> devices;
  std::vector<api::effect_runtime*> runtimes;
  std::once_flag before_fg_start;
  addon::BeforeFgVk before_fg_vk;
  addon::NrClaim claim;  // at most one device loads NR
  // A Direct3D 9 device's NR runs in the helper, not in this process, so it does not count against the one
  // in-process NR above. The single helper still serves one device at a time: this claim is the helper's alone (a Vulkan device on the helper
  // route takes it too, and the one above as well, so the helper's NR never runs next to an in-process one on that device).
  addon::NrClaim helper_claim;
  AddonRouting routing;
  addon::NgxBridge bridge{routing};
  ngx_hooks::HookInstaller hooks;
  bool diagnostic_before_fg = false;
  std::string dlss_unavailable_reason;  // decided at AddonInit; empty when a DLSS placement can run
  // Why the vkCreateDevice hook is not installed (a Vulkan device then runs CPU-ordered); empty when it is or was not tried. Written by
  // AddonInit and by create_device(vulkan), which takes no other lock of this add-on, so it has a lock of its own: a leaf, never
  // held while taking another.
  std::mutex vk_hook_error_mutex;
  std::string vk_hook_error;
  // NoteLatchIfTripped may run on an evaluate or a teardown thread, where mirroring
  // the trip into ReShade.ini would race the present thread's own flush_cache()/save(). Set there,
  // cleared and acted on only by the next OnPresent, on the present thread.
  bool latch_save_pending = false;
  addon::E2eHarness e2e;
  // Native NR at Present's crash latch. The pending marker is written before the first native NR load of this start and deleted once
  // NR has applied NATIVE_MARKER_FRAMES frames natively (or at a clean exit); one still there at the next start sets VulkanNativeNr = 0. A device loss soon
  // after a native submission sets it too. `vk_native_off`: the native route's reason while VulkanNativeNr is 0 (empty while it is 1).
  std::filesystem::path vk_native_marker;
  std::atomic<bool> vk_native_marker_pending{false};
  api::device* vk_native_marker_owner = nullptr;  // the device whose native Present wrote it; its Session reaching OFF clears it
  bool vk_native_marker_done = false;
  uint32_t vk_native_frames = 0u;
  std::string vk_native_off;
  bool tracking_registered = false;
  HMODULE self_module = nullptr;
  std::filesystem::path addon_directory;
  std::filesystem::path game_exe_path;  // the game's own executable, for the latch marker's name
  std::filesystem::path game_directory;
  std::filesystem::path ngx_data_directory;
  std::filesystem::path latch_marker_path;   // computed once, at AddonInit
  bool snippet_hashed = false;               // the ~166 MB runtime is hashed at most once per process
  bool warned_no_immediate_list = false;     // ReShade gave no immediate list for a presenting queue
  bool warned_invalid_snippet_path = false;  // SnippetPath was not valid UTF-8: logged once
  uint64_t settings_generation = 0u;         // every SaveAndApply; restarts the retry backoff
  // Made in AddonInit. Hosts every Direct3D 9 device (and the helper they share); Direct3D 10, 11 and 12 never touch it. Also every Vulkan
  // device whose NR at Present takes the helper route.
  std::optional<addon::HelperFront> front;
  // ReShade raises the Vulkan create events Uplift's native NR relies on (6.8 or newer); decided at AddonInit.
  bool reshade_vulkan_events = false;
  // The tokens of a stale VkListState that init_command_list replaced. init_command_list may run inside Uplift's
  // own work (a command buffer the NR runtime allocates under the add-on's lock), so it never takes that lock: the tokens wait here, under a leaf
  // lock of their own, and the device's next present recycles them.
  std::mutex vk_orphans_mutex;
  std::vector<addon::StaleVkList> vk_orphans;
  // Orders the Direct3D 12 token submissions against later Resets of any command list (OnExecuteCommandList).
  addon::SubmitEpochs submit_epochs;
};

// Created in AddonInit and never freed: with the NGX hooks running the module is pinned, and at
// process exit D3D12 objects must not be touched.
AddonState* g_state = nullptr;
// The device whose tokens this thread submitted and has not stamped yet.
thread_local api::device* t_unstamped_device = nullptr;
// The submit epoch of this thread's latest token submission (valid while t_unstamped_device is set).
thread_local uint64_t t_unstamped_epoch = addon::NO_SUBMIT_EPOCH;
// Compute pipelines NVIDIA's DLSS bound on this thread inside its Vulkan evaluates since the last main-handle evaluate (OnVulkanBindPipeline).
thread_local uint32_t t_dlss_compute_binds = 0u;
// OnVulkanBindPipeline is registered (AddonInit), so the count above means something.
std::atomic<bool> g_dlss_binds_watched{false};
// This thread holds the add-on's lock while it runs NR's runtime on a Vulkan device (a Session that may unload, a teardown, a
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

// Settings that act outside the per-frame FrameSettings.
void ApplyProcessSettings(const ui::Settings& settings) {
  addon::InstallLogBridge(settings.log_level);
  // The NGX runtime reads it when the snippet next loads.
  SetEnvironmentVariableW(L"__NGX_SHOW_INDICATOR", (settings.show_nv_indicator ? L"1024" : nullptr));
}

void PublishSettings(AddonState& state) {
  ++state.settings_generation;
  state.front->SettingsChanged(state.settings);
}

// After the overlay or the hotkey changed a setting.
void SaveAndApply(AddonState* state) {
  state->settings = ui::Sanitized(state->settings);
  ui::SaveSettings(state->settings, &state->config);
  state->poller.Seen(state->settings.enabled);
  ApplyProcessSettings(state->settings);
  PublishSettings(*state);
}

// With the add-on's lock held.
addon::DeviceContext* ContextOf(api::device* device) {
  const auto found = g_state->devices.find(device);
  return (found == g_state->devices.end() ? nullptr : found->second.context.get());
}

// These process-wide counters are collected only while DiagnosticBeforeFG is enabled.
struct PresentCounters {
  std::atomic<uint64_t> dxgi_real{};
  std::atomic<uint64_t> vk_real{};
  std::atomic<uint64_t> reshade{};
  std::atomic<uint64_t> nr_passes{};
} present_counts;

bool BeforeFgTrace() { return g_state->diagnostic_before_fg; }

bool SamplePresentCount(uint64_t count) { return count <= BEFORE_FG_TRACE_INITIAL_PRESENTS || count % BEFORE_FG_TRACE_INTERVAL == 0u; }

std::optional<uint64_t> NextBeforeFgTraceSample(std::atomic<uint64_t>& total) {
  if (!BeforeFgTrace()) return std::nullopt;
  const uint64_t count = ++total;
  return SamplePresentCount(count) ? std::optional{count} : std::nullopt;
}

// Install hooks for newly loaded NGX cores outside the loader lock. An idle poll costs one atomic exchange.
void PollHooks() {
  g_state->hooks.Poll();
  addon::streamline::Poll();
}

// This thread's earlier ExecuteCommandLists calls have returned by its next Uplift event,
// so one Signal per queue now follows them. `later_call`: the event is an execute_command_list for a list that proves a later call.
void StampThisThread(bool later_call = false) {
  api::device* const device = std::exchange(t_unstamped_device, nullptr);
  if (device == nullptr) return;
  const std::unique_lock lock(g_state->mutex);
  if (addon::DeviceContext* const context = ContextOf(device)) {
    context->StampThread(GetCurrentThreadId(), later_call);
  }
}

void DropTokens(api::device* device, std::span<const uint64_t> tokens) {
  const std::unique_lock lock(g_state->mutex);
  if (addon::DeviceContext* const context = ContextOf(device)) {
    context->DropTokens(tokens);
  }
}

// The first time `context` reports a
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
// `Context` is a DeviceContext or a VkDlssContext (a Vulkan device lost soon after NR ran in the game's command buffer latches the same way).
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
  // The ini mirror runs only on the present thread now (see latch_save_pending).
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
  const HoldsLockForNr holds;  // A starving present's catch-up may unload NR here, and the snippet's Shutdown1 may reach the core's
  addon::DeviceContext* const context = ContextOf(static_cast<api::device*>(const_cast<void*>(device)));
  if (context == nullptr) {
    g_state->bridge.NoteForeignD3D12Dlss();  // A device without a context (a mod's own, in a Direct3D 11 game)
    return;
  }
  addon::ReshadeDlssFrameHost host(list_state);
  context->OnDlssEvaluate(frame, host, std::chrono::steady_clock::now());
  // A removal first seen by an evaluate (presents may be starving) still
  // decides the latch here, under the lock this function already holds.
  NoteLatchIfTripped(g_state, context);
}

ID3D12Resource* AddonRouting::BeforeMainEvaluate(const void* device, ID3D12GraphicsCommandList* list,
                                                 const ngx_hooks::DlssFrame& frame) {
  StampThisThread();
  addon::ListState* const list_state = addon::FindListState(list);
  if (list_state == nullptr) return nullptr;
  const std::unique_lock lock(g_state->mutex);
  const HoldsLockForNr holds;  // as OnMainEvaluate's
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

// A tracked command buffer names its device without the add-on's lock; only CreateFeature1's VkDevice for an untracked
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
  // The token's event closes this call on every path, an exception in the recording included, while the lock is still held.
  struct CloseEvaluate {
    addon::VkDlssContext* context;
    VkCommandBuffer buffer;
    addon::VkListState* list_state;
    ~CloseEvaluate() {
      try {
        context->EndEvaluate(buffer, *list_state);
      } catch (...) {
        // The hook's catch reports the recording's failure. A token left open here is dropped once both stale-age guards are met.
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

// The game's NVSDK_NGX_VULKAN_Shutdown1 for `vk_device` (Streamline's slShutdown, before the game destroys the device), on the game's
// thread before the core's own runs. Native NR on that device makes its last NGX calls here (after a wait of at most 2 s for its recordings, events only),
// so none comes after the core's; the lock is released when this returns, before the original runs. The vkDestroyDevice detour then frees only Uplift's
// own Vulkan objects.
void AddonRouting::BeforeCoreShutdownVk(VkDevice vk_device) {
  // Never when Uplift's own locked code reached the core's Shutdown1 on this thread (it would wait for itself), nor once the
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

// The game's NVSDK_NGX_D3D12_Shutdown1 for `device` (a DLSS settings change that re-initialises NGX, or Streamline's slShutdown
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
    // Between two contexts (the present queue went, and no present has made the next one yet) nothing of NGX's is held here; the hold
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

// The NGX API the game's DLSS on this device goes through, for the registry's per-API questions.
ngx_hooks::NgxApi DlssApiOf(const DeviceEntry& entry) {
  return (entry.vulkan ? ngx_hooks::NgxApi::VULKAN : entry.d3d11 ? ngx_hooks::NgxApi::D3D11 : ngx_hooks::NgxApi::D3D12);
}

// Why the DLSS stages cannot run on this Direct3D 11 device, or empty. With the add-on's lock held.
std::string_view D3D11DlssReason(const AddonState& state, const DeviceEntry& entry) {
  if (!state.dlss_unavailable_reason.empty()) return state.dlss_unavailable_reason;  // hooks off or failed, ReShade, the latch
  if (entry.foreign_dlss && !(entry.context && entry.context->DlssSeen())) return ui::FOREIGN_D3D12_DLSS_REASON;
  return {};
}

// DiagnosticRemoveDevice at a Direct3D 11 DLSS stage removes Uplift's private device inside the game's frame, right after a hand-off
// queued the game's wait on it, so the e2e case proves the game's queue is released. The present-time removal skips such a device.
// The removal happens with both queues at rest, only waiting, as the never-hang proof means it. On
// the removal frame the private queue is first held by a gate fence nobody signals, queued ahead of the hand-off's own work, so NR's kernels do not run, and
// the removal waits (DiagnosticWaitForGameSignal, capped by DIAGNOSTIC_GAME_WAIT) until the game's queue passed its copies into the shares and sits at its wait. Removing the
// device while GPU work touching its allocations runs (NR's kernels, or the game's copies in) faults every SM (MMU fault) and the GPU's recovery hangs the
// game's device too, whatever the fences do: measured DEVICE_HUNG and 384 nvlddmkm events with no gate, and the same with the gate alone; with both queues
// waiting the removal releases the game's wait at once (four probes, and this case: 0 events). The gate exists only on that one frame of the hidden key.
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
    const bool game_waiting = entry.d3d11_bridge->DiagnosticWaitForGameSignal(static_cast<DWORD>(DIAGNOSTIC_GAME_WAIT.count()));
    nr::Logf(nr::LogLevel::WARN,
             "Diagnostic (DiagnosticRemoveDevice = {}): removing Uplift's own private Direct3D 12 device inside the game's frame (its queue held ahead of "
             "NR's work; the game's queue {})",
             state.settings.diagnostic_remove_device, (game_waiting ? "at its wait" : std::format(uplift::text::GAME_QUEUE_NOT_WAITING_LINE, DIAGNOSTIC_GAME_WAIT.count())));
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

// After the original evaluate of a Direct3D 11 main handle, on the game's thread and immediate context: the context decides first
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
    entry.context->NoteGameDeviceRemoved(removed, now);  // the latch window decides; nothing crosses again
    NoteLatchIfTripped(&state, entry.context.get());
    return;
  }
  const nr::Rect output = bridge::ResolveRegion11(nr::D3D11ResourceOf(frame.output), frame.output_region);
  const addon::BridgedDlssDecision decision = entry.context->DecideAfterEvaluate(frame, {output.width, output.height}, now);
  // Split chain: with pass 1 inside the frame and a later group at Present, the ring copy comes first, then After DLSS's hand-off (both may run).
  if (decision.work == addon::BridgedDlssWork::PRESENT_MOTION || decision.present_motion) {
    ID3D11Resource* const motion = nr::D3D11ResourceOf(frame.motion_vectors);
    if (entry.d3d11_bridge->CopyPresentMotion(motion, frame.motion_region, decision.motion_scale_x, decision.motion_scale_y) && !entry.present_motion_logged) {
      entry.present_motion_logged = true;
      const nr::Rect region = bridge::ResolveRegion11(motion, frame.motion_region);
      nr::Logf(nr::LogLevel::INFO, "Direct3D 11: DLSS's motion vectors copied for Present ({}x{})", region.width, region.height);
    }
  }
  if (decision.work == addon::BridgedDlssWork::AFTER_DLSS) {
    const Microsoft::WRL::ComPtr<ID3D12Fence> gate = DiagnosticGateMidFrame(state, entry);  // null but on the hidden key's removal frame
    DiagnosticRemoveMidFrame(state, entry, bridge::RunDlssStage(*entry.d3d11_bridge, *entry.context, decision.work, frame, now), gate.Get());
  }
  NoteLatchIfTripped(&state, entry.context.get());
}

// Before the original evaluate: NR before upscaling through the hand-off; DLSS reads the bridge's shared RGBA16F texture as Color for this
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
    entry.context->NoteGameDeviceRemoved(removed, now);  // the latch window decides; nothing crosses again
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

// The process's first game DLSS evaluate, once, at INFO, for remote testers. Lock-free: the device by its mark or its list's state.
// NgxBridge calls this ahead of Vulkan's close guard, so it never throws; a failure logs nothing.
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

// NR at Present runs natively on this Vulkan device (the native route, this frame), so the effect events reach its native context (RunNative).
bool NativePresent(const DeviceEntry& entry) {
  return entry.vk_native_present && entry.vk_dlss;
}

// The frame's two early-marker facts, from the present event's read of the window (both frame sites).
void NoteEarlyMarker(const addon::RenderedMarker& marker, addon::FrameFacts* facts) {
  facts->marker_rendered_early = marker.Rendered();
  facts->early_motion_extent = marker.extent;
}

// The device entry whose primary swap chain presents through `runtime`, when `cmd_list` is that
// frame's immediate list. Effects another add-on renders on a game command list are ignored.
// A Vulkan device whose NR at Present runs natively has no bridged context: its native one is reached instead.
DeviceEntry* FindFrameEntry(AddonState* state, api::effect_runtime* runtime, api::command_list* cmd_list) {
  const auto found = state->devices.find(runtime->get_device());
  if (found == state->devices.end()) return nullptr;
  [[maybe_unused]] auto& [device, entry] = *found;
  if (entry.before_fg.outer_owns_frame) return nullptr;
  if ((!entry.context && !entry.helper.HasClient() && !NativePresent(entry)) || entry.runtime != runtime || entry.queue == nullptr) return nullptr;
  if (cmd_list != entry.queue->get_immediate_command_list()) return nullptr;
  return &entry;
}

// Runs NR through `host` for this frame, or skips it with a one-time WARN if ReShade could not
// give the presenting queue an immediate command list.
void RunOrWarnOnce(AddonState* state, DeviceEntry& entry, ReshadeFrameHost* host,
                   D3D12_RESOURCE_STATES entry_state, addon::TriggerPoint point,
                   ID3D12Resource* launchpad_motion = nullptr) {
  if (host->Valid()) {
    const auto result = entry.context->Run(host, entry_state, point, launchpad_motion);
    if (result.nr_applied) entry.before_fg.activity.passes += result.passes_run;
    if (result.nr_applied && BeforeFgTrace() && addon::streamline::Installed()) present_counts.nr_passes.fetch_add(result.passes_run, std::memory_order_relaxed);
    return;
  }
  if (!state->warned_no_immediate_list) {
    state->warned_no_immediate_list = true;
    nr::Log(nr::LogLevel::WARN, "ReShade gave no immediate command list for this queue; NR is skipped until it does");
  }
}

// One Vulkan image as the bridge describes it, from ReShade's description (shared with the helper front).
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
    // A delete needs the runtime's context current: while it is not, the names wait for the next valid frame.
    addon::ReshadeGlHost host(entry.gl_bridge->Gl(), entry.queue);
    entry.gl_bridge->ReleaseMask(host);
  }
}

// Native NR at Present is about to load on `device` (its context's hook, right before the Session's load, so a crash inside NGX's
// initialisation on the game's device is latched too): the pending marker goes on disk, unless it is there already or native NR has proved itself.
void NoteNativePresentLoad(api::device* device) {
  if (g_state->vk_native_marker_done || g_state->vk_native_marker_pending.load(std::memory_order_relaxed)) return;
  addon::WriteLatchMarker(g_state->vk_native_marker, "native Vulkan NR at Present started; waiting for it to run for a few seconds");
  g_state->vk_native_marker_owner = device;
  g_state->vk_native_frames = 0u;
  g_state->vk_native_marker_pending.store(true, std::memory_order_release);
}

// Native NR at Present ended cleanly on the marker's device (its Session is OFF: drained, or never loaded; or FAILED: NR runs no frame
// there until a retry loads it again): the marker goes. It is written again if native NR loads again before it has proved itself.
void ClearNativePresentMarker(api::device* device, nr::SessionState native_state) {
  if (!g_state->vk_native_marker_pending.load(std::memory_order_relaxed) || g_state->vk_native_marker_owner != device
      || (native_state != nr::SessionState::OFF && native_state != nr::SessionState::FAILED)) {
    return;
  }
  g_state->vk_native_marker_pending.store(false, std::memory_order_release);
  g_state->vk_native_marker_owner = nullptr;
  addon::DeleteLatchMarker(g_state->vk_native_marker);
}

void NoteNativePresentFrame(api::device* device) {
  if (g_state->vk_native_marker_pending.load(std::memory_order_relaxed) && g_state->vk_native_marker_owner == device
      && ++g_state->vk_native_frames >= NATIVE_MARKER_FRAMES) {
    g_state->vk_native_marker_pending.store(false, std::memory_order_release);
    g_state->vk_native_marker_done = true;
    addon::DeleteLatchMarker(g_state->vk_native_marker);
    nr::Logf(nr::LogLevel::INFO, "Vulkan: native NR at Present ran {} frames; its start marker is cleared", NATIVE_MARKER_FRAMES);
  }
}

// NR at `point` natively on a Vulkan device's own VkDevice (the native route), in a command buffer of Uplift's own submitted to the effect
// runtime's queue (ReShade's immediate command list is never recorded into, only flushed before Uplift submits). `usage`: the
// back buffer's state in the calling event (PRESENT at the present event, RENDER_TARGET in the effects), as RunBridged's. `launchpad`: this frame's
// UPLIFT_MV (the Uplift technique's event); none elsewhere. With the add-on's lock held; the only CPU wait is the capped one for a second present queue.
void RunNative(DeviceEntry& entry, api::device* device, addon::TriggerPoint point, vk::Usage usage, api::resource_view launchpad = {0u}) {
  if (point == addon::TriggerPoint::NONE || !entry.vk_dlss || !entry.vk_dlss->FrameReady() || entry.queue == nullptr) return;
  addon::VkDlssContext& native = *entry.vk_dlss;
  api::command_queue* const queue = entry.queue;
  addon::VkPresentQueues queues = {
      .queue = addon::VulkanHandleOf<VkQueue>(queue->get_native()),
      .flush_effects = [queue] { queue->flush_immediate_command_list(); },
  };
  // As in RunBridged, Uplift's submission is ordered after the game's frame only on the effect queue, so a second present queue is flushed
  // and waited for on the CPU first (2 s; a timeout skips this frame's NR).
  std::optional<addon::ReshadeVkHost> present_host;
  if (entry.present_queue != nullptr && entry.present_queue != queue) {
    if (!entry.vk_second_queue_logged) {
      entry.vk_second_queue_logged = true;
      nr::Log(nr::LogLevel::INFO, "Vulkan: the game presents from another queue than ReShade's effects; Uplift waits for it on the CPU before NR");
    }
    present_host.emplace(device, entry.present_queue, native.QueueSubmit());
    queues.flush_present_queue = [&present_host](VkFence fence) { return present_host->FlushWithFence(fence); };
  }
  addon::VkLaunchpadMotion motion;
  if (launchpad.handle != 0u) {
    const api::resource_desc description = device->get_resource_desc(device->get_resource_from_view(launchpad));
    motion = {
        .view = addon::VulkanHandleOf<VkImageView>(launchpad.handle),
        .size = {description.texture.width, description.texture.height},
        .format = vk::VkFormatOf(static_cast<DXGI_FORMAT>(description.texture.format)),
    };
  }
  const auto result = native.RunPresent(point, usage, queues, motion, std::chrono::steady_clock::now());
  if (result.nr_applied) {
    NoteNativePresentFrame(device);
    entry.before_fg.activity.passes += result.passes_run;
  }
  if (usage == vk::Usage::PRESENT) {
    // Only a semaphore orders a present after earlier work on the queue. ReShade's final flush waits on the game's semaphores and signals the
    // one the present waits on, but skips an empty immediate list (effects off, no techniques): one global barrier through ReShade's API makes it run.
    addon::ReshadeVkHost(device, queue, nullptr).Barrier(VK_NULL_HANDLE, vk::Usage::GENERAL, vk::Usage::GENERAL);
  }
}

// A VulkanNr pick of Direct3D 12 or Helper while the stored stage is a DLSS one moves it to Present, as a click on
// Present (the caller saves it). True when it moved.
bool MoveStageForVkRoute(ui::Settings* settings, ui::VulkanNrMode before) {
  if (!addon::VkRoutePickMovesStage(before, settings->vulkan_nr, ui::SourcePickOf(*settings, true))) return false;
  ui::SetSourcePick(settings, ui::SourcePick::PRESENT, true);
  nr::Logf(nr::LogLevel::INFO, "Vulkan NR is {}: the NR stage moves to Present (only native Vulkan NR runs inside the game's DLSS)",
           (settings->vulkan_nr == ui::VulkanNrMode::DIRECT3D_12 ? "Direct3D 12" : "Helper"));
  return true;
}

// Route picks and settings publication use the same owners as the panel.
void E2eLiveSettings(AddonState& state, DeviceEntry& entry, api::device* device) {
  const auto now = std::chrono::steady_clock::now();
  ui::Settings next = state.e2e.Poll(state.settings, state.config, entry.context.get(), entry.vk_dlss.get(), state.front, device, now);
  // A VulkanNr change applies as the panel's pick does, and the stage it moves is written back, as the panel's save writes it.
  if (entry.vulkan && MoveStageForVkRoute(&next, state.settings.vulkan_nr)) {
    state.config.Set(addon::E2E_SOURCE_KEY, std::to_string(static_cast<uint32_t>(next.source)));
    state.config.Set(addon::E2E_PRE_UPSCALE_KEY, std::to_string(static_cast<uint32_t>(next.pre_upscale)));
  }
  if (next == state.settings) return;
  state.e2e.FinishHold(next, entry.vk_dlss.get(), now);
  state.settings = next;
  PublishSettings(state);
}

// Vulkan bridges that a retry, or a move to the native or the helper route, tore down go once the game's queue has passed the fences behind
// their imports. So do the helper clients a route change left; a client holds no device of its own, so it goes under the lock.
void FreeRetiredVulkanBridges(DeviceEntry& entry, api::device* device, std::vector<std::unique_ptr<bridge::VkBridge>>* after_unlock) {
  for (auto retiring = entry.retiring_vk.begin(); retiring != entry.retiring_vk.end();) {
    addon::ReshadeVkHost host(device, entry.queue, (*retiring)->VulkanDevice().vkQueueSubmit);
    if (entry.queue != nullptr && (*retiring)->FreeRetired(host)) {
      after_unlock->push_back(std::move(*retiring));
      retiring = entry.retiring_vk.erase(retiring);
    } else {
      ++retiring;
    }
  }
  std::erase_if(entry.retiring_clients, [&entry, device](const std::unique_ptr<client::VkClient>& retiring) {
    addon::ReshadeVkHost host(device, entry.queue, retiring->VulkanDevice().vkQueueSubmit);
    return entry.queue != nullptr && retiring->FreeRetired(host);
  });
}

// NR at `point` for a bridged device. Copies and hand-offs wrap DeviceContext::Run on the private device, with the add-on's lock held. Nothing here waits
// on the CPU for the GPU, except a Vulkan device's capped waits (VkBridge::WAIT_CAP_MS: the present queue, and the CPU-ordered path's). `launchpad_motion`: this
// frame's UPLIFT_MV handle, from the Uplift technique's event; 0 elsewhere. `entry_state`: the Vulkan back buffer's
// state in the calling event (PRESENT at the present point, RENDER_TARGET in the effects); unused elsewhere. `frame`: an OpenGL device's frame, FB0 at the
// present point and ReShade's intermediate (the event's rtv resource) at the technique and after the effects; unused elsewhere.
void RunBridged(DeviceEntry& entry, api::device* device, addon::TriggerPoint point, uint64_t launchpad_motion, vk::Usage entry_state,
                api::resource frame = {0u}, vk::PresentRelayHost* outer_host = nullptr, const vk::ImageInfo* outer_image = nullptr, uint32_t* completed_passes = nullptr) {
  if (point == addon::TriggerPoint::NONE || !entry.context->FrameReady()) return;
  const bridge::Recorder record = [&entry, point](ID3D12GraphicsCommandList* list, const bridge::BridgeTargets& targets) {
    return bridge::RecordNr(*entry.context, point, list, targets);
  };
  if (entry.gl_bridge) {
    // The present event checks that its queue is the runtime's; the effect events have no present queue, only the current context to check.
    addon::ReshadeGlHost host(entry.gl_bridge->Gl(), entry.queue, (entry_state == vk::Usage::PRESENT ? entry.present_queue : nullptr));
    entry.gl_bridge->Run(host, addon::GlImageInfo(device, frame), addon::GlImageInfo(device, {launchpad_motion}), record);
    return;
  }
  if (entry.vk_bridge) {
    const PFN_vkQueueSubmit submit = entry.vk_bridge->VulkanDevice().vkQueueSubmit;
    addon::ReshadeVkHost host(device, entry.queue, submit);
    // The game presents from a queue other than the effect runtime's. Uplift's submissions are ordered after the game's frame
    // only by submission order on the runtime's queue, so the bridge flushes the present queue and waits for it on the CPU (2 s; a timeout skips this
    // frame's NR) inside Run, once per frame, after its own skip checks and before the copy in.
    std::optional<addon::ReshadeVkHost> present_host;
    if (!outer_host && entry.present_queue != nullptr && entry.queue != nullptr && entry.present_queue != entry.queue) {
      if (!entry.vk_second_queue_logged) {
        entry.vk_second_queue_logged = true;
        nr::Log(nr::LogLevel::INFO,
                "Vulkan: the game presents from another queue than ReShade's effects; Uplift waits for it on the CPU before NR");
      }
      entry.vk_bridge->NoteSecondQueue();
      present_host.emplace(device, entry.present_queue, submit);
    }
    // DLSS's motion vectors copied in the game's frame for this present, when the native context made one (Auto's order: DLSS first);
    // otherwise the Uplift technique's UPLIFT_MV, as before.
    const vk::NrImage dlss_motion = (entry.vk_dlss ? entry.vk_dlss->PresentMotion() : vk::NrImage{});
    const bool motion_is_dlss = (dlss_motion.image != VK_NULL_HANDLE);
    const vk::ImageInfo motion = (motion_is_dlss ? vk::ImageInfo{.image = dlss_motion.image,
                                                                 .size = {dlss_motion.ngx.Resource.ImageViewInfo.Width, dlss_motion.ngx.Resource.ImageViewInfo.Height},
                                                                 .format = DXGI_FORMAT_R16G16_FLOAT,
                                                                 .samples = 1u}
                                                 : VulkanImageInfo(device, {launchpad_motion}));
    if (outer_host && outer_image) {
      if (motion_is_dlss) outer_host->Register(motion.image, motion.size);
      if (entry.vk_bridge->Run(*outer_host, *outer_image, entry_state, motion, record, nullptr, motion_is_dlss) && completed_passes) {
        const auto status = entry.context->Status();
        for (uint32_t i = 0; i < status.group_count; ++i) {
          if (status.groups[i].stage == addon::Stage::PRESENT && status.groups[i].nr_applied) *completed_passes = status.groups[i].passes_run;
        }
      }
    } else {
      entry.vk_bridge->Run(host, VulkanImageInfo(device, entry.back_buffer), entry_state, motion, record, (present_host ? &*present_host : nullptr), motion_is_dlss);
    }
    return;
  }
  if (entry.d3d10_bridge) {
    entry.d3d10_bridge->Run(reinterpret_cast<ID3D10Resource*>(entry.back_buffer.handle),
                            reinterpret_cast<ID3D10Resource*>(launchpad_motion), record);
  } else {
    // At Present, this frame's ring slot of DLSS's vectors (copied in the game's evaluate) comes before Launchpad's.
    entry.d3d11_bridge->Run(reinterpret_cast<ID3D11Resource*>(entry.back_buffer.handle),
                            reinterpret_cast<ID3D11Resource*>(launchpad_motion), record,
                            /*dlss_motion=*/(entry.context->CurrentPlacement() == addon::Placement::PRESENT
                                             || entry.context->RunsAt(addon::Stage::PRESENT)));  // split chain: a later group at Present too
  }
}

// A retained UPLIFT_MASK variable does not prove that an enabled effect still writes it; MaskEffectRunning checks the writer.
using addon::MaskEffectRunning;

void OnInitCommandQueue(api::command_queue* queue) {
  try {
    if (!addon::streamline::Installed()) return;
    const auto graphics = queue->get_device()->get_api();
    if (graphics != api::device_api::vulkan && graphics != api::device_api::d3d12) return;
    const std::unique_lock lock(g_state->queue_mutex);
    g_state->queues[queue->get_native()] = {queue, queue->get_device()};
  } UPLIFT_CATCH("before_fg_queue", )
}

void OnDestroyCommandQueue(api::command_queue* queue) {
  try {
    {
      const std::unique_lock lock(g_state->queue_mutex);
      g_state->queues.erase(queue->get_native());
    }
    api::device* const queue_device = queue->get_device();
    if (queue_device->get_api() == api::device_api::d3d9) {
      // A Direct3D 9 Reset raises this, not destroy_device. The front releases the client's DEFAULT-pool
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
    const HoldsLockForNr holds;  // the teardown's unload reaches the snippet's Shutdown1 on this thread, under the lock
    auto* const native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    for (auto& [device, entry] : g_state->devices) {
      if (!entry.context) continue;
      if (entry.context->PresentQueue() != native_queue) {
        entry.context->ForgetQueue(native_queue);  // tokens submitted there are never signalled on it again
        continue;
      }
      // The frame fence is signalled on this queue: unload synchronously while it still exists.
      entry.context->Teardown();
      // A removal first seen only at teardown still decides the latch,
      // checked before the context that just decided it is destroyed below.
      NoteLatchIfTripped(g_state, entry.context.get());
      entry.core_hold = entry.context->CoreHold();  // the next context on this device starts from it
      entry.context.reset();
      entry.queue = nullptr;
      g_state->claim.Forget(device);
    }
  }
  UPLIFT_CATCH("destroy_command_queue", )
}

void OnDestroyDevice(api::device* device) {
  try {
    // A D3D11 device's bridge goes after the lock. It releases the private device ReShade
    // returned, whose destroy_device comes back into this handler on this thread.
    std::unique_ptr<bridge::D3D11Bridge> d3d11_bridge;
    std::unique_ptr<bridge::D3D10Bridge> d3d10_bridge;  // goes after the lock too (its relay's proxy)
    std::unique_ptr<bridge::VkBridge> vk_bridge;        // the same, for a Vulkan device's private D3D12 device
    std::unique_ptr<bridge::GlBridge> gl_bridge;        // and for an OpenGL context's
    std::vector<std::unique_ptr<bridge::VkBridge>> retiring_vk;  // Vulkan bridges a retry tore down, still behind their fences
    std::unique_ptr<addon::VkDlssContext> vk_dlss;      // abandoned under the lock, destroyed after it (no Vulkan or NGX call)
    {
      const std::unique_lock lock(g_state->mutex);
      const HoldsLockForNr holds;  // as destroy_command_queue's
      const auto found = g_state->devices.find(device);
      if (found != g_state->devices.end() && found->second.vk_dlss) {
        // The vkDestroyDevice detour did not tear the native context down (it moves it out when it does). ReShade has already
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
        // vkDestroyDevice frees the device's pools without destroy_command_list, so its buffers' states (and their tokens) go
        // here; a later device at this address, reusing a buffer handle, never inherits them.
        addon::ForgetVkListStates(device);
      }
      if (device->get_api() == api::device_api::d3d9 || (found != g_state->devices.end() && found->second.helper.HasClient())) {
        // Every Direct3D 9 device, also one that never presented (the 9Ex marker's rule); an entry's clients are released. It hands
        // back a Direct3D 10 client to destroy after the lock; a Direct3D 9 device has none (D3D10 devices here go through D3D10Bridge).
        // A Vulkan device's helper client too (FreeVulkan only, ReShade has already dropped the device's queues).
        static_cast<void>(g_state->front->DestroyDevice((found != g_state->devices.end() ? &found->second.helper : nullptr), device));
      }
      if (found != g_state->devices.end() && found->second.d3d11_bridge) {
        // Before Teardown, so a wedged private queue drains during Teardown's own wait instead of
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
        // Teardown() first, so a removal first seen only here still decides
        // the latch while Status() can still be read -- the erase below would otherwise destroy this
        // context (its destructor tears it down the same way, but Status() is gone with it by then).
        found->second.context->Teardown();
        NoteLatchIfTripped(g_state, found->second.context.get());
      }
      if (device->get_api() == api::device_api::d3d12) {
        // Uplift's mark goes with the ReShade device (ReShade removes its own the same way, after this event); a no-op on a device never marked.
        reinterpret_cast<ID3D12Device*>(device->get_native())->SetPrivateData(addon::UPLIFT_RESHADE_DEVICE_GUID, 0u, nullptr);
      }
      if (device->get_api() == api::device_api::d3d11) {
        addon::MarkD3D11Device(reinterpret_cast<ID3D11Device*>(device->get_native()), nullptr);  // the mark goes with the ReShade device
      }
      if (found != g_state->devices.end()) {
        g_state->bridge.NoteD3D11Watch(found->second.d3d11_watch_counted, false);
      }
      if (found != g_state->devices.end() && found->second.vk_bridge) {
        // ReShade has already deleted its queues and dropped this device from its dispatch map, so no Vulkan call through
        // its layer may follow. An image goes only through ReShade's destroy_resource (the host, with no queue, does exactly that); memory,
        // semaphores and fences through functions the layer does not intercept. After Stop and Teardown, as the D3D bridges.
        addon::ReshadeVkHost host(device, nullptr, found->second.vk_bridge->VulkanDevice().vkQueueSubmit);
        found->second.vk_bridge->FreeVulkan(host);
      }
      if (found != g_state->devices.end() && found->second.gl_bridge) {
        // On OpenGL, ReShade makes the share group's last context current only for its own teardown, so Uplift makes no GL call here at all: its
        // names die with the share group. After Stop and Teardown, as the D3D bridges.
        found->second.gl_bridge->ForgetGl();
      }
      if (found != g_state->devices.end()) {
        // A retry's torn-down Vulkan bridges free what is left of their imports as the live one does (no queue, the device is idle).
        for (std::unique_ptr<bridge::VkBridge>& retiring : found->second.retiring_vk) {
          addon::ReshadeVkHost host(device, nullptr, retiring->VulkanDevice().vkQueueSubmit);
          retiring->FreeVulkan(host);
        }
        retiring_vk = std::move(found->second.retiring_vk);
        for (std::unique_ptr<client::VkClient>& retiring : found->second.retiring_clients) {  // the same for helper clients a route change left
          addon::ReshadeVkHost host(device, nullptr, retiring->VulkanDevice().vkQueueSubmit);
          retiring->FreeVulkan(host);
        }
      }
      if (device->get_api() == api::device_api::vulkan) {
        // The pending marker's other end: every Vulkan device, also one that never presented. The hook's own lock, never this one's.
        vk::DeviceHook::NoteDestroyed(reinterpret_cast<VkDevice>(device->get_native()));
      }
      if (found != g_state->devices.end()) {
        d3d11_bridge = std::move(found->second.d3d11_bridge);  // the context, erased below, goes first
        d3d10_bridge = std::move(found->second.d3d10_bridge);
        vk_bridge = std::move(found->second.vk_bridge);
        gl_bridge = std::move(found->second.gl_bridge);
      }
      // Erasing destroys the entry's DeviceContext (Teardown() above already made that a no-op).
      {
        const std::unique_lock queue_lock(g_state->queue_mutex);
        std::erase_if(g_state->queues, [device](const auto& item) { return item.second.device == device; });
      }
      g_state->devices.erase(device);
      g_state->claim.Forget(device);
      g_state->helper_claim.Forget(device);
      g_state->bridge.DeviceDestroyed(device);  // A device recreated at this address does not start as "DLSS seen"
    }
  }
  UPLIFT_CATCH("destroy_device", )
}

// The add-on's lock is taken by a Vulkan command-list event only for a buffer that holds Uplift tokens, which only a game
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
      // A stale state for a reused handle (its pool was destroyed unannounced) hands its tokens to the device's next present.
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
    list_state->reset_epoch = g_state->submit_epochs.AtReset();
  }
  UPLIFT_CATCH("reset_command_list", )
}

void OnExecuteCommandList(api::command_queue* queue, api::command_list* cmd_list) {
  try {
    if (cmd_list->get_device()->get_api() == api::device_api::vulkan) {
      // Before the native submit, under the queue's lock in ReShade. Never a queue signal here: the tokens are the buffer's events.
      addon::VkListState* const vk_list = addon::FindVkListState(reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cmd_list->get_native())));
      if (vk_list == nullptr) return;
      PollHooks();
      if (!vk_list->tokens.empty()) {
        const std::unique_lock lock(g_state->mutex);
        if (addon::VkDlssContext* const context = VkContextOf(vk_list->device)) {
          if (vk_list->executed) {
            context->Reexecuted(vk_list->tokens, vk_list->first_token, queue);  // submitted again without a new recording
          } else {
            context->Executed(vk_list->tokens, queue);  // split chain: the queue decides whether a later Present group may run beside it
          }
        }
      }
      vk_list->executed = true;
      return;
    }
    addon::ListState* const list_state = addon::FindListState(cmd_list);
    if (list_state == nullptr) return;
    PollHooks();
    // No stamp for a list of the same ExecuteCommandLists call as this thread's last submission: ReShade raises this event for every list of one
    // call before the single native call, so a Signal now could precede the submission of an earlier list in the same batch. Reset, evaluate and
    // present stamp instead. A list reset after that submission is not part of that call, so the call has returned and a Signal now follows it on
    // its queue: a thread that only executes lists is stamped here, rather than by age.
    if (addon::StampsAtExecute(t_unstamped_device != nullptr, list_state->reset_epoch, t_unstamped_epoch)) {
      StampThisThread(true);
    }
    const bool executed_again = (list_state->tokens.empty() && list_state->executed && list_state->first_token != 0u);
    if (list_state->tokens.empty() && !executed_again) return;
    const std::unique_lock lock(g_state->mutex);
    if (addon::DeviceContext* const context = ContextOf(list_state->device)) {
      auto* const native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
      const auto now = std::chrono::steady_clock::now();
      if (executed_again) {
        context->ResubmitTokens(list_state->first_token, native_queue, GetCurrentThreadId(), now);
      } else {
        context->SubmitTokens(list_state->tokens, native_queue, GetCurrentThreadId(), now);
      }
      t_unstamped_device = list_state->device;  // this thread stamps it at its next event
      t_unstamped_epoch = g_state->submit_epochs.AtSubmit();
    }
    list_state->tokens.clear();
    list_state->executed = true;
  }
  UPLIFT_CATCH("execute_command_list", )
}

void BeforeFgCreated(IDXGISwapChain* chain, IUnknown* device, void* inner) noexcept {
  try {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
      if (BeforeFgTrace()) nr::Log(nr::LogLevel::INFO, "Before FG DX12: returned swapchain has no D3D12 creation queue");
      return;
    }
    const auto reason = addon::BeforeFgDx12::Attach(chain, device, inner);
    if (BeforeFgTrace()) {
      if (!reason.empty()) nr::Logf(nr::LogLevel::INFO, "Before FG DX12: route rejected for outer={} reason={}", static_cast<void*>(chain), reason);
      nr::Logf(nr::LogLevel::INFO, "Before FG trace: game-facing DX12 swapchain={} creation queue={} ReShade swapchain={}",
               static_cast<void*>(chain), static_cast<void*>(queue.Get()), inner);
    }
  } UPLIFT_CATCH("before_fg_create", )
}

void BeforeFgRetry(IDXGISwapChain* chain) noexcept {
  try { if (auto route = addon::BeforeFgDx12::Find(chain)) route->Retry(); } UPLIFT_CATCH("before_fg_retry", )
}

void BeforeFgDxgi(IDXGISwapChain* chain, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) noexcept {
  try {
    const auto real = NextBeforeFgTraceSample(present_counts.dxgi_real);
    const auto route = addon::BeforeFgDx12::Find(chain);
    {
      const std::unique_lock lock(g_state->mutex);
      const HoldsLockForNr holds;
      for (auto& [device, entry] : g_state->devices) {
        if (entry.before_fg.early_dxgi == chain && entry.before_fg.early_thread == GetCurrentThreadId()) {
          ++entry.before_fg.activity.real;
          entry.before_fg.early_confirmed = true;
          entry.before_fg.outer_owns_frame = false;
          break;
        }
        if (!route || !route->Inner() || entry.selector.Primary() != route->Inner()) continue;
        ++entry.before_fg.activity.real;
        entry.before_fg.outer_inner = route->Inner();
        entry.before_fg.outer_present_passes = 0u;
        if (!g_state->settings.enabled || addon::streamline::Mode() == addon::streamline::FgMode::OFF || !entry.before_fg.outer_owns_frame || !entry.before_fg.outer_config || !entry.context || g_state->settings.fg_present != ui::Settings::FgPresent::BEFORE) break;
        if (parameters && (parameters->DirtyRectsCount || parameters->pScrollRect || parameters->pScrollOffset)) {
          entry.before_fg.note = {BeforeFgNote::PARTIAL_PRESENT};
          break;
        }
        if ((flags & DXGI_PRESENT_DO_NOT_WAIT) != 0) {
          entry.before_fg.note = {BeforeFgNote::NONBLOCKING_PRESENT};
          break;
        }
        const auto result = route->Record(chain, reinterpret_cast<ID3D12Device*>(device->get_native()), *entry.context, entry.before_fg.outer_config->settings,
                                    entry.before_fg.outer_config->facts, std::chrono::steady_clock::now());
        entry.before_fg.activity.passes += result.passes;
        entry.before_fg.outer_present_passes = result.passes;
        if (result.passes) {
          if (BeforeFgTrace()) present_counts.nr_passes.fetch_add(result.passes, std::memory_order_relaxed);
          entry.before_fg.note = {BeforeFgNote::DX12_PRESENT};
        } else if (!result.reason.empty()) {
          entry.before_fg.note = {BeforeFgNote::DETAIL, std::string(result.reason)};
        }
        break;
      }
    }
    if (!real) return;
    {
      const std::shared_lock lock(g_state->mutex);
      for (const auto& [device, entry] : g_state->devices) {
        nr::Logf(nr::LogLevel::INFO, "Before FG trace: binding route={} inner={} primary={} owned={} config={} mode={} reason=[{}]",
                 route != nullptr, route ? route->Inner() : nullptr, entry.selector.Primary(), entry.before_fg.outer_owns_frame, entry.before_fg.outer_config.has_value(),
                 static_cast<int>(g_state->settings.fg_present), entry.before_fg.note.Text());
      }
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain3> current;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&current)))) return;
    nr::Logf(nr::LogLevel::INFO, "Before FG trace: outer DXGI swapchain={} index={} flags={:#x} calls={} ReShade events={} NR Present passes={}",
             static_cast<void*>(chain), current->GetCurrentBackBufferIndex(), flags, *real, present_counts.reshade.load(), present_counts.nr_passes.load());
  } UPLIFT_CATCH("before_fg_dxgi", )
}

VkImageUsageFlags BeforeFgVkUsage(VkDevice device, const VkSwapchainCreateInfoKHR& info) noexcept {
  try {
    if (info.imageArrayLayers != 1u || (info.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
        || info.presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR || info.presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) return 0u;
    for (auto* ext = static_cast<const VkBaseInStructure*>(info.pNext); ext; ext = ext->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT
          && reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(ext)->fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT) return 0u;
    }
    const auto record = vk::DeviceHook::Find(device);
    const auto* loader = vk::Loader::Get();
    if (!record || !record->instance || !loader || !loader->get_instance_proc_addr) return 0u;
    const auto capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        loader->get_instance_proc_addr(record->instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    VkSurfaceCapabilitiesKHR caps{};
    constexpr VkImageUsageFlags needed = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!capabilities || capabilities(record->physical, info.surface, &caps) != VK_SUCCESS || (caps.supportedUsageFlags & needed) != needed) return 0u;
    return needed;
  } UPLIFT_CATCH("before_fg_vk_usage", 0u)
}

void BeforeFgVkCreated(VkDevice device, VkSwapchainKHR chain, const VkSwapchainCreateInfoKHR& info, void* inner) noexcept {
  try {
    const std::unique_lock lock(g_state->mutex);
    g_state->before_fg_vk.Created(device, chain, info, inner);
    if (BeforeFgTrace()) {
      nr::Logf(nr::LogLevel::INFO, "Before FG trace: Vulkan application swapchain={} ReShade swapchain={} format={} usage={:#x} size={}x{}",
               reinterpret_cast<void*>(chain), inner, static_cast<int>(info.imageFormat), info.imageUsage, info.imageExtent.width, info.imageExtent.height);
    }
  } UPLIFT_CATCH("before_fg_vk_create", )
}
void BeforeFgVkImages(VkDevice device, VkSwapchainKHR chain, uint32_t count, const VkImage* images) noexcept {
  try { const std::unique_lock lock(g_state->mutex); g_state->before_fg_vk.Images(device, chain, count, images); } UPLIFT_CATCH("before_fg_vk_images", )
}
void BeforeFgVkDestroyed(VkDevice device, VkSwapchainKHR chain) noexcept {
  try { const std::unique_lock lock(g_state->mutex); g_state->before_fg_vk.Destroyed(device, chain); } UPLIFT_CATCH("before_fg_vk_destroy", )
}
void BeforeFgVkAcquired(VkDevice device, VkSwapchainKHR chain, uint32_t index) noexcept {
  try { const std::unique_lock lock(g_state->mutex); g_state->before_fg_vk.Acquired(device, chain, index); } UPLIFT_CATCH("before_fg_vk_acquire", )
}

addon::BeforeFgVk::Result RecordOuterVulkanBridge(DeviceEntry& entry, api::device* device, api::command_queue* game_queue, VkQueue queue,
                                                 const VkPresentInfoKHR& app, addon::streamline::VkForward& forward) {
  const auto& settings = g_state->settings;
  const auto frame = g_state->before_fg_vk.Prepare(queue, app, settings.encoding);
  if (!frame.reason.empty()) return {.reason = frame.reason};
  const auto bridge_host = g_state->before_fg_vk.BridgeHost(queue, frame);
  if (!bridge_host.host) return {.reason = bridge_host.reason};
  auto* host = bridge_host.host;
  if (!host->BeginFrame({app.pWaitSemaphores, app.waitSemaphoreCount})) return {.reason = uplift::text::REAL_FRAME_VK_RECORDING_LINE};
  const auto record = [&]() -> addon::BeforeFgVk::Result {
    struct FinishRelay {
      vk::PresentRelayHost& host;
      addon::streamline::VkForward& forward;
      ~FinishRelay() { forward.failure = host.FinishFrame(); }
    } finish{*host, forward};
    host->Register(frame.image.image, frame.image.size);
    const auto now = std::chrono::steady_clock::now();
    if (entry.before_fg.helper_frame) {
      auto helper_frame = *entry.before_fg.helper_frame;
      helper_frame.now = now;
      helper_frame.runtime = entry.runtime;
      helper_frame.queue = game_queue;
      helper_frame.present_queue = game_queue;
      helper_frame.marker = {};
      helper_frame.early_marker = {};
      helper_frame.back_buffer = {addon::ReshadeHandleOf(frame.image.image)};
      helper_frame.outer_vulkan_host = host;
      helper_frame.outer_vulkan_image = frame.image;
      helper_frame.outer_color_space = frame.space;
      const auto before = entry.helper.before_fg_passes;
      g_state->front->Present(entry.helper, helper_frame);
      return {.passes = static_cast<uint32_t>(entry.helper.before_fg_passes - before)};
    }
    if (!entry.vk_bridge || !entry.context || !entry.before_fg.outer_config) return {.reason = uplift::text::VK_BRIDGE_NOT_READY_LINE};
    const addon::BeforeFgFrame& outer = *entry.before_fg.outer_config;
    if (entry.vk_dlss && entry.before_fg.native_config) {
      const addon::VkPresentFrame native_frame{.back_buffer = frame.image, .color_space = frame.space};
      entry.vk_dlss->BeginOuterFrame(forward.native_tick, game_queue, entry.before_fg.native_config->settings,
                                     entry.before_fg.native_config->facts, entry.before_fg.native_config->facts.nr_allowed, entry.before_fg.outer_holds_nr, now, native_frame);
    }
    const auto bridge_frame = entry.vk_bridge->BeginFrame(*host, frame.image, settings.enabled, entry.context->NrState() != nr::SessionState::OFF, now);
    const std::string problem(bridge_frame.problem);
    const addon::TargetInfo target{.resource = bridge_frame.color, .color_space = frame.space, .problem = problem, .size = bridge_frame.size, .format = bridge_frame.format};
    const auto point = entry.context->BeginFrame(entry.vk_bridge->Queue(), outer.settings, outer.facts, target, false, now);
    uint32_t passes = 0;
    RunBridged(entry, device, point, 0u, vk::Usage::PRESENT, {}, host, &frame.image, &passes);
    return {.passes = passes};
  };
  const auto result = record();
  return host->Valid() ? result : addon::BeforeFgVk::Result{.reason = uplift::text::VK_COPYBACK_RECORDING_LINE};
}

void ReleaseUnclaimedOuterTick(const addon::streamline::VkForward* forward) noexcept {
  try {
    const std::unique_lock lock(g_state->mutex);
    for (auto& [device, entry] : g_state->devices) {
      if (entry.before_fg.outer_tick == forward) entry.before_fg.outer_tick = nullptr;
    }
  } UPLIFT_CATCH("before_fg_tick_release", )
}

void BeforeFgVk(VkQueue queue, const VkPresentInfoKHR* info, addon::streamline::VkForward* forwarded) noexcept {
  try {
    const auto real = NextBeforeFgTraceSample(present_counts.vk_real);
    if (!info->pSwapchains || !info->pImageIndices) return;
    {
      const std::unique_lock lock(g_state->mutex);
      const HoldsLockForNr holds;
      if (info->swapchainCount == 1u) {
        const auto binding = g_state->before_fg_vk.Find(info->pSwapchains[0]);
        for (auto& [device, entry] : g_state->devices) {
          if (!entry.vulkan || !binding.device || device->get_native() != addon::ReshadeHandleOf(binding.device)) continue;
          if (entry.before_fg.early_vk_chain == info->pSwapchains[0] && entry.before_fg.early_vk_queue == queue && entry.before_fg.early_thread == GetCurrentThreadId()) {
            ++entry.before_fg.activity.real;
            entry.before_fg.early_confirmed = true;
            entry.before_fg.outer_owns_frame = false;
            break;
          }
          if (!binding.inner || binding.inner != entry.selector.Primary()) continue;
          ++entry.before_fg.activity.real;
          entry.before_fg.outer_inner = binding.inner;
          entry.before_fg.outer_present_passes = 0u;
          if (!g_state->settings.enabled || g_state->settings.fg_present != ui::Settings::FgPresent::BEFORE || addon::streamline::Mode() == addon::streamline::FgMode::OFF
              || !entry.before_fg.outer_owns_frame || (!entry.before_fg.outer_config && !entry.before_fg.helper_frame)) break;
          api::command_queue* game_queue = nullptr;
          {
            const std::shared_lock queue_lock(g_state->queue_mutex);
            const auto found = g_state->queues.find(addon::ReshadeHandleOf(queue));
            if (found != g_state->queues.end() && found->second.device == device) game_queue = found->second.queue;
          }
          if (!game_queue) {
            entry.before_fg.note = {BeforeFgNote::VK_QUEUE_UNTRACKED};
            break;
          }
          // The scope owns this receipt. The accepted primary inner consumes it, including an early-return path.
          entry.before_fg.outer_tick = forwarded;
          forwarded->release_tick.store(&ReleaseUnclaimedOuterTick, std::memory_order_release);
          const bool native = entry.vk_dlss && entry.before_fg.outer_config && entry.before_fg.outer_config->facts.vk_native_present;
          const auto result = native
              ? g_state->before_fg_vk.Record(queue, game_queue, *info, *entry.vk_dlss, entry.before_fg.outer_config->settings, entry.before_fg.outer_config->facts, entry.before_fg.outer_holds_nr, forwarded->native_tick, std::chrono::steady_clock::now())
              : RecordOuterVulkanBridge(entry, device, game_queue, queue, *info, *forwarded);
          if (forwarded->failure != VK_SUCCESS) {
            entry.before_fg.note = {BeforeFgNote::VK_RELAY_FAILED};
          } else if (result.passes) {
            if (native) NoteNativePresentFrame(device);
            entry.before_fg.activity.passes += result.passes;
            entry.before_fg.outer_present_passes = result.passes;
            if (BeforeFgTrace()) present_counts.nr_passes.fetch_add(result.passes, std::memory_order_relaxed);
            entry.before_fg.note = {BeforeFgNote::VK_PRESENT};
          } else if (!result.reason.empty()) {
            entry.before_fg.note = {BeforeFgNote::DETAIL, std::string(result.reason)};
          }
          break;
        }
      }
    }
    if (!real) return;
    {
      const std::shared_lock lock(g_state->mutex);
      for (const auto& [device, entry] : g_state->devices) {
        if (entry.vulkan) nr::Logf(nr::LogLevel::INFO, "Before FG trace: Vulkan primary={} owned={} native={} helper={} reason=[{}]",
                                  entry.selector.Primary(), entry.before_fg.outer_owns_frame, NativePresent(entry), entry.before_fg.helper_frame.has_value(), entry.before_fg.note.Text());
      }
    }
    for (uint32_t index = 0; index < info->swapchainCount; ++index) {
      nr::Logf(nr::LogLevel::INFO, "Before FG trace: outer Vulkan queue={} swapchain={} index={} waits={} calls={} ReShade events={} NR Present passes={}",
               reinterpret_cast<void*>(queue), reinterpret_cast<void*>(info->pSwapchains[index]), info->pImageIndices[index],
               info->waitSemaphoreCount, *real, present_counts.reshade.load(), present_counts.nr_passes.load());
    }
  } UPLIFT_CATCH("before_fg_vk", )
}

void BeforeFgResized(IDXGISwapChain* chain, UINT count, IUnknown* const* queues, void* inner) noexcept {
  try { if (auto route = addon::BeforeFgDx12::Find(chain)) route->Resize(count, queues, inner); } UPLIFT_CATCH("before_fg_resize", )
}

void BeforeFgColor(IDXGISwapChain* chain, DXGI_COLOR_SPACE_TYPE space) noexcept {
  try { if (auto route = addon::BeforeFgDx12::Find(chain)) route->ColorSpace(space); } UPLIFT_CATCH("before_fg_color", )
}

void BeforeFgInitDevice(api::device* device) {
  try {
    const auto graphics = device->get_api();
    if (graphics != api::device_api::d3d12 && graphics != api::device_api::vulkan) return;
    // A bridged game can create Uplift's private D3D12 device while running NR.
    if (t_holds_lock_for_nr || addon::streamline::Installed()) return;
    std::call_once(g_state->before_fg_start, [] {
      if (addon::host::PinReshade(reshade::internal::get_reshade_module_handle()) && addon::streamline::Start(g_state->self_module, {BeforeFgCreated, BeforeFgDxgi, BeforeFgVk, BeforeFgResized, BeforeFgColor, BeforeFgVkCreated, BeforeFgVkImages, BeforeFgVkDestroyed, BeforeFgVkAcquired, BeforeFgVkUsage, BeforeFgRetry, BeforeFgTrace()})) {
        nr::Log(nr::LogLevel::INFO, "Before FG: observing Streamline and ReShade presentation");
      } else {
        nr::Log(nr::LogLevel::WARN, uplift::text::BeforeFgNoteInfo(BeforeFgNote::HOOKS_UNAVAILABLE).line);
      }
    });
  } UPLIFT_CATCH("before_fg_init_device", )
}

void BeforeFgDestroyChain(api::swapchain* chain) noexcept {
  try {
    if (!addon::streamline::Installed()) return;
    const auto graphics = chain->get_device()->get_api();
    if (graphics != api::device_api::d3d12 && graphics != api::device_api::vulkan) return;
    const std::unique_lock lock(g_state->mutex);
    const auto found = g_state->devices.find(chain->get_device());
    if (found == g_state->devices.end() || found->second.selector.Primary() != chain) return;
    found->second.before_fg.Reset();
  } UPLIFT_CATCH("before_fg_destroy", )
}

void BeforeFgInitChain(api::swapchain* chain) { addon::streamline::NoteReshadeSwapchain(chain); }

bool BeforeFgMaskConflicts(DeviceEntry& entry, const ui::Settings& settings) {
  if (settings.mask == ui::MaskMode::AUTO && entry.runtime && entry.runtime->get_effects_state()) {
    const auto variable = entry.runtime->find_texture_variable(nullptr, uplift::text::UPLIFT_MASK_TEXTURE);
    return variable.handle && MaskEffectRunning(entry.runtime, variable);
  }
  return false;
}

std::string VulkanDlssReason(const AddonState& state, const DeviceEntry& entry);

addon::BeforeFgDecision DecideBeforeFgFor(AddonState& state, DeviceEntry& entry, api::device* device, api::swapchain* swapchain,
                                          const addon::BeforeFgInputs& before_fg_facts, addon::BeforeFgRoute route) {
  auto inputs = before_fg_facts;
  inputs.route = route;
  inputs.handoff_pending = route == addon::BeforeFgRoute::VK_HELPER
      ? entry.before_fg.helper_frame.has_value() : entry.before_fg.outer_config.has_value();
  inputs.hooks_installed = addon::streamline::Installed();
  if (route == addon::BeforeFgRoute::DX12) {
    inputs.measured_cadence = entry.context->HasFrameGenerationCadence();
    if (addon::BeforeFgActive(inputs) && inputs.early_confirmed) {
      inputs.early_dxgi_matches = entry.before_fg.early_dxgi == reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
      if (inputs.early_dxgi_matches) inputs.inside_outer_present = addon::streamline::InsideOuterPresent();
    }
  }
  if (addon::BeforeFgChecksOuterInputs(inputs)) {
    if (route != addon::BeforeFgRoute::VK_HELPER) {
      inputs.upscaler_live = state.bridge.Registry().UpscalerLive(
          device, route == addon::BeforeFgRoute::DX12 ? ngx_hooks::NgxApi::D3D12 : ngx_hooks::NgxApi::VULKAN);
    }
    inputs.mask_conflicts = BeforeFgMaskConflicts(entry, state.settings);
  }
  return addon::DecideBeforeFg(inputs);
}

// The NR runtime both of a device's contexts load (the bridge's and the native Vulkan one), and what keeps it from loading at all.
std::optional<std::filesystem::path> LocateSnippetPath(AddonState& state) {
  const std::string& configured = state.settings.snippet_path;
  // A hand-edited SnippetPath can be invalid UTF-8; it is treated like an unset one.
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
}

void NoteStaticBlock(AddonState& state, DeviceEntry& entry, const std::optional<std::filesystem::path>& snippet) {
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
}

void HashRuntimeOnce(AddonState& state, DeviceEntry& entry) {
  if (state.settings.enabled && !state.snippet_hashed && !entry.snippet_path.empty()) {
    state.snippet_hashed = true;
    nr::Logf(nr::LogLevel::INFO, "NR runtime file {} (SHA-256 {})", addon::Utf8FromPath(entry.snippet_path),
             addon::Sha256Hex(entry.snippet_path));
  }
}

// Why NR stays off on this device whatever the placement: a static block, or another NR producer.
std::string BlockedReason(AddonState& state, DeviceEntry& entry) {
  // nvngx_dlssnr.dll mapped from the file Uplift would load, which sits in the game's own folder: NVIDIA's DLSS does that on RTX 50 cards, and NR cannot run
  // on that copy (1.1.3 tried: the game crashed). Said plainly, whatever ForeignNr says. A copy mapped from any other file does not block: Uplift loads its own.
  entry.runtime_in_game_folder = false;
  const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  const nr::MappedRuntimeDecision* decision = nullptr;
  if (!entry.snippet_path.empty()) {
    // Decided again at once when the path changed or a Load ran since (so the card never contradicts a load that just happened).
    const uint64_t load_count = nr::SnippetLoadCount();
    if (entry.mapped_runtime_for != entry.snippet_path || entry.mapped_runtime_load_count != load_count
        || now - entry.mapped_runtime_checked >= MAPPED_RUNTIME_RECHECK_MS) {
      entry.mapped_runtime_load_count = load_count;
      entry.mapped_runtime = nr::DecideMappedRuntime(entry.snippet_path, nr::MappedRuntimeCopies(entry.snippet_path));
      entry.mapped_runtime_for = entry.snippet_path;
      entry.mapped_runtime_checked = now;
    }
    decision = &entry.mapped_runtime;
  }
  if (entry.static_block.empty() && !entry.snippet_path.empty() && !state.game_directory.empty()) {
    std::error_code same_error;
    if (decision != nullptr && decision->rule == nr::MappedRuntimeRule::SAME_FILE
        && std::filesystem::equivalent(decision->same_file.parent_path(), state.game_directory, same_error) && !same_error) {
      entry.runtime_in_game_folder = true;
      return std::string(ui::RUNTIME_COPY_BODY);
    }
  }
  // The same file mapped anywhere else: Snippet::Load refuses it whatever ForeignNr says, so the card says why whatever ForeignNr says.
  if (entry.static_block.empty() && decision != nullptr && decision->rule == nr::MappedRuntimeRule::SAME_FILE) {
    return std::format("{} ({})", uplift::text::RUNTIME_MAPPED_ELSEWHERE_LINE, addon::Utf8FromPath(decision->same_file));
  }
  std::optional<std::string> foreign;
  if (state.settings.foreign_nr == ui::ForeignNrMode::YIELD) {
    const std::optional<std::chrono::steady_clock::time_point> last_evaluated =
        state.bridge.Registry().LastEvaluated(ngx_hooks::FeatureKind::NEURAL_RENDERING);
    foreign = addon::ForeignNrProducer({
        .foreign_nr_evaluating = last_evaluated && now - *last_evaluated < addon::FOREIGN_NR_IDLE_MS,
        .renodx_marker = addon::IsEnvironmentMarkerSet(addon::RENODX_NR_MARKER),
    });
  }
  return (!entry.static_block.empty() ? entry.static_block : (foreign ? std::format("{} (ForeignNr = Yield)", *foreign) : std::string()));
}

// Whether an UPLIFT_MASK is running on this device, and why not (the note). Looked up once per present, by the first path
// that needs it. A declared UPLIFT_MASK variable also needs a technique of its effect still enabled.
std::pair<bool, std::string_view> MaskState(AddonState& state, DeviceEntry& entry,
                                            std::optional<std::pair<bool, std::string_view>>& mask_lookup) {
  if (!mask_lookup) {
    const api::effect_texture_variable variable =
        (state.settings.mask == ui::MaskMode::AUTO && entry.runtime != nullptr && entry.runtime->get_effects_state())
            ? entry.runtime->find_texture_variable(nullptr, uplift::text::UPLIFT_MASK_TEXTURE)
            : api::effect_texture_variable{0u};
    const bool declared = variable.handle != 0u;
    mask_lookup.emplace(declared && MaskEffectRunning(entry.runtime, variable),
                        (state.settings.mask != ui::MaskMode::AUTO) ? std::string_view()
                        : declared                                  ? std::string_view("NR mask: the UPLIFT_MASK effect is off")
                                                                    : std::string_view("NR mask: no effect writes UPLIFT_MASK"));
  }
  return *mask_lookup;
}

// What OnPresent hands a VkDeviceRouter: its locals the Vulkan block reads, and the two it writes for the code after it (by reference).
struct VkPresentInputs {
  api::device* device;
  api::swapchain* swapchain;
  std::chrono::steady_clock::time_point now;
  bool outer_ticked;
  const addon::BeforeFgInputs& before_fg_facts;
  const addon::RenderedMarker& early_marker;
  std::vector<std::unique_ptr<bridge::VkBridge>>& vk_after_unlock;
  std::optional<std::pair<bool, std::string_view>>& mask_lookup;
  bool& present_motion_wanted;
  bool& vk_dlss_stages_off;
};

// NR after DLSS natively on the game's Vulkan device. Source = Auto stays at Present on Vulkan, and the
// DLSS placements are the explicit choices, taken from the Present path (NrClaim) once the game's DLSS is seen. NR at
// Present runs natively too, on the native route (addon::ChooseVkNrRoute: VulkanNr, and what makes native NR impossible here), else on the private
// Direct3D 12 device (VkBridge) as before.
// One router per OnPresent call on a Vulkan device, constructed and destroyed under OnPresent's lock, holding no state between presents.
// Members set in Present() are scratch shared by its steps; anything that must persist lives in DeviceEntry or AddonState.
// Present() is called once per instance. The members keep the names OnPresent's locals had, so each step reads as the block did.
class VkDeviceRouter {
 public:
  VkDeviceRouter(AddonState& addon_state, DeviceEntry& device_entry, const VkPresentInputs& in)
      : state(addon_state), entry(device_entry), device(in.device), swapchain(in.swapchain), now(in.now), outer_ticked(in.outer_ticked),
        before_fg_facts(in.before_fg_facts), early_marker(in.early_marker), vk_after_unlock(in.vk_after_unlock), mask_lookup(in.mask_lookup),
        present_motion_wanted(in.present_motion_wanted), vk_dlss_stages_off(in.vk_dlss_stages_off) {}

  // The steps below, in order. True when OnPresent returns.
  [[nodiscard]] bool Present();

 private:
  // The native context's facts for this present: what native NR needs, a creation failure or a stop latched, the device-loss latch, the Direct3D 12
  // route's creation failure. True when the device was lost or its native NR abandoned (no route starts).
  [[nodiscard]] bool NoteNativeFacts();
  // The route, the DLSS stages, the helper's part and the one owner of NR on this device.
  void ChooseRoute();
  // The present through the helper front, as gitc-uplift.addon32 hosts it for a 32-bit Vulkan device (the client, the helper's FRAME, NR at the
  // present point; the effect events reach the front through FindFrameEntry). Two claims: the helper serves one device (`helper_claim`, shared with the
  // Direct3D 9 devices), and the in-process one keeps the helper's NR from running next to this device's native context at a DLSS placement (which takes
  // it once the helper has drained). `route_wants_nr` false drains the helper's NR.
  void PresentThroughHelper(bool route_wants_nr);
  // The route left the helper (or the placement left Present): drain it, then detach it. True when this present goes without NR.
  [[nodiscard]] bool LeaveHelper();
  // The native or helper route: the Direct3D 12 route's bridge and context go first.
  void StopBridgeOffRoute();
  // Whether the native context copies DLSS's vectors for the Present path this present.
  void DecidePresentMotion();
  // The native context, made once it is wanted.
  void EnsureNativeContext();
  // The native context's per-present work. True when OnPresent returns (NR ran natively at Present, or natively alone).
  [[nodiscard]] bool RunNativeContext();
  // NR at Present in Uplift's helper process.
  void RunHelperRoute();

  // OnPresent's locals, bound once.
  AddonState& state;
  DeviceEntry& entry;
  api::device* const device;
  api::swapchain* const swapchain;
  const std::chrono::steady_clock::time_point now;
  const bool outer_ticked;
  const addon::BeforeFgInputs& before_fg_facts;
  const addon::RenderedMarker& early_marker;
  std::vector<std::unique_ptr<bridge::VkBridge>>& vk_after_unlock;
  std::optional<std::pair<bool, std::string_view>>& mask_lookup;
  bool& present_motion_wanted;
  bool& vk_dlss_stages_off;
  // Scratch shared by the steps, set in the step that first decides it.
  VkDevice vk_device = VK_NULL_HANDLE;  // NoteNativeFacts
  addon::VkRouteChoice route;           // ChooseRoute, and the five below it
  bool native_route = false;
  bool helper_route = false;
  bool native_at_dlss_stage = false;
  bool helper_runs = false;
  addon::VkNrOwner owner = addon::VkNrOwner::NONE;
  bool replace_late_motion = false;  // DecidePresentMotion
};

bool VkDeviceRouter::Present() {
  if (NoteNativeFacts()) return true;
  ChooseRoute();
  if (LeaveHelper()) return true;
  StopBridgeOffRoute();
  DecidePresentMotion();
  EnsureNativeContext();
  if (entry.vk_dlss && RunNativeContext()) return true;
  if (native_route) return true;  // the native context could not start this present; the Direct3D 12 route takes over at the next one
  if (helper_runs) {
    RunHelperRoute();
    return true;
  }
  return false;
}

bool VkDeviceRouter::NoteNativeFacts() {
  vk_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device->get_native()));
  entry.vk_native_present = false;
  if (!entry.vk_native_checked) {
    entry.vk_native_checked = true;
    std::string needs = [&]() -> std::string {
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
    if (!needs.empty()) {
      entry.vk_native_block = addon::VkNativeBlock{addon::VkNativeBlock::Cause::NEEDS, std::move(needs)};
    }
  }
  // A creation failure on the game's device keeps NR at Present on the Direct3D 12 route for the rest of the session (logged once, with NGX's
  // result; no retry ping-pong between the routes). A context without Uplift's own command buffers cannot run NR at Present at all.
  if (entry.vk_dlss && !entry.vk_native_block) {
    if (std::optional<addon::VkNativeBlock> block = entry.vk_dlss->PresentBlock(); block && block->cause == addon::VkNativeBlock::Cause::CREATE_FAILURE) {
      nr::Logf(nr::LogLevel::WARN, VULKAN_NATIVE_CREATE_FAILED_WARN_LINE, block->detail);
      entry.vk_native_block = std::move(block);
    }
  }
  // Native NR at Present stopped on this device (second-queue timeouts, the game's NGX shutdown): the chain goes on for the session.
  if (entry.vk_dlss && !entry.vk_native_stopped && entry.vk_dlss->PresentStopped()) {
    entry.vk_native_stopped = std::format(VULKAN_NATIVE_STOPPED_LINE, *entry.vk_dlss->PresentStopped());
    nr::Logf(nr::LogLevel::WARN, VULKAN_NATIVE_STOPPED_WARN_LINE, *entry.vk_dlss->PresentStopped());
  }
  // The device was lost soon after a native Present submission: native NR stays off from now on, and from the next start (VulkanNativeNr = 0).
  if (entry.vk_dlss && entry.vk_dlss->PresentLatchTripped() && !entry.vk_native_latch_noted) {
    entry.vk_native_latch_noted = true;
    state.settings.vulkan_native_nr = false;
    state.vk_native_off = std::string(VULKAN_NATIVE_OFF_REASON);
    state.latch_save_pending = true;  // saved on this present thread, below
    nr::Logf(nr::LogLevel::WARN, uplift::text::VK_PRESENT_DEVICE_LOST_LINE, addon::VkDlssContext::DLSS_LATCH_WINDOW.count());
  }
  // No route starts on a device its native context saw lost (the native runtime was left mapped there): NR stays off on it, as the loss's
  // own message says. Only the latch's save, and the native marker's clear, still happen. The same for any native NR that
  // was dropped (a switch or a shutdown whose GPU work did not finish): its runtime may still be mapped, and its context keeps the claim.
  if (entry.vk_dlss && (entry.vk_dlss->DeviceLost() || entry.vk_dlss->Abandoned())) {
    ClearNativePresentMarker(device, entry.vk_dlss->NrState());
    if (state.latch_save_pending) {
      state.latch_save_pending = false;
      SaveAndApply(&state);
    }
    return true;
  }
  if (entry.vk_dlss && !entry.vk_native_block) {
    entry.vk_native_block = entry.vk_dlss->PresentBlock();  // only Present's command buffers here: a creation failure was latched above
  }
  // The same for the Direct3D 12 route, whose creation failure (structured, never parsed from the message) sends NR at Present on to the helper.
  if (entry.context && (!entry.vk_d3d12_block || entry.vk_d3d12_block->cause != addon::VkD3d12Block::Cause::CREATE_FAILURE)) {
    if (std::string failure = entry.context->CreateFailure(); !failure.empty()) {
      nr::Logf(nr::LogLevel::WARN, VULKAN_D3D12_CREATE_FAILED_WARN_LINE, failure);
      entry.vk_d3d12_block = addon::VkD3d12Block{addon::VkD3d12Block::Cause::CREATE_FAILURE, std::move(failure)};
    }
  }
  return false;
}

void VkDeviceRouter::ChooseRoute() {
  route = addon::ChooseVkNrRoute({
      .setting = state.settings.vulkan_nr,
      .native = entry.vk_native_block,
      .native_off = (entry.vk_native_stopped ? std::string_view(*entry.vk_native_stopped) : std::string_view(state.vk_native_off)),
      .d3d12 = entry.vk_d3d12_block,
  });
  if (route.route == addon::VkNrRoute::DIRECT3D_12) {
    addon::PrepareBeforeFgBridge(entry.before_fg, addon::BeforeFgActive(before_fg_facts));
  }
  native_route = (route.route == addon::VkNrRoute::NATIVE);
  helper_route = (route.route == addon::VkNrRoute::HELPER);
  // Only native Vulkan NR runs inside the game's DLSS. With VulkanNr Direct3D 12 or Helper, or a failure that stops native NR inside the game's DLSS too,
  // the DLSS stages are greyed (the panel) and the native context gets Present (its config, below), so NR moves to Present at once, through the one owner
  // hand-over. A failure of native Present alone leaves them native. While the native context runs a DLSS stage the helper is neither driven nor attached.
  // The stage is this present's (WouldRunDlssStage), so the owner moves in the frame the stage does: one hand-over.
  const addon::VkStageDecision stages = addon::DecideVkStages({
      .setting = state.settings.vulkan_nr,
      .native_dlss_off = VulkanDlssReason(state, entry),
      .dlss_stage_wanted = (entry.vk_dlss && entry.vk_dlss->WouldRunDlssStage(state.settings.source)),
      .stopped_at_dlss_stage = (entry.vk_dlss && !entry.vk_dlss->CanCopy() && entry.vk_dlss->AtDlssPlacement()),
  });
  entry.vk_stages_off = stages.stages_off;
  vk_dlss_stages_off = stages.native_present_only;
  native_at_dlss_stage = stages.native_at_dlss_stage;
  helper_runs = addon::VkHelperRuns(route.route, native_at_dlss_stage);
  if (!entry.vk_route || entry.vk_route->route != route.route || entry.vk_route->reason != route.reason) {
    nr::Log(nr::LogLevel::INFO, addon::VkRouteLine(route));
  }
  entry.vk_route = route;
  // One owner of NR per device (addon::ChooseVkNrOwner) and one hand-over for every change of it: whatever holds NR and is
  // not the owner drains now, with no grace (VkHolderDrainsNow: the native context through its config, the bridge's context through its, the helper through
  // its FRAME), its GPU work finished first, and the new owner loads from OFF once the claims are free. The owner keeps its grace (a plain NR off).
  owner = addon::ChooseVkNrOwner({.native_at_dlss_stage = native_at_dlss_stage, .route = route.route});
  if (const addon::VkOwnerChange change = addon::VkOwnerChangeOf(entry.vk_owner, owner, native_at_dlss_stage != entry.vk_owner_stage,
                                                                 state.settings.vulkan_nr != entry.vk_owner_setting);
      change.changed) {
    nr::Log(nr::LogLevel::INFO, std::format("Vulkan: {}", addon::VkOwnerChangeLine(entry.vk_owner, owner, change.cause, route.reason)));
  }
  entry.vk_owner = owner;
  entry.vk_owner_stage = native_at_dlss_stage;
  entry.vk_owner_setting = state.settings.vulkan_nr;
}

void VkDeviceRouter::PresentThroughHelper(bool route_wants_nr) {
  const bool wants_nr = (state.settings.enabled && route_wants_nr);
  const bool helper_loaded = state.front->NrLoaded(device);
  const bool helper_allowed = state.helper_claim.Update(device, wants_nr, false, helper_loaded);
  const bool in_process_allowed = state.claim.Update(device, wants_nr, false, helper_loaded);
  // The claims already keep the helper's NR from loading while NR in this process holds the device (the native context's own
  // claim, the bridged context's teardown before this route). Checked here as well, so a slip there waits instead of loading NR twice.
  const bool in_process_off = ((!entry.vk_dlss || entry.vk_dlss->NrState() == nr::SessionState::OFF)
                               && (!entry.context || entry.context->NrState() == nr::SessionState::OFF));
  if (helper_allowed && in_process_allowed && !in_process_off && !std::exchange(entry.helper_guard_noted, true)) {
    nr::Log(nr::LogLevel::WARN, "Vulkan: the helper's NR waits for NR in the game's process to unload first (the claims allowed it early)");
  }
  std::string vk_hook_error;  // an unseen device's reason, read under its leaf lock
  if (!entry.helper.HasClient()) {
    const std::scoped_lock error_lock(state.vk_hook_error_mutex);
    vk_hook_error = state.vk_hook_error;
  }
  addon::HelperFrame helper_frame = {
                                         .device = device,
                                         .swapchain = swapchain,
                                         .runtime = entry.runtime,
                                         .queue = entry.queue,
                                         .present_queue = entry.present_queue,
                                         .vk_hook_error = vk_hook_error,
                                         .back_buffer = entry.back_buffer,
                                         .marker = entry.marker,
                                         .early_marker = early_marker,
                                         .settings = &state.settings,
                                         .overlay = &state.overlay,
                                         .nr_allowed = (helper_allowed && in_process_allowed && in_process_off
                                            ),
                                         .claimed_elsewhere = (wants_nr && !helper_allowed && state.helper_claim.Owner() != device),
                                         .drain_now = addon::VkHolderDrainsNow(owner, addon::VkNrHolder::HELPER),
                                         .now = now,
                                     };
  if (route_wants_nr) {
    const auto decision = DecideBeforeFgFor(state, entry, device, swapchain, before_fg_facts, addon::BeforeFgRoute::VK_HELPER);
    addon::ApplyBeforeFgHelper(entry.before_fg, helper_frame, decision);
    if (decision.state == addon::BeforeFgState::ARM_OUTER) return;
  } else {
    addon::DrainBeforeFgHelper(entry.before_fg, before_fg_facts.early_confirmed && addon::BeforeFgActive(before_fg_facts));
  }
  state.front->Present(entry.helper, helper_frame);
}

bool VkDeviceRouter::LeaveHelper() {
  if (!helper_runs && entry.helper.HasClient()) {
    // The route left the helper. Its NR drains there first (the claims refuse it), so it never runs next to NR in this process on this device; then
    // the client's shares and fences retire behind the effect queue's fences, the helper is detached, and the new route starts at the next present.
    // The same when the placement left Present for a DLSS stage, except that the native context's per-frame work goes on through
    // the drain (its claim waits for the helper's NR to go).
    // "unloaded" is known, never assumed: a FRAME sent without nr_allowed was answered with the helper's Session OFF (or the
    // helper is gone). Until then FRAMEs drain it every present, and no route in this process loads NR on this device; a helper that answers but
    // never gets there is reported once and still waited for (one that stops answering is ended by RemoteNr's own cap).
    if (state.front->NrLoaded(device) || entry.queue == nullptr) {
      if (!entry.helper_drain_since) {
        entry.helper_drain_since = now;
      } else if (now - *entry.helper_drain_since > HELPER_DRAIN_WARN && !std::exchange(entry.helper_drain_warned, true)) {
        nr::Logf(nr::LogLevel::WARN, uplift::text::NR_DRAIN_TIMEOUT_LINE, HELPER_DRAIN_WARN.count());
      }
      PresentThroughHelper(false);
      if (!native_at_dlss_stage) return true;
    } else {
      if (entry.helper_drain_since) {
        nr::Logf(nr::LogLevel::INFO, "Vulkan: Uplift's helper process released NR {} ms after NR moved away from it",
                 std::chrono::duration_cast<std::chrono::milliseconds>(now - *entry.helper_drain_since).count());
      }
      entry.helper_drain_since.reset();
      entry.helper_drain_warned = false;
      addon::ReshadeVkHost host(device, entry.queue, entry.helper.vulkan->VulkanDevice().vkQueueSubmit);
      entry.helper.vulkan->ReleaseAll(host);
      entry.retiring_clients.push_back(std::move(entry.helper.vulkan));
      state.front->Detach(device);
      state.helper_claim.Forget(device);
      state.claim.Forget(device);
      entry.helper = {};
      if (native_at_dlss_stage) {
        nr::Log(nr::LogLevel::INFO, "Vulkan: NR runs at a DLSS stage natively; Uplift's helper process waits until the placement is Present again");
      } else {
        nr::Logf(nr::LogLevel::INFO, "Vulkan: the helper route is gone; NR at Present starts {}",
                 (native_route ? "natively on the game's device" : "on a private Direct3D 12 device"));
        return true;  // this frame goes without NR
      }
    }
  }
  return false;
}

void VkDeviceRouter::StopBridgeOffRoute() {
  if ((native_route || helper_route) && (entry.context || entry.vk_bridge)) {
    // The Direct3D 12 route goes before NR runs natively, as Retry now tears a bridge down (Stop before Teardown; its imports retire behind
    // their fences), so two NR runtimes never coexist: the bridged context's Teardown unloads NR synchronously, and the claim is free after it. The
    // same holds before NR runs in the helper, so the helper's NR never runs next to this one.
    StopBridge(entry, (native_route ? "The Vulkan bridge stopped: NR at Present runs natively on the game's device"
                                    : "The Vulkan bridge stopped: NR at Present runs in Uplift's helper process"));
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
    entry.rejected = false;
    entry.message.clear();
    entry.retry_pending = false;
    entry.diagnostic_retry_in = 0u;
    nr::Logf(nr::LogLevel::INFO, "Vulkan: the Direct3D 12 route is gone; NR at Present starts {}",
             (native_route ? "natively on the game's device" : "in Uplift's helper process"));
  }
}

void VkDeviceRouter::DecidePresentMotion() {
  // DLSS's motion vectors for the Present path, copied in the game's frame by the native context. Only while NR runs at Present on
  // a GPU-ordered bridge (a CPU-ordered one holds this lock across capped CPU waits, which the hooked evaluate must not wait on) or natively, and
  // the game presents on the effect runtime's queue (else the present event waits a GPU frame), the native context can still copy (the game has not shut
  // NGX down), the native placement is not a DLSS one, the motion preference is DLSS (MotionVectors Auto or DLSS), and the game has created DLSS on this
  // device.
  const nr::SessionState present_state =
      (native_route ? (entry.vk_dlss ? entry.vk_dlss->NrState() : nr::SessionState::OFF) : (entry.context ? entry.context->NrState() : nr::SessionState::OFF));
  const bool outer_motion_route = addon::BeforeFgOuterMotionCandidate(before_fg_facts);
  replace_late_motion = outer_motion_route && (state.settings.motion_vectors == ui::MotionVectorSource::LAUNCHPAD
      || state.settings.motion_vectors == ui::MotionVectorSource::LUMENITE);
  present_motion_wanted = addon::PresentMotionWanted({
      .enabled = state.settings.enabled,
      .nr_state = present_state,
      .bridge_gpu_ordered = (entry.vk_bridge && entry.vk_bridge->GpuOrdered() && !entry.vk_bridge->Stopped()),
      .native_present = native_route,
      .same_queue = (entry.present_queue == entry.queue || outer_motion_route),
      .native_wants_nr = (entry.vk_dlss && entry.vk_dlss->AtDlssPlacement()),
      .native_present_group = (entry.vk_dlss && entry.vk_dlss->AtDlssPlacement() && entry.vk_dlss->RunsAt(addon::Stage::PRESENT)),
      .native_can_copy = (!entry.vk_dlss || entry.vk_dlss->CanCopy()),
      .problem = (addon::VkNativeBlocksDlss(entry.vk_native_block) || !state.dlss_unavailable_reason.empty()),
      .dlss_motion = replace_late_motion ? state.settings.before_fg_motion == ui::Settings::FgMotion::DLSS : ui::MotionPickOf(state.settings) == ui::MotionPick::DLSS,
      .upscaler_created = state.bridge.Registry().UpscalerCreated(device),
  });
}

void VkDeviceRouter::EnsureNativeContext() {
  if (!entry.vk_dlss && !addon::VkNativeBlocksDlss(entry.vk_native_block)
      && (native_route
          || (state.dlss_unavailable_reason.empty() && ((state.settings.source == ui::PlacementSource::DLSS && !vk_dlss_stages_off) || present_motion_wanted)))) {
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
      const std::optional<std::filesystem::path> snippet = LocateSnippetPath(state);
      entry.snippet_path = snippet.value_or(std::filesystem::path());
      entry.vk_dlss = addon::VkDlssContext::Create(
          {
              .snippet = {.snippet_path = entry.snippet_path, .application_data_path = state.ngx_data_directory},
              .binding = {.instance = record->instance,
                          .physical = record->physical,
                          .device = vk_device,
                          .get_instance_proc_addr = (record->ext_buffer_device_address ? &vk::GetExtNgxInstanceProcAddr : loader->get_instance_proc_addr),
                          .get_device_proc_addr = (record->ext_buffer_device_address ? &vk::GetExtNgxDeviceProcAddr : loader->get_device_proc_addr)},
              .adapter = adapter,
              // The frame semaphore is signalled through ReShade's queue, in the present event, where ReShade holds its lock.
              .signal = [](void* signal_queue, VkSemaphore semaphore, uint64_t value) {
                return signal_queue != nullptr && static_cast<api::command_queue*>(signal_queue)->signal(api::fence{addon::ReshadeHandleOf(semaphore)}, value);
              },
              .queue_family = record->graphics_family,  // native NR at Present's own command buffers
              .before_present_load = [device = device] { NoteNativePresentLoad(device); },
          },
          &error);
      if (!entry.context) {
        entry.static_block.clear();
        NoteStaticBlock(state, entry, snippet);  // the bridge's context has not decided it yet
      }
    }
    if (entry.vk_dlss) {
      state.bridge.AddVkContexts(1);  // the NGX hooks' Vulkan paths stop being a passthrough
      entry.vk_dlss->SetDlssUnavailableReason(state.dlss_unavailable_reason);
      nr::Log(nr::LogLevel::INFO, (native_route                                        ? "Vulkan: native NR is ready on the game's device"
                                   : state.settings.source == ui::PlacementSource::DLSS ? "Vulkan: NR after DLSS available on this device"
                                                                                        : "Vulkan: DLSS's motion vectors can reach the Present path on this device"));
    } else {
      entry.vk_native_block = addon::VkNativeBlock{addon::VkNativeBlock::Cause::START_FAILED, error};  // the Direct3D 12 route from the next present
      nr::Log(nr::LogLevel::ERR, addon::VkNativeDlssProblem(entry.vk_native_block));
    }
  }
}

bool VkDeviceRouter::RunNativeContext() {
  addon::VkDlssContext& native = *entry.vk_dlss;
  // Tokens of stale command-buffer states, handed over by init_command_list.
  std::vector<addon::StaleVkList> orphans;
  {
    const std::scoped_lock orphans_lock(state.vk_orphans_mutex);
    for (addon::StaleVkList& orphan : state.vk_orphans) {
      if (orphan.device == device) {
        orphans.push_back(std::move(orphan));
      }
    }
    std::erase_if(state.vk_orphans, [device = device](const addon::StaleVkList& orphan) { return orphan.device == device; });
  }
  for (const addon::StaleVkList& orphan : orphans) {
    native.Recycle(orphan.tokens);
  }
  native.SetBlockedReason(BlockedReason(state, entry));
  // NrClaim, with the native context's own identity: it wants NR when its placement is a DLSS one (DLSS seen) or, on the native route, at Present; a
  // DLSS placement takes NR from the bridge (the device's identity), which drains first. Once it neither wants NR nor holds it, it is forgotten, so the
  // bridge can take NR back.
  const bool native_wants = native.WantsNr();
  bool native_allowed =
      state.claim.Update(&native, state.settings.enabled && native_wants, native.AtDlssPlacement(), native.NrState() != nr::SessionState::OFF);
  // The claim keeps it while the helper drains (the device's identity holds it); checked here as well, so NR never loads in
  // this process while the helper's NR may still run for this device.
  if (native_allowed && native.NrState() == nr::SessionState::OFF && state.front->NrLoaded(device)) {
    native_allowed = false;
    if (!std::exchange(entry.native_guard_noted, true)) {
      nr::Log(nr::LogLevel::WARN, "Vulkan: native NR waits for the helper's NR to unload first (the claim allowed it early)");
    }
  }
  if (!native_wants && native.NrState() == nr::SessionState::OFF) {
    state.claim.Forget(&native);
  }
  addon::FrameSettings native_settings = addon::BuildFrameSettings(
      {.settings = &state.settings, .drag = state.overlay.drag, .defaults_view = state.overlay.defaults_view, .settings_generation = state.settings_generation},
      &state.coalescer, now);
  addon::FrameFacts native_facts = addon::BuildFrameFacts(
      state.settings,
      {.ngx_frame_generation = addon::BeforeFgConfigCount(before_fg_facts), .nr_allowed = native_allowed, .uplift_mv_lumenite = entry.uplift_mv_lumenite});
  native_facts.present_motion_copy = present_motion_wanted;
  if (replace_late_motion) {
    native_facts.motion_at_dlss_stage = native_settings.motion_vectors;
    native_settings.motion_vectors = state.settings.before_fg_motion == ui::Settings::FgMotion::DLSS;
  }
  native_facts.dlss_motion_upside_down = entry.depth_upside_down.value_or(false);  // 1.1.6
  // The game released its Vulkan DLSS here (asked of the registry once the context saw DLSS).
  native_facts.dlss_released = addon::DlssReleased(state.bridge.Registry(), device, ngx_hooks::NgxApi::VULKAN, native.DlssSeen());
  // The route, and the NGX core initialised for the game's device unless the game created DLSS on it (then the core is the game's own).
  native_facts.vk_native_present = native_route;
  native_facts.vk_initialize_core = !state.bridge.Registry().UpscalerCreated(device);
  // Split chain: the native context runs passes at several stages while VulkanNr is Native and its DLSS stages may run; its Present then
  // needs the native route (a Present that fell back cannot run, and its passes join the DLSS stage).
  native_facts.split_capable = addon::SplitCapable(true, false, state.settings.vulkan_nr == ui::VulkanNrMode::NATIVE && !vk_dlss_stages_off);
  NoteEarlyMarker(early_marker, &native_facts);
  native_facts.effects_on = (entry.runtime != nullptr && entry.runtime->get_effects_state());  // 1.1.6: NR before effects at their begin event
  if (vk_dlss_stages_off) {
    // The route is Direct3D 12 or Helper, so the native context runs no DLSS stage whatever is stored (Auto stays at Present here).
    native_settings.source = ui::PlacementSource::AUTO;
    native_settings.pre_upscale = false;
  }
  if (addon::VkHolderDrainsNow(owner, addon::VkNrHolder::NATIVE)) {
    native_settings.session.grace = addon::NO_GRACE;  // transitions: not the owner: it drains at once
  }
  addon::PrepareBeforeFgNative(entry.before_fg, native_settings, native_facts, addon::BeforeFgActive(before_fg_facts), state.claim.Owner() == device);
  addon::BeforeFgDecision native_before_fg;
  if (native_route) {
    native_before_fg = DecideBeforeFgFor(state, entry, device, swapchain, before_fg_facts, addon::BeforeFgRoute::VK_NATIVE);
    addon::ApplyBeforeFgNative(entry.before_fg, native_settings, native_facts, native_before_fg, before_fg_facts.outer_matches_primary,
                              state.claim.Owner() == device, entry.vk_native_present);
  } else if (route.route != addon::VkNrRoute::DIRECT3D_12) {
    addon::ClearBeforeFgNativePrelude(entry.before_fg);
  }
  const addon::VkPresentFrame present_frame = {
      .back_buffer = VulkanImageInfo(device, entry.back_buffer),
      .color_space = static_cast<color::ColorSpace>(swapchain->get_color_space()),
      .marker_expected = (entry.marker.handle != 0u),
  };
  // The effect runtime's queue (none until the runtime is up: the frame is then not signalled).
  const addon::TriggerPoint native_point = native.BeginInnerFrame(native_route && entry.before_fg.outer_owns_frame, outer_ticked, entry.queue,
                                                                   native_settings, native_facts, native_allowed, state.claim.Owner() == device, now, present_frame);
  ClearNativePresentMarker(device, native.NrState());
  NoteLatchIfTripped(&state, &native);
  if (const auto [mask_running, mask_idle_note] = MaskState(state, entry, mask_lookup); !mask_running) {
    // The native mask copy goes here too. The bridged context's branch below, which releases it as well, is never reached while that
    // context (or its bridge) could not start, nor when native NR alone runs the device.
    native.ReleaseMask();
    entry.mask_note = mask_idle_note;
  }
  // The NGX hooks watch this device's Vulkan DLSS evaluates only while Source is DLSS (the context learns the game's
  // DLSS from them) or the context wants or holds NR; after a pick back to Auto or Present, once NR is released, the evaluate is a passthrough again.
  const bool watching = ((state.settings.source == ui::PlacementSource::DLSS && !vk_dlss_stages_off) || native.WantsNr() || native.NrState() != nr::SessionState::OFF
                         || present_motion_wanted);
  if (watching != entry.vk_nr_counted) {
    state.bridge.NoteVkContextNr(entry.vk_nr_counted, watching);
    nr::Log(nr::LogLevel::INFO, (watching ? "Vulkan: the game's DLSS evaluates are watched for NR on this device"
                                          : "Vulkan: NR is off here and no DLSS placement is chosen: the game's DLSS evaluates are a pure passthrough again"));
  }
  if (native_route) {
    // NR at Present runs natively and the Direct3D 12 route does not exist here. The effect events reach the native context (RunNative).
    entry.vk_native_present = true;
    FreeRetiredVulkanBridges(entry, device, &vk_after_unlock);
    HashRuntimeOnce(state, entry);
    if (state.latch_save_pending) {
      state.latch_save_pending = false;
      SaveAndApply(&state);
    }
    state.bridge.SetPreSr(state.settings.enabled && state.settings.pre_upscale && state.settings.source != ui::PlacementSource::PRESENT
                          && state.dlss_unavailable_reason.empty() && !vk_dlss_stages_off);
    if (native_before_fg.state == addon::BeforeFgState::ARM_OUTER) return true;
    if (entry.queue != nullptr && native.NrState() != nr::SessionState::OFF) {
      // As in the bridge's BeginFrame, the present event comes before ReShade records its effects, so this barrier leads the
      // immediate list. It orders the game's whole frame on this queue before the effects, which a trigger inside them flushes early (before Uplift submits)
      // without the game's present semaphores. ReShade's API only: nothing raw goes into its immediate list.
      addon::ReshadeVkHost host(device, entry.queue, native.QueueSubmit());
      if (host.Valid()) {
        vk::RecordOpeningBarrier(host);
      }
    }
    RunNative(entry, device, native_point, vk::Usage::PRESENT, native_point == addon::TriggerPoint::EARLY_MARKER ? early_marker.motion : api::resource_view{});
    return true;
  }
  if (native.WantsNr() && !entry.context && !helper_runs) {
    // NR runs here natively and the Present path has nothing to do: no bridge is made for it (the helper route drives the front below, whose claim
    // yields to the native context).
    HashRuntimeOnce(state, entry);
    if (state.latch_save_pending) {
      state.latch_save_pending = false;
      SaveAndApply(&state);
    }
    state.bridge.SetPreSr(state.settings.enabled && state.settings.pre_upscale && state.settings.source != ui::PlacementSource::PRESENT
                          && state.dlss_unavailable_reason.empty() && !vk_dlss_stages_off);
    return true;
  }
  return false;
}

void VkDeviceRouter::RunHelperRoute() {
  // NR at Present runs in gitc-uplift-helper64.exe (a clean process: the KytyPS5 emulator reserves the address space NVIDIA's CUDA kernels need in
  // this one), and no bridge or Direct3D 12 core exists here. The native context, if any, still runs a DLSS placement.
  entry.helper.route_reason = route.reason;
  FreeRetiredVulkanBridges(entry, device, &vk_after_unlock);
  if (entry.vk_dlss) {
    HashRuntimeOnce(state, entry);
  }
  if (state.latch_save_pending) {
    state.latch_save_pending = false;
    SaveAndApply(&state);
  }
  state.bridge.SetPreSr(state.settings.enabled && state.settings.pre_upscale && state.settings.source != ui::PlacementSource::PRESENT
                        && state.dlss_unavailable_reason.empty() && !vk_dlss_stages_off);
  if (!entry.helper.rejected) {
    PresentThroughHelper(true);
  }
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
    const bool d3d9 = (device_api == api::device_api::d3d9);  // routed to the helper front after the common prefix
    const bool vulkan = (device_api == api::device_api::vulkan);
    const bool opengl = (device_api == api::device_api::opengl);  // the same shape as Vulkan, on a GL context
    PollHooks();
    StampThisThread();
    const auto now = std::chrono::steady_clock::now();
    // The private device D3D12Side made for a new bridge. On the fallback it is D3D12CreateDevice's (ReShade's proxy in a game),
    // whose last release raises destroy_device, whose handler takes the add-on's lock; the device factory's raises nothing, and goes the same way.
    // Declared before the lock, it is released after it.
    Microsoft::WRL::ComPtr<ID3D12Device> release_after_unlock;
    // What D3D11CreateDevice returned for a new D3D10 bridge's relay (ReShade's proxy in a game); released after
    // the lock, like release_after_unlock.
    Microsoft::WRL::ComPtr<ID3D11Device> relay_after_unlock;
    // What Retry now tears down, destroyed after the lock as destroy_device does (a private device's last release raises destroy_device).
    std::unique_ptr<bridge::D3D11Bridge> d3d11_after_unlock;
    std::unique_ptr<bridge::D3D10Bridge> d3d10_after_unlock;
    std::unique_ptr<bridge::GlBridge> gl_after_unlock;
    std::vector<std::unique_ptr<bridge::VkBridge>> vk_after_unlock;
    const std::unique_lock lock(g_state->mutex);
    // A Session may unload NR here (BeginFrame: the native Vulkan context's, and the Direct3D 12 context's), and its snippet may reach the
    // core's Shutdown1 on this thread, under the lock.
    const HoldsLockForNr holds;
    AddonState& state = *g_state;

    addon::host::PollEnabled(state, now);

    DeviceEntry& entry = state.devices[device];
    // The device's API, from its first present on: Bridged(), DlssApiOf() and the panel answer the same before and after a bridge exists.
    entry.d3d11 = (device_api == api::device_api::d3d11);
    entry.d3d10 = (device_api == api::device_api::d3d10);
    entry.vulkan = vulkan;
    entry.opengl = opengl;
    if (d3d9) {
      state.front->NotePresent(entry.helper, device, state.settings);  // before the selector, as gitc-uplift.addon32 does
    }
    if (vulkan) {
      addon::host::CountVulkanPresent(entry, device);
    }
    if (!entry.selector.OnPresent(swapchain, now)) return;
    const bool outer_ticked = entry.before_fg.TakeOuterTick();
    if (addon::streamline::Installed() && (device_api == api::device_api::d3d12 || vulkan)) {
      ++entry.before_fg.activity.reshade;
      if (const auto count = NextBeforeFgTraceSample(present_counts.reshade)) {
        nr::Logf(nr::LogLevel::INFO, "Before FG trace: ReShade swapchain={} native={:#x} queue={:#x} index={} inside outer={} NGX FG handles={} events={}",
                 static_cast<void*>(swapchain), swapchain->get_native(), queue->get_native(), swapchain->get_current_back_buffer_index(),
                 addon::streamline::InsideOuterPresent(), state.bridge.Registry().LiveCount(ngx_hooks::FeatureKind::FRAME_GENERATION), *count);
      }
    }
    if (device_api == api::device_api::d3d12 && addon::streamline::Installed()) {
      auto* native = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
      if (addon::streamline::InsideOuterPresent() || entry.before_fg.early_dxgi != native || entry.before_fg.early_thread != GetCurrentThreadId()) {
        entry.before_fg.early_confirmed = false;
        entry.before_fg.early_dxgi = nullptr;
      }
      if (!entry.before_fg.early_confirmed && addon::streamline::WatchGameFacing(native, BeforeFgTrace() && SamplePresentCount(entry.before_fg.activity.reshade))) {
        entry.before_fg.early_dxgi = native;
        entry.before_fg.early_thread = GetCurrentThreadId();
      }
    }
    if (vulkan && addon::streamline::InsideOuterPresent()) entry.before_fg.early_confirmed = false;
    if (vulkan && addon::streamline::Installed() && !addon::streamline::InsideOuterPresent()) {
      if (entry.before_fg.early_vk_chain != addon::VulkanHandleOf<VkSwapchainKHR>(swapchain->get_native())
          || entry.before_fg.early_vk_queue != addon::VulkanHandleOf<VkQueue>(queue->get_native()) || entry.before_fg.early_thread != GetCurrentThreadId()) entry.before_fg.early_confirmed = false;
      entry.before_fg.early_vk_chain = addon::VulkanHandleOf<VkSwapchainKHR>(swapchain->get_native());
      entry.before_fg.early_vk_queue = addon::VulkanHandleOf<VkQueue>(queue->get_native());
      entry.before_fg.early_thread = GetCurrentThreadId();
    }
    // One read of the Streamline mode per Present: the handoff clear, the detector-count mask and the facts below cannot disagree.
    const addon::streamline::FgMode fg_mode = addon::streamline::Mode();
    if (!state.settings.enabled || state.settings.fg_present != ui::Settings::FgPresent::BEFORE || fg_mode == addon::streamline::FgMode::OFF) {
      addon::ClearBeforeFgHandoff(entry.before_fg);
    }
    const bool observed_fg_off = state.settings.fg_present == ui::Settings::FgPresent::BEFORE && (entry.before_fg.outer_inner == swapchain || entry.before_fg.early_confirmed)
        && fg_mode == addon::streamline::FgMode::OFF;
    if (state.e2e.enabled) {
      E2eLiveSettings(state, entry, device);
    }
    // Snapshot common evidence once; the primary route completes its inputs only after its early-return boundaries.
    const addon::BeforeFgInputs before_fg_facts = {
        .enabled = state.settings.enabled,
        .requested = state.settings.fg_present,
        .mode = fg_mode,
        .ngx_live = state.bridge.Registry().LiveCount(ngx_hooks::FeatureKind::FRAME_GENERATION),
        .observed_fg_off = observed_fg_off,
        .early_confirmed = entry.before_fg.early_confirmed,
        .outer_matches_primary = entry.before_fg.outer_inner == swapchain,
        .motion_source = state.settings.motion_vectors,
        .before_fg_motion = state.settings.before_fg_motion,
    };
    // This bookkeeping runs before the context (and, on D3D11 and D3D10, the bridge) exist, so a bridged device
    // waiting for its first enable still has `entry.runtime`, which the Enable hotkey and the overlay need.
    addon::host::BindPresentRuntime(entry, state.runtimes, queue, swapchain);
    if (const bool upside_down = DepthUpsideDown(entry.runtime); entry.depth_upside_down != upside_down) {
      if (entry.depth_upside_down.has_value() || upside_down) {
        nr::Logf(nr::LogLevel::INFO, "{} is {}: DLSS's motion vectors are {} at Present", DEPTH_UPSIDE_DOWN_DEFINE, (upside_down ? 1 : 0),
                 (upside_down ? "flipped vertically" : "used as they are"));
      }
      entry.depth_upside_down = upside_down;
    }
    addon::host::BindPresentQueues(entry, queue, swapchain, vulkan || opengl);
    entry.output_space = static_cast<color::ColorSpace>(swapchain->get_color_space());
    if (!d3d9) {
      const api::resource_desc back_buffer_desc = device->get_resource_desc(entry.back_buffer);
      entry.swapchain_size = {back_buffer_desc.texture.width, back_buffer_desc.texture.height};
    }
    entry.launchpad_ready = false;
    entry.lumenite_ready = false;
    entry.helper.launchpad_ready = false;
    entry.helper.lumenite_ready = false;
    if (const std::optional<addon::MotionLinkStep> step = addon::host::LinkMotion(entry, state.settings.motion_vectors, d3d9, now)) {
      addon::host::NoteMotionLinkStep(*step, &entry);
      addon::host::NoteMotionLinkStep(*step, &entry.helper);
    }
    const addon::RenderedMarker early_marker = addon::TakeEarlyMarker(entry.motion_links, entry.runtime, entry.marker);
    if (d3d9) {
      // A Direct3D 9 device's NR runs in gitc-uplift-helper64.exe (next to this add-on), through the front both
      // add-ons share: its client, the helper's FRAME, and NR at the present point. No context and no bridge exist for it.
      if (entry.helper.rejected) return;
      const bool nr_allowed = state.helper_claim.Update(device, state.settings.enabled, false, state.front->NrLoaded(device));
      state.front->Present(entry.helper, {
                                             .device = device,
                                             .swapchain = swapchain,
                                             .runtime = entry.runtime,
                                             .back_buffer = entry.back_buffer,
                                             .marker = entry.marker,
                                             .early_marker = early_marker,
                                             .settings = &state.settings,
                                             .overlay = &state.overlay,
                                             .nr_allowed = nr_allowed,
                                             .claimed_elsewhere = (state.settings.enabled && !nr_allowed && state.helper_claim.Owner() != device),
                                             .now = now,
                                         });
      return;
    }

    std::optional<std::pair<bool, std::string_view>> mask_lookup;
    bool present_motion_wanted = false;  // whether the native Vulkan context copies DLSS's vectors this present (decided in the Vulkan block below)
    bool vk_dlss_stages_off = false;     // a Vulkan device whose route is Direct3D 12 or Helper runs no DLSS stage (decided below)
    if (vulkan) {
      if (VkDeviceRouter(state, entry,
                         {.device = device,
                          .swapchain = swapchain,
                          .now = now,
                          .outer_ticked = outer_ticked,
                          .before_fg_facts = before_fg_facts,
                          .early_marker = early_marker,
                          .vk_after_unlock = vk_after_unlock,
                          .mask_lookup = mask_lookup,
                          .present_motion_wanted = present_motion_wanted,
                          .vk_dlss_stages_off = vk_dlss_stages_off}).Present()) return;
    }
    if (Bridged(entry)) {
      FreeRetiredVulkanBridges(entry, device, &vk_after_unlock);
      // A stop is counted once for the 2-strike rule. A private device the context found removed latches its bridge too, so the
      // card and the log name it.
      if (entry.context && entry.strikes.Note(PrivateDeviceStopped(entry), PrivateDeviceIndependent(entry))) {
        if (const std::optional<addon::NrHeartbeat::Figures> figures = entry.heartbeat.Stop(now)) {
          LogHeartbeat(entry, device, "NR stopped after", *figures);  // how long NR ran, with what, and the memory then
        }
        // A context on a bridged device implies its bridge (the context is made only after it, and the two go together), so the pointer is never
        // null here; without one there would be nothing to stop or to name.
        if (bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge(); private_bridge != nullptr && private_bridge->Latch().empty()) {
          const std::string reason = std::format(PRIVATE_DEVICE_REMOVED_LINE, private_bridge->Label());
          nr::Log(nr::LogLevel::ERR, reason);
          private_bridge->Stop(reason);
        }
        if (entry.strikes.Exhausted()) {
          nr::Logf(nr::LogLevel::WARN, "NR stopped twice this session on this device: it stays off until the game restarts");
        } else if (entry.strikes.Unretryable()) {
          // The fallback's private device is the adapter's shared one, which the abandoned NR runtime keeps removed: no new device
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
        // Retry now. As the game's own device going: Stop before Teardown, so a wedged private queue drains during Teardown's wait;
        // the private device is only released (Teardown abandons a removed one's NR without an NGX call). The game's side of the bridge goes by
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
            state.bridge.NoteD3D11Watch(entry.d3d11_watch_counted, false);  // the next context counts again
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
      entry.static_block.clear();  // a rebuild must not keep a stale reason
      ID3D12Device* native_device = nullptr;
      if (vulkan) {
        // The bridge is the Direct3D 12 route's private device. On another route (the helper's, while the native context sits at a DLSS
        // stage without NR) a bridge made here would be torn down by the next present's StopBridgeOffRoute.
        if (!addon::VkPresentMakesBridge(entry.vk_route ? entry.vk_route->route : addon::VkNrRoute::DIRECT3D_12)) return;
        // On Vulkan, the private device is made at the first present with NR on, then kept, as on D3D11. The vendor, the LUID and the
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
            if (!entry.vk_bridge) {
              if (!entry.vk_d3d12_block || entry.vk_d3d12_block->cause != addon::VkD3d12Block::Cause::CREATE_FAILURE) {
                entry.vk_d3d12_block = addon::VkD3d12Block{addon::VkD3d12Block::Cause::START_FAILED, error};  // NR at Present goes on to the helper from the next present
              }
            }
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
        // On OpenGL, the private device is made at the first present with NR on, then kept, as on Vulkan. The context is checked
        // first: from the queues and the system wglGetCurrentContext alone, with no GL call. A context of the game's own, a present
        // with no context current, or a runtime that is not up yet gets nothing this frame, is asked again at the next one, and is
        // never rejected for good; only the game's own context gets the card's context-mismatch text (the others say nothing).
        // gl::Functions::Load, which does make GL calls, then runs once, on the runtime's own context, and the bridge keeps its table
        // (every path after it makes the bridge or rejects the device, so Load never runs twice). The device's gaps (no
        // memory-object extension, a foreign GPU) are decided once.
        if (!state.settings.enabled) {
          entry.message.clear();  // only the context-mismatch text can be here (a rejected device returned above): NR is off, so there is nothing to say
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
        // On D3D11 and D3D10, the private devices are made at the first present with NR on, then
        // kept. A game that never enables NR pays nothing.
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
          entry.message = uplift::text::NVIDIA_GPU_REQUIRED_LINE;
          nr::Log(nr::LogLevel::WARN, entry.message);
          return;
        }
      }
      const std::optional<std::filesystem::path> snippet = LocateSnippetPath(state);
      entry.snippet_path = snippet.value_or(std::filesystem::path());
      std::string error;
      entry.context = addon::DeviceContext::Create(
          native_device, {.snippet_path = entry.snippet_path, .application_data_path = state.ngx_data_directory}, &error,
          {.bridged = Bridged(entry), .dlss_stages = entry.d3d11});  // A Direct3D 11 bridge's context runs the DLSS stages
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
        if (vulkan) {
          if (!entry.vk_d3d12_block || entry.vk_d3d12_block->cause != addon::VkD3d12Block::Cause::CREATE_FAILURE) {
            entry.vk_d3d12_block = addon::VkD3d12Block{addon::VkD3d12Block::Cause::START_FAILED, error};  // NR at Present goes on to the helper from the next present
          }
        }
        entry.message = std::format("Uplift could not start on this device: {}", error);
        nr::Log(nr::LogLevel::ERR, entry.message);
        return;
      }
      entry.message.clear();
      entry.context->SetDlssUnavailableReason(state.dlss_unavailable_reason);
      NoteStaticBlock(state, entry, snippet);
      if (entry.d3d11) {
        // The mark the hooked Direct3D 11 calls find this device by, whichever pointer the game hands NGX (ReShade's proxy forwards it).
        addon::MarkD3D11Device(reinterpret_cast<ID3D11Device*>(device->get_native()), device);
      }
      if (!Bridged(entry)) {
        // The mark the game's NGX shutdown finds this device by (BeforeCoreShutdownD3D12), whichever pointer the game hands NGX.
        api::device* const owner = device;
        native_device->SetPrivateData(addon::UPLIFT_RESHADE_DEVICE_GUID, sizeof(owner), &owner);
        // A context made after the game shut NGX down here (its last one ended with its present queue) starts held, or abandoned.
        entry.context->SeedCoreHold(entry.core_hold);
      }
    }

    const ui::Settings& settings = state.settings;
    HashRuntimeOnce(state, entry);
    if (device->get_api() == api::device_api::d3d12) state.bridge.LogD3D12Diagnostics(now);
    entry.context->SetBlockedReason(BlockedReason(state, entry));
    if (entry.d3d11) {
      // A foreign Direct3D 12 DLSS device can appear at any time; the game's own device removed decides the latch window; the hooks
      // watch this device's evaluates while NR is on or not yet released.
      if (!entry.foreign_dlss && state.bridge.ForeignD3D12Dlss() && !entry.context->DlssSeen()) {
        entry.foreign_dlss = true;
        nr::Logf(nr::LogLevel::INFO, "Direct3D 11: {}", ui::FOREIGN_D3D12_DLSS_REASON);
      }
      entry.context->SetDlssUnavailableReason(std::string(D3D11DlssReason(state, entry)));
      if (const HRESULT removed = reinterpret_cast<ID3D11Device*>(device->get_native())->GetDeviceRemovedReason(); FAILED(removed)) {
        entry.context->NoteGameDeviceRemoved(removed, now);
        NoteLatchIfTripped(&state, entry.context.get());  // now, as the evaluate path does, with the removal's own text
      }
      state.bridge.NoteD3D11Watch(entry.d3d11_watch_counted, state.settings.enabled || entry.context->NrState() != nr::SessionState::OFF);
    }
    const addon::ContextStatus before = entry.context->Status();
    // A line every NrHeartbeat::PERIOD while NR runs, from the recordings that applied NR (any placement, any thread) and the latest one's motion source. A
    // native Direct3D 12 device's removal is reported right after BeginFrame below, which is where it is found; a bridged device's stop where its strike
    // is counted, above.
    if (const std::optional<addon::NrHeartbeat::Figures> figures =
            entry.heartbeat.Note(entry.context->NrRecordings(), entry.context->LatestMotionSource(), now)) {
      LogHeartbeat(entry, device, "NR running:", *figures);
    }
    // DiagnosticRemoveDevice (hidden, diagnostic only): Uplift's own private device is removed once NR has run that many frames on it (never the
    // game's: Bridged only), so Retry now and the 2-strike rule can be tested without a real hang.
    if (state.settings.diagnostic_remove_device != 0u && Bridged(entry) && before.nr_applied && !entry.strikes.Stopped()
        && !(entry.d3d11 && addon::IsDlssStage(entry.context->CurrentPlacement()))  // DiagnosticRemoveMidFrame's
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
    // Without a running UPLIFT_MASK there is no mask to bind; the copy is released (and the native context's too, in the Vulkan block
    // above, which a device whose bridge never started reaches alone).
    const auto [mask_running, mask_idle_note] = MaskState(state, entry, mask_lookup);
    if (!mask_running) {
      entry.context->PrepareMaskCopy(nullptr);
      ReleaseBridgeMask(entry, device);
      if (entry.vk_dlss) {
        entry.vk_dlss->ReleaseMask();  // the native context's copy on the game's device
      }
      entry.mask_note = mask_idle_note;
    }
    // BuildFrameSettings and BuildFrameFacts are shared with the 32-bit helper; the Defaults view is applied inside them.
    addon::FrameSettings frame_settings = addon::BuildFrameSettings(
        {.settings = &settings, .drag = state.overlay.drag, .defaults_view = state.overlay.defaults_view, .settings_generation = state.settings_generation},
        &state.coalescer, now);
    addon::FrameFacts frame_facts = addon::BuildFrameFacts(
        settings, {.ngx_frame_generation = addon::BeforeFgConfigCount(before_fg_facts), .nr_allowed = nr_allowed, .uplift_mv_lumenite = entry.uplift_mv_lumenite});
    if (entry.vk_bridge) {
      frame_settings.source = addon::VkPresentSource(frame_settings.source);
    }
    // The bridged context's Details motion line says what is true on Vulkan, where DLSS's vectors come as copies.
    frame_facts.vulkan = entry.vulkan;
    if (Bridged(entry) && !entry.vk_bridge) {
      const auto decision = DecideBeforeFgFor(state, entry, device, swapchain, before_fg_facts, addon::BeforeFgRoute::UNSUPPORTED_BRIDGE);
      addon::ApplyBeforeFgUnsupported(entry.before_fg, frame_facts, decision);
    } else {
      addon::SelectBeforeFg(frame_facts, addon::BeforeFgActive(before_fg_facts));
    }
    // Split chain: Direct3D 12 native and a Direct3D 11 device's DLSS bridge run passes at several stages; the other bridges one stage.
    frame_facts.split_capable = addon::SplitCapable(Bridged(entry), entry.d3d11);
    frame_facts.present_motion_copy = present_motion_wanted;
    if (entry.d3d11) {
      // Direct3D 11's ring, as the Direct3D 12 path: Source = Present.
      frame_facts.present_motion_copy = (settings.source == ui::PlacementSource::PRESENT && frame_settings.motion_vectors);
    }
    // A Direct3D 12 context held after the game's NGX shutdown resumes once this rises (the game created its DLSS again); bridged ones never hold.
    frame_facts.upscaler_creates = state.bridge.Registry().UpscalerCreates(device);
    // The game released its DLSS on this device (or on Direct3D 11 shut NGX down): once that holds, its DLSS stages fall back to Present.
    // The registry is asked once the context saw DLSS.
    frame_facts.dlss_released = addon::DlssReleased(state.bridge.Registry(), device, DlssApiOf(entry), entry.context->DlssSeen());
    NoteEarlyMarker(early_marker, &frame_facts);
    frame_facts.effects_on = (entry.runtime != nullptr && entry.runtime->get_effects_state());  // 1.1.6: NR before effects at their begin event
    frame_facts.dlss_motion_upside_down = entry.depth_upside_down.value_or(false);                // 1.1.6: DLSS's vectors flipped at Present
    if (entry.vulkan && addon::VkHolderDrainsNow(entry.vk_owner, addon::VkNrHolder::BRIDGE)) {
      frame_settings.session.grace = addon::NO_GRACE;  // transitions: the bridge is not NR's owner on this device: it drains at once
    }
    if (entry.vk_bridge) {
      const auto decision = DecideBeforeFgFor(state, entry, device, swapchain, before_fg_facts, addon::BeforeFgRoute::VK_BRIDGE);
      addon::ApplyBeforeFgBridge(entry.before_fg, frame_settings, frame_facts, decision);
      if (decision.state == addon::BeforeFgState::ARM_OUTER) {
        entry.context->NoteDisplayedPresent(now, frame_facts.ngx_frame_generation);
        return;
      }
    }
    addon::TargetInfo target = {
        .resource = reinterpret_cast<ID3D12Resource*>(entry.back_buffer.handle),
        .color_space = static_cast<color::ColorSpace>(swapchain->get_color_space()),
    };
    ID3D12CommandQueue* frame_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    // Owns a copy of the bridge's problem text: BridgeFrame::problem only views the bridge's own string until its
    // next call (RunBridged below calls into it again), so `target.problem` must not keep pointing into it.
    std::string bridge_problem;
    if (Bridged(entry)) {
      // The back buffer reaches NR through the bridge's shared copy, on the private device's queue.
      // The copy exists only while NR's state is not OFF; until then the Session loads from the
      // described size and format, and NR runs from the next frame.
      const bool running = (entry.context->NrState() != nr::SessionState::OFF);
      bridge::BridgeFrame frame;
      if (entry.vk_bridge) {
        // The retire fences of a bridge that just went off are submitted here, on the effect runtime's queue.
        addon::ReshadeVkHost host(device, entry.queue, entry.vk_bridge->VulkanDevice().vkQueueSubmit);
        frame = entry.vk_bridge->BeginFrame(host, VulkanImageInfo(device, entry.back_buffer), settings.enabled, running, now);
      } else if (entry.gl_bridge) {
        // FB0 at the present event is what NR runs on unless the Uplift technique is in the preset, where it is ReShade's single-sampled intermediate
        // (and MSAA is no problem: `at_present` decides). The GL names that waited for a valid host are deleted here, with the runtime's context current.
        addon::ReshadeGlHost host(entry.gl_bridge->Gl(), entry.queue, entry.present_queue);
        frame = entry.gl_bridge->BeginFrame(host, addon::GlImageInfo(device, entry.back_buffer), entry.marker.handle == 0u || frame_facts.marker_rendered_early, settings.enabled, running, now);
      } else if (entry.d3d10_bridge) {
        frame = entry.d3d10_bridge->BeginFrame(reinterpret_cast<ID3D10Resource*>(entry.back_buffer.handle), settings.enabled, running, now);
      } else {
        // At a DLSS stage NR runs inside the game's frame, so the back buffer is only described. Split chain: unless a later group runs at
        // Present (the chain of the latest present: a split's first frame at Present waits for the copy, as NR's first frame does).
        frame = entry.d3d11_bridge->BeginFrame(
            reinterpret_cast<ID3D11Resource*>(entry.back_buffer.handle), settings.enabled, running, now,
            /*present_copy=*/(!addon::IsDlssStage(entry.context->CurrentPlacement()) || entry.context->RunsAt(addon::Stage::PRESENT)));
      }
      // The private device's queue: the one bridge an arm above ran on, which a bridged device's context implies (the context is made only
      // after it, and the two go together).
      if (const bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge()) {
        frame_queue = private_bridge->Queue();
      }
      target.resource = frame.color;
      bridge_problem = std::string(frame.problem);
      target.problem = bridge_problem;
      target.size = frame.size;
      target.format = frame.format;
    }
    if (!Bridged(entry)) {
      const auto decision = DecideBeforeFgFor(state, entry, device, swapchain, before_fg_facts, addon::BeforeFgRoute::DX12);
      addon::ApplyBeforeFgDx12(entry.before_fg, frame_settings, frame_facts, decision, before_fg_facts.outer_matches_primary);
      if (decision.state == addon::BeforeFgState::ARM_OUTER) entry.context->NoteDisplayedPresent(now, frame_facts.ngx_frame_generation);
    }
    const addon::TriggerPoint point = entry.before_fg.outer_owns_frame ? addon::TriggerPoint::NONE
        : entry.context->BeginFrame(frame_queue, frame_settings, frame_facts, target, entry.marker.handle != 0u, now);
    if (entry.d3d11_bridge) {
      // The shares of what no longer runs go now: the DLSS stages' when NR runs at Present, the ring when its copies are not wanted (NR off
      // included, so "copied for Present" is logged once per activation).
      const addon::Placement placement = entry.context->CurrentPlacement();
      if (!addon::IsDlssStage(placement)) {
        entry.d3d11_bridge->ReleaseDlss();
      }
      if (!entry.context->PresentRingWanted() || entry.context->NrState() == nr::SessionState::OFF) {  // split chain: a later group at Present wants it too
        entry.d3d11_bridge->ReleasePresentMotion();
        entry.present_motion_logged = false;
      }
    }
    if (!Bridged(entry) && entry.context->DeviceLost()) {
      // The game's own Direct3D 12 device, found removed by this very BeginFrame (or earlier): the heartbeat's figures, once.
      if (const std::optional<addon::NrHeartbeat::Figures> figures = entry.heartbeat.Stop(now)) {
        LogHeartbeat(entry, device, "NR stopped after", *figures);
      }
    }
    const api::resource early_motion = (point == addon::TriggerPoint::EARLY_MARKER && early_marker.motion.handle ? device->get_resource_from_view(early_marker.motion) : api::resource{});
    if (Bridged(entry)) {
      RunBridged(entry, device, point, early_motion.handle, vk::Usage::PRESENT, entry.back_buffer);
    } else if (!entry.before_fg.outer_owns_frame) {
      ReshadeFrameHost host(queue, entry.back_buffer);
      RunOrWarnOnce(&state, entry, &host, D3D12_RESOURCE_STATE_PRESENT, point, reinterpret_cast<ID3D12Resource*>(early_motion.handle));
    }

    // The safety latch, persisted so the next start keeps the DLSS placements off, and
    // held for the rest of this session on every device.
    NoteLatchIfTripped(&state, entry.context.get());
    NoteLatchIfTripped(&state, entry.vk_dlss.get());  // A no-op without a native Vulkan context
    // The ini mirror runs here, on the present thread, whether this present's own
    // call just set the flag or an earlier evaluate or teardown call (on another thread) did.
    if (state.latch_save_pending) {
      state.latch_save_pending = false;
      SaveAndApply(&state);
    }
    // Tracking runs only while a DLSS placement can use it, so a game without DLSS (or before
    // its first evaluate) pays one relaxed load per tracking event. The first list reset after DLSS appears
    // is tracked; an evaluate on a list reset earlier skips once with nr::SkipReason::LIST_STATE_UNKNOWN. It is
    // also gated on the latch (or any other reason DLSS placements stopped being available mid-session).
    // Tracking runs whatever Source says now, so a live Source switch applies at once; a
    // Source = Present game still needs it once MotionVectors wants DLSS's motion vectors there too.
    // Never for a bridged device: a bridge never records on a game's Direct3D 12 list, and a Direct3D 11 DLSS game must not turn state tracking
    // on for a mod's device.
    addon::SetTracking(state.tracking_registered && state.settings.enabled && before.dlss_seen && !Bridged(entry)
                       && state.dlss_unavailable_reason.empty()
                       && (state.settings.source != ui::PlacementSource::PRESENT
                           || state.settings.motion_vectors == ui::MotionVectorSource::AUTO
                           || state.settings.motion_vectors == ui::MotionVectorSource::DLSS
                           || (entry.before_fg.outer_owns_frame && frame_settings.motion_vectors)));
    state.bridge.SetPreSr(state.settings.enabled && state.settings.pre_upscale
                          && state.settings.source != ui::PlacementSource::PRESENT
                          && state.dlss_unavailable_reason.empty() && !vk_dlss_stages_off);
  }
  UPLIFT_CATCH("present", )
}

// 1.1.6: NR before effects, when FrameTrigger waits for it here: after every add-on's present event (RenoDX's swap chain proxy draws over the back buffer in
// its own), before the first effect.
void OnBeginEffects(api::effect_runtime* runtime, api::command_list* cmd_list, api::resource_view rtv, api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    // A Direct3D 9 device keeps NR in the present event. So does a Vulkan device on the helper route (the helper replays the trigger point the
    // front reached; the front passes it no begin-effects wait, as in gitc-uplift.addon32).
    if (entry == nullptr || entry->helper.HasClient()) return;
    // ReShade renders into the back buffer, or into its own copy written back after the effects (an X8 or multisampled back buffer), which would paint over
    // NR's result: such a frame keeps NR in the present event. OpenGL's copy is the frame NR runs on, as at the technique.
    const api::resource target = runtime->get_device()->get_resource_from_view(rtv);
    if (NativePresent(*entry)) {
      RunNative(*entry, runtime->get_device(), entry->vk_dlss->OnBeginEffects(target == entry->back_buffer), vk::Usage::RENDER_TARGET);
      return;
    }
    const addon::TriggerPoint point = entry->context->OnBeginEffects(entry->opengl || target == entry->back_buffer);
    if (Bridged(*entry)) {
      RunBridged(*entry, runtime->get_device(), point, 0u, vk::Usage::RENDER_TARGET, (entry->opengl ? target : api::resource{0u}));
      return;
    }
    ReshadeFrameHost host(entry->queue, entry->back_buffer);
    RunOrWarnOnce(g_state, *entry, &host, D3D12_RESOURCE_STATE_RENDER_TARGET, point);
  }
  UPLIFT_CATCH("begin_effects", )
}

void OnRenderTechnique(api::effect_runtime* runtime, api::effect_technique technique, api::command_list* cmd_list,
                       api::resource_view rtv, api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    if (entry == nullptr) return;
    if (addon::ObserveEarlyTechnique(entry->motion_links, entry->marker, runtime, technique, rtv)) return;
    const bool is_marker = (technique.handle == entry->marker.handle);
    if (entry->helper.HasClient()) {
      g_state->front->Technique(entry->helper, runtime, is_marker, rtv.handle, g_state->settings);  // A Direct3D 9 device; a helper-route Vulkan one
      return;
    }
    // At the Uplift technique, the UPLIFT_MV its pass just wrote, while LaunchPad runs, or Lumenite's Kernel,
    // whose vectors Uplift.fx writes into the same UPLIFT_MV.
    // The same lookup, API-neutral, so the bridged branch below (D3D11 and D3D10) can use it too.
    api::resource launchpad = {0u};
    api::resource_view launchpad_view = {0u};  // native NR at Present samples UPLIFT_MV through ReShade's own view
    if (is_marker) {
      // 1.2.1: only while the technique of the source Uplift.fx is compiled with is on; the other one's being on says nothing about UPLIFT_MV, which
      // then holds the compiled source's last vectors, frozen (a reload that re-applied a preset).
      const api::effect_technique launchpad_technique = runtime->find_technique(nullptr, addon::LAUNCHPAD_TECHNIQUE);
      const api::effect_technique lumenite_technique = runtime->find_technique(nullptr, addon::LUMENITE_TECHNIQUE);
      const bool source_on = addon::UpliftMvValid(entry->uplift_mv_source,
                                                  (launchpad_technique.handle != 0u && runtime->get_technique_state(launchpad_technique)),
                                                  (lumenite_technique.handle != 0u && runtime->get_technique_state(lumenite_technique)));
      const api::effect_texture_variable motion = runtime->find_texture_variable(nullptr, uplift::text::UPLIFT_MOTION_TEXTURE);
      if (source_on && motion.handle != 0u) {
        api::resource_view view_srgb = {0u};
        runtime->get_texture_binding(motion, &launchpad_view, &view_srgb);
        if (launchpad_view.handle != 0u) {
          launchpad = runtime->get_device()->get_resource_from_view(launchpad_view);
        }
      }
    }
    if (NativePresent(*entry)) {
      RunNative(*entry, runtime->get_device(), entry->vk_dlss->OnTechnique(is_marker), vk::Usage::RENDER_TARGET, launchpad_view);
      return;
    }
    if (Bridged(*entry)) {
      // UPLIFT_MV crosses only when MotionVectors can use it (a 32 MiB copy at 4K otherwise).
      const bool launchpad_wanted = addon::MotionUsesUpliftMv(g_state->settings.motion_vectors);  // Auto, Launchpad or Lumenite
      // An OpenGL device's frame here is ReShade's intermediate, the event's rtv, not FB0 (which still holds the game's pre-effects frame).
      RunBridged(*entry, runtime->get_device(), entry->context->OnTechnique(is_marker), (launchpad_wanted ? launchpad.handle : 0u),
                 vk::Usage::RENDER_TARGET, (entry->opengl ? runtime->get_device()->get_resource_from_view(rtv) : api::resource{0u}));
      return;
    }
    ReshadeFrameHost host(entry->queue, entry->back_buffer);
    RunOrWarnOnce(g_state, *entry, &host, D3D12_RESOURCE_STATE_RENDER_TARGET, entry->context->OnTechnique(is_marker),
                  reinterpret_cast<ID3D12Resource*>(launchpad.handle));
  }
  UPLIFT_CATCH("render_technique", )
}

void OnFinishEffects(api::effect_runtime* runtime, api::command_list* cmd_list, api::resource_view /*rtv*/,
                     api::resource_view /*rtv_srgb*/) {
  try {
    const std::unique_lock lock(g_state->mutex);
    DeviceEntry* const entry = FindFrameEntry(g_state, runtime, cmd_list);
    // A Vulkan device whose native context runs NR at a DLSS placement (or natively at Present) takes UPLIFT_MASK on the game's
    // own device. FindFrameEntry needs a bridged context or native Present, so the entry is looked up here, for this copy alone.
    DeviceEntry* native = nullptr;
    if (const auto found = g_state->devices.find(runtime->get_device()); found != g_state->devices.end()) {
      DeviceEntry& candidate = found->second;
      const bool native_view = (candidate.vk_dlss && (candidate.vk_dlss->WantsNr() || candidate.vk_dlss->NrState() != nr::SessionState::OFF));
      if (native_view && candidate.runtime == runtime && candidate.queue != nullptr && cmd_list == candidate.queue->get_immediate_command_list()) {
        native = &candidate;
      }
    }
    if (entry == nullptr && native == nullptr) return;
    if (entry != nullptr && entry->helper.HasClient() && native == nullptr) {
      // A Direct3D 9 device. A Vulkan device on the helper route, unless its native context runs NR at a DLSS placement (the helper yields).
      g_state->front->FinishEffects(entry->helper, runtime, g_state->settings);
      return;
    }
    if (const auto found = g_state->devices.find(runtime->get_device()); found != g_state->devices.end() && found->second.before_fg.outer_owns_frame) return;
    // This frame's UPLIFT_MASK, copied now that the effects have run, on this same list,
    // into Uplift's own texture; every placement binds that copy from the next recording on.
    if (g_state->settings.mask != ui::MaskMode::AUTO) return;
    const api::effect_texture_variable variable = runtime->find_texture_variable(nullptr, uplift::text::UPLIFT_MASK_TEXTURE);
    if (variable.handle == 0u) return;
    DeviceEntry& masked = (native != nullptr ? *native : *entry);
    // The variable can still exist, with its last content, after every technique of its
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
      // Into the native context's image on the game's device, through ReShade's command list (as Direct3D 12's copy below). ReShade keeps effect
      // textures in shader_resource between techniques; the copy rests there between copies, and is UNDEFINED before its first.
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
    if (!entry->context) return;  // native Present whose context stopped (a loss): no copy anywhere
    if (Bridged(*entry)) {
      // UPLIFT_MASK crosses in the bridge's shared mask; the next bridged recording copies it
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
      // "not supported" only for a format NR cannot read; any other failure is the share's.
      if (copied.size) {
        entry->mask_note = std::format("NR mask: UPLIFT_MASK ({}x{})", copied.size->width, copied.size->height);
      } else if (copied.unsupported) {
        entry->mask_note = "NR mask: UPLIFT_MASK's format is not supported";
      } else if (copied.relay_failed) {
        entry->mask_note = "NR mask: UPLIFT_MASK could not be shared with Uplift's relay device";  // D3D10's keyed share
      } else {
        entry->mask_note = "NR mask: UPLIFT_MASK could not be shared with the private Direct3D 12 device";
      }
      return;
    }
    const D3D12_RESOURCE_DESC description = reinterpret_cast<ID3D12Resource*>(mask.handle)->GetDesc();
    ID3D12Resource* const copy = entry->context->PrepareMaskCopy(&description);
    if (copy == nullptr) {
      // Split the two causes PrepareMaskCopy's nullptr can mean. The format check
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
    const auto found = state.devices.find(runtime->get_device());
    if (found != state.devices.end() && found->second.runtime == runtime && !found->second.before_fg.outer_owns_frame) {
      DeviceEntry& entry = found->second;
      entry.motion_links.early.BeginObserving();
      if (entry.helper.HasClient()) entry.helper.trigger.EndFrame();
      else if (NativePresent(entry)) entry.vk_dlss->EndFrame();
      else if (entry.context) entry.context->EndFrame();
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

// Why the DLSS stages cannot run on a Vulkan device: the session's reason, what native NR lacks here, or the native context's
// stop for good (the game shut NGX down on the device, or it was lost: nothing inside the game's frame runs again this session). Empty when they can.
std::string VulkanDlssReason(const AddonState& state, const DeviceEntry& entry) {
  if (!state.dlss_unavailable_reason.empty()) return state.dlss_unavailable_reason;
  if (std::string problem = addon::VkNativeDlssProblem(entry.vk_native_block); !problem.empty()) return problem;
  if (entry.vk_dlss && !entry.vk_dlss->CanCopy()) return std::string(ui::VULKAN_NGX_SHUT_DOWN_REASON);
  return {};
}

// The VulkanNr row highlights the route that runs, greys each choice that cannot run here with why, and says why the route
// differs from the stored choice. The helper route too (Helper is never greyed). With the add-on's lock held.
void FillVulkanRoute(const AddonState& state, const DeviceEntry& entry, ui::OverlayView* view) {
  if (!entry.vulkan || !entry.vk_route) return;
  const addon::VkRouteChoice& route = *entry.vk_route;
  ui::VulkanNrMode running = ui::VulkanNrMode::NATIVE;
  std::string_view running_on;
  switch (route.route) {
    case addon::VkNrRoute::NATIVE: break;
    case addon::VkNrRoute::DIRECT3D_12:
      running = ui::VulkanNrMode::DIRECT3D_12;
      running_on = "a private Direct3D 12 device";
      break;
    case addon::VkNrRoute::HELPER:
      running = ui::VulkanNrMode::HELPER;
      running_on = "Uplift's helper process";
      break;
  }
  view->vulkan_nr_running = running;
  view->vulkan_native_unavailable = route.native_unavailable;
  view->vulkan_d3d12_unavailable = route.d3d12_unavailable;
  if (running != state.settings.vulkan_nr) {
    view->vulkan_nr_note = std::format("Now: {} ({})", running_on, route.reason);
  }
}

// The panel shows the helper front's lines and card for a Vulkan device whose NR at Present takes the helper route (or still drains there), unless its
// native context wants or holds NR (a DLSS placement, or its drain after the native route).
bool HelperView(const DeviceEntry& entry) {
  const bool native_view = (entry.vk_dlss && (entry.vk_dlss->WantsNr() || entry.vk_dlss->NrState() != nr::SessionState::OFF));
  return entry.vulkan && !native_view && (entry.helper.HasClient() || (entry.vk_route && entry.vk_route->route == addon::VkNrRoute::HELPER));
}

// `entry`'s status card, from its Status() facts, for the overlay.
// `status` is null when the device has no context. Called with the add-on's lock held.
ui::StatusCard CardFor(const AddonState& state, const DeviceEntry& entry, const addon::ContextStatus* status,
                       std::string_view working_line) {
  if (status == nullptr) {
    // A D3D11 device makes its private D3D12 device only at the first present with NR on (D3D10 too). An OpenGL context that presents
    // from another context than the runtime's has no bridge yet and says why.
    if (Bridged(entry) && !entry.rejected && entry.message.empty()) return ui::BuildStatusCard({.enabled = state.settings.enabled});
    return ui::BuildStatusCard({
        .device_problem = (entry.message.empty() ? std::string_view("Waiting for the game's first frame")
                                                 : std::string_view(entry.message)),
    });
  }
  // A stopped private-device bridge is its own card (Retry now, or the second time the restart); its latch text names what stopped.
  const bool private_stopped = (Bridged(entry) && (entry.strikes.Stopped() || PrivateDeviceStopped(entry)));  // the present counts it next
  std::string_view private_reason = BridgeLatch(entry);
  if (private_reason.empty()) {
    private_reason = status->output_problem;
  }
  const std::string before_fg_note = state.settings.fg_present == ui::Settings::FgPresent::BEFORE ? entry.before_fg.note.Text() : std::string{};
  return ui::BuildStatusCard({
      .private_stopped = (private_stopped ? private_reason : std::string_view()),
      .private_stops_final = (Bridged(entry) && entry.strikes.Exhausted()),
      .private_stop_unretryable = (Bridged(entry) && entry.strikes.Unretryable()),
      .device_lost = status->device_lost,
      .stopped = (status->abandoned ? ui::D3D12_NGX_ABANDONED_REASON : std::string_view()),
      .blocked = status->blocked,
      .blocked_stage = (entry.static_block.empty() ? ui::CardStage::CONFLICTS : entry.block_stage),
      .runtime_in_game_folder = entry.runtime_in_game_folder,  // 1.1.4
      .claimed_elsewhere = status->claimed_elsewhere,
      .enabled = state.settings.enabled,
      .held = status->held,
      .session = status->session,
      .output_problem = status->output_problem,
      .placement_note = status->placement_note,
      .dlss_latched = state.settings.dlss_placement_blocked,
      .nr_applied = status->nr_applied,
      .passes_run = status->passes_run,
      .skip_reason = status->skip_reason,
      .skip_from_session = status->skip_from_session,
      .working_line = working_line,
      .before_fg_note = before_fg_note,
      .vram_check = state.settings.vram_check,
  });
}

void OnDrawOverlay(api::effect_runtime* runtime) {
  try {
    const std::unique_lock lock(g_state->mutex);
    AddonState& state = *g_state;
    const auto now = std::chrono::steady_clock::now();
    ui::OverlayView view = {.status_line = "OFF", .last_key_pressed = runtime->last_key_pressed()};
    const auto found = state.devices.find(runtime->get_device());
    const api::device_api overlay_device_api = runtime->get_device()->get_api();
    // On Vulkan, Before upscaling and After DLSS are explicit choices, offered where native NR can run.
    view.dlss_explicit = (overlay_device_api == api::device_api::vulkan);
    view.cross_queue_passes_available = (sizeof(void*) == 8u && overlay_device_api == api::device_api::d3d12);
    // Setup's facts. The stored preference here; what is possible and what runs below, per path; the draw resolves them.
    ui::SetupFacts facts;
    std::string after_dlss_problem;  // Setup's facts view these; they outlive FinishSetup
    std::string before_upscaling_problem;
    std::string chain_note;  // split chain: the card's note line
    addon::FillPreference(&facts, state.settings, view.dlss_explicit);
    bool setup_finished = false;  // the helper front finished Setup (a Direct3D 9 device, a Vulkan device on the helper route)
    if (overlay_device_api != api::device_api::d3d9) {
      view.d3d9ex_unavailable = "Only for Direct3D 9 games";  // the 9Ex toggle works in 64-bit Direct3D 9 games too
    }
    if (overlay_device_api != api::device_api::vulkan) {
      view.vulkan_nr_unavailable = "Only for Vulkan games";
    }
    if (overlay_device_api != api::device_api::d3d12 && overlay_device_api != api::device_api::d3d11
        && overlay_device_api != api::device_api::d3d10 && overlay_device_api != api::device_api::d3d9
        && overlay_device_api != api::device_api::vulkan && overlay_device_api != api::device_api::opengl) {
      view.card = ui::BuildStatusCard({.device_problem = "Uplift supports Direct3D 9, 10, 11 and 12, Vulkan and OpenGL games only"});
      view.dlss_unavailable = state.dlss_unavailable_reason;
      view.keep_faces_unavailable = std::string(ui::KeepFacesUnavailable({.nr_runs = false}));
    } else if (overlay_device_api == api::device_api::d3d9) {
      // The helper front's lines and card; no DLSS placement without a Direct3D 12 game.
      view.dlss_unavailable = std::string(addon::BRIDGED_DLSS_REASON);
      state.front->Overlay(runtime->get_device(), (found != state.devices.end() ? &found->second.helper : nullptr), overlay_device_api,
                           state.settings, &view, &state.overlay, now);  // finishes Setup too
      setup_finished = true;
    } else if (found == state.devices.end()) {
      view.card = ui::BuildStatusCard({.device_problem = "Waiting for the game's first frame"});
      view.dlss_unavailable = state.dlss_unavailable_reason;
    } else if (HelperView(found->second)) {
      // NR at Present runs in Uplift's helper process: the helper front's lines, card and Setup, as for a Direct3D 9 device. The DLSS
      // stages (and DLSS's vectors, which never reach the helper) are greyed on this route, with why.
      view.dlss_unavailable = VulkanDlssReason(state, found->second);
      view.vulkan_stages_off = found->second.vk_stages_off;  // the stages alone; DLSS's vectors get their own reason (HelperFront)
      FillVulkanRoute(state, found->second, &view);
      state.front->Overlay(runtime->get_device(), &found->second.helper, overlay_device_api, state.settings, &view, &state.overlay, now);  // finishes Setup too
      setup_finished = true;
    } else {
      [[maybe_unused]] const auto& [device, entry] = *found;
      // Known before a context exists, so a D3D11 game greys the DLSS toggles from
      // its first frame.
      // A Direct3D 11 device runs the DLSS stages through its bridge; its own reasons (a foreign Direct3D 12 DLSS device) replace the
      // bridged one.
      view.dlss_unavailable = (entry.d3d11   ? std::string(D3D11DlssReason(state, entry))
                               : Bridged(entry) ? std::string(addon::BRIDGED_DLSS_REASON)
                                                : state.dlss_unavailable_reason);
      // The native Vulkan context stopped for good (the game shut NGX down on the device, or it was lost): nothing inside the game's frame
      // runs again this session, so the DLSS stages and DLSS's vectors are a fixed cause.
      const bool native_stopped = (entry.vk_dlss && !entry.vk_dlss->CanCopy());
      if (entry.vulkan) {
        view.dlss_unavailable = VulkanDlssReason(state, entry);
      }
      // While the native Vulkan context wants or holds NR, the panel shows it; the bridge's Present otherwise.
      const bool native_view = (entry.vk_dlss && (entry.vk_dlss->WantsNr() || entry.vk_dlss->NrState() != nr::SessionState::OFF));
      // The VulkanNr row; the card's Details line names the route too.
      const bool native_present = NativePresent(entry);
      FillVulkanRoute(state, entry, &view);
      // Stopped at a DLSS stage, the native context keeps NR's claim (shutdown protection: the Present path never loads NR into a game
      // that may be quitting), so Present cannot run either; what is highlighted is the stage it stopped at, where the card says NR stays off.
      if (native_view && native_stopped) {
        facts.present_fixed = ui::VULKAN_NGX_SHUT_DOWN_PRESENT_REASON;
      }
      // What is possible here. The game created a DLSS feature on this device (sticky; the NGX hooks feed it in every mode), and
      // whether the main one is Ray Reconstruction.
      // On Direct3D 12 only a real evaluate counts (added below from the context's status); a create counts on Vulkan, where the passthrough
      // context sees no evaluate until a DLSS pick makes it watch.
      facts.dlss_seen = addon::SetupDlssSeen(entry.vulkan, state.bridge.Registry().UpscalerCreated(device), (entry.vk_dlss && entry.vk_dlss->DlssSeen()));
      const auto observed_feature = entry.vulkan
          ? (entry.vk_dlss ? entry.vk_dlss->ObservedDlssFeature() : std::nullopt)
          : (entry.context ? entry.context->ObservedDlssFeature() : std::nullopt);
      facts.ray_reconstruction = addon::SetupRayReconstruction(state.bridge.Registry(), device, entry.swapchain_size,
                                                              DlssApiOf(entry), observed_feature);
      // Before upscaling runs on Ray Reconstruction on Direct3D 12 and Vulkan; the Direct3D 11 bridge keeps
      // DLSS-SR only.
      facts.ray_reconstruction_after_dlss = !addon::PreSrFeatureAllowed(NVSDK_NGX_Feature_RayReconstruction, DlssApiOf(entry));
      facts.before_fg_late_effects = state.settings.fg_present == ui::Settings::FgPresent::BEFORE && !entry.before_fg.early_confirmed && entry.before_fg.note.id != BeforeFgNote::NONE;
      facts.launchpad_ready = entry.launchpad_ready;
      facts.lumenite_ready = entry.lumenite_ready;
      // Split chain: as FrameFacts::split_capable, the native Vulkan context's included.
      facts.split_capable = addon::SplitCapable(Bridged(entry), entry.d3d11,
                                                (entry.vulkan && native_view && state.settings.vulkan_nr == ui::VulkanNrMode::NATIVE && entry.vk_stages_off.empty()));
      facts.uplift_mv_lumenite = entry.uplift_mv_lumenite;
      facts.match_game_readable = (!Bridged(entry) || entry.d3d11 || native_view);  // the bridges never see the game's DLSS create (Direct3D 11's context reads it)
      // At Present on Vulkan a DLSS stage reads it when it can run here, so Match game waits for one (temporary).
      facts.match_game_at_dlss_stages = (entry.vulkan && !native_view && view.dlss_unavailable.empty());
      if (entry.vulkan && (!native_view || native_present)) {
        // At Present on Vulkan DLSS's vectors reach NR through the copies the native context makes in the game's frame: on a GPU-ordered bridge only,
        // a game that presents on the effect runtime's queue, and while the native context can still copy. The bridge's order is known from the
        // device's record before the bridge exists (VkBridge::Create's rule); only a refused fence import is unknowable until it does.
        // Natively at Present no bridge orders anything: only the second queue matters.
        const std::optional<vk::DeviceRecord> record =
            ((entry.vk_bridge || native_present) ? std::nullopt : vk::DeviceHook::Find(reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device->get_native()))));
        const bool cpu_ordered = (native_present  ? false
                                  : entry.vk_bridge ? !entry.vk_bridge->GpuOrdered()
                                                    : (record && !(record->semaphore_win32 && record->timeline && record->memory_win32)));
        if (cpu_ordered) {
          facts.dlss_motion_fixed = ui::VULKAN_CPU_ORDERED_MOTION;
        } else if (entry.present_queue != nullptr && entry.queue != nullptr && entry.present_queue != entry.queue) {
          facts.dlss_motion_fixed = ui::VULKAN_SECOND_QUEUE_MOTION;
        }  // a stopped native context: dlss_unavailable above
      }
      facts.api = addon::host::ApiName(api::device_api::d3d12);
      if (entry.d3d11) {
        facts.api = addon::host::ApiName(api::device_api::d3d11);
      } else if (entry.d3d10) {
        facts.api = addon::host::ApiName(api::device_api::d3d10);
      } else if (entry.opengl) {
        facts.api = addon::host::ApiName(api::device_api::opengl);
      } else if (entry.vulkan) {
        facts.api = (native_view ? "Vulkan (native NR)" : "Vulkan (Direct3D 12)");  // the route
      }
      if (native_view || entry.context) {
        const addon::ContextStatus status = (native_view ? entry.vk_dlss->Status() : entry.context->Status());
        const auto fg_mode = addon::streamline::Mode();
        const bool early_fg = entry.before_fg.early_confirmed && addon::streamline::FrameGenerationActive(fg_mode, status.frame_generation.active);
        if (state.settings.enabled && status.session.state == nr::SessionState::ACTIVE) {
          for (uint32_t i = 0; i < status.group_count; ++i) {
            const auto& group = status.groups[i];
            uint32_t count = group.nr_applied ? group.passes_run : 0u;
            size_t node = static_cast<size_t>(group.stage);
            if (group.stage == addon::Stage::PRESENT) {
              node = (entry.before_fg.outer_owns_frame || early_fg) ? 2u : 3u;
              if (entry.before_fg.outer_owns_frame) count = entry.before_fg.outer_present_passes;
            }
            auto& row = view.stage_runs[node];
            row = {group.first, std::min(group.count, count), group.canvas.Empty() ? group.work : group.canvas, group.motion_source, group.count, std::string(nr::SkipReasonText(group.skip_reason))};
            if (!row.count && group.stage == addon::Stage::PRESENT && entry.before_fg.note.id != BeforeFgNote::NONE) row.skip = entry.before_fg.note.Text();
          }
        }
        if (status.abandoned || (status.device_lost && !Bridged(entry))) {
          view.safety_locks.push_back({"NR on this device", status.device_lost ? "The game lost its graphics device." : std::string(ui::D3D12_NGX_ABANDONED_REASON)});
        }
        if (status.session.retries_exhausted) view.safety_locks.push_back({"Automatic NR retries", status.session.message, nullptr, true, true});
        if (!status.before_upscaling_problem.empty()) view.safety_locks.push_back({"Pre-DLSS", status.before_upscaling_problem});
        if (!status.after_dlss_problem.empty()) view.safety_locks.push_back({"After DLSS", status.after_dlss_problem});
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
        view.details_backend = facts.api;
        if (Bridged(entry) && !entry.vulkan) view.details_backend = std::format("{} (Direct3D 12 bridge)", facts.api);
        view.details_note = status.chain_note;
        view.lumenite_motion = entry.uplift_mv_lumenite;
        // Split chain: the chain's groups while it is split, and why a stored "Runs at" does not run as stored (the working card's note too).
        chain_note = status.chain_note;
        facts.chain_note = chain_note;
        view.mask_note = entry.mask_note;
        view.ui_correction_note = status.ui_correction_note;
        view.keep_faces_unavailable = std::string(ui::KeepFacesUnavailable({.gpu_ready = status.keep_faces_supported}));
        view.frame_generation_line = ui::FormatFrameGenerationLine(
            status.frame_generation.active, status.frame_generation.multiplier, status.frame_generation.from_ngx);
        view.frame_generation_active = status.frame_generation.active;
        if (const std::optional<std::string> warning =
                ui::FrameGenerationWarning(status.frame_generation.active, state.settings.pass_count)) {
          view.frame_generation_warning = *warning;
        }
        view.intermediate_bytes = status.intermediate_bytes;
        if (entry.vk_bridge) {
          if (!native_view) {
            // A Vulkan bridge's shared surfaces count as Uplift's while the panel shows the bridge, not the native context.
            view.intermediate_bytes += entry.vk_bridge->SharedBytes();
            // And so do the four copies of DLSS's motion vectors the native context holds on the game's device while it makes them.
            if (const uint64_t copies = (entry.vk_dlss ? entry.vk_dlss->HeldBytes() : 0u); copies > 0u) {
              view.intermediate_bytes += copies;
            }
          }
        } else if (const bridge::PrivateDeviceBridge* const private_bridge = entry.ActiveBridge()) {
          // The bridge's shared textures count as Uplift's (on Direct3D 10 the relay's keyed ones too).
          view.intermediate_bytes += private_bridge->SharedBytes();
        }
        view.runtime_bytes = status.session.runtime_bytes;
        view.pass_models = status.session.pass_models;
        view.legacy_runtime = status.session.legacy_runtime;
        view.effective_diffuse_white_nits = status.diffuse_white_nits;
        view.encoding = status.encoding;
        view.card = CardFor(state, entry, &status, view.status_line);
        addon::FillRunning(&facts, status, view.card.working);
        after_dlss_problem = status.after_dlss_problem;  // an image the bridge cannot share greys its stage
        before_upscaling_problem = status.before_upscaling_problem;
        facts.after_dlss_fixed = after_dlss_problem;
        facts.before_upscaling_fixed = before_upscaling_problem;
        if (entry.vulkan && (!native_view || native_present) && entry.vk_dlss) {
          // At Present on Vulkan the bridged context never sees the game's evaluate; the native context made (or missed) the copies
          // (for its own Present too).
          facts.dlss_motion_gap = entry.vk_dlss->PresentMotionGap();
          facts.game_passes_motion = (facts.dlss_motion_gap != ui::MotionGap::GAME_PASSED_NONE);
        }
        facts.dlss_seen = facts.dlss_seen || status.dlss_seen;
        // After the game's NGX shutdown on Direct3D 12, every option waits for the game's DLSS (temporary); abandoned there, nothing runs again this
        // session (every option greyed with the fixed reason, the highlights included).
        facts.held = status.held;
        if (status.abandoned) {
          view.dlss_unavailable = std::string(ui::D3D12_NGX_ABANDONED_REASON);
          facts.stopped = ui::D3D12_NGX_ABANDONED_REASON;
        }
        if (Bridged(entry) && entry.strikes.Exhausted()) {
          facts.stopped = ui::PRIVATE_DEVICE_STOPPED_TWICE_REASON;  // the 2-strike rule's second stop is a fixed cause
        } else if (Bridged(entry) && entry.strikes.Unretryable()) {
          facts.stopped = ui::PRIVATE_DEVICE_STOPPED_FINAL_REASON;  // a stop on the shared device, final at once
        }
        facts.frame_generation_blocks_present = (status.frame_generation.active && state.settings.fg_present == ui::Settings::FgPresent::OFF);
        facts.vram_bytes = view.runtime_bytes.value_or(0u) + view.intermediate_bytes;
        facts.latch_note = (state.settings.dlss_placement_blocked ? addon::LATCH_UNAVAILABLE_REASON : std::string_view());
        facts.passes_note = view.card.WorkingNote();
        facts.frame_generation_line = (view.frame_generation_active ? std::string_view(view.frame_generation_line) : std::string_view());
      } else {
        view.card = CardFor(state, entry, nullptr, {});
      }
      // On the Direct3D 12 route (chosen, or where the chain landed) only Present runs: the DLSS stages are a fixed cause, with why.
      if (!entry.vk_stages_off.empty()) {
        facts.after_dlss_fixed = entry.vk_stages_off;
        facts.before_upscaling_fixed = entry.vk_stages_off;
      }
      // DLSS seen here, and the game switched it off since (a release, or on Direct3D 11 an NGX shutdown): the DLSS stages, DLSS's vectors
      // and Match game wait for it (temporary), Present runs. The core-shutdown hold, the stopped Vulkan context and the fixed causes keep their own text (ResolveSetup).
      facts.dlss_off = addon::DlssReleased(state.bridge.Registry(), device, DlssApiOf(entry), facts.dlss_seen);
    }
    // Clear latch also clears native Vulkan NR's latch (VulkanNativeNr), from the next start, so it shows in a Vulkan game while that is set.
    view.dlss_latched = (state.settings.dlss_placement_blocked || (overlay_device_api == api::device_api::vulkan && !state.settings.vulkan_native_nr));
    if (!setup_finished) {
      facts.dlss_unavailable = view.dlss_unavailable;
      ui::FinishSetup(&view, &state.overlay, runtime->get_device(), facts, now);
    }
    const ui::VulkanNrMode vulkan_nr_before = state.settings.vulkan_nr;
    if (found != state.devices.end() && setup_finished && found->second.vulkan) {
      const auto& entry = found->second;
      if (entry.before_fg.outer_owns_frame && entry.before_fg.helper_frame) {
        auto& before = view.stage_runs[2];
        before.first = 0u;
        before.count = state.settings.enabled ? entry.before_fg.outer_present_passes : 0u;
        view.stage_runs[3] = {};
      } else if (entry.before_fg.early_confirmed && addon::BeforeFgActive({
                     .enabled = state.settings.enabled,
                     .requested = state.settings.fg_present,
                     .mode = addon::streamline::Mode(),
                     .ngx_live = state.bridge.Registry().LiveCount(ngx_hooks::FeatureKind::FRAME_GENERATION),
                 })) {
        view.stage_runs[2] = view.stage_runs[3];
        view.stage_runs[3] = {};
      }
      view.frame_generation_active = view.frame_generation_active || entry.before_fg.outer_owns_frame || view.stage_runs[2].count > 0u;
    }
    if (found != state.devices.end()) {
      const auto& entry = found->second;
      view.output_size = entry.swapchain_size;
      view.output_space = entry.output_space;
      view.frame_generation_active = addon::streamline::FrameGenerationShown(addon::streamline::Mode(), view.frame_generation_active,
                                                                             entry.before_fg.outer_owns_frame);
      if (view.frame_generation_active && entry.before_fg.activity.real && (entry.before_fg.outer_owns_frame || (!HelperView(entry) && (!Bridged(entry) || NativePresent(entry))))) {
        view.activity = entry.before_fg.activity;
      }
    }
    // Before FG greyed, with why, where frame generation cannot run: not a Direct3D 12 or Vulkan device, no Streamline hooks, a game that
    // has not loaded Streamline, or frame generation not running, as the diagram's FG box shows it (the choice returns by itself once it runs).
    view.before_fg_unavailable = std::string(ui::BeforeFgUnavailable({
        .api_supported = (overlay_device_api == api::device_api::d3d12 || overlay_device_api == api::device_api::vulkan),
        .hooks_installed = addon::streamline::Installed(),
        .streamline_loaded = addon::streamline::StreamlineLoaded(),
        .frame_generation_running = view.frame_generation_active,
    }));
    if (found != state.devices.end()) view.AddBeforeFgNote(found->second.before_fg.note);
    const bool vk = overlay_device_api == api::device_api::vulkan;
    view.AddStartupLocks(state.settings, vk, overlay_device_api == api::device_api::d3d9);
    const bool has_dlss_stages = vk || overlay_device_api == api::device_api::d3d12 || overlay_device_api == api::device_api::d3d11;
    if (has_dlss_stages && (state.settings.dlss_placement_blocked || state.dlss_unavailable_reason == addon::LATCH_UNAVAILABLE_REASON)) {
      view.safety_locks.push_back({"Pre-DLSS and After DLSS", "DLSS placements are blocked following a device removal.", &ui::Settings::dlss_placement_blocked, false});
    }
    const bool native_latched = vk && (!state.settings.vulkan_native_nr || !state.vk_native_off.empty());
    if (native_latched) {
      const char* reason = "Native Present is blocked for this game after an unsuccessful start or a device loss.";
      if (view.vulkan_nr_running == ui::VulkanNrMode::DIRECT3D_12) reason = "Native Present is blocked. Present uses the Direct3D 12 bridge.";
      if (view.vulkan_nr_running == ui::VulkanNrMode::HELPER) reason = "Native Present is blocked. Present uses the 64-bit helper.";
      view.safety_locks.push_back({"Native Vulkan NR at Present", reason, &ui::Settings::vulkan_native_nr});
    }
    if (found != state.devices.end()) {
      const auto& entry = found->second;
      if (vk) {
        if (entry.vk_dlss && !entry.vk_dlss->CanCopy() && !entry.vk_dlss->Abandoned()) {
          view.safety_locks.push_back({"Native Vulkan DLSS stages and motion capture", std::string(ui::VULKAN_NGX_SHUT_DOWN_REASON)});
        }
        const std::string native_reason = addon::VkNativeLockReason(entry.vk_native_stopped, entry.vk_native_block);
        if (!native_latched && !native_reason.empty()) view.safety_locks.push_back({"Native Vulkan NR at Present", native_reason});
        const std::string bridge_reason = (entry.vk_d3d12_block ? entry.vk_d3d12_block->detail : std::string());
        if (!bridge_reason.empty()) view.safety_locks.push_back({"Vulkan to Direct3D 12 bridge", bridge_reason});
      }
      if (entry.strikes.Stopped() || !BridgeLatch(entry).empty()) {
        const auto reason = BridgeLatch(entry);
        view.safety_locks.push_back({"NR bridge", reason.empty() ? std::string("The private rendering device stopped.") : std::string(reason),
                                     nullptr, true, entry.strikes.RetryAllowed() && !HelperView(entry)});
      }
    }
    if (ui::DrawOverlay(view, &state.settings, &state.overlay)) {
      if (overlay_device_api == api::device_api::vulkan && MoveStageForVkRoute(&state.settings, vulkan_nr_before)) {
        state.overlay.setup_settle.Clicked(ui::SetupRow::STAGE, static_cast<uint32_t>(ui::SourcePick::PRESENT));  // as a click on Present
      }
      if (view.dlss_latched && !state.settings.dlss_placement_blocked) {
        // Clear latch: only the persisted record goes away here (the in-session latch keeps the placements off
        // for the rest of THIS session on purpose, as the message itself says: restart to try again).
        addon::DeleteLatchMarker(state.latch_marker_path);
      }
      SaveAndApply(&state);
    } else if (state.overlay.language_changed) {
      ui::SaveSettings(state.settings, &state.config);
    }
    // Retry now, decided by the card's button; never after a removal or a teardown
    // (DeviceContext::RetryNow's own guard).
    if (std::exchange(state.overlay.retry_now, false)) {
      if (overlay_device_api == api::device_api::d3d9) {
        state.front->RetryNow(found != state.devices.end() ? &found->second.helper : nullptr);
      } else if (found != state.devices.end() && HelperView(found->second)) {
        state.front->RetryNow(&found->second.helper);  // the card shown was the helper front's
      } else if (found != state.devices.end() && found->second.vk_dlss
                 && (found->second.vk_dlss->WantsNr() || found->second.vk_dlss->NrState() != nr::SessionState::OFF)) {
        // The card shown was the native context's. Checked before the bridge's, since every Vulkan device counts as bridged and native
        // Present has no bridge to tear down.
        found->second.vk_dlss->RetryNow();
      } else if (found != state.devices.end() && Bridged(found->second) && found->second.strikes.RetryAllowed()) {
        found->second.retry_pending = true;  // the card shown was the stopped bridge's; the next present tears it down
      } else if (found != state.devices.end() && found->second.context) {
        found->second.context->RetryNow();
      }
    }
  }
  UPLIFT_CATCH("draw_overlay", )
}

// DeviceHook's vkDestroyDevice callback, on the game's thread before ReShade's own vkDestroyDevice (inside the detour's try), so
// NR on the device is released while ReShade still dispatches it. The game has idled the device. The context goes after the lock.
void OnVulkanDeviceDestroyed(VkDevice vk_device) {
  std::unique_ptr<addon::VkDlssContext> torn_down;
  const std::unique_lock lock(g_state->mutex);
  if (addon::streamline::Installed()) g_state->before_fg_vk.DestroyDevice(vk_device);
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

// Compute pipelines NVIDIA's DLSS binds inside its own Vulkan evaluate, seen through
// ReShade's layer. NGX's and Streamline's contract makes the game bind its own state again after DLSS, and Uplift replays none on Vulkan:
// this records, once in the log, whether DLSS-SR itself disturbs the compute bind point. Lock-free: one TLS read for every other bind.
void OnVulkanBindPipeline(api::command_list* /*cmd_list*/, api::pipeline_stage stages, api::pipeline /*pipeline*/) {
  if (!ngx_hooks::InsideVulkanEvaluate() || (static_cast<uint32_t>(stages) & state::PIPELINE_STAGE_COMPUTE) == 0u) return;
  ++t_dlss_compute_binds;
}

}  // namespace

// ReShade calls this after loading the add-on, outside the loader lock (v6.0.0's add-on loader), so
// MinHook and the module enumeration can run here. A second call comes when ReShade reloads a pinned
// Uplift after the last device went away: everything is still registered, so it only reports success.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
  // With the hooks running the module is pinned, so a second call here only
  // means "still registered, report success" (below). Without them, AddonUninit fully unregisters
  // once the last device goes away, so a later call on the same loaded module must register again --
  // g_state != nullptr alone must not short-circuit that.
  // The Vulkan device hook pins the module too, whatever the NGX hooks did.
  // That holds only while ReShade is pinned as well; it then still lists Uplift with every event and overlay, and registering
  // the module a second time would even be refused (ReShadeRegisterAddon takes a module once). Without the pin (best effort: it does not fail in
  // practice) this call registers again below, as a first one: a ReShade that stayed loaded refuses that with two ERROR lines and keeps the first
  // registration, and one that really unloaded starts empty and takes it.
  if (g_state != nullptr && addon::host::ReshadePinned() && (g_state->hooks.Started() || vk::DeviceHook::Installed() || addon::streamline::Installed())) return true;
  // Fails when ReShade lacks this API version or ImGui table.
  if (!reshade::register_addon(addon_module, reshade_module)) return false;
  g_state = new AddonState();
  g_state->self_module = addon_module;
  std::vector<std::string> warnings;
  g_state->settings = ui::LoadSettings(g_state->config, &warnings);
  g_state->e2e.Initialize();
  ApplyProcessSettings(g_state->settings);
  addon::host::ApplyStartEnabled(*g_state);

  g_state->addon_directory = addon::host::ModulePath(addon_module).parent_path();
  g_state->game_exe_path = addon::host::ModulePath(nullptr);
  g_state->game_directory = g_state->game_exe_path.parent_path();
  // One resolution (UPLIFT_STATE_DIR, else LOCALAPPDATA, else temp) shared
  // by NGX's own app data and the latch marker below, instead of repeating LOCALAPPDATA-then-temp for
  // the former -- so a test's UPLIFT_STATE_DIR override moves NGX's app data out of the user's profile
  // too, not just the latch marker.
  const std::filesystem::path state_directory = addon::UpliftStateDirectory();
  g_state->ngx_data_directory = state_directory / L"ngx";

  // Read before DlssUnavailableReason decides whether a DLSS placement
  // can run this session, so a marker from a crash last session blocks them from this session's very
  // first present -- DlssPlacementBlocked stays a mirror of it, for the overlay.
  g_state->latch_marker_path = addon::LatchMarkerPath(state_directory, g_state->game_exe_path);
  if (addon::LatchMarkerExists(g_state->latch_marker_path)) {
    g_state->settings.dlss_placement_blocked = true;
  }

  // Direct3D 9 devices run NR in gitc-uplift-helper64.exe, which lies next to this add-on; the front is the one
  // gitc-uplift.addon32 uses. The 9Ex pending marker is checked here, before the settings text the helper reads is made.
  g_state->front.emplace("gitc-uplift.addon64", g_state->addon_directory, g_state->game_directory, state_directory, g_state->game_exe_path);
  if (std::optional<std::string> warning = g_state->front->CheckExMarker(&g_state->settings, &g_state->config)) {
    warnings.push_back(std::move(*warning));
  }
  g_state->front->SettingsChanged(g_state->settings);

  const auto diagnostic = g_state->config.Get("DiagnosticBeforeFG");
  g_state->diagnostic_before_fg = diagnostic && ui::ParseBoolSetting(*diagnostic).value_or(false);
  // The NGX hooks, unless NgxHooks = Off.
  std::string hook_error;
  const bool hooks_started =
      (g_state->settings.ngx_hooks == ui::NgxHooksMode::AUTO && g_state->hooks.Start(&g_state->bridge, &hook_error));
  const std::optional<addon::ModuleVersion> reshade_version = addon::ReadModuleVersion(reshade_module);
  g_state->dlss_unavailable_reason =
      addon::DlssUnavailableReason(g_state->settings, reshade_version, hooks_started, hook_error);

  // ReShade's Vulkan layer runs this inside the game's vkCreateInstance, before any device exists, so a detour on
  // vkCreateDevice installed now is in time for every one. The pending marker (a start whose adjusted device never presented) turns the adjustment
  // off, as the 9Ex marker does; CPU-ordered NR is the fallback then.
  const std::filesystem::path vk_marker = addon::host::VkDevicePendingMarkerPath(state_directory, g_state->game_exe_path);
  // A start with NGX's additions that missed its first frame drops only those; one without them that missed it drops the adjustment.
  switch (vk::DeviceHook::CheckMarker(vk_marker)) {
    case vk::DeviceHook::MarkerVerdict::NGX_OFF:
      g_state->settings.startup_latches |= ui::Settings::LATCH_VK_NGX;
      g_state->settings.adjust_vulkan_devices_for_ngx = false;
      ui::SaveSettings(g_state->settings, &g_state->config);
      warnings.push_back("Vulkan: NGX's device additions turned off (AdjustVulkanDevicesForNgx = 0): the last start with them did not reach its first frame; "
                         "NR at Present on Vulkan uses the Direct3D 12 route");
      break;
    case vk::DeviceHook::MarkerVerdict::ADJUST_OFF:
      warnings.emplace_back(addon::host::TurnDeviceAdjustmentOff(*g_state));
      break;
    case vk::DeviceHook::MarkerVerdict::NONE: break;
  }
  // A start whose native NR at Present never reached NATIVE_MARKER_FRAMES (a crash, a hang) turns native NR off, as the device marker does the
  // device additions.
  g_state->vk_native_marker = addon::LatchMarkerPath(state_directory, g_state->game_exe_path).replace_extension(addon::VK_NATIVE_PENDING_EXTENSION);
  if (addon::LatchMarkerExists(g_state->vk_native_marker)) {
    addon::DeleteLatchMarker(g_state->vk_native_marker);
    if (g_state->settings.vulkan_native_nr) {
      g_state->settings.vulkan_native_nr = false;
      ui::SaveSettings(g_state->settings, &g_state->config);
      warnings.push_back("Vulkan: native NR at Present turned off (VulkanNativeNr = 0): the last start with it ended before it had run for a few seconds; "
                         "NR at Present on Vulkan starts at the next route");
    }
  }
  if (!g_state->settings.vulkan_native_nr) {
    g_state->vk_native_off = std::string(VULKAN_NATIVE_OFF_REASON);
  }
  const bool reshade_vulkan_events = (reshade_version && *reshade_version >= addon::MIN_RESHADE_VULKAN_EVENTS);
  g_state->reshade_vulkan_events = reshade_vulkan_events;
  // NGX's device additions only when VulkanNr is Native now, before any device exists (the Direct3D 12 and helper routes never need them).
  // Not while native NR is latched off either.
  vk::DeviceHook::Configure(g_state->settings.adjust_vulkan_devices, g_state->settings.adjust_vulkan_devices_for_ngx,
                            g_state->settings.vulkan_nr == ui::VulkanNrMode::NATIVE, !g_state->settings.vulkan_native_nr, reshade_vulkan_events, vk_marker);
  // Native Vulkan NR is torn down in the vkDestroyDevice detour, before ReShade drops the device.
  vk::DeviceHook::SetDestroyCallback(&OnVulkanDeviceDestroyed);
  addon::host::InstallDeviceHookWithoutVulkanEvents(*g_state, reshade_vulkan_events, &warnings);
  // Uplift is pinned now (the NGX hooks, the detour), so ReShade's module is too.
  if (hooks_started || vk::DeviceHook::Installed()) {
    addon::host::PinReshade(reshade_module);
  }

  reshade::register_event<reshade::addon_event::init_device>(BeforeFgInitDevice);
  reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
  reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyCommandQueue);
  reshade::register_event<reshade::addon_event::init_swapchain>(BeforeFgInitChain);
  reshade::register_event<reshade::addon_event::destroy_swapchain>(BeforeFgDestroyChain);
  reshade::register_event<reshade::addon_event::init_effect_runtime>(addon::host::OnInitEffectRuntime<g_state>);
  reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(addon::host::OnEffectsReloaded<g_state>);
  reshade::register_event<reshade::addon_event::destroy_effect_runtime>(addon::host::OnDestroyEffectRuntime<g_state>);
  reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(addon::host::OnSetCurrentPresetPath<g_state>);
  reshade::register_event<reshade::addon_event::present>(OnPresent);
  reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);  // 1.1.6
  reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
  reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
  reshade::register_event<reshade::addon_event::reshade_open_overlay>(addon::host::OnOpenOverlay<g_state>);
  reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
  addon::RegisterCreateDeviceEvent(&addon::host::OnCreateDevice<g_state, NOTE_INSTANCE_API_VERSION>);
  addon::RegisterCreateSwapchainEvent(reshade_vulkan_events);  // copy access on a Vulkan swap chain's images (event 97, raw)
  if (hooks_started) {
    // Identity and completion-token bookkeeping, a few calls per list per frame.
    reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
    reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
    reshade::register_event<reshade::addon_event::execute_command_list>(OnExecuteCommandList);
    if (reshade_vulkan_events && g_state->settings.source == ui::PlacementSource::DLSS && g_state->dlss_unavailable_reason.empty()
        && addon::host::VulkanLoaderLoaded()) {
      // The "violator" observation, lock-free. Only where it can matter, a start with Source already on a DLSS
      // placement (native NR runs only then) under a ReShade that raises the Vulkan events native NR needs (ReShade's layer runs AddonInit
      // inside its vkCreateInstance). Never registered later: ReShade's event lists take no lock against a dispatch on another thread.
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnVulkanBindPipeline);
      g_dlss_binds_watched.store(true, std::memory_order_relaxed);
    }
  }
  if (g_state->dlss_unavailable_reason.empty()) {
    addon::RegisterTrackingEvents();  // once, and never unregistered while the game runs
    g_state->tracking_registered = true;
  }
  reshade::register_overlay(addon::host::OVERLAY_TITLE, OnDrawOverlay);

  nr::Logf(nr::LogLevel::INFO, "Uplift {} registered (build {}, ReShade API {}, Dear ImGui {}, ReShade {})", UPLIFT_VERSION, UPLIFT_BUILD_ID,
           RESHADE_API_VERSION, IMGUI_VERSION_NUM,
           (reshade_version ? addon::FormatModuleVersion(*reshade_version) : std::string("of unknown version")));
  if (hooks_started) {
    for (const ngx_hooks::HookedModule& module : g_state->hooks.Modules()) {
      // Every API the module's hooks cover, Direct3D 11 included (ngx_hooks::HookedApis).
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
  // Pinned by the Vulkan device hook too; its detour and the events stay for the process.
  if (g_state == nullptr || g_state->hooks.Started() || vk::DeviceHook::Installed() || addon::streamline::Installed()) return;
  // With no module pin the Direct3D 9 helper exits here. A pinned add-on relies on QUIT, the helper's idle exit or its kill-on-close job.
  g_state->front->Quit();
  addon::RemoveLogBridge();
  reshade::unregister_addon(addon_module, reshade_module);
}

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID reserved) {
  // Outside ReShade the add-on refuses to load. Inside it, ReShade calls AddonInit next.
  if (addon::host::RefusesLoad(reason)) return FALSE;
  // The process is ending (not a FreeLibrary): a clean exit clears the Vulkan pending marker. No lock, no allocation.
  if (addon::host::ProcessEnding(reason, reserved)) {
    g_process_exiting.store(true, std::memory_order_release);  // no core Shutdown1 waits for the add-on's lock from here on
    vk::DeviceHook::NoteProcessExit();
    // A clean exit is no crash: the native marker goes (no lock, no allocation; the path was made at AddonInit and is never freed).
    if (g_state != nullptr && g_state->vk_native_marker_pending.load(std::memory_order_acquire)) {
      DeleteFileW(g_state->vk_native_marker.c_str());
    }
  }
  return TRUE;
}
