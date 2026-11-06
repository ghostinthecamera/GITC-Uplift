#include "addon/e2e_harness.hpp"

#include <Windows.h>

#include <algorithm>
#include <format>

#include "nr/log.hpp"
#include "ui/settings_schema.hpp"

namespace uplift::addon {
namespace {

constexpr wchar_t LIVE_SETTINGS_ENV[] = L"UPLIFT_E2E_LIVE_SETTINGS";
constexpr DWORD ENV_BUFFER_CHARS = 8u;
constexpr DWORD ENABLED_TEXT_LENGTH = 1u;
constexpr wchar_t ENABLED_TEXT = L'1';
constexpr int64_t NO_FIRST_APPLY = -1;
constexpr std::string_view E2E_LIVE_KEYS[] = {"VulkanNr", E2E_SOURCE_KEY, E2E_PRE_UPSCALE_KEY, "MotionVectors", "KeepFaces", "ShowFaceMask", "PassCount", "Pass2Stage", "Pass3Stage"};

}  // namespace

void E2eHarness::Initialize() {
  wchar_t live[ENV_BUFFER_CHARS] = {};
  enabled = (GetEnvironmentVariableW(LIVE_SETTINGS_ENV, live, ENV_BUFFER_CHARS) == ENABLED_TEXT_LENGTH && live[0] == ENABLED_TEXT);
}

ui::Settings E2eHarness::Poll(const ui::Settings& settings, const ui::ConfigStore& config,
    const DeviceContext* context, const VkDlssContext* native, const std::optional<HelperFront>& front, reshade::api::device* device,
    std::chrono::steady_clock::time_point now) {
  if (!held_) {
    held_ = settings;  // the start-up state, held until the host's first change
    hold_start_ = now;
  }
  const bool native_view = (native && (native->WantsNr() || native->NrState() != nr::SessionState::OFF));
  bool applied = false;
  std::string skip;
  if (native_view || context) {
    const ContextStatus status = (native_view ? native->Status() : context->Status());
    applied = status.nr_applied;
    skip = (status.message.empty() ? std::string(nr::SessionStateName(status.session.state)) : status.message);
  } else {
    applied = front->NrApplied(device);
    skip = "the helper's NR has not applied";
  }
  if (applied && !first_applied_) {
    first_applied_ = presents_;
    first_applied_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(now - hold_start_).count();
  }
  ++presents_;
  if (applied) {
    ++applied_presents_;
  } else {
    ++skips_[skip];
  }
  ui::Settings next = settings;
  for (const auto key : E2E_LIVE_KEYS) {
    const auto text = config.Get(key);
    if (!text) continue;
    if (const auto* row = ui::FindSetting(key)) {
      ui::ApplySettingText(next, *row, *text);
    }
  }
  return next;
}

void E2eHarness::FinishHold(const ui::Settings& next, const VkDlssContext* native, std::chrono::steady_clock::time_point now) {
  // The state just held, its presents, the first that applied NR (and when), the presents with NR from there on, and why the others went without it.
  const ui::Settings& held = *held_;
  const uint64_t first = first_applied_.value_or(presents_);
  std::string skips;
  for (const auto& [reason, count] : skips_) {
    skips += std::format("{}{} x{}", (skips.empty() ? "" : "; "), reason, count);
  }
  // A native split names each stage group and its recordings with NR applied during this hold.
  std::string groups;
  if (native) {
    const std::array<uint64_t, STAGE_COUNT> recordings = native->StageRecordings();
    const ContextStatus native_status = native->Status();
    for (uint32_t group = 0u; native_status.group_count > 1u && group < native_status.group_count; ++group) {
      const ContextStatus::Group& stage_group = native_status.groups[group];
      const auto stage = static_cast<size_t>(stage_group.stage);
      const std::string_view name = (stage_group.stage == Stage::BEFORE_UPSCALING ? "before upscaling"
                                     : stage_group.stage == Stage::AFTER_DLSS    ? "after DLSS"
                                                                                        : "at Present");
      groups += std::format("{}{} {}-{} x{}", (groups.empty() ? " | groups: " : ", "), name, stage_group.first + 1u, stage_group.first + stage_group.count,
                            recordings[stage] - std::min(recordings[stage], stage_recordings_[stage]));
    }
    stage_recordings_ = recordings;
  }
  nr::Logf(nr::LogLevel::INFO,
           "e2e: held VulkanNr={} Source={} PreUpscale={} MotionVectors={} KeepFaces={} ShowFaceMask={} PassCount={} Pass2Stage={} Pass3Stage={}: {} of {} "
           "presents with NR applied, the first after {} presents ({} ms), {} of {} from there on; without NR: {}{}",
           static_cast<uint32_t>(held.vulkan_nr), static_cast<uint32_t>(held.source), (held.pre_upscale ? 1 : 0), static_cast<uint32_t>(held.motion_vectors),
           (held.keep_faces ? 1 : 0), (held.show_face_mask ? 1 : 0), held.pass_count, static_cast<uint32_t>(held.passes[0].stage),
           static_cast<uint32_t>(held.passes[1].stage), applied_presents_, presents_, first, first_applied_ms_.value_or(NO_FIRST_APPLY),
           applied_presents_, presents_ - first, (skips.empty() ? std::string("none") : skips), groups);
  applied_presents_ = 0u;
  presents_ = 0u;
  first_applied_.reset();
  first_applied_ms_.reset();
  skips_.clear();
  hold_start_ = now;
  held_ = next;
}

}  // namespace uplift::addon
