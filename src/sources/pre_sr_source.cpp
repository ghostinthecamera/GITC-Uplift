#include "sources/pre_sr_source.hpp"

#include <utility>

namespace uplift::sources {

PreSrSource::PreSrSource(NrPipeline& pipeline, nr::Timeline& timeline) : pipeline_(pipeline), timeline_(timeline) {}

PipelineResult PreSrSource::Run(DlssFrameHost& host, const ngx_hooks::DlssFrame& frame, bool reset_hint,
                                const AfterDlssConfig& config) {
  const bool reset_was_owed = std::exchange(reset_owed_, true);
  ID3D12GraphicsCommandList* const list = host.NativeList();
  if (list == nullptr || frame.color == nullptr) return {.reason = "invalid input"};
  // v2 design §3.9: Ray Reconstruction's input is still noisy, so NR stays after it.
  if (frame.feature != NVSDK_NGX_Feature_SuperSampling) return {.reason = "Ray Reconstruction keeps NR after DLSS"};
  if (!host.StateKnown()) return {.reason = "list state unknown"};
  if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return {.reason = "DLSS runs on a compute queue"};
  if (config.controls.intensity <= 0.f) return {.reason = "intensity 0"};  // exact pass-through: no swap either
  const GameFrame game = ReadGameFrame(frame, frame.color, frame.color_region, (reset_hint || reset_was_owed), config);
  if (game.inputs.motion.resource != nullptr) {
    // v2 design §3.9 step 1: the motion-vector region lies inside its texture.
    const D3D12_RESOURCE_DESC motion = game.inputs.motion.resource->GetDesc();
    const nr::Rect& rect = game.inputs.motion.rect;
    if (uint64_t{rect.x} + rect.width > motion.Width || uint64_t{rect.y} + rect.height > motion.Height) {
      return {.reason = "motion vectors outside their texture"};
    }
  }
  const PipelineResult result = RecordOnGameList(host, timeline_, config.restore, [&] {
    return pipeline_.RecordPreSr(list, game.target, game.inputs, config.controls, config.layout);
  });
  if (result.nr_applied) {
    reset_owed_ = false;
  }
  return result;
}

}  // namespace uplift::sources
