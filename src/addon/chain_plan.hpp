#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "addon/placement_kind.hpp"
#include "nr/types.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// Split chain (2026-10-10, design 3.1): the three stages of the frame NR can run at, in chain order. A Session's stage index (nr::StageSlice::stage) is
// static_cast<uint8_t>(Stage).
enum class Stage : uint8_t {
  BEFORE_UPSCALING,
  AFTER_DLSS,
  PRESENT,
};
inline constexpr size_t STAGE_COUNT = 3u;

// The passes [first, first + count), 0-based, that run at `stage`.
struct StageGroup {
  Stage stage = Stage::PRESENT;
  uint32_t first = 0u;
  uint32_t count = 0u;
};

struct ChainPlan {
  std::array<StageGroup, STAGE_COUNT> groups = {};  // in stage order; groups[0] holds pass 1
  uint32_t group_count = 0u;                         // 0: no NR (pass 1 has no placement)
  std::array<Stage, ui::MAX_PASSES> effective = {};  // where each pass runs; a pass past the pass count reads the last pass's stage
  std::string note;  // the card's one line when a stored pick does not run as stored ("" otherwise)
  [[nodiscard]] bool Split() const { return group_count > 1u; }
};

struct ChainFacts {
  Placement pass1 = Placement::NONE;  // ChoosePlacement's result, unchanged (NONE: no NR, no plan)
  uint32_t passes = 1u;               // min(PassCount, MAX_PASSES)
  std::array<ui::PassStage, ui::MAX_PASSES - 1u> stored = {};  // Pass{n}Stage for passes 2..10
  bool split_capable = false;  // the route can run more than one stage (ruling 6)
  // Per stage (Stage order), why it cannot run now; empty: it can. A DLSS pause is a wait, never a cause here (design Q4).
  std::array<std::string_view, STAGE_COUNT> cannot = {};
};

// Split chain (design 3.4, Task 4): whether a device's route runs passes at more than one stage (ChainFacts::split_capable): native Direct3D 12, and a
// Direct3D 11 device's bridge, whose DLSS stages run through its mid-frame hand-off (`d3d11_dlss_stages`, ContextOptions::dlss_stages). Task 5: and a
// Vulkan device's native context while VulkanNr is Native and native NR may run inside the game's DLSS (`vulkan_native`; Present that fell back off Native
// then cannot run, decision Q6). Every other bridged route (Direct3D 9 and 10, OpenGL, Vulkan through a bridge or the helper) runs one stage.
[[nodiscard]] constexpr bool SplitCapable(bool bridged, bool d3d11_dlss_stages, bool vulkan_native = false) {
  return !bridged || d3d11_dlss_stages || vulkan_native;
}

// Where each pass runs (design 3.1, decisions Q1, Q4). Pass 1 runs at `facts.pass1`. Pass n's wanted stage is its stored pick, or the stage of the pass
// before it for SAME, raised to the pass before's stage when earlier (never backwards). A wanted stage later than the pass before's that cannot run moves
// later first, then earlier, never before the pass before's (which always takes it). Not split-capable: every pass at pass 1's stage. Contiguous equal
// stages form one group. Pure.
[[nodiscard]] ChainPlan DecideChain(const ChainFacts& facts);

// A split chain's groups in words (design 6.2), "passes 1-2 before upscaling (960x540), 3 after DLSS (1920x1080), 4 at Present (1920x1080)", each with its
// stage's canvas from `canvas` (Stage order; an empty size is left out). Empty for a chain of one group. Details prefixes "Chain: ", the log "NR chain: ".
[[nodiscard]] std::string FormatChainLine(const ChainPlan& plan, const std::array<nr::Size, STAGE_COUNT>& canvas);

}  // namespace uplift::addon
