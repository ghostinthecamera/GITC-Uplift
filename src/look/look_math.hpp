#pragma once

#include <cstdint>
#include <vector>

#include "nr/types.hpp"

namespace uplift::look {

// v2 design §3.12: the Stabilize combo, in stored-index order.
enum class StabilizeMode : uint32_t {
  OFF = 0u,
  STATIC = 1u,  // a moving average in place: photo modes and still scenes
  MOTION = 2u,  // reprojected along the motion vectors; without them, change-gated static (decision D4)
};

// v2 design §3.11/§3.12's multipliers and limits, in the order of operations.
struct ShapeSettings {
  float tone = 1.f;           // k_T, Large-scale tone
  float detail = 1.f;         // k_F, Fine detail
  float halo = 0.f;           // h, Halo suppression
  float brighten = 1.f;       // k_B
  float darken = 1.f;         // k_D
  float color = 1.f;          // k_C: movement toward or away from grey
  float hue = 1.f;            // k_H: the hue turn
  float shadows = 1.f;        // k_sh
  float midtones = 1.f;       // k_mid
  float highlights = 1.f;     // k_hi
  float edit_strength = 1.f;  // k_E
  float max_brighten = 0.f;   // M_B, stops; 0 = no limit
  float max_darken = 0.f;     // M_D, stops; 0 = no limit
  float max_color = 0.f;      // M_C, normalised chroma; 0 = no limit
  friend bool operator==(const ShapeSettings&, const ShapeSettings&) = default;
};

// Shape result and its rows (v2 design §5 "Look · Result", "Limits and stability"). All live: none restarts NR's history.
struct LookSettings {
  bool enabled = false;  // ShapeResult: off leaves NR's output exactly as Plan 4's
  ShapeSettings shape;
  float detail_radius = 2.f;  // % of the work image's height: where broad change ends
  StabilizeMode stabilize = StabilizeMode::OFF;
  float stabilize_ms = 250.f;  // τ
  bool stabilize_detail = false;
  friend bool operator==(const LookSettings&, const LookSettings&) = default;
};

// Whether the look stage runs: Shape result is on and a control would change NR's result.
[[nodiscard]] bool LookRuns(const LookSettings& settings);

struct Float2 { float x = 0.f; float y = 0.f; };
struct Float4 { float x = 0.f; float y = 0.f; float z = 0.f; float w = 0.f; };

// CPU twin of shaders/look_common.hlsli: same functions, same maths. tests/gpu/look_gpu_test.cpp compares them.
[[nodiscard]] float SmoothStep(float low, float high, float value);  // HLSL's smoothstep
// ℓ_O, the tone key in scene stops against diffuse white, from the change field's guide ℓ_M (amendment 3):
// Neutwo's luminance inverse on HDR paths (encode's gamut clip keeps luminance), the value itself on SDR.
[[nodiscard]] float ToneKey(bool sdr, float guide);
struct ToneWeights { float shadows = 0.f; float midtones = 0.f; float highlights = 0.f; };
[[nodiscard]] ToneWeights WeighTone(float key);  // v2 design §3.11's tonal bands; they sum to 1
struct ChromaSplit { Float2 radial; Float2 tangential; };  // R(δ): toward or away from grey; T(δ): the hue turn
// v2 design §3.11: δ split along NR's input chroma κ_M; near grey only saturation is meaningful, so all of it is radial.
[[nodiscard]] ChromaSplit SplitChroma(Float2 delta, Float2 basis);
// v2 design §3.12's soft limit, value / sqrt(1 + (value/limit)²): 0.71·limit at the limit; 0 = no limit.
[[nodiscard]] float SoftLimit(float value, float limit);
[[nodiscard]] Float2 LimitChroma(Float2 delta, float limit);

// One pixel's inputs to the shape pass, after v2 design §3.11 steps 1-3.
struct ShapeInput {
  float low = 0.f;        // ã_low: the broad change (stabilised with Stabilize on), stops
  float high = 0.f;       // ã_high: the fine change a − a_low (stabilised with Stabilize detail)
  Float2 chroma_low;      // δ̃_low
  Float2 chroma_high;     // δ̃_high
  float reference = 0.f;  // a_ref: the halo's wider band
  float peak = 0.f;       // P_max: the brightest ℓ_M within the radius
  float guide = 0.f;      // ℓ_M
  Float2 basis;           // κ_M: NR's input chroma
};
struct ShapedChange { float stops = 0.f; Float2 chroma; };  // (a*, δ*)
// v2 design §3.11 steps 4-10 and 12 (step 11, the mask, is the compose's). A non-finite result is no change.
[[nodiscard]] ShapedChange ShapeChange(const ShapeInput& input, const ShapeSettings& settings, bool sdr);

// The pyramid's levels 1..levels side by side in one atlas texture: level k is ceil(image / 2^k), at x = the widths
// of levels 1..k-1, y = 0 (level 0 is the image). look_common.hlsli derives the same layout.
[[nodiscard]] nr::Size LevelSize(nr::Size image, uint32_t level);
[[nodiscard]] uint32_t LevelOffset(nr::Size image, uint32_t level);
struct Atlas { nr::Size image; uint32_t levels = 0u; nr::Size size; };  // levels: down to a shorter side of 4, at least 1
[[nodiscard]] Atlas MakeAtlas(nr::Size image);

// Where the bands sit for Detail radius (% of height) on a work image of `height` rows (v2 design §3.12, amendment 4):
// λ = log2(2σ) clamped to [1, levels]; the halo's max level ceil(log2 σ); the stabiliser's floor(λ) (and the next).
struct Bands { float low_level = 1.f; uint32_t peak_level = 1u; uint32_t stable_level = 1u; };
[[nodiscard]] Bands MakeBands(float detail_radius, uint32_t height, uint32_t levels);
// Keep faces fix round 1 addendum: the face mask's tuning, the Advanced rows (owner's request: tuned live in game). Shader constants only: none restarts NR or
// the twin.
// Fix round 3 (the owner's glowing spots): the threshold and full protection are relative, % of the pixel's own brightness (faces_cs.hlsl's FirstWeight), so
// a few percent of NR's noise on a specular glint no longer passes them; and the lighting is added back only where the weights are dense (`density`).
struct FacesTuning {
  float threshold = 1.5f;      // FaceMaskThreshold: a change below this % of the pixel's brightness is the two runs' noise
  float full = 6.f;            // FaceMaskFull: from this % on it protects fully (kept above `threshold` by MakeFacesParameters)
  float strength = 2.f;        // FaceMaskStrength: the mask's gain on its coverage (2: a half-covered neighbourhood reads as inside)
  float softness = 0.5f;       // FaceMaskSoftness: the mask's radius, % of the height
  float edge_falloff = 0.15f;  // FaceEdgeFalloff: the light blur of the coverage that fades kept lighting out at the outline (I1), % of the height
  float density = 0.3f;        // FaceLightingDensity: the weights' coverage at the Lighting scale from which lighting is added back in full
  // Round 5: FaceSpeckSize, the despike ring's Chebyshev distance in pixels (spikes up to 2r - 1 px wide keep NR's normal result); 0: off.
  uint32_t speck_size = 2u;
  bool fill_skin = true;         // FaceFillSkin (round 5): the mask grows over skin of the marked skin's colour
  float fill_radius = 3.f;       // FaceFillRadius: how far it grows, % of the height
  float fill_tolerance = 0.04f;  // FaceFillTolerance: the rg-chromaticity distance from the local skin colour at which it stops
  friend bool operator==(const FacesTuning&, const FacesTuning&) = default;
};
// Keep faces (2026-10-08): faces_cs.hlsl's parameters on a canvas of `levels` levels for a work image of `height` rows. The levels are MakeBands' λ:
// `low_level` for Lighting scale (% of the height), the band above which a change on characters counts as lighting; `mask_level` for the mask's softness,
// where the coverage of the character's pixels is read; `edge_level` for the edge falloff (fix round 1, I1), the only one that goes down to 0 (the per-pixel
// weights; fix round 2). The weights' dead zone and full weight are fractions of the pixel's brightness (fix round 3), the full weight at least
// FACES_MIN_SPAN percent above the dead zone (the user's own values are left as they are); `density` is FacesTuning's, kept above FACES_MIN_DENSITY.
inline constexpr float FACES_PERCENT = 0.01f;
inline constexpr float FACES_MIN_SPAN = 0.25f;  // in percent
inline constexpr float FACES_MIN_DENSITY = 0.01f;
inline constexpr uint32_t FACES_MAX_SPECK_RADIUS = 3u;  // faces_cs.hlsl's despike tile apron (SPECK_MAX_RADIUS)
inline constexpr float FACES_MIN_FILL_TOLERANCE = 0.001f;
struct FacesParameters {
  float low_level = 1.f;
  float mask_level = 1.f;
  float edge_level = 1.f;
  float dead_zone = 1.5f * FACES_PERCENT;
  float full_weight = 6.f * FACES_PERCENT;
  float coverage_gain = 2.f;
  float density = 0.3f;
  uint32_t speck_radius = 2u;    // FacesTuning::speck_size, at most FACES_MAX_SPECK_RADIUS; 0: no despike
  bool fill = true;              // FacesTuning::fill_skin
  float fill_level = 1.f;        // MakeBands' λ for the Fill radius
  float fill_tolerance = 0.04f;  // at least FACES_MIN_FILL_TOLERANCE
};
[[nodiscard]] FacesParameters MakeFacesParameters(float lighting_scale, const FacesTuning& tuning, uint32_t height, uint32_t levels);
// α = 1 − exp(−Δt/τ), with Δt clamped to [1 ms, 100 ms]: frame-rate independent smoothing.
[[nodiscard]] float StabilizeRate(float frame_seconds, float stabilize_ms);

// look_common.hlsli's LowPass at work pixel (x, y) of an atlas (row-major texels): trilinear between floor(level) and
// the next level, each a bilinear sample, or with `tent` a 3x3 tent of them from level 3 up.
[[nodiscard]] Float4 LowPass(const std::vector<Float4>& atlas_texels, const Atlas& atlas, float level, uint32_t x, uint32_t y,
                             bool tent);
// The largest of the 2x2 texels of `level` around work pixel (x, y): the halo's P_max.
[[nodiscard]] float PeakAround(const std::vector<float>& peaks, const Atlas& atlas, uint32_t level, uint32_t x, uint32_t y);

}  // namespace uplift::look
