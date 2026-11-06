#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace uplift::text {

enum class BeforeFgNote {
  NONE,
  PARTIAL_PRESENT,
  NONBLOCKING_PRESENT,
  DX12_PRESENT,
  DETAIL,
  VK_QUEUE_UNTRACKED,
  VK_RELAY_FAILED,
  VK_PRESENT,
  MASK_UNAVAILABLE,
  MOTION_SELECTION_REQUIRED,
  WAITING_DLSS_MOTION,
  HELPER_DLSS_MOTION,
  WAITING_HELPER_FRAME,
  NO_VK_PRESENT,
  VK_RESHADE_PRESENT,
  WAITING_VK_FRAME,
  HOOKS_UNAVAILABLE,
  BRIDGE_HANDOFF_UNAVAILABLE,
  DX12_RESHADE_PRESENT,
  WAITING_DX12_FRAME,
  NO_STREAMLINE_PRESENT,
  COUNT,
};

// UNAVAILABLE marks hand-off blockers shown in Details; INFO includes progress and recording outcomes.
enum class BeforeFgSeverity { NONE, INFO, UNAVAILABLE };

inline constexpr char BEFORE_FG_DETAIL_LINE[] = "Before FG: {}";

inline constexpr char WAIT_SUCCESSFUL_PRESENT_LINE[] = "Waiting for a successful Present after a failed or occluded frame";
inline constexpr char OUTER_COLOR_SPACE_LINE[] = "Unsupported outer swapchain color space";
inline constexpr char REAL_BACKBUFFER_UNKNOWN_LINE[] = "Cannot identify the real backbuffer";
inline constexpr char REAL_BACKBUFFER_QUEUE_LINE[] = "No queue for the real backbuffer";
inline constexpr char OUTER_QUEUE_DEVICE_LINE[] = "Outer queue belongs to a different device";
inline constexpr char REAL_BACKBUFFER_UNAVAILABLE_LINE[] = "Could not get the real backbuffer";
inline constexpr char OUTER_IMAGE_DEVICE_LINE[] = "Outer image belongs to a different device";
inline constexpr char REAL_FRAME_COMMAND_LIST_LINE[] = "Could not create the real-frame command list";
inline constexpr char REAL_FRAME_RECORDING_BUSY_LINE[] = "Real-frame recording still in use";
inline constexpr char REAL_FRAME_SUBMISSION_LINE[] = "Real-frame command submission failed";
inline constexpr char PRIMARY_VK_SWAPCHAIN_LINE[] = "Cannot isolate the primary Vulkan swapchain";
inline constexpr char GAME_COMPLETION_SIGNAL_LINE[] = "Streamline Present has no game-rendered completion signal";
inline constexpr char VERIFIED_VK_SWAPCHAIN_LINE[] = "No verified game-facing Vulkan swapchain";
inline constexpr char OUTER_VK_IMAGE_LINE[] = "Unsupported outer Vulkan image or color space";
inline constexpr char OUTER_VK_IMAGE_USAGE_LINE[] = "Outer Vulkan image lacks copy-source or copy-destination usage";
inline constexpr char REAL_FRAME_ACQUISITION_LINE[] = "No matching real-frame image acquisition";
inline constexpr char DEVICE_GROUP_PRESENT_LINE[] = "Unsupported device-group presentation";
inline constexpr char VK_DEVICE_UNOBSERVED_LINE[] = "The Vulkan device was not observed at creation";
inline constexpr char OUTER_VK_QUEUE_FAMILY_LINE[] = "The outer Vulkan queue uses an unsupported queue family";
inline constexpr char REAL_FRAME_VK_RECORDING_LINE[] = "Real-frame Vulkan recording is busy or unavailable";
inline constexpr char VK_RECORDING_FUNCTIONS_LINE[] = "Vulkan recording functions are unavailable";
inline constexpr char VK_BRIDGE_NOT_READY_LINE[] = "The Vulkan bridge is not ready";
inline constexpr char VK_COPYBACK_RECORDING_LINE[] = "The Vulkan copy-back submission did not finish recording";

struct BeforeFgNoteSpec {
  BeforeFgSeverity severity;
  std::string_view line;
};

inline constexpr auto BEFORE_FG_NOTES = std::to_array<BeforeFgNoteSpec>({
    {BeforeFgSeverity::NONE, ""},  // NONE
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: Present1 updates only part of the image"},  // PARTIAL_PRESENT
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: nonblocking Present can retry the same image"},  // NONBLOCKING_PRESENT
    {BeforeFgSeverity::INFO, "Before FG: game-facing DX12 Present"},  // DX12_PRESENT
    {BeforeFgSeverity::INFO, BEFORE_FG_DETAIL_LINE},  // DETAIL
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: the game's Vulkan queue is untracked"},  // VK_QUEUE_UNTRACKED
    {BeforeFgSeverity::INFO, "Before FG: Vulkan relay submission failed"},  // VK_RELAY_FAILED
    {BeforeFgSeverity::INFO, "Before FG: game-facing Vulkan Present"},  // VK_PRESENT
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: the active ReShade mask cannot be associated with a real frame"},  // MASK_UNAVAILABLE
    {BeforeFgSeverity::UNAVAILABLE, "ReShade motion runs after frame generation: choose DLSS motion or Off in Advanced"},  // MOTION_SELECTION_REQUIRED
    {BeforeFgSeverity::UNAVAILABLE, "Before FG is waiting for the selected DLSS motion source"},  // WAITING_DLSS_MOTION
    {BeforeFgSeverity::UNAVAILABLE, "Before FG: DLSS motion is unavailable on the helper route"},  // HELPER_DLSS_MOTION
    {BeforeFgSeverity::INFO, "Before FG: waiting for the real frame through the Vulkan helper"},  // WAITING_HELPER_FRAME
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: no verified game-facing Vulkan Present"},  // NO_VK_PRESENT
    {BeforeFgSeverity::INFO, "Before FG: ReShade on the game-facing Vulkan swapchain"},  // VK_RESHADE_PRESENT
    {BeforeFgSeverity::INFO, "Before FG: waiting for the next real Vulkan frame"},  // WAITING_VK_FRAME
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: Streamline presentation hooks could not be initialized"},  // HOOKS_UNAVAILABLE
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: the selected bridge has no verified real-frame handoff"},  // BRIDGE_HANDOFF_UNAVAILABLE
    {BeforeFgSeverity::INFO, "Before FG: ReShade on the game-facing DX12 swapchain"},  // DX12_RESHADE_PRESENT
    {BeforeFgSeverity::INFO, "Before FG: waiting for the next real frame"},  // WAITING_DX12_FRAME
    {BeforeFgSeverity::UNAVAILABLE, "Before FG unavailable: no verified game-facing Streamline Present"},  // NO_STREAMLINE_PRESENT
});
static_assert(BEFORE_FG_NOTES.size() == static_cast<size_t>(BeforeFgNote::COUNT));

constexpr const BeforeFgNoteSpec& BeforeFgNoteInfo(BeforeFgNote note) {
  return BEFORE_FG_NOTES[static_cast<size_t>(note)];
}

struct BeforeFgMessage {
  BeforeFgNote id = BeforeFgNote::NONE;
  std::string detail;  // A recording result may borrow context storage that changes before the overlay reads it.

  BeforeFgSeverity Severity() const { return BeforeFgNoteInfo(id).severity; }
  std::string Text() const {
    return id == BeforeFgNote::DETAIL ? std::format(BEFORE_FG_DETAIL_LINE, detail) : std::string(BeforeFgNoteInfo(id).line);
  }
};

}  // namespace uplift::text
