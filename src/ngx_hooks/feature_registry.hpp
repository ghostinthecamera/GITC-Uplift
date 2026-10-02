#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

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
};

// The live NGX features, fed by the create and release hooks from any thread (v2 design §3.1).
class FeatureRegistry {
 public:
  void OnCreate(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device,
                const CreateSnapshot& snapshot);
  void OnRelease(const NVSDK_NGX_Handle* handle);
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
  // Plan 15 fix round: the serial of the newest feature created so far, on any device (0 before the first create). A feature whose serial is above the value
  // read at the game's NGX shutdown was created after it.
  [[nodiscard]] uint64_t LastSerial() const;
  // Spec §8.2 as amended: the upscaler on `device` (or of an unknown device) whose output region
  // best matches the swap chain: an aspect within 1 % first, then the largest output, then the
  // earliest create. With an empty `swapchain`, the largest output. Null when no upscaler is live.
  [[nodiscard]] const NVSDK_NGX_Handle* MainHandle(const void* device, nr::Size swapchain) const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<const NVSDK_NGX_Handle*, FeatureRecord> features_;
  std::unordered_map<const void*, uint64_t> upscaler_creates_;  // devices an upscaler was ever created on, with how many (Plan 15)
  uint64_t next_serial_ = 1u;
};

}  // namespace uplift::ngx_hooks
