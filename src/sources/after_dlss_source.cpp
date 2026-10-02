#include "sources/after_dlss_source.hpp"

#include <utility>

namespace uplift::sources {

AfterDlssSource::AfterDlssSource(NrPipeline& pipeline, nr::Timeline& timeline) : pipeline_(pipeline), timeline_(timeline) {}

color::Encoding AfterDlssSource::ResolveEncoding(color::Encoding setting, bool is_hdr) {
  if (setting != color::Encoding::AUTO) return setting;
  return (is_hdr ? color::Encoding::LINEAR_BT709 : color::Encoding::SRGB);
}

nr::Rect ResolveOutputRegion(const ngx_hooks::DlssFrame& frame) {
  if (frame.output_region.width != 0u && frame.output_region.height != 0u) return frame.output_region;
  if (frame.output == nullptr) return {};
  const D3D12_RESOURCE_DESC output = frame.output->GetDesc();
  return {.x = 0u, .y = 0u, .width = static_cast<uint32_t>(output.Width), .height = output.Height};
}

GameFrame ReadGameFrame(const ngx_hooks::DlssFrame& frame, ID3D12Resource* resource, nr::Rect region, bool reset_hint,
                        const AfterDlssConfig& config) {
  const color::Encoding encoding = AfterDlssSource::ResolveEncoding(config.encoding, frame.snapshot.IsHdr());
  GameFrame game = {
      .target = {
          .resource = resource,
          .region = region,
          .encoding = encoding,
          .diffuse_white_nits = (config.diffuse_white_nits > 0.f ? config.diffuse_white_nits
                                                                 : color::DefaultDiffuseWhiteNits(encoding)),
          .transfer_strength = config.transfer_strength,
          .color_strength = config.color_strength,
          .exposure = frame.exposure_texture,
          .exposure_factor = frame.exposure_scale / frame.pre_exposure,
      },
      .inputs = {
          .motion_scale_x = frame.mv_scale_x * config.motion_scale_x,
          .motion_scale_y = frame.mv_scale_y * config.motion_scale_y,
          .reset_hint = reset_hint,
          .depth_inverted = (config.depth_inverted.value_or(frame.snapshot.DepthInverted()) ? 1 : 0),
          .chained_history = config.chained_history,
      },
  };
  if (config.motion_vectors && frame.motion_vectors != nullptr) {
    game.inputs.motion = {.resource = frame.motion_vectors, .rect = frame.motion_region};
  }
  return game;
}

PipelineResult AfterDlssSource::Run(DlssFrameHost& host, const ngx_hooks::DlssFrame& frame, bool reset_hint,
                                    const AfterDlssConfig& config) {
  // Plan 3, Minor 4: pessimistic; only a frame that applies NR clears the owed reset.
  const bool reset_was_owed = std::exchange(reset_owed_, true);
  ID3D12GraphicsCommandList* const list = host.NativeList();
  if (list == nullptr || frame.output == nullptr) return {.reason = "invalid input"};
  if (!host.StateKnown()) return {.reason = "list state unknown"};
  // A COMPUTE list is declined until E11 shows the runtime is safe there (v2 design §3.4).
  if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return {.reason = "DLSS runs on a compute queue"};
  const nr::Rect region = ResolveOutputRegion(frame);
  if (!nr::MeetsNrFloor({region.width, region.height})) return {.reason = "frame too small"};
  if (!pipeline_.SupportsUavTarget(frame.output->GetDesc().Format)) return {.reason = "unsupported format"};
  // Exact pass-through (v2 design §3.4): nothing is recorded, so the Output stays bit-identical.
  if (config.controls.intensity <= 0.f) return {.reason = "intensity 0"};
  const GameFrame game = ReadGameFrame(frame, frame.output, region, (reset_hint || reset_was_owed), config);
  const PipelineResult result = RecordOnGameList(host, timeline_, config.restore, [&] {
    return pipeline_.RecordUav(list, game.target, game.inputs, config.controls, config.layout);
  });
  if (result.nr_applied) {
    reset_owed_ = false;
  }
  return result;
}

}  // namespace uplift::sources
