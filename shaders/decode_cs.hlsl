#include "decode_common.hlsli"

// With UPLIFT_VK_SOURCE_STORAGE decode_common.hlsli has declared it (the in-place target).
#if !UPLIFT_VK_SOURCE_STORAGE
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> target_texture : register(u0);
#endif

// v2 design §3.4/§3.9: NR's result into a region of a UAV target (DLSS's Output, pre-SR's private
// colour), with compute-legal state only.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  target_texture[uint2(origin_x, origin_y) + id.xy] = DecodePixel(id.xy);
}
