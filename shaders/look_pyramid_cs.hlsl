#include "look_common.hlsli"
#include "vk_common.hlsli"

// v2 design §3.12: one level of the Gaussian pyramid of the change field ([1 3 3 1]/8 at stride 2) and of the
// max pyramid of ℓ_M, each into its atlas. Dispatched once per level, with a UAV barrier between levels.
cbuffer PyramidConstants : register(b0) {
  uint image_width;
  uint image_height;
  uint level;  // written by this dispatch; level - 1 is read (0: the change field)
  uint reserved0;
};

Texture2D<float4> change_texture : register(t0);
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> gauss_atlas : register(u0);
UPLIFT_IMAGE_FORMAT("r16f") RWTexture2D<float> peak_atlas : register(u1);

static const float KERNEL[4] = {0.125f, 0.375f, 0.375f, 0.125f};

// A texel of level - 1, clamped to it: the change field, zeroed where not finite, or the atlas.
float4 SourceTexel(int2 texel, int2 size, int base) {
  const int2 clamped = clamp(texel, int2(0, 0), size - 1);
  if (level == 1u) {
    const float4 value = change_texture.Load(int3(clamped, 0));
    return (all(isfinite(value)) ? value : float4(0.f, 0.f, 0.f, 0.f));
  }
  return gauss_atlas[uint2(base + clamped.x, clamped.y)];
}

float SourcePeak(int2 texel, int2 size, int base) {
  if (level == 1u) return SourceTexel(texel, size, base).w;
  const int2 clamped = clamp(texel, int2(0, 0), size - 1);
  return peak_atlas[uint2(base + clamped.x, clamped.y)];
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 image = uint2(image_width, image_height);
  const uint2 size = LevelSize(image, level);
  if (id.x >= size.x || id.y >= size.y) return;
  const int2 source_size = int2(level == 1u ? image : LevelSize(image, level - 1u));
  const int source_base = (level == 1u ? 0 : int(LevelOffset(image, level - 1u)));
  const int2 origin = int2(id.xy) * 2;
  float4 sum = float4(0.f, 0.f, 0.f, 0.f);
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      sum += (KERNEL[x] * KERNEL[y]) * SourceTexel(origin + int2(x - 1, y - 1), source_size, source_base);
    }
  }
  float peak = SourcePeak(origin, source_size, source_base);
  peak = max(peak, SourcePeak(origin + int2(1, 0), source_size, source_base));
  peak = max(peak, SourcePeak(origin + int2(0, 1), source_size, source_base));
  peak = max(peak, SourcePeak(origin + int2(1, 1), source_size, source_base));
  const uint base = LevelOffset(image, level);
  gauss_atlas[uint2(base + id.x, id.y)] = sum;
  peak_atlas[uint2(base + id.x, id.y)] = peak;
}
