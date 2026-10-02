#include "change_common.hlsli"
#include "vk_common.hlsli"

cbuffer EncodeConstants : register(b0) {
  uint encoding;
  uint width;          // the work image: the source region resampled to this size
  uint height;
  float input_scale;
  uint second_output;  // encode mode: clear u1 (the zero motion); change mode: write NR's input chroma κ_M into u1
  uint has_exposure;
  float exposure_factor;
  uint source_x;       // the source region
  uint source_y;
  uint source_width;
  uint source_height;
  uint canvas_width;   // the model texture: the image plus mirrored padding
  uint canvas_height;
  uint change_mode;    // 1: write NR's change field (t1 = NR's output) at the image size instead
  uint options;        // Plan 5: OPTION_* (the primaries, the near-black guard)
  uint reserved0;
};

// Plan 13: on Vulkan's After DLSS the source is DLSS's output, read in place from its storage view (u2).
#if UPLIFT_VK_SOURCE_STORAGE
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> source_texture : register(u2);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture[uint2(pixel)]
#else
Texture2D<float4> source_texture : register(t0);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture.Load(int3((pixel), 0))
#endif
Texture2D<float4> nr_texture : register(t1);
Texture2D<float> exposure_texture : register(t2);
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> output_texture : register(u0);  // the model texture, or the change field in change mode
UPLIFT_IMAGE_FORMAT("rg16f") RWTexture2D<float2> motion_texture : register(u1);

static const int MAX_TAPS = 8;  // per side of one footprint: the floor keeps every downscale well below this

// v2 design §3.8's downscale: the mean of the source under image pixel `pixel`'s footprint in linear
// light, each source pixel weighted by the share of it the footprint covers.
float3 AreaAverage(uint2 pixel, float scale) {
  const float2 ratio = float2(source_width, source_height) / float2(width, height);
  const float2 low = float2(pixel) * ratio;
  const float2 high = low + ratio;
  const int2 first = int2(floor(low));
  const int2 last = min(int2(ceil(high)) - 1, int2(source_width, source_height) - 1);
  float3 sum = float3(0.f, 0.f, 0.f);
  float total = 0.f;
  [loop] for (int y = first.y; y <= min(last.y, first.y + MAX_TAPS - 1); ++y) {
    const float cover_y = min(high.y, float(y + 1)) - max(low.y, float(y));
    [loop] for (int x = first.x; x <= min(last.x, first.x + MAX_TAPS - 1); ++x) {
      const float cover = cover_y * (min(high.x, float(x + 1)) - max(low.x, float(x)));
      sum += cover * LinearLight(encoding, options & OPTION_PRIMARIES_MASK,
                                 UPLIFT_LOAD_SOURCE(int2(source_x, source_y) + int2(x, y)).rgb, scale);
      total += cover;
    }
  }
  return (total > 0.f ? sum / total : float3(0.f, 0.f, 0.f));
}

// What NR is given at image pixel `pixel`. At the source's own size it is exactly the encode the
// full-size decode recomputes; smaller, the encoded linear-light area mean.
float3 ModelAt(uint2 pixel, float scale) {
  if (source_width == width && source_height == height) {
    return QuantizeHalf(EncodeModel(encoding, options & OPTION_PRIMARIES_MASK,
                                    UPLIFT_LOAD_SOURCE(uint2(source_x, source_y) + pixel).rgb, scale));
  }
  return QuantizeHalf(EncodeLinear(encoding, AreaAverage(pixel, scale)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 extent = (change_mode != 0u ? uint2(width, height) : uint2(canvas_width, canvas_height));
  if (id.x >= extent.x || id.y >= extent.y) return;
  // `precise` keeps the model value bit-identical to the full-size decode's recomputation (DecodeModel's
  // recomputed_model) and to this shader's own change mode. The value is an exact FP16 number, so the
  // UAV conversion cannot round differently from QuantizeHalf.
  precise float scale = input_scale * GameExposure(has_exposure, exposure_texture.Load(int3(0, 0, 0)), exposure_factor);
  precise float3 model_value = ModelAt(uint2(Mirror(id.x, width), Mirror(id.y, height)), scale);
  if (change_mode != 0u) {
    output_texture[id.xy] =
        ChangeField(encoding, model_value, nr_texture.Load(int3(id.xy, 0)).rgb, (options & OPTION_NEAR_BLACK_GUARD) != 0u);
    if (second_output != 0u) {
      motion_texture[id.xy] = Chroma(LinearToOkLab(SrgbDecode(model_value)));  // κ_M, the look's colour basis
    }
    return;
  }
  output_texture[id.xy] = float4(model_value, 1.f);
  if (second_output != 0u) {
    motion_texture[id.xy] = float2(0.f, 0.f);
  }
}
