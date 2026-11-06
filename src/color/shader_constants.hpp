#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "color/shader_values.h"

namespace uplift::color::shader {

inline constexpr size_t WORD_BYTES = 4u;
inline constexpr size_t ENCODE_WORDS = 16u;
inline constexpr size_t DECODE_WORDS = 16u;
inline constexpr size_t MOTION_WORDS = 16u;
inline constexpr size_t METER_WORDS = 16u;
inline constexpr size_t PYRAMID_WORDS = 4u;
inline constexpr size_t STABILIZE_WORDS = 16u;
inline constexpr size_t SHAPE_WORDS = 20u;
inline constexpr size_t RESOLVE_WORDS = 4u;
inline constexpr size_t FACES_WORDS = 16u;

enum EncodeChangeMode : uint32_t {
  ENCODE_INPUT = 0u,  // Encode the source for NR.
  CHANGE_RECOMPUTED = 1u,  // Extract changes after recomputing the model input.
  CHANGE_PRESERVED = 2u,  // Extract changes using preserved FP16 model input.
};
enum StabilizeMode : uint32_t {
  STABILIZE_CUT = UPLIFT_STABILIZE_CUT,  // Compare scene means.
  STABILIZE_LEVELS = UPLIFT_STABILIZE_LEVELS,  // Update stable low-band levels.
  STABILIZE_DETAIL = UPLIFT_STABILIZE_DETAIL,  // Update the fine-band history.
};
enum StabilizeFlag : uint32_t {
  STABILIZE_FLAG_RESET = UPLIFT_STABILIZE_FLAG_RESET,  // Discard previous history.
  STABILIZE_FLAG_MOTION = UPLIFT_STABILIZE_FLAG_MOTION,  // Reproject and gate history on change.
  STABILIZE_FLAG_VECTORS = UPLIFT_STABILIZE_FLAG_VECTORS,  // Motion vectors are bound.
};
enum FacesMode : uint32_t {
  FACES_MODE_LEVEL = UPLIFT_FACES_MODE_LEVEL,  // Build one pyramid level.
  FACES_MODE_FIRST = UPLIFT_FACES_MODE_FIRST,  // Combine the first pass and write its mask.
  FACES_MODE_LATER = UPLIFT_FACES_MODE_LATER,  // Combine a later pass using that mask.
  FACES_MODE_SHOW = UPLIFT_FACES_MODE_SHOW,  // Tint the displayed mask.
  FACES_MODE_DESPIKE = UPLIFT_FACES_MODE_DESPIKE,  // Despike the pass difference.
};
enum FacesFlag : uint32_t {
  FACES_FLAG_FIRST = UPLIFT_FACES_FLAG_FIRST,  // LEVEL derives weights from the first pass.
  FACES_FLAG_FILL = UPLIFT_FACES_FLAG_FILL,  // Build or consume the fill pyramid.
};
enum ShapeFlag : uint32_t {
  SHAPE_FLAG_SDR = UPLIFT_SHAPE_FLAG_SDR,  // Use SDR tone interpretation.
  SHAPE_FLAG_STABILIZED = UPLIFT_SHAPE_FLAG_STABILIZED,  // Stable low-band history is bound.
  SHAPE_FLAG_DETAIL = UPLIFT_SHAPE_FLAG_DETAIL,  // Stable fine-band history is bound.
};

static_assert(sizeof(uint32_t) == WORD_BYTES && sizeof(float) == WORD_BYTES);
// These scalar layouts mirror the named HLSL cbuffers; value initialization zeroes reserved words.

// encode_cs.hlsl: EncodeConstants.
struct EncodeConstants {
  uint32_t encoding{};  // Resolved color::Encoding value.
  uint32_t width{};  // Work image width after source-region resampling.
  uint32_t height{};
  float input_scale{};  // Linear input multiplier before model encoding.
  uint32_t second_output{};  // Clear motion in encode mode; write input chroma in change mode.
  uint32_t has_exposure{};  // Whether the game exposure input is present.
  float exposure_factor{};  // DLSS exposure scale divided by pre-exposure.
  uint32_t source_x{};  // Source region origin.
  uint32_t source_y{};  // Source region origin.
  uint32_t source_width{};
  uint32_t source_height{};
  uint32_t canvas_width{};  // Model image width including mirrored padding.
  uint32_t canvas_height{};  // Model image height including mirrored padding.
  uint32_t change_mode{};  // EncodeChangeMode selects encode or change-field extraction.
  uint32_t options{};  // shader_options flags.
  uint32_t reserved0{};
};
static_assert(std::is_standard_layout_v<EncodeConstants> && std::is_trivially_copyable_v<EncodeConstants>);
static_assert(sizeof(EncodeConstants) == ENCODE_WORDS * WORD_BYTES);
static_assert(offsetof(EncodeConstants, encoding) == 0u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, width) == 1u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, height) == 2u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, input_scale) == 3u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, second_output) == 4u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, has_exposure) == 5u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, exposure_factor) == 6u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, source_x) == 7u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, source_y) == 8u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, source_width) == 9u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, source_height) == 10u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, canvas_width) == 11u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, canvas_height) == 12u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, change_mode) == 13u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, options) == 14u * WORD_BYTES);
static_assert(offsetof(EncodeConstants, reserved0) == 15u * WORD_BYTES);

// decode_common.hlsli: DecodeConstants.
struct DecodeConstants {
  uint32_t encoding{};  // Resolved color::Encoding value.
  uint32_t target_srgb{};  // The pixel-shader RTV applies sRGB encoding to linear output.
  float input_scale{};  // Linear input multiplier used by the matching encode.
  float transfer_strength{};  // Strength of the luminance change.
  float color_strength{};  // Strength of the chroma change.
  uint32_t has_exposure{};  // Whether the game exposure input is present.
  float exposure_factor{};  // DLSS exposure scale divided by pre-exposure.
  uint32_t origin_x{};  // Compute decode destination-region origin.
  uint32_t origin_y{};  // Compute decode destination-region origin.
  uint32_t width{};  // Output region width.
  uint32_t height{};
  uint32_t upsampling{};  // Direct reads or the encoded resampling choice.
  uint32_t source_x{};  // Source-region origin.
  uint32_t source_y{};  // Source-region origin.
  uint32_t options{};  // shader_options flags, including the optional mask.
  float chroma_clamp{};  // Chroma-change limit in stops; zero disables it.
};
static_assert(std::is_standard_layout_v<DecodeConstants> && std::is_trivially_copyable_v<DecodeConstants>);
static_assert(sizeof(DecodeConstants) == DECODE_WORDS * WORD_BYTES);
static_assert(offsetof(DecodeConstants, encoding) == 0u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, target_srgb) == 1u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, input_scale) == 2u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, transfer_strength) == 3u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, color_strength) == 4u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, has_exposure) == 5u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, exposure_factor) == 6u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, origin_x) == 7u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, origin_y) == 8u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, width) == 9u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, height) == 10u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, upsampling) == 11u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, source_x) == 12u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, source_y) == 13u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, options) == 14u * WORD_BYTES);
static_assert(offsetof(DecodeConstants, chroma_clamp) == 15u * WORD_BYTES);

// motion_cs.hlsl: MotionConstants.
struct MotionConstants {
  uint32_t region_x{};  // Source vector-region origin.
  uint32_t region_y{};  // Source vector-region origin.
  uint32_t region_width{};
  uint32_t region_height{};
  uint32_t image_width{};  // Destination image width before padding.
  uint32_t image_height{};  // Destination image height before padding.
  uint32_t canvas_width{};  // Destination width including mirrored padding.
  uint32_t canvas_height{};  // Destination height including mirrored padding.
  float scale_x{};  // Horizontal vector multiplier into destination pixels.
  float scale_y{};  // Vertical vector multiplier into destination pixels.
  uint32_t flip_y{};  // Whether source and destination have opposite vertical orientation.
  float offset_x{};  // Horizontal jitter compensation after scaling, before flipping.
  float offset_y{};  // Vertical jitter compensation after scaling, before flipping.
  uint32_t reserved3{};
  uint32_t reserved4{};
  uint32_t reserved5{};
};
static_assert(std::is_standard_layout_v<MotionConstants> && std::is_trivially_copyable_v<MotionConstants>);
static_assert(sizeof(MotionConstants) == MOTION_WORDS * WORD_BYTES);
static_assert(offsetof(MotionConstants, region_x) == 0u * WORD_BYTES);
static_assert(offsetof(MotionConstants, region_y) == 1u * WORD_BYTES);
static_assert(offsetof(MotionConstants, region_width) == 2u * WORD_BYTES);
static_assert(offsetof(MotionConstants, region_height) == 3u * WORD_BYTES);
static_assert(offsetof(MotionConstants, image_width) == 4u * WORD_BYTES);
static_assert(offsetof(MotionConstants, image_height) == 5u * WORD_BYTES);
static_assert(offsetof(MotionConstants, canvas_width) == 6u * WORD_BYTES);
static_assert(offsetof(MotionConstants, canvas_height) == 7u * WORD_BYTES);
static_assert(offsetof(MotionConstants, scale_x) == 8u * WORD_BYTES);
static_assert(offsetof(MotionConstants, scale_y) == 9u * WORD_BYTES);
static_assert(offsetof(MotionConstants, flip_y) == 10u * WORD_BYTES);
static_assert(offsetof(MotionConstants, offset_x) == 11u * WORD_BYTES);
static_assert(offsetof(MotionConstants, offset_y) == 12u * WORD_BYTES);
static_assert(offsetof(MotionConstants, reserved3) == 13u * WORD_BYTES);
static_assert(offsetof(MotionConstants, reserved4) == 14u * WORD_BYTES);
static_assert(offsetof(MotionConstants, reserved5) == 15u * WORD_BYTES);

// meter_cs.hlsl: MeterConstants.
struct MeterConstants {
  uint32_t encoding{};  // Resolved color::Encoding value.
  uint32_t primaries{};  // SourcePrimaries selection.
  float input_scale{};  // Linear input multiplier used by the matching encode.
  uint32_t source_x{};  // Metered source-region origin.
  uint32_t source_y{};  // Metered source-region origin.
  uint32_t source_width{};
  uint32_t source_height{};
  uint32_t snap{};  // Start the state at its new target without adaptation.
  uint32_t smooth{};  // Whether the exposure target is adapted over time.
  float brighter_rate{};  // Stops per second toward a darker exposure.
  float darker_rate{};  // Stops per second toward a brighter exposure.
  float frame_seconds{};  // Elapsed frame time in seconds.
  uint32_t probe{};  // Write the game-exposure comparison texel.
  uint32_t has_game_texture{};  // Whether the game exposure texture is present.
  float game_factor{};  // DLSS exposure scale divided by pre-exposure.
  float game_weight{};  // Game contribution to the exposure multiplier, in stops.
};
static_assert(std::is_standard_layout_v<MeterConstants> && std::is_trivially_copyable_v<MeterConstants>);
static_assert(sizeof(MeterConstants) == METER_WORDS * WORD_BYTES);
static_assert(offsetof(MeterConstants, encoding) == 0u * WORD_BYTES);
static_assert(offsetof(MeterConstants, primaries) == 1u * WORD_BYTES);
static_assert(offsetof(MeterConstants, input_scale) == 2u * WORD_BYTES);
static_assert(offsetof(MeterConstants, source_x) == 3u * WORD_BYTES);
static_assert(offsetof(MeterConstants, source_y) == 4u * WORD_BYTES);
static_assert(offsetof(MeterConstants, source_width) == 5u * WORD_BYTES);
static_assert(offsetof(MeterConstants, source_height) == 6u * WORD_BYTES);
static_assert(offsetof(MeterConstants, snap) == 7u * WORD_BYTES);
static_assert(offsetof(MeterConstants, smooth) == 8u * WORD_BYTES);
static_assert(offsetof(MeterConstants, brighter_rate) == 9u * WORD_BYTES);
static_assert(offsetof(MeterConstants, darker_rate) == 10u * WORD_BYTES);
static_assert(offsetof(MeterConstants, frame_seconds) == 11u * WORD_BYTES);
static_assert(offsetof(MeterConstants, probe) == 12u * WORD_BYTES);
static_assert(offsetof(MeterConstants, has_game_texture) == 13u * WORD_BYTES);
static_assert(offsetof(MeterConstants, game_factor) == 14u * WORD_BYTES);
static_assert(offsetof(MeterConstants, game_weight) == 15u * WORD_BYTES);

// look_pyramid_cs.hlsl: PyramidConstants.
struct PyramidConstants {
  uint32_t image_width{};
  uint32_t image_height{};
  uint32_t level{};  // Level written by this dispatch; the preceding level is read.
  uint32_t reserved0{};
};
static_assert(std::is_standard_layout_v<PyramidConstants> && std::is_trivially_copyable_v<PyramidConstants>);
static_assert(sizeof(PyramidConstants) == PYRAMID_WORDS * WORD_BYTES);
static_assert(offsetof(PyramidConstants, image_width) == 0u * WORD_BYTES);
static_assert(offsetof(PyramidConstants, image_height) == 1u * WORD_BYTES);
static_assert(offsetof(PyramidConstants, level) == 2u * WORD_BYTES);
static_assert(offsetof(PyramidConstants, reserved0) == 3u * WORD_BYTES);

// look_stabilize_cs.hlsl: StabilizeConstants.
struct StabilizeConstants {
  uint32_t image_width{};
  uint32_t image_height{};
  uint32_t levels{};  // Number of levels in the atlas.
  uint32_t mode{};  // StabilizeMode pass selection.
  uint32_t level{};  // Lower of the two stable levels used by LEVELS.
  uint32_t flags{};  // StabilizeFlag bits.
  float rate{};  // Blend coefficient from look::StabilizeRate.
  float low_level{};  // Fractional low-band pyramid level used by DETAIL.
  float motion_scale_x{};  // Horizontal vector multiplier into motion-rect pixels.
  float motion_scale_y{};  // Vertical vector multiplier into motion-rect pixels.
  uint32_t motion_x{};  // Motion-vector rectangle origin.
  uint32_t motion_y{};  // Motion-vector rectangle origin.
  uint32_t motion_width{};  // Motion-vector rectangle width; unit extent without vectors.
  uint32_t motion_height{};  // Motion-vector rectangle height; unit extent without vectors.
  uint32_t reserved0{};
  uint32_t reserved1{};
};
static_assert(std::is_standard_layout_v<StabilizeConstants> && std::is_trivially_copyable_v<StabilizeConstants>);
static_assert(sizeof(StabilizeConstants) == STABILIZE_WORDS * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, image_width) == 0u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, image_height) == 1u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, levels) == 2u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, mode) == 3u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, level) == 4u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, flags) == 5u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, rate) == 6u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, low_level) == 7u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, motion_scale_x) == 8u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, motion_scale_y) == 9u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, motion_x) == 10u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, motion_y) == 11u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, motion_width) == 12u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, motion_height) == 13u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, reserved0) == 14u * WORD_BYTES);
static_assert(offsetof(StabilizeConstants, reserved1) == 15u * WORD_BYTES);

// look_shape_cs.hlsl: ShapeConstants.
struct ShapeConstants {
  uint32_t image_width{};
  uint32_t image_height{};
  uint32_t levels{};  // Number of levels in the atlas.
  uint32_t flags{};  // ShapeFlag bits.
  float low_level{};  // Fractional low-band pyramid level.
  uint32_t peak_level{};  // Max-pyramid level used for halo suppression.
  float tone{};  // Large-scale tone multiplier.
  float detail{};  // Fine-detail multiplier.
  float halo{};  // Halo-suppression amount.
  float brighten{};  // Brightening multiplier.
  float darken{};  // Darkening multiplier.
  float color{};  // Chroma movement toward or away from grey.
  float hue{};  // Hue-turn multiplier.
  float shadows{};  // Shadow-band multiplier.
  float midtones{};  // Midtone-band multiplier.
  float highlights{};  // Highlight-band multiplier.
  float edit_strength{};  // Overall edit multiplier.
  float max_brighten{};  // Brightening limit in stops; zero disables it.
  float max_darken{};  // Darkening limit in stops; zero disables it.
  float max_color{};  // Normalized chroma limit; zero disables it.
};
static_assert(std::is_standard_layout_v<ShapeConstants> && std::is_trivially_copyable_v<ShapeConstants>);
static_assert(sizeof(ShapeConstants) == SHAPE_WORDS * WORD_BYTES);
static_assert(offsetof(ShapeConstants, image_width) == 0u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, image_height) == 1u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, levels) == 2u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, flags) == 3u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, low_level) == 4u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, peak_level) == 5u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, tone) == 6u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, detail) == 7u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, halo) == 8u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, brighten) == 9u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, darken) == 10u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, color) == 11u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, hue) == 12u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, shadows) == 13u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, midtones) == 14u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, highlights) == 15u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, edit_strength) == 16u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, max_brighten) == 17u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, max_darken) == 18u * WORD_BYTES);
static_assert(offsetof(ShapeConstants, max_color) == 19u * WORD_BYTES);

// resolve_cs.hlsl: ResolveConstants.
struct ResolveConstants {
  uint32_t width{};
  uint32_t height{};
  float transfer_strength{};  // Strength of this pass's luminance change.
  float color_strength{};  // Strength of this pass's chroma change.
};
static_assert(std::is_standard_layout_v<ResolveConstants> && std::is_trivially_copyable_v<ResolveConstants>);
static_assert(sizeof(ResolveConstants) == RESOLVE_WORDS * WORD_BYTES);
static_assert(offsetof(ResolveConstants, width) == 0u * WORD_BYTES);
static_assert(offsetof(ResolveConstants, height) == 1u * WORD_BYTES);
static_assert(offsetof(ResolveConstants, transfer_strength) == 2u * WORD_BYTES);
static_assert(offsetof(ResolveConstants, color_strength) == 3u * WORD_BYTES);

// faces_cs.hlsl: FacesConstants.
struct FacesConstants {
  uint32_t width{};  // Canvas width used by NR.
  uint32_t height{};
  uint32_t levels{};  // Number of levels in the atlas.
  uint32_t mode{};  // FacesMode pass selection.
  uint32_t level{};  // Pyramid level written by LEVEL.
  uint32_t flags{};  // FacesFlag bits.
  float low_level{};  // Fractional pyramid level for the lighting scale.
  float mask_level{};  // Pyramid level used for mask softness.
  float edge_level{};  // Pyramid level used for edge falloff.
  float dead_zone{};  // Change threshold as a fraction of pixel brightness.
  float full_weight{};  // Change fraction at which protection reaches full weight.
  float coverage_gain{};  // Multiplier applied to mask coverage.
  float density{};  // Coverage needed for full lighting add-back.
  uint32_t speck_radius{};  // Despeckle ring's Chebyshev distance in pixels.
  float fill_level{};  // Pyramid level used for skin fill.
  float fill_tolerance{};  // Chroma distance at which fill similarity reaches zero.
};
static_assert(std::is_standard_layout_v<FacesConstants> && std::is_trivially_copyable_v<FacesConstants>);
static_assert(sizeof(FacesConstants) == FACES_WORDS * WORD_BYTES);
static_assert(offsetof(FacesConstants, width) == 0u * WORD_BYTES);
static_assert(offsetof(FacesConstants, height) == 1u * WORD_BYTES);
static_assert(offsetof(FacesConstants, levels) == 2u * WORD_BYTES);
static_assert(offsetof(FacesConstants, mode) == 3u * WORD_BYTES);
static_assert(offsetof(FacesConstants, level) == 4u * WORD_BYTES);
static_assert(offsetof(FacesConstants, flags) == 5u * WORD_BYTES);
static_assert(offsetof(FacesConstants, low_level) == 6u * WORD_BYTES);
static_assert(offsetof(FacesConstants, mask_level) == 7u * WORD_BYTES);
static_assert(offsetof(FacesConstants, edge_level) == 8u * WORD_BYTES);
static_assert(offsetof(FacesConstants, dead_zone) == 9u * WORD_BYTES);
static_assert(offsetof(FacesConstants, full_weight) == 10u * WORD_BYTES);
static_assert(offsetof(FacesConstants, coverage_gain) == 11u * WORD_BYTES);
static_assert(offsetof(FacesConstants, density) == 12u * WORD_BYTES);
static_assert(offsetof(FacesConstants, speck_radius) == 13u * WORD_BYTES);
static_assert(offsetof(FacesConstants, fill_level) == 14u * WORD_BYTES);
static_assert(offsetof(FacesConstants, fill_tolerance) == 15u * WORD_BYTES);

// Borrow the scalar object representation for a synchronous constant upload.
template<class Constants>
std::span<const std::byte> Bytes(const Constants& constants) {
  return std::as_bytes(std::span{&constants, size_t{1}});
}

}  // namespace uplift::color::shader
