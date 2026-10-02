#include "color/reference.hpp"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>

namespace uplift::color {
namespace {

constexpr float Y_RED = 0.2126390059f;
constexpr float Y_GREEN = 0.7151686788f;
constexpr float Y_BLUE = 0.0721923154f;

constexpr float PQ_M1 = 0.1593017578125f;
constexpr float PQ_M2 = 78.84375f;
constexpr float PQ_C1 = 0.8359375f;
constexpr float PQ_C2 = 18.8515625f;
constexpr float PQ_C3 = 18.6875f;

// ITU-R BT.2087 linear-light conversions.
constexpr float BT709_TO_BT2020[3][3] = {
    {0.627403902f, 0.329283020f, 0.043313078f},
    {0.069097304f, 0.919540396f, 0.011362300f},
    {0.016391460f, 0.088013317f, 0.895595223f},
};
constexpr float BT2020_TO_BT709[3][3] = {
    {1.6604910f, -0.5876411f, -0.0728499f},
    {-0.1245505f, 1.1328999f, -0.0083494f},
    {-0.0181508f, -0.1005789f, 1.1187297f},
};

// Luminance or OkLab lightness at or below this is black: no ratio, no chroma transfer.
constexpr float BLACK = 1e-6f;
constexpr float RATIO_MIN = 1.f / 8.f;
constexpr float RATIO_MAX = 8.f;

Rgb Multiply(const float (&matrix)[3][3], Rgb value) {
  return {
      matrix[0][0] * value.r + matrix[0][1] * value.g + matrix[0][2] * value.b,
      matrix[1][0] * value.r + matrix[1][1] * value.g + matrix[1][2] * value.b,
      matrix[2][0] * value.r + matrix[2][1] * value.g + matrix[2][2] * value.b,
  };
}

template <class Function>
Rgb PerChannel(Rgb value, Function function) {
  return {function(value.r), function(value.g), function(value.b)};
}

bool IsFinite(Rgb value) {
  return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b);
}

Rgb Saturate(Rgb value) {
  return PerChannel(value, [](float channel) { return std::clamp(channel, 0.f, 1.f); });
}

// Plan 5: the AP1 (ACEScg, D60) <-> BT.709 (D65) matrices, with a Bradford adaptation.
constexpr float AP1_TO_BT709[3][3] = {
    {1.7050509927f, -0.6217921207f, -0.0832588720f},
    {-0.1302564175f, 1.1408047366f, -0.0105483191f},
    {-0.0240033568f, -0.1289689761f, 1.1529723329f},
};
constexpr float BT709_TO_AP1[3][3] = {
    {0.6130974024f, 0.3395231462f, 0.0473794514f},
    {0.0701937225f, 0.9163538791f, 0.0134523985f},
    {0.0206155929f, 0.1095697729f, 0.8698146342f},
};
constexpr float CHANGE_LIMIT = 3.f;
constexpr float LIGHTNESS_FLOOR = 0.0625f;
constexpr float SDR_EPSILON = 1.f / 4096.f;
constexpr float HDR_EPSILON = 1.f / 16777216.f;
constexpr float CONSISTENT_GAIN_LIMIT = 4.f;  // 2 stops where Neutwo is nearly flat
constexpr float CLAMP_CHANNEL_FLOOR = 0.05f;  // channels below 5 % of the luminance are not clamped

// `resolved`: ResolvePrimaries' result, never AUTO.
Rgb ToBt709(Primaries resolved, Rgb rgb) {
  switch (resolved) {
    case Primaries::BT2020: return Multiply(BT2020_TO_BT709, rgb);
    case Primaries::AP1:    return Multiply(AP1_TO_BT709, rgb);
    default:                return rgb;
  }
}

Rgb FromBt709(Primaries resolved, Rgb rgb) {
  switch (resolved) {
    case Primaries::BT2020: return Multiply(BT709_TO_BT2020, rgb);
    case Primaries::AP1:    return Multiply(BT709_TO_AP1, rgb);
    default:                return rgb;
  }
}

float ChangeEpsilon(Encoding encoding) {
  return (IsSdr(encoding) ? SDR_EPSILON : HDR_EPSILON);
}

// v2 design §3.7's rebuild with the F15 Consistent gain and the F14 chroma clamp (change_common.hlsli's Rebuild).
Rgb Rebuild(Encoding encoding, Rgb original, const ChangeTexel& change, const RebuildOptions& options) {
  const float epsilon = ChangeEpsilon(encoding);
  const float luminance = std::max(Luminance(original), 0.f);
  const float gain =
      (options.consistent && !IsSdr(encoding) ? std::min(1.f + luminance * luminance, CONSISTENT_GAIN_LIMIT) : 1.f);
  const float target = std::max((luminance + epsilon) * std::exp2(options.transfer_strength * (gain * change.stops)) - epsilon, 0.f);
  const Lab lab = LinearToOkLab(original);
  const float lightness = (luminance > 0.f ? lab.l * std::cbrt(target / luminance) : std::cbrt(target));
  const float scale = std::max(lightness, LIGHTNESS_FLOOR);
  const float chroma_a = lab.a / std::max(lab.l, LIGHTNESS_FLOOR) + options.color_strength * change.chroma_a;
  const float chroma_b = lab.b / std::max(lab.l, LIGHTNESS_FLOOR) + options.color_strength * change.chroma_b;
  Rgb rebuilt = OkLabToLinear({lightness, chroma_a * scale, chroma_b * scale});
  const float rebuilt_luminance = Luminance(rebuilt);
  if (rebuilt_luminance > 0.f) {
    rebuilt = rebuilt * (target / rebuilt_luminance);
  }
  if (options.chroma_clamp > 0.f && luminance > 0.f) {
    const float low = target / luminance * std::exp2(-options.chroma_clamp);
    const float high = target / luminance * std::exp2(options.chroma_clamp);
    const auto clamp_channel = [&](float out, float in) {
      return (in > CLAMP_CHANNEL_FLOOR * luminance ? std::clamp(out, in * low, in * high) : out);
    };
    rebuilt = {clamp_channel(rebuilt.r, original.r), clamp_channel(rebuilt.g, original.g), clamp_channel(rebuilt.b, original.b)};
    const float clamped_luminance = Luminance(rebuilt);
    if (clamped_luminance > 0.f) {
      rebuilt = rebuilt * (target / clamped_luminance);
    }
  }
  return rebuilt;
}

}  // namespace

float Luminance(Rgb linear_bt709) {
  return Y_RED * linear_bt709.r + Y_GREEN * linear_bt709.g + Y_BLUE * linear_bt709.b;
}

Rgb SrgbEncode(Rgb linear) {
  return PerChannel(linear, [](float channel) {
    const float magnitude = std::abs(channel);
    const float encoded = (magnitude <= 0.0031308f ? 12.92f * magnitude : 1.055f * std::pow(magnitude, 1.f / 2.4f) - 0.055f);
    return std::copysign(encoded, channel);
  });
}

Rgb SrgbDecode(Rgb encoded) {
  return PerChannel(encoded, [](float channel) {
    const float magnitude = std::abs(channel);
    const float decoded = (magnitude <= 0.04045f ? magnitude / 12.92f : std::pow((magnitude + 0.055f) / 1.055f, 2.4f));
    return std::copysign(decoded, channel);
  });
}

Rgb PqToNits(Rgb pq) {
  return PerChannel(pq, [](float channel) {
    const float power = std::pow(std::clamp(channel, 0.f, 1.f), 1.f / PQ_M2);
    const float numerator = std::max(power - PQ_C1, 0.f);
    return 10000.f * std::pow(numerator / (PQ_C2 - PQ_C3 * power), 1.f / PQ_M1);
  });
}

Rgb NitsToPq(Rgb nits) {
  return PerChannel(nits, [](float channel) {
    const float power = std::pow(std::max(channel, 0.f) / 10000.f, PQ_M1);
    return std::pow((PQ_C1 + PQ_C2 * power) / (1.f + PQ_C3 * power), PQ_M2);
  });
}

Rgb Bt2020ToBt709(Rgb rgb) {
  return Multiply(BT2020_TO_BT709, rgb);
}

Rgb Bt709ToBt2020(Rgb rgb) {
  return Multiply(BT709_TO_BT2020, rgb);
}

Rgb NeutwoLuminance(Rgb linear) {
  const float luminance = Luminance(linear);
  return linear * (1.f / std::sqrt(luminance * luminance + 1.f));
}

Rgb GamutClip(Rgb linear) {
  const float luminance = Luminance(linear);
  if (luminance <= 0.f) return {};
  if (luminance >= 1.f) return {1.f, 1.f, 1.f};
  float keep = 1.f;
  for (const float channel : {linear.r, linear.g, linear.b}) {
    if (channel > 1.f) {
      keep = std::min(keep, (1.f - luminance) / (channel - luminance));
    } else if (channel < 0.f) {
      keep = std::min(keep, luminance / (luminance - channel));
    }
  }
  const Rgb grey = {luminance, luminance, luminance};
  return Saturate(grey + (linear - grey) * keep);
}

// Björn Ottosson's OkLab (https://bottosson.github.io/posts/oklab/, MIT). std::cbrt keeps the
// sign, so out-of-gamut colours with negative LMS survive a round trip.
Lab LinearToOkLab(Rgb linear) {
  const float l = std::cbrt(0.4122214708f * linear.r + 0.5363325363f * linear.g + 0.0514459929f * linear.b);
  const float m = std::cbrt(0.2119034982f * linear.r + 0.6806995451f * linear.g + 0.1073969566f * linear.b);
  const float s = std::cbrt(0.0883024619f * linear.r + 0.2817188376f * linear.g + 0.6299787005f * linear.b);
  return {
      0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
      1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
      0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
  };
}

Rgb OkLabToLinear(Lab lab) {
  const float l = lab.l + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
  const float m = lab.l - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
  const float s = lab.l - 0.0894841775f * lab.a - 1.2914855480f * lab.b;
  const float l3 = l * l * l;
  const float m3 = m * m * m;
  const float s3 = s * s * s;
  return {
      4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3,
      -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3,
      -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3,
  };
}

float QuantizeHalf(float value) {
  return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(value));
}

Rgb QuantizeHalf(Rgb value) {
  return PerChannel(value, [](float channel) { return QuantizeHalf(channel); });
}

Rgb ToSceneLinear(Encoding encoding, Rgb value, float input_scale, Primaries primaries) {
  const Primaries resolved = ResolvePrimaries(primaries, encoding);
  switch (encoding) {
    case Encoding::LINEAR_BT709:
    case Encoding::SCRGB:     return ToBt709(resolved, value) * input_scale;
    case Encoding::SCRGB_NL:  return ToBt709(resolved, SrgbDecode(value)) * input_scale;
    case Encoding::PQ_BT2100: return ToBt709(resolved, PqToNits(value)) * input_scale;
    case Encoding::AUTO:
    case Encoding::SRGB:      return value;
  }
  return value;
}

Rgb FromSceneLinear(Encoding encoding, Rgb scene, float input_scale, Primaries primaries) {
  const Primaries resolved = ResolvePrimaries(primaries, encoding);
  const Rgb unscaled = FromBt709(resolved, scene * (1.f / input_scale));
  switch (encoding) {
    case Encoding::LINEAR_BT709:
    case Encoding::SCRGB:     return unscaled;
    case Encoding::SCRGB_NL:  return SrgbEncode(unscaled);
    case Encoding::PQ_BT2100: return NitsToPq(unscaled);
    case Encoding::AUTO:
    case Encoding::SRGB:      return scene;
  }
  return scene;
}

float GameExposure(bool has_texture, float texture_value, float factor) {
  const float exposure = (has_texture ? texture_value : 1.f) * factor;
  return (std::isfinite(exposure) && exposure > 0.f ? exposure : 1.f);
}

Rgb EncodeModel(Encoding encoding, Rgb value, float input_scale, Primaries primaries) {
  if (encoding == Encoding::SRGB || encoding == Encoding::AUTO) return Saturate(value);
  Rgb scene = ToSceneLinear(encoding, value, input_scale, primaries);
  if (!IsFinite(scene)) {
    scene = {};
  }
  return SrgbEncode(GamutClip(NeutwoLuminance(scene)));
}

Rgb DecodeModel(Encoding encoding, Rgb value, Rgb nr_output, float input_scale, float transfer_strength,
                float color_strength, Primaries primaries) {
  if (encoding == Encoding::SRGB || encoding == Encoding::AUTO) return (IsFinite(nr_output) ? nr_output : value);
  const Rgb original = ToSceneLinear(encoding, value, input_scale, primaries);
  if (!IsFinite(original)) return value;
  // A may have been overwritten by pass 2, so recompute what NR was given, rounded as A stored it.
  const Rgb linear_in = SrgbDecode(QuantizeHalf(EncodeModel(encoding, value, input_scale, primaries)));
  const Rgb linear_nr = SrgbDecode(nr_output);
  const float luminance_in = Luminance(linear_in);
  const float ratio = (luminance_in > BLACK ? std::clamp(Luminance(linear_nr) / luminance_in, RATIO_MIN, RATIO_MAX) : 1.f);
  const Rgb scaled = original * (1.f + (ratio - 1.f) * transfer_strength);
  Rgb restored = scaled;
  const Lab lab_scaled = LinearToOkLab(scaled);
  const Lab lab_in = LinearToOkLab(linear_in);
  const Lab lab_nr = LinearToOkLab(linear_nr);
  if (color_strength > 0.f && lab_scaled.l > BLACK && lab_in.l > BLACK && lab_nr.l > BLACK) {
    const float a = lab_scaled.a / lab_scaled.l + color_strength * (lab_nr.a / lab_nr.l - lab_in.a / lab_in.l);
    const float b = lab_scaled.b / lab_scaled.l + color_strength * (lab_nr.b / lab_nr.l - lab_in.b / lab_in.l);
    restored = OkLabToLinear({lab_scaled.l, a * lab_scaled.l, b * lab_scaled.l});
    const float luminance_restored = Luminance(restored);
    const float luminance_target = Luminance(scaled);
    if (luminance_restored > BLACK && luminance_target > BLACK) {
      restored = restored * (luminance_target / luminance_restored);
    }
  }
  const Rgb decoded = FromSceneLinear(encoding, restored, input_scale, primaries);
  return (IsFinite(decoded) ? decoded : value);
}

ChangeTexel ChangeField(Encoding encoding, Rgb model_in, Rgb model_out, bool near_black_guard) {
  const float epsilon = ChangeEpsilon(encoding);
  const Rgb linear_in = SrgbDecode(model_in);
  const float luminance_in = std::max(Luminance(linear_in), 0.f);
  const float guide = std::log2(luminance_in + epsilon);
  const bool unchanged = (model_out.r == model_in.r && model_out.g == model_in.g && model_out.b == model_in.b);
  if (!IsFinite(model_out) || unchanged) return {.guide = guide};
  const Rgb linear_out = SrgbDecode(model_out);
  const float stops = std::clamp(std::log2((std::max(Luminance(linear_out), 0.f) + epsilon) / (luminance_in + epsilon)),
                                 -CHANGE_LIMIT, CHANGE_LIMIT);
  const Lab lab_in = LinearToOkLab(linear_in);
  const Lab lab_out = LinearToOkLab(linear_out);
  float guard = 1.f;
  if (near_black_guard) {
    const float t = std::clamp((std::min(lab_out.l, lab_in.l) - LIGHTNESS_FLOOR) / LIGHTNESS_FLOOR, 0.f, 1.f);
    guard = t * t * (3.f - 2.f * t);  // smoothstep(L_ε, 2·L_ε, min(L_N, L_M))
  }
  return {
      .stops = stops,
      .chroma_a = guard * (lab_out.a / std::max(lab_out.l, LIGHTNESS_FLOOR) - lab_in.a / std::max(lab_in.l, LIGHTNESS_FLOOR)),
      .chroma_b = guard * (lab_out.b / std::max(lab_out.l, LIGHTNESS_FLOOR) - lab_in.b / std::max(lab_in.l, LIGHTNESS_FLOOR)),
      .guide = guide,
  };
}

Rgb Compose(Encoding encoding, Rgb value, const ChangeTexel& change, float input_scale, const RebuildOptions& options,
            Primaries primaries) {
  if (change.stops == 0.f && change.chroma_a == 0.f && change.chroma_b == 0.f) return value;
  const Rgb original = (IsSdr(encoding) ? SrgbDecode(Saturate(value)) : ToSceneLinear(encoding, value, input_scale, primaries));
  const bool finite_change = std::isfinite(change.stops) && std::isfinite(change.chroma_a) && std::isfinite(change.chroma_b);
  if (!IsFinite(original) || !finite_change) return value;
  const Rgb rebuilt = Rebuild(encoding, original, change, options);
  const Rgb stored = (IsSdr(encoding) ? SrgbEncode(GamutClip(rebuilt)) : FromSceneLinear(encoding, rebuilt, input_scale, primaries));
  return (IsFinite(stored) ? stored : value);
}

Rgb ResolveStrengths(Rgb given, Rgb returned, float transfer_strength, float color_strength) {
  const bool unchanged = (returned.r == given.r && returned.g == given.g && returned.b == given.b);
  if (!IsFinite(returned) || unchanged) return given;
  const Rgb linear_given = SrgbDecode(given);
  const Rgb linear_returned = SrgbDecode(returned);
  const float luminance_given = std::max(Luminance(linear_given), 0.f);
  const float ratio = (luminance_given > BLACK ? std::clamp(std::max(Luminance(linear_returned), 0.f) / luminance_given, RATIO_MIN, RATIO_MAX)
                                               : 1.f);
  const float target = luminance_given * (1.f + (ratio - 1.f) * transfer_strength);
  const Lab lab_given = LinearToOkLab(linear_given);
  const Lab lab_returned = LinearToOkLab(linear_returned);
  const float lightness = (luminance_given > 0.f ? lab_given.l * std::cbrt(target / luminance_given) : std::cbrt(target));
  const float floor_given = std::max(lab_given.l, LIGHTNESS_FLOOR);
  const float floor_returned = std::max(lab_returned.l, LIGHTNESS_FLOOR);
  const float chroma_a = lab_given.a / floor_given + color_strength * (lab_returned.a / floor_returned - lab_given.a / floor_given);
  const float chroma_b = lab_given.b / floor_given + color_strength * (lab_returned.b / floor_returned - lab_given.b / floor_given);
  const float scale = std::max(lightness, LIGHTNESS_FLOOR);
  Rgb rebuilt = OkLabToLinear({lightness, chroma_a * scale, chroma_b * scale});
  const float rebuilt_luminance = Luminance(rebuilt);
  if (rebuilt_luminance > 0.f) {
    rebuilt = rebuilt * (target / rebuilt_luminance);
  }
  return SrgbEncode(GamutClip(rebuilt));
}

float MeterAnchor(const std::array<uint32_t, METER_BINS>& bins) {
  uint32_t total = 0u;
  for (const uint32_t count : bins) {
    total += count;
  }
  if (total == 0u) return std::log2(0.18f);  // nothing metered: E* = 0
  const float low = 0.4f * static_cast<float>(total);
  const float high = 0.9f * static_cast<float>(total);
  float below = 0.f;
  float sum = 0.f;
  for (uint32_t bin = 0u; bin < METER_BINS; ++bin) {
    const auto count = static_cast<float>(bins[bin]);
    sum += std::max(std::min(below + count, high) - std::max(below, low), 0.f)
           * (METER_LOW_STOPS + (static_cast<float>(bin) + 0.5f) * METER_BIN_STOPS);
    below += count;
  }
  return sum / (high - low);
}

ExposureState GovernExposure(ExposureState previous, float anchor, float frame_seconds, bool snap, const GovernorSettings& settings) {
  constexpr float TARGET_LIMIT = 10.f;
  constexpr float DEADBAND = 0.1f;
  constexpr float SCENE_CUT = 1.5f;
  const float target = std::clamp(std::log2(0.18f) - anchor, -TARGET_LIMIT, TARGET_LIMIT);
  float exposure = previous.exposure;
  if (snap || !previous.set || !settings.smooth || std::abs(anchor - previous.anchor) > SCENE_CUT) {
    exposure = target;
  } else if (std::abs(target - exposure) > DEADBAND) {
    const float seconds = std::clamp(frame_seconds, 0.001f, 0.1f);
    exposure += std::clamp(target - exposure, -settings.brighter * seconds, settings.darker * seconds);
  }
  return {.exposure = exposure, .anchor = anchor, .set = true};
}

}  // namespace uplift::color
