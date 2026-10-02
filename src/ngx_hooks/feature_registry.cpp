#include "ngx_hooks/feature_registry.hpp"

#include <cmath>
#include <mutex>

#include "ngx_hooks/ngx_parameters.hpp"

namespace uplift::ngx_hooks {
namespace {

constexpr double ASPECT_TOLERANCE = 0.01;

}  // namespace

CreateSnapshot ReadCreateSnapshot(const NVSDK_NGX_Parameter& parameters) {
  return {
      .render = {ReadUint(parameters, NVSDK_NGX_Parameter_Width).value_or(0u),
                 ReadUint(parameters, NVSDK_NGX_Parameter_Height).value_or(0u)},
      .output = {ReadUint(parameters, NVSDK_NGX_Parameter_OutWidth).value_or(0u),
                 ReadUint(parameters, NVSDK_NGX_Parameter_OutHeight).value_or(0u)},
      .perf_quality = ReadInt(parameters, NVSDK_NGX_Parameter_PerfQualityValue).value_or(-1),
      .flags = ReadInt(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags).value_or(0),
  };
}

FeatureKind KindOf(NVSDK_NGX_Feature feature) {
  switch (feature) {
    case NVSDK_NGX_Feature_SuperSampling:
    case NVSDK_NGX_Feature_RayReconstruction: return FeatureKind::UPSCALER;
    case NVSDK_NGX_Feature_FrameGeneration:   return FeatureKind::FRAME_GENERATION;
    case NVSDK_NGX_Feature_Reserved18:        return FeatureKind::NEURAL_RENDERING;
    default:                                  return FeatureKind::OTHER;
  }
}

void FeatureRegistry::OnCreate(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device,
                               const CreateSnapshot& snapshot) {
  if (handle == nullptr) return;
  const std::unique_lock lock(mutex_);
  features_.insert_or_assign(handle, FeatureRecord{.feature = feature, .device = device, .snapshot = snapshot, .serial = next_serial_++});
  if (device != nullptr && KindOf(feature) == FeatureKind::UPSCALER) {
    ++upscaler_creates_[device];
  }
}

void FeatureRegistry::OnRelease(const NVSDK_NGX_Handle* handle) {
  const std::unique_lock lock(mutex_);
  features_.erase(handle);
}

void FeatureRegistry::OnDeviceDestroyed(const void* device) {
  const std::unique_lock lock(mutex_);
  upscaler_creates_.erase(device);
}

std::optional<FeatureRecord> FeatureRegistry::Find(const NVSDK_NGX_Handle* handle) const {
  const std::shared_lock lock(mutex_);
  const auto found = features_.find(handle);
  if (found == features_.end()) return std::nullopt;
  return found->second;
}

uint32_t FeatureRegistry::LiveCount(FeatureKind kind) const {
  const std::shared_lock lock(mutex_);
  uint32_t count = 0u;
  for (const auto& [handle, record] : features_) {
    if (KindOf(record.feature) == kind) {
      ++count;
    }
  }
  return count;
}

bool FeatureRegistry::UpscalerCreated(const void* device) const {
  return UpscalerCreates(device) > 0u;
}

uint64_t FeatureRegistry::LastSerial() const {
  const std::shared_lock lock(mutex_);
  return next_serial_ - 1u;
}

uint64_t FeatureRegistry::UpscalerCreates(const void* device) const {
  const std::shared_lock lock(mutex_);
  const auto found = upscaler_creates_.find(device);
  return (found == upscaler_creates_.end() ? 0u : found->second);
}

const NVSDK_NGX_Handle* FeatureRegistry::MainHandle(const void* device, nr::Size swapchain) const {
  const std::shared_lock lock(mutex_);
  const NVSDK_NGX_Handle* best = nullptr;
  bool best_matches = false;
  uint64_t best_area = 0u;
  uint64_t best_serial = 0u;
  for (const auto& [handle, record] : features_) {
    if (KindOf(record.feature) != FeatureKind::UPSCALER) continue;
    if (record.device != nullptr && record.device != device) continue;
    // Inlined from the brief's AspectMatches (controller ruling: a single-call-site helper is
    // inlined at its call site): whether this feature's output shares the swap chain's aspect
    // ratio within ASPECT_TOLERANCE.
    const nr::Size output = record.snapshot.output;
    bool matches = false;
    if (!output.Empty() && !swapchain.Empty()) {
      const double output_cross = static_cast<double>(output.width) * swapchain.height;
      const double swapchain_cross = static_cast<double>(swapchain.width) * output.height;
      matches = (std::abs(output_cross - swapchain_cross) <= ASPECT_TOLERANCE * swapchain_cross);
    }
    const uint64_t area = output.Pixels();
    const bool better = (best == nullptr || (matches && !best_matches)
                         || (matches == best_matches && (area > best_area || (area == best_area && record.serial < best_serial))));
    if (better) {
      best = handle;
      best_matches = matches;
      best_area = area;
      best_serial = record.serial;
    }
  }
  return best;
}

}  // namespace uplift::ngx_hooks
