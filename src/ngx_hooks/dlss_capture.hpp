#pragma once

#include <chrono>
#include <optional>

#include "ngx_hooks/feature_registry.hpp"
#include "nr/ngx.hpp"
#include "nr/types.hpp"
#include "nr/vk_handles.hpp"

namespace uplift::ngx_hooks {

// One DLSS evaluate as the After-DLSS placement needs it (v2 design §3.1). Resources are the game's;
// nothing here is dereferenced or kept past the evaluate.
struct DlssFrame {
  const NVSDK_NGX_Handle* handle = nullptr;
  NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_Reserved_Unknown;
  CreateSnapshot snapshot;
  uint64_t serial = 0u;  // Plan 15 fix round: the feature's creation order (FeatureRecord::serial), so a context can tell a feature created after a shutdown
  ID3D12Resource* color = nullptr;
  ID3D12Resource* output = nullptr;
  ID3D12Resource* motion_vectors = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* exposure_texture = nullptr;  // 1×1: the final exposure scale
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  float mv_scale_x = 1.f;  // a 0 in the block means 1, as NGX's helper sets it
  float mv_scale_y = 1.f;
  float pre_exposure = 1.f;    // likewise
  float exposure_scale = 1.f;  // likewise
  bool reset = false;
  nr::Size render;         // DLSS.Render.Subrect.Dimensions, else the create-time render size
  nr::Rect color_region;   // the Input.Color subrect base, render-sized
  nr::Rect motion_region;  // the Input.MV subrect base; render-sized for low-resolution MVs, else output-sized
  nr::Rect output_region;  // the Output subrect base, sized by the create-time output size (empty when unknown)
};

[[nodiscard]] DlssFrame CaptureDlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle,
                                         const FeatureRecord& record);

// Plan 18 (design §2): CaptureDlssFrame for a Direct3D 11 block, whose resources NGX_D3D11_EVALUATE_DLSS_EXT sets as ID3D11Resource*: each is read as one
// (else as a void*) and carried punned (nr/d3d11_handles.hpp), never dereferenced. Regions the block leaves unset stay at the create-time sizes; the
// Direct3D 11 bridge resolves an empty one from the texture (bridge::ResolveRegion11).
[[nodiscard]] DlssFrame CaptureD3D11DlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record);

// Plan 13 (design §5, key decision b): a Vulkan evaluate's five resources, copied by value during the hooked call (the game's
// NVSDK_NGX_Resource_VK pointers are valid for that call alone). A DlssFrame captured with CaptureVkDlssFrame points at these copies.
struct VkDlssResources {
  NVSDK_NGX_Resource_VK color = {};
  NVSDK_NGX_Resource_VK output = {};
  NVSDK_NGX_Resource_VK motion_vectors = {};
  NVSDK_NGX_Resource_VK depth = {};
  NVSDK_NGX_Resource_VK exposure_texture = {};
};

// CaptureDlssFrame's scalars and regions for a Vulkan block, whose resources are NVSDK_NGX_Resource_VK* set as void*: each one the
// game set is copied into `copies`, and its DlssFrame field becomes nr::AsResource(&copies->...) (null where the game set none). The
// frame is only valid while `copies` is.
[[nodiscard]] DlssFrame CaptureVkDlssFrame(const NVSDK_NGX_Parameter& parameters, const NVSDK_NGX_Handle* handle, const FeatureRecord& record,
                                           VkDlssResources* copies);
// The region of a Vulkan DLSS Output NR runs on: the output subrect at the create-time output size, else the output's own size (an
// image view: Width x Height); empty when neither is known.
[[nodiscard]] nr::Rect ResolveVkOutputRegion(const DlssFrame& frame, const VkDlssResources& copies);

// The game's Reset as NR's reset hint (v2 design §3.1): the first Reset of a burst passes, and any
// further Reset within `window` of the last one that passed is merged, because a burst of history
// resets shows as pops.
class ResetMerger {
 public:
  explicit ResetMerger(std::chrono::milliseconds window = std::chrono::milliseconds(250)) : window_(window) {}
  [[nodiscard]] bool Filter(bool game_reset, std::chrono::steady_clock::time_point now);

 private:
  std::chrono::milliseconds window_;
  std::optional<std::chrono::steady_clock::time_point> last_passed_;
};

}  // namespace uplift::ngx_hooks
