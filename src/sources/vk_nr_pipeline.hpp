#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "color/encoding.hpp"
#include "color/vk_color_pipeline.hpp"
#include "look/look_math.hpp"
#include "nr/session.hpp"
#include "nr/timeline.hpp"
#include "nr/types.hpp"
#include "sources/auto_exposure.hpp"
#include "sources/look_plan.hpp"
#include "sources/nr_pipeline.hpp"
#include "sources/present_motion_ring.hpp"
#include "vk/nr_functions.hpp"
#include "vk/nr_image.hpp"

namespace uplift::sources {

// The After-DLSS target on Vulkan (v2 design §3.4 and Plan 13 design §4.3): the region of DLSS's Output that NR runs on. UavTarget's twin; the resources
// are the copies of the game's NVSDK_NGX_Resource_VK the caller made during the hooked call (their pointers are valid for it alone). Before upscaling's input
// uses it too, with `resource` the game's Color (a sampled view in SHADER_READ_ONLY_OPTIMAL, only read) and `region` its render subrect.
struct VkDlssTarget {
  const NVSDK_NGX_Resource_VK* resource = nullptr;   // After DLSS: DLSS's Output, a storage view in GENERAL on entry and on return
  nr::Rect region;                                   // all zero means the whole resource (After DLSS only)
  color::Encoding encoding = color::Encoding::SRGB;  // resolved, never AUTO
  float diffuse_white_nits = 100.f;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  const NVSDK_NGX_Resource_VK* exposure = nullptr;  // DLSS's ExposureTexture (a sampled view in SHADER_READ_ONLY_OPTIMAL), or null
  float exposure_factor = 1.f;                      // DLSS.Exposure.Scale / DLSS.Pre.Exposure
};

// Plan 19: native NR at Present's target: the swap-chain image, in `layout` on entry and on return (PRESENT_SRC_KHR at the present event, ReShade's
// render-target layout inside its effects). Its bits are copied raw into Uplift's image of `copy_format`, the format the Vulkan bridge shares it in (the
// UNORM member of a mutable-format swap chain's family: an sRGB-created image's stored values, never linearised), which a blit converts into an RGBA16F
// intermediate that NR runs on in place, as on DLSS's Output; the result goes back the same way. `encoding` is resolved for `copy_format`'s values.
struct VkPresentTarget {
  VkImage image = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  nr::Size size;
  VkFormat copy_format = VK_FORMAT_UNDEFINED;
  color::Encoding encoding = color::Encoding::SRGB;  // resolved, never AUTO
  float diffuse_white_nits = 100.f;
  float transfer_strength = 1.f;
  float color_strength = 1.f;
  // DLSS's vectors copied in the game's frame for this present (PresentMotion: SHADER_READ_ONLY_OPTIMAL, in the motion region's own pixels, scaled), bound
  // whole at (1, 1); or null.
  const NVSDK_NGX_Resource_VK* dlss_motion = nullptr;
  // Stress round: DLSS's copy is the motion input this NR load was set up with, but none was made for this present (DLSS skipped a frame, a slot was busy): a
  // cleared RG16F image of this size (the copies' own) stands in, so NGX sees the same kind of input every frame. Empty: no stand-in.
  nr::Size dlss_motion_gap_size;
  // Otherwise this frame's UPLIFT_MV (a sampled view in SHADER_READ_ONLY_OPTIMAL, back-buffer pixels), converted into the work image's pixels; or none.
  color::VkSampledView launchpad_motion;
  nr::Size launchpad_size;
  VkFormat launchpad_format = VK_FORMAT_UNDEFINED;
  float motion_scale_x = 1.f;  // MotionScaleX/Y for UPLIFT_MV
  float motion_scale_y = 1.f;
};

// The Vulkan DLSS placements' recordings (Plan 13 design §4.3, Plan 14 design §3): encode DLSS's Output in place into A, the Session's NR passes, Direct3D 12's
// change path and look stage (RecordAfterNr's port: the change field, the pyramids, the stabiliser, the shape), decode in place (After DLSS); or encode the
// game's Color (sampled) into A on a padded canvas, the same, and decode into Uplift's own colour that DLSS reads instead (Before upscaling). Both work at a
// settled work size below the output (the decode upsamples the change field), meter the input's exposure when InputExposure asks for it, give each later pass
// its own Transfer and Colour strength (this class is the Session's nr::PassResolver), and bind the NR mask copy. Owns A, B, the change field, the all-zero
// motion image, Before upscaling's canvas motion and private colour (each an image with its NVSDK_NGX_Resource_VK, which is what the Session's opaque handles
// point at: nr/vk_handles.hpp), the look surfaces, the exposure state, the mask copy and the VkColorPipeline; they are freed at their marks. Every Uplift image starts each recording
// from UNDEFINED into GENERAL, except the stabiliser's histories and scene-cut state, which carry across frames in GENERAL; no game image is ever
// transitioned. Never blocks. Not thread-safe: the add-on's lock.
class VkNrPipeline final : private nr::PassResolver {
 public:
  VkNrPipeline(const vk::NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, nr::Session& session,
               nr::Timeline& timeline);
  ~VkNrPipeline();  // frees directly: only after the device is idle for them
  VkNrPipeline(const VkNrPipeline&) = delete;
  VkNrPipeline& operator=(const VkNrPipeline&) = delete;

  bool Initialize(std::string* error);
  // Plan 14 (design §3.2): the look stage, the colour fixes (InputExposure included: Auto or Game binds DLSS's exposure texture and scale, Metered the meter's
  // state, Manual none), the mask and passes 2..10's model controls and own strengths for the next recordings, with Δt per recording. A changed colour fix
  // makes the meter snap to its target (v2 design §3.14).
  void SetLookConfig(const LookConfig& config);
  // The device can load and store the look's R16F and RGBA32F images (RGBA16F is native NR's own requirement); without them the look stage, the meter and the
  // per-pass strengths are skipped, as on a Direct3D 12 device without typed UAV loads.
  void SetLookStorage(bool supported) { look_storage_ = supported; }
  // Batch 2 review (minor 3): builds the encode, decode and look pipelines of `output_format` now (the caller warms them at enable, outside the hooked
  // evaluate). False when the format has no variant or one could not be built: nothing would be recorded for it.
  bool Prepare(VkFormat output_format) { return color_.Prepare(output_format); }
  // Batch 3 review, minor 5: `output_format`'s pipelines are built already, so a recording builds nothing inside the hooked evaluate.
  [[nodiscard]] bool Prepared(VkFormat output_format) const { return color_.Prepared(output_format); }
  // NR on the region of DLSS's Output, inside the game's command buffer `buffer` right after DLSS's evaluate: opening barrier, encode (in place) into A at the
  // work image of `layout` (empty: the region's own size, Full), the Session's passes, the change path and the look, decode (in place), closing barrier.
  // The caller has checked the placement (the main handle, a tracked list), ticked the Session and issued its completion token before this call (the
  // token's event is set at the end of the hooked evaluate, whatever the result). `recorded` says whether anything went onto `buffer`. Nothing is recorded
  // for: Intensity 0 ("intensity 0": exact pass-through), an output format without a variant ("unsupported format"), a variant whose pipelines cannot be
  // built ("pipeline failed"), a region or a canvas below the NR floor (640x360) ("frame too small").
  PipelineResult RecordAfterDlss(VkCommandBuffer buffer, const VkDlssTarget& target, const nr::FrameInputs& inputs, const nr::Controls& controls,
                                 const WorkLayout& layout = {});
  // Task 10, Before upscaling (design §4.1, v2 design §3.9): NR on the region of the game's Color (`color.resource`: a sampled image view in
  // SHADER_READ_ONLY_OPTIMAL, only read) before DLSS-SR upscales it, inside the game's command buffer right before DLSS's evaluate. The region is resampled
  // into the SETTLED work image of `layout` (the set is keyed on it, never on the per-frame render region, Plan 4's I-1), mirror-padded into the floor
  // canvas (`layout.canvas`; the image itself when it meets the floor), the game's motion vectors are copied into a canvas motion image, NR runs at the
  // canvas, and the decode composes NR's change into PrivateColor() at the region, upsampled from the work image (Plan 14: a render region that moves while
  // the work size settles keeps NR running, R88). DLSS reads that as its Color for this evaluate (the NGX detour swaps it in). Nothing is recorded for:
  // Intensity 0, a Color format the sampled encode cannot read ("unsupported format"), a pipeline that cannot be built ("pipeline failed"), motion vectors
  // outside their image. The caller issued the token before this call; its event is set after DLSS has read the private colour. `nr_applied` says the
  // private colour holds NR's result, left in SHADER_READ_ONLY_OPTIMAL for DLSS.
  PipelineResult RecordPreSr(VkCommandBuffer buffer, const VkDlssTarget& color, const nr::FrameInputs& inputs, const nr::Controls& controls,
                             const WorkLayout& layout);
  // Plan 19: native NR at Present, inside Uplift's own command buffer `buffer` (never a game's, never ReShade's immediate one), which the caller submits to
  // the effect queue: the swap-chain image copied in (VkPresentTarget), RecordAfterDlss on the RGBA16F intermediate (no game exposure; UI correction as
  // LookConfig says; DLSS's Present copy, else Launchpad's UPLIFT_MV converted, else none, NR's history restarting when that provider changes), and the
  // result copied back only when NR applied. The image is back in its layout on return either way, behind a global dependency on everything after. The
  // intermediates are used on ReShade's queue alone, so they go at their marks through FreeFinished (never the timeline's pending releases). Nothing is
  // recorded for: Intensity 0, a frame or canvas below the NR floor, a pipeline that cannot be built, a busy descriptor slot, surfaces that cannot be made.
  PipelineResult RecordPresent(VkCommandBuffer buffer, const VkPresentTarget& target, const nr::Controls& controls, bool reset_hint,
                               const WorkLayout& layout = {});
  // After RecordPreSr applied NR and until the next recording: what DLSS reads as Color (a pointer to the struct the pipeline owns, valid while the set is).
  [[nodiscard]] const NVSDK_NGX_Resource_VK* PrivateColor() const {
    return intermediates_.private_color.image != VK_NULL_HANDLE ? &intermediates_.private_color.ngx : nullptr;
  }
  // The formats Before upscaling reads the game's Color in: the five output variants, and BGRA8 (a sampled view needs no storage format).
  [[nodiscard]] static bool PreSrColorFormatSupported(VkFormat format);
  // The game's motion vectors are read as floats (D3D12's MotionViewFormat: unconverted ones cannot be copied, E9): RG16F, RG32F, RGBA16F, RGBA32F.
  [[nodiscard]] static bool MotionFormatReadable(VkFormat format);
  // The colour pipeline's pre-SR pipelines (the sampled encode, the private-colour decode, the motion copy, the look), built now so the first compile stays
  // out of the hooked evaluate. False when one could not be built.
  bool PreparePreSr() { return color_.PreparePreSr(); }
  // Plan 14 (design §3.2, key decision 8 of Plan 5): Uplift's copy of UPLIFT_MASK on the game's device, an image of `format` (the effect texture's own, as
  // a format the decode samples raw: color::DescribeMaskFormat's view format) at `size`, which the add-on copies the effect's texture into at the end of
  // ReShade's effects through ReShade's command list. Recordings bind it (t3, SHADER_READ_ONLY_OPTIMAL) from NoteMaskCopied on. A format Uplift's decode
  // cannot sample, or an empty `size`, releases the copy and returns VK_NULL_HANDLE (as does an image that cannot be made). `*first`: nothing was copied
  // into it yet, so its layout is UNDEFINED (between copies it rests in SHADER_READ_ONLY_OPTIMAL).
  VkImage PrepareMaskCopy(VkFormat format, nr::Size size, bool* first);
  void NoteMaskCopied() { mask_.copied = (mask_.image.image != VK_NULL_HANDLE); }
  // Plan 17: the exposure the latest recording's encode read (the Details line), as Direct3D 12's NrPipeline.
  [[nodiscard]] ExposureReport LastExposure() const { return exposure_report_; }
  // Keep faces (2026-10-08): the device loads and stores the formats its recombination needs (the look's).
  [[nodiscard]] bool KeepFacesSupported() const { return look_storage_ && !color_.FacesFailed(); }
  // The mask copy, now when the GPU has passed its last use, else at its mark (Mask off, the effect off, NR off).
  void ReleaseMaskCopy() { RetireMask(); }
  // Plan 14 (design §2.3): at the hooked evaluate, with NR on the Present path, DLSS's motion vectors (`motion`, a sampled view in
  // SHADER_READ_ONLY_OPTIMAL, its region `region`) into the slot of `frame` (the present that closes this frame), at the region's own size and in its
  // pixels times (scale_x, scale_y). The slot ends in SHADER_READ_ONLY_OPTIMAL, the state the bridge's copy-in assumes. The Session is never involved: nothing
  // here loads NR. False (nothing recorded) when the slot or the constants ring is still in use, or an image cannot be made. The caller issued its
  // completion token before this call.
  // 1.1.6: `flip_y` when DLSS's images are upside down against the back buffer (ReShade's RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN).
  // Present-motion flicker fix: says why no copy was made (the per-second counters).
  PresentMotionWrite RecordPresentMotion(VkCommandBuffer buffer, const color::VkSampledView& motion, nr::Rect region, float scale_x, float scale_y,
                                         uint64_t frame, bool flip_y = false);
  // The copy for `frame` (PickPresentMotion: its own, else the newest at most PRESENT_MOTION_MAX_AGE frames older, whose age goes to `age`), or an empty one;
  // stamps its last use with the current mark (NR or the bridge reads it in this frame).
  [[nodiscard]] vk::NrImage PresentMotion(uint64_t frame, uint64_t* age = nullptr);
  [[nodiscard]] bool HasPresentMotion(uint64_t frame) const;  // PresentMotion without the stamp
  // The motion copy's pipeline, built now so its first compile stays out of the hooked evaluate. False when it could not be built.
  bool PreparePresentMotion() { return color_.PrepareMotion(); }
  // The slots, freed now when the GPU has passed them, else at their marks (FreeFinished); the ring starts at four slots again.
  void ReleasePresentMotion();
  // Frees the intermediates, the look surfaces and the mask copy now when the GPU has passed the newest recording that used them (a drain to OFF normally
  // has); otherwise hands them to the timeline (the mask, and Plan 19's Present surfaces: FreeFinished), which frees them once it does. The Present motion
  // copies are left alone (ReleasePresentMotion).
  void ReleaseIntermediates();
  // ReleaseIntermediates without the mask copy: A, B, the change field, the look surfaces and the meter's state, which only the game's command buffers use,
  // so their completion tokens cover them (the game's NGX shutdown, final review C-1).
  void ReleaseNrSurfaces();
  // Final review C-1: the images ReShade's queue also uses (a Present copy slot, read by the bridge's copy-in; the mask copy, written by the add-on's copy)
  // never go through the timeline's pending releases, which a flush runs whatever the GPU still does (the Session's, at the game's NGX shutdown), and
  // no completion token covers that queue. Retired ones wait here: this frees those whose mark is complete. Returns how many still wait. The
  // destructor frees the rest (after the game's idle).
  size_t FreeFinished();
  [[nodiscard]] uint64_t HeldBytes() const {
    const uint64_t motion_images = (intermediates_.zero_motion.image != VK_NULL_HANDLE ? 1u : 0u) + (intermediates_.canvas_motion.image != VK_NULL_HANDLE ? 1u : 0u);
    return (intermediates_.a.image != VK_NULL_HANDLE ? intermediates_.bytes : 0u) + motion_images * intermediates_.motion_bytes + present_motion_bytes_
           + look_.bytes + (mask_.image.image != VK_NULL_HANDLE ? mask_.bytes : 0u) + (exposure_.state.image != VK_NULL_HANDLE ? EXPOSURE_STATE_BYTES : 0u)
           + present_.bytes + present_.launchpad_bytes + faces_.bytes;
  }

 private:
  struct Intermediates {
    vk::NrImage a;  // the model texture NR reads first; RGBA16F at `size`
    vk::NrImage b;
    // RG16F at `size`. Batch 2 review (minor 4): made at the first frame that needs one, and freed with the set.
    vk::NrImage zero_motion;    // MotionVectors = None (or Before upscaling's canvas without readable vectors)
    vk::NrImage canvas_motion;  // Before upscaling in a canvas: the game's vectors copied in, mirrored like the image
    vk::NrImage private_color;  // Before upscaling: RGBA16F at the game's Color dimensions, what DLSS reads as Color
    vk::NrImage change;         // Plan 14: RGBA16F at `image`, below the output size and always before upscaling (Direct3D 12's main-set C)
    nr::Size size;              // A and B: the canvas NR runs at (the work image, padded before upscaling below the floor)
    nr::Size image;             // the work image: the region resampled to this size
    nr::Size output;            // After DLSS: the output region the set was made for; Before upscaling: the work image (I-1)
    nr::Size color_size;        // Before upscaling: the game's Color dimensions; empty for After DLSS
    bool pre_sr = false;
    uint64_t bytes = 0u;         // what the budget is charged for A, B, the change field and the private colour
    uint64_t motion_bytes = 0u;  // and for each motion image, while it exists
    bool reset_pending = true;   // a new set, or a new encoding: NR's history starts afresh (Plan 3)
    color::Encoding encoding = color::Encoding::AUTO;
    float input_scale = 0.f;
    nr::Mark last_use;  // the newest recording on this set: the GPU is done with it once complete
  };

  // Plan 14 (Direct3D 12's LookSurfaces): the look stage's surfaces (sources::LookPlan), apart from the main set so that the look never restarts NR's history.
  struct LookSurfaces {
    LookPlan plan;
    look::Atlas atlas;
    vk::NrImage change;                         // own_change: RGBA16F at the image size
    vk::NrImage basis;                          // κ_M, RG16F
    vk::NrImage gauss;                          // the pyramid atlas, RGBA16F
    vk::NrImage peaks;                          // the max-pyramid atlas, R16F
    std::array<vk::NrImage, 2> history;         // the stabilised levels, ping-pong
    std::array<vk::NrImage, 2> detail_history;  // the stabilised fine band, ping-pong
    vk::NrImage state;                          // 1x1 RGBA32F: the scene cut
    uint32_t current = 0u;                      // the history this frame writes
    bool history_valid = false;                 // the histories hold a previous frame
    bool carried = false;                       // the histories and the state were moved UNDEFINED -> GENERAL (once: they carry across frames)
    uint64_t bytes = 0u;
    nr::Mark last_use;
  };

  // Keep faces (2026-10-08), Direct3D 12's FaceSurfaces: the twin's output (an NGX output, then the face mask the later passes read) and the pyramid of a pass's
  // weighted change, at the canvas. Apart from the main set, made while Keep faces is on, opened each recording like A and B, and freed like the look set.
  struct FaceSurfaces {
    vk::NrImage twin;    // RGBA16F
    vk::NrImage detail;  // RGBA16F: fix round 4's despiked difference (made with the others, so Remove specks never reallocates)
    vk::NrImage atlas;   // RGBA16F, look::MakeAtlas(size)
    vk::NrImage fill;    // RGBA16F, the same size: round 5's fill pyramid (made with the others, so Fill skin by colour never reallocates)
    look::Atlas layout;
    nr::Size size;
    uint64_t bytes = 0u;
    nr::Mark last_use;
  };

  // Plan 14: Uplift's copy of the effect's UPLIFT_MASK, sampled in SHADER_READ_ONLY_OPTIMAL once copied into.
  struct MaskCopy {
    vk::NrImage image;
    nr::Size size;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t bytes = 0u;
    bool copied = false;  // the add-on has copied into it: recordings may bind it, and it rests in SHADER_READ_ONLY_OPTIMAL
    nr::Mark last_use;
  };

  // After NR: what the decode reads as t1.
  struct AfterNr {
    VkImageView view = VK_NULL_HANDLE;            // NR's output, or the (shaped) change field
    std::optional<color::Upsampling> upsampling;  // set: `view` is the change field
    bool ok = true;                               // false: the change pass could not be recorded
  };

  // v2 design §3.14: the governor's RGBA32F state, (2^E, E, the last anchor, set), which the encode and the decode read as their exposure. It carries across
  // frames in GENERAL, as the stabiliser's histories do. Plan 17: 2x1, the second texel Auto's check (the game's exposure beside the meter's target).
  struct MeterState {
    vk::NrImage state;
    bool snap = true;      // the next meter snaps: a new image, or a changed colour fix
    bool carried = false;  // moved UNDEFINED -> GENERAL (once)
    nr::Mark last_use;
  };
  // What the encode and the decode read as their exposure (InputExposure).
  struct ExposureChoice {
    color::VkSampledView view;
    float factor = 1.f;
    bool metered = false;
    bool probe = false;  // Plan 17: the game's exposure is used, and the meter runs beside it for Auto's check
  };
  // Plan 17: Auto's check, as Direct3D 12's: the state's two texels copied into a host-visible ring after the meter, read once the GPU has passed them.
  static constexpr uint32_t CHECK_SLOTS = 4u;
  struct ExposureCheck {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const std::byte* mapped = nullptr;  // host-coherent, mapped for the buffer's life
    std::array<nr::Mark, CHECK_SLOTS> marks = {};
    std::array<bool, CHECK_SLOTS> pending = {};
    uint32_t next = 0u;  // the oldest slot, written next
  };

  // One of the Present motion copies (PRESENT_MOTION_SLOTS): the frame it was made for (0 = none) and the newest recording that wrote or read it.
  struct PresentMotionSlot {
    vk::NrImage image;
    uint64_t frame = 0u;
    nr::Mark last_use;
  };
  static constexpr size_t PRESENT_MOTION_SLOTS = ::uplift::sources::PRESENT_MOTION_SLOTS;  // flicker fix: room for 8; 4 in use until the ring grows
  [[nodiscard]] std::array<uint64_t, PRESENT_MOTION_SLOTS> PresentMotionFrames() const;
  static constexpr size_t MAX_OPENED = 24u;              // the images one recording moves UNDEFINED -> GENERAL at its start (Before upscaling: 20)
  static constexpr uint64_t EXPOSURE_STATE_BYTES = 32u;  // the meter's 2x1 RGBA32F state

  // Plan 19: native NR at Present's own images, used in Uplift's own command buffers on ReShade's queue alone (their marks are frames: that queue's frame
  // signal follows each submission).
  struct PresentSurfaces {
    vk::NrImage staging;    // `format`: the swap-chain image's bits, copied raw
    vk::NrImage color;      // RGBA16F: NR's target, in place
    vk::NrImage launchpad;  // RG16F at `launchpad_size`: UPLIFT_MV in the work image's pixels
    vk::NrImage stand_in;   // stress round: RG16F, cleared at each use: the motion input's stand-in for a frame its source missed
    nr::Size stand_in_size;
    nr::Size size;
    VkFormat format = VK_FORMAT_UNDEFINED;
    nr::Size launchpad_size;
    uint64_t bytes = 0u;  // the staging image and the intermediate, while they exist
    uint64_t launchpad_bytes = 0u;
    nr::Mark last_use;
    nr::Mark launchpad_last_use;
  };

  // An image ReShade's queue also uses, retired at `mark` (FreeFinished).
  struct RetiredImage {
    vk::NrImage image;
    nr::Mark mark;
  };

  void RetireSet();  // freed now if the GPU is done with them, else at their mark
  void RetireLook();
  void RetireMask();
  void RetirePresent();           // Plan 19: the staging image and the intermediate (and Launchpad's conversion)
  void RetirePresentLaunchpad();  // Plan 19: Launchpad's conversion alone (its vectors stopped coming)
  // Plan 19: UPLIFT_MV into present_.launchpad at `work`'s canvas, in the work image's pixels, filtered as the motion copy filters (non-finite vectors dropped,
  // the rest clamped), then handed to NGX in SHADER_READ_ONLY_OPTIMAL. False when nothing was written (NR then binds no vectors).
  bool ConvertLaunchpadMotion(VkCommandBuffer buffer, const VkPresentTarget& target, const WorkLayout& work);
  // Stress round: present_.stand_in at `size`, cleared to zero and handed to NGX in SHADER_READ_ONLY_OPTIMAL; null when it cannot be made.
  const NVSDK_NGX_Resource_VK* ZeroStandIn(VkCommandBuffer buffer, nr::Size size);
  void RetireExposure();
  void RetireExposureCheck();
  bool EnsureExposureCheck();  // the readback ring, made on first use
  void PollExposureChecks();   // reads the samples the GPU has passed into auto_exposure_
  // Final review C-1: frees `image` now when `mark` is complete, else keeps it in `reshade_retired_` (never in the timeline's pending releases).
  void RetireReshadeImage(const vk::NrImage& image, const nr::Mark& mark);
  // The Session's nr::PassResolver: pass `index`'s own Transfer and Colour strength applied to its raw output in place (Direct3D 12's ResolvePass).
  void ResolvePass(ID3D12GraphicsCommandList* list, uint32_t index, ID3D12Resource* given, ID3D12Resource* returned) override;
  // Keep faces: pass 1's recombination, after the Session's twin evaluate (Direct3D 12's CombineFaces).
  void CombineFaces(ID3D12GraphicsCommandList* list, ID3D12Resource* lighting, ID3D12Resource* faces) override;
  // Keep faces: the twin and the atlas at `canvas`, made now unless held. False (logged once): Keep faces is skipped this recording.
  bool EnsureFaces(nr::Size canvas);
  void RetireFaces();
  // Keep faces: one pass's recombination (`pass.changed` rewritten) on the recording's slot: a barrier, the pyramid of its weighted change, the combine.
  void RecordFaces(VkCommandBuffer buffer, color::VkFacesPass pass);
  // Keep faces, before a recording's Session evaluate: the twin's surfaces and `frame_inputs->faces` while it is on (and can run), else its surfaces go.
  // Call before OpenRecording, which opens the surfaces.
  void PrepareFaces(nr::Size canvas, const nr::Controls& controls, nr::FrameInputs* frame_inputs);
  // The look set for `plan`, made now unless it is the one held. False (logged once): the look (and C at the output size) is skipped this recording.
  bool EnsureLook(const LookPlan& plan);
  [[nodiscard]] LookPlan LookPlanFor(nr::Size image, bool reduced) const;  // sources::PlanLook for this device and this set
  [[nodiscard]] uint32_t ShaderOptions() const;                            // the encode's and the decode's shader_options
  [[nodiscard]] color::VkSampledView BoundMask(uint32_t slot);             // the mask copy for this recording, or none
  // A global memory dependency: everything written before `source_stage` is visible to everything after (every Uplift image stays in GENERAL).
  void Barrier(VkCommandBuffer buffer, VkPipelineStageFlags source_stage) const;
  // The opening of a recording: the game's earlier writes are visible to Uplift's reads and writes, and `images` (those that exist) move UNDEFINED -> GENERAL
  // (their contents are rewritten in full every recording); `with_look`: the look's surfaces too, and its histories and scene-cut state the first time only
  // (they carry across frames). Then the colour pipeline's placeholders likewise.
  void OpenRecording(VkCommandBuffer buffer, std::span<const vk::NrImage* const> images, bool with_look);
  // An Uplift image NGX reads as an input (the zero or canvas motion, the private colour) moves GENERAL -> SHADER_READ_ONLY_OPTIMAL, as a game's own inputs
  // are (R79), with the write finished and visible to everything after.
  void HandToNgx(VkCommandBuffer buffer, const vk::NrImage& image) const;
  // The game's exposure texture as a sampled view (SHADER_READ_ONLY_OPTIMAL), or empty when it is not an image view.
  [[nodiscard]] static color::VkSampledView GameExposureView(const NVSDK_NGX_Resource_VK* exposure);
  // Direct3D 12's ChooseExposure: the game's exposure, the meter's state (made now when it is wanted and the device can), or none. Call before OpenRecording.
  ExposureChoice ChooseExposure(color::Encoding encoding, const NVSDK_NGX_Resource_VK* game_texture, float game_factor);
  // The meter before the encode (`pass`'s state, snap, smoothing, rates and Δt are filled here), then a barrier so the encode reads what it wrote. False: no
  // dispatch could be recorded.
  // Plan 17: with `exposure.probe`, the meter also writes Auto's check, and its state is copied into the readback ring.
  bool RecordMeter(VkCommandBuffer buffer, uint32_t slot, color::VkMeterPass pass, const ExposureChoice& exposure);
  // The stabiliser's motion vectors (the game's own, current -> previous) with a resolved rect and their scale, or none: absent or unreadable vectors make
  // Motion change-gated static (decision D4), as Direct3D 12's StabilizeMotionOf.
  [[nodiscard]] static color::VkStabilizeMotion StabilizeMotionOf(const nr::FrameInputs& inputs);
  // The target's input scale, and NR's history restarting when the encoding or the scale changed (Plan 2 final review M4).
  float NoteEncoding(const VkDlssTarget& target);
  // Direct3D 12's RecordAfterNr: the change field and the look stage after NR, or NR's own output for the full-size restore. Ends with the look's writes
  // visible to the decode.
  AfterNr RecordAfterNr(VkCommandBuffer buffer, uint32_t slot, const color::VkEncodePass& encode, VkImageView nr_output, bool reduced, bool look_ready,
                        color::Upsampling upsampling, const color::VkStabilizeMotion& motion, bool reset);
  void LogAllocationFailure(nr::Size size);  // once until an allocation works again

  vk::NrFunctions functions_;
  VkDevice device_;
  nr::Session& session_;
  nr::Timeline& timeline_;
  color::VkColorPipeline color_;
  VkPhysicalDeviceMemoryProperties memory_;
  LookConfig config_;
  bool look_storage_ = true;
  Intermediates intermediates_;
  LookSurfaces look_;
  FaceSurfaces faces_;
  bool faces_ran_ = false;  // Keep faces: this recording's pass 1 ran its twin, so the later passes keep only their broad change inside the face mask
  MaskCopy mask_;
  MeterState exposure_;
  ExposureCheck check_;
  AutoExposure auto_exposure_;  // Plan 17: kept for the pipeline's life (a latch never flips back)
  ExposureReport exposure_report_;
  uint32_t resolve_slot_ = 0u;  // the recording's ring slot, for ResolvePass
  std::array<PresentMotionSlot, PRESENT_MOTION_SLOTS> present_motion_;
  nr::Size present_motion_size_;        // the slots' size: the motion region
  uint64_t present_motion_bytes_ = 0u;  // the slots in use, while they exist
  size_t present_motion_slots_ = PRESENT_MOTION_MIN_SLOTS;  // fix round 1 (M6): 4, or 8 once a copy was skipped for a slot still read
  // Makes present_motion_[first, end) at `size`; on a failure none of them stays (logged) and false.
  bool MakePresentMotionSlots(size_t first, size_t end, nr::Size size);
  PresentSurfaces present_;                                         // Plan 19
  MotionSource present_motion_source_ = MotionSource::NONE;         // Plan 19: the latest Present recording's provider (a switch restarts NR's history)
  std::vector<RetiredImage> reshade_retired_;  // final review C-1: retired slots and mask copies whose mark is not complete yet
  std::array<nr::Mark, color::VkColorPipeline::RING_SLOTS> slot_marks_ = {};
  uint32_t next_slot_ = 0u;
  bool allocation_failure_logged_ = false;
};

}  // namespace uplift::sources
