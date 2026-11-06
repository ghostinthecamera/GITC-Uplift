#include "addon/frame_config.hpp"

#include <optional>

#include "addon/before_fg_decision.hpp"
#include "state/compute_shadow.hpp"
#include "ui/settings_schema.hpp"

namespace uplift::addon {

sources::LookConfig LookConfigFrom(const ui::Settings& settings,
                                   const std::array<std::optional<nr::Controls>, sources::LATER_PASSES>& later_controls) {
  sources::LookConfig config = {
      .look = settings.look,
      .fixes = {
          .primaries = settings.source_primaries,
          .linear_unit_nits = settings.linear_unit_nits,
          .input_exposure = settings.input_exposure,
          .auto_mode = settings.auto_exposure_mode,
          .auto_blend = settings.auto_exposure_blend,
          .smooth_adapt = (settings.exposure_adapt == ui::ExposureAdapt::SMOOTH),
          .adapt_brighter = settings.adapt_brighter_stops,
          .adapt_darker = settings.adapt_darker_stops,
          .transfer = settings.neural_transfer,
          .chroma_clamp = settings.chroma_clamp,
          .near_black_guard = settings.near_black_guard,
      },
      .later_controls = later_controls,
      .mask = (settings.mask == ui::MaskMode::AUTO),
      .present_ui_correction = (settings.ui_correction == ui::UiCorrection::ON),
      .keep_faces = {.enabled = settings.keep_faces, .protection = settings.face_protection, .lighting_scale = settings.lighting_scale,
                     .show_mask = settings.show_face_mask, .tuning = settings.face_tuning},
  };
  for (size_t index = 0u; index < settings.passes.size(); ++index) {
    const ui::PassSettings& pass = settings.passes[index];
    if (!pass.follow_pass1) {
      config.later_strengths[index] = {.transfer = pass.transfer_strength, .color = pass.color_strength};
    }
  }
  return config;
}

FrameSettings BuildFrameSettings(const FrameSettingsInputs& inputs, ui::ControlsCoalescer* coalescer, std::chrono::steady_clock::time_point now) {
  const ui::Settings& settings = *inputs.settings;
  // With Defaults on, the look group runs at its built-in values. The Session keeps the
  // user's pass count and evaluates fewer live passes (no rebuild). Never saved.
  const ui::Settings look_settings = (inputs.defaults_view ? ui::DefaultsView(settings) : settings);
  FrameSettings frame = {
      .session = ui::ToSessionOptions(settings),
      .controls = coalescer->Update(look_settings, inputs.drag, now),
      .encoding = settings.encoding,
      .diffuse_white_nits = settings.diffuse_white_nits,
      .transfer_strength = settings.transfer_strength,
      .color_strength = settings.color_strength,
      .source = settings.source,
      .allow_cross_queue_passes = settings.allow_cross_queue_passes,
      // Launchpad and Lumenite feed the presented image only: inside the game's frame either runs on DLSS's own vectors.
      .motion_vectors = (settings.motion_vectors == ui::MotionVectorSource::AUTO
                         || settings.motion_vectors == ui::MotionVectorSource::DLSS
                         || ((settings.motion_vectors == ui::MotionVectorSource::LAUNCHPAD || settings.motion_vectors == ui::MotionVectorSource::LUMENITE)
                             && settings.source != ui::PlacementSource::PRESENT)),
      .motion_vectors_want_launchpad = (settings.motion_vectors == ui::MotionVectorSource::LAUNCHPAD
                                        || settings.motion_vectors == ui::MotionVectorSource::LUMENITE),
      .motion_scale_x = settings.motion_scale_x,
      .motion_scale_y = settings.motion_scale_y,
      .depth_inverted = (settings.depth_direction == ui::DepthDirection::GAME
                             ? std::optional<bool>()
                             : std::optional<bool>(settings.depth_direction == ui::DepthDirection::INVERTED)),
      .chained_history = settings.chained_history,
      .restore = (settings.state_restore == ui::StateRestore::MINIMAL ? state::RestoreMode::MINIMAL
                                                                      : state::RestoreMode::FULL),
      .resolution = settings.resolution_mode,
      .resolution_scale = settings.resolution_scale,
      .resolution_slider_held = inputs.drag.resolution_scale,
      .upsampling = settings.upsampling,
      .pre_upscale = settings.pre_upscale,
      .look = LookConfigFrom(look_settings, coalescer->LaterPasses(look_settings, inputs.drag.pass_slider, now)),
      .launchpad_motion = (settings.motion_vectors == ui::MotionVectorSource::AUTO || settings.motion_vectors == ui::MotionVectorSource::LAUNCHPAD
                           || settings.motion_vectors == ui::MotionVectorSource::LUMENITE),
      .pass_view_limit = (inputs.defaults_view ? ui::DefaultSettings().pass_count : 0u),
      .settings_generation = inputs.settings_generation,
      // The later stage groups' own Resolution and "Keep faces here" (After DLSS, Present). `split_capable` is the add-on's.
      .stage_resolution = {{{.mode = settings.resolution_mode_after_dlss, .scale = settings.resolution_scale_after_dlss,
                              .slider_held = inputs.drag.resolution_scale_after_dlss},
                             {.mode = settings.resolution_mode_present, .scale = settings.resolution_scale_present,
                              .slider_held = inputs.drag.resolution_scale_present}}},
      .stage_keep_faces = {settings.keep_faces_after_dlss, settings.keep_faces_present},
  };
  // Split chain: passes 2..10's "Runs at", which the Defaults view keeps (PASS_STAGE).
  for (size_t index = 0u; index < settings.passes.size(); ++index) {
    frame.pass_stages[index] = settings.passes[index].stage;
  }
  // Keep faces: Face protection restarts the extra run's history, so a drag applies it at most every 100 ms, as Skin structure's does.
  frame.look.keep_faces.protection = coalescer->FaceProtection(look_settings, inputs.drag.face_protection, now);
  return frame;
}

FrameFacts BuildFrameFacts(const ui::Settings& settings, const FrameFactsInputs& inputs) {
  return {
      .fg_present_path = settings.fg_present == ui::Settings::FgPresent::AFTER ? FgPresentPath::ALL_PRESENTS : FgPresentPath::NORMAL,
      .ngx_frame_generation = inputs.ngx_frame_generation,
      .nr_allowed = inputs.nr_allowed,
      .uplift_mv_lumenite = inputs.uplift_mv_lumenite,
  };
}

void ApplyGameFacingPresent(FrameSettings& frame_settings, FrameFacts& facts, const ui::Settings& settings) {
  facts.game_facing_present = true;
  facts.fg_present_path = FgPresentPath::BEFORE_READY;
  facts.effects_on = false;
  ApplyOuterInputs(frame_settings, facts, OuterMotionAction(settings.motion_vectors, settings.before_fg_motion));
}

}  // namespace uplift::addon
