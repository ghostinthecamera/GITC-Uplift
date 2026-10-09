#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "color/encoding.hpp"
#include "look/look_math.hpp"
#include "nr/types.hpp"
#include "vk/nr_functions.hpp"
#include "vk/nr_image.hpp"

namespace uplift::color {

// An image view read as a sampled image, and the layout the image is in for the read: GENERAL for Uplift's own images, SHADER_READ_ONLY_OPTIMAL for the
// game's (design R79). A null view binds the placeholder.
struct VkSampledView {
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};

// The encode (v2 design §3.4 step 3): the source region into the model texture A.
struct VkEncodePass {
  // After DLSS: DLSS's Output in place, a storage view in GENERAL of `source_format` (one of OutputVariantOf's); Before upscaling: the game's Color as a
  // sampled view (`source_format` VK_FORMAT_UNDEFINED, `source_layout` its layout).
  VkImageView source = VK_NULL_HANDLE;
  VkImageLayout source_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkFormat source_format = VK_FORMAT_UNDEFINED;
  VkImageView model = VK_NULL_HANDLE;   // A: RGBA16F storage, GENERAL
  VkImageView motion = VK_NULL_HANDLE;  // optional RG16F storage, GENERAL: written with zeros when set (MotionVectors = None)
  uint32_t width = 0u;                  // the work image: the region resampled to this size
  uint32_t height = 0u;
  Encoding encoding = Encoding::SRGB;  // resolved, never AUTO
  float input_scale = 1.f;
  VkSampledView exposure;       // optional 1x1 exposure texture
  float exposure_factor = 1.f;  // DLSS.Exposure.Scale / DLSS.Pre.Exposure
  nr::Rect region;              // the part of `source` resampled to the image; all zero = width x height at (0, 0)
  nr::Size canvas;              // the model texture: the image plus mirrored padding; empty = the image
  uint32_t options = 0u;        // shader_options (the primaries, the near-black guard)
};

// The decode (v2 design §3.4 step 5): NR's output at Full, or (Plan 14) the change field composed with the original pixel.
struct VkDecodePass {
  // In place (After DLSS): `source` is empty and `target` is DLSS's Output, a storage view in GENERAL of `target_format` (one of OutputVariantOf's) that
  // also holds the original pixels. Otherwise (Before upscaling): `source` is the game's Color, sampled, and `target` is Uplift's RGBA16F private colour.
  VkSampledView source;
  VkImageView target = VK_NULL_HANDLE;
  VkFormat target_format = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkImageView nr_output = VK_NULL_HANDLE;  // NR's output (A or B), or the change field: RGBA16F, GENERAL
  uint32_t origin_x = 0u;                  // the region's top-left corner in the target
  uint32_t origin_y = 0u;
  uint32_t width = 0u;  // the region: the output size
  uint32_t height = 0u;
  Encoding encoding = Encoding::SRGB;
  float input_scale = 1.f;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  VkSampledView exposure;
  float exposure_factor = 1.f;
  uint32_t source_x = 0u;  // the region's top-left corner in the source (in place: the target)
  uint32_t source_y = 0u;
  std::optional<Upsampling> upsampling;  // Plan 14: set: `nr_output` is the change field at the work size
  uint32_t options = 0u;                 // shader_options (the primaries, Consistent)
  float chroma_clamp = 0.f;              // Plan 14 (F14): stops; 0 = off
  VkSampledView mask;                    // Plan 14 (§3.13): the mask copy, SHADER_READ_ONLY_OPTIMAL; null = none
};

// Amendment 7 and v2 design §3.9: the game's motion vectors into an Uplift RG16F image, in destination pixels (Before upscaling's canvas).
struct VkMotionPass {
  VkSampledView source;                 // the game's motion vectors
  nr::Rect region;                      // their region in `source`
  VkImageView target = VK_NULL_HANDLE;  // RG16F storage, GENERAL
  nr::Size image;                       // the region resampled to this size, at the target's origin
  nr::Size canvas;                      // the target's size: the image plus mirrored padding
  float scale_x = 1.f;                  // multiplies the vectors into target pixels
  float scale_y = 1.f;
  bool flip_y = false;                  // 1.1.6: `source` is upside down against `target` (motion_cs.hlsl)
};

// Plan 14 (v2 design §3.7): NR's change field at the work image size, from the model input recomputed exactly as `encode` computed it (a later pass may
// have overwritten A) and NR's output: the encode in its change mode.
struct VkChangePass {
  VkEncodePass encode;                     // the encode that fed NR; its model and motion are not used
  VkImageView nr_output = VK_NULL_HANDLE;  // RGBA16F model domain, GENERAL, the canvas size
  VkImageView change = VK_NULL_HANDLE;     // RGBA16F storage, GENERAL, the image size
  VkImageView basis = VK_NULL_HANDLE;      // RG16F storage, GENERAL: NR's input chroma κ_M, for the look; null = none
};

// Plan 14 (v2 design §3.12): the Gaussian pyramid of C and the max pyramid of ℓ_M, levels 1..K, in two atlases. Every image GENERAL.
struct VkPyramidPass {
  VkImageView change = VK_NULL_HANDLE;  // C, sampled
  VkImageView gauss = VK_NULL_HANDLE;   // RGBA16F storage atlas
  VkImageView peaks = VK_NULL_HANDLE;   // R16F storage atlas
  look::Atlas atlas;
};

// The motion vectors the stabiliser reprojects along (current -> previous); a null view is none.
struct VkStabilizeMotion {
  VkSampledView vectors;  // the game's, SHADER_READ_ONLY_OPTIMAL
  nr::Rect rect;          // resolved and non-empty while `vectors` is set
  float scale_x = 1.f;    // into the pixels of their rect
  float scale_y = 1.f;
};

// Plan 14 (v2 design §3.12, amendment 4): RecordStabilize runs the scene cut and the two levels the low band reads; RecordDetail runs the fine band at the
// work size. Every image GENERAL.
struct VkStabilizePass {
  VkImageView gauss = VK_NULL_HANDLE;     // sampled
  VkImageView change = VK_NULL_HANDLE;    // RecordDetail: C, sampled
  VkImageView previous = VK_NULL_HANDLE;  // last frame's history (the level atlas, or the detail), sampled
  VkImageView current = VK_NULL_HANDLE;   // this frame's, RGBA16F storage
  VkImageView state = VK_NULL_HANDLE;     // 1x1 RGBA32F scene-cut state, storage
  look::Atlas atlas;
  look::Bands bands;         // stable_level for RecordStabilize, low_level for RecordDetail
  float rate = 1.f;          // look::StabilizeRate
  bool motion_mode = false;  // Stabilize = Motion
  bool reset = false;
  VkStabilizeMotion motion;
};

// Plan 14 (v2 design §3.11/§3.12): C rewritten in place with the shaped change. Every image GENERAL.
struct VkShapePass {
  VkImageView change = VK_NULL_HANDLE;   // C, RGBA16F storage
  VkImageView basis = VK_NULL_HANDLE;    // κ_M, sampled
  VkImageView gauss = VK_NULL_HANDLE;    // sampled
  VkImageView peaks = VK_NULL_HANDLE;    // sampled
  VkImageView history = VK_NULL_HANDLE;  // this frame's history atlas; null: Stabilize off
  VkImageView detail = VK_NULL_HANDLE;   // this frame's detail history; null: Stabilize detail off
  look::Atlas atlas;
  look::Bands bands;
  look::ShapeSettings settings;
  bool sdr = false;
};

// Plan 14 Task 10 (v2 design §3.14, amendment 2): the exposure meter and governor, one thread group before the encode. The state image's x is the encode's
// and the decode's exposure multiplier.
struct VkMeterPass {
  // Read as the encode reads its source: DLSS's Output in place (a storage view in GENERAL of `source_format`), or the game's Color as a sampled view
  // (`source_format` VK_FORMAT_UNDEFINED, `source_layout` its layout).
  VkImageView source = VK_NULL_HANDLE;
  VkImageLayout source_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkFormat source_format = VK_FORMAT_UNDEFINED;
  nr::Rect region;  // the part of `source` the encode reads
  Encoding encoding = Encoding::LINEAR_BT709;
  uint32_t primaries = 0u;  // the SourcePrimaries index (Primaries; 0 = automatic)
  float input_scale = 1.f;
  VkImageView state = VK_NULL_HANDLE;  // 2x1 RGBA32F storage, GENERAL: (2^E, E, the last anchor, set) at (0, 0)
  bool snap = false;
  bool smooth = true;
  float brighter_rate = 2.f;
  float darker_rate = 0.7f;
  float frame_seconds = 1.f / 60.f;
  // Plan 17: Input exposure = Auto's check, as Direct3D 12's MeterPass: the game's exposure (a sampled view, or none) and its target into texel (1, 0).
  bool probe = false;
  VkSampledView game_exposure;
  float game_exposure_factor = 1.f;
  float game_weight = 0.f;  // 2026-10-09, Auto's Blend (with `probe`), as Direct3D 12's MeterPass
};

// Plan 14 Task 10 (v2 design §3.10): pass n's own Transfer and Colour strength, applied to its raw output in place. Every image GENERAL.
struct VkResolvePass {
  VkImageView given = VK_NULL_HANDLE;     // what the pass was given: RGBA16F, sampled
  VkImageView returned = VK_NULL_HANDLE;  // what it returned: RGBA16F storage, rewritten
  nr::Size size;                          // the canvas
  float transfer_strength = 1.f;
  float color_strength = 1.f;
};

// Keep faces (2026-10-08): ColorPipeline's FacesPass on Vulkan (shaders/faces_cs.hlsl). Every image RGBA16F in GENERAL, at the canvas (the atlas:
// look::MakeAtlas of it).
struct VkFacesPass {
  VkImageView changed = VK_NULL_HANDLE;    // pass 1's raw output, or a later pass's: sampled by the pyramid, rewritten (storage) by the combine
  VkImageView reference = VK_NULL_HANDLE;  // the twin's output (pass 1: rewritten by the combine with the face mask), or a later pass's input
  VkImageView mask = VK_NULL_HANDLE;       // a later pass: pass 1's face mask (the twin's image); null: pass 1
  VkImageView atlas = VK_NULL_HANDLE;      // the pyramid: storage for RecordFacesPyramid, sampled by RecordFacesCombine
  VkImageView detail = VK_NULL_HANDLE;     // fix round 4: the despiked difference: storage for RecordFacesDespike, sampled by RecordFacesCombine
  VkImageView fill = VK_NULL_HANDLE;       // round 5: the fill's atlas (pass 1 with parameters.fill): storage for the pyramid, sampled by the combine
  look::Atlas layout;
  look::FacesParameters parameters;
};

// The SPIR-V twin of ColorPipeline's compute side (design §4.2): one push-descriptor set layout (sampled images 0-4, storage images 16-18, a uniform
// buffer 32), one pipeline layout, compute pipelines made per variant on first use, a host-visible coherent constants ring (RING_SLOTS recordings x 94
// slots x 256 B: Plan 14's look, meter and resolve passes, one slot per dispatch, and Keep faces' 52), and two 1x1 placeholders (RGBA16F sampled + storage, RG16F storage) for the bindings a
// pass leaves empty. ReShade-free: raw Vulkan through vk::NrFunctions. Never blocks. Not thread-safe: the caller's lock.
class VkColorPipeline {
 public:
  // As ColorPipeline: reused only after the slot's mark completed.
  static constexpr uint32_t RING_SLOTS = 128u;
  // The output formats a DLSS Output can have here (an sRGB-typed or BGRA view cannot be a storage image of these).
  static constexpr uint32_t OUTPUT_VARIANTS = 5u;
  // Passes 2..10 each have one resolve (the Session's passes are at most MAX_PASSES = 10).
  static constexpr uint32_t RESOLVE_PASSES = 9u;

  VkColorPipeline() = default;
  ~VkColorPipeline();
  VkColorPipeline(const VkColorPipeline&) = delete;
  VkColorPipeline& operator=(const VkColorPipeline&) = delete;

  // The layouts, the ring and the placeholders. Pipelines are made when first used. False, with `error`, when one call failed.
  bool Initialize(const vk::NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, std::string* error);
  // Device idle only (the vkDestroyDevice detour, the smoke's teardown) or never used. Idempotent.
  void Destroy();
  // Once per recording, before its first pass: moves the placeholders UNDEFINED -> GENERAL (their contents are never read).
  void PrepareRecording(VkCommandBuffer buffer);
  // Batch 2 review (minor 3): builds every pipeline an in-place recording of `output_format` needs, the encode, the decode and (Plan 14) the three look
  // passes, so a variant that cannot be built is known before anything is recorded (and the ms-scale first compile can run outside the hooked evaluate:
  // the caller warms it at enable). True when all exist. False for a format without a variant, or when one could not be built (logged once, never
  // retried).
  bool Prepare(VkFormat output_format);
  // Batch 3 review, minor 5: all of `output_format`'s pipelines exist already (Prepare would build nothing). Never builds.
  [[nodiscard]] bool Prepared(VkFormat output_format) const;
  // Task 10: the same for Before upscaling, which needs the sampled encode, the private-colour decode, the motion copy, the sampled meter and the look
  // passes whatever the game's formats are.
  bool PreparePreSr();
  // Plan 14: the motion copy alone (DLSS's vectors copied for the Present path), built now so its first compile stays out of the hooked evaluate.
  bool PrepareMotion();
  [[nodiscard]] bool Initialized() const { return ring_ != nullptr; }
  // Each: false (logged once) when `slot` is out of range or the variant's pipeline could not be made; nothing is recorded then.
  bool RecordEncode(VkCommandBuffer buffer, uint32_t slot, const VkEncodePass& pass);  // After DLSS (storage source) or Before upscaling (sampled)
  bool RecordDecode(VkCommandBuffer buffer, uint32_t slot, const VkDecodePass& pass);  // in place, or into the private colour
  bool RecordMotion(VkCommandBuffer buffer, uint32_t slot, const VkMotionPass& pass);  // Before upscaling's canvas motion
  // Plan 14, Direct3D 12's look passes with their constants verbatim. A global barrier comes between the dispatches inside one call (a pyramid level
  // reads the one before; the levels read the scene cut); the caller orders one call after another.
  bool RecordChange(VkCommandBuffer buffer, uint32_t slot, const VkChangePass& pass);
  bool RecordPyramid(VkCommandBuffer buffer, uint32_t slot, const VkPyramidPass& pass);  // one dispatch per level
  bool RecordStabilize(VkCommandBuffer buffer, uint32_t slot, const VkStabilizePass& pass);
  bool RecordDetail(VkCommandBuffer buffer, uint32_t slot, const VkStabilizePass& pass);
  bool RecordShape(VkCommandBuffer buffer, uint32_t slot, const VkShapePass& pass);
  // Plan 14 Task 10: the meter before the encode; and pass `index`'s (1..RESOLVE_PASSES: passes 2..10) own strengths between NR's passes.
  bool RecordMeter(VkCommandBuffer buffer, uint32_t slot, const VkMeterPass& pass);
  bool RecordResolve(VkCommandBuffer buffer, uint32_t slot, uint32_t index, const VkResolvePass& pass);
  // Keep faces: the pyramid of a pass's weighted change (one dispatch per level, a barrier between them), the combine, and Show the face mask's tint of
  // `mask` into `target` at `size`. The later passes of one recording share their constants' ring slots: theirs are the same.
  [[nodiscard]] bool FacesReady() const;  // the Keep faces pipeline exists (Prepare, PreparePreSr build it); never builds
  [[nodiscard]] bool FacesFailed() const;  // fix round 1 (M2): building it was tried and failed: Keep faces is unavailable on this device
  bool RecordFacesPyramid(VkCommandBuffer buffer, uint32_t slot, const VkFacesPass& pass);
  bool RecordFacesCombine(VkCommandBuffer buffer, uint32_t slot, const VkFacesPass& pass);
  bool RecordFacesDespike(VkCommandBuffer buffer, uint32_t slot, const VkFacesPass& pass);  // fix round 4: `changed` - `reference` into `detail`
  bool RecordFacesShow(VkCommandBuffer buffer, uint32_t slot, VkImageView target, VkImageView mask, nr::Size size);
  // The variant for a DLSS Output view format; nullopt: "unsupported format".
  [[nodiscard]] static std::optional<uint32_t> OutputVariantOf(VkFormat format);

 private:
  struct Bindings {
    std::array<VkSampledView, 5> sampled;     // t0-t4
    std::array<VkImageView, 3> storage = {};  // u0-u2, GENERAL
  };

  [[nodiscard]] bool SlotInRange(uint32_t slot, const char* pass) const;
  // The pipeline for `index` (see the .cpp), made on first use; VK_NULL_HANDLE (logged once) when it cannot be.
  VkPipeline PipelineFor(uint32_t index);
  // The three look passes' pipelines and the per-pass resolve (both placements use them), made now. True when all exist.
  bool PrepareLook();
  // The encode's pipeline for `pass` (its in-place variant, or the sampled one); VK_NULL_HANDLE when there is none.
  VkPipeline EncodePipelineFor(const VkEncodePass& pass);
  // Writes `constants` into the ring slot of (`slot`, `pass`), binds `pipeline`, pushes the set, and dispatches width x height in 8x8 groups.
  void Dispatch(VkCommandBuffer buffer, VkPipeline pipeline, uint32_t slot, uint32_t pass, std::span<const uint32_t> constants, const Bindings& bindings,
                uint32_t width, uint32_t height);
  // Compute writes so far are visible to the compute reads and writes after.
  void ComputeBarrier(VkCommandBuffer buffer) const;

  vk::NrFunctions functions_;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_ = {};
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  // The sampled encode, the in-place encodes, the private-colour decode, the in-place decodes, the motion copy, the look's three, the resolve, the sampled
  // meter, the in-place meters, and Keep faces'.
  static constexpr size_t PIPELINES = 1u + OUTPUT_VARIANTS + 1u + OUTPUT_VARIANTS + 1u + 3u + 1u + 1u + OUTPUT_VARIANTS + 1u;
  std::array<VkPipeline, PIPELINES> pipelines_ = {};
  std::array<bool, PIPELINES> failed_ = {};  // a variant that could not be built is not retried every frame
  VkBuffer ring_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory ring_memory_ = VK_NULL_HANDLE;
  std::byte* ring_ = nullptr;
  vk::NrImage placeholder_color_;   // RGBA16F, 1x1
  vk::NrImage placeholder_motion_;  // RG16F, 1x1
};

}  // namespace uplift::color
