#include "look_common.hlsli"
#include "vk_common.hlsli"

// Keep faces (2026-10-08, .superpowers/sdd/2026-10-08-keep-faces/design.md). Pass 1 runs twice on the same input: L1 with the user's settings and F1, the
// twin, with NR's Character mask on and Skin structure at Face protection. Away from characters the two agree, so their difference D = L1 - F1 is NR's own
// character mask, recovered. Inside the mask M only the part of a pass's change broader than the Lighting scale is kept:
//   result = changed - M * (D - w * LP(D))
// For pass 1 `changed` is L1 and `reference` F1, so inside M the result is F1 + LP(L1 - F1); for a later pass `changed` is its raw output and `reference` its
// input, and M is pass 1's. LP is a normalised low-pass over the character's own pixels (the weights c), so the kept lighting does not fade toward a
// character's edges. Fix round 1 (I1): w is the weights' local coverage (a light blur, at edge_level) over their coverage at the mask's radius, so it is 1
// inside a covered region and falls to 0 just outside the outline (and in uncovered holes such as eyes), where M is still above 0 but D is about 0: the
// character's mean change is not added back there, so no light leaks past the outline. Every value is in the model domain NR reads and writes. The pyramid
// is the look stage's (look_common.hlsli's atlas, kernel and LowPass), of (c·D, c).
// Fix round 3 (the owner's glowing spots on characters):
// - Pass 1's weight c is relative: |D| against the pixel's own brightness in F1 (FirstWeight), so a few percent of noise on a specular glint, a bright edge
//   or an emissive no longer passes the dead zone the way an absolute model-domain step did.
// - The add-back is gated by the weights' density at the Lighting scale: w also carries saturate(LP(c) / density). A sparse point has almost no coverage
//   there, so its neighbours keep `changed` instead of receiving its whole change.
// - The add-back is bounded: |w · LPn(D)| = w · |LP(c·D)| / LP(c) <= (LP(c) / density) · |LP(c·D)| / LP(c) = |LP(c·D)| / density <= LP(|D|) / density, the
//   plain low-pass of |D| at the same level over the density. The combine also clamps to |LP(c·D)| / density per channel, so whatever the rounding, one
//   extreme pixel can only reach its neighbours through the kernel's own small weight at that level, never at full strength.
// Fix round 4 (fine specks on glints), round 5 (their size): with a speck radius r > 0 the difference a pass removes is first clamped to the per-channel
// range of the differences on the ring at Chebyshev distance r around the pixel (everything inside the ring excluded), in a pass of its own
// (FACES_MODE_DESPIKE) into the detail image, because the combine rewrites `changed` (and pass 1's twin) in place and so cannot read its neighbours. A
// spike up to 2r - 1 px wide, where NR's two runs disagree on a few bright pixels, falls back into the ring's range, so it keeps about `changed`; a line or
// an edge survives, since the ring crosses it. The pyramid (and so LP, the mask and the add-back) still reads the raw difference.
// Round 5 (holes in the mask on shadowed skin): M marks where NR's skin input changed the image, not where skin is. With FLAG_FILL, pass 1 also builds a
// second pyramid of the weights times the reference's chromaticity, (c·k·u, c·k·v, c·k, c), k = 1 above FILL_BRIGHTNESS_FLOOR, and grows the mask over
// skin of the same colour: F = saturate(FILL_GAIN · LP(c)) · similarity at the Fill radius, M' = max(M, F). M' is what the combine of pass 1 and the later
// passes remove under, and what Show the face mask tints; the add-back's density gate keeps reading the raw coverage LP(c), so filled skin never adds back
// lighting from glints. (u, v) is the normalised rgb chromaticity, rgb / (r + g + b), of the model-domain reference: bounded, so a tolerance means the same
// everywhere, and it averages linearly in the pyramid, which log-chroma would not do near black.
cbuffer FacesConstants : register(b0) {
  uint width;   // the canvas NR runs at
  uint height;
  uint levels;  // the atlas's (look::MakeAtlas)
  uint mode;    // FACES_MODE_*
  uint level;   // FACES_MODE_LEVEL: the level this dispatch writes; level - 1 is read (0: the pass's own images)
  uint flags;   // FLAG_*
  float low_level;   // λ of the Lighting scale (look::MakeFacesParameters)
  float mask_level;  // where the mask reads the weights' coverage (Mask softness)
  float edge_level;  // fix round 1 (I1): where the add-back reads the weights' local coverage (Edge falloff)
  // Fix round 1 addendum (the Advanced rows, look::FacesTuning): the weights from |D|: below dead_zone the two runs differ only by their own temporal
  // histories (noise), from full_weight on surely by NR's mask (always above dead_zone); and the mask's gain on its coverage.
  float dead_zone;      // fix round 3: fractions of the pixel's brightness
  float full_weight;
  float coverage_gain;
  float density;        // fix round 3: the weights' coverage at the Lighting scale from which the lighting is added back in full
  uint speck_radius;    // round 5: the despike ring's Chebyshev distance; 0: the combine removes the raw difference
  float fill_level;     // round 5: where the fill reads its pyramid (Fill radius)
  float fill_tolerance; // round 5: the chromaticity distance at which the fill's similarity reaches 0 (Colour tolerance)
};

static const uint FACES_MODE_LEVEL = 0u;
static const uint FACES_MODE_FIRST = 1u;    // pass 1: L1 rewritten with its result, F1 with the mask
static const uint FACES_MODE_LATER = 2u;    // a later pass's raw output rewritten with its result
static const uint FACES_MODE_SHOW = 3u;     // Show the face mask: the mask tinted into the result
static const uint FACES_MODE_DESPIKE = 4u;  // fix round 4: `changed` - `reference`, clamped to its ring's range, into the detail image
static const uint FLAG_FIRST = 1u;          // LEVEL: pass 1 (the weights from |D|), else a later pass (the weights are pass 1's mask)
static const uint FLAG_FILL = 2u;           // round 5: LEVEL of pass 1 also writes the fill pyramid; FIRST fills the mask from it

static const float3 LUMA_WEIGHTS = float3(0.2126f, 0.7152f, 0.0722f);
static const float WEIGHT_FLOOR = 1.f / 1024.f;  // the normalised low-pass needs this much weight around a pixel; below it LP is 0 (and so is M)
// Fix round 3: FirstWeight's brightness offset, so darks are judged against at least this much model-domain brightness instead of their own near-zero.
static const float DARK_OFFSET = 0.15f;
static const float3 MASK_TINT = float3(1.f, 0.f, 1.f);
static const float MASK_TINT_STRENGTH = 0.6f;
// Round 5: the fill. Pixels darker than FILL_BRIGHTNESS_FLOOR (model-domain luma) have no meaningful chroma: they give the fill pyramid no colour and get
// FILL_DARK_SIMILARITY instead of a colour match, so shadowed skin next to marked skin fills partly. FILL_GAIN: a neighbourhood a quarter covered by marked
// skin at the Fill radius fills fully.
static const float FILL_BRIGHTNESS_FLOOR = 0.06f;
static const float FILL_DARK_SIMILARITY = 0.5f;
static const float FILL_GAIN = 4.f;
// Round 5: the despike's group tile: the 8x8 group and an apron of the largest ring (look::FACES_MAX_SPECK_RADIUS).
static const int SPECK_MAX_RADIUS = 3;
static const int SPECK_TILE = 8 + 2 * SPECK_MAX_RADIUS;

Texture2D<float4> source_texture : register(t0);     // LEVEL 1, DESPIKE: `changed`; FIRST, LATER: the atlas
Texture2D<float4> reference_texture : register(t1);  // LEVEL 1, DESPIKE: `reference`; LATER, SHOW: the mask (M' in every channel)
Texture2D<float4> extra_texture : register(t2);      // LEVEL 1 of a later pass: the mask; LATER: `reference`
Texture2D<float4> detail_texture : register(t3);     // FIRST, LATER with a speck radius: the despiked difference
Texture2D<float4> fill_texture : register(t4);       // round 5: FIRST with FLAG_FILL: the fill pyramid
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> target_texture : register(u0);  // LEVEL: the atlas; FIRST, LATER: `changed`; SHOW: the result;
                                                                                    // DESPIKE: the detail image
// Vulkan: the set's third storage binding, so the second (the colour pipeline's RG16F placeholder) is never declared here.
#if defined(__spirv__)
[[vk::binding(18, 0)]]
#endif
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> second_target : register(u1);  // FIRST: F1 in, the mask out; LEVEL with FLAG_FILL: the fill pyramid

groupshared float4 speck_tile[SPECK_TILE * SPECK_TILE];  // DESPIKE: (D, 1) of the tile's pixels; 0 outside the canvas or where not finite

// Pass 1's weight c of a pixel's change D against its brightness in F1 (`reference`): 0 in the dead zone, 1 from full_weight on (fix round 3: relative).
float FirstWeight(float3 change, float3 reference) {
  const float relative = dot(abs(change), LUMA_WEIGHTS) / (max(dot(reference, LUMA_WEIGHTS), 0.f) + DARK_OFFSET);
  return saturate((relative - dead_zone) / (full_weight - dead_zone));
}

// Round 5: the normalised rgb chromaticity (r, g) / (r + g + b) of a model-domain colour.
float2 Chromaticity(float3 value) {
  const float3 positive = max(value, float3(0.f, 0.f, 0.f));
  const float sum = positive.r + positive.g + positive.b;
  return (sum > 0.f ? positive.rg / sum : float2(1.f / 3.f, 1.f / 3.f));
}

// Level 1's two texels at a canvas pixel, clamped to it: the pass's weighted change (c·D, c) and (round 5) the fill's (c·k·u, c·k·v, c·k, c); zero where
// not finite.
void FirstLevelTexels(int2 texel, int2 size, out float4 weighted, out float4 fill) {
  const int2 clamped = clamp(texel, int2(0, 0), size - 1);
  const float3 reference = reference_texture.Load(int3(clamped, 0)).rgb;
  const float3 change = source_texture.Load(int3(clamped, 0)).rgb - reference;
  weighted = float4(0.f, 0.f, 0.f, 0.f);
  fill = float4(0.f, 0.f, 0.f, 0.f);
  if (!all(isfinite(change))) return;
  const float weight = ((flags & FLAG_FIRST) != 0u ? FirstWeight(change, reference) : saturate(extra_texture.Load(int3(clamped, 0)).a));
  weighted = float4(weight * change, weight);
  const float coloured = (dot(reference, LUMA_WEIGHTS) >= FILL_BRIGHTNESS_FLOOR ? weight : 0.f);
  fill = float4(coloured * Chromaticity(reference), coloured, weight);
}

// The kept lighting at a pixel before the edge falloff: LPn(D), the weighted mean of D at the Lighting scale, times the density gate
// saturate(LP(c) / density), clamped to |LP(c·D)| / density per channel (the header's bound).
float3 BroadChange(uint2 pixel) {
  const float4 low = LowPass(source_texture, uint2(width, height), levels, low_level, pixel, true);
  if (low.a <= WEIGHT_FLOOR) return float3(0.f, 0.f, 0.f);
  const float3 bound = abs(low.rgb) / density;
  return clamp(saturate(low.a / density) * (low.rgb / low.a), -bound, bound);
}

// The weights' coverage around a pixel at `at_level` (the atlas's alpha).
float Coverage(uint2 pixel, float at_level) {
  return LowPass(source_texture, uint2(width, height), levels, at_level, pixel, false).a;
}

void Level(uint2 id) {
  const uint2 image = uint2(width, height);
  const uint2 size = LevelSize(image, level);
  if (id.x >= size.x || id.y >= size.y) return;
  const bool fill = ((flags & FLAG_FILL) != 0u);
  const int2 source_size = int2(level == 1u ? image : LevelSize(image, level - 1u));
  const int source_base = (level == 1u ? 0 : int(LevelOffset(image, level - 1u)));
  const int2 origin = int2(id) * 2;
  float4 sum = float4(0.f, 0.f, 0.f, 0.f);
  float4 fill_sum = float4(0.f, 0.f, 0.f, 0.f);
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      const float kernel = PYRAMID_KERNEL[x] * PYRAMID_KERNEL[y];
      const int2 texel = origin + int2(x - 1, y - 1);
      if (level == 1u) {
        float4 weighted;
        float4 coloured;
        FirstLevelTexels(texel, source_size, weighted, coloured);
        sum += kernel * weighted;
        fill_sum += kernel * coloured;
      } else {
        const uint2 at = uint2(source_base + clamp(texel.x, 0, source_size.x - 1), clamp(texel.y, 0, source_size.y - 1));
        sum += kernel * target_texture[at];
        if (fill) {
          fill_sum += kernel * second_target[at];
        }
      }
    }
  }
  const uint2 written = uint2(LevelOffset(image, level) + id.x, id.y);
  target_texture[written] = sum;
  if (fill) {
    second_target[written] = fill_sum;
  }
}

// A pixel's difference (DESPIKE: source_texture is `changed`, reference_texture `reference`, neither written in this dispatch).
float3 DifferenceAt(int2 pixel) {
  return source_texture.Load(int3(pixel, 0)).rgb - reference_texture.Load(int3(pixel, 0)).rgb;
}

// Round 5: the group's tile of differences (the 8x8 group and an apron of SPECK_MAX_RADIUS) into groupshared memory, then each pixel's difference clamped to
// the per-channel range of the finite differences on the ring at Chebyshev distance speck_radius (pixels off the canvas are left out); a pixel with none
// left, or a non-finite one, keeps its own. Every thread of the group takes part in the loads and the barrier, those past the canvas included.
void Despike(uint2 group_origin, uint2 local, uint local_index) {
  const int2 tile_origin = int2(group_origin) - SPECK_MAX_RADIUS;
  for (uint entry = local_index; entry < uint(SPECK_TILE * SPECK_TILE); entry += 64u) {
    const int2 pixel = tile_origin + int2(int(entry) % SPECK_TILE, int(entry) / SPECK_TILE);
    float4 value = float4(0.f, 0.f, 0.f, 0.f);
    if (all(pixel >= int2(0, 0)) && pixel.x < int(width) && pixel.y < int(height)) {
      const float3 difference = DifferenceAt(pixel);
      if (all(isfinite(difference))) {
        value = float4(difference, 1.f);
      }
    }
    speck_tile[entry] = value;
  }
  GroupMemoryBarrierWithGroupSync();
  const uint2 id = group_origin + local;
  if (id.x >= width || id.y >= height) return;
  const int radius = int(speck_radius);
  const int2 centre = int2(local) + SPECK_MAX_RADIUS;
  const float4 own = speck_tile[centre.y * SPECK_TILE + centre.x];
  float3 low = float3(1.e30f, 1.e30f, 1.e30f);
  float3 high = -low;
  bool any = false;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      if (max(abs(x), abs(y)) != radius) continue;  // the ring only: everything inside it is excluded
      const float4 ring = speck_tile[(centre.y + y) * SPECK_TILE + centre.x + x];
      if (ring.w == 0.f) continue;
      low = min(low, ring.rgb);
      high = max(high, ring.rgb);
      any = true;
    }
  }
  target_texture[id] = float4((own.w != 0.f && any ? clamp(own.rgb, low, high) : DifferenceAt(int2(id))), 0.f);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID, uint local_index : SV_GroupIndex, uint3 group : SV_GroupID) {
  if (mode == FACES_MODE_DESPIKE) {
    Despike(group.xy * 8u, local.xy, local_index);  // before any early return: the whole group loads its tile
    return;
  }
  if (mode == FACES_MODE_LEVEL) {
    Level(id.xy);
    return;
  }
  if (id.x >= width || id.y >= height) return;
  const float4 changed = target_texture[id.xy];
  if (mode == FACES_MODE_SHOW) {
    const float shown = reference_texture.Load(int3(id.xy, 0)).a;
    target_texture[id.xy] = float4(lerp(changed.rgb, MASK_TINT, MASK_TINT_STRENGTH * saturate(shown)), changed.a);
    return;
  }
  const float broad_coverage = Coverage(id.xy, mask_level);
  float3 reference;
  float mask;
  if (mode == FACES_MODE_FIRST) {
    reference = second_target[id.xy].rgb;
    mask = saturate(coverage_gain * broad_coverage);
    if ((flags & FLAG_FILL) != 0u) {
      // Round 5: the mask grows over skin of the marked skin's colour (M' = max(M, F)).
      const float4 skin = LowPass(fill_texture, uint2(width, height), levels, fill_level, id.xy, false);
      float similarity = 0.f;
      if (skin.z > WEIGHT_FLOOR) {
        similarity = (dot(reference, LUMA_WEIGHTS) >= FILL_BRIGHTNESS_FLOOR
                          ? saturate(1.f - length(Chromaticity(reference) - skin.xy / skin.z) / fill_tolerance)
                          : FILL_DARK_SIMILARITY);
      }
      const float filled = saturate(FILL_GAIN * skin.w) * similarity;
      mask = max(mask, (isfinite(filled) ? filled : 0.f));
    }
    second_target[id.xy] = float4(mask, mask, mask, mask);
  } else {
    reference = extra_texture.Load(int3(id.xy, 0)).rgb;
    mask = reference_texture.Load(int3(id.xy, 0)).a;
  }
  // Fix round 2 (3): below level 1 the local coverage blends toward the pixel's own weight (level 0: pass 1's c, a later pass's M').
  const float3 change = changed.rgb - reference;
  const float own_weight = (mode == FACES_MODE_FIRST ? (all(isfinite(change)) ? FirstWeight(change, reference) : 0.f) : saturate(mask));
  const float local_coverage = (edge_level >= 1.f ? Coverage(id.xy, edge_level) : lerp(own_weight, Coverage(id.xy, 1.f), edge_level));
  const float add_back = (broad_coverage > WEIGHT_FLOOR ? saturate(local_coverage / broad_coverage) : 0.f);
  const float3 removed = (speck_radius != 0u ? detail_texture.Load(int3(id.xy, 0)).rgb : change);  // round 4/5: the despiked difference
  const float3 kept = changed.rgb - mask * (removed - add_back * BroadChange(id.xy));
  target_texture[id.xy] = float4((all(isfinite(kept)) ? kept : changed.rgb), changed.a);
}
