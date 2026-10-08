#include "addon/frame_config.hpp"

#include <optional>

#include "state/compute_shadow.hpp"
#include "ui/settings_schema.hpp"

namespace uplift::addon {

FrameConfig BuildFrameConfig(const FrameConfigInputs& inputs, ui::ControlsCoalescer* coalescer,
                             std::chrono::steady_clock::time_point now) {
  const ui::Settings& settings = *inputs.settings;
  // Plan 6 (v2 design §3.16, D6): with Defaults on, the look group runs at its built-in values. The Session keeps the
  // user's pass count and evaluates fewer live passes (no rebuild). Never saved.
  const ui::Settings look_settings = (inputs.defaults_view ? ui::DefaultsView(settings) : settings);
  FrameConfig config = {
      .session = ui::ToSessionOptions(settings),
      .controls = coalescer->Update(look_settings, inputs.drag, now),
      .encoding = settings.encoding,
      .diffuse_white_nits = settings.diffuse_white_nits,
      .transfer_strength = settings.transfer_strength,
      .color_strength = settings.color_strength,
      .source = settings.source,
      .present_with_frame_gen = settings.present_with_frame_gen,
      // Launchpad and (2026-10-08) Lumenite feed the presented image only: inside the game's frame either runs on DLSS's own vectors.
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
      .ngx_frame_generation = inputs.ngx_frame_generation,
      .nr_allowed = inputs.nr_allowed,
      .resolution = settings.resolution_mode,
      .resolution_scale = settings.resolution_scale,
      .resolution_slider_held = inputs.drag.resolution_scale,
      .upsampling = settings.upsampling,
      .pre_upscale = settings.pre_upscale,
      .look = LookConfigFrom(look_settings, coalescer->LaterPasses(look_settings, inputs.drag.pass_slider, now)),
      .launchpad_motion = (settings.motion_vectors == ui::MotionVectorSource::AUTO || settings.motion_vectors == ui::MotionVectorSource::LAUNCHPAD
                           || settings.motion_vectors == ui::MotionVectorSource::LUMENITE),
      .uplift_mv_lumenite = inputs.uplift_mv_lumenite,
      .pass_view_limit = (inputs.defaults_view ? ui::DefaultSettings().pass_count : 0u),
      .settings_generation = inputs.settings_generation,
  };
  // Keep faces: Face protection restarts the extra run's history, so a drag applies it at most every 100 ms, as Skin structure's does.
  config.look.keep_faces.protection = coalescer->FaceProtection(look_settings, inputs.drag.face_protection, now);
  return config;
}

}  // namespace uplift::addon
