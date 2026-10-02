#include "gl/format.hpp"

#include <format>

namespace uplift::gl {
namespace {

// ReShade's api::format values that DXGI has no number for (reshade_api_format.hpp): r8g8b8x8_unorm and r8g8b8x8_unorm_srgb.
constexpr uint32_t RESHADE_R8G8B8X8_UNORM = 0x424757B9u;
constexpr uint32_t RESHADE_R8G8B8X8_UNORM_SRGB = 0x424757BAu;

}  // namespace

DXGI_FORMAT SharedFormatOf(DXGI_FORMAT reported) {
  switch (static_cast<uint32_t>(reported)) {  // ReShade's two X8 formats are not DXGI enumerators
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case RESHADE_R8G8B8X8_UNORM:
    case RESHADE_R8G8B8X8_UNORM_SRGB:       return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                                return DXGI_FORMAT_UNKNOWN;
  }
}

GLenum InternalFormatOf(DXGI_FORMAT shared) {
  switch (shared) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return GL_RGBA8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return GL_RGB10_A2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return GL_RGBA16F;
    case DXGI_FORMAT_R8_UNORM:           return GL_R8;
    case DXGI_FORMAT_R16_FLOAT:          return GL_R16F;
    case DXGI_FORMAT_R32_FLOAT:          return GL_R32F;
    case DXGI_FORMAT_R16G16_FLOAT:       return GL_RG16F;
    default:                             return 0u;
  }
}

std::string BackBufferProblem(const ImageInfo& frame, bool at_present) {
  if (at_present && frame.samples != 1u) {
    return "Multisampled back buffers need the Uplift technique on OpenGL: NR then runs on ReShade's resolved copy";
  }
  if (SharedFormatOf(frame.format) == DXGI_FORMAT_UNKNOWN) {
    return std::format("Unsupported back-buffer format (DXGI_FORMAT {})", static_cast<int>(frame.format));
  }
  return {};
}

}  // namespace uplift::gl
