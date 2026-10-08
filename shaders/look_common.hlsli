#ifndef UPLIFT_LOOK_COMMON_HLSLI
#define UPLIFT_LOOK_COMMON_HLSLI

#include "change_common.hlsli"

// GPU twin of src/look/look_math.cpp (v2 design §3.11/§3.12): the tone key and bands, the colour split, the soft
// limits, the order of operations and the pyramid atlas. tests/gpu/look_gpu_test.cpp compares the two.

static const float TONE_EPSILON = 1.f / 65536.f;               // ℓ_O = log2(Y(O)⁺ + 2^-16)
static const float MAX_MODEL_LUMINANCE = 1.f - 1.f / 4096.f;  // Neutwo's inverse diverges at 1

// ℓ_O in scene stops against diffuse white, from the change field's guide ℓ_M (amendment 3).
float ToneKey(bool sdr, float guide) {
  const float model = max(exp2(guide) - (sdr ? SDR_EPSILON : HDR_EPSILON), 0.f);
  if (sdr) return log2(model + TONE_EPSILON);
  const float bounded = min(model, MAX_MODEL_LUMINANCE);
  return log2(bounded / sqrt(1.f - bounded * bounded) + TONE_EPSILON);
}

float3 WeighTone(float key) {  // (shadows, midtones, highlights): they sum to 1
  const float shadows = 1.f - smoothstep(-4.5f, -2.5f, key);
  const float highlights = smoothstep(-1.f, 1.f, key);
  return float3(shadows, 1.f - shadows - highlights, highlights);
}

void SplitChroma(float2 delta, float2 basis, out float2 radial, out float2 tangential) {
  const float magnitude = sqrt(dot(basis, basis));
  const float weight = smoothstep(0.01f, 0.02f, magnitude);
  const float2 direction = (magnitude > 0.f ? basis / magnitude : float2(0.f, 0.f));
  radial = weight * dot(delta, direction) * direction + (1.f - weight) * delta;
  tangential = delta - radial;
}

float SoftLimit(float value, float limit) {
  if (limit <= 0.f) return value;
  const float ratio = value / limit;
  return value / sqrt(1.f + ratio * ratio);
}

float2 LimitChroma(float2 delta, float limit) {
  if (limit <= 0.f) return delta;
  const float ratio = sqrt(dot(delta, delta)) / limit;
  return delta / sqrt(1.f + ratio * ratio);
}

struct ShapeSettings {
  float tone;
  float detail;
  float halo;
  float brighten;
  float darken;
  float color;
  float hue;
  float shadows;
  float midtones;
  float highlights;
  float edit_strength;
  float max_brighten;
  float max_darken;
  float max_color;
};

struct ShapeInput {
  float low;           // ã_low
  float high;          // ã_high
  float2 chroma_low;   // δ̃_low
  float2 chroma_high;  // δ̃_high
  float reference;     // a_ref
  float peak;          // P_max
  float guide;         // ℓ_M
  float2 basis;        // κ_M
};

// v2 design §3.11 steps 4-10 and 12 (step 11, the mask, is the compose's). Non-finite results: no change.
float3 ShapeChange(ShapeInput shape_input, ShapeSettings settings, bool sdr) {
  const float banded = settings.tone * shape_input.low + settings.detail * shape_input.high;
  const float2 chroma = shape_input.chroma_low + shape_input.chroma_high;
  const float halo_weight = smoothstep(1.f, 3.f, shape_input.peak - shape_input.guide);
  const float dehaloed = banded - settings.halo * halo_weight * min(banded - shape_input.reference, 0.f);
  const float split = settings.brighten * max(dehaloed, 0.f) + settings.darken * min(dehaloed, 0.f);
  float2 radial;
  float2 tangential;
  SplitChroma(chroma, shape_input.basis, radial, tangential);
  const float2 recoloured = settings.color * radial + settings.hue * tangential;
  const float3 tone = WeighTone(ToneKey(sdr, shape_input.guide));
  const float weight = settings.shadows * tone.x + settings.midtones * tone.y + settings.highlights * tone.z;
  const float strength = settings.edit_strength * weight;
  const float stops = strength * split;
  const float limited = SoftLimit(stops, (stops > 0.f ? settings.max_brighten : settings.max_darken));
  const float2 chroma_out = LimitChroma(strength * recoloured, settings.max_color);
  if (!isfinite(limited) || !all(isfinite(chroma_out))) return float3(0.f, 0.f, 0.f);
  return float3(clamp(limited, -CHANGE_LIMIT, CHANGE_LIMIT), chroma_out);
}

// The pyramid's [1 3 3 1]/8 kernel at stride 2 (look_pyramid_cs.hlsl, faces_cs.hlsl).
static const float PYRAMID_KERNEL[4] = {0.125f, 0.375f, 0.375f, 0.125f};

// The pyramid atlas (look::MakeAtlas): level k is ceil(image / 2^k), at x = the widths of levels 1..k-1.
uint2 LevelSize(uint2 image, uint level) {
  return (image + (1u << level) - 1u) >> level;
}

uint LevelOffset(uint2 image, uint level) {
  uint offset = 0u;
  for (uint below = 1u; below < level; ++below) {
    offset += LevelSize(image, below).x;
  }
  return offset;
}

// Work pixel `pixel`'s centre in level `level`'s texels.
float2 LevelPosition(uint2 pixel, uint level) {
  return (float2(pixel) + 0.5f) / float(1u << level) - 0.5f;
}

// A bilinear sample of the `size` region at `base` of `atlas`, clamped to that region.
float4 RegionBilinear(Texture2D<float4> atlas, int2 base, int2 size, float2 position) {
  const float2 clamped = clamp(position, float2(0.f, 0.f), float2(size - 1));
  const int2 first = int2(floor(clamped));
  const int2 last = min(first + 1, size - 1);
  const float2 fraction = clamped - float2(first);
  const float4 top = lerp(atlas.Load(int3(base + first, 0)), atlas.Load(int3(base + int2(last.x, first.y), 0)), fraction.x);
  const float4 bottom = lerp(atlas.Load(int3(base + int2(first.x, last.y), 0)), atlas.Load(int3(base + last, 0)), fraction.x);
  return lerp(top, bottom, fraction.y);
}

// One level's sample at a work pixel: bilinear, or with `tent` a 3x3 tent of bilinear taps from level 3 up.
float4 LevelSample(Texture2D<float4> atlas, uint2 image, uint level, uint2 pixel, bool tent) {
  const int2 base = int2(LevelOffset(image, level), 0);
  const int2 size = int2(LevelSize(image, level));
  const float2 position = LevelPosition(pixel, level);
  if (!tent || level < 3u) return RegionBilinear(atlas, base, size, position);
  float4 sum = float4(0.f, 0.f, 0.f, 0.f);
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      const float weight = (x == 0 ? 0.5f : 0.25f) * (y == 0 ? 0.5f : 0.25f);
      sum += weight * RegionBilinear(atlas, base, size, position + float2(x, y));
    }
  }
  return sum;
}

// v2 design §3.12's LP: trilinear between floor(level) and the next level, clamped to [1, levels].
float4 LowPass(Texture2D<float4> atlas, uint2 image, uint levels, float level, uint2 pixel, bool tent) {
  const float clamped = clamp(level, 1.f, float(levels));
  const uint first = uint(floor(clamped));
  const uint second = min(first + 1u, levels);
  return lerp(LevelSample(atlas, image, first, pixel, tent), LevelSample(atlas, image, second, pixel, tent),
              clamped - float(first));
}

// The halo's P_max: the largest of the 2x2 texels of `level` around the work pixel.
float PeakAround(Texture2D<float> peaks, uint2 image, uint level, uint2 pixel) {
  const int2 base = int2(LevelOffset(image, level), 0);
  const int2 size = int2(LevelSize(image, level));
  const int2 first = clamp(int2(floor(LevelPosition(pixel, level))), int2(0, 0), size - 1);
  const int2 last = min(first + 1, size - 1);
  return max(max(peaks.Load(int3(base + first, 0)), peaks.Load(int3(base + int2(last.x, first.y), 0))),
             max(peaks.Load(int3(base + int2(first.x, last.y), 0)), peaks.Load(int3(base + last, 0))));
}

#endif  // UPLIFT_LOOK_COMMON_HLSLI
