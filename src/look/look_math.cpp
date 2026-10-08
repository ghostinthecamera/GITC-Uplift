#include "look/look_math.hpp"

#include <algorithm>
#include <cmath>

namespace uplift::look {
namespace {

constexpr float SDR_EPSILON = 1.f / 4096.f;      // shaders/change_common.hlsli's
constexpr float HDR_EPSILON = 1.f / 16777216.f;
constexpr float CHANGE_LIMIT = 3.f;
constexpr float TONE_EPSILON = 1.f / 65536.f;    // ℓ_O = log2(Y(O)⁺ + 2^-16)
constexpr float MAX_MODEL_LUMINANCE = 1.f - 1.f / 4096.f;  // Neutwo's inverse diverges at 1
constexpr uint32_t MIN_LEVEL_SIDE = 4u;
constexpr uint32_t MAX_LEVELS = 16u;

Float4 Lerp(Float4 a, Float4 b, float t) {
  return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), a.w + t * (b.w - a.w)};
}

// look_common.hlsli's LevelSample: one level's bilinear sample at work pixel (x, y), or a 3x3 tent of them.
Float4 LevelSample(const std::vector<Float4>& texels, const Atlas& atlas, uint32_t level, uint32_t x, uint32_t y, bool tent) {
  const nr::Size size = LevelSize(atlas.image, level);
  const uint32_t offset = LevelOffset(atlas.image, level);
  const float scale = static_cast<float>(1u << level);
  const float px = (static_cast<float>(x) + 0.5f) / scale - 0.5f;
  const float py = (static_cast<float>(y) + 0.5f) / scale - 0.5f;
  const auto bilinear = [&](float bx, float by) {
    const float cx = std::clamp(bx, 0.f, static_cast<float>(size.width) - 1.f);
    const float cy = std::clamp(by, 0.f, static_cast<float>(size.height) - 1.f);
    const auto x0 = static_cast<uint32_t>(std::floor(cx));
    const auto y0 = static_cast<uint32_t>(std::floor(cy));
    const uint32_t x1 = std::min(x0 + 1u, size.width - 1u);
    const uint32_t y1 = std::min(y0 + 1u, size.height - 1u);
    const auto at = [&](uint32_t tx, uint32_t ty) { return texels[size_t{ty} * atlas.size.width + offset + tx]; };
    const float fx = cx - static_cast<float>(x0);
    return Lerp(Lerp(at(x0, y0), at(x1, y0), fx), Lerp(at(x0, y1), at(x1, y1), fx), cy - static_cast<float>(y0));
  };
  if (!tent || level < 3u) return bilinear(px, py);
  Float4 sum = {};
  for (int32_t j = -1; j <= 1; ++j) {
    for (int32_t i = -1; i <= 1; ++i) {
      const float weight = (i == 0 ? 0.5f : 0.25f) * (j == 0 ? 0.5f : 0.25f);
      const Float4 tap = bilinear(px + static_cast<float>(i), py + static_cast<float>(j));
      sum = {sum.x + weight * tap.x, sum.y + weight * tap.y, sum.z + weight * tap.z, sum.w + weight * tap.w};
    }
  }
  return sum;
}

}  // namespace

bool LookRuns(const LookSettings& settings) {
  return settings.enabled && (settings.shape != ShapeSettings{} || settings.stabilize != StabilizeMode::OFF);
}

float SmoothStep(float low, float high, float value) {
  const float t = std::clamp((value - low) / (high - low), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

float ToneKey(bool sdr, float guide) {
  const float model = std::max(std::exp2(guide) - (sdr ? SDR_EPSILON : HDR_EPSILON), 0.f);
  if (sdr) return std::log2(model + TONE_EPSILON);
  const float bounded = std::min(model, MAX_MODEL_LUMINANCE);
  return std::log2(bounded / std::sqrt(1.f - bounded * bounded) + TONE_EPSILON);
}

ToneWeights WeighTone(float key) {
  const float shadows = 1.f - SmoothStep(-4.5f, -2.5f, key);
  const float highlights = SmoothStep(-1.f, 1.f, key);
  return {.shadows = shadows, .midtones = 1.f - shadows - highlights, .highlights = highlights};
}

ChromaSplit SplitChroma(Float2 delta, Float2 basis) {
  const float magnitude = std::sqrt(basis.x * basis.x + basis.y * basis.y);
  const float weight = SmoothStep(0.01f, 0.02f, magnitude);
  const Float2 direction = (magnitude > 0.f ? Float2{basis.x / magnitude, basis.y / magnitude} : Float2{});
  const float along = delta.x * direction.x + delta.y * direction.y;
  const Float2 radial = {
      weight * along * direction.x + (1.f - weight) * delta.x,
      weight * along * direction.y + (1.f - weight) * delta.y,
  };
  return {.radial = radial, .tangential = {delta.x - radial.x, delta.y - radial.y}};
}

float SoftLimit(float value, float limit) {
  if (limit <= 0.f) return value;
  const float ratio = value / limit;
  return value / std::sqrt(1.f + ratio * ratio);
}

Float2 LimitChroma(Float2 delta, float limit) {
  if (limit <= 0.f) return delta;
  const float ratio = std::sqrt(delta.x * delta.x + delta.y * delta.y) / limit;
  const float root = std::sqrt(1.f + ratio * ratio);
  return {delta.x / root, delta.y / root};
}

ShapedChange ShapeChange(const ShapeInput& input, const ShapeSettings& settings, bool sdr) {
  // Step 4: the broad and fine bands.
  const float banded = settings.tone * input.low + settings.detail * input.high;
  const Float2 chroma = {input.chroma_low.x + input.chroma_high.x, input.chroma_low.y + input.chroma_high.y};
  // Step 5: halo suppression, before the sign split, so Darkening still scales what it leaves.
  const float halo_weight = SmoothStep(1.f, 3.f, input.peak - input.guide);
  const float dehaloed = banded - settings.halo * halo_weight * std::min(banded - input.reference, 0.f);
  // Step 6: brightening and darkening.
  const float split = settings.brighten * std::max(dehaloed, 0.f) + settings.darken * std::min(dehaloed, 0.f);
  // Step 7: colour and hue.
  const ChromaSplit parts = SplitChroma(chroma, input.basis);
  const Float2 recoloured = {
      settings.color * parts.radial.x + settings.hue * parts.tangential.x,
      settings.color * parts.radial.y + settings.hue * parts.tangential.y,
  };
  // Steps 8-9: the tonal weight on the game's own image, then Edit strength.
  const ToneWeights tone = WeighTone(ToneKey(sdr, input.guide));
  const float weight = settings.shadows * tone.shadows + settings.midtones * tone.midtones + settings.highlights * tone.highlights;
  const float strength = settings.edit_strength * weight;
  const float stops = strength * split;
  // Step 10: the soft limits, last, so they hold whatever the multipliers are.
  const float limited = SoftLimit(stops, (stops > 0.f ? settings.max_brighten : settings.max_darken));
  const Float2 chroma_out = LimitChroma({strength * recoloured.x, strength * recoloured.y}, settings.max_color);
  if (!std::isfinite(limited) || !std::isfinite(chroma_out.x) || !std::isfinite(chroma_out.y)) return {};
  // Step 12: the safety clamp.
  return {.stops = std::clamp(limited, -CHANGE_LIMIT, CHANGE_LIMIT), .chroma = chroma_out};
}

nr::Size LevelSize(nr::Size image, uint32_t level) {
  const uint32_t round_up = (1u << level) - 1u;
  return {(image.width + round_up) >> level, (image.height + round_up) >> level};
}

uint32_t LevelOffset(nr::Size image, uint32_t level) {
  uint32_t offset = 0u;
  for (uint32_t below = 1u; below < level; ++below) {
    offset += LevelSize(image, below).width;
  }
  return offset;
}

Atlas MakeAtlas(nr::Size image) {
  uint32_t levels = 1u;
  while (levels < MAX_LEVELS) {
    const nr::Size next = LevelSize(image, levels + 1u);
    if (std::min(next.width, next.height) < MIN_LEVEL_SIDE) break;
    ++levels;
  }
  return {.image = image, .levels = levels, .size = {LevelOffset(image, levels + 1u), LevelSize(image, 1u).height}};
}

Bands MakeBands(float detail_radius, uint32_t height, uint32_t levels) {
  const float sigma = std::max(detail_radius / 100.f * static_cast<float>(height), 0.5f);
  const float top = static_cast<float>(levels);
  const float low_level = std::clamp(std::log2(2.f * sigma), 1.f, top);
  return {
      .low_level = low_level,
      .peak_level = static_cast<uint32_t>(std::clamp(std::ceil(std::log2(sigma)), 1.f, top)),
      .stable_level = static_cast<uint32_t>(std::floor(low_level)),
  };
}

FacesParameters MakeFacesParameters(float lighting_scale, const FacesTuning& tuning, uint32_t height, uint32_t levels) {
  return {.low_level = MakeBands(lighting_scale, height, levels).low_level,
          .mask_level = MakeBands(tuning.softness, height, levels).low_level,
          // Fix round 2 (3): unlike the other bands, down to level 0 (the combine's own per-pixel weights), so the row's low end is finer, not a floor.
          .edge_level = std::clamp(std::log2(2.f * std::max(tuning.edge_falloff / 100.f * static_cast<float>(height), 0.5f)), 0.f, static_cast<float>(levels)),
          .dead_zone = tuning.threshold * FACES_PERCENT,
          .full_weight = std::max(tuning.full, tuning.threshold + FACES_MIN_SPAN) * FACES_PERCENT,
          .coverage_gain = tuning.strength,
          .density = std::max(tuning.density, FACES_MIN_DENSITY),
          .speck_radius = std::min(tuning.speck_size, FACES_MAX_SPECK_RADIUS),
          .fill = tuning.fill_skin,
          .fill_level = MakeBands(tuning.fill_radius, height, levels).low_level,
          .fill_tolerance = std::max(tuning.fill_tolerance, FACES_MIN_FILL_TOLERANCE)};
}

float StabilizeRate(float frame_seconds, float stabilize_ms) {
  const float seconds = std::clamp(frame_seconds, 0.001f, 0.1f);
  return 1.f - std::exp(-seconds / (stabilize_ms / 1000.f));
}

Float4 LowPass(const std::vector<Float4>& atlas_texels, const Atlas& atlas, float level, uint32_t x, uint32_t y, bool tent) {
  const float clamped = std::clamp(level, 1.f, static_cast<float>(atlas.levels));
  const auto first = static_cast<uint32_t>(std::floor(clamped));
  const uint32_t second = std::min(first + 1u, atlas.levels);
  return Lerp(LevelSample(atlas_texels, atlas, first, x, y, tent), LevelSample(atlas_texels, atlas, second, x, y, tent),
              clamped - static_cast<float>(first));
}

float PeakAround(const std::vector<float>& peaks, const Atlas& atlas, uint32_t level, uint32_t x, uint32_t y) {
  const nr::Size size = LevelSize(atlas.image, level);
  const uint32_t offset = LevelOffset(atlas.image, level);
  const float scale = static_cast<float>(1u << level);
  const auto first_x = static_cast<uint32_t>(std::clamp(
      static_cast<int32_t>(std::floor((static_cast<float>(x) + 0.5f) / scale - 0.5f)), 0, static_cast<int32_t>(size.width) - 1));
  const auto first_y = static_cast<uint32_t>(std::clamp(
      static_cast<int32_t>(std::floor((static_cast<float>(y) + 0.5f) / scale - 0.5f)), 0, static_cast<int32_t>(size.height) - 1));
  const uint32_t last_x = std::min(first_x + 1u, size.width - 1u);
  const uint32_t last_y = std::min(first_y + 1u, size.height - 1u);
  const auto at = [&](uint32_t px, uint32_t py) { return peaks[size_t{py} * atlas.size.width + offset + px]; };
  return std::max({at(first_x, first_y), at(last_x, first_y), at(first_x, last_y), at(last_x, last_y)});
}

}  // namespace uplift::look
