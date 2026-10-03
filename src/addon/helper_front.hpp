#pragma once

// Plan 10 (design §2.2). The add-on side of NR in Uplift's 64-bit helper: the D3D9, D3D10 and D3D11 clients, the helper's lifetime (RemoteNr),
// the 9Ex toggle, the overlay's lines and the status card. Moved out of addon32.cpp unchanged: gitc-uplift.addon32 hosts it for every device,
// gitc-uplift.addon64 for its Direct3D 9 devices. Each host keeps the ReShade registration, the settings and the panel state, the NrClaim, the
// swap-chain selector, the runtime, back-buffer and marker lookup, the LaunchPad link and the API check, and calls the front from its
// handlers. Not thread-safe: the host's lock.

#include <Windows.h>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "addon/bridge_strikes.hpp"
#include "addon/frame_trigger.hpp"
#include "addon/reshade_api.hpp"
#include "client/d3d10_client.hpp"
#include "client/d3d11_client.hpp"
#include "client/d3d12_client.hpp"
#include "client/d3d9_client.hpp"
#include "client/gl_client.hpp"
#include "client/remote_nr.hpp"
#include "client/vk_client.hpp"
#include "gl/game_host.hpp"
#include "ui/overlay.hpp"
#include "ui/settings.hpp"
#include "vk/game_host.hpp"

namespace uplift::addon {

// Final review I-1: ReShade keeps an effect's resources once one of its techniques has created them, even after every technique using them
// is disabled, so a found UPLIFT_MASK variable does not mean an effect still writes it. True only while at least one technique of the
// effect that declares `variable` is enabled.
inline bool MaskEffectRunning(reshade::api::effect_runtime* runtime, reshade::api::effect_texture_variable variable) {
  char effect_name[MAX_PATH] = {};
  runtime->get_texture_variable_effect_name(variable, effect_name);
  bool running = false;
  runtime->enumerate_techniques(effect_name, [&](reshade::api::effect_runtime* technique_runtime, reshade::api::effect_technique technique) {
    running |= technique_runtime->get_technique_state(technique);
  });
  return running;
}

// Plans 11 and 12: the game-side hosts the clients that record or free through ReShade's API use (a Vulkan device's, an OpenGL context's, a Direct3D 12 device's).
// At most one is set, the one of the device being handled; null for every other API, whose clients free their objects by reference counting.
struct ClientHosts {
  vk::GameHost* vulkan = nullptr;
  gl::GameHost* opengl = nullptr;
  client::D3D12Host* d3d12 = nullptr;
};

// One device's helper-side state (was DeviceEntry32 minus what the host keeps: the selector, the queue, the runtime, the marker and the
// LaunchPad link).
struct HelperDevice {
  std::unique_ptr<client::D3D9Client> d3d9;    // a Direct3D 9 device's client; made at the first present with NR on
  std::unique_ptr<client::D3D10Client> d3d10;  // a Direct3D 10 device's client (32-bit games): the relay and a D3D11 client on it
  std::unique_ptr<client::D3D11Client> d3d11;  // a Direct3D 11 device's client
  std::unique_ptr<client::VkClient> vulkan;    // Plan 11: a Vulkan device's client (DXVK in a 32-bit game)
  std::unique_ptr<client::GlClient> gl;        // Plan 12: an OpenGL context's client (a 32-bit game)
  std::unique_ptr<client::D3D12Client> d3d12;  // Plan 12: a 32-bit Direct3D 12 device's client (dgVoodoo2's D3D12 output)
  reshade::api::command_queue* present_queue = nullptr;  // Vulkan and OpenGL: the present event's queue; the host's `queue` is the effect runtime's (design §3.1)
  bool vk_second_queue_logged = false;
  std::string message;                         // why this device has no client
  bool rejected = false;                       // not NVIDIA, or the client could not start: decided once
  bool snippet_located = false;
  std::filesystem::path snippet;                       // resolved once at the client's creation; empty when not found
  std::string snippet_problem;                         // no runtime found: no helper starts, and the card says so
  uint64_t back_buffer = 0u;                           // the last present's back buffer (a ReShade handle), for the Direct3D 10 and 11 clients' RUN
  FrameTrigger trigger;                                // authoritative: the helper replays the point this reached (design §2.1)
  TriggerPoint unreported_point = TriggerPoint::NONE;  // reached without a RUN; the next FRAME tells the helper
  bool frame_ready = false;                            // this present's FRAME reply said the helper is ready for a RUN
  bool running = false;                                // the last FRAME reply said NR's state is not OFF
  bool retry_now = false;                              // Retry now, for the next FRAME
  // Plan 17: the 2-strike rule for the helper's stops (its own device removed, a hang, an exit), as for a bridge's private device: Retry now restarts it once.
  BridgeStrikes strikes;
  bool claimed_elsewhere = false;
  bool nr_returned_logged = false;  // "NR ran on the game's frame" was logged since the client last attached (Reset, helper exit)
  bool ex_checked = false;
  uint32_t ex_presents = 0u;  // present events of a 9Ex device, counted up to 2: the second proves the first real Present returned
  bool d3d9_ex = false;       // a Direct3D 9Ex device (KMT)
  std::string mask_note;      // Plan 5: the overlay's "NR mask" line
  // Plan 14: Launchpad's technique and the Uplift technique are both enabled (Setup's Launchpad option); the host sets it at each present.
  bool launchpad_ready = false;
  // The game's DXGI local budget and usage (design §2.10), measured at most once a second and sent with the next FRAME.
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
  std::chrono::steady_clock::time_point last_memory_query;
  uint64_t memory_budget = 0u;
  uint64_t memory_usage = 0u;

  [[nodiscard]] bool HasClient() const {
    return d3d9 != nullptr || d3d10 != nullptr || d3d11 != nullptr || vulkan != nullptr || gl != nullptr || d3d12 != nullptr;
  }
  [[nodiscard]] LUID Luid() const {
    if (d3d9) return d3d9->Luid();
    if (d3d10) return d3d10->Luid();
    if (vulkan) return vulkan->Luid();
    if (gl) return gl->Luid();
    if (d3d12) return d3d12->Luid();
    return d3d11->Luid();
  }
  [[nodiscard]] ipc::Transport Kind() const {
    if (d3d9) return d3d9->Kind();
    if (d3d10) return d3d10->Kind();
    if (vulkan) return vulkan->Kind();
    if (gl) return gl->Kind();
    if (d3d12) return d3d12->Kind();
    return d3d11->Kind();
  }
  [[nodiscard]] bool Fenced() const { return HasClient() && Kind() == ipc::Transport::FENCED; }
  // A client that shares UPLIFT_MASK and LaunchPad's motion with the helper: Direct3D 9 on both its transports (Plan 10), Vulkan and OpenGL on both
  // (Plans 11 and 12), Direct3D 10, 11 and 12 with fences (a Direct3D 11 device on the CPU-ordered fallback has neither; Direct3D 12 has no fallback).
  [[nodiscard]] bool SharesImages() const { return d3d9 != nullptr || vulkan != nullptr || gl != nullptr || Fenced(); }
  // `hosts`: the Vulkan, OpenGL or Direct3D 12 client's host (empty for the other APIs, which free their objects by reference counting).
  void ReleaseMask(const ClientHosts& hosts = {}) {  // the client's shared mask
    if (d3d9) {
      d3d9->ReleaseMask();
    } else if (d3d10) {
      d3d10->ReleaseMask();
    } else if (vulkan) {
      vulkan->ReleaseMask(*hosts.vulkan);
    } else if (gl) {
      gl->ReleaseMask(*hosts.opengl);
    } else if (d3d12) {
      d3d12->ReleaseMask(*hosts.d3d12);
    } else if (d3d11) {
      d3d11->ReleaseMask();
    }
  }
  // Plan 17: Retry now restarts the helper after it stopped: the client's latch from that stop goes too.
  void ClearClientLatch() {
    if (d3d10) {
      d3d10->ClearLatch();
    } else if (vulkan) {
      vulkan->ClearLatch();
    } else if (gl) {
      gl->ClearLatch();
    } else if (d3d12) {
      d3d12->ClearLatch();
    } else if (d3d11) {
      d3d11->ClearLatch();
    }
  }
  // The helper is gone (or the client's transport is): everything shared with it goes.
  void ReleaseAll(const ClientHosts& hosts = {}) {
    if (d3d9) {
      d3d9->ReleaseAll();
    } else if (d3d10) {
      d3d10->ReleaseAll();
    } else if (vulkan) {
      vulkan->ReleaseAll(*hosts.vulkan);
    } else if (gl) {
      gl->ReleaseAll(*hosts.opengl);
    } else if (d3d12) {
      d3d12->ReleaseAll(*hosts.d3d12);
    } else if (d3d11) {
      d3d11->ReleaseAll();
    }
  }
};

// What the host looked up at this present.
struct HelperFrame {
  reshade::api::device* device = nullptr;
  reshade::api::swapchain* swapchain = nullptr;
  reshade::api::effect_runtime* runtime = nullptr;  // may be null
  // Vulkan and OpenGL (Plans 11 and 12, design §3.1): the effect runtime's queue, whose immediate list Uplift records into, and the queue the present event
  // came from (on OpenGL the runtime's context must be the one that presents). Direct3D 12 (Plan 12): `queue` is the present event's, which is the runtime's.
  // Null for every other API, and for a Vulkan or OpenGL device whose runtime is not up yet (nothing runs then).
  reshade::api::command_queue* queue = nullptr;
  reshade::api::command_queue* present_queue = nullptr;
  std::string_view vk_hook_error;  // why the vkCreateDevice hook is not installed (an unseen device's Details reason); empty when it is
  reshade::api::resource back_buffer = {0u};
  reshade::api::effect_technique marker = {0u};  // 0 when the Uplift technique is off
  const ui::Settings* settings = nullptr;
  const ui::OverlayState* overlay = nullptr;
  bool nr_allowed = true;          // the host's NrClaim
  bool claimed_elsewhere = false;  // NR is on and the claim went to another device: Present returns after noting it
  std::chrono::steady_clock::time_point now;
};

class HelperFront {
 public:
  // `addon_file`: "gitc-uplift.addon32" or "gitc-uplift.addon64", named in the texts and sent to the helper. `addon_directory`: where gitc-uplift-helper64.exe
  // lies. `state_directory`: UpliftStateDirectory(), for the helper's NGX data and the 9Ex marker; `game_exe` names the marker.
  HelperFront(std::string_view addon_file, std::filesystem::path addon_directory, std::filesystem::path game_directory,
              std::filesystem::path state_directory, const std::filesystem::path& game_exe);
  HelperFront(const HelperFront&) = delete;
  HelperFront& operator=(const HelperFront&) = delete;

  // AddonInit (design §2.9): the pending 9Ex marker. When the last start asked for 9Ex and never got a 9Ex device to present or to destroy,
  // the toggle is turned off (in `settings`, saved through `config`) and the marker deleted, once; the warning to log is returned.
  std::optional<std::string> CheckExMarker(ui::Settings* settings, ui::ConfigStore* config);
  // The settings changed (SaveAndApply, the Enabled poll) and once at AddonInit: re-serialises the text the helper reads and bumps its
  // generation, which also restarts the helper's retry backoff (Plan 6, D11).
  void SettingsChanged(const ui::Settings& settings);
  // Every present of a D3D9 device, BEFORE the host's swap-chain selector (any swap chain counts): the 9Ex check, and the pending marker's
  // deletion at a 9Ex device's second present event.
  void NotePresent(HelperDevice& device, reshade::api::device* owner, const ui::Settings& settings);
  // The rest of the present event, once the host has passed its selector, looked up the runtime, back buffer and marker, run the LaunchPad
  // link, skipped a rejected device and updated its NrClaim: the client, the helper's FRAME, and NR at the present point.
  // `relay_after_unlock` (a Direct3D 10 device): receives what D3D11CreateDevice returned for the client's relay, whether or not it succeeds
  // (ReShade's proxy in a game), which the host releases only after it has released its lock: its last release raises destroy_device.
  void Present(HelperDevice& device, const HelperFrame& frame, Microsoft::WRL::ComPtr<ID3D11Device>* relay_after_unlock = nullptr);
  // reshade_render_technique on the frame's own immediate list: NR at the Uplift technique, with LaunchPad's motion (Direct3D 9, and 10 and 11
  // with fences). `rtv` is the event's render target (bit 0 marks sRGB).
  void Technique(HelperDevice& device, reshade::api::effect_runtime* runtime, bool is_marker, uint64_t rtv, const ui::Settings& settings);
  // reshade_finish_effects on the frame's own immediate list: NR when the technique never came, then UPLIFT_MASK (Direct3D 9, and 10 and 11 with
  // fences).
  void FinishEffects(HelperDevice& device, reshade::api::effect_runtime* runtime, uint64_t rtv, const ui::Settings& settings);
  // A D3D9 device's destroy_command_queue, raised before every Reset and ResetEx (helper design §2.7): everything the client made on
  // the device goes (a live D3DPOOL_DEFAULT object fails the game's Reset) and the helper is detached; its Session stays.
  void DestroyQueue(HelperDevice& device, reshade::api::device* owner);
  // Every destroy_device of a device this front hosts: the 9Ex marker's deletion for any 9Ex device (one created and destroyed without a
  // Present is neither of the two cases the marker guards), then `device`'s release and DETACH when the host has an entry (null otherwise).
  // Returns the Direct3D 10 client, moved out of `device`: the relay is ReShade's proxy, so the host destroys it only after it has released
  // its lock (null for any other device).
  [[nodiscard]] std::unique_ptr<client::D3D10Client> DestroyDevice(HelperDevice* device, reshade::api::device* owner);
  // For the host's NrClaim: `owner`'s NR is loaded (its helper runs and its Session is not OFF).
  [[nodiscard]] bool NrLoaded(reshade::api::device* owner) const;
  // The panel's view for `game_device` (of `device_api`), after the host's API check: the 9Ex readout, the motion line, the lines and the
  // status card, then Setup and the working card's rows (ui::FinishSetup, with `state` and `now`: Plan 14, from the helper's status in protocol 6).
  // `device` is the host's entry, or null before the game's first frame. The host has set `view->dlss_unavailable`.
  void Overlay(reshade::api::device* game_device, const HelperDevice* device, reshade::api::device_api device_api,
               const ui::Settings& settings, ui::OverlayView* view, ui::OverlayState* state, std::chrono::steady_clock::time_point now) const;
  // The card's Retry now: a stopped helper starts afresh; otherwise the next FRAME carries it.
  void RetryNow(HelperDevice* device);
  // ReShade's create_device event (id 96): asks for a Direct3D 9Ex device when the toggle is on, and writes the pending marker.
  bool CreateDevice(reshade::api::device_api device_api, uint32_t& api_version, bool use_d3d9ex);
  // AddonUninit: QUIT, a capped wait for the exit, then the job closes: the helper never outlives the game.
  void Quit();

 private:
  // NR at `point` for this frame: out through the device's client, RUN, back. `source`: a Direct3D 9 surface (the back buffer at PRESENT; the
  // event's render target at the marker and after the effects) or an OpenGL image (FB0 at PRESENT; ReShade's intermediate, the event's rtv resource, at the
  // marker and after the effects); `launchpad_motion`: this frame's UPLIFT_MV (a ReShade resource handle: an IDirect3DTexture9 on Direct3D 9). When no RUN
  // goes out the helper never sees a marker point, so the next FRAME reports it. Vulkan, OpenGL and Direct3D 12: `game_device` and `queue` (the effect
  // runtime's) make the client's host; `at_present`: the calling event is the present event (the back buffer is in the present state, and on OpenGL the
  // present queue's context is checked).
  void RunClient(HelperDevice& device, reshade::api::device* game_device, reshade::api::command_queue* queue, TriggerPoint point, uint64_t source,
                 uint64_t launchpad_motion, bool at_present);

  std::string addon_file_;
  std::filesystem::path addon_directory_;
  std::filesystem::path game_directory_;
  std::filesystem::path ngx_data_directory_;  // the helper's NGX app data, as in 64-bit games
  std::filesystem::path ex_marker_path_;      // Design §2.9: written when Uplift asks for 9Ex, deleted once a 9Ex device's first Present returned
  // One helper per process, made at the first present that needs it, for the device that owns the claim. It outlives a D3D9 Reset
  // (destroy_command_queue only detaches), and Quit ends it.
  std::unique_ptr<client::RemoteNr> remote_;
  reshade::api::device* remote_owner_ = nullptr;
  bool ex_event_seen_ = false;  // ReShade raised create_device at least once
  bool ex_turned_off_ = false;  // the marker was left by the last start: UseD3D9Ex was turned off
  bool warned_ex_event_ = false;
  bool warned_invalid_snippet_path_ = false;
  bool warned_settings_size_ = false;
  uint64_t settings_generation_ = 0u;  // every SettingsChanged; the AddonInit call makes it 1, and 0 is "nothing sent yet"
  std::string settings_text_;          // ui::SaveSettings of the host's settings, for the helper
};

// Registers `callback` raw for ReShade's create_device event (id 96, add-on API 11+; the pinned v6.0.0 headers stop at 92): a ReShade
// without the event ignores the id (addon_manager.cpp:556-560), and CreateDevice is never called. Both hosts do it in AddonInit.
void RegisterCreateDeviceEvent(bool (*callback)(reshade::api::device_api device_api, uint32_t& api_version));

}  // namespace uplift::addon
