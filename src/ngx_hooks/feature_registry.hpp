#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "ngx_hooks/ngx_api.hpp"
#include "nr/ngx.hpp"
#include "nr/types.hpp"

namespace uplift::ngx_hooks {

// The create-time keys of a DLSS feature (v2 design §3.1).
struct CreateSnapshot {
  nr::Size render;        // Width × Height: the largest render size
  nr::Size output;        // OutWidth × OutHeight: the output region's size
  int perf_quality = -1;  // PerfQualityValue; -1 when not set
  int flags = 0;          // DLSS.Feature.Create.Flags

  [[nodiscard]] bool IsHdr() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0; }
  [[nodiscard]] bool MvLowRes() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0; }
  [[nodiscard]] bool MvJittered() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0; }
  [[nodiscard]] bool DepthInverted() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0; }
  [[nodiscard]] bool AutoExposure() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0; }
};

[[nodiscard]] CreateSnapshot ReadCreateSnapshot(const NVSDK_NGX_Parameter& parameters);

enum class FeatureKind : uint8_t {
  OTHER,
  UPSCALER,          // SuperSampling (1) or RayReconstruction (13): the After-DLSS input
  FRAME_GENERATION,  // FrameGeneration (11)
  NEURAL_RENDERING,  // Reserved18: another NR producer, since Uplift never creates NR through the core
};

[[nodiscard]] FeatureKind KindOf(NVSDK_NGX_Feature feature);

struct FeatureRecord {
  NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_Reserved_Unknown;
  const void* device = nullptr;  // opaque key of the device the feature was created on; null = unknown
  CreateSnapshot snapshot;
  uint64_t serial = 0u;  // creation order, for stable ties
  NgxApi api = NgxApi::D3D12;  // Plan 18: the API the feature was created through
};

// The live NGX features, fed by the create and release hooks from any thread (v2 design §3.1).
class FeatureRegistry {
 public:
  void OnCreate(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device, const CreateSnapshot& snapshot,
                NgxApi api = NgxApi::D3D12);
  // Plan 18 fix round 1 (I-1): a feature the game created before Uplift's hooks were in place, adopted at its first hooked evaluate: registered as OnCreate
  // registers it, unless `handle` already is (a hooked create on another thread won the race, and its record is the right one). True when this call
  // registered it.
  bool TryAdopt(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device, const CreateSnapshot& snapshot, NgxApi api);
  // Plan 18 fix round 1 (I-1): the unknown handles whose evaluate was looked at and not adopted, so they are not probed again every frame. A create, a
  // release or a core shutdown forgets them: the core reuses handles.
  void NoteNotAdopted(const NVSDK_NGX_Handle* handle);
  [[nodiscard]] bool NotAdopted(const NVSDK_NGX_Handle* handle) const;
  void OnRelease(const NVSDK_NGX_Handle* handle);
  // Plan 18 (design §6): the game shut NGX down for `api`: drops `api`'s features of `device` and of the unknown device (null); a null `device` drops every
  // feature of `api`. Another API's features are never touched, and the sticky create counts stay (as after a release). Returns how many went.
  size_t OnCoreShutdown(NgxApi api, const void* device);
  // The device is gone: UpscalerCreated forgets it, so a device recreated at the same address does not start as "DLSS seen" (batch 1 review, minor 5).
  void OnDeviceDestroyed(const void* device);
  [[nodiscard]] std::optional<FeatureRecord> Find(const NVSDK_NGX_Handle* handle) const;
  [[nodiscard]] uint32_t LiveCount(FeatureKind kind) const;
  // Plan 14 (design §1.2): the game created a DLSS-SR or Ray Reconstruction feature on `device` at some point this session. Sticky: a release does not clear
  // it. Fed by every create, so the Vulkan passthrough (no watching contexts) learns it too.
  [[nodiscard]] bool UpscalerCreated(const void* device) const;
  // Plan 15: how many DLSS-SR or Ray Reconstruction features the game has created on `device` this session (0 for none, or once the device went). A
  // create after the game's own NGX shutdown proves it re-initialised NGX there (a DLSS setting changed), which ends Uplift's hold on the device.
  [[nodiscard]] uint64_t UpscalerCreates(const void* device) const;
  // Plan 18 Task 12: the game's DLSS is live on `device`: a DLSS-SR or Ray Reconstruction feature of `api` is created and not yet released there, or on an
  // unknown device (MainHandle's rule: it may be this device's main handle). A pause (a live feature the game does not evaluate) is live; the release of the
  // last one is not, nor a Direct3D 11 NGX shutdown (OnCoreShutdown), the only shutdown that reaches the registry. An unknown device's upscaler that is
  // never released keeps every device of its API live (fail-safe: today's wait).
  [[nodiscard]] bool UpscalerLive(const void* device, NgxApi api) const;
  // Plan 15 fix round: the serial of the newest feature created so far, on any device (0 before the first create). A feature whose serial is above the value
  // read at the game's NGX shutdown was created after it.
  [[nodiscard]] uint64_t LastSerial() const;
  // Spec §8.2 as amended: the upscaler on `device` (or of an unknown device) whose output region
  // best matches the swap chain: an aspect within 1 % first, then the largest output, then the
  // earliest create. With an empty `swapchain`, the largest output. Null when no upscaler is live. Plan 18: only `api`'s features compete (an unknown device's
  // Direct3D 12 feature, a mod's, is never a Direct3D 11 device's main handle).
  [[nodiscard]] const NVSDK_NGX_Handle* MainHandle(const void* device, nr::Size swapchain, NgxApi api = NgxApi::D3D12) const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<const NVSDK_NGX_Handle*, FeatureRecord> features_;
  std::unordered_map<const void*, uint64_t> upscaler_creates_;  // devices an upscaler was ever created on, with how many (Plan 15)
  std::unordered_set<const NVSDK_NGX_Handle*> not_adopted_;     // Plan 18 fix round 1 (I-1)
  uint64_t next_serial_ = 1u;
};

}  // namespace uplift::ngx_hooks
