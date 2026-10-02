#include "addon/ngx_bridge.hpp"

#include <format>

namespace uplift::addon {

const void* NgxBridge::DeviceOf(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list) const {
  return (api == ngx_hooks::NgxApi::VULKAN ? routing_.DeviceOfVk(list, VK_NULL_HANDLE) : routing_.DeviceOf(list));
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
  // Plan 13: a Vulkan create names its device through its command buffer, or CreateFeature1's VkDevice.
  const void* const device =
      (call.api == ngx_hooks::NgxApi::VULKAN ? routing_.DeviceOfVk(call.list, call.vk_device) : routing_.DeviceOf(call.list));
  registry_.OnCreate(handle, call.feature, device, ngx_hooks::ReadCreateSnapshot(*call.parameters));
}

void NgxBridge::BeforeEvaluate(ngx_hooks::EvaluateCall* call) {
  if (!pre_sr_.load(std::memory_order_relaxed)) return;  // Before upscaling is off: one relaxed load per evaluate
  // Batch 3 review I-2: on Vulkan only a native context records before upscaling. Without one watching, nothing else is asked (not even the registry's lock).
  if (call->api == ngx_hooks::NgxApi::VULKAN && vk_nr_contexts_.load(std::memory_order_relaxed) <= 0) return;
  const std::optional<ngx_hooks::FeatureRecord> record = registry_.Find(call->handle);
  // v2 design §3.9: SuperSampling only; Ray Reconstruction keeps NR after DLSS.
  if (!record || record->feature != NVSDK_NGX_Feature_SuperSampling) return;
  const void* const device = DeviceOf(call->api, call->list);
  if (device == nullptr) return;  // a list Uplift does not track
  if (registry_.MainHandle(device, routing_.SwapchainSize(device)) != call->handle) return;
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
  } else {
    routing_.ColorSwapRejected(device, list);
  }
}

void NgxBridge::AfterEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                              const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Result result) {
  if (api == ngx_hooks::NgxApi::D3D12) {
    if (NVSDK_NGX_FAILED(result) || parameters == nullptr) return;
    const std::optional<ngx_hooks::FeatureRecord> record = registry_.Find(handle);
    if (!record || ngx_hooks::KindOf(record->feature) != ngx_hooks::FeatureKind::UPSCALER) return;
    const void* const device = routing_.DeviceOf(list);
    if (device == nullptr) return;  // a list Uplift does not track
    if (registry_.MainHandle(device, routing_.SwapchainSize(device)) != handle) return;
    routing_.OnMainEvaluate(device, list, ngx_hooks::CaptureDlssFrame(*parameters, handle, *record));
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
  if (registry_.MainHandle(device, routing_.SwapchainSize(device)) != handle) return;
  ngx_hooks::VkDlssResources copies;
  const ngx_hooks::DlssFrame frame = ngx_hooks::CaptureVkDlssFrame(*parameters, handle, *record, &copies);
  routing_.OnMainEvaluateVk(device, list, frame, copies);
}

void NgxBridge::BeforeRelease(NVSDK_NGX_Handle* handle) {
  registry_.OnRelease(handle);
}

void NgxBridge::BeforeCoreShutdown(ngx_hooks::NgxApi api, void* device, const void* caller) {
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
