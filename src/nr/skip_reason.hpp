#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uplift::nr {

// Why a recording did not apply NR; NONE: nothing to explain (NR applied, or a skip that says nothing). Produced by the Session, the pipelines, the
// sources, the bridges and the contexts, compared as a value, and turned into its text only where a person or the log reads it: the status card, the
// stage rows, the Details line ("NR skipped: {}"), Before FG's note, the log, and the helper's status, which crosses the wire as text. Each text is a
// catalogue key (src/ui/localization.cpp), so its bytes never change.
enum class SkipReason : uint8_t {
  NONE,
  // Session::Evaluate's: its state while not ACTIVE, then its own checks.
  SESSION_OFF,
  SESSION_LOADING,
  SESSION_GRACE,
  SESSION_DRAINING,
  SESSION_FAILED,
  INVALID_INPUT,
  RESIZING,         // a rebuild waits for the feature pool to empty
  BUDGET,           // the budget cut a pass, or fitted none
  FRAME_TOO_SMALL,  // below the NR floor
  CREATE_FAILED,    // NGX could not create a feature
  EVALUATE_FAILED,  // a feature's evaluate failed
  // The pipelines'.
  INACTIVE,
  UNSUPPORTED_FORMAT,
  NO_TARGET_PIPELINE,
  ALREADY_RAN,
  DESCRIPTORS_BUSY,
  OUT_OF_MEMORY,
  ENCODE_FAILED,
  DECODE_FAILED,
  INTENSITY_ZERO,
  PIPELINE_FAILED,
  MOTION_OUTSIDE_TEXTURE,
  // The DLSS sources'.
  LIST_STATE_UNKNOWN,
  COMPUTE_QUEUE,
  NOT_SR_OR_RR,
  // The contexts'.
  DEVICE_REMOVED,
  NOT_AFTER_DLSS,
  NR_OFF,
  NO_COLOR_SWAP,  // the NGX detour could not swap Before upscaling's result in as DLSS's Color
  FRAME_GENERATION_ON,
  WAITING_EARLIER_STAGES,
  PRESENT_QUEUE_BUSY,
  PRESENT_QUEUE_FAILED,
  COMMAND_BUFFERS_BUSY,
  COMMAND_BUFFER_BEGIN_FAILED,
  COMMAND_BUFFER_SUBMIT_FAILED,
  MOTION_CHANGED,
  MOTION_RELOADS,
  NGX_SHUT_DOWN,
  DEVICE_LOST,
  NO_READABLE_MOTION,
  PREPARING_FORMAT,
  // The bridges' DLSS hand-off.
  BRIDGE_STOPPED,
  BRIDGE_BUSY,
  DLSS_REGION_OUTSIDE,
  BRIDGE_SHARE_FAILED,
  PRIVATE_LIST_NOT_SUBMITTED,
  NR_RESULT_LOST,
};

// The number of SkipReason values.
inline constexpr size_t SKIP_REASON_COUNT = static_cast<size_t>(SkipReason::NR_RESULT_LOST) + 1u;

// Each value's text, at the value's index.
inline constexpr std::array<std::string_view, SKIP_REASON_COUNT> SKIP_REASON_TEXTS = {
    std::string_view(),
    "off",
    "loading",
    "grace",
    "draining",
    "failed",
    "invalid input",
    "resizing",
    "budget",
    "frame too small",
    "create failed",
    "evaluate failed",
    "inactive",
    "unsupported format",
    "no pipeline for the target format",
    "already ran this frame",
    "descriptors busy",
    "out of memory",
    "encode failed",
    "decode failed",
    "intensity 0",
    "pipeline failed",
    "motion vectors outside their texture",
    "list state unknown",
    "DLSS runs on a compute queue",
    "not DLSS-SR or Ray Reconstruction",
    "device removed",
    "not placed after DLSS",
    "NR is off",
    "no DLSS colour to swap",
    "frame generation is on",
    "waiting for the earlier stages' NR",
    "the game's present queue is still busy",
    "a submission on the game's present queue failed",
    "Uplift's command buffers are busy",
    "Uplift's command buffer could not be begun",
    "Uplift's command buffer could not be submitted",
    "the motion vectors changed (NR reloads if that lasts)",
    "the motion vectors changed: NR reloads",
    "NGX shut down",
    "device lost",
    "no readable motion vectors",
    "preparing the new format",
    "bridge stopped",
    "bridge busy",
    "DLSS's region lies outside its texture",
    "the bridge could not share DLSS's image",
    "the private list was not submitted",
    "NR's result did not come back",
};

[[nodiscard]] constexpr std::string_view SkipReasonText(SkipReason reason) {
  return SKIP_REASON_TEXTS[static_cast<size_t>(reason)];
}

// The value whose text is `text`. NONE for empty, and for a text no value has: only a helper of another build could send one across the wire, and
// ipc::PROTOCOL_VERSION keeps the helper to the add-on's own build.
[[nodiscard]] constexpr SkipReason SkipReasonFromText(std::string_view text) {
  for (size_t index = 0u; index < SKIP_REASON_COUNT; ++index) {
    if (SKIP_REASON_TEXTS[index] == text) return static_cast<SkipReason>(index);
  }
  return SkipReason::NONE;
}

// Whether Session::Evaluate wrote a fresh status message (SessionStatus::message) for `reason`. Every other reason (RESIZING, a state, INVALID_INPUT)
// leaves the message from an earlier call, so ContextMessage must not be told to trust it.
[[nodiscard]] constexpr bool SessionWroteMessage(SkipReason reason) {
  return reason == SkipReason::FRAME_TOO_SMALL || reason == SkipReason::BUDGET || reason == SkipReason::CREATE_FAILED
         || reason == SkipReason::EVALUATE_FAILED;
}

}  // namespace uplift::nr
