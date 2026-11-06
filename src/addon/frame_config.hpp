#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include "color/encoding.hpp"
#include "nr/types.hpp"
#include "sources/look_plan.hpp"
#include "state/compute_shadow.hpp"
#include "ui/controls_coalescer.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// A route's drain at once (NR's owner moved away from it): its Session goes to OFF with no grace.
inline constexpr std::chrono::milliseconds NO_GRACE{0};

enum class FgPresentPath {
  NORMAL,  // Ordinary frame-generation blocking and placement rules.
  ALL_PRESENTS,  // Allow frame-generation presentations without selecting Before FG.
  BEFORE_PENDING,  // Keep the Before FG stage selected while its Present recording is blocked.
  BEFORE_READY,  // Keep the Before FG stage selected and allow its Present recording.
};

[[nodiscard]] constexpr bool AllowsFrameGeneration(FgPresentPath path) {
  return path == FgPresentPath::ALL_PRESENTS || path == FgPresentPath::BEFORE_READY;
}

[[nodiscard]] constexpr bool IsBeforeFg(FgPresentPath path) {
  return path == FgPresentPath::BEFORE_PENDING || path == FgPresentPath::BEFORE_READY;
}

// Select Before FG without changing permission to record with frame generation.
[[nodiscard]] constexpr FgPresentPath WithBeforeFg(FgPresentPath path) {
  return AllowsFrameGeneration(path) ? FgPresentPath::BEFORE_READY : FgPresentPath::BEFORE_PENDING;
}

// Change recording permission without changing whether Before FG is selected.
[[nodiscard]] constexpr FgPresentPath WithFrameGeneration(FgPresentPath path, bool allowed) {
  if (IsBeforeFg(path)) return allowed ? FgPresentPath::BEFORE_READY : FgPresentPath::BEFORE_PENDING;
  return allowed ? FgPresentPath::ALL_PRESENTS : FgPresentPath::NORMAL;
}

// The settings one frame runs with on its route: BuildFrameSettings's output (the user's settings, the drag, the Defaults view and the controls'
// coalescer at the present), which a route adjusts on its own copy only to drain at once (NO_GRACE), to run at Present (`source`, `pre_upscale`) or for
// Before FG's outer inputs (ApplyOuterInputs: the three motion fields). Used by the evaluates until the next present.
struct FrameSettings {
  ui::SessionOptions session;
  nr::Controls controls;
  color::Encoding encoding = color::Encoding::AUTO;
  float diffuse_white_nits = 0.f;  // 0 = automatic for the resolved encoding
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  ui::PlacementSource source = ui::PlacementSource::AUTO;
  bool allow_cross_queue_passes = false;
  bool motion_vectors = true;           // MotionVectors is not None, or must act like it is (MotionVectors ==
                                         // Launchpad while placement is inside the game's frame)
  bool motion_vectors_want_launchpad = false;  // MotionVectors == Launchpad (or Lumenite), for the motion
                                                // readout's "Launchpad feeds the presented image only" reason
  float motion_scale_x = 1.f;           // MotionScaleX/Y
  float motion_scale_y = 1.f;
  std::optional<bool> depth_inverted;                        // DepthDirection; nullopt = Game
  bool chained_history = true;                               // ChainedHistory
  state::RestoreMode restore = state::RestoreMode::FULL;     // StateRestore
  ui::ResolutionMode resolution = ui::ResolutionMode::FULL;
  float resolution_scale = 100.f;                            // Resolution = Custom: % of the output
  bool resolution_slider_held = false;                       // the Custom slider is being dragged: the work size waits
  color::Upsampling upsampling = color::Upsampling::EDGE_AWARE;
  bool pre_upscale = false;  // NR before DLSS-SR upscales
  sources::LookConfig look;  // the look stage, the colour fixes, Mask, UI correction, passes 2..10
  bool launchpad_motion = true;       // MotionVectors Auto, Launchpad or Lumenite: the Present path may bind UPLIFT_MV
  uint32_t pass_view_limit = 0u;      // the Defaults view's passes on the live features; 0 = all
  uint64_t settings_generation = 0u;  // changes with every saved settings change; restarts the retry backoff
  // Passes 2..10's stored "Runs at" (Pass{n}Stage). Without FrameFacts::split_capable every pass runs at pass 1's stage.
  std::array<ui::PassStage, ui::MAX_PASSES - 1u> pass_stages = {};
  // Split chain: a later stage group's own Resolution and "Keep faces here", at [0] After DLSS and [1] Present. Pass 1's group
  // uses `resolution` and Keep faces itself.
  struct StageResolution {
    ui::ResolutionMode mode = ui::ResolutionMode::FULL;
    float scale = 100.f;  // Custom: % of the output
    bool slider_held = false;  // its Custom slider is being dragged: the work size waits for the release
    friend bool operator==(const StageResolution&, const StageResolution&) = default;
  };
  std::array<StageResolution, 2> stage_resolution = {};
  std::array<bool, 2> stage_keep_faces = {};
  friend bool operator==(const FrameSettings&, const FrameSettings&) = default;
};

// What the add-on observed at this present, or decided for it (the route, Before FG, the claims), for the evaluates until the next one.
struct FrameFacts {
  FgPresentPath fg_present_path = FgPresentPath::NORMAL;
  bool game_facing_present = false;  // Observed execution fact, independent of the Present policy.
  std::optional<bool> motion_at_dlss_stage;  // Preserve the game-stage choice when Before FG overrides only Present.
  uint32_t ngx_frame_generation = 0u;                        // live NGX FrameGeneration features (FeatureRegistry)
  bool nr_allowed = true;                                    // NrClaim lets this device load NR
  bool uplift_mv_lumenite = false;    // UPLIFT_MV holds Lumenite's vectors (Uplift.fx compiled with UPLIFT_USE_LUMENITE): the readouts name it
  bool present_motion_copy = false;   // A native Vulkan context copies DLSS's motion vectors for the Present path (set by the add-on only)
  bool vulkan = false;                // a Vulkan device's bridged context, whose motion line must not name Direct3D 12 or Launchpad (set by the add-on only)
  uint64_t upscaler_creates = 0u;     // the game's DLSS creates on this device so far (FeatureRegistry::UpscalerCreates; set by the add-on only)
  // DLSS was seen on this device and no DLSS feature of its API is live there now (addon::DlssReleased; set by the add-on only): the game
  // released it, or on Direct3D 11 shut NGX down. The context's DlssOffLatch decides from it when the game switched its DLSS off.
  bool dlss_released = false;
  bool marker_rendered_early = false;
  nr::Size early_motion_extent{};
  bool effects_on = false;  // ReShade's effects are on, so NR before effects may wait for their begin-effects event (set by the add-on only)
  // ReShade's RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN is set: the game's camera images, DLSS's among them, are upside down against the back buffer
  // (Unity, for one), so DLSS's motion vectors are flipped for the Present path (set by the add-on only).
  bool dlss_motion_upside_down = false;
  // NR at Present runs natively on this Vulkan device, in the native context (the route the add-on chose; set by the add-on only).
  bool vk_native_present = false;
  // The game created no DLSS on this Vulkan device, so the native context's NR load initialises an NGX core already loaded in the process for the
  // device too (SnippetConfig::initialize_core_for_device; set by the add-on only).
  bool vk_initialize_core = false;
  // This route runs passes at more than one stage (set by the add-on: Direct3D 12 native and the Direct3D 11 DLSS bridge, SplitCapable).
  bool split_capable = false;
  friend bool operator==(const FrameFacts&, const FrameFacts&) = default;
};

// The name of what UPLIFT_MV holds, and why none arrived, for the motion readouts.
inline std::string_view UpliftMvName(const FrameFacts& facts) { return (facts.uplift_mv_lumenite ? "Lumenite" : "Launchpad"); }
inline std::string_view NoUpliftMvReason(const FrameFacts& facts) {
  return (facts.uplift_mv_lumenite ? "no UPLIFT_MV this frame (Uplift.fx below Lumenite's Kernel)" : "no UPLIFT_MV this frame (Uplift.fx below Launchpad)");
}

// FrameSettings::look from `settings`, with passes 2..10's `later_controls` (ControlsCoalescer::LaterPasses);
// the frame time is set per recording.
[[nodiscard]] sources::LookConfig LookConfigFrom(const ui::Settings& settings,
                                                 const std::array<std::optional<nr::Controls>, sources::LATER_PASSES>& later_controls);


// The FrameSettings one present runs with, for the 64-bit add-on and the helper alike: a function of the settings, the drag, the Defaults view, the
// settings generation and the coalescer's state at `now` (it advances the coalescer, as each present does).
struct FrameSettingsInputs {
  const ui::Settings* settings = nullptr;
  ui::DragState drag;
  bool defaults_view = false;
  uint64_t settings_generation = 0u;
};
[[nodiscard]] FrameSettings BuildFrameSettings(const FrameSettingsInputs& inputs, ui::ControlsCoalescer* coalescer,
                                               std::chrono::steady_clock::time_point now);
// The FrameFacts a present starts from: the facts the caller observed, and the Present path the FgPresent setting asks for. The add-on and the helper set
// the rest as they observe or decide it.
struct FrameFactsInputs {
  uint32_t ngx_frame_generation = 0u;
  bool nr_allowed = true;
  bool uplift_mv_lumenite = false;  // Uplift.fx writes Lumenite's vectors into UPLIFT_MV (the host's link knows)
};
[[nodiscard]] FrameFacts BuildFrameFacts(const ui::Settings& settings, const FrameFactsInputs& inputs);
// The game-facing present the helper records for Before FG: it is game-facing and BEFORE_READY, effects are off, and Before FG's outer motion
// inputs (BeforeFgMotion) apply.
void ApplyGameFacingPresent(FrameSettings& frame_settings, FrameFacts& facts, const ui::Settings& settings);

}  // namespace uplift::addon
