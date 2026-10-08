#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "nr/types.hpp"

namespace uplift::nr {

struct MemoryInfo {
  uint64_t budget = 0u;  // DXGI local-segment Budget
  uint64_t usage = 0u;   // DXGI local-segment CurrentUsage (whole process)
};

// Defaults measured on the RTX 4090 with DLSS-NR 310.8 (spikes E6/E10).
struct BudgetConfig {
  uint64_t feature_fixed_bytes = 163ull << 20u;       // per-feature intercept
  uint64_t bytes_per_megapixel = 101ull << 20u;       // 93.4 MiB/MP activations + 8 B/px capacity buffer
  uint64_t min_bytes_per_megapixel = 60ull << 20u;
  uint64_t max_bytes_per_megapixel = 240ull << 20u;
  uint64_t first_use_bytes = 165ull << 20u;           // one-time per device, never returned
  uint64_t stats_overcount_bytes = 147'719'680ull;    // weight heap the stats count per feature
  uint64_t min_margin_bytes = 512ull << 20u;
  double margin_fraction = 0.05;
  uint32_t yield_samples = 2u;
  std::chrono::seconds resume_hold{10};
};

struct FitResult {
  uint32_t passes = 0u;  // 0 = skip NR at this size
  uint64_t need_bytes = 0u;
  uint64_t available_bytes = 0u;
};

enum class YieldAction : uint8_t {
  NONE,
  DROP_PASS,
  SUSPEND,
};

class Budget {
 public:
  explicit Budget(BudgetConfig config = {});

  [[nodiscard]] uint64_t FeatureBytes(Size size) const;
  [[nodiscard]] uint64_t Margin(const MemoryInfo& info) const;
  // Spec §12 `BudgetMarginMB`: a fixed margin instead of max(512 MiB, 5% of budget); nullopt restores that.
  void SetMarginOverride(std::optional<uint64_t> margin_bytes) { margin_override_ = margin_bytes; }
  // v2 design §3.21: need = passes × FeatureBytes(work) + `surface_bytes` (every surface the caller
  // holds for this chain, each at its own size) + the first-use cost while it is pending.
  // `held_bytes`: allocations that stay part of the plan and are already inside info.usage (live
  // features being kept, the caller's surfaces).
  [[nodiscard]] FitResult Fit(Size work, uint32_t requested_passes, uint64_t surface_bytes, const MemoryInfo& info,
                              uint64_t held_bytes, bool first_use_pending) const;
  // `measured_feature_bytes`: the real (DXGI-equivalent) cost of one new
  // feature, e.g. its stats delta minus StatsOvercountBytes() plus the 8 B/px
  // capacity buffer that create-time stats omit (spike E6).
  void Calibrate(Size size, uint64_t measured_feature_bytes);
  [[nodiscard]] uint64_t StatsOvercountBytes() const { return config_.stats_overcount_bytes; }
  [[nodiscard]] uint64_t FirstUseBytes() const { return config_.first_use_bytes; }  // what Fit charges while the first use is pending
  // Call at 1 Hz while NR is active.
  [[nodiscard]] YieldAction Sample(const MemoryInfo& info, uint32_t active_passes);
  // Clears the yield streak and the resume hold. Session::Load calls this on
  // every (re)activation, so each suspension waits a fresh resume_hold.
  void ResetYield() {
    over_samples_ = 0u;
    resume_since_.reset();
  }
  [[nodiscard]] bool ResumeReady(const MemoryInfo& info, uint64_t need_bytes,
                                 std::chrono::steady_clock::time_point now);
  [[nodiscard]] uint64_t BytesPerMegapixel() const { return bytes_per_megapixel_; }

 private:
  [[nodiscard]] uint64_t Available(const MemoryInfo& info) const;

  BudgetConfig config_;
  uint64_t bytes_per_megapixel_;
  uint32_t over_samples_ = 0u;
  std::optional<std::chrono::steady_clock::time_point> resume_since_;
  std::optional<uint64_t> margin_override_;
};

}  // namespace uplift::nr
