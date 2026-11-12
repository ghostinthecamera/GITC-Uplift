#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>

#include "addon/chain_plan.hpp"
#include "nr/skip_reason.hpp"
#include "nr/types.hpp"
#include "sources/motion_source.hpp"
#include "sources/work_size.hpp"

namespace uplift::addon {

// The Details line of a split chain: the stage groups and their canvases.
inline constexpr char CHAIN_LINE[] = "Chain: {}";

// What a stage group's latest recording left.
struct StageRecord {
  sources::WorkSizeSettle settle;  // the stage's settled work size
  nr::Size work;                   // the work size the recording ran at
  nr::Size canvas;                 // the canvas the recording ran at
  bool nr_applied = false;         // whether the recording applied NR
  uint32_t passes_run = 0u;        // the passes the recording ran
  nr::SkipReason skip_reason = nr::SkipReason::NONE;                    // why the recording skipped
  sources::MotionSource motion_source = sources::MotionSource::NONE;  // the motion the recording bound
};

// The Extra of a book whose stages keep nothing beyond the record.
struct NoStageExtra {};

// A stage's record and the Extra a context keeps beside it.
template <typename Extra = NoStageExtra>
struct StageEntry : StageRecord, Extra {};

// The stage records of a split chain, in Stage order, and the two things both contexts' Status() reads off them.
template <typename Extra = NoStageExtra>
class StageBook {
 public:
  [[nodiscard]] StageEntry<Extra>& operator[](size_t index) { return entries_[index]; }
  [[nodiscard]] const StageEntry<Extra>& operator[](size_t index) const { return entries_[index]; }
  [[nodiscard]] auto begin() { return entries_.begin(); }
  [[nodiscard]] auto end() { return entries_.end(); }
  [[nodiscard]] auto begin() const { return entries_.begin(); }
  [[nodiscard]] auto end() const { return entries_.end(); }

  // Split chain: every group's passes count, not only pass 1's group. A group whose latest recording skipped counts none, and a group's latest
  // recording counts at most its own passes (it may predate a chain change in this frame).
  [[nodiscard]] uint32_t SplitPassesRun(const ChainPlan& chain) const {
    uint32_t passes_run = 0u;
    for (uint32_t index = 0u; index < chain.group_count; ++index) {
      const StageGroup& group = chain.groups[index];
      const StageEntry<Extra>& stage = entries_[static_cast<size_t>(group.stage)];
      passes_run += (stage.nr_applied ? std::min(stage.passes_run, group.count) : 0u);
    }
    return passes_run;
  }

  // Split chain: the chain line and note, and each group's latest recording. A template on `Status` only because ContextStatus lives in
  // device_context.hpp, which includes this header.
  template <typename Status>
  void WriteChain(const ChainPlan& chain, Status& status) const {
    std::array<nr::Size, STAGE_COUNT> canvases = {};
    for (size_t stage = 0u; stage < STAGE_COUNT; ++stage) {
      canvases[stage] = entries_[stage].canvas;
    }
    if (chain.Split()) {
      status.chain_line = std::format(CHAIN_LINE, FormatChainLine(chain, canvases));
    }
    status.chain_note = chain.note;
    status.group_count = chain.group_count;
    for (uint32_t index = 0u; index < chain.group_count; ++index) {
      const StageGroup& group = chain.groups[index];
      const StageEntry<Extra>& stage = entries_[static_cast<size_t>(group.stage)];
      status.groups[index] = {.stage = group.stage, .first = group.first, .count = group.count, .nr_applied = stage.nr_applied,
                              .passes_run = stage.passes_run, .skip_reason = stage.skip_reason, .motion_source = stage.motion_source,
                              .work = stage.work, .canvas = stage.canvas};
    }
  }

 private:
  std::array<StageEntry<Extra>, STAGE_COUNT> entries_;
};

}  // namespace uplift::addon
