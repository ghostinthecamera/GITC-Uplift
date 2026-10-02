#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "color/encoding.hpp"
#include "look/look_math.hpp"
#include "nr/types.hpp"

namespace uplift::color {

// Plan 5: the `options` bits the encode and the decodes read (shaders/change_common.hlsli's OPTION_*).
namespace shader_options {
inline constexpr uint32_t PRIMARIES_MASK = 3u;    // bits 0-1: the SourcePrimaries index (Primaries; 0 = automatic)
inline constexpr uint32_t NEAR_BLACK_GUARD = 4u;  // F16: read by the change pass
inline constexpr uint32_t CONSISTENT = 8u;        // F15: read by the compose
inline constexpr uint32_t MASK = 16u;             // set by RecordDecode/RecordComputeDecode when a mask is given
}  // namespace shader_options

struct EncodePass {
  ID3D12Resource* source = nullptr;  // original pixels, readable by compute
  DXGI_FORMAT source_view_format = DXGI_FORMAT_UNKNOWN;
  ID3D12Resource* model = nullptr;   // RGBA16F, UNORDERED_ACCESS
  ID3D12Resource* motion = nullptr;  // optional RG16F, UNORDERED_ACCESS: written with zeros when set
  uint32_t width = 0u;
  uint32_t height = 0u;
  Encoding encoding = Encoding::SRGB;  // resolved, never AUTO
  float input_scale = 1.f;
  ID3D12Resource* exposure = nullptr;  // optional 1x1 exposure texture, NON_PIXEL_SHADER_RESOURCE
  float exposure_factor = 1.f;         // DLSS.Exposure.Scale ÷ DLSS.Pre.Exposure
  nr::Rect region;  // the part of `source` resampled to the image; all zero = width x height at (0, 0)
  nr::Size canvas;  // the model texture: the image plus mirrored padding; empty = the image
  uint32_t options = 0u;  // Plan 5: shader_options (the primaries, the near-black guard)
};

struct DecodePass {
  ID3D12Resource* source = nullptr;     // original pixels, readable by pixel shaders
  DXGI_FORMAT source_view_format = DXGI_FORMAT_UNKNOWN;
  ID3D12Resource* nr_output = nullptr;  // RGBA16F model domain, readable by pixel shaders
  ID3D12Resource* target = nullptr;     // RENDER_TARGET
  DXGI_FORMAT target_view_format = DXGI_FORMAT_UNKNOWN;
  bool target_view_srgb = false;
  uint32_t width = 0u;
  uint32_t height = 0u;
  Encoding encoding = Encoding::SRGB;
  float input_scale = 1.f;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  ID3D12Resource* exposure = nullptr;
  float exposure_factor = 1.f;
  std::optional<Upsampling> upsampling;  // set: nr_output is the change field at the work size
  uint32_t options = 0u;                              // Plan 5: shader_options (the primaries, Consistent)
  float chroma_clamp = 0.f;                           // Plan 5 (F14): stops; 0 = off
  ID3D12Resource* mask = nullptr;                     // Plan 5 (§3.13): the mask copy, both shader-read states; null = none
  DXGI_FORMAT mask_view_format = DXGI_FORMAT_R8_UNORM;
};

// The After-DLSS decode (v2 design §3.4 step 5): compute, into a region of a UAV target.
struct ComputeDecodePass {
  ID3D12Resource* source = nullptr;  // the source copy, NON_PIXEL_SHADER_RESOURCE
  DXGI_FORMAT source_view_format = DXGI_FORMAT_UNKNOWN;
  ID3D12Resource* nr_output = nullptr;  // RGBA16F model domain, NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* target = nullptr;     // UNORDERED_ACCESS
  DXGI_FORMAT target_uav_format = DXGI_FORMAT_UNKNOWN;
  uint32_t origin_x = 0u;  // the region's top-left corner in the target
  uint32_t origin_y = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
  Encoding encoding = Encoding::SRGB;
  float input_scale = 1.f;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  ID3D12Resource* exposure = nullptr;
  float exposure_factor = 1.f;
  uint32_t source_x = 0u;  // the region's top-left corner in `source`
  uint32_t source_y = 0u;
  std::optional<Upsampling> upsampling;  // set: nr_output is the change field at the work size
  uint32_t options = 0u;                              // Plan 5: shader_options (the primaries, Consistent)
  float chroma_clamp = 0.f;                           // Plan 5 (F14): stops; 0 = off
  ID3D12Resource* mask = nullptr;                     // Plan 5 (§3.13): the mask copy, both shader-read states; null = none
  DXGI_FORMAT mask_view_format = DXGI_FORMAT_R8_UNORM;
};

// v2 design §3.7: NR's change field at the work image size, from the model input recomputed exactly
// as `encode` computed it (a later pass may have overwritten A) and NR's output.
struct ChangePass {
  EncodePass encode;                    // the encode that fed NR; its model and motion are not used
  ID3D12Resource* nr_output = nullptr;  // RGBA16F model domain, NON_PIXEL_SHADER_RESOURCE, the canvas size
  ID3D12Resource* change = nullptr;     // RGBA16F, UNORDERED_ACCESS, the image size
  ID3D12Resource* basis = nullptr;  // Plan 5: RG16F, UNORDERED_ACCESS: NR's input chroma κ_M, for the look; null = none
};

// Amendment 7 and v2 design §3.9: the game's motion vectors into an Uplift RG16F texture, in its pixels.
struct MotionPass {
  ID3D12Resource* source = nullptr;  // the game's motion vectors, NON_PIXEL_SHADER_RESOURCE
  DXGI_FORMAT source_view_format = DXGI_FORMAT_UNKNOWN;
  nr::Rect region;                   // their region in `source`
  ID3D12Resource* target = nullptr;  // RG16F, UNORDERED_ACCESS
  nr::Size image;                    // the region resampled to this size, at the target's origin
  nr::Size canvas;                   // the target's size: the image plus mirrored padding
  float scale_x = 1.f;               // multiplies the vectors into target pixels
  float scale_y = 1.f;
};

// Plan 5 (v2 design §3.14): the exposure meter and governor, one thread group, before the encode.
struct MeterPass {
  ID3D12Resource* source = nullptr;  // read as the encode reads it
  DXGI_FORMAT source_view_format = DXGI_FORMAT_UNKNOWN;
  nr::Rect region;                   // the region the encode reads
  Encoding encoding = Encoding::LINEAR_BT709;
  uint32_t primaries = 0u;           // the SourcePrimaries index (Primaries; 0 = automatic)
  float input_scale = 1.f;
  ID3D12Resource* state = nullptr;   // 1x1 RGBA32F, UNORDERED_ACCESS: (2^E, E, the last anchor, set)
  bool snap = false;
  bool smooth = true;
  float brighter_rate = 2.f;
  float darker_rate = 0.7f;
  float frame_seconds = 1.f / 60.f;
};

// Plan 5 (v2 design §3.12): the Gaussian pyramid of C and the max pyramid of ℓ_M, levels 1..K, in two atlases.
struct PyramidPass {
  ID3D12Resource* change = nullptr;  // C, NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* gauss = nullptr;   // RGBA16F atlas, UNORDERED_ACCESS
  ID3D12Resource* peaks = nullptr;   // R16F atlas, UNORDERED_ACCESS
  look::Atlas atlas;
};

// The motion vectors the stabiliser reprojects along (current -> previous); a null resource is none.
struct StabilizeMotion {
  nr::BoundResource vectors;  // NON_PIXEL_SHADER_RESOURCE, with a resolved, non-empty rect
  DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
  float scale_x = 1.f;        // into the pixels of their rect
  float scale_y = 1.f;
};

// Plan 5 (v2 design §3.12, amendment 4): RecordStabilize runs the scene cut and the two levels the low band reads;
// RecordDetail runs the fine band (C minus the low band) at the work size.
struct StabilizePass {
  ID3D12Resource* gauss = nullptr;     // NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* change = nullptr;    // RecordDetail: C, NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* previous = nullptr;  // last frame's history (the level atlas, or the detail), NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* current = nullptr;   // this frame's, UNORDERED_ACCESS
  ID3D12Resource* state = nullptr;     // 1x1 RGBA32F scene-cut state, UNORDERED_ACCESS
  look::Atlas atlas;
  look::Bands bands;                   // stable_level for RecordStabilize, low_level for RecordDetail
  float rate = 1.f;                    // look::StabilizeRate
  bool motion_mode = false;            // Stabilize = Motion
  bool reset = false;
  StabilizeMotion motion;
};

struct ShapePass {
  ID3D12Resource* change = nullptr;   // C, UNORDERED_ACCESS: rewritten in place with the shaped change
  ID3D12Resource* basis = nullptr;    // κ_M, NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* gauss = nullptr;    // NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* peaks = nullptr;    // NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* history = nullptr;  // this frame's history atlas; null: Stabilize off
  ID3D12Resource* detail = nullptr;   // this frame's detail history; null: Stabilize detail off
  look::Atlas atlas;
  look::Bands bands;
  look::ShapeSettings settings;
  bool sdr = false;
};

// v2 design §3.10: a pass's own strengths between NR passes.
struct ResolveStrengthsPass {
  ID3D12Resource* given = nullptr;     // the pass's input, NON_PIXEL_SHADER_RESOURCE
  ID3D12Resource* returned = nullptr;  // its raw output, UNORDERED_ACCESS: rewritten
  nr::Size size;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  bool swapped = false;                // B -> A rather than A -> B: its own descriptor table
};

// The D3D12 side of spec §7: one root signature, the encode and compute-decode pipelines, one
// decode graphics pipeline per render-target format (back buffers have no UAV), and a
// shader-visible descriptor ring. The caller uses one slot per recording and reuses it only after
// the GPU has finished that recording. Never blocks. Not thread-safe.
class ColorPipeline {
 public:
  // Fix round 2, Important 1: sized for Timeline's own AGED_SUBMISSION_TIME (250 ms) at up to 240 fps
  // on a submit-only thread (a common engine shape, e.g. Unreal Engine 5's D3D12 RHI), so the ring
  // never starves NR into "descriptors busy" there: 128 slots of 11 tables of 7 descriptors, about 350 KB.
  static constexpr uint32_t RING_SLOTS = 128u;

  ColorPipeline() = default;
  ColorPipeline(const ColorPipeline&) = delete;
  ColorPipeline& operator=(const ColorPipeline&) = delete;

  bool Initialize(ID3D12Device* device, std::string* error);
  // False (logged, nothing recorded) when slot is out of range.
  bool RecordEncode(ID3D12GraphicsCommandList* list, uint32_t slot, const EncodePass& pass);
  // False when slot is out of range, or when no pipeline exists for pass.target_view_format.
  bool RecordDecode(ID3D12GraphicsCommandList* list, uint32_t slot, const DecodePass& pass);
  // False (logged, nothing recorded) when slot is out of range.
  bool RecordComputeDecode(ID3D12GraphicsCommandList* list, uint32_t slot, const ComputeDecodePass& pass);
  // False (logged, nothing recorded) when slot is out of range. Also used for the change field.
  bool RecordChange(ID3D12GraphicsCommandList* list, uint32_t slot, const ChangePass& pass);
  bool RecordMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const MotionPass& pass);
  // Plan 5. Each: false (logged, nothing recorded) when slot is out of range.
  bool RecordMeter(ID3D12GraphicsCommandList* list, uint32_t slot, const MeterPass& pass);
  // One dispatch per level, with UAV barriers on both atlases between them.
  bool RecordPyramid(ID3D12GraphicsCommandList* list, uint32_t slot, const PyramidPass& pass);
  // The scene cut, a UAV barrier on `state`, then the two levels.
  bool RecordStabilize(ID3D12GraphicsCommandList* list, uint32_t slot, const StabilizePass& pass);
  bool RecordDetail(ID3D12GraphicsCommandList* list, uint32_t slot, const StabilizePass& pass);
  bool RecordShape(ID3D12GraphicsCommandList* list, uint32_t slot, const ShapePass& pass);
  bool RecordResolve(ID3D12GraphicsCommandList* list, uint32_t slot, const ResolveStrengthsPass& pass);
  // Plan 2 final review M5: whether RecordDecode can write `target_view_format`.
  [[nodiscard]] bool HasDecodePipeline(DXGI_FORMAT target_view_format) const;
  // Whether the device can store to `format` through a typed UAV (cached per format).
  [[nodiscard]] bool SupportsTypedUavStore(DXGI_FORMAT format) const;
  // Key decision 12: typed UAV loads of RGBA16F, R16F and RGBA32F, which the Plan 5 passes need.
  [[nodiscard]] bool SupportsUavLoads() const { return uav_loads_; }

 private:
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptor(uint32_t index) const;
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor(uint32_t index) const;
  void WriteShaderResource(uint32_t index, ID3D12Resource* resource, DXGI_FORMAT format);
  void WriteUnorderedAccess(uint32_t index, ID3D12Resource* resource, DXGI_FORMAT format);
  struct View {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // also a null view's
  };
  // The exposure SRV: the texture's own float format (typeless read as float), or a null R32_FLOAT view.
  [[nodiscard]] static View ExposureView(ID3D12Resource* exposure);
  // One pass's table: `shader_resources` into t0.., `unordered` into u0..; every other slot gets a null view.
  void WriteTable(uint32_t base, std::initializer_list<View> shader_resources, std::initializer_list<View> unordered);
  [[nodiscard]] bool SlotInRange(uint32_t slot, const char* pass) const;
  // `constants` into the root constants, then `pipeline` over width x height in 8x8 groups.
  void DispatchCompute(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline, std::span<const uint32_t> constants,
                       uint32_t base, uint32_t width, uint32_t height);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> encode_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compute_decode_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> motion_pipeline_;
  // Plan 5: the look, meter and resolve pipelines.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> meter_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pyramid_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> stabilize_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> shape_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_pipeline_;
  // A null entry records a format whose pipeline could not be built, so it is not retried every frame.
  std::unordered_map<DXGI_FORMAT, Microsoft::WRL::ComPtr<ID3D12PipelineState>> decode_pipelines_;
  mutable std::unordered_map<DXGI_FORMAT, bool> typed_uav_store_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shader_heap_;  // shader-visible CBV/SRV/UAV ring
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> target_heap_;  // one CPU RTV, rewritten per decode
  uint32_t descriptor_size_ = 0u;
  bool uav_loads_ = false;  // key decision 12: TypedUAVLoadAdditionalFormats
};

}  // namespace uplift::color
