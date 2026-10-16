#include "addon/chain_plan.hpp"

#include <algorithm>
#include <format>

namespace uplift::addon {

ChainPlan DecideChain(const ChainFacts& facts) {
  ChainPlan plan;
  if (facts.pass1 == Placement::NONE) return plan;
  const Stage pass1_stage = [&facts] {
    switch (facts.pass1) {
      case Placement::BEFORE_UPSCALING: return Stage::BEFORE_UPSCALING;
      case Placement::AFTER_DLSS:       return Stage::AFTER_DLSS;
      case Placement::PRESENT:
      case Placement::NONE:             break;
    }
    return Stage::PRESENT;
  }();
  const auto stage_name = [](Stage stage) -> std::string_view {
    switch (stage) {
      case Stage::BEFORE_UPSCALING: return "Before upscaling";
      case Stage::AFTER_DLSS:       return "After DLSS";
      case Stage::PRESENT:          break;
    }
    return "Present";
  };
  const auto can_run = [&facts](Stage stage) { return facts.cannot[static_cast<size_t>(stage)].empty(); };
  // Only the first pass that does not run where it is stored is named: the passes after it follow from it.
  const auto note = [&plan, &stage_name](uint32_t index, std::string_view why) {
    if (plan.note.empty()) {
      plan.note = std::format("Pass {} runs at {}: {}", index + 1u, stage_name(plan.effective[index]), why);
    }
  };

  const uint32_t passes = std::clamp(facts.passes, 1u, ui::MAX_PASSES);
  plan.effective.fill(pass1_stage);
  for (uint32_t index = 1u; index < passes; ++index) {
    const Stage before = plan.effective[index - 1u];
    const ui::PassStage stored = facts.stored[index - 1u];
    plan.effective[index] = before;
    if (stored == ui::PassStage::SAME) continue;
    const auto picked = static_cast<Stage>(static_cast<uint32_t>(stored) - 1u);
    if (!facts.split_capable) {
      if (picked != before) {
        note(index, "all passes run at one stage here");
      }
      continue;
    }
    if (picked < before) {
      note(index, std::format("it cannot run before pass {}", index));
      continue;
    }
    // The pass before's stage always takes it: that pass runs there.
    if (picked == before || can_run(picked)) {
      plan.effective[index] = picked;
      continue;
    }
    // Ruling 3: the nearest stage that can run, later first, then earlier, never before the pass before's.
    bool found = false;
    for (auto stage = static_cast<uint32_t>(picked) + 1u; !found && stage < STAGE_COUNT; ++stage) {
      if (can_run(static_cast<Stage>(stage))) {
        plan.effective[index] = static_cast<Stage>(stage);
        found = true;
      }
    }
    for (auto stage = static_cast<uint32_t>(picked) - 1u; !found && stage > static_cast<uint32_t>(before); --stage) {
      if (can_run(static_cast<Stage>(stage))) {
        plan.effective[index] = static_cast<Stage>(stage);
        found = true;
      }
    }
    note(index, facts.cannot[static_cast<size_t>(picked)]);
  }
  std::fill(plan.effective.begin() + passes, plan.effective.end(), plan.effective[passes - 1u]);

  for (uint32_t index = 0u; index < passes; ++index) {
    if (plan.group_count > 0u && plan.groups[plan.group_count - 1u].stage == plan.effective[index]) {
      ++plan.groups[plan.group_count - 1u].count;
    } else {
      plan.groups[plan.group_count] = {.stage = plan.effective[index], .first = index, .count = 1u};
      ++plan.group_count;
    }
  }
  return plan;
}

std::string FormatChainLine(const ChainPlan& plan, const std::array<nr::Size, STAGE_COUNT>& canvas) {
  if (!plan.Split()) return {};
  std::string line;
  for (uint32_t index = 0u; index < plan.group_count; ++index) {
    const StageGroup& group = plan.groups[index];
    const std::string_view where = [&group] {
      switch (group.stage) {
        case Stage::BEFORE_UPSCALING: return "before upscaling";
        case Stage::AFTER_DLSS:       return "after DLSS";
        case Stage::PRESENT:          break;
      }
      return "at Present";
    }();
    const std::string passes = (group.count == 1u ? std::format("{}", group.first + 1u) : std::format("{}-{}", group.first + 1u, group.first + group.count));
    const std::string_view noun = (index > 0u ? "" : (group.count == 1u ? "pass " : "passes "));
    const nr::Size size = canvas[static_cast<size_t>(group.stage)];
    const std::string at_size = (size.Empty() ? std::string() : std::format(" ({}x{})", size.width, size.height));
    line += std::format("{}{}{} {}{}", (index > 0u ? ", " : ""), noun, passes, where, at_size);
  }
  return line;
}

}  // namespace uplift::addon
