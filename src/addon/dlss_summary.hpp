#pragma once

#include <string>
#include <string_view>

#include "ngx_hooks/dlss_capture.hpp"
#include "ngx_hooks/feature_registry.hpp"
#include "ngx_hooks/ngx_api.hpp"
#include "nr/ngx.hpp"
#include "nr/types.hpp"

namespace uplift::addon {

// Plan 18 (design §5): the first game DLSS evaluate a process shows Uplift, written once at INFO so a remote tester's ReShade.log says what the game or mod
// does. Plain data: FormatDlssSummary is pure.
struct DlssSummary {
  std::string_view api;     // "Direct3D 11", "Direct3D 12", "Vulkan"
  std::string_view device;  // which device or context (the add-on's words)
  NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_Reserved_Unknown;
  ngx_hooks::CreateSnapshot snapshot;
  nr::Size render;             // this evaluate's render subrect
  std::string color_format;    // "DXGI_FORMAT 10", "VkFormat 97", "none"
  nr::Size output;
  std::string output_format;
  bool motion_vectors = false;
  nr::Size motion;
  std::string motion_format;
  float mv_scale_x = 1.f;
  float mv_scale_y = 1.f;
  bool exposure_texture = false;
  float pre_exposure = 1.f;
  float exposure_scale = 1.f;
  std::string dlss_file;     // the loaded nvngx_dlss.dll's path (UTF-8); empty when none is loaded under that name
  std::string dlss_version;  // its file version, "1.0.11.0"; empty when unknown
};

// The one INFO line: "First DLSS evaluate: Direct3D 11, DLSS-SR (Quality), the game's device; render 1280x720 (DXGI_FORMAT 10) -> output 1920x1080 ...".
[[nodiscard]] std::string FormatDlssSummary(const DlssSummary& summary);

// Fills a summary from one captured frame: `render`, the scales and exposure values, whether vectors and an exposure texture were set; describes `color`,
// `output` and `motion_vectors` as `api` holds them (Direct3D 12: GetDesc; Direct3D 11: the ID3D11Texture2D's format, else "not a 2D texture"; Vulkan: the
// image view's VkFormat, else "a Vulkan buffer"; an unset one "none"); and reads the loaded nvngx_dlss.dll's path and version. `frame`'s resources are the
// game's, valid for the hooked call alone (on Vulkan, the copies the frame points at): call it from that call. Takes no lock and keeps no resource.
[[nodiscard]] DlssSummary SummarizeEvaluate(ngx_hooks::NgxApi api, const ngx_hooks::DlssFrame& frame, std::string_view device);

}  // namespace uplift::addon
