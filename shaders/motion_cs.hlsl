#include "change_common.hlsli"
#include "vk_common.hlsli"

cbuffer MotionConstants : register(b0) {
  uint region_x;       // the game's motion vectors: this region of t1
  uint region_y;
  uint region_width;
  uint region_height;
  uint image_width;    // the destination image, at the destination's origin
  uint image_height;
  uint canvas_width;   // the destination: the image plus mirrored padding
  uint canvas_height;
  float scale_x;       // MV.Scale × MotionScale × image / region: vectors land in destination pixels
  float scale_y;
  uint reserved0;
  uint reserved1;
  uint reserved2;
  uint reserved3;
  uint reserved4;
  uint reserved5;
};

Texture2D<float4> motion_source : register(t1);
UPLIFT_IMAGE_FORMAT("rg16f") RWTexture2D<float2> motion_target : register(u1);

// Amendment 7 and v2 design §3.9 step 4: the game's motion vectors copied into an Uplift texture in
// destination pixels, resampled to the image and mirror-padded to the canvas, with the mirrored axis's
// component negated. Uplift binds the result with its full size as the subrect and a scale of (1, 1).
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= canvas_width || id.y >= canvas_height) return;
  const uint2 image = uint2(image_width, image_height);
  const bool2 mirrored = ((id.xy % (2u * image)) >= image);
  const uint2 pixel = uint2(Mirror(id.x, image_width), Mirror(id.y, image_height));
  const uint2 region = uint2(region_width, region_height);
  const uint2 read = min(uint2((float2(pixel) + 0.5f) * float2(region) / float2(image)), region - 1u);
  float2 motion = motion_source.Load(int3(uint2(region_x, region_y) + read, 0)).xy * float2(scale_x, scale_y);
  motion = select(mirrored, -motion, motion);
  motion_target[id.xy] = (all(isfinite(motion)) ? motion : float2(0.f, 0.f));
}
