#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "color/color_pipeline.hpp"
#include "color/encoding.hpp"
#include "look/look_math.hpp"
#include "nr/session.hpp"
#include "nr/timeline.hpp"
#include "nr/types.hpp"
#include "sources/auto_exposure.hpp"
#include "sources/look_plan.hpp"
#include "sources/motion_source.hpp"
#include "sources/present_motion_ring.hpp"

namespace uplift::sources {

// The Present path's target (spec §8.1): the primary swap chain's back buffer.
struct PresentTarget {
  ID3D12Resource* resource = nullptr;                // COPY_SOURCE on entry and on return
  color::Encoding encoding = color::Encoding::SRGB;  // resolved, never AUTO
  float diffuse_white_nits = 100.f;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  nr::BoundResource dlss_motion;       // Plan 14: DLSS's vectors copied in the game's frame (Vulkan's bridge): the region's own pixels, bound whole at a scale of (1, 1); or none
  float dlss_motion_scale_x = 1.f;     // Plan 18: Direct3D 11's ring keeps DLSS's raw vectors: their MV.Scale x MotionScale; 1 for Vulkan's scaled copies
  float dlss_motion_scale_y = 1.f;
  // Plan 18 (fix round 1, M-1): `dlss_motion` holds DLSS's raw vectors (Direct3D 11's ring), so the recording runs them through the motion copy
  // (motion_cs.hlsl: non-finite vectors dropped, the rest clamped, the scale applied) at the region's size and binds the result at (1, 1), as
  // RecordMotionCopy's copies; false for Vulkan's copies, which their own copy filtered.
  bool dlss_motion_raw = false;
  bool dlss_motion_flip = false;  // 1.1.6: `dlss_motion` (raw) is upside down against the back buffer: the conversion flips it
  nr::BoundResource launchpad_motion;  // Plan 6: this frame's UPLIFT_MV (back-buffer pixels), or none; converted before NR (F1)
  float motion_scale_x = 1.f;          // MotionScaleX/Y for it
  float motion_scale_y = 1.f;
};

// The After-DLSS target (v2 design §3.4): the region of DLSS's Output that NR runs on. Also pre-SR's
// input, where `resource` is the game's Color, only read.
struct UavTarget {
  ID3D12Resource* resource = nullptr;                // UNORDERED_ACCESS on entry and on return
  nr::Rect region;                                   // all zero means the whole resource
  color::Encoding encoding = color::Encoding::SRGB;  // resolved, never AUTO
  float diffuse_white_nits = 100.f;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  ID3D12Resource* exposure = nullptr;  // DLSS's ExposureTexture, NON_PIXEL_SHADER_RESOURCE, or null
  float exposure_factor = 1.f;         // DLSS.Exposure.Scale ÷ DLSS.Pre.Exposure
};

struct PipelineResult {
  bool recorded = false;    // commands went onto the list: an After-DLSS caller owes a replay and a token
  bool nr_applied = false;  // the target now holds the decoded NR output
  uint32_t passes_run = 0u;
  std::string_view reason;    // static text; empty when every requested pass ran
  bool from_session = false;  // M1 fix round 1: `reason` is Session::Evaluate's own, so its message explains it
  MotionSource motion_source = MotionSource::NONE;
  float motion_scale_x = 1.f;  // valid when motion_source == DIRECT: MV.Scale x MotionScale
  float motion_scale_y = 1.f;
};

// v2 design §3.8/§3.9: the size NR works at for one recording.
struct WorkLayout {
  nr::Size image;   // the output region resampled to this size; empty = the output size (Full)
  nr::Size canvas;  // NR's network size: `image` plus mirrored padding (pre-SR below the floor); empty = `image`
  color::Upsampling upsampling = color::Upsampling::EDGE_AWARE;  // how NR's change returns to the output size
};

// Plan 5's LookConfig (what the next recordings apply beyond their target), its parts and LookPlan are in sources/look_plan.hpp since Plan 14:
// Vulkan's pipeline shares them.

enum class TargetKind : uint8_t {
  PRESENT,  // the source copy rests in both shader-read states, for the pixel-shader decode
  UAV,      // only NON_PIXEL_SHADER_RESOURCE, legal on compute queues too
  PRE_SR,   // no source copy: the game's Color is read in place, the result lands in the private colour
};

// Spec §8.1 and v2 design §3.7: source copy ← target, encode into A, the Session's passes, decode
// into the target. Owns the colour intermediates and the all-zero RG16F motion texture (the §9 None
// provider), sized to the region NR runs on. Never blocks. Not thread-safe: the device context's lock.
class NrPipeline final : private nr::PassResolver {
 public:
  NrPipeline(ID3D12Device* device, nr::Session& session, nr::Timeline& timeline);
  ~NrPipeline();  // releases the intermediates directly: only after the GPU is idle for them
  NrPipeline(const NrPipeline&) = delete;
  NrPipeline& operator=(const NrPipeline&) = delete;

  bool Initialize(std::string* error);
  // Once per presented frame: the whole back buffer at `layout`. Binds the copy of the game's motion
  // vectors made for this frame (RecordMotionCopy), else the zero texture; a switch between the two
  // restarts NR's history for one frame (spec §9).
  PipelineResult RecordPresent(ID3D12GraphicsCommandList* list, const PresentTarget& target, const nr::Controls& controls,
                               bool reset_hint, const WorkLayout& layout = {});
  // Any number of times per frame (every evaluate of the main handle, v2 design §3.1): a region copy,
  // encode with the game's exposure, NR with `inputs` (a null inputs.motion.resource binds the
  // all-zero texture), a compute decode into the region, then a UAV barrier on the target. Only
  // compute-legal states. The caller issues its completion token before calling.
  PipelineResult RecordUav(ID3D12GraphicsCommandList* list, const UavTarget& target, const nr::FrameInputs& inputs,
                           const nr::Controls& controls, const WorkLayout& layout = {});
  // v2 design §3.9: NR on the region of the game's Color (`color.resource`, only read) before DLSS
  // upscales it. With a canvas in `layout` the image is mirror-padded into it, and the game's motion
  // vectors are copied into a canvas texture. NR's result is composed into PrivateColor() at the same
  // region, left in NON_PIXEL_SHADER_RESOURCE for DLSS.
  PipelineResult RecordPreSr(ID3D12GraphicsCommandList* list, const UavTarget& color, const nr::FrameInputs& inputs,
                             const nr::Controls& controls, const WorkLayout& layout);
  // After RecordPreSr applied NR and until the next recording: what DLSS reads as Color.
  [[nodiscard]] ID3D12Resource* PrivateColor() const { return intermediates_.private_color.Get(); }
  // Amendment 7: at an evaluate of the game's DLSS, copies its motion vectors (`motion`, scaled into the
  // region's own pixels by `scale_x/y`) on the game's list, for the present that closes the current
  // frame. False when nothing was recorded.
  // 1.1.6: `flip_y` when DLSS's images are upside down against the back buffer (ReShade's RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN).
  bool RecordMotionCopy(ID3D12GraphicsCommandList* list, const nr::BoundResource& motion, float scale_x, float scale_y, bool flip_y = false);
  // Frees the intermediates now when the GPU has passed the newest recording that used them (a drain to OFF
  // normally has); otherwise hands them to the timeline, which frees them once it does. Also the look set,
  // the mask copy and the exposure state too.
  void ReleaseIntermediates();
  // Minor 4 (Plan 4 fix round 4): the Present motion copies alone, the same way -- for a caller that
  // drops them as soon as Source leaves Present or MotionVectors becomes None, without waiting for a
  // drain to OFF. A no-op when there is nothing to release.
  // Plan 18 (fix round 1, M-1): Direct3D 11's converted ring vectors too, which the same conditions stop.
  void ReleaseMotionCopies() {
    RetireMotionCopies();
    RetireDlssPresentMotion();
  }
  // 1.0.1 (fix round 1, minor 2): Launchpad's converted motion alone, the same way, once the placement leaves Present or MotionVectors stops wanting
  // Launchpad. Separate from ReleaseMotionCopies, which also runs every frame of Source = Present with MotionVectors = Launchpad (no DLSS copies
  // wanted), where the converted motion is in use.
  void ReleaseLaunchpadMotion() { RetireLaunchpadMotion(); }
  [[nodiscard]] uint64_t HeldBytes() const;
  // A DLSS Output format RecordUav can write: DescribeUavFormat knows it and the device stores to it.
  [[nodiscard]] bool SupportsUavTarget(DXGI_FORMAT format) const;
  // Plan 5: the look stage, the colour fixes, the mask and passes 2..10 for the next recordings.
  void SetLookConfig(const LookConfig& config);
  // Plan 5 (v2 design §3.13, key decision 8): Uplift's copy of UPLIFT_MASK, shaped like `mask` (its size and
  // format), which the add-on copies the effect's texture into at the end of ReShade's effects. Recordings bind it
  // from NoteMaskCopied on. `mask` null, or a format Uplift cannot read, releases the copy and returns null.
  ID3D12Resource* PrepareMaskCopy(const D3D12_RESOURCE_DESC* mask);
  void NoteMaskCopied() { mask_.copied = (mask_.texture != nullptr); }
  // Plan 17: the exposure the latest recording's encode read (the Details line).
  [[nodiscard]] ExposureReport LastExposure() const { return exposure_report_; }
  // Keep faces (2026-10-08): the device loads the formats its recombination reads back (typed UAV loads, as the look stage needs).
  [[nodiscard]] bool KeepFacesSupported() const { return color_.SupportsUavLoads() && color_.FacesReady(); }

 private:
  using Texture = Microsoft::WRL::ComPtr<ID3D12Resource>;
  // Plan 5 (key decision 4): the look stage's surfaces (sources::LookPlan), apart from the main set so that the look never restarts
  // NR's history. Freed like the main set: at once when the GPU is done with them, else at their mark.
  struct LookSurfaces {
    LookPlan plan;
    look::Atlas atlas;
    Texture change;                         // own_change: RGBA16F at the image size
    Texture basis;                          // κ_M, RG16F
    Texture gauss;                          // the pyramid atlas, RGBA16F
    Texture peaks;                          // the max-pyramid atlas, R16F
    std::array<Texture, 2> history;         // the stabilised levels, ping-pong
    std::array<Texture, 2> detail_history;  // the stabilised fine band, ping-pong
    Texture state;                          // 1x1 RGBA32F: the scene cut
    uint32_t current = 0u;                  // the history this frame writes
    bool history_valid = false;             // the histories hold a previous frame
    uint64_t bytes = 0u;
    nr::Mark last_use;
  };
  // Key decision 8: Uplift's copy of the effect's UPLIFT_MASK, in both shader-read states.
  struct MaskCopy {
    Texture texture;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view_format = DXGI_FORMAT_R8_UNORM;
    uint64_t bytes = 0u;
    bool copied = false;  // the add-on has copied into it: recordings may bind it
    nr::Mark last_use;
  };
  // v2 design §3.14: the governor's RGBA32F state, (2^E, E, the last anchor, set), in both shader-read states. Plan 17: 2x1, the second texel Auto's check
  // (the game's exposure beside the meter's target).
  struct MeterState {
    Texture state;
    bool snap = true;  // the next meter snaps: a new texture, or a settings change
    nr::Mark last_use;
  };
  // What the encode and the decode read as their exposure (InputExposure).
  struct ExposureChoice {
    ID3D12Resource* texture = nullptr;
    float factor = 1.f;
    bool metered = false;  // `texture` is the meter's state
    bool probe = false;    // Plan 17: the meter runs beside the game's exposure for Auto's check
    // With `probe`: the game's exposure as the meter reads it, and (2026-10-09, Auto's Blend) its share of the state's multiplier in stops.
    ID3D12Resource* game_texture = nullptr;
    float game_factor = 1.f;
    float game_weight = 0.f;
  };
  // Plan 17: Auto's check. The meter state's two texels are copied into a readback ring after the meter and read once the GPU has passed them: a few frames
  // late, never a wait. A busy ring skips the sample.
  static constexpr uint32_t CHECK_SLOTS = 4u;
  struct ExposureCheck {
    Texture readback;  // CHECK_SLOTS placed footprints of 2x1 RGBA32F, READBACK heap, COPY_DEST
    std::array<nr::Mark, CHECK_SLOTS> marks = {};
    std::array<bool, CHECK_SLOTS> pending = {};
    uint32_t next = 0u;  // the oldest slot, written next
  };
  // Keep faces (2026-10-08): the twin's output, which pass 1's recombination rewrites with the face mask the later passes read, and the pyramid of a pass's
  // weighted change, both at the canvas. Apart from the main set, made while Keep faces is on and freed like the look set.
  struct FaceSurfaces {
    Texture twin;    // RGBA16F
    Texture detail;  // RGBA16F: fix round 4's despiked difference (made with the others, so Remove specks never reallocates)
    Texture atlas;   // RGBA16F, look::MakeAtlas(size)
    Texture fill;    // RGBA16F, the same size: round 5's fill pyramid (made with the others, so Fill skin by colour never reallocates)
    look::Atlas layout;
    nr::Size size;
    uint64_t bytes = 0u;
    nr::Mark last_use;
  };

  // After NR: what the decode reads as t1.
  struct AfterNr {
    ID3D12Resource* texture = nullptr;            // NR's output, or the (shaped) change field
    std::optional<color::Upsampling> upsampling;  // set: `texture` is the change field
    bool ok = true;                               // false: the change pass could not be recorded
  };

  void ResolvePass(ID3D12GraphicsCommandList* list, uint32_t index, ID3D12Resource* given, ID3D12Resource* returned) override;
  // Keep faces: pass 1's recombination (FacesPass with no mask), after the Session's twin evaluate.
  void CombineFaces(ID3D12GraphicsCommandList* list, ID3D12Resource* lighting, ID3D12Resource* faces) override;
  // Keep faces: the twin and the atlas at `canvas`, made now unless held. False (logged once): Keep faces is skipped this recording.
  bool EnsureFaces(nr::Size canvas);
  void RetireFaces();
  // Keep faces: the recombination of one pass (`pass.changed` rewritten), on the recording's slot: the pyramid of its weighted change, then the combine.
  void RecordFaces(ID3D12GraphicsCommandList* list, color::FacesPass pass);
  [[nodiscard]] LookPlan LookPlanFor(nr::Size image, bool reduced) const;  // sources::PlanLook for this device and this set
  bool EnsureLook(const LookPlan& plan);       // false: the look (and C at Full) is skipped this recording
  void RetireLook();
  void RetireMask();
  void RetireExposure();
  [[nodiscard]] uint32_t ShaderOptions() const;  // the encode's and the decodes' shader_options
  ExposureChoice ChooseExposure(color::Encoding encoding, ID3D12Resource* game_texture, float game_factor);
  // The meter (and, for Auto's check, the copy of its state into the readback ring) before the encode.
  void RecordMeter(ID3D12GraphicsCommandList* list, uint32_t slot, color::MeterPass pass, const ExposureChoice& exposure);
  void PollExposureChecks();  // reads the samples the GPU has passed into auto_exposure_
  void RetireExposureCheck();
  ID3D12Resource* BoundMask(uint32_t slot);  // the mask copy for this recording, or null
  AfterNr RecordAfterNr(ID3D12GraphicsCommandList* list, uint32_t slot, const color::EncodePass& encode,
                        ID3D12Resource* nr_output, bool reduced, bool look_ready, color::Upsampling upsampling,
                        const color::StabilizeMotion& motion, bool reset);

  LookConfig config_;
  LookSurfaces look_;
  FaceSurfaces faces_;
  bool faces_ran_ = false;  // Keep faces: this recording's pass 1 ran its twin, so the later passes keep only their broad change inside the face mask
  MaskCopy mask_;
  MeterState exposure_;
  ExposureCheck check_;
  AutoExposure auto_exposure_;  // Plan 17: kept for the pipeline's life (a latch never flips back), across disables and setting changes
  ExposureReport exposure_report_;
  uint32_t resolve_slot_ = 0u;  // the recording's descriptor slot, for ResolvePass

  // What one recording needs allocated (v2 design §3.21's surfaces). A different plan reallocates the set.
  struct SetPlan {
    nr::Size output;                                // the region NR's result lands on
    nr::Size image;                                 // resolved: never empty
    nr::Size canvas;                                // resolved: never empty
    DXGI_FORMAT copy_format = DXGI_FORMAT_UNKNOWN;  // the source copy's; UNKNOWN: none (pre-SR reads Color in place)
    uint32_t source_bytes_per_pixel = 0u;
    TargetKind kind = TargetKind::PRESENT;
    nr::Size private_color;                         // pre-SR: the game's Color dimensions
    bool canvas_motion = false;                     // pre-SR in a canvas, with motion vectors to copy into it
    friend bool operator==(const SetPlan&, const SetPlan&) = default;
  };
  struct Intermediates {
    Texture source_copy;
    Texture model_a;
    Texture model_b;
    Texture zero_motion;
    Texture change;         // the image size: only below the output size
    Texture canvas_motion;  // the canvas size: only with SetPlan::canvas_motion
    Texture private_color;  // pre-SR only
    SetPlan plan;
    uint64_t bytes = 0u;    // every texture above, as the budget charges them
    bool motion_needs_clear = true;
    bool reset_pending = true;  // see Plan 3
    color::Encoding encoding = color::Encoding::AUTO;
    float input_scale = 0.f;
    nr::Mark last_use;  // the newest recording on this set (CommitSlot): the GPU is done with it once complete
  };
  // Amendment 7, deepened by I-2 (Plan 4 fix round 4): copies indexed by the frame the next present closes. The flicker fix (2026-10-08): four slots, grown
  // to eight once a copy is skipped for a slot still read (a lag past 3; fix round 1, M6), and a present without its own binds the
  // newest at most PRESENT_MOTION_MAX_AGE frames older (present_motion_ring.hpp), so the motion source never switches for a missed copy.
  static constexpr size_t MOTION_COPY_SLOTS = PRESENT_MOTION_SLOTS;  // room for the grown ring
  struct MotionCopies {
    std::array<Texture, MOTION_COPY_SLOTS> textures;
    std::array<uint64_t, MOTION_COPY_SLOTS> frames = {};    // the frame each copy was made for; 0 = none
    size_t slots = PRESENT_MOTION_MIN_SLOTS;                 // the slots in use (made): 4, or 8 once grown
    std::array<nr::Mark, MOTION_COPY_SLOTS> last_use = {};  // the newest recording that wrote or read it
    nr::Size size;
  };

  // 1.0.1 (the Launchpad investigation, F1): Launchpad's UPLIFT_MV as NR gets it on the Present path, made by the motion copy (motion_cs.hlsl) in the
  // recording that binds it: resampled to the work image in its pixels, non-finite vectors dropped and the rest clamped. One texture, rewritten every
  // recording on the same queue, as the canvas motion is.
  struct LaunchpadMotion {
    Texture texture;  // RG16F at the canvas size
    nr::Size size;
    nr::Mark last_use;  // the newest recording that wrote and read it
  };
  // Plan 18 (fix round 1, M-1): Direct3D 11's ring slot as the Present path binds it, made by the motion copy in the recording that binds it: the region's
  // own pixels with the scale applied, non-finite vectors dropped and the rest clamped (RecordMotionCopy's copy). One texture, rewritten every recording on
  // the same queue, as Launchpad's.
  struct DlssPresentMotion {
    Texture texture;  // RG16F at the region's size
    nr::Size size;
    nr::Mark last_use;  // the newest recording that wrote and read it
  };

  Texture CreateTexture(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                        const wchar_t* name);
  std::string_view EnsureIntermediates(const SetPlan& plan);  // empty on success, else "out of memory"
  void RetireSet();            // the intermediates only: freed now if the GPU is done with them, else at their mark
  void RetireMotionCopies();   // likewise for the motion copies
  void RetireLaunchpadMotion();  // likewise for Launchpad's converted motion
  void RetireDlssPresentMotion();  // Plan 18 (fix round 1, M-1): likewise for Direct3D 11's converted ring vectors
  bool MakeMotionCopies(size_t first, size_t end, nr::Size size);  // motion_copies_.textures[first, end); none kept on a failure
  // Plan 18 (fix round 1, M-1): records the motion copy of `motion` (DLSS's raw vectors in the ring slot, the region its rect) into dlss_present_motion_ at
  // the region's size with `scale_x/y` applied, on the committed `slot`. The texture to bind whole at a scale of (1, 1), or none (an unreadable format, a
  // region outside the texture, or no memory): NR then runs with the zero motion, never with the raw vectors.
  nr::BoundResource ConvertDlssMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const nr::BoundResource& motion, float scale_x, float scale_y,
                                      bool flip_y);
  // F1: records the conversion of `motion` (UPLIFT_MV, back-buffer pixels times MotionScale) into launchpad_motion_ at `work`'s canvas, on the committed
  // `slot`. The texture to bind whole at a scale of (1, 1), or none (an unreadable format, a region outside the texture, or no memory): NR then runs with
  // the zero motion, never with the raw vectors.
  nr::BoundResource ConvertLaunchpadMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const nr::BoundResource& motion, float scale_x, float scale_y,
                                           const WorkLayout& work);
  void NoteEncoding(color::Encoding encoding, float input_scale);
  void CommitSlot(uint32_t slot);
  bool Encode(ID3D12GraphicsCommandList* list, uint32_t slot, color::EncodePass pass);
  nr::EvaluateResult Evaluate(ID3D12GraphicsCommandList* list, uint32_t slot, nr::FrameInputs inputs, const nr::Controls& controls);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  nr::Session& session_;
  nr::Timeline& timeline_;
  color::ColorPipeline color_;
  Intermediates intermediates_;
  std::array<nr::Mark, color::ColorPipeline::RING_SLOTS> slot_marks_ = {};
  uint32_t next_slot_ = 0u;
  uint64_t last_present_frame_ = 0u;
  bool allocation_failure_logged_ = false;
  MotionCopies motion_copies_;
  LaunchpadMotion launchpad_motion_;
  DlssPresentMotion dlss_present_motion_;  // Plan 18 (fix round 1, M-1)
  MotionSource present_motion_ = MotionSource::NONE;  // the last present's provider
};

}  // namespace uplift::sources
