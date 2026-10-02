#include "nr/budget.hpp"

#include <algorithm>

namespace uplift::nr {

Budget::Budget(BudgetConfig config) : config_(config), bytes_per_megapixel_(config.bytes_per_megapixel) {}

uint64_t Budget::FeatureBytes(Size size) const {
  // 1 MP = 1,000,000 pixels; integer maths keeps the tests exact.
  return config_.feature_fixed_bytes + (size.Pixels() * bytes_per_megapixel_) / 1'000'000ull;
}

uint64_t Budget::Margin(const MemoryInfo& info) const {
  if (margin_override_) return *margin_override_;
  const auto fraction = static_cast<uint64_t>(static_cast<double>(info.budget) * config_.margin_fraction);
  return std::max(config_.min_margin_bytes, fraction);
}

uint64_t Budget::Available(const MemoryInfo& info) const {
  const uint64_t reserved = info.usage + Margin(info);
  return info.budget > reserved ? info.budget - reserved : 0u;
}

FitResult Budget::Fit(Size work, uint32_t requested_passes, uint64_t surface_bytes, const MemoryInfo& info,
                      uint64_t held_bytes, bool first_use_pending) const {
  const uint64_t available = Available(info) + held_bytes;
  const uint64_t fixed = surface_bytes + (first_use_pending ? config_.first_use_bytes : 0u);
  const uint64_t feature = FeatureBytes(work);
  for (uint32_t passes = requested_passes; passes >= 1u; --passes) {
    const uint64_t need = passes * feature + fixed;
    if (need <= available) return {passes, need, available};
  }
  return {0u, feature + fixed, available};
}

void Budget::Calibrate(Size size, uint64_t measured_feature_bytes) {
  if (size.Empty() || measured_feature_bytes <= config_.feature_fixed_bytes) return;
  const uint64_t per_megapixel = ((measured_feature_bytes - config_.feature_fixed_bytes) * 1'000'000ull) / size.Pixels();
  bytes_per_megapixel_ = std::clamp(per_megapixel, config_.min_bytes_per_megapixel, config_.max_bytes_per_megapixel);
}

YieldAction Budget::Sample(const MemoryInfo& info, uint32_t active_passes) {
  const uint64_t half_margin = Margin(info) / 2u;
  const uint64_t threshold = info.budget > half_margin ? info.budget - half_margin : 0u;
  over_samples_ = info.usage > threshold ? over_samples_ + 1u : 0u;
  if (over_samples_ < config_.yield_samples) return YieldAction::NONE;
  over_samples_ = 0u;
  return active_passes > 1u ? YieldAction::DROP_PASS : YieldAction::SUSPEND;
}

bool Budget::ResumeReady(const MemoryInfo& info, uint64_t need_bytes, std::chrono::steady_clock::time_point now) {
  if (Available(info) < need_bytes + Margin(info)) {
    resume_since_.reset();
    return false;
  }
  if (!resume_since_) {
    resume_since_ = now;
  }
  return now - *resume_since_ >= config_.resume_hold;
}

}  // namespace uplift::nr
