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
  uint flip_y;         // 1.1.6: 1 when the source is upside down against the destination (DLSS's vectors at Present in some engines, Unity's for one)
  uint reserved1;
  uint reserved2;
  uint reserved3;
  uint reserved4;
  uint reserved5;
};

Texture2D<float4> motion_source : register(t1);
UPLIFT_IMAGE_FORMAT("rg16f") RWTexture2D<float2> motion_target : register(u1);

// 1.0.1 (the Launchpad investigation): the longest vector NR is given, in canvases per axis. A vector more than twice the canvas long points off
// the frame however long it is, so clamping it there keeps its direction (history off-screen) while NR, the stabiliser and the look never see
// thousands of canvases (65504 px, RG16F's largest finite value, from an optical flow on a cut). Every source copied here gets it: the game's DLSS
// vectors (the Present copy, the pre-SR canvas) and Launchpad's UPLIFT_MV.
static const float MOTION_LIMIT_CANVASES = 2.f;

// Amendment 7 and v2 design §3.9 step 4: the game's motion vectors copied into an Uplift texture in
// destination pixels, resampled to the image and mirror-padded to the canvas, with the mirrored axis's
// component negated. Uplift binds the result with its full size as the subrect and a scale of (1, 1).
// A non-finite vector (Inf, NaN) becomes no motion; a finite one is clamped to MOTION_LIMIT_CANVASES.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= canvas_width || id.y >= canvas_height) return;
  const uint2 image = uint2(image_width, image_height);
  const bool2 mirrored = ((id.xy % (2u * image)) >= image);
  const uint2 pixel = uint2(Mirror(id.x, image_width), Mirror(id.y, image_height));
  const uint2 region = uint2(region_width, region_height);
  uint2 read = min(uint2((float2(pixel) + 0.5f) * float2(region) / float2(image)), region - 1u);
  if (flip_y != 0u) {
    read.y = region.y - 1u - read.y;  // the source's rows run the other way
  }
  float2 motion = motion_source.Load(int3(uint2(region_x, region_y) + read, 0)).xy * float2(scale_x, scale_y);
  if (flip_y != 0u) {
    motion.y = -motion.y;  // and so does its vertical motion
  }
  motion = select(mirrored, -motion, motion);
  // Non-finite first: clamp's result for a NaN is the hardware's choice.
  motion = (all(isfinite(motion)) ? motion : float2(0.f, 0.f));
  const float2 limit = MOTION_LIMIT_CANVASES * float2(canvas_width, canvas_height);
  motion_target[id.xy] = clamp(motion, -limit, limit);
}
