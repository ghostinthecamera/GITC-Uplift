#ifndef UPLIFT_DECODE_COMMON_HLSLI
#define UPLIFT_DECODE_COMMON_HLSLI

#include "change_common.hlsli"
#include "vk_common.hlsli"

// Shared by decode_ps.hlsl (the Present path, through an RTV) and decode_cs.hlsl (UAV targets).
cbuffer DecodeConstants : register(b0) {
  uint encoding;
  uint target_srgb;    // decode_ps only: the RTV sRGB-encodes, so the shader writes linear values
  float input_scale;
  float transfer_strength;
  float color_strength;
  uint has_exposure;
  float exposure_factor;
  uint origin_x;       // decode_cs only: the region's top-left corner in the target
  uint origin_y;
  uint width;          // the region: the output size
  uint height;
  uint upsampling;     // 0: NR ran at the output size and t1 is its output; 1 Classic, 2 Edge-aware: t1 is the change field
  uint source_x;       // the region's top-left corner in the source
  uint source_y;
  uint options;        // Plan 5: OPTION_* (the primaries, Consistent transfer, the mask)
  float chroma_clamp;  // Plan 5 (F14): stops; 0 = off
};

// Plan 13: on Vulkan's After DLSS the decode works in place: the target (DLSS's output, u0) is also where the original pixel is read.
#if UPLIFT_VK_SOURCE_STORAGE
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> target_texture : register(u0);
#define UPLIFT_LOAD_SOURCE(pixel) target_texture[uint2(pixel)]
#else
Texture2D<float4> source_texture : register(t0);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture.Load(int3((pixel), 0))
#endif
Texture2D<float4> nr_texture : register(t1);
Texture2D<float> exposure_texture : register(t2);
Texture2D<float4> mask_texture : register(t3);  // Plan 5 (§3.13): Uplift's copy of UPLIFT_MASK, read with OPTION_MASK

static const float SPATIAL_RADIUS = 1.5f;  // the tent's half-width, in work pixels (v2 design §3.8)
static const float RANGE_SIGMA = 0.5f;     // the range kernel's width, in stops
static const float MIN_WEIGHT = 1e-4f;     // below this total the edge-aware sample falls back to bilinear

// Classic: a bilinear sample of the change field at `position` (work pixels), clamped to its texels.
float3 ChangeBilinear(float2 position, uint2 size) {
  const float2 clamped = clamp(position, float2(0.f, 0.f), float2(size) - 1.f);
  const int2 first = int2(floor(clamped));
  const int2 last = min(first + 1, int2(size) - 1);
  const float2 fraction = clamped - float2(first);
  const float3 top = lerp(nr_texture.Load(int3(first, 0)).xyz, nr_texture.Load(int3(last.x, first.y, 0)).xyz, fraction.x);
  const float3 bottom = lerp(nr_texture.Load(int3(first.x, last.y, 0)).xyz, nr_texture.Load(int3(last, 0)).xyz, fraction.x);
  return lerp(top, bottom, fraction.y);
}

// Edge-aware: v2 design §3.8's joint bilateral upsample over the 4x4 work pixels around `position`:
// a tent in space, times how close each pixel's guide ℓ_M is to this output pixel's `guide`.
float3 ChangeEdgeAware(float2 position, uint2 size, float guide) {
  const int2 base = int2(floor(position));
  float3 sum = float3(0.f, 0.f, 0.f);
  float total = 0.f;
  [unroll] for (int y = -1; y <= 2; ++y) {
    [unroll] for (int x = -1; x <= 2; ++x) {
      const int2 tap = base + int2(x, y);
      const float2 gap = abs(position - float2(tap));  // not `distance`: an HLSL intrinsic
      const float spatial = max(1.f - gap.x / SPATIAL_RADIUS, 0.f) * max(1.f - gap.y / SPATIAL_RADIUS, 0.f);
      const float4 change = nr_texture.Load(int3(clamp(tap, int2(0, 0), int2(size) - 1), 0));
      const float difference = guide - change.w;
      const float weight = spatial * exp(-difference * difference / (2.f * RANGE_SIGMA * RANGE_SIGMA));
      if (weight > 0.f && all(isfinite(change))) {
        sum += weight * change.xyz;
        total += weight;
      }
    }
  }
  return (total >= MIN_WEIGHT ? sum / total : ChangeBilinear(position, size));
}

// The mask's R channel, bilinear at this output pixel whatever the mask's size, clamped to [0, 1]; 1 when not finite.
float MaskAt(uint2 pixel) {
  uint2 size;
  mask_texture.GetDimensions(size.x, size.y);
  const float2 position = (float2(pixel) + 0.5f) * (float2(size) / float2(width, height)) - 0.5f;
  const float2 clamped = clamp(position, float2(0.f, 0.f), float2(size) - 1.f);
  const int2 first = int2(floor(clamped));
  const int2 last = min(first + 1, int2(size) - 1);
  const float2 fraction = clamped - float2(first);
  const float top = lerp(mask_texture.Load(int3(first, 0)).r, mask_texture.Load(int3(last.x, first.y, 0)).r, fraction.x);
  const float bottom = lerp(mask_texture.Load(int3(first.x, last.y, 0)).r, mask_texture.Load(int3(last, 0)).r, fraction.x);
  const float value = lerp(top, bottom, fraction.y);
  return (isfinite(value) ? saturate(value) : 1.f);
}

// Region pixel `pixel` (0-based) in the stored encoding, with the source's alpha.
float4 DecodePixel(uint2 pixel) {
  const float4 source = UPLIFT_LOAD_SOURCE(uint2(source_x, source_y) + pixel);
  precise float scale = input_scale * GameExposure(has_exposure, exposure_texture.Load(int3(0, 0, 0)), exposure_factor);
  const uint primaries = options & OPTION_PRIMARIES_MASK;
  if (upsampling == 0u) {
    return float4(DecodeModel(encoding, primaries, source.rgb, nr_texture.Load(int3(pixel, 0)).rgb, scale, transfer_strength,
                              color_strength),
                  source.a);
  }
  uint2 size;
  nr_texture.GetDimensions(size.x, size.y);
  const float2 position = (float2(pixel) + 0.5f) * (float2(size) / float2(width, height)) - 0.5f;  // p_w
  float3 change;
  if (upsampling == 2u) {
    // g(p): the guide at this output pixel, with M recomputed here the way encode computes it.
    const float3 model = EncodeModel(encoding, primaries, source.rgb, scale);
    change = ChangeEdgeAware(position, size, log2(max(Luminance(SrgbDecode(model)), 0.f) + ChangeEpsilon(encoding)));
  } else {
    change = ChangeBilinear(position, size);
  }
  // v2 design §3.13, step 11: the mask scales the final change at this pixel, so 0 is exactly the original.
  if ((options & OPTION_MASK) != 0u) {
    change *= MaskAt(pixel);
  }
  return float4(Compose(encoding, primaries, source.rgb, change, scale, transfer_strength, color_strength, options, chroma_clamp),
                source.a);
}

#endif  // UPLIFT_DECODE_COMMON_HLSLI
