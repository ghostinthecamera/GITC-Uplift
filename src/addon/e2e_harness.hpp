#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "addon/device_context.hpp"
#include "addon/helper_front.hpp"
#include "addon/vk_dlss_context.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

inline constexpr std::string_view E2E_SOURCE_KEY = "Source";
inline constexpr std::string_view E2E_PRE_UPSCALE_KEY = "PreUpscale";

// Only enabled by the synthetic host. Counts the previous frame before reading its next settings.
struct E2eHarness {
  bool enabled = false;
  void Initialize();
  [[nodiscard]] ui::Settings Poll(const ui::Settings& settings, const ui::ConfigStore& config,
      const DeviceContext* context, const VkDlssContext* native, const std::optional<HelperFront>& front, reshade::api::device* device,
      std::chrono::steady_clock::time_point now);
  // Logs and resets the old hold; the caller then publishes the new settings through its normal path.
  void FinishHold(const ui::Settings& next, const VkDlssContext* native, std::chrono::steady_clock::time_point now);

 private:
  uint64_t applied_presents_ = 0u;
  uint64_t presents_ = 0u;
  std::optional<ui::Settings> held_;  // the state being held (its keys) since `hold_start_`
  std::chrono::steady_clock::time_point hold_start_;
  std::optional<uint64_t> first_applied_;     // the hold's first present with NR applied (its index)
  std::optional<int64_t> first_applied_ms_;  // and when, since the hold began
  std::map<std::string, uint64_t> skips_;     // why the hold's other presents went without NR
  // Per-stage recording totals at the start of the current hold.
  std::array<uint64_t, STAGE_COUNT> stage_recordings_ = {};
};

}  // namespace uplift::addon
