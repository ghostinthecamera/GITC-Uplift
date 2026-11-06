#pragma once

#include <array>
#include <cstdint>

#include "color/encoding.hpp"

namespace uplift::color {

// CPU reference of shaders/color_common.hlsli. It is the oracle the GPU tests compare against,
// so every function here has a same-named HLSL twin with the same maths.
struct Rgb {
  float r = 0.f;
  float g = 0.f;
  float b = 0.f;
};

struct Lab {
  float l = 0.f;
  float a = 0.f;
  float b = 0.f;
};

[[nodiscard]] constexpr Rgb operator+(Rgb x, Rgb y) { return {x.r + y.r, x.g + y.g, x.b + y.b}; }
[[nodiscard]] constexpr Rgb operator-(Rgb x, Rgb y) { return {x.r - y.r, x.g - y.g, x.b - y.b}; }
[[nodiscard]] constexpr Rgb operator*(Rgb x, float scale) { return {x.r * scale, x.g * scale, x.b * scale}; }

[[nodiscard]] float Luminance(Rgb linear_bt709);
// Sign-preserving ("extended") sRGB transfer functions.
[[nodiscard]] Rgb SrgbEncode(Rgb linear);
[[nodiscard]] Rgb SrgbDecode(Rgb encoded);
// SMPTE ST 2084, in nits.
[[nodiscard]] Rgb PqToNits(Rgb pq);
[[nodiscard]] Rgb NitsToPq(Rgb nits);
[[nodiscard]] Rgb Bt2020ToBt709(Rgb rgb);
[[nodiscard]] Rgb Bt709ToBt2020(Rgb rgb);
// Luminance Neutwo y/sqrt(y²+1) (RenoDX, MIT), applied by scaling RGB: hue-preserving.
[[nodiscard]] Rgb NeutwoLuminance(Rgb linear);
// Desaturates towards the pixel's own luminance just far enough to land in [0,1]³.
[[nodiscard]] Rgb GamutClip(Rgb linear);
[[nodiscard]] Lab LinearToOkLab(Rgb linear);
[[nodiscard]] Rgb OkLabToLinear(Lab lab);
// Round to FP16 and back, as an RGBA16F texture stores it.
[[nodiscard]] float QuantizeHalf(float value);
[[nodiscard]] Rgb QuantizeHalf(Rgb value);
// Stored value → linear BT.709 with diffuse white at 1.0 (identity for sRGB/AUTO), and back.
// `primaries`: SourcePrimaries.
[[nodiscard]] Rgb ToSceneLinear(Encoding encoding, Rgb value, float input_scale, Primaries primaries = Primaries::AUTO);
[[nodiscard]] Rgb FromSceneLinear(Encoding encoding, Rgb scene, float input_scale, Primaries primaries = Primaries::AUTO);
// The game's exposure on the After-DLSS path, ExposureTexture ÷ DLSS.Pre.Exposure ×
// DLSS.Exposure.Scale, where `factor` is the last two. 1 when missing, non-finite or not positive.
[[nodiscard]] float GameExposure(bool has_texture, float texture_value, float factor);
// Model domain: sRGB-encoded BT.709 in [0,1]. `primaries`: SourcePrimaries.
[[nodiscard]] Rgb EncodeModel(Encoding encoding, Rgb value, float input_scale, Primaries primaries = Primaries::AUTO);
// shaders/change_common.hlsli: NR's change, the compose, the per-pass resolve.
struct ChangeTexel {
  float stops = 0.f;     // a
  float chroma_a = 0.f;  // δ
  float chroma_b = 0.f;
  float guide = 0.f;     // ℓ_M
};
// NR's edit from what it was given to what it returned (both model domain). An unchanged or non-finite output
// gives exactly zero. `near_black_guard` fades δ where either is near black.
[[nodiscard]] ChangeTexel ChangeField(Encoding encoding, Rgb model_in, Rgb model_out, bool near_black_guard);
struct RebuildOptions {
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  bool consistent = false;   // NeuralTransfer = Consistent: HDR paths only
  float chroma_clamp = 0.f;  // stops; 0 = off
};
// One output pixel from its stored `value` and the final change; a zero change returns `value` exactly, and so
// does a non-finite original or result.
[[nodiscard]] Rgb Compose(Encoding encoding, Rgb value, const ChangeTexel& change, float input_scale,
                          const RebuildOptions& options, Primaries primaries = Primaries::AUTO);
// Each pass's own Transfer and Colour strength, from its input to its raw output, in the model domain.
// An unchanged or non-finite output returns `given` exactly.
[[nodiscard]] Rgb ResolveStrengths(Rgb given, Rgb returned, float transfer_strength, float color_strength);

// shaders/meter_cs.hlsl: a log2-luminance histogram with the bin count, lower bound and step below.
inline constexpr uint32_t METER_BINS = 96u;
inline constexpr float METER_LOW_STOPS = -16.f;
inline constexpr float METER_BIN_STOPS = 0.25f;
// The mean of the histogram's 40th-90th percentiles, from bin centres: ignores sky and deep shadow.
[[nodiscard]] float MeterAnchor(const std::array<uint32_t, METER_BINS>& bins);
struct ExposureState {
  float exposure = 0.f;  // E, stops
  float anchor = 0.f;    // the last anchor, for the scene cut
  bool set = false;
};
struct GovernorSettings {
  bool smooth = true;    // ExposureAdapt = Smooth; Off applies the target every frame
  float brighter = 2.f;  // stops per second toward a darker exposure (the scene got brighter)
  float darker = 0.7f;   // stops per second toward a brighter exposure
};
// The governor: snaps to E* = log2(0.18) − anchor (clamped to ±10) on `snap`, the first frame and a scene cut
// (the anchor moved more than 1.5 stops); holds within 0.1 stop; otherwise moves at its rates.
[[nodiscard]] ExposureState GovernExposure(ExposureState previous, float anchor, float frame_seconds, bool snap,
                                           const GovernorSettings& settings);

}  // namespace uplift::color
