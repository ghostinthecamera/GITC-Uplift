#include "sources/look_plan.hpp"

namespace uplift::sources {

bool NeedsChangePath(const LookConfig& config, bool mask_ready, bool look_loads) {
  return (look::LookRuns(config.look) && look_loads) || (config.mask && mask_ready) || config.fixes.near_black_guard
         || config.fixes.transfer == color::NeuralTransfer::CONSISTENT || config.fixes.chroma_clamp > 0.f;
}

LookPlan PlanLook(const LookConfig& config, nr::Size image, bool reduced, bool main_set_has_change, bool mask_ready, bool look_loads) {
  const bool shape = (look::LookRuns(config.look) && look_loads);
  const bool own_change = (!main_set_has_change && (reduced || NeedsChangePath(config, mask_ready, look_loads)));
  const bool stabilize = (shape && config.look.stabilize != look::StabilizeMode::OFF);
  return {
      .image = ((shape || own_change) ? image : nr::Size{}),
      .own_change = own_change,
      .shape = shape,
      .stabilize = stabilize,
      .detail = (stabilize && config.look.stabilize_detail),
  };
}

uint64_t FaceSurfaceBytes(nr::Size canvas) {
  constexpr uint64_t RGBA16F_BYTES = 8u;
  return (2u * canvas.Pixels() + 2u * look::MakeAtlas(canvas).size.Pixels()) * RGBA16F_BYTES;
}

nr::Controls FaceTwinControls(const nr::Controls& pass1, float protection) {
  nr::Controls twin = pass1;
  twin.auto_mask = true;
  twin.skin_structure = protection;
  return twin;
}

}  // namespace uplift::sources
