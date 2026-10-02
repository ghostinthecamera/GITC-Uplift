#include "ngx_hooks/dlss_capture.hpp"

#include <cmath>

#include "ngx_hooks/ngx_parameters.hpp"

namespace uplift::ngx_hooks {

DlssFrame CaptureDlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle,
                           const FeatureRecord& record) {
  const auto one_when_unset = [&parameters](const char* key) {
    const std::optional<float> value = ReadFloat(parameters, key);
    return ((value.has_value() && *value != 0.f && std::isfinite(*value)) ? *value : 1.f);
  };
  const auto base = [&parameters](const char* key) { return ReadUint(parameters, key).value_or(0u); };
  const nr::Size render_subrect = {base(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width),
                                   base(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height)};
  const nr::Size render = (render_subrect.Empty() ? record.snapshot.render : render_subrect);
  const nr::Size output = record.snapshot.output;
  const nr::Size motion = (record.snapshot.MvLowRes() ? render : output);
  return {
      .handle = handle,
      .feature = record.feature,
      .snapshot = record.snapshot,
      .serial = record.serial,
      .color = ReadResource(parameters, NVSDK_NGX_Parameter_Color),
      .output = ReadResource(parameters, NVSDK_NGX_Parameter_Output),
      .motion_vectors = ReadResource(parameters, NVSDK_NGX_Parameter_MotionVectors),
      .depth = ReadResource(parameters, NVSDK_NGX_Parameter_Depth),
      .exposure_texture = ReadResource(parameters, NVSDK_NGX_Parameter_ExposureTexture),
      .jitter_x = ReadFloat(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X).value_or(0.f),
      .jitter_y = ReadFloat(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y).value_or(0.f),
      .mv_scale_x = one_when_unset(NVSDK_NGX_Parameter_MV_Scale_X),
      .mv_scale_y = one_when_unset(NVSDK_NGX_Parameter_MV_Scale_Y),
      .pre_exposure = one_when_unset(NVSDK_NGX_Parameter_DLSS_Pre_Exposure),
      .exposure_scale = one_when_unset(NVSDK_NGX_Parameter_DLSS_Exposure_Scale),
      .reset = (ReadInt(parameters, NVSDK_NGX_Parameter_Reset).value_or(0) != 0),
      .render = render,
      .color_region = {base(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X),
                       base(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y), render.width, render.height},
      .motion_region = {base(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X),
                        base(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y), motion.width, motion.height},
      .output_region = {base(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X),
                        base(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y), output.width, output.height},
  };
}

DlssFrame CaptureVkDlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record,
                             VkDlssResources* copies) {
  DlssFrame frame = CaptureDlssFrame(parameters, handle, record);
  *copies = {};
  const auto copy = [](ID3D12Resource*& field, NVSDK_NGX_Resource_VK* copied) {
    if (field == nullptr) return;
    *copied = *nr::VkResourceOf(field);  // the game's struct, read during the call
    field = nr::AsResource(copied);
  };
  copy(frame.color, &copies->color);
  copy(frame.output, &copies->output);
  copy(frame.motion_vectors, &copies->motion_vectors);
  copy(frame.depth, &copies->depth);
  copy(frame.exposure_texture, &copies->exposure_texture);
  return frame;
}

nr::Rect ResolveVkOutputRegion(const DlssFrame& frame, const VkDlssResources& copies) {
  if (frame.output_region.width != 0u && frame.output_region.height != 0u) return frame.output_region;
  if (frame.output == nullptr || copies.output.Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {};
  const NVSDK_NGX_ImageViewInfo_VK& output = copies.output.Resource.ImageViewInfo;
  return {.x = 0u, .y = 0u, .width = output.Width, .height = output.Height};
}

bool ResetMerger::Filter(bool game_reset, std::chrono::steady_clock::time_point now) {
  if (!game_reset) return false;
  if (last_passed_ && now - *last_passed_ < window_) return false;
  last_passed_ = now;
  return true;
}

}  // namespace uplift::ngx_hooks
