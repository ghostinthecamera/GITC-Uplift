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
  const auto now = std::chrono::steady_clock::now();
  const std::unique_lock lock(mutex_);
  features_.insert_or_assign(handle, FeatureRecord{.feature = feature, .device = device, .snapshot = snapshot, .serial = next_serial_++, .api = api,
                                                   .last_active = now, .run_start = now});
  not_adopted_.erase(handle);  // a reused handle
  if (device != nullptr && KindOf(feature) == FeatureKind::UPSCALER) {
    ++upscaler_creates_[device];
  }
}

bool FeatureRegistry::TryAdopt(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device, const CreateSnapshot& snapshot,
                               NgxApi api) {
  if (handle == nullptr) return false;
  const auto now = std::chrono::steady_clock::now();
  const std::unique_lock lock(mutex_);
  // Insert-if-absent under the lock: a hooked create of the same handle on another thread keeps its own record.
  const bool inserted = features_
                            .try_emplace(handle, FeatureRecord{.feature = feature, .device = device, .snapshot = snapshot, .serial = next_serial_, .api = api,
                                                               .last_active = now, .run_start = now})
                            .second;
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
  not_adopted_.erase(handle);  // the core may hand the handle out again
}

void FeatureRegistry::OnEvaluate(const NVSDK_NGX_Handle* handle, std::chrono::steady_clock::time_point when) {
  const std::unique_lock lock(mutex_);
  const auto found = features_.find(handle);
  if (found == features_.end()) return;
  FeatureRecord& record = found->second;
  switch (KindOf(record.feature)) {
    case FeatureKind::UPSCALER: {
      if (when <= record.last_active) break;  // a racing thread's older stamp never moves the record back
      const std::chrono::steady_clock::duration gap = when - record.last_active;
      if (gap > DLSS_IDLE_MINIMUM) record.run_start = when;  // after a pause, a new run of evaluates
      record.previous_gap = record.gap;
      record.gap = gap;
      record.last_active = when;
      break;
    }
    case FeatureKind::NEURAL_RENDERING: last_nr_evaluate_ = when; break;
    case FeatureKind::FRAME_GENERATION:
    case FeatureKind::OTHER:            break;
  }
}

std::optional<std::chrono::steady_clock::time_point> FeatureRegistry::LastEvaluated(FeatureKind kind) const {
  if (kind != FeatureKind::NEURAL_RENDERING) return std::nullopt;
  const std::shared_lock lock(mutex_);
  return last_nr_evaluate_;
}

size_t FeatureRegistry::OnCoreShutdown(NgxApi api, const void* device) {
  const std::unique_lock lock(mutex_);
  not_adopted_.clear();  // every handle the shutdown ended may come back
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

const NVSDK_NGX_Handle* FeatureRegistry::MainHandle(const void* device, nr::Size swapchain, NgxApi api, const NVSDK_NGX_Handle* evaluating,
                                                    std::chrono::steady_clock::time_point now) const {
  const std::shared_lock lock(mutex_);
  using Entry = decltype(features_)::value_type;
  // The geometry ranking, among `api`'s upscalers on `device` (or of an unknown device) that `admit` accepts.
  const auto rank = [&](const auto& admit) -> const Entry* {
    const Entry* best = nullptr;
    bool best_matches = false;
    uint64_t best_area = 0u;
    uint64_t best_serial = 0u;
    for (const Entry& entry : features_) {
      const auto& [handle, record] = entry;
      if (KindOf(record.feature) != FeatureKind::UPSCALER) continue;
      if (record.api != api) continue;
      if (record.device != nullptr && record.device != device) continue;
      if (!admit(handle, record)) continue;
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
        best = &entry;
        best_matches = matches;
        best_area = area;
        best_serial = record.serial;
      }
    }
    return best;
  };
  // A feature's activity at `now`. The evaluating handle's own evaluate counts as already recorded, as OnEvaluate records it, so the answer before
  // an evaluate and the answer after it agree.
  const auto last_of = [&](const NVSDK_NGX_Handle* handle, const FeatureRecord& record) { return (handle == evaluating ? now : record.last_active); };
  const auto run_start_of = [&](const NVSDK_NGX_Handle* handle, const FeatureRecord& record) {
    return ((handle == evaluating && now - record.last_active > DLSS_IDLE_MINIMUM) ? now : record.run_start);
  };
  const Entry* const winner = rank([](const NVSDK_NGX_Handle*, const FeatureRecord&) { return true; });
  if (winner == nullptr) return nullptr;
  const Entry* chosen = winner;
  // A winner the game has stopped evaluating (paused, or abandoned without a release, as a DLSS mod may leave it) gives way to a feature of the
  // same view, one sharing a width or a height with the winner (a letterbox or a constrained-aspect change) or with the swap chain (a resolution
  // change whose old feature was not released), that the game has evaluated without a pause for longer than the pause interval. The winner
  // counts as stopped only once it has gone the pause interval past its own cadence (the shorter of its last two gaps), so a winner on a settled
  // slow rhythm (two gaps alike: once a second, or once a frame below four frames a second) keeps its place. What this cannot tell from
  // abandonment, and so may move main: the first two gaps of a slowdown; a winner evaluated in bursts (two evaluates close together, then a gap
  // over the pause interval), whose shorter gap reads as fast; and a frame over the pause interval in which a same-view feature is evaluated
  // twice. With nothing so steady, the winner stays, as it would before.
  const FeatureRecord& kept = winner->second;
  if (now - last_of(winner->first, kept) > DLSS_IDLE_MINIMUM + std::min(kept.gap, kept.previous_gap)) {
    const nr::Size view = winner->second.snapshot.output;
    const Entry* const steady = rank([&](const NVSDK_NGX_Handle* handle, const FeatureRecord& record) {
      const nr::Size output = record.snapshot.output;
      const bool same_view = (output.width == view.width || output.height == view.height
                              || (!swapchain.Empty() && (output.width == swapchain.width || output.height == swapchain.height)));
      return (!output.Empty() && same_view && now - last_of(handle, record) <= DLSS_IDLE_MINIMUM && now - run_start_of(handle, record) > DLSS_IDLE_MINIMUM);
    });
    if (steady != nullptr) chosen = steady;
  }
  const nr::Size chosen_output = chosen->second.snapshot.output;
  if ((api == NgxApi::D3D12 || api == NgxApi::VULKAN) && evaluating != nullptr && !chosen_output.Empty()) {
    const auto current = features_.find(evaluating);
    if (current != features_.end() && current->second.api == api && KindOf(current->second.feature) == FeatureKind::UPSCALER
        && (current->second.device == nullptr || current->second.device == device) && current->second.snapshot.output == chosen_output) {
      return evaluating;
    }
  }
  return chosen->first;
}

}  // namespace uplift::ngx_hooks
