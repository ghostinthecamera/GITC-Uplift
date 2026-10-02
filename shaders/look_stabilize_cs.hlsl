#include "look_common.hlsli"
#include "vk_common.hlsli"

// v2 design §3.12's stabiliser (amendment 4). Three modes over one set of constants:
// - CUT, one thread: the top level's mean ℓ_M against last frame's (a scene cut: more than 1.5 stops);
// - LEVELS: the moving average of the two levels the low band reads;
// - DETAIL: the same at the work size, on the fine band (a − a_low, δ − δ_low), for Stabilize detail.
cbuffer StabilizeConstants : register(b0) {
  uint image_width;
  uint image_height;
  uint levels;
  uint mode;             // STABILIZE_CUT, STABILIZE_LEVELS or STABILIZE_DETAIL
  uint level;            // LEVELS: the lower of the two levels (look::Bands::stable_level)
  uint flags;            // STABILIZE_FLAG_*
  float rate;            // α = 1 − exp(−Δt/τ)
  float low_level;       // DETAIL: λ, for a_low
  float motion_scale_x;  // the vectors' scale into the pixels of their rect (MV.Scale x MotionScale)
  float motion_scale_y;
  uint motion_x;         // their rect in t2
  uint motion_y;
  uint motion_width;
  uint motion_height;
  uint reserved0;
  uint reserved1;
};

static const uint STABILIZE_CUT = 0u;
static const uint STABILIZE_LEVELS = 1u;
static const uint STABILIZE_DETAIL = 2u;
static const uint STABILIZE_FLAG_RESET = 1u;    // this frame starts the history afresh
static const uint STABILIZE_FLAG_MOTION = 2u;   // Motion: reproject, and drop history where the image changed
static const uint STABILIZE_FLAG_VECTORS = 4u;  // t2 holds motion vectors; without them Motion is change-gated static
static const float SCENE_CUT_STOPS = 1.5f;

Texture2D<float4> gauss_atlas : register(t0);
Texture2D<float4> previous_texture : register(t1);  // last frame's history: the level atlas, or the detail texture
Texture2D<float4> motion_texture : register(t2);    // current -> previous
Texture2D<float4> change_texture : register(t3);    // DETAIL: C
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> current_texture : register(u0);  // this frame's history
UPLIFT_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> state_texture : register(u1);    // 1x1: (the top level's mean ℓ_M, scene cut, set, 0)

// u(p) in UV, current -> previous: E9's rule, the vectors times their scale over their rect. Zero without vectors.
float2 MotionAt(float2 uv) {
  if ((flags & STABILIZE_FLAG_VECTORS) == 0u) return float2(0.f, 0.f);
  const uint2 rect = uint2(motion_width, motion_height);
  const uint2 texel = uint2(motion_x, motion_y) + min(uint2(uv * float2(rect)), rect - 1u);
  const float2 motion = motion_texture.Load(int3(texel, 0)).xy * float2(motion_scale_x, motion_scale_y) / float2(rect);
  return (all(isfinite(motion)) ? motion : float2(0.f, 0.f));
}

// This frame's `current` against the history at `pixel` of the `size` region at `base` of t1.
float4 Blend(float4 current, uint2 pixel, int2 base, uint2 size) {
  if ((flags & STABILIZE_FLAG_RESET) != 0u || state_texture[uint2(0, 0)].y > 0.5f) return current;
  float4 history;
  float confidence = 1.f;
  if ((flags & STABILIZE_FLAG_MOTION) != 0u) {
    const float2 uv = (float2(pixel) + 0.5f) / float2(size);
    const float2 previous_uv = uv + MotionAt(uv);
    history = RegionBilinear(previous_texture, base, int2(size), previous_uv * float2(size) - 0.5f);
    const bool inside = all(previous_uv >= 0.f) && all(previous_uv <= 1.f);
    confidence = (inside ? 1.f - smoothstep(0.25f, 1.f, abs(current.w - history.w)) : 0.f);
  } else {
    history = previous_texture.Load(int3(base + int2(pixel), 0));  // Static: the same texel, exactly
  }
  const float blend = 1.f - (1.f - rate) * confidence;
  const float4 blended = history + blend * (current - history);
  return (all(isfinite(blended)) ? blended : current);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 image = uint2(image_width, image_height);
  if (mode == STABILIZE_CUT) {
    if (any(id.xy != 0u)) return;
    const uint2 top = LevelSize(image, levels);
    const int top_base = int(LevelOffset(image, levels));
    float sum = 0.f;
    for (uint y = 0u; y < top.y; ++y) {
      for (uint x = 0u; x < top.x; ++x) {
        sum += gauss_atlas.Load(int3(top_base + int(x), int(y), 0)).w;
      }
    }
    const float mean = sum / float(top.x * top.y);
    const float4 previous = state_texture[uint2(0, 0)];
    const bool cut = (previous.z > 0.5f && abs(mean - previous.x) > SCENE_CUT_STOPS);
    state_texture[uint2(0, 0)] = float4(mean, (cut ? 1.f : 0.f), 1.f, 0.f);
    return;
  }
  if (mode == STABILIZE_DETAIL) {
    if (id.x >= image.x || id.y >= image.y) return;
    const float4 change = change_texture.Load(int3(id.xy, 0));
    const float4 low = LowPass(gauss_atlas, image, levels, low_level, id.xy, true);
    current_texture[id.xy] = Blend(float4(change.xyz - low.xyz, change.w), id.xy, int2(0, 0), image);
    return;
  }
  const uint2 size = LevelSize(image, level);
  if (id.x >= size.x || id.y >= size.y) return;
  const int2 base = int2(LevelOffset(image, level), 0);
  current_texture[uint2(base) + id.xy] = Blend(gauss_atlas.Load(int3(base + int2(id.xy), 0)), id.xy, base, size);
  const uint second = min(level + 1u, levels);
  const uint2 second_size = LevelSize(image, second);
  if (second != level && id.x < second_size.x && id.y < second_size.y) {
    const int2 second_base = int2(LevelOffset(image, second), 0);
    current_texture[uint2(second_base) + id.xy] =
        Blend(gauss_atlas.Load(int3(second_base + int2(id.xy), 0)), id.xy, second_base, second_size);
  }
}
