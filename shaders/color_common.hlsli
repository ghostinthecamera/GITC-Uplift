#ifndef UPLIFT_COLOR_COMMON_HLSLI
#define UPLIFT_COLOR_COMMON_HLSLI

// GPU twin of src/color/reference.cpp: same functions, same maths. tests/gpu/color_gpu_test.cpp
// compares the two, so change both together.

static const uint ENCODING_AUTO = 0u;
static const uint ENCODING_LINEAR_BT709 = 1u;
static const uint ENCODING_SRGB = 2u;
static const uint ENCODING_PQ_BT2100 = 3u;
static const uint ENCODING_SCRGB = 4u;
static const uint ENCODING_SCRGB_NL = 5u;

// Plan 5 (v2 design §3.14, F7): the source's primaries, the SourcePrimaries index (color::Primaries). AUTO resolves
// per encoding (ResolvePrimaries), so options of 0 keep Plan 4's maths.
static const uint PRIMARIES_AUTO = 0u;
static const uint PRIMARIES_BT709 = 1u;
static const uint PRIMARIES_BT2020 = 2u;
static const uint PRIMARIES_AP1 = 3u;

static const float3 BT709_LUMINANCE = float3(0.2126390059f, 0.7151686788f, 0.0721923154f);
static const float PQ_M1 = 0.1593017578125f;
static const float PQ_M2 = 78.84375f;
static const float PQ_C1 = 0.8359375f;
static const float PQ_C2 = 18.8515625f;
static const float PQ_C3 = 18.6875f;
static const float BLACK = 1e-6f;
static const float RATIO_MIN = 0.125f;
static const float RATIO_MAX = 8.f;

// ITU-R BT.2087, row by row, used as mul(matrix, column).
static const float3x3 BT709_TO_BT2020 = float3x3(
    0.627403902f, 0.329283020f, 0.043313078f,
    0.069097304f, 0.919540396f, 0.011362300f,
    0.016391460f, 0.088013317f, 0.895595223f);
static const float3x3 BT2020_TO_BT709 = float3x3(
    1.6604910f, -0.5876411f, -0.0728499f,
    -0.1245505f, 1.1328999f, -0.0083494f,
    -0.0181508f, -0.1005789f, 1.1187297f);

// ACES AP1 (ACEScg, D60) and BT.709 (D65), with a Bradford adaptation.
static const float3x3 AP1_TO_BT709 = float3x3(
    1.7050509927f, -0.6217921207f, -0.0832588720f,
    -0.1302564175f, 1.1408047366f, -0.0105483191f,
    -0.0240033568f, -0.1289689761f, 1.1529723329f);
static const float3x3 BT709_TO_AP1 = float3x3(
    0.6130974024f, 0.3395231462f, 0.0473794514f,
    0.0701937225f, 0.9163538791f, 0.0134523985f,
    0.0206155929f, 0.1095697729f, 0.8698146342f);

// AUTO is BT.2020 for PQ (Plan 4's rule) and BT.709 otherwise (twin: color::ResolvePrimaries).
uint ResolvePrimaries(uint encoding, uint primaries) {
  if (primaries != PRIMARIES_AUTO) return primaries;
  return (encoding == ENCODING_PQ_BT2100 ? PRIMARIES_BT2020 : PRIMARIES_BT709);
}

float3 ToBt709(uint resolved, float3 rgb) {
  if (resolved == PRIMARIES_BT2020) return mul(BT2020_TO_BT709, rgb);
  if (resolved == PRIMARIES_AP1) return mul(AP1_TO_BT709, rgb);
  return rgb;
}

float3 FromBt709(uint resolved, float3 rgb) {
  if (resolved == PRIMARIES_BT2020) return mul(BT709_TO_BT2020, rgb);
  if (resolved == PRIMARIES_AP1) return mul(BT709_TO_AP1, rgb);
  return rgb;
}

float Luminance(float3 rgb) {
  return dot(rgb, BT709_LUMINANCE);
}

float3 SrgbEncode(float3 rgb) {
  const float3 magnitude = abs(rgb);
  const float3 encoded = select(magnitude <= 0.0031308f, 12.92f * magnitude, 1.055f * pow(magnitude, 1.f / 2.4f) - 0.055f);
  return float3(sign(rgb)) * encoded;
}

float3 SrgbDecode(float3 encoded) {
  const float3 magnitude = abs(encoded);
  const float3 decoded = select(magnitude <= 0.04045f, magnitude / 12.92f, pow((magnitude + 0.055f) / 1.055f, 2.4f));
  return float3(sign(encoded)) * decoded;
}

float3 PqToNits(float3 pq) {
  const float3 power = pow(saturate(pq), 1.f / PQ_M2);
  const float3 numerator = max(power - PQ_C1, 0.f);
  return 10000.f * pow(numerator / (PQ_C2 - PQ_C3 * power), 1.f / PQ_M1);
}

float3 NitsToPq(float3 nits) {
  const float3 power = pow(max(nits, 0.f) / 10000.f, PQ_M1);
  return pow((PQ_C1 + PQ_C2 * power) / (1.f + PQ_C3 * power), PQ_M2);
}

// Luminance Neutwo y/sqrt(y²+1) (RenoDX, MIT), applied by scaling RGB: hue-preserving.
float3 NeutwoLuminance(float3 rgb) {
  const float luminance = Luminance(rgb);
  return rgb * rsqrt(luminance * luminance + 1.f);
}

// Desaturates towards the pixel's own luminance just far enough to land in [0,1]³.
float3 GamutClip(float3 rgb) {
  const float luminance = Luminance(rgb);
  if (luminance <= 0.f) return float3(0.f, 0.f, 0.f);
  if (luminance >= 1.f) return float3(1.f, 1.f, 1.f);
  float keep = 1.f;
  [unroll] for (uint channel = 0u; channel < 3u; ++channel) {
    const float value = rgb[channel];
    if (value > 1.f) {
      keep = min(keep, (1.f - luminance) / (value - luminance));
    } else if (value < 0.f) {
      keep = min(keep, luminance / (luminance - value));
    }
  }
  return saturate(luminance + (rgb - luminance) * keep);
}

// Björn Ottosson's OkLab (MIT). The signed cube root keeps out-of-gamut colours reversible.
float3 LinearToOkLab(float3 rgb) {
  const float3 lms = float3(
      dot(rgb, float3(0.4122214708f, 0.5363325363f, 0.0514459929f)),
      dot(rgb, float3(0.2119034982f, 0.6806995451f, 0.1073969566f)),
      dot(rgb, float3(0.0883024619f, 0.2817188376f, 0.6299787005f)));
  const float3 root = float3(sign(lms)) * pow(abs(lms), 1.f / 3.f);
  return float3(
      dot(root, float3(0.2104542553f, 0.7936177850f, -0.0040720468f)),
      dot(root, float3(1.9779984951f, -2.4285922050f, 0.4505937099f)),
      dot(root, float3(0.0259040371f, 0.7827717662f, -0.8086757660f)));
}

float3 OkLabToLinear(float3 lab) {
  const float3 root = float3(
      lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z,
      lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z,
      lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z);
  const float3 lms = root * root * root;
  return float3(
      dot(lms, float3(4.0767416621f, -3.3077115913f, 0.2309699292f)),
      dot(lms, float3(-1.2684380046f, 2.6097574011f, -0.3413193965f)),
      dot(lms, float3(-0.0041960863f, -0.7034186147f, 1.7076147010f)));
}

// Plan 14 (R94): the model values NR is given round as Direct3D 12's do: toward zero. gpu_vk_look measured half of the model values one half above Direct3D
// 12's when the SPIR-V build used f16tof32(f32tof16()) too (the driver may fold that pair, and its RGBA16F store rounds to nearest), so the SPIR-V build
// quantizes with integer masks, which nothing can fold: the low 13 of float32's 23 mantissa bits go in half's normal range, a whole number of 2^-24 below
// it, and a finite value above the largest half becomes the largest half.
float3 QuantizeHalf(float3 value) {
#if defined(__spirv__)
  const float3 magnitude = abs(value);
  const float3 normal = asfloat(asuint(magnitude) & 0xFFFFE000u);
  const float3 subnormal = floor(magnitude * 16777216.f) / 16777216.f;
  const float3 quantized = select(magnitude < 6.103515625e-5f, subnormal, min(normal, 65504.f));
  return select(isfinite(value), asfloat(asuint(quantized) | (asuint(value) & 0x80000000u)), value);
#else
  return f16tof32(f32tof16(value));
#endif
}

float3 ToSceneLinear(uint encoding, uint primaries, float3 value, float input_scale) {
  const uint resolved = ResolvePrimaries(encoding, primaries);
  if (encoding == ENCODING_LINEAR_BT709 || encoding == ENCODING_SCRGB) return ToBt709(resolved, value) * input_scale;
  if (encoding == ENCODING_SCRGB_NL) return ToBt709(resolved, SrgbDecode(value)) * input_scale;
  if (encoding == ENCODING_PQ_BT2100) return ToBt709(resolved, PqToNits(value)) * input_scale;
  return value;
}

float3 FromSceneLinear(uint encoding, uint primaries, float3 scene, float input_scale) {
  const float3 unscaled = FromBt709(ResolvePrimaries(encoding, primaries), scene * (1.f / input_scale));
  if (encoding == ENCODING_LINEAR_BT709 || encoding == ENCODING_SCRGB) return unscaled;
  if (encoding == ENCODING_SCRGB_NL) return SrgbEncode(unscaled);
  if (encoding == ENCODING_PQ_BT2100) return NitsToPq(unscaled);
  return scene;
}

// v2 design §3.4: the game's exposure on the After-DLSS path, ExposureTexture ÷ DLSS.Pre.Exposure ×
// DLSS.Exposure.Scale, where `factor` is the last two. 1 when missing, non-finite or not positive.
float GameExposure(uint has_texture, float texture_value, float factor) {
  const float exposure = (has_texture != 0u ? texture_value : 1.f) * factor;
  return (isfinite(exposure) && exposure > 0.f ? exposure : 1.f);
}

// Model domain (spike E3): sRGB-encoded BT.709 in [0,1].
float3 EncodeModel(uint encoding, uint primaries, float3 value, float input_scale) {
  if (encoding == ENCODING_AUTO || encoding == ENCODING_SRGB) return saturate(value);
  float3 scene = ToSceneLinear(encoding, primaries, value, input_scale);
  if (!all(isfinite(scene))) {
    scene = float3(0.f, 0.f, 0.f);
  }
  return SrgbEncode(GamutClip(NeutwoLuminance(scene)));
}

// Spec §7 restore (Plan 2 amendment 6).
float3 DecodeModel(uint encoding, uint primaries, float3 value, float3 nr_output, float input_scale, float transfer_strength,
                   float color_strength) {
  // Plan 2 final review M2: a non-finite result never reaches the game's image; the source value does.
  if (encoding == ENCODING_AUTO || encoding == ENCODING_SRGB) return (all(isfinite(nr_output)) ? nr_output : value);
  const float3 original = ToSceneLinear(encoding, primaries, value, input_scale);
  if (!all(isfinite(original))) return value;
  // A may have been overwritten by pass 2: recompute what NR was given, rounded as A stored it.
  // `precise` keeps this bit-identical to encode_cs.hlsl's computation of the same expression.
  precise float3 recomputed_model = QuantizeHalf(EncodeModel(encoding, primaries, value, input_scale));
  const float3 linear_in = SrgbDecode(recomputed_model);
  const float3 linear_nr = SrgbDecode(nr_output);
  const float luminance_in = Luminance(linear_in);
  const float ratio = (luminance_in > BLACK ? clamp(Luminance(linear_nr) / luminance_in, RATIO_MIN, RATIO_MAX) : 1.f);
  const float3 scaled = original * (1.f + (ratio - 1.f) * transfer_strength);
  float3 restored = scaled;
  const float3 lab_scaled = LinearToOkLab(scaled);
  const float3 lab_in = LinearToOkLab(linear_in);
  const float3 lab_nr = LinearToOkLab(linear_nr);
  if (color_strength > 0.f && lab_scaled.x > BLACK && lab_in.x > BLACK && lab_nr.x > BLACK) {
    const float2 chroma = lab_scaled.yz / lab_scaled.x + color_strength * (lab_nr.yz / lab_nr.x - lab_in.yz / lab_in.x);
    restored = OkLabToLinear(float3(lab_scaled.x, chroma * lab_scaled.x));
    const float luminance_restored = Luminance(restored);
    const float luminance_target = Luminance(scaled);
    if (luminance_restored > BLACK && luminance_target > BLACK) {
      restored *= luminance_target / luminance_restored;
    }
  }
  const float3 decoded = FromSceneLinear(encoding, primaries, restored, input_scale);
  return (all(isfinite(decoded)) ? decoded : value);
}

#endif  // UPLIFT_COLOR_COMMON_HLSLI
