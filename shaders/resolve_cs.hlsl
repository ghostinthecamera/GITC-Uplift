#include "change_common.hlsli"
#include "vk_common.hlsli"

// v2 design §3.10: pass n's own Transfer and Colour strength, applied to its raw output in place.
cbuffer ResolveConstants : register(b0) {
  uint width;
  uint height;
  float transfer_strength;
  float color_strength;
};

Texture2D<float4> given_texture : register(t0);       // what the pass was given
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> returned_texture : register(u0);  // what it returned; rewritten

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  const float4 returned = returned_texture[id.xy];
  returned_texture[id.xy] =
      float4(ResolveStrengths(given_texture.Load(int3(id.xy, 0)).rgb, returned.rgb, transfer_strength, color_strength), returned.a);
}
