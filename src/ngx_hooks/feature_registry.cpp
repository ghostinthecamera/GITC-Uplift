#include "ngx_hooks/feature_registry.hpp"

#include <algorithm>
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

void FeatureRegistry::OnCreate(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device, const CreateSnapshot& snapshot,
                               NgxApi api) {
  if (handle == nullptr) return;
  const std::unique_lock lock(mutex_);
  features_.insert_or_assign(handle,
                             FeatureRecord{.feature = feature, .device = device, .snapshot = snapshot, .serial = next_serial_++, .api = api});
  not_adopted_.erase(handle);  // Plan 18 fix round 1 (I-1): a reused handle
  if (device != nullptr && KindOf(feature) == FeatureKind::UPSCALER) {
    ++upscaler_creates_[device];
  }
}

bool FeatureRegistry::TryAdopt(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device, const CreateSnapshot& snapshot,
                               NgxApi api) {
  if (handle == nullptr) return false;
  const std::unique_lock lock(mutex_);
  // Insert-if-absent under the lock: a hooked create of the same handle on another thread keeps its own record.
  const bool inserted =
      features_.try_emplace(handle, FeatureRecord{.feature = feature, .device = device, .snapshot = snapshot, .serial = next_serial_, .api = api}).second;
  if (!inserted) return false;
  ++next_serial_;
  not_adopted_.erase(handle);
  if (device != nullptr && KindOf(feature) == FeatureKind::UPSCALER) {
    ++upscaler_creates_[device];
  }
  return true;
}

void FeatureRegistry::NoteNotAdopted(const NVSDK_NGX_Handle* handle) {
  if (handle == nullptr) return;
  const std::unique_lock lock(mutex_);
  if (!features_.contains(handle)) {
    not_adopted_.insert(handle);
  }
}

bool FeatureRegistry::NotAdopted(const NVSDK_NGX_Handle* handle) const {
  const std::shared_lock lock(mutex_);
  return not_adopted_.contains(handle);
}

void FeatureRegistry::OnRelease(const NVSDK_NGX_Handle* handle) {
  const std::unique_lock lock(mutex_);
  features_.erase(handle);
  not_adopted_.erase(handle);  // Plan 18 fix round 1 (I-1): the core may hand the handle out again
}

size_t FeatureRegistry::OnCoreShutdown(NgxApi api, const void* device) {
  const std::unique_lock lock(mutex_);
  not_adopted_.clear();  // Plan 18 fix round 1 (I-1): every handle the shutdown ended may come back
  return std::erase_if(features_, [api, device](const auto& entry) {
    const FeatureRecord& record = entry.second;
    return record.api == api && (device == nullptr || record.device == device || record.device == nullptr);
  });
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

bool FeatureRegistry::UpscalerLive(const void* device, NgxApi api) const {
  const std::shared_lock lock(mutex_);
  return std::ranges::any_of(features_, [device, api](const auto& entry) {
    const FeatureRecord& record = entry.second;
    return KindOf(record.feature) == FeatureKind::UPSCALER && record.api == api && (record.device == device || record.device == nullptr);
  });
}

const NVSDK_NGX_Handle* FeatureRegistry::MainHandle(const void* device, nr::Size swapchain, NgxApi api) const {
  const std::shared_lock lock(mutex_);
  const NVSDK_NGX_Handle* best = nullptr;
  bool best_matches = false;
  uint64_t best_area = 0u;
  uint64_t best_serial = 0u;
  for (const auto& [handle, record] : features_) {
    if (KindOf(record.feature) != FeatureKind::UPSCALER) continue;
    if (record.api != api) continue;  // Plan 18
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
