// Plan 10 (design §2.2): addon::HelperFront, moved out of addon32.cpp with no behaviour change in 32-bit games. See the header.
#include "addon/helper_front.hpp"

#include <Windows.h>

#include <d3d11.h>
#include <d3d9.h>

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <utility>

#include "addon/environment.hpp"
#include "addon/launchpad_link.hpp"
#include "addon/live_facts.hpp"
#include "addon/removal_latch.hpp"
#include "addon/swapchain_usage.hpp"
#include "addon/reshade_d3d12_host.hpp"
#include "addon/reshade_gl_host.hpp"
#include "addon/reshade_vk_host.hpp"
#include "build_id.hpp"
#include "color/encoding.hpp"
#include "ipc/control_block.hpp"
#include "ipc/protocol.hpp"
#include "ipc/settings_text.hpp"
#include "nr/log.hpp"
#include "nr/session_status.hpp"
#include "ui/settings_schema.hpp"
#include "ui/status_text.hpp"

namespace uplift::addon {
namespace {

namespace api = reshade::api;

constexpr char MASK_TEXTURE[] = "UPLIFT_MASK";
constexpr char MOTION_TEXTURE[] = "UPLIFT_MV";
constexpr char LAUNCHPAD_TECHNIQUE[] = "MartysMods_Launchpad";
constexpr wchar_t HELPER_FILE[] = L"gitc-uplift-helper64.exe";
constexpr auto MEMORY_PERIOD = std::chrono::seconds(1);  // the game's DXGI budget crosses at most this often (design §2.10)

static_assert(static_cast<uint32_t>(api::color_space::unknown) == static_cast<uint32_t>(color::ColorSpace::UNKNOWN));
static_assert(static_cast<uint32_t>(api::color_space::srgb_nonlinear) == static_cast<uint32_t>(color::ColorSpace::SRGB_NONLINEAR));
static_assert(static_cast<uint32_t>(api::color_space::extended_srgb_linear) == static_cast<uint32_t>(color::ColorSpace::EXTENDED_SRGB_LINEAR));
static_assert(static_cast<uint32_t>(api::color_space::hdr10_st2084) == static_cast<uint32_t>(color::ColorSpace::HDR10_ST2084));
static_assert(static_cast<uint32_t>(api::color_space::hdr10_hlg) == static_cast<uint32_t>(color::ColorSpace::HDR10_HLG));

// A ReShade handle or get_native() value is 64 bits wide even in a 32-bit game; the object it names is a pointer of the game's width.
template <class T>
T* AsNative(uint64_t handle) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(handle));
}

// The helper's own log lines: its level as an ASCII digit, then the message (ipc::HelperLauncher::DrainLog).
void HelperLog(std::string_view line) {
  if (line.empty()) return;
  const int level = std::clamp(line.front() - '0', 0, static_cast<int>(nr::LogLevel::TRACE));
  nr::Log(static_cast<nr::LogLevel>(level), std::format("helper: {}", line.substr(1)));
}
const std::function<void(std::string_view)> HELPER_LOG = &HelperLog;

// The Session's public state, rebuilt from the helper's status (ipc::Status).
nr::SessionStatus SessionOf(const ipc::Status& status) {
  nr::SessionStatus session;
  session.state = static_cast<nr::SessionState>(status.session_state);
  session.passes_requested = status.passes_requested;
  if (status.has_runtime_bytes != 0u) {
    session.runtime_bytes = status.runtime_bytes;
  }
  session.message = std::string(ipc::TextOf(status.session_message));
  session.suspended = (status.suspended != 0u);
  session.grace_remaining = std::chrono::milliseconds(status.grace_remaining_ms);
  if (status.retry_in_ms >= 0) {
    session.retry_in = std::chrono::milliseconds(status.retry_in_ms);
  }
  session.retries_exhausted = (status.retries_exhausted != 0u);
  return session;
}

// Plans 11 and 12: the host of the device's client (ReShade's API on `queue`): a Vulkan device's, an OpenGL context's or a Direct3D 12 device's; none is set for
// every other API. `present_queue`: the present event's queue, null in the effect events (an OpenGL host is valid only when it is the runtime's).
struct Hosts {
  std::optional<ReshadeVkHost> vulkan;
  std::optional<ReshadeGlHost> opengl;
  std::optional<ReshadeD3D12Host> d3d12;
  [[nodiscard]] ClientHosts Pointers() {
    return {.vulkan = (vulkan ? &*vulkan : nullptr), .opengl = (opengl ? &*opengl : nullptr), .d3d12 = (d3d12 ? &*d3d12 : nullptr)};
  }
};
Hosts HostsOf(const HelperDevice& device, api::device* game_device, api::command_queue* queue, api::command_queue* present_queue = nullptr) {
  Hosts hosts;
  if (device.vulkan) {
    hosts.vulkan.emplace(game_device, queue, device.vulkan->VulkanDevice().vkQueueSubmit);
  } else if (device.gl) {
    hosts.opengl.emplace(device.gl->Gl(), queue, present_queue);
  } else if (device.d3d12) {
    hosts.d3d12.emplace(queue);
  }
  return hosts;
}

// The image NR runs on in an effect event: Direct3D 9's render target (bit 0 marks sRGB), OpenGL's intermediate, which is the event's rtv resource and not FB0
// (design §3.2). Other APIs run on the back buffer and ignore it.
uint64_t EventImage(const HelperDevice& device, api::effect_runtime* runtime, uint64_t rtv) {
  return device.gl ? runtime->get_device()->get_resource_from_view(api::resource_view{rtv}).handle : (rtv & ~uint64_t{1u});
}

// The API's name in the texts.
std::string_view ApiName(api::device_api device_api) {
  switch (device_api) {
    case api::device_api::d3d9:   return "Direct3D 9";
    case api::device_api::d3d10:  return "Direct3D 10";
    case api::device_api::d3d11:  return "Direct3D 11";
    case api::device_api::d3d12:  return "Direct3D 12";
    case api::device_api::opengl: return "OpenGL";
    case api::device_api::vulkan: return "Vulkan";
  }
  return "an unknown API";
}

// Design §2.9: ReShade's create_device event.
constexpr uint32_t CREATE_DEVICE_EVENT = 96u;
constexpr uint32_t D3D9EX_API_VERSION = 0x9100u;

}  // namespace

HelperFront::HelperFront(std::string_view addon_file, std::filesystem::path addon_directory, std::filesystem::path game_directory,
                         std::filesystem::path state_directory, const std::filesystem::path& game_exe)
    : addon_file_(addon_file),
      addon_directory_(std::move(addon_directory)),
      game_directory_(std::move(game_directory)),
      ngx_data_directory_(state_directory / L"ngx"),
      // Design §2.9: the pending marker sits next to the latch marker, per game exe, with its own extension.
      ex_marker_path_(LatchMarkerPath(state_directory, game_exe).replace_extension(L".d3d9ex")) {}

std::optional<std::string> HelperFront::CheckExMarker(ui::Settings* settings, ui::ConfigStore* config) {
  // If the marker survived the last start, that start asked for 9Ex and never got a 9Ex device to present or to destroy: turn the toggle
  // off, once, and say so.
  if (!LatchMarkerExists(ex_marker_path_)) return std::nullopt;
  settings->use_d3d9ex = false;
  ui::SaveSettings(*settings, config);
  DeleteLatchMarker(ex_marker_path_);
  ex_turned_off_ = true;
  return std::string("Direct3D 9Ex turned off: the last start with it did not reach its first frame");
}

void HelperFront::SettingsChanged(const ui::Settings& settings) {
  ++settings_generation_;  // the helper reloads its copy, and restarts the retry backoff (Plan 6, D11)
  // What the helper reads instead of ReShade.ini (design §2.1).
  ipc::TextConfigStore store;
  ui::SaveSettings(settings, &store);
  settings_text_ = store.Serialize();
  if (settings_text_.size() >= ipc::SETTINGS_BYTES && !warned_settings_size_) {
    warned_settings_size_ = true;
    nr::Logf(nr::LogLevel::WARN, "the settings are larger than the helper's {} byte channel; the helper reads them cut short",
             ipc::SETTINGS_BYTES);
  }
}

void HelperFront::NotePresent(HelperDevice& device, api::device* owner, const ui::Settings& settings) {
  if (!device.ex_checked) {
    device.ex_checked = true;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> device_ex;
    device.d3d9_ex = SUCCEEDED(AsNative<IDirect3DDevice9>(owner->get_native())->QueryInterface(IID_PPV_ARGS(&device_ex)));
    if (settings.use_d3d9ex && !ex_event_seen_ && !warned_ex_event_) {
      warned_ex_event_ = true;
      nr::Log(nr::LogLevel::WARN,
              "UseD3D9Ex is on, but this ReShade never raised the create_device add-on event: it cannot switch the game to Direct3D 9Ex");
    }
  }
  if (device.d3d9_ex && device.ex_presents < 2u) {
    // Design §2.9: the pending marker asks whether the game got through a first real Present on a 9Ex device. ReShade raises
    // this event before it calls that Present, so the second event is the proof: a 9Ex problem that crashes inside the first
    // Present, or in the first frame, leaves the marker.
    ++device.ex_presents;
    if (device.ex_presents == 2u) {
      DeleteLatchMarker(ex_marker_path_);
    }
  }
}

bool HelperFront::NrLoaded(api::device* owner) const {
  return remote_owner_ == owner && remote_ && remote_->NrMayRun();
}

bool HelperFront::NrApplied(api::device* owner) const {
  return remote_owner_ == owner && remote_ && remote_->Running() && remote_->LastStatus().nr_applied != 0u;
}

void HelperFront::RunClient(HelperDevice& device, api::device* game_device, api::command_queue* queue, TriggerPoint point, uint64_t source,
                            uint64_t launchpad_motion, bool at_present) {
  if (point == TriggerPoint::NONE) return;
  bool sent = false;
  bool returned = false;  // NR's output is back in the game's frame
  if (device.frame_ready && remote_) {
    if (device.vulkan) {
      Hosts hosts = HostsOf(device, game_device, queue);
      // Design §3.4 case 2: the game presents from a queue other than the effect runtime's. Uplift's submissions are ordered after the game's frame only
      // by submission order on the runtime's queue, so the client flushes the present queue and waits for it on the CPU (2 s; a timeout skips this
      // frame's NR) inside Run, once per frame, after its own skip checks and before the copy in (final review, minor 4).
      std::optional<ReshadeVkHost> present_host;
      if (device.present_queue != nullptr && queue != nullptr && device.present_queue != queue) {
        if (!device.vk_second_queue_logged) {
          device.vk_second_queue_logged = true;
          nr::Log(nr::LogLevel::INFO, "Vulkan: the game presents from another queue than ReShade's effects; Uplift waits for it on the CPU before NR");
        }
        device.vulkan->NoteSecondQueue();
        present_host.emplace(game_device, device.present_queue, device.vulkan->VulkanDevice().vkQueueSubmit);
      }
      returned = device.vulkan->Run(*hosts.vulkan, VulkanImageInfo(game_device, {device.back_buffer}),
                                    (at_present ? vk::Usage::PRESENT : vk::Usage::RENDER_TARGET), VulkanImageInfo(game_device, {launchpad_motion}), point,
                                    *remote_, (present_host ? &*present_host : nullptr));
      sent = device.vulkan->LastRunSent();
    } else if (device.gl) {
      // Design §3.2: FB0 at the present event, ReShade's intermediate (the event's rtv resource) at the technique and after the effects. GL work runs only
      // when the present is on the runtime's own context (the host is invalid otherwise, and Run does nothing).
      Hosts hosts = HostsOf(device, game_device, queue, (at_present ? device.present_queue : nullptr));
      returned = device.gl->Run(*hosts.opengl, GlImageInfo(game_device, {source}), GlImageInfo(game_device, {launchpad_motion}), point, *remote_);
      sent = device.gl->LastRunSent();
    } else if (device.d3d12) {
      Hosts hosts = HostsOf(device, game_device, queue);
      returned = device.d3d12->Run(*hosts.d3d12, AsNative<ID3D12Resource>(device.back_buffer),
                                   (at_present ? client::D3D12Usage::PRESENT : client::D3D12Usage::RENDER_TARGET), AsNative<ID3D12Resource>(launchpad_motion),
                                   point, *remote_);
      sent = device.d3d12->LastRunSent();
    } else if (device.d3d9) {
      returned = device.d3d9->Run(AsNative<IDirect3DSurface9>(source), AsNative<IDirect3DTexture9>(launchpad_motion), point, *remote_);
      sent = device.d3d9->LastRunSent();
    } else if (device.d3d10) {
      returned = device.d3d10->Run(AsNative<ID3D10Resource>(device.back_buffer), AsNative<ID3D10Resource>(launchpad_motion), point,
                                   *remote_);
      sent = device.d3d10->LastRunSent();
    } else if (device.d3d11) {
      returned = device.d3d11->Run(AsNative<ID3D11Resource>(device.back_buffer), AsNative<ID3D11Resource>(launchpad_motion), point,
                                   *remote_);
      sent = device.d3d11->LastRunSent();
    }
  }
  if (returned && !device.nr_returned_logged) {
    // Once per attach: the e2e cases (and the checklist) read it as "NR ran here", e.g. again after a Direct3D 9 Reset.
    device.nr_returned_logged = true;
    nr::Logf(nr::LogLevel::INFO, "NR ran on the game's frame ({}, {})",
             (device.vulkan  ? "Vulkan"
              : device.gl    ? "OpenGL"
              : device.d3d12 ? "Direct3D 12"
              : device.d3d9  ? "Direct3D 9"
              : device.d3d10 ? "Direct3D 10"
                             : "Direct3D 11"),
             client::BITNESS);
  }
  if (!sent && point != TriggerPoint::PRESENT) {
    device.unreported_point = point;  // PRESENT changes nothing on the helper's copy of the trigger
  }
}

void HelperFront::Present(HelperDevice& entry, const HelperFrame& frame, Microsoft::WRL::ComPtr<ID3D11Device>* relay_after_unlock) {
  api::device* const device = frame.device;
  const ui::Settings& settings = *frame.settings;
  const bool d3d9 = (device->get_api() == api::device_api::d3d9);
  const bool d3d10 = (device->get_api() == api::device_api::d3d10);
  const bool vulkan = (device->get_api() == api::device_api::vulkan);
  const bool opengl = (device->get_api() == api::device_api::opengl);
  const bool d3d12 = (device->get_api() == api::device_api::d3d12);
  entry.back_buffer = frame.back_buffer.handle;
  entry.frame_ready = false;
  entry.claimed_elsewhere = frame.claimed_elsewhere;
  if (entry.claimed_elsewhere) return;
  if (vulkan || opengl) {
    entry.present_queue = frame.present_queue;
    if (frame.queue == nullptr) {
      // Vulkan design §3.1: without an effect runtime (not up yet) there is no queue to record on, and nothing runs. Nothing is said either: an OpenGL card's R63
      // text from an earlier present (ReShade makes its runtime again) must not outlive it, as addon.cpp's NONE case clears it (a rejected device never gets here).
      if (opengl) {
        entry.message.clear();
      }
      return;
    }
  }

  // The client is made at the first present with NR on: a game that never enables NR pays nothing (design §2.7).
  if (!entry.HasClient()) {
    if (!settings.enabled) {
      entry.message.clear();  // only R63's text can be here (a rejected device never gets this far): NR is off, so there is nothing to say
      return;
    }
    std::string error;
    if (opengl) {
      // OpenGL design §3.1, as addon.cpp's GlBridge (batch 2 review I-1): the context is checked FIRST, from the queues and the system wglGetCurrentContext
      // alone, with no GL call. A context of the game's own, or a present with none current, gets nothing this frame, is asked again at the next one, and is
      // never rejected for good; only the game's own context gets the card's R63 text. gl::Functions::Load, which does make GL calls, then runs once, on the runtime's own context.
      if (const RuntimeContext context = CheckRuntimeContext(frame.queue, frame.present_queue); context != RuntimeContext::CURRENT) {
        entry.message = (context == RuntimeContext::OTHER ? std::string(gl::WRONG_CONTEXT_PROBLEM) : std::string());
        return;
      }
      entry.message.clear();
      std::optional<gl::Functions> functions = gl::Functions::Load(&error);
      LUID luid = {};
      if (functions && GlAdapterLuid(*functions, &luid, &error)) {  // else `error` is the card's text: not NVIDIA's, or its adapter is unknown
        entry.gl = std::make_unique<client::GlClient>(std::move(*functions), luid, true);
      }
    } else if (d3d12) {
      entry.d3d12 = client::D3D12Client::Create(AsNative<ID3D12Device>(device->get_native()), &error);
    } else if (vulkan) {
      // Vulkan design §4 (as addon.cpp's VkBridge): the vendor, the LUID and the device's functions (with what the vkCreateDevice hook recorded) decide it.
      LUID luid = {};
      if (VulkanAdapterLuid(device, &luid, &error)) {
        std::optional<vk::Device> functions = vk::Device::Open(VulkanHandleOf<VkDevice>(device->get_native()));
        if (!functions) {
          error = "the Vulkan loader, or a Vulkan function every device has, is missing";
        } else {
          if (!functions->seen && !frame.vk_hook_error.empty()) {
            functions->record.not_adjusted = std::format("Uplift's vkCreateDevice hook could not be installed ({})", frame.vk_hook_error);
          }
          entry.vulkan = std::make_unique<client::VkClient>(std::move(*functions), luid, true);
        }
      }
    } else if (d3d9) {
      entry.d3d9 = client::D3D9Client::Create(AsNative<IDirect3DDevice9>(device->get_native()), &error);
    } else if (d3d10) {
      // The relay's proxy comes out through `relay_after_unlock` whether or not this succeeds: the host releases it after its lock.
      entry.d3d10 = client::D3D10Client::Create(AsNative<ID3D10Device>(device->get_native()), relay_after_unlock, &error);
    } else {
      entry.d3d11 = client::D3D11Client::Create(AsNative<ID3D11Device>(device->get_native()), true, &error);
    }
    if (!entry.HasClient()) {
      entry.rejected = true;
      entry.message = (opengl ? std::format("Uplift could not start on this OpenGL context: {}", error)
                              : std::format("Uplift could not start on this {} device: {}", ApiName(device->get_api()), error));
      nr::Log(nr::LogLevel::ERR, entry.message);
      return;
    }
  }
  if (!entry.snippet_located) {
    entry.snippet_located = true;
    const std::string& configured = settings.snippet_path;
    // I1: a hand-edited SnippetPath can be invalid UTF-8; it is treated like an unset one.
    const std::optional<std::filesystem::path> configured_path = PathFromUtf8(configured);
    if (!configured_path && !configured.empty() && !warned_invalid_snippet_path_) {
      warned_invalid_snippet_path_ = true;
      nr::Log(nr::LogLevel::WARN, std::format("SnippetPath is not valid UTF-8; searching next to {} and the game instead", addon_file_));
    }
    const std::optional<std::filesystem::path> snippet = LocateSnippet({
        .configured = configured_path.value_or(std::filesystem::path()),
        .addon_directory = addon_directory_,
        .game_directory = game_directory_,
    });
    entry.snippet = snippet.value_or(std::filesystem::path());
    if (!snippet) {
      entry.snippet_problem = (configured.empty() ? std::format("nvngx_dlssnr.dll not found next to {} or the game; copy it there or set Runtime path", addon_file_)
                                                  : std::format("Runtime path not found: {}", configured));
      nr::Log(nr::LogLevel::WARN, entry.snippet_problem);
    }
  }
  // No runtime: no helper starts, and the overlay's card is the RUNTIME card.
  if (!entry.snippet_problem.empty()) return;

  if (remote_owner_ != device || !remote_) {
    ipc::Attach attach;
    const LUID luid = entry.Luid();
    attach.luid_low = luid.LowPart;
    attach.luid_high = luid.HighPart;
    attach.api = (vulkan   ? ipc::Api::VULKAN
                  : opengl ? ipc::Api::OPENGL
                  : d3d12  ? ipc::Api::D3D12
                  : d3d9   ? ipc::Api::D3D9
                  : d3d10  ? ipc::Api::D3D10
                           : ipc::Api::D3D11);
    attach.transport = entry.Kind();
    ipc::CopyText(attach.snippet_path, Utf8FromPath(entry.snippet));
    ipc::CopyText(attach.ngx_data_directory, Utf8FromPath(ngx_data_directory_));
    ipc::CopyText(attach.addon_file, addon_file_);
    if (!remote_) {
      remote_ = std::make_unique<client::RemoteNr>(client::RemoteNrConfig{
          .helper_exe = addon_directory_ / HELPER_FILE,
          .build_id = UPLIFT_BUILD_ID,
          .attach = attach,
      });
    } else {
      remote_->SetAttach(attach);
    }
    remote_owner_ = device;
  }
  client::RemoteNr& remote = *remote_;
  // Plans 11 and 12: a Vulkan, OpenGL or Direct3D 12 device's host, on the effect runtime's queue (none for every other API); OpenGL's is valid only while the
  // present is on the runtime's own context.
  Hosts hosts = HostsOf(entry, device, frame.queue, frame.present_queue);

  // The FRAME (design §2.2): what crosses every present.
  ipc::Frame wire = {
      .settings_generation = settings_generation_,
      .drag_bits = ipc::DragBits(frame.overlay->drag),
      .defaults_view = (frame.overlay->defaults_view ? 1u : 0u),
      .marker_expected = (frame.marker.handle != 0u ? 1u : 0u),
      .nr_allowed = (frame.nr_allowed ? 1u : 0u),
      .unreported_point = static_cast<uint32_t>(entry.unreported_point),
      .retry_now = (entry.retry_now ? 1u : 0u),
  };
  wire.drain_now = (frame.drain_now ? 1u : 0u);
  wire.uplift_mv_lumenite = (entry.uplift_mv_lumenite ? 1u : 0u);  // 2026-10-08: the helper's readouts name UPLIFT_MV's source
  // Plan 5 (key decision 8): without a running UPLIFT_MASK there is no mask to bind. Shared on Direct3D 9 (both transports, Plan 10) and on
  // Direct3D 10 and 11 with fences (design §2.8).
  bool mask_running = false;
  if (entry.SharesImages()) {
    const api::effect_texture_variable mask_variable =
        (settings.mask == ui::MaskMode::AUTO && frame.runtime != nullptr && frame.runtime->get_effects_state())
            ? frame.runtime->find_texture_variable(nullptr, MASK_TEXTURE)
            : api::effect_texture_variable{0u};
    const bool mask_declared = mask_variable.handle != 0u;
    mask_running = mask_declared && MaskEffectRunning(frame.runtime, mask_variable);
    if (!mask_running) {
      entry.ReleaseMask(hosts.Pointers());
      entry.mask_note = (settings.mask != ui::MaskMode::AUTO) ? ""
                        : mask_declared                       ? "NR mask: the UPLIFT_MASK effect is off"
                                                              : "NR mask: no effect writes UPLIFT_MASK";
    }
  } else {
    entry.mask_note = (d3d10 ? "NR mask: not available on this Direct3D 10 device (no fences)"
                             : "NR mask: not available on this Direct3D 11 device (no fences)");
  }
  wire.mask_running = (mask_running ? 1u : 0u);
  if (frame.now - entry.last_memory_query >= MEMORY_PERIOD) {
    entry.last_memory_query = frame.now;
    if (!entry.adapter) {
      const LUID luid = entry.Luid();
      Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&entry.adapter));
      }
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO memory = {};
    if (entry.adapter && SUCCEEDED(entry.adapter->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
      entry.memory_budget = memory.Budget;
      entry.memory_usage = memory.CurrentUsage;
    }
  }
  wire.game_budget = entry.memory_budget;  // kept until a FRAME carries it (0 = not measured)
  wire.game_usage = entry.memory_usage;
  // A marker frame's source is ReShade's own render target at the technique, so a multisampled back buffer is fine then. The D3D9
  // client's objects follow the helper's Session (`entry.running`), so a Session that is OFF for its own reason with NR on
  // (a VRAM suspension, its device removed, a share it could not make) costs no surfaces at all. A first FRAME that answers late
  // (NGX loading) leaves `entry.running` stale for one present: the block the helper hands out then finds nothing to map it into,
  // and Describe's `block_mapped` makes the helper hand one out again.
  if (entry.vulkan) {
    wire.target = entry.vulkan->Describe(*hosts.vulkan, VulkanImageInfo(device, frame.back_buffer), settings.enabled, entry.running);
  } else if (entry.gl) {
    // FB0 describes the target at the present event (the Session's size and shared format); the technique's intermediate has FB0's size and format family
    // (design §3.2). MSAA is refused only when NR would run at PRESENT (no marker): `at_present`.
    wire.target = entry.gl->Describe(*hosts.opengl, GlImageInfo(device, frame.back_buffer), /*at_present=*/frame.marker.handle == 0u, settings.enabled,
                                     entry.running);
  } else if (entry.d3d12) {
    wire.target = entry.d3d12->Describe(*hosts.d3d12, AsNative<ID3D12Resource>(frame.back_buffer.handle)->GetDesc(), settings.enabled, entry.running);
  } else if (entry.d3d9) {
    wire.target = entry.d3d9->Describe(AsNative<IDirect3DSurface9>(frame.back_buffer.handle), entry.running, /*at_present=*/frame.marker.handle == 0u);
  } else if (entry.d3d10) {
    wire.target = entry.d3d10->Describe(AsNative<ID3D10Resource>(frame.back_buffer.handle), settings.enabled, entry.running);
  } else {
    wire.target = entry.d3d11->Describe(AsNative<ID3D11Resource>(frame.back_buffer.handle), settings.enabled, entry.running);
  }
  wire.target.color_space = static_cast<uint32_t>(frame.swapchain->get_color_space());

  std::optional<ipc::Reply> reply = remote.Present(settings.enabled, wire, settings_text_, HELPER_LOG);
  if (remote.Attached() && remote.LastStatus().device_lost != 0u) {
    // Plan 17: the helper's own Direct3D 12 device was removed, so its NR would stay off for the session. The helper is ended as a hung one is, and Retry
    // now starts a fresh one with a new device, under the same 2-strike rule as a bridge's private device.
    remote.Abort("Its Direct3D 12 device was removed (ReShade.log has the details)");
    reply.reset();
  }
  if (remote_owner_ == device) {
    entry.strikes.Note(!remote.Failure().empty(), true);  // a fresh helper process brings a fresh device: always retryable
  }
  if (remote.FrameSent()) {
    if (entry.vulkan) {
      entry.vulkan->NoteFrameSent();  // the colour handle a reply (or a stash of an earlier one) brings is for this target, not the next present's
    } else if (entry.gl) {
      entry.gl->NoteFrameSent();
    } else if (entry.d3d12) {
      entry.d3d12->NoteFrameSent();
    }
    entry.unreported_point = TriggerPoint::NONE;  // the helper has seen it
    entry.retry_now = false;
    entry.memory_budget = 0u;
    entry.memory_usage = 0u;
  }
  if (reply) {
    entry.running = (reply->running != 0u);
    entry.frame_ready = (reply->frame_ready != 0u);
    bool fell_back = false;
    if (entry.vulkan) {
      fell_back = entry.vulkan->Apply(*reply, remote, *hosts.vulkan);
    } else if (entry.gl) {
      fell_back = entry.gl->Apply(*reply, remote, *hosts.opengl);
    } else if (entry.d3d12) {
      entry.d3d12->Apply(*reply, remote, *hosts.d3d12);
    } else if (entry.d3d9) {
      entry.d3d9->Apply(*reply, remote);
    } else if (entry.d3d10) {
      fell_back = entry.d3d10->Apply(*reply, remote);
    } else {
      fell_back = entry.d3d11->Apply(*reply, remote);
    }
    if (fell_back) {
      // The device could not open the helper's fences and the client is KMT now: the helper drops its FENCED transport (its Session
      // stays), and the next present attaches afresh with Kind(), as after a Direct3D 9 Reset.
      remote.Detach();
      remote_owner_ = nullptr;
      entry.frame_ready = false;
    }
  } else if (!remote.Running()) {
    // The helper exited, failed, or has not started: everything shared with it goes (design §2.7).
    entry.running = false;
    entry.nr_returned_logged = false;
    entry.ReleaseAll(hosts.Pointers());
  }
  // The first event of the frame; at PRESENT NR runs now, otherwise at the marker or after the effects. Direct3D 9 keeps NR before effects in the present
  // event (the helper replays this point; 1.1.6's begin-effects wait is not passed to it).
  RunClient(entry, device, frame.queue, entry.trigger.OnPresent(frame.marker.handle != 0u, /*effects_on=*/false), frame.back_buffer.handle, 0u, /*at_present=*/true);
}

void HelperFront::Technique(HelperDevice& device, api::effect_runtime* runtime, bool is_marker, uint64_t rtv, const ui::Settings& settings) {
  const TriggerPoint point = device.trigger.OnTechnique(is_marker);
  if (point == TriggerPoint::NONE) return;
  // Plan 6 (v2 design §3.20), Plan 7 (decision 3): at the Uplift technique, the UPLIFT_MV its pass just wrote while LaunchPad
  // runs, and only when MotionVectors can use it (a 32 MiB copy at 4K otherwise); FENCED only (design §2.8). Plan 10 (design §4.3): Direct3D 9
  // too, on both transports, while LAUNCHPAD_ON_D3D9 holds (the one switch): the binding is the IDirect3DTexture9 itself.
  // 2026-10-08: Lumenite's Kernel too (from Direct3D 10 on), whose vectors Uplift.fx writes into the same UPLIFT_MV.
  api::resource launchpad = {0u};
  const bool launchpad_here = (!device.d3d9 || LAUNCHPAD_ON_D3D9);
  const bool lumenite_here = (!device.d3d9 || LUMENITE_ON_D3D9);
  if (device.SharesImages() && (launchpad_here || lumenite_here) && MotionUsesUpliftMv(settings.motion_vectors)) {
    const api::effect_technique launchpad_technique = (launchpad_here ? runtime->find_technique(nullptr, LAUNCHPAD_TECHNIQUE) : api::effect_technique{0u});
    const api::effect_technique lumenite_technique = (lumenite_here ? runtime->find_technique(nullptr, LUMENITE_TECHNIQUE) : api::effect_technique{0u});
    const bool source_on = ((launchpad_technique.handle != 0u && runtime->get_technique_state(launchpad_technique))
                            || (lumenite_technique.handle != 0u && runtime->get_technique_state(lumenite_technique)));
    const api::effect_texture_variable motion = runtime->find_texture_variable(nullptr, MOTION_TEXTURE);
    if (source_on && motion.handle != 0u) {
      api::resource_view view = {0u};
      api::resource_view view_srgb = {0u};
      runtime->get_texture_binding(motion, &view, &view_srgb);
      if (view.handle != 0u) {
        launchpad = runtime->get_device()->get_resource_from_view(view);
      }
    }
  }
  // Direct3D 9: the event's render target, bit 0 marking sRGB (design §2.8): for X8 and multisampled back buffers it is
  // ReShade's resolved copy, which is where NR must read and write. OpenGL: ReShade's intermediate, the rtv's resource (design §3.2).
  RunClient(device, runtime->get_device(), runtime->get_command_queue(), point, EventImage(device, runtime, rtv), launchpad.handle, /*at_present=*/false);
}

void HelperFront::FinishEffects(HelperDevice& device, api::effect_runtime* runtime, uint64_t rtv, const ui::Settings& settings) {
  RunClient(device, runtime->get_device(), runtime->get_command_queue(), device.trigger.OnFinishEffects(), EventImage(device, runtime, rtv), 0u,
            /*at_present=*/false);
  // Plan 5 (key decision 8): this frame's UPLIFT_MASK, copied now that the effects have run, into the shared mask the next
  // RUN hands to NR. Direct3D 9, and 10 and 11 with fences (the note is set in the present event otherwise).
  if (!device.SharesImages() || settings.mask != ui::MaskMode::AUTO) return;
  const api::effect_texture_variable variable = runtime->find_texture_variable(nullptr, MASK_TEXTURE);
  if (variable.handle == 0u) return;
  Hosts hosts = HostsOf(device, runtime->get_device(), runtime->get_command_queue());  // OpenGL: no present queue in an effect event
  if (!MaskEffectRunning(runtime, variable)) {
    device.ReleaseMask(hosts.Pointers());
    device.mask_note = "NR mask: the UPLIFT_MASK effect is off";
    return;
  }
  api::resource_view view = {0u};
  api::resource_view view_srgb = {0u};
  runtime->get_texture_binding(variable, &view, &view_srgb);
  if (view.handle == 0u) return;
  const api::resource mask = runtime->get_device()->get_resource_from_view(view);
  if (mask.handle == 0u || !remote_) return;
  if (remote_->LastStatus().session_state != static_cast<uint32_t>(nr::SessionState::ACTIVE)) {
    device.mask_note = "NR mask: waiting for NR to run";
    return;
  }
  client::MaskCopy copied;
  if (device.vulkan) {
    copied = device.vulkan->CopyMask(*hosts.vulkan, VulkanImageInfo(runtime->get_device(), mask), *remote_);
  } else if (device.gl) {
    copied = device.gl->CopyMask(*hosts.opengl, GlImageInfo(runtime->get_device(), mask), *remote_);
  } else if (device.d3d12) {
    copied = device.d3d12->CopyMask(*hosts.d3d12, AsNative<ID3D12Resource>(mask.handle), *remote_);
  } else if (device.d3d9) {
    copied = device.d3d9->CopyMask(AsNative<IDirect3DTexture9>(mask.handle), *remote_);
  } else if (device.d3d10) {
    copied = device.d3d10->CopyMask(AsNative<ID3D10Resource>(mask.handle), *remote_);
  } else {
    copied = device.d3d11->CopyMask(AsNative<ID3D11Resource>(mask.handle), *remote_);
  }
  if (copied.size) {
    device.mask_note = std::format("NR mask: UPLIFT_MASK ({}x{})", copied.size->width, copied.size->height);
  } else if (copied.unsupported) {
    device.mask_note = (device.d3d9 ? std::format("NR mask: UPLIFT_MASK's format is not supported on Direct3D 9 (D3DFMT {})", copied.d3d9_format)
                                    : std::string("NR mask: UPLIFT_MASK's format is not supported"));
  } else if (copied.relay_failed) {
    device.mask_note = "NR mask: UPLIFT_MASK could not be shared with Uplift's relay device";  // Plan 8's text: the Direct3D 10 keyed share
  } else if (!copied.busy) {
    device.mask_note = "NR mask: UPLIFT_MASK could not be shared with Uplift's helper";
  }
}

// Design §2.7: ReShade raises no destroy_device at a Direct3D 9 Reset or ResetEx. What it does raise, after it has released its own
// resources (swap chain, effect runtime, back-buffer views, auto depth-stencil) and before it calls the real Reset, is
// destroy_command_queue (d3d9_device.cpp Reset/ResetEx -> on_reset). On Direct3D 9 the queue is the device. A live D3DPOOL_DEFAULT
// resource makes the game's Reset fail with D3DERR_INVALIDCALL, so every object this add-on made on the device goes here (the twins,
// the plain surface, the shared texture, the event query) and the helper is detached (it keeps its Session). The entry stays: the
// client makes its objects again at the next present, and the next FRAME attaches afresh. The device's destructor raises this event
// too, just before destroy_device.
void HelperFront::DestroyQueue(HelperDevice& device, api::device* owner) {
  if (!device.d3d9) return;
  device.d3d9->ReleaseAll();
  device.nr_returned_logged = false;
  if (remote_ && remote_owner_ == owner) {
    remote_->Detach();
    remote_owner_ = nullptr;
  }
}

// Design §2.7: the device is going away. Everything this add-on made on it is released and the helper detached (a Direct3D 9
// device already did both at destroy_command_queue).
std::unique_ptr<client::D3D10Client> HelperFront::DestroyDevice(HelperDevice* device, api::device* owner) {
  if (owner->get_api() == api::device_api::d3d9) {
    // Design §2.9: a 9Ex device that was created and then destroyed is neither of the two cases the pending marker guards (a
    // CreateDeviceEx that failed leaves no device; a crash leaves no destroy_device). A game that makes a probe device and
    // destroys it without presenting must not turn the toggle off at the next load.
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> device_ex;
    if (SUCCEEDED(AsNative<IDirect3DDevice9>(owner->get_native())->QueryInterface(IID_PPV_ARGS(&device_ex)))) {
      DeleteLatchMarker(ex_marker_path_);
    }
  }
  if (device == nullptr) return nullptr;
  if (device->vulkan) {
    // Vulkan design §6, R62: ReShade has already deleted its queues and dropped the device from its dispatch map, so the host has no queue and this is
    // only ever ReShade's destroy_resource for the images and functions its layer does not intercept for the rest.
    ReshadeVkHost host(owner, nullptr, device->vulkan->VulkanDevice().vkQueueSubmit);
    device->vulkan->FreeVulkan(host);
  } else if (device->gl) {
    // OpenGL design §3.6: ReShade makes the share group's last context current only for its own teardown, so no GL call is made: the names die with it.
    device->gl->ForgetGl();
  } else if (device->d3d12) {
    // Design §6: no host (ReShade may already have released its queue). A capped wait for the retire fence, then everything is released (or leaked on a
    // timeout) and the native device reference is dropped here, before ReShade's own release.
    device->d3d12->DestroyDevice();
  } else {
    device->ReleaseAll();
  }
  if (remote_ && remote_owner_ == owner) {
    remote_->Detach();
    remote_owner_ = nullptr;
  }
  return std::move(device->d3d10);  // the host destroys it after its lock: the relay is ReShade's proxy
}

void HelperFront::Overlay(api::device* game_device, const HelperDevice* device, api::device_api device_api, const ui::Settings& settings,
                          ui::OverlayView* view, ui::OverlayState* state, std::chrono::steady_clock::time_point now) const {
  const bool d3d9 = (device_api == api::device_api::d3d9);
  // Plan 14 (design §1.2, §1.3): every game this front hosts runs NR in the helper, on the presented image: the DLSS choices and Match game are fixed causes
  // (the host set view->dlss_unavailable), and what runs comes back in the helper's status.
  ui::SetupFacts facts;
  FillPreference(&facts, settings, false);
  facts.dlss_unavailable = view->dlss_unavailable;
  facts.match_game_readable = false;
  // Fix round 1 (M5): a 64-bit Vulkan device on the helper route. The DLSS stages are greyed with the route's reason (the host's), and DLSS's vectors with
  // their own: the native context copies them only for a Present that runs on the game's device or a private Direct3D 12 device.
  if (!view->vulkan_stages_off.empty()) {
    facts.after_dlss_fixed = view->vulkan_stages_off;
    facts.before_upscaling_fixed = view->vulkan_stages_off;
  }
  if (device != nullptr && !device->route_reason.empty()) {
    facts.dlss_motion_fixed = "DLSS's motion vectors do not reach Uplift's helper process (Launchpad's and Lumenite's do)";
  }
  facts.launchpad_ready = (device != nullptr && device->launchpad_ready);
  facts.lumenite_ready = (device != nullptr && device->lumenite_ready);
  facts.uplift_mv_lumenite = (device != nullptr && device->uplift_mv_lumenite);
  if (device != nullptr && device->HasClient() && !device->SharesImages()) {
    // Batch 1 review: a Direct3D 10 or 11 device on the CPU-ordered fallback shares no images with the helper, so UPLIFT_MV never reaches NR (Technique skips it).
    facts.launchpad_fixed = (device->d3d10 ? "Launchpad's vectors are not available on this Direct3D 10 device (no fences)"
                                           : "Launchpad's vectors are not available on this Direct3D 11 device (no fences)");
    facts.lumenite_fixed = (device->d3d10 ? "Lumenite's vectors are not available on this Direct3D 10 device (no fences)"
                                          : "Lumenite's vectors are not available on this Direct3D 11 device (no fences)");
  }
  if (d3d9 && !LUMENITE_ON_D3D9) {
    facts.lumenite_fixed = LUMENITE_D3D9_REASON;  // Uplift.fx's Lumenite pass is compiled from Direct3D 10 on
  }
  std::string api_label = (addon_file_ == "gitc-uplift.addon32" ? std::format("{} (32-bit)", ApiName(device_api)) : std::string(ApiName(device_api)));
  if (device != nullptr && !device->route_reason.empty()) {
    api_label = std::format("{} (helper)", ApiName(device_api));  // T5: a 64-bit Vulkan device's route, as "Vulkan (native NR)" and "Vulkan (Direct3D 12)"
  }
  facts.api = api_label;
  if (d3d9) {
    if (device != nullptr) {
      // Design §2.9: which case applies, from the device, the toggle, and whether ReShade ever raised create_device.
      view->d3d9ex_readout = (device->d3d9_ex ? "Now: Direct3D 9Ex: frames reach NR on the GPU"
                              : !settings.use_d3d9ex
                                  ? (ex_turned_off_
                                         ? "Now: plain Direct3D 9: 9Ex was turned off because the last start with it did not reach its first frame"
                                         : "Now: plain Direct3D 9: frames reach NR through shared memory")
                              : ex_event_seen_
                                  ? "Now: plain Direct3D 9 (the switch applies at the next game start)"
                                  : "Now: plain Direct3D 9: this ReShade cannot switch games to 9Ex (it needs the create_device add-on event)");
    }
    if (!LAUNCHPAD_ON_D3D9) {
      view->motion_line = "Motion vectors: none (Launchpad does not run on Direct3D 9)";
    }
  }
  if (device == nullptr) {
    view->card = ui::BuildStatusCard({.device_problem = "Waiting for the game's first frame"});
    ui::FinishSetup(view, state, game_device, facts, now);
    return;
  }
  const client::RemoteNr* const remote = remote_.get();
  const bool owner = (remote != nullptr && remote_owner_ == game_device);
  const std::string_view helper_problem = (owner ? remote->Failure() : std::string_view());
  std::string_view latch;
  if (device->d3d10) {
    latch = device->d3d10->Latch();
  } else if (device->vulkan) {
    latch = device->vulkan->Latch();
  } else if (device->gl) {
    latch = device->gl->Latch();
  } else if (device->d3d12) {
    latch = device->d3d12->Latch();
  } else if (device->d3d11) {
    latch = device->d3d11->Latch();
  }
  const bool has_client = device->HasClient();
  const bool status_known = (owner && remote->Running() && remote->Attached());
  const ipc::Status* const status = (status_known ? &remote->LastStatus() : nullptr);
  const std::string_view api_name = ApiName(device_api);
  if (status != nullptr) {
    view->status_line = std::string(ipc::TextOf(status->status_line));
    if (const std::string_view line = ipc::TextOf(status->placement_line); !line.empty()) {
      view->placement_line = std::string(line);
    }
    if (!d3d9 || LAUNCHPAD_ON_D3D9) {
      if (const std::string_view line = ipc::TextOf(status->motion_line); !line.empty()) {
        view->motion_line = std::string(line);
      }
    }
    view->work_line = std::string(ipc::TextOf(status->work_line));
    view->exposure_line = std::string(ipc::TextOf(status->exposure_line));  // Plan 17
    view->ui_correction_note = std::string(ipc::TextOf(status->ui_correction_note));
    view->mask_note = device->mask_note;
    view->intermediate_bytes = status->intermediate_bytes;
    if (status->has_runtime_bytes != 0u) {
      view->runtime_bytes = status->runtime_bytes;
    }
    view->effective_diffuse_white_nits = status->diffuse_white_nits;
  }
  if (has_client) {
    uint64_t client_bytes = 0u;
    std::string client_line;
    if (device->d3d9) {
      client_bytes = device->d3d9->SharedBytes();
      client_line = device->d3d9->Line();
    } else if (device->d3d10) {
      client_bytes = device->d3d10->SharedBytes();
      client_line = device->d3d10->Line();
    } else if (device->vulkan) {
      client_line = device->vulkan->Line(device->route_reason);  // its imports alias the helper's textures, which the helper's own figure counts
    } else if (device->gl) {
      client_line = device->gl->Line();  // the same
    } else if (device->d3d12) {
      client_line = device->d3d12->Line();  // the same
    } else {
      client_bytes = device->d3d11->SharedBytes();
      client_line = device->d3d11->Line();
    }
    view->intermediate_bytes += client_bytes;
    std::string line = ((remote != nullptr && owner && remote->Running()) ? std::move(client_line)
                                                                          : std::format("{} ({}): Uplift's 64-bit helper starts when NR is turned on", api_name, client::BITNESS));
    const uint64_t busy = (status != nullptr ? status->busy_skips : 0u) + (owner ? remote->BusyFrames() : 0u);
    if (busy > 0u) {
      line += std::format(", {} frame(s) without NR while the helper was busy", busy);
    }
    view->api_line = std::move(line);
  }
  // The first failing stage, as in 64-bit games (Plan 6, D1), with the helper's own stage after the device's.
  if (device->claimed_elsewhere) {
    view->card = ui::BuildStatusCard({.claimed_elsewhere = true, .enabled = true});
  } else if (!device->message.empty()) {
    view->card = ui::BuildStatusCard({.device_problem = device->message});
  } else if (!device->snippet_problem.empty()) {
    view->card = ui::BuildStatusCard({.blocked = device->snippet_problem, .blocked_stage = ui::CardStage::RUNTIME, .enabled = settings.enabled, .addon_file = addon_file_});
  } else if (!helper_problem.empty()) {
    view->card = ui::BuildStatusCard({.helper_problem = helper_problem, .private_stops_final = device->strikes.Exhausted(), .enabled = settings.enabled});
  } else if (!latch.empty()) {
    view->card = ui::BuildStatusCard({.device_problem = latch});
  } else if (status != nullptr) {
    view->card = ui::BuildStatusCard({
        .device_lost = (status->device_lost != 0u),
        .blocked = ipc::TextOf(status->blocked),
        .blocked_stage = static_cast<ui::CardStage>(status->blocked_stage),
        .enabled = settings.enabled,
        .session = SessionOf(*status),
        .output_problem = ipc::TextOf(status->output_problem),
        .placement_note = ipc::TextOf(status->placement_note),
        .nr_applied = (status->nr_applied != 0u),
        .passes_run = status->passes_run,
        .skip_reason = ipc::TextOf(status->skip_reason),
        .skip_from_session = (status->skip_from_session != 0u),
        .working_line = view->status_line,
        .addon_file = addon_file_,
    });
  } else if (settings.enabled && has_client && owner && remote->Running()) {
    // The helper's process is up but not attached yet (design §2.2, start-up).
    view->card = {.stage = ui::CardStage::SESSION, .title = "Starting NR", .reason = "Starting Uplift's 64-bit helper"};
  } else {
    view->card = ui::BuildStatusCard({.enabled = settings.enabled, .addon_file = addon_file_});
  }
  // Plan 14: what runs (the helper's context decided it), and the card's detail.
  if (status != nullptr) {
    FillRunning(&facts, *status, view->card.working);
  }
  if (device->strikes.Exhausted() && !helper_problem.empty()) {
    facts.stopped = ui::PRIVATE_DEVICE_STOPPED_TWICE_REASON;  // Plan 17: the helper's second stop is a fixed cause
  }
  facts.vram_bytes = view->runtime_bytes.value_or(0u) + view->intermediate_bytes;
  facts.passes_note = ((view->card.working && !view->card.fixes.empty()) ? std::string_view(view->card.fixes.front()) : std::string_view());
  ui::FinishSetup(view, state, game_device, facts, now);
}

void HelperFront::RetryNow(HelperDevice* device) {
  if (remote_ && !remote_->Failure().empty()) {
    // Plan 17: the 2-strike rule; the card offers no Retry now after the second stop, and this refuses it too.
    if (device != nullptr) {
      if (!device->strikes.Retried()) return;
      device->ClearClientLatch();
    }
    remote_->RetryNow();
  } else if (device != nullptr) {
    device->retry_now = true;
  }
}

void HelperFront::Detach(api::device* owner) {
  if (remote_ && remote_owner_ == owner) {
    remote_->Detach();
    remote_owner_ = nullptr;
  }
}

bool HelperFront::CreateDevice(api::device_api device_api, uint32_t& api_version, bool use_d3d9ex) {
  ex_event_seen_ = true;
  if (device_api != api::device_api::d3d9 || !use_d3d9ex) return false;
  // Written before the device exists and deleted when a 9Ex device's first Present has returned or the device is destroyed: a
  // refused CreateDeviceEx or a start that dies before that leaves it, and turns the toggle off at the next start.
  WriteLatchMarker(ex_marker_path_, "Direct3D 9Ex requested; waiting for the first frame");
  api_version = D3D9EX_API_VERSION;
  NoteD3D9ExRequested();  // 1.1.5: exclusive fullscreen becomes a window of the same size (swapchain_usage.hpp)
  nr::Log(nr::LogLevel::INFO, "Direct3D 9: asking ReShade for a Direct3D 9Ex device (UseD3D9Ex)");
  return true;
}

void HelperFront::Quit() {
  if (remote_) {
    remote_->Quit();  // QUIT, a capped wait for the exit, then the job closes: the helper never outlives the game
  }
}

void RegisterCreateDeviceEvent(bool (*callback)(api::device_api device_api, uint32_t& api_version)) {
  const auto register_event = reinterpret_cast<void (*)(reshade::addon_event, void*)>(
      GetProcAddress(reshade::internal::get_reshade_module_handle(), "ReShadeRegisterEvent"));
  if (register_event != nullptr) {
    register_event(static_cast<reshade::addon_event>(CREATE_DEVICE_EVENT), reinterpret_cast<void*>(callback));
  }
}

}  // namespace uplift::addon
