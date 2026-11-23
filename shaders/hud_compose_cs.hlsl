// HUD-less (design §4), Vulkan: out = F + (1 - A) * (N - H) in stored values, as hud_compose_ps does on Direct3D 12. The result goes to
// a UNORM or float storage image (UPLIFT_VK_TARGET_FORMAT) that is then copied raw over the back buffer; for a BGRA8 back buffer the
// channels are written swapped into an RGBA8 storage image, so the raw copy lands them in place.
#include "vk_common.hlsli"

Texture2D<float4> final_frame : register(t0);
Texture2D<float4> nr_hudless : register(t1);
Texture2D<float4> game_hudless : register(t2);
Texture2D<float4> opacity : register(t3);
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> output : register(u0);

struct HudCompose {
  uint alpha_channel;  // 0: the opacity is in .r; 3: in .a
  uint clamp_unit;     // 1 for UNORM back buffers
  uint swap_rb;        // 1 for a BGRA8 back buffer written through RGBA8
  uint width;
  uint height;
};
[[vk::push_constant]] HudCompose constants;

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= constants.width || id.y >= constants.height) return;
  const int3 texel = int3(int2(id.xy), 0);
  const float4 final_value = final_frame.Load(texel);
  const float4 alpha_texel = opacity.Load(texel);
  const float alpha = (constants.alpha_channel == 3u ? alpha_texel.a : alpha_texel.r);
  float4 result = final_value;
  result.rgb = final_value.rgb + (1.f - alpha) * (nr_hudless.Load(texel).rgb - game_hudless.Load(texel).rgb);
  if (constants.clamp_unit != 0u) result = saturate(result);
  output[id.xy] = (constants.swap_rb != 0u ? result.bgra : result);
}
