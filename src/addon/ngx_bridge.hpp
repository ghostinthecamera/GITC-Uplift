#pragma once

#include <d3d11.h>
#include <d3d12.h>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include "addon/reshade_version.hpp"
#include "ngx_hooks/dlss_capture.hpp"
#include "ngx_hooks/dlss_overrides.hpp"
#include "ngx_hooks/feature_registry.hpp"
#include "ngx_hooks/hook_installer.hpp"
#include "nr/types.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// What the add-on supplies to the NGX observer; unit tests supply a fake. Every method is called on a
// game thread, after (or, for Overrides, before) the original NGX call, never across it.
class NgxRouting {
 public:
  virtual ~NgxRouting() = default;
  // The DLSS-SR overrides to apply to a SuperSampling create now.
  [[nodiscard]] virtual ngx_hooks::SrOverrides Overrides() = 0;
  // The ReShade device a game list was created on (Uplift's identity data), or null when untracked.
  [[nodiscard]] virtual const void* DeviceOf(ID3D12GraphicsCommandList* list) = 0;
  // The primary swap chain's size on `device`, for the main-handle choice; empty before its first present.
  [[nodiscard]] virtual nr::Size SwapchainSize(const void* device) = 0;
  // Whether `address` (a create's caller) lies in Uplift's module or in the NR runtime Uplift loaded.
  [[nodiscard]] virtual bool IsOwnCode(const void* address) = 0;
  // An evaluate of `device`'s main handle, after the original returned (v2 design §3.1).
  virtual void OnMainEvaluate(const void* device, ID3D12GraphicsCommandList* list, const ngx_hooks::DlssFrame& frame) = 0;
  // Before the original evaluate of `device`'s SuperSampling main handle (v2 design §3.9): records NR
  // before upscaling when that is the placement, and returns what DLSS must read as Color, or null.
  // The add-on's lock is released before it returns.
  virtual ID3D12Resource* BeforeMainEvaluate(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/,
                                             const ngx_hooks::DlssFrame& /*frame*/) {
    return nullptr;
  }
  // Minor 9 (Plan 4 fix round 4): BeforeMainEvaluate's colour could not be swapped into DLSS's Color
  // this evaluate (the game's own NGX block had none to swap it into). Same device/list, same call.
  virtual void ColorSwapRejected(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/) {}

  // Plan 13 (design §5): the Vulkan twins. `list` is a VkCommandBuffer (nr/vk_handles.hpp) and never reaches a Direct3D 12 call.
  // The device a command buffer was allocated on (its VkListState), else the ReShade device whose VkDevice is `vk_device`
  // (CreateFeature1's), or null. Never takes the add-on's lock for a tracked command buffer.
  [[nodiscard]] virtual const void* DeviceOfVk(ID3D12GraphicsCommandList* /*list*/, VkDevice /*vk_device*/) { return nullptr; }
  // An evaluate of `device`'s main handle on a Vulkan command buffer, after the original returned; `frame` points at `copies`.
  virtual void OnMainEvaluateVk(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/, const ngx_hooks::DlssFrame& /*frame*/,
                                const ngx_hooks::VkDlssResources& /*copies*/) {}
  // Before the original evaluate of `device`'s SuperSampling main handle on Vulkan: what DLSS must read as Color (an Uplift-owned
  // NVSDK_NGX_Resource_VK*, punned as nr::AsResource), or null. The add-on's lock is released before it returns.
  virtual ID3D12Resource* BeforeMainEvaluateVk(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/, const ngx_hooks::DlssFrame& /*frame*/,
                                               const ngx_hooks::VkDlssResources& /*copies*/) {
    return nullptr;
  }
  virtual void ColorSwapRejectedVk(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/) {}
  // Key decision c: the end of every hooked Vulkan evaluate of a feature the game created, on every path (a failed DLSS result and an
  // exception in Uplift's recording included): the event of the token this call opened on `list`, if one is still open, is set now.
  // Lock-free when no token is open.
  virtual void EndVulkanEvaluate(ID3D12GraphicsCommandList* /*list*/) {}
  // Batch 3 review I-1: the game is about to shut the NGX core down for `device` (NVSDK_NGX_VULKAN_Shutdown1, not Uplift's own call). Native NR on that
  // device makes its last NGX calls here; the add-on's lock is released before this returns, and so before the core's Shutdown1 runs.
  virtual void BeforeCoreShutdownVk(VkDevice /*device*/) {}
  // Plan 15: the Direct3D 12 twin (NVSDK_NGX_D3D12_Shutdown1, not Uplift's own call): `device` is the pointer the game passed, ReShade's proxy or the
  // device itself. NR on the game's device makes its last NGX calls here, and is held off until the game's DLSS is created there again.
  virtual void BeforeCoreShutdownD3D12(ID3D12Device* /*device*/) {}

  // Plan 18 (design §2): the Direct3D 11 twins. `list` is an ID3D11DeviceContext (nr/d3d11_handles.hpp) and never reaches a Direct3D 12 call.
  // The ReShade device Uplift marked on the device of the evaluate's context, or null: unmarked, or a deferred context (logged once). Lock-free.
  [[nodiscard]] virtual const void* DeviceOfD3D11(ID3D12GraphicsCommandList* /*list*/) { return nullptr; }
  // The ReShade device marked on `device` (Shutdown1's), or null. Lock-free.
  [[nodiscard]] virtual const void* DeviceOfD3D11Device(ID3D11Device* /*device*/) { return nullptr; }
  // After the original evaluate of `device`'s main handle on Direct3D 11: NR after DLSS through the bridge's mid-frame hand-off, or DLSS's vectors into the
  // Present ring. The add-on's lock is released before it returns.
  virtual void OnMainEvaluateD3D11(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/, const ngx_hooks::DlssFrame& /*frame*/) {}
  // Before the original evaluate of `device`'s SuperSampling main handle on Direct3D 11: NR before upscaling through the hand-off; what DLSS must read as
  // Color (the bridge's shared ID3D11Resource, punned as nr::AsResource), or null.
  virtual ID3D12Resource* BeforeMainEvaluateD3D11(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/, const ngx_hooks::DlssFrame& /*frame*/) {
    return nullptr;
  }
  virtual void ColorSwapRejectedD3D11(const void* /*device*/, ID3D12GraphicsCommandList* /*list*/) {}
  // Plan 18 (design §5): the process's first game DLSS evaluate, for the one-time INFO summary. Never takes the add-on's lock.
  virtual void LogFirstEvaluate(ngx_hooks::NgxApi /*api*/, ID3D12GraphicsCommandList* /*list*/, const ngx_hooks::DlssFrame& /*frame*/) {}
};

// The add-on's NgxObserver (v2 design §3.1, §3.5, §3.6, §3.19): the DLSS-SR overrides, the feature
// registry (frame generation and foreign NR included), and the main handle's evaluates. Thread-safe.
class NgxBridge final : public ngx_hooks::NgxObserver {
 public:
  explicit NgxBridge(NgxRouting& routing) : routing_(routing) {}

  void BeforeCreate(ngx_hooks::CreateCall* call) override;
  void AfterCreate(const ngx_hooks::CreateCall& call, NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result) override;
  void BeforeEvaluate(ngx_hooks::EvaluateCall* call) override;
  void OnColorSwapRejected(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle) override;
  void AfterEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                     const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Result result) override;
  void BeforeRelease(NVSDK_NGX_Handle* handle) override;
  void BeforeCoreShutdown(ngx_hooks::NgxApi api, void* device, const void* caller) override;

  [[nodiscard]] const ngx_hooks::FeatureRegistry& Registry() const { return registry_; }
  // The add-on's destroy_device: the registry forgets that an upscaler was created on `device`.
  void DeviceDestroyed(const void* device) { registry_.OnDeviceDestroyed(device); }
  // v2 design §3.9: on/off for Before upscaling (PreUpscale, Source ≠ Present, DLSS unavailable is
  // empty). One relaxed load per evaluate while off.
  void SetPreSr(bool on) { pre_sr_.store(on, std::memory_order_relaxed); }
  // Batch 3 review I-2: the add-on's live native Vulkan contexts (+1 when one is made, -1 when it goes with its device). While there are none, a
  // core shutdown is a pure passthrough: one relaxed load, then the core's own call.
  void AddVkContexts(int change) { vk_contexts_.fetch_add(change, std::memory_order_relaxed); }
  // Plan 13 final review, minor 1: the contexts whose hooks must watch the game's Vulkan DLSS evaluates. `watching` is true while Source is DLSS (the
  // context learns the game's DLSS only from them) or while the context wants or holds NR; `counted` is that context's own flag, and the count moves
  // with its changes. Once every context stops watching (Source back on Auto or Present, NR released), a Vulkan evaluate is a pure passthrough again:
  // one relaxed load, no lock, no lookup, no capture, nothing recorded. Safe: a token opens only while the Session is ACTIVE, and the Session cannot
  // reach OFF while a token is open (its event does not exist yet, so the teardown mark stays incomplete), so no close can be skipped by this count.
  void NoteVkContextNr(bool& counted, bool watching) {
    if (counted == watching) return;
    counted = watching;
    vk_nr_contexts_.fetch_add((watching ? 1 : -1), std::memory_order_relaxed);
  }
  // Plan 18 (Plan 13 I-2's rule on Direct3D 11): the add-on's Direct3D 11 bridge contexts that watch the game's DLSS evaluates (the context exists and NR is
  // on or not yet released). While none does, a Direct3D 11 evaluate is a pure passthrough: one relaxed load, no lookup, no lock. `counted` is the context's
  // own flag; the count moves with its changes.
  void NoteD3D11Watch(bool& counted, bool watching) {
    if (counted == watching) return;
    counted = watching;
    d3d11_watching_.fetch_add((watching ? 1 : -1), std::memory_order_relaxed);
  }
  // Plan 18 (design §5): a game DLSS evaluate came on a Direct3D 12 device Uplift does not run (a list ReShade does not track, or a device without a
  // context): in a Direct3D 11 game, a mod's own device. Sticky.
  void NoteForeignD3D12Dlss() { foreign_d3d12_.store(true, std::memory_order_relaxed); }
  [[nodiscard]] bool ForeignD3D12Dlss() const { return foreign_d3d12_.load(std::memory_order_relaxed); }

 private:
  // The device of a hooked call's list, through the API's own routing (a VkCommandBuffer never reaches a Direct3D 12 lookup, nor a Direct3D 11 context a
  // Direct3D 12 or Vulkan one).
  [[nodiscard]] const void* DeviceOf(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list) const;
  // Plan 18 (design §5): summarises the process's first game DLSS evaluate through the routing, once.
  void NoteFirstEvaluate(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter& parameters);
  // Plan 18 fix round 1 (I-1, design §2): a Direct3D 11 DLSS-SR feature the hooks did not see created (the game loaded NGX and created DLSS before the next
  // present, the only Direct3D 11 entry point where a newly loaded core is hooked; or it kept the feature across an NGX shutdown), registered at its first
  // hooked evaluate from that evaluate's block, on the device its context names (null: the unknown device). Adopted only with an upscaler's key set: Color,
  // Output, and the motion vectors or the depth (DeepDVC and DLISP have neither), and no albedo (Ray Reconstruction is not on Direct3D 11); the create
  // keys when the block still has them, else the render subrect's and the textures' sizes, low-res vectors, and (final review) IsHDR when the Output is a
  // float format. The record, or nullopt (refused, and not probed again until the handle is created, released or shut down).
  std::optional<ngx_hooks::FeatureRecord> AdoptD3D11Upscaler(const NVSDK_NGX_Handle* handle, ID3D12GraphicsCommandList* list,
                                                             const NVSDK_NGX_Parameter& parameters);

  NgxRouting& routing_;
  ngx_hooks::FeatureRegistry registry_;
  std::atomic<bool> pre_sr_{false};
  std::atomic<int> vk_contexts_{0};
  std::atomic<int> vk_nr_contexts_{0};
  std::atomic<int> d3d11_watching_{0};
  std::atomic<bool> foreign_d3d12_{false};
  std::atomic<bool> summary_logged_{false};
};

// Fix round 1, Important 2: the message DlssUnavailableReason returns for a latch read from the
// marker file at AddonInit, and the one the add-on sets live on `state.dlss_unavailable_reason` (and
// pushes to every context with SetDlssUnavailableReason) the moment a trip happens mid-session -- one
// string, so the two can never read differently for the same cause.
inline constexpr std::string_view LATCH_UNAVAILABLE_REASON =
    "The DLSS placements are latched off after a device removal: Clear latch, then restart the game";

// Why no DLSS placement can run this session, or empty. Decided once, at AddonInit: the tracking
// events are registered then or never (this plan's amendment 2).
[[nodiscard]] std::string DlssUnavailableReason(const ui::Settings& settings, const std::optional<ModuleVersion>& reshade,
                                                bool hooks_started, std::string_view hook_error);

}  // namespace uplift::addon
