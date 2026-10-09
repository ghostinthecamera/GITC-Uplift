#include "nr/budget.hpp"

#include <algorithm>
#include <limits>

namespace uplift::nr {

Budget::Budget(BudgetConfig config) : config_(config), bytes_per_megapixel_(config.bytes_per_megapixel) {}

uint64_t Budget::FeatureBytes(Size size) const {
  // 1 MP = 1,000,000 pixels; integer maths keeps the tests exact.
  return config_.feature_fixed_bytes + (size.Pixels() * bytes_per_megapixel_) / 1'000'000ull;
}

uint64_t Budget::Margin() const {
  return margin_override_.value_or(config_.automatic_margin_bytes);
}

uint64_t Budget::CheckedBudget(const MemoryInfo& info) const {
  if (vram_check_ != VramCheck::RELAXED) return info.budget;
  // Saturating: RealHost reports an unlimited budget as UINT64_MAX.
  const uint64_t allowance = std::min(config_.relaxed_allowance_bytes, std::numeric_limits<uint64_t>::max() - info.budget);
  return info.budget + allowance;
}

uint64_t Budget::Available(const MemoryInfo& info) const {
  const uint64_t budget = CheckedBudget(info);
  const uint64_t reserved = info.usage + Margin();
  return budget > reserved ? budget - reserved : 0u;
}

FitResult Budget::Fit(Size work, uint32_t requested_passes, uint64_t surface_bytes, const MemoryInfo& info,
                      uint64_t held_bytes, bool first_use_pending) const {
  const uint64_t available = Available(info) + held_bytes;
  const uint64_t fixed = surface_bytes + (first_use_pending ? config_.first_use_bytes : 0u);
  const uint64_t feature = FeatureBytes(work);
  for (uint32_t passes = requested_passes; passes >= 1u; --passes) {
    const uint64_t need = passes * feature + fixed;
    // Off takes every requested pass; `available` stays what Careful would see, for the log and the card.
    if (need <= available || vram_check_ == VramCheck::OFF) return {passes, need, available};
  }
  return {0u, feature + fixed, available};
}

void Budget::Calibrate(Size size, uint64_t measured_feature_bytes) {
  if (size.Empty() || measured_feature_bytes <= config_.feature_fixed_bytes) return;
  const uint64_t per_megapixel = ((measured_feature_bytes - config_.feature_fixed_bytes) * 1'000'000ull) / size.Pixels();
  bytes_per_megapixel_ = std::clamp(per_megapixel, config_.min_bytes_per_megapixel, config_.max_bytes_per_megapixel);
}

YieldAction Budget::Sample(const MemoryInfo& info, uint32_t active_passes) {
  if (vram_check_ == VramCheck::OFF) {
    over_samples_ = 0u;  // a switch back to Careful starts a fresh streak
    return YieldAction::NONE;
  }
  const uint64_t half_margin = Margin() / 2u;
  const uint64_t budget = CheckedBudget(info);
  const uint64_t threshold = budget > half_margin ? budget - half_margin : 0u;
  over_samples_ = info.usage > threshold ? over_samples_ + 1u : 0u;
  if (over_samples_ < config_.yield_samples) return YieldAction::NONE;
  over_samples_ = 0u;
  return active_passes > 1u ? YieldAction::DROP_PASS : YieldAction::SUSPEND;
}

bool Budget::ResumeReady(const MemoryInfo& info, uint64_t need_bytes, std::chrono::steady_clock::time_point now) {
  if (vram_check_ == VramCheck::OFF) {
    resume_since_.reset();
    return true;
  }
  if (Available(info) < need_bytes + Margin()) {
    resume_since_.reset();
    return false;
  }
  if (!resume_since_) {
    resume_since_ = now;
  }
  return now - *resume_since_ >= config_.resume_hold;
}

}  // namespace uplift::nr
