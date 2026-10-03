#include "addon/dlss_summary.hpp"

#include <Windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <filesystem>
#include <format>

#include "addon/environment.hpp"
#include "addon/placement.hpp"
#include "addon/reshade_version.hpp"
#include "nr/d3d11_handles.hpp"
#include "nr/vk_handles.hpp"

namespace uplift::addon {
namespace {

std::string DescribeD3D12(ID3D12Resource* resource) {
  if (resource == nullptr) return "none";
  return std::format("DXGI_FORMAT {}", static_cast<int>(resource->GetDesc().Format));
}

std::string DescribeD3D11(ID3D12Resource* punned) {
  ID3D11Resource* const resource = nr::D3D11ResourceOf(punned);
  if (resource == nullptr) return "none";
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return "not a 2D texture";
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  return std::format("DXGI_FORMAT {}", static_cast<int>(desc.Format));
}

std::string DescribeVulkan(ID3D12Resource* punned) {
  if (punned == nullptr) return "none";
  const NVSDK_NGX_Resource_VK& resource = *nr::VkResourceOf(punned);
  if (resource.Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return "a Vulkan buffer";
  return std::format("VkFormat {}", static_cast<int>(resource.Resource.ImageViewInfo.Format));
}

}  // namespace

std::string FormatDlssSummary(const DlssSummary& s) {
  const std::string feature = (s.feature == NVSDK_NGX_Feature_SuperSampling       ? std::string("DLSS-SR")
                               : s.feature == NVSDK_NGX_Feature_RayReconstruction ? std::string("Ray Reconstruction")
                                                                                  : std::format("feature {}", static_cast<int>(s.feature)));
  const std::string motion = (s.motion_vectors ? std::format("{}x{} ({}), {}, scale {} x {}", s.motion.width, s.motion.height, s.motion_format,
                                                             (s.snapshot.MvLowRes() ? "low-res" : "output-res"), s.mv_scale_x, s.mv_scale_y)
                                               : std::string("none"));
  const std::string file = (s.dlss_file.empty() ? std::string("not loaded as nvngx_dlss.dll")
                                                : std::format("{} ({})", s.dlss_file, (s.dlss_version.empty() ? "version unknown" : s.dlss_version)));
  return std::format("First DLSS evaluate: {}, {} ({}), {}; render {}x{} ({}) -> output {}x{} ({}), {}; motion vectors: {}; exposure: {}, pre-exposure {}, "
                     "scale {}, auto-exposure {}; DLSS file: {}",
                     s.api, feature, QualityName(s.snapshot.perf_quality), s.device, s.render.width, s.render.height, s.color_format, s.output.width,
                     s.output.height, s.output_format, (s.snapshot.IsHdr() ? "HDR" : "LDR"), motion, (s.exposure_texture ? "texture" : "no texture"),
                     s.pre_exposure, s.exposure_scale, (s.snapshot.AutoExposure() ? "on" : "off"), file);
}

DlssSummary SummarizeEvaluate(ngx_hooks::NgxApi api, const ngx_hooks::DlssFrame& frame, std::string_view device) {
  const auto describe = [api](ID3D12Resource* resource) {
    switch (api) {
      case ngx_hooks::NgxApi::D3D11:  return DescribeD3D11(resource);
      case ngx_hooks::NgxApi::VULKAN: return DescribeVulkan(resource);
      case ngx_hooks::NgxApi::D3D12:  break;
    }
    return DescribeD3D12(resource);
  };
  DlssSummary summary = {
      .api = (api == ngx_hooks::NgxApi::D3D11 ? "Direct3D 11" : api == ngx_hooks::NgxApi::VULKAN ? "Vulkan" : "Direct3D 12"),
      .device = device,
      .feature = frame.feature,
      .snapshot = frame.snapshot,
      .render = frame.render,
      .color_format = describe(frame.color),
      .output = {frame.output_region.width, frame.output_region.height},
      .output_format = describe(frame.output),
      .motion_vectors = (frame.motion_vectors != nullptr),
      .motion = {frame.motion_region.width, frame.motion_region.height},
      .motion_format = describe(frame.motion_vectors),
      .mv_scale_x = frame.mv_scale_x,
      .mv_scale_y = frame.mv_scale_y,
      .exposure_texture = (frame.exposure_texture != nullptr),
      .pre_exposure = frame.pre_exposure,
      .exposure_scale = frame.exposure_scale,
  };
  // The DLSS file the game (or its mod) loaded, if it is nvngx_dlss.dll; a game that ships another name shows as "not loaded as nvngx_dlss.dll".
  if (const HMODULE dlss = GetModuleHandleW(L"nvngx_dlss.dll"); dlss != nullptr) {
    std::wstring path(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(dlss, path.data(), static_cast<DWORD>(path.size()));
    if (length > 0u && length < path.size()) {
      path.resize(length);
      summary.dlss_file = Utf8FromPath(std::filesystem::path(path));
      if (const std::optional<ModuleVersion> version = ReadModuleVersion(dlss)) summary.dlss_version = FormatModuleVersion(*version);
    }
  }
  return summary;
}

}  // namespace uplift::addon
