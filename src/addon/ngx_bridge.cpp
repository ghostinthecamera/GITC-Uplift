#include "addon/ngx_bridge.hpp"

#include <wrl/client.h>

#include <format>

#include "ngx_hooks/ngx_parameters.hpp"
#include "nr/d3d11_handles.hpp"
#include "nr/log.hpp"

namespace uplift::addon {
namespace {

// Plan 18 fix round 1 (I-1): a 2D texture's description, or all zero (not a 2D texture). The game's resource, read during its own evaluate call.
D3D11_TEXTURE2D_DESC D3D11TextureDescription(ID3D11Resource* resource) {
  D3D11_TEXTURE2D_DESC description = {};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (resource != nullptr && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
    texture->GetDesc(&description);
  }
  return description;
}

// A 2D texture's size, or empty (not a 2D texture).
nr::Size D3D11TextureSize(ID3D11Resource* resource) {
  const D3D11_TEXTURE2D_DESC description = D3D11TextureDescription(resource);
  return {description.Width, description.Height};
}

// Plan 18 final review: the formats a scene-linear output has. A mod usually injects DLSS before tone mapping, so a float Output says the game's DLSS
// runs on HDR. The 8-bit and 10-bit formats are display-referred (or say nothing), so they never infer it.
bool IsFloatHdrFormat(DXGI_FORMAT format) {
  return (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS || format == DXGI_FORMAT_R11G11B10_FLOAT
          || format == DXGI_FORMAT_R32G32B32A32_FLOAT);
}

}  // namespace

const void* NgxBridge::DeviceOf(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list) const {
  return (api == ngx_hooks::NgxApi::VULKAN ? routing_.DeviceOfVk(list, VK_NULL_HANDLE)
          : api == ngx_hooks::NgxApi::D3D11 ? routing_.DeviceOfD3D11(list)
                                            : routing_.DeviceOf(list));
}

void NgxBridge::BeforeCreate(ngx_hooks::CreateCall* call) {
  // New hardening (fix round 1): never take the add-on's lock for Uplift's own NGX creates. Checked
  // before anything else, whatever the feature, so a create from Uplift's own runtime can never
  // reach Overrides() below and re-lock the add-on's non-recursive mutex if the calling thread
  // already holds it (spec §6.1: Uplift's runtime creates no SuperSampling feature today, but this
  // must hold even if a future NR version ever did).
  if (routing_.IsOwnCode(call->caller)) return;
  if (call->feature != NVSDK_NGX_Feature_SuperSampling || call->parameters == nullptr) return;
  const ngx_hooks::SrOverrides overrides = routing_.Overrides();
  if (overrides.Any()) {
    ngx_hooks::ApplySrOverrides(call->parameters, overrides, &call->saved);
  }
}

void NgxBridge::AfterCreate(const ngx_hooks::CreateCall& call, NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result) {
  // Fix round 2, Important 2: the same own-code check as BeforeCreate, checked before anything else,
  // whatever the feature -- not just Reserved18. Uplift's runtime creates no SuperSampling feature
  // through the core today (spec §6.1), but if a future NR version ever did, this must still return
  // before the registry insert below: a registered handle would later reach AfterEvaluate, which calls
  // SwapchainSize() and re-locks the add-on's non-recursive mutex if the calling thread already holds
  // it -- the exact self-deadlock the own-create hardening exists to prevent, one call later.
  if (routing_.IsOwnCode(call.caller)) return;
  if (NVSDK_NGX_FAILED(result) || handle == nullptr || call.parameters == nullptr) return;
  // Plan 13: a Vulkan create names its device through its command buffer, or CreateFeature1's VkDevice. Plan 18: a Direct3D 11 one through its context.
  const void* const device =
      (call.api == ngx_hooks::NgxApi::VULKAN ? routing_.DeviceOfVk(call.list, call.vk_device)
       : call.api == ngx_hooks::NgxApi::D3D11 ? routing_.DeviceOfD3D11(call.list)
                                              : routing_.DeviceOf(call.list));
  registry_.OnCreate(handle, call.feature, device, ngx_hooks::ReadCreateSnapshot(*call.parameters), call.api);
}

void NgxBridge::BeforeEvaluate(ngx_hooks::EvaluateCall* call) {
  if (!pre_sr_.load(std::memory_order_relaxed)) return;  // Before upscaling is off: one relaxed load per evaluate
  // Batch 3 review I-2: on Vulkan only a native context records before upscaling. Without one watching, nothing else is asked (not even the registry's lock).
  if (call->api == ngx_hooks::NgxApi::VULKAN && vk_nr_contexts_.load(std::memory_order_relaxed) <= 0) return;
  // Plan 18: on Direct3D 11 only a bridge's context that watches records before upscaling; without one, nothing else is asked.
  if (call->api == ngx_hooks::NgxApi::D3D11 && d3d11_watching_.load(std::memory_order_relaxed) <= 0) return;
  const std::optional<ngx_hooks::FeatureRecord> record = registry_.Find(call->handle);
  // v2 design §3.9: SuperSampling only; Ray Reconstruction keeps NR after DLSS.
  if (!record || record->feature != NVSDK_NGX_Feature_SuperSampling) return;
  const void* const device = DeviceOf(call->api, call->list);
  if (device == nullptr) return;  // a list Uplift does not track
  if (registry_.MainHandle(device, routing_.SwapchainSize(device), call->api) != call->handle) return;  // Plan 18: within the evaluate's API
  if (call->api == ngx_hooks::NgxApi::D3D11) {
    call->color = routing_.BeforeMainEvaluateD3D11(device, call->list, ngx_hooks::CaptureD3D11DlssFrame(*call->parameters, call->handle, *record));
    return;
  }
  if (call->api == ngx_hooks::NgxApi::VULKAN) {
    ngx_hooks::VkDlssResources copies;
    const ngx_hooks::DlssFrame frame = ngx_hooks::CaptureVkDlssFrame(*call->parameters, call->handle, *record, &copies);
    call->color = routing_.BeforeMainEvaluateVk(device, call->list, frame, copies);
    return;
  }
  call->color = routing_.BeforeMainEvaluate(device, call->list, ngx_hooks::CaptureDlssFrame(*call->parameters, call->handle, *record));
}

void NgxBridge::OnColorSwapRejected(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* /*handle*/) {
  // BeforeEvaluate already gated this call's handle on the main SuperSampling handle before it ever set
  // call->color, so ColorSwap only reaches here for that same handle: no need to re-check the registry.
  const void* const device = DeviceOf(api, list);
  if (device == nullptr) return;  // a list Uplift does not track
  if (api == ngx_hooks::NgxApi::VULKAN) {
    routing_.ColorSwapRejectedVk(device, list);
  } else if (api == ngx_hooks::NgxApi::D3D11) {
    routing_.ColorSwapRejectedD3D11(device, list);
  } else {
    routing_.ColorSwapRejected(device, list);
  }
}

void NgxBridge::AfterEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                              const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Result result) {
  // Plan 18 (design §5): the process's first game DLSS evaluate is summarised once, whatever watches; one relaxed load from then on.
  if (!summary_logged_.load(std::memory_order_relaxed) && NVSDK_NGX_SUCCEED(result) && parameters != nullptr) {
    NoteFirstEvaluate(api, list, handle, *parameters);
  }
  if (api == ngx_hooks::NgxApi::D3D12) {
    if (NVSDK_NGX_FAILED(result) || parameters == nullptr) return;
    const std::optional<ngx_hooks::FeatureRecord> record = registry_.Find(handle);
    if (!record || ngx_hooks::KindOf(record->feature) != ngx_hooks::FeatureKind::UPSCALER) return;
    const void* const device = routing_.DeviceOf(list);
    if (device == nullptr) {
      NoteForeignD3D12Dlss();  // Plan 18: a list ReShade does not track
      return;
    }
    if (registry_.MainHandle(device, routing_.SwapchainSize(device), ngx_hooks::NgxApi::D3D12) != handle) return;
    routing_.OnMainEvaluate(device, list, ngx_hooks::CaptureDlssFrame(*parameters, handle, *record));
    return;
  }
  if (api == ngx_hooks::NgxApi::D3D11) {
    // Plan 18: without a watching Direct3D 11 context (none made yet, or NR off and released) a pure passthrough: no lookup, no lock, no capture.
    if (d3d11_watching_.load(std::memory_order_relaxed) <= 0) return;
    if (NVSDK_NGX_FAILED(result) || parameters == nullptr) return;
    std::optional<ngx_hooks::FeatureRecord> record = registry_.Find(handle);
    if (!record) {
      record = AdoptD3D11Upscaler(handle, list, *parameters);  // Plan 18 fix round 1 (I-1): created before the hooks
    }
    if (!record || ngx_hooks::KindOf(record->feature) != ngx_hooks::FeatureKind::UPSCALER) return;
    const void* const device = routing_.DeviceOfD3D11(list);
    if (device == nullptr) return;  // an unmarked device, or a deferred context
    if (registry_.MainHandle(device, routing_.SwapchainSize(device), ngx_hooks::NgxApi::D3D11) != handle) return;
    routing_.OnMainEvaluateD3D11(device, list, ngx_hooks::CaptureD3D11DlssFrame(*parameters, handle, *record));
    return;
  }
  // Batch 3 review I-2 and final review minor 1: without a native Vulkan context that watches (Source on Auto or Present, the default, also after a DLSS
  // pick was undone) no token can be open and nothing is recorded, so the evaluate is a pure passthrough: no registry lookup, no add-on lock
  // (SwapchainSize, OnMainEvaluateVk), no capture.
  if (vk_nr_contexts_.load(std::memory_order_relaxed) <= 0) return;
  // Plan 13 (key decision c). Only a feature the game created can have an Uplift token open on its command buffer (Uplift's own runtime
  // never registers), so the registry is asked first, lock-free. From there EndVulkanEvaluate closes the call on every path below, after
  // NR's own recording: the token's event follows everything Uplift recorded in this call.
  const std::optional<ngx_hooks::FeatureRecord> record = registry_.Find(handle);
  if (!record) return;
  struct CloseEvaluate {
    NgxRouting& routing;
    ID3D12GraphicsCommandList* list;
    ~CloseEvaluate() {
      try {
        routing.EndVulkanEvaluate(list);
      } catch (...) {
        // No-op: the hook's own catch logs observer failures; a destructor must not throw (the token then waits for the stale drop).
      }
    }
  } close{routing_, list};
  if (NVSDK_NGX_FAILED(result) || parameters == nullptr) return;
  if (ngx_hooks::KindOf(record->feature) != ngx_hooks::FeatureKind::UPSCALER) return;
  const void* const device = routing_.DeviceOfVk(list, VK_NULL_HANDLE);
  if (device == nullptr) return;  // a command buffer Uplift does not track
  if (registry_.MainHandle(device, routing_.SwapchainSize(device), ngx_hooks::NgxApi::VULKAN) != handle) return;
  ngx_hooks::VkDlssResources copies;
  const ngx_hooks::DlssFrame frame = ngx_hooks::CaptureVkDlssFrame(*parameters, handle, *record, &copies);
  routing_.OnMainEvaluateVk(device, list, frame, copies);
}

void NgxBridge::NoteFirstEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                  const NVSDK_NGX_Parameter& parameters) {
  // Only a game's upscaler: Uplift's own NR is never registered, and a frame-generation evaluate waits for the upscaler's.
  std::optional<ngx_hooks::FeatureRecord> record = registry_.Find(handle);
  if (!record && api == ngx_hooks::NgxApi::D3D11) {
    record = AdoptD3D11Upscaler(handle, list, parameters);  // Plan 18 fix round 1 (I-1): the summary even before NR was ever on
  }
  if (!record || ngx_hooks::KindOf(record->feature) != ngx_hooks::FeatureKind::UPSCALER) return;
  if (summary_logged_.exchange(true)) return;
  if (api == ngx_hooks::NgxApi::VULKAN) {
    ngx_hooks::VkDlssResources copies;
    routing_.LogFirstEvaluate(api, list, ngx_hooks::CaptureVkDlssFrame(parameters, handle, *record, &copies));
    return;
  }
  routing_.LogFirstEvaluate(api, list, (api == ngx_hooks::NgxApi::D3D11 ? ngx_hooks::CaptureD3D11DlssFrame(parameters, handle, *record)
                                                                        : ngx_hooks::CaptureDlssFrame(parameters, handle, *record)));
}

std::optional<ngx_hooks::FeatureRecord> NgxBridge::AdoptD3D11Upscaler(const NVSDK_NGX_Handle* handle, ID3D12GraphicsCommandList* list,
                                                                     const NVSDK_NGX_Parameter& parameters) {
  if (handle == nullptr || registry_.NotAdopted(handle)) return std::nullopt;
  ID3D11Resource* const color = ngx_hooks::ReadD3D11Resource(parameters, NVSDK_NGX_Parameter_Color);
  ID3D11Resource* const output = ngx_hooks::ReadD3D11Resource(parameters, NVSDK_NGX_Parameter_Output);
  ID3D11Resource* const motion = ngx_hooks::ReadD3D11Resource(parameters, NVSDK_NGX_Parameter_MotionVectors);
  ID3D11Resource* const depth = ngx_hooks::ReadD3D11Resource(parameters, NVSDK_NGX_Parameter_Depth);
  // Ray Reconstruction's guide buffers (nvsdk_ngx_defs_dlssd.h): not on Direct3D 11, so a block that has them is not a DLSS-SR one.
  const bool albedo = (ngx_hooks::ReadD3D11Resource(parameters, "DLSS.Input.DiffuseAlbedo") != nullptr
                       || ngx_hooks::ReadD3D11Resource(parameters, "DLSS.Input.SpecularAlbedo") != nullptr);
  if (color == nullptr || output == nullptr || (motion == nullptr && depth == nullptr) || albedo) {
    registry_.NoteNotAdopted(handle);
    return std::nullopt;
  }
  // The create keys when the game evaluates with its create block (NVIDIA's helpers do), else what the evaluate itself says: the render subrect or the
  // Color's size, the Output's size, and low-res vectors when they are smaller than the Output (DLSS 1 may leave the subrect and the jitter unset).
  ngx_hooks::CreateSnapshot snapshot = ngx_hooks::ReadCreateSnapshot(parameters);
  if (snapshot.output.Empty()) {
    snapshot.output = D3D11TextureSize(output);
  }
  if (snapshot.render.Empty()) {
    const nr::Size subrect = {ngx_hooks::ReadUint(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width).value_or(0u),
                              ngx_hooks::ReadUint(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height).value_or(0u)};
    snapshot.render = (subrect.Empty() ? D3D11TextureSize(color) : subrect);
  }
  // Without the create flags (no create block at the evaluate) the evaluate's own block is all there is: the vectors' size says low-res, the Output's format
  // says HDR (final review: else Encoding Auto reads an HDR game as sRGB). A feature that has its create flags keeps them exactly as they are.
  bool hdr_inferred = false;
  if (!ngx_hooks::ReadInt(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags).has_value()) {
    if (motion != nullptr) {
      const nr::Size vectors = D3D11TextureSize(motion);
      if (!vectors.Empty() && (vectors.width < snapshot.output.width || vectors.height < snapshot.output.height)) {
        snapshot.flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
      }
    }
    if (IsFloatHdrFormat(D3D11TextureDescription(output).Format)) {
      snapshot.flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
      hdr_inferred = true;
    }
  }
  const void* const device = routing_.DeviceOfD3D11(list);  // null: an unmarked device's (NR has not been on), the unknown device
  if (registry_.TryAdopt(handle, NVSDK_NGX_Feature_SuperSampling, device, snapshot, ngx_hooks::NgxApi::D3D11)) {
    // Final review: "not seen being created" is true of a feature made before the hooks and of one the game kept across an NGX shutdown that only dropped
    // a reference count; "HDR inferred" says the IsHDR flag is the Output's float format's, not the game's.
    nr::Logf(nr::LogLevel::INFO,
             "Direct3D 11: adopted the game's DLSS feature at its first evaluate (not seen being created: created before Uplift's hooks, or kept across an NGX "
             "shutdown; render {}x{} -> output {}x{}{})",
             snapshot.render.width, snapshot.render.height, snapshot.output.width, snapshot.output.height, (hdr_inferred ? ", HDR inferred" : ""));
  }
  return registry_.Find(handle);
}

void NgxBridge::BeforeRelease(NVSDK_NGX_Handle* handle) {
  registry_.OnRelease(handle);
}

void NgxBridge::BeforeCoreShutdown(ngx_hooks::NgxApi api, void* device, const void* caller) {
  if (api == ngx_hooks::NgxApi::D3D11) {
    // Plan 18 (design §6): Uplift makes no NGX call on a game's Direct3D 11 device (its NR is on the bridge's private Direct3D 12 device), so the game's
    // shutdown only ends what the core forgets: that device's DLSS features (every Direct3D 11 device's, for the device-less Shutdown). No add-on lock.
    if (routing_.IsOwnCode(caller)) return;
    // A device Uplift never marked (a mod's own, or one made before the mark) has only the unknown device's features, never a marked device's: `UNMARKED`
    // names no record's device, so OnCoreShutdown drops the unknown device's alone (a null owner would drop every Direct3D 11 feature).
    static const char UNMARKED = 0;
    const void* owner = nullptr;
    if (device != nullptr) {
      owner = routing_.DeviceOfD3D11Device(static_cast<ID3D11Device*>(device));
      if (owner == nullptr) owner = &UNMARKED;
    }
    const size_t forgotten = registry_.OnCoreShutdown(ngx_hooks::NgxApi::D3D11, owner);
    nr::Logf(nr::LogLevel::INFO, "Direct3D 11: the game shut NGX down on {} ({} DLSS feature(s) forgotten; NR on Uplift's private device is unaffected)",
             (device == nullptr ? "every device" : "its device"), forgotten);
    return;
  }
  // Never for Uplift's own code: the NR runtime's own teardown, which may reach the core, runs under the add-on's lock (the same rule as the creates').
  if (device == nullptr) return;
  if (api == ngx_hooks::NgxApi::D3D12) {
    // Plan 15: the routing tells the game's device from any other (a bridge's private device, Uplift's own core init) before it takes the add-on's lock.
    if (routing_.IsOwnCode(caller)) return;
    routing_.BeforeCoreShutdownD3D12(static_cast<ID3D12Device*>(device));
    return;
  }
  if (vk_contexts_.load(std::memory_order_relaxed) <= 0 || routing_.IsOwnCode(caller)) return;
  routing_.BeforeCoreShutdownVk(static_cast<VkDevice>(device));
}

std::string DlssUnavailableReason(const ui::Settings& settings, const std::optional<ModuleVersion>& reshade,
                                  bool hooks_started, std::string_view hook_error) {
  if (settings.ngx_hooks == ui::NgxHooksMode::OFF) return "NGX hooks are off (NgxHooks = Off)";
  if (!hooks_started) return std::format("NGX hooks could not start: {}", hook_error);
  if (!reshade) return "ReShade's version could not be read: DLSS placement needs ReShade 6.1 or newer";
  if (!SupportsDlssPlacement(*reshade)) {
    return std::format("DLSS placement needs ReShade 6.1 or newer (this is {})", FormatModuleVersion(*reshade));
  }
  if (settings.dlss_placement_blocked) {
    return std::string(LATCH_UNAVAILABLE_REASON);
  }
  return {};
}

}  // namespace uplift::addon
