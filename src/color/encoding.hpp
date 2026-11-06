#pragma once

#include <dxgiformat.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace uplift::color {

// The `Encoding` setting; the order matches the RenoDX host's combo.
enum class Encoding : uint32_t {
  AUTO = 0u,
  LINEAR_BT709 = 1u,
  SRGB = 2u,
  PQ_BT2100 = 3u,
  SCRGB = 4u,
  SCRGB_NL = 5u,
};

// How NR's change returns to the output size below Full; the Upsampling setting's index.
enum class Upsampling : uint32_t {
  CLASSIC = 0u,     // a bilinear sample
  EDGE_AWARE = 1u,  // a joint bilateral upsample guided by the original
};

// SourcePrimaries values are the shader primaries bits (color_common.hlsli's PRIMARIES_*).
// AUTO resolves to BT.2020 for PQ and BT.709 otherwise.
enum class Primaries : uint32_t {
  AUTO = 0u,
  BT709 = 1u,
  BT2020 = 2u,
  AP1 = 3u,  // ACEScg
};

// The InputExposure index.
enum class InputExposure : uint32_t {
  AUTO = 0u,     // Game exposure, blended or switched by AutoExposureMode; metered alone when absent or broken
  GAME = 1u,     // DLSS's exposure (none at Present)
  METERED = 2u,  // Uplift's own meter and governor
  MANUAL = 3u,   // DiffuseWhiteNits alone
};

// The AutoExposureMode index, how Auto uses a valid game exposure. Either falls back to the meter
// alone once the game's is found broken.
enum class AutoExposureMode : uint32_t {
  BLEND = 0u,   // the game's and the meter's, blended in stops by AutoExposureBlend
  SWITCH = 1u,  // the game's exposure until it is found broken
};

// The NeuralTransfer index.
enum class NeuralTransfer : uint32_t {
  BOUNDED_RATIO = 0u,
  CONSISTENT = 1u,  // undoes Neutwo's compression, capped at 4x
};

// Mirrors reshade::api::color_space; the add-on static_asserts that the values match.
enum class ColorSpace : uint32_t {
  UNKNOWN = 0u,
  SRGB_NONLINEAR = 1u,
  EXTENDED_SRGB_LINEAR = 2u,
  HDR10_ST2084 = 3u,
  HDR10_HLG = 4u,
};

struct FormatInfo {
  DXGI_FORMAT copy_format = DXGI_FORMAT_UNKNOWN;         // typeless family: the source copy is created in it
  DXGI_FORMAT source_view_format = DXGI_FORMAT_UNKNOWN;  // SRV format that reads the stored values unchanged
  DXGI_FORMAT target_view_format = DXGI_FORMAT_UNKNOWN;  // RTV format decode writes the target through
  bool target_view_srgb = false;                         // the RTV sRGB-encodes, so the shader writes linear values
  uint32_t bytes_per_pixel = 0u;
};

// nullopt for back-buffer formats Uplift does not process.
[[nodiscard]] std::optional<FormatInfo> DescribeFormat(DXGI_FORMAT format);

// A DLSS Output the After-DLSS decode writes through a UAV.
struct UavFormatInfo {
  DXGI_FORMAT copy_format = DXGI_FORMAT_UNKNOWN;         // the source copy's format (its typeless family where one exists)
  DXGI_FORMAT source_view_format = DXGI_FORMAT_UNKNOWN;  // SRV that reads the stored values unchanged
  DXGI_FORMAT uav_format = DXGI_FORMAT_UNKNOWN;          // UAV the decode writes through; never sRGB-typed
  uint32_t bytes_per_pixel = 0u;
};

// nullopt for formats with no UAV (sRGB-typed ones) or that Uplift does not process. Whether the
// device can store to `uav_format` is ColorPipeline::SupportsTypedUavStore's question.
[[nodiscard]] std::optional<UavFormatInfo> DescribeUavFormat(DXGI_FORMAT format);
// AUTO follows the swap chain's colour space; with an unknown colour space FP16 means scRGB and
// anything else sRGB. nullopt when the colour space is not supported (HDR10 HLG).
[[nodiscard]] std::optional<Encoding> ResolveEncoding(Encoding setting, ColorSpace color_space, DXGI_FORMAT format);
// Automatic diffuse white for each encoding, in nits; sRGB has no diffuse-white scaling.
[[nodiscard]] float DefaultDiffuseWhiteNits(Encoding encoding);
// Factor that puts `diffuse_white_nits` at 1.0: scRGB and scRGB-nl unit/DW (unit 80 nits), linear BT.709 unit/DW
// (unit 100 nits), PQ 1/DW on decoded nits, 1 for sRGB. `linear_unit_nits` (LinearUnitNits): 0 = automatic.
[[nodiscard]] float InputScale(Encoding encoding, float diffuse_white_nits, float linear_unit_nits = 0.f);
[[nodiscard]] std::string_view EncodingName(Encoding encoding);

// `setting` with AUTO resolved for an image of `encoding`: BT.2020 for PQ, else BT.709.
[[nodiscard]] Primaries ResolvePrimaries(Primaries setting, Encoding encoding);
// sRGB or Auto: display-referred, so no primaries, no Neutwo and never metered.
[[nodiscard]] bool IsSdr(Encoding encoding);
// Only relative scene-linear images are metered (Linear BT.709, scRGB-nl); sRGB, scRGB and PQ never.
[[nodiscard]] bool Meterable(Encoding encoding);

// A format from UPLIFT_MASK, read through a typed red-channel view.
struct MaskFormatInfo {
  DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
  uint32_t bytes_per_pixel = 0u;
};
[[nodiscard]] std::optional<MaskFormatInfo> DescribeMaskFormat(DXGI_FORMAT format);

}  // namespace uplift::color
