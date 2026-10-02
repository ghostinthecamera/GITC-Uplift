#include "look_common.hlsli"
#include "vk_common.hlsli"

// v2 design §3.11/§3.12: the change field C rewritten in place with the shaped change (a*, δ*), keeping ℓ_M in w
// for the edge-aware upsample.
cbuffer ShapeConstants : register(b0) {
  uint image_width;
  uint image_height;
  uint levels;
  uint flags;           // SHAPE_FLAG_*
  float low_level;      // λ
  uint peak_level;      // the halo's max-pyramid level
  float tone;           // look::ShapeSettings, in its order
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

static const uint SHAPE_FLAG_SDR = 1u;
static const uint SHAPE_FLAG_STABILIZED = 2u;  // t2 holds this frame's stabilised levels
static const uint SHAPE_FLAG_DETAIL = 4u;      // t4 holds this frame's stabilised fine band

Texture2D<float4> gauss_atlas : register(t0);
Texture2D<float> peak_atlas : register(t1);
Texture2D<float4> history_atlas : register(t2);
Texture2D<float2> basis_texture : register(t3);
Texture2D<float4> detail_texture : register(t4);
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> change_texture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 image = uint2(image_width, image_height);
  if (id.x >= image.x || id.y >= image.y) return;
  const float4 change = change_texture[id.xy];
  const float4 low = LowPass(gauss_atlas, image, levels, low_level, id.xy, true);
  float4 stable_low = low;
  if ((flags & SHAPE_FLAG_STABILIZED) != 0u) {
    stable_low = LowPass(history_atlas, image, levels, low_level, id.xy, true);
  }
  float4 high = float4(change.xyz - low.xyz, 0.f);
  if ((flags & SHAPE_FLAG_DETAIL) != 0u) {
    high = detail_texture.Load(int3(id.xy, 0));
  }
  // In field order: (low, high, chroma_low, chroma_high, reference, peak, guide, basis).
  const ShapeInput shape_input = {stable_low.x, high.x, stable_low.yz, high.yz,
                                  LowPass(gauss_atlas, image, levels, low_level + 1.f, id.xy, false).x,
                                  PeakAround(peak_atlas, image, peak_level, id.xy), change.w, basis_texture.Load(int3(id.xy, 0))};
  const ShapeSettings settings = {tone, detail, halo, brighten, darken, color, hue, shadows, midtones, highlights, edit_strength,
                                  max_brighten, max_darken, max_color};
  change_texture[id.xy] = float4(ShapeChange(shape_input, settings, (flags & SHAPE_FLAG_SDR) != 0u), change.w);
}
