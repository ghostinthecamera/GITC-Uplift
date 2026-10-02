#ifndef UPLIFT_CHANGE_COMMON_HLSLI
#define UPLIFT_CHANGE_COMMON_HLSLI

#include "color_common.hlsli"

// Plan 4/5's GPU maths (v2 design §3.7-§3.10, §3.14): resampling, NR's change field, the compose and the
// per-pass resolve. CPU twins: color::ChangeField, color::Compose (with its Rebuild) and color::ResolveStrengths
// (src/color/reference.cpp); tests/gpu/color_gpu_test.cpp and tests/gpu/look_gpu_test.cpp compare them.

static const float CHANGE_LIMIT = 3.f;              // ±3 stops: the restore's ratio clamp of [1/8, 8]
static const float LIGHTNESS_FLOOR = 0.0625f;       // L_ε: κ divides by max(L, L_ε)
static const float SDR_EPSILON = 1.f / 4096.f;      // 2^-12: NR's near-black edits stay additive on SDR
static const float HDR_EPSILON = 1.f / 16777216.f;  // 2^-24: a pure ratio on HDR, so NR cannot lift black
static const float CONSISTENT_GAIN_LIMIT = 4.f;  // F15: 2 stops where Neutwo is nearly flat
static const float CLAMP_CHANNEL_FLOOR = 0.05f;  // F14: channels below 5 % of the luminance are not clamped

// The `options` bits of the encode and decode constants (color::shader_options).
static const uint OPTION_PRIMARIES_MASK = 3u;    // bits 0-1: PRIMARIES_* (0 = automatic)
static const uint OPTION_NEAR_BLACK_GUARD = 4u;  // F16: the change pass fades NR's colour change near black
static const uint OPTION_CONSISTENT = 8u;        // F15: the rebuild undoes Neutwo's compression
static const uint OPTION_MASK = 16u;             // §3.13: the compose multiplies the change by the NR mask

bool IsSdr(uint encoding) {
  return encoding == ENCODING_AUTO || encoding == ENCODING_SRGB;
}

float ChangeEpsilon(uint encoding) {
  return (IsSdr(encoding) ? SDR_EPSILON : HDR_EPSILON);
}

// Mirror-repeat with period 2·size (v2 design §3.9): the image pixel a canvas pixel `p` shows.
uint Mirror(uint p, uint size) {
  const uint wrapped = p % (2u * size);
  return (wrapped < size ? wrapped : 2u * size - 1u - wrapped);
}

// Linear light for resampling and the compose: scene-linear BT.709 with diffuse white at 1.0, or for
// SDR the sRGB-decoded display value (v2 design §0's O). Non-finite values count as black, as encode does.
float3 LinearLight(uint encoding, uint primaries, float3 value, float scale) {
  const float3 light = (IsSdr(encoding) ? SrgbDecode(saturate(value)) : ToSceneLinear(encoding, primaries, value, scale));
  return (all(isfinite(light)) ? light : float3(0.f, 0.f, 0.f));
}

// The model domain of a linear-light value: EncodeModel after its ToSceneLinear step.
float3 EncodeLinear(uint encoding, float3 light) {
  if (IsSdr(encoding)) return SrgbEncode(saturate(light));
  return SrgbEncode(GamutClip(NeutwoLuminance(light)));
}

// Normalised OkLab chroma κ (v2 design §0).
float2 Chroma(float3 lab) {
  return lab.yz / max(lab.x, LIGHTNESS_FLOOR);
}

// v2 design §3.7: NR's edit from what it was given to what it returned, both in the model domain, as (a in stops,
// δ in chroma, ℓ_M the guide). An unchanged or non-finite output gives exactly zero. With the near-black guard
// (F16) δ fades where either side is near black.
float4 ChangeField(uint encoding, float3 model_in, float3 model_out, bool near_black_guard) {
  const float epsilon = ChangeEpsilon(encoding);
  const float3 linear_in = SrgbDecode(model_in);
  const float luminance_in = max(Luminance(linear_in), 0.f);
  const float guide = log2(luminance_in + epsilon);
  if (!all(isfinite(model_out)) || all(model_out == model_in)) return float4(0.f, 0.f, 0.f, guide);
  const float3 linear_out = SrgbDecode(model_out);
  const float stops = clamp(log2((max(Luminance(linear_out), 0.f) + epsilon) / (luminance_in + epsilon)), -CHANGE_LIMIT,
                            CHANGE_LIMIT);
  const float3 lab_in = LinearToOkLab(linear_in);
  const float3 lab_out = LinearToOkLab(linear_out);
  const float guard = (near_black_guard ? smoothstep(LIGHTNESS_FLOOR, 2.f * LIGHTNESS_FLOOR, min(lab_out.x, lab_in.x)) : 1.f);
  return float4(stops, guard * (Chroma(lab_out) - Chroma(lab_in)), guide);
}

// v2 design §3.7's rebuild of one pixel from its linear-light original and the final change: the luminance
// moves by T·a stops (×Neutwo's slope with Consistent), the chroma by C·δ, then the luminance is set exactly;
// with a chroma clamp each channel's gain stays within ±χ stops of the luminance gain.
float3 Rebuild(uint encoding, float3 original, float3 change, float transfer_strength, float color_strength, uint options,
               float chroma_clamp) {
  const float epsilon = ChangeEpsilon(encoding);
  const float luminance = max(Luminance(original), 0.f);
  const float gain =
      ((options & OPTION_CONSISTENT) != 0u && !IsSdr(encoding) ? min(1.f + luminance * luminance, CONSISTENT_GAIN_LIMIT) : 1.f);
  const float target = max((luminance + epsilon) * exp2(transfer_strength * (gain * change.x)) - epsilon, 0.f);
  const float3 lab = LinearToOkLab(original);
  // OkLab's L scales with the cube root of a colour's scale; a black original takes a grey's.
  const float lightness = (luminance > 0.f ? lab.x * pow(target / luminance, 1.f / 3.f) : pow(target, 1.f / 3.f));
  const float2 chroma = Chroma(lab) + color_strength * change.yz;
  float3 rebuilt = OkLabToLinear(float3(lightness, chroma * max(lightness, LIGHTNESS_FLOOR)));
  const float rebuilt_luminance = Luminance(rebuilt);
  if (rebuilt_luminance > 0.f) {
    rebuilt *= target / rebuilt_luminance;
  }
  if (chroma_clamp > 0.f && luminance > 0.f) {
    const float low = target / luminance * exp2(-chroma_clamp);
    const float high = target / luminance * exp2(chroma_clamp);
    [unroll] for (uint channel = 0u; channel < 3u; ++channel) {
      if (original[channel] > CLAMP_CHANNEL_FLOOR * luminance) {
        rebuilt[channel] = clamp(rebuilt[channel], original[channel] * low, original[channel] * high);
      }
    }
    const float clamped_luminance = Luminance(rebuilt);
    if (clamped_luminance > 0.f) {
      rebuilt *= target / clamped_luminance;
    }
  }
  return rebuilt;
}

// v2 design §3.7/§3.8: an output pixel from its stored `value` and the upsampled final change. A zero change
// returns `value` exactly; so does a non-finite original or result (the final finite guard).
float3 Compose(uint encoding, uint primaries, float3 value, float3 change, float scale, float transfer_strength,
               float color_strength, uint options, float chroma_clamp) {
  if (all(change == float3(0.f, 0.f, 0.f))) return value;
  const float3 original = (IsSdr(encoding) ? SrgbDecode(saturate(value)) : ToSceneLinear(encoding, primaries, value, scale));
  if (!all(isfinite(original)) || !all(isfinite(change))) return value;
  const float3 rebuilt = Rebuild(encoding, original, change, transfer_strength, color_strength, options, chroma_clamp);
  const float3 stored =
      (IsSdr(encoding) ? SrgbEncode(GamutClip(rebuilt)) : FromSceneLinear(encoding, primaries, rebuilt, scale));
  return (all(isfinite(stored)) ? stored : value);
}

// v2 design §3.10: pass n's own Transfer and Colour strength, from what it was given to what it returned, both in
// the model domain; the result is pass n's output. An unchanged or non-finite output returns `given` exactly: the
// rebuild's round trip is off by up to ~1e-4 relative, and the FP16 UAV store rounds toward zero, so recomputing an
// unchanged pixel would drop it a whole FP16 step and invent a change the change field then applies.
float3 ResolveStrengths(float3 given, float3 returned, float transfer_strength, float color_strength) {
  if (!all(isfinite(returned)) || all(returned == given)) return given;
  const float3 linear_given = SrgbDecode(given);
  const float3 linear_returned = SrgbDecode(returned);
  const float luminance_given = max(Luminance(linear_given), 0.f);
  const float ratio =
      (luminance_given > BLACK ? clamp(max(Luminance(linear_returned), 0.f) / luminance_given, RATIO_MIN, RATIO_MAX) : 1.f);
  const float target = luminance_given * (1.f + (ratio - 1.f) * transfer_strength);
  const float3 lab_given = LinearToOkLab(linear_given);
  const float lightness =
      (luminance_given > 0.f ? lab_given.x * pow(target / luminance_given, 1.f / 3.f) : pow(target, 1.f / 3.f));
  const float2 chroma = Chroma(lab_given) + color_strength * (Chroma(LinearToOkLab(linear_returned)) - Chroma(lab_given));
  float3 rebuilt = OkLabToLinear(float3(lightness, chroma * max(lightness, LIGHTNESS_FLOOR)));
  const float rebuilt_luminance = Luminance(rebuilt);
  if (rebuilt_luminance > 0.f) {
    rebuilt *= target / rebuilt_luminance;
  }
  return SrgbEncode(GamutClip(rebuilt));
}

#endif  // UPLIFT_CHANGE_COMMON_HLSLI
