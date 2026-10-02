#include "color/encoding.hpp"

namespace uplift::color {

std::optional<FormatInfo> DescribeFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return FormatInfo{.copy_format = DXGI_FORMAT_R8G8B8A8_TYPELESS, .source_view_format = DXGI_FORMAT_R8G8B8A8_UNORM,
                        .target_view_format = DXGI_FORMAT_R8G8B8A8_UNORM, .target_view_srgb = false, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return FormatInfo{.copy_format = DXGI_FORMAT_R8G8B8A8_TYPELESS, .source_view_format = DXGI_FORMAT_R8G8B8A8_UNORM,
                        .target_view_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, .target_view_srgb = true, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return FormatInfo{.copy_format = DXGI_FORMAT_B8G8R8A8_TYPELESS, .source_view_format = DXGI_FORMAT_B8G8R8A8_UNORM,
                        .target_view_format = DXGI_FORMAT_B8G8R8A8_UNORM, .target_view_srgb = false, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return FormatInfo{.copy_format = DXGI_FORMAT_B8G8R8A8_TYPELESS, .source_view_format = DXGI_FORMAT_B8G8R8A8_UNORM,
                        .target_view_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, .target_view_srgb = true, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return FormatInfo{.copy_format = DXGI_FORMAT_R10G10B10A2_TYPELESS, .source_view_format = DXGI_FORMAT_R10G10B10A2_UNORM,
                        .target_view_format = DXGI_FORMAT_R10G10B10A2_UNORM, .target_view_srgb = false, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return FormatInfo{.copy_format = DXGI_FORMAT_R16G16B16A16_TYPELESS, .source_view_format = DXGI_FORMAT_R16G16B16A16_FLOAT,
                        .target_view_format = DXGI_FORMAT_R16G16B16A16_FLOAT, .target_view_srgb = false, .bytes_per_pixel = 8u};
    default:
      return std::nullopt;
  }
}

std::optional<UavFormatInfo> DescribeUavFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return UavFormatInfo{.copy_format = DXGI_FORMAT_R16G16B16A16_TYPELESS, .source_view_format = DXGI_FORMAT_R16G16B16A16_FLOAT,
                           .uav_format = DXGI_FORMAT_R16G16B16A16_FLOAT, .bytes_per_pixel = 8u};
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return UavFormatInfo{.copy_format = DXGI_FORMAT_R32G32B32A32_TYPELESS, .source_view_format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                           .uav_format = DXGI_FORMAT_R32G32B32A32_FLOAT, .bytes_per_pixel = 16u};
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return UavFormatInfo{.copy_format = DXGI_FORMAT_R11G11B10_FLOAT, .source_view_format = DXGI_FORMAT_R11G11B10_FLOAT,
                           .uav_format = DXGI_FORMAT_R11G11B10_FLOAT, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return UavFormatInfo{.copy_format = DXGI_FORMAT_R10G10B10A2_TYPELESS, .source_view_format = DXGI_FORMAT_R10G10B10A2_UNORM,
                           .uav_format = DXGI_FORMAT_R10G10B10A2_UNORM, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return UavFormatInfo{.copy_format = DXGI_FORMAT_R8G8B8A8_TYPELESS, .source_view_format = DXGI_FORMAT_R8G8B8A8_UNORM,
                           .uav_format = DXGI_FORMAT_R8G8B8A8_UNORM, .bytes_per_pixel = 4u};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return UavFormatInfo{.copy_format = DXGI_FORMAT_B8G8R8A8_TYPELESS, .source_view_format = DXGI_FORMAT_B8G8R8A8_UNORM,
                           .uav_format = DXGI_FORMAT_B8G8R8A8_UNORM, .bytes_per_pixel = 4u};
    default:
      return std::nullopt;
  }
}

std::optional<Encoding> ResolveEncoding(Encoding setting, ColorSpace color_space, DXGI_FORMAT format) {
  if (setting != Encoding::AUTO) return setting;
  switch (color_space) {
    case ColorSpace::SRGB_NONLINEAR:       return Encoding::SRGB;
    case ColorSpace::EXTENDED_SRGB_LINEAR: return Encoding::SCRGB;
    case ColorSpace::HDR10_ST2084:         return Encoding::PQ_BT2100;
    case ColorSpace::HDR10_HLG:            return std::nullopt;
    case ColorSpace::UNKNOWN:              break;
  }
  const bool half_float = (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
  return (half_float ? Encoding::SCRGB : Encoding::SRGB);
}

float DefaultDiffuseWhiteNits(Encoding encoding) {
  switch (encoding) {
    case Encoding::LINEAR_BT709: return 100.f;
    case Encoding::PQ_BT2100:
    case Encoding::SCRGB:        return 250.f;
    case Encoding::SCRGB_NL:     return 203.f;
    case Encoding::AUTO:
    case Encoding::SRGB:         return 0.f;
  }
  return 0.f;
}

float InputScale(Encoding encoding, float diffuse_white_nits, float linear_unit_nits) {
  switch (encoding) {
    case Encoding::LINEAR_BT709: return (linear_unit_nits > 0.f ? linear_unit_nits : 100.f) / diffuse_white_nits;
    case Encoding::SCRGB:
    case Encoding::SCRGB_NL:     return (linear_unit_nits > 0.f ? linear_unit_nits : 80.f) / diffuse_white_nits;
    case Encoding::PQ_BT2100:    return 1.f / diffuse_white_nits;
    case Encoding::AUTO:
    case Encoding::SRGB:         return 1.f;
  }
  return 1.f;
}

std::string_view EncodingName(Encoding encoding) {
  switch (encoding) {
    case Encoding::AUTO:         return "Auto";
    case Encoding::LINEAR_BT709: return "Linear BT.709";
    case Encoding::SRGB:         return "sRGB";
    case Encoding::PQ_BT2100:    return "BT.2100 PQ";
    case Encoding::SCRGB:        return "scRGB";
    case Encoding::SCRGB_NL:     return "scRGB-nl";
  }
  return "Auto";
}

Primaries ResolvePrimaries(Primaries setting, Encoding encoding) {
  if (setting != Primaries::AUTO) return setting;
  return (encoding == Encoding::PQ_BT2100 ? Primaries::BT2020 : Primaries::BT709);
}

bool IsSdr(Encoding encoding) {
  return encoding == Encoding::SRGB || encoding == Encoding::AUTO;
}

bool Meterable(Encoding encoding) {
  return encoding == Encoding::LINEAR_BT709 || encoding == Encoding::SCRGB_NL;
}

std::optional<MaskFormatInfo> DescribeMaskFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:              return MaskFormatInfo{DXGI_FORMAT_R8_UNORM, 1u};
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:             return MaskFormatInfo{DXGI_FORMAT_R16_FLOAT, 2u};
    case DXGI_FORMAT_R16_UNORM:             return MaskFormatInfo{DXGI_FORMAT_R16_UNORM, 2u};
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:             return MaskFormatInfo{DXGI_FORMAT_R32_FLOAT, 4u};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:            return MaskFormatInfo{DXGI_FORMAT_R8G8_UNORM, 2u};
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:          return MaskFormatInfo{DXGI_FORMAT_R16G16_FLOAT, 4u};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return MaskFormatInfo{DXGI_FORMAT_R8G8B8A8_UNORM, 4u};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return MaskFormatInfo{DXGI_FORMAT_B8G8R8A8_UNORM, 4u};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return MaskFormatInfo{DXGI_FORMAT_R10G10B10A2_UNORM, 4u};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return MaskFormatInfo{DXGI_FORMAT_R16G16B16A16_FLOAT, 8u};
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return MaskFormatInfo{DXGI_FORMAT_R32G32B32A32_FLOAT, 16u};
    default:                                return std::nullopt;
  }
}

}  // namespace uplift::color
