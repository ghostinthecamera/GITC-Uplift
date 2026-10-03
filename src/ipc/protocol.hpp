#pragma once

// Plan 9 (design §2.2): the control block the add-ons (gitc-uplift.addon32, and since Plan 10 gitc-uplift.addon64 for Direct3D 9 games) and
// gitc-uplift-helper64.exe (x64) share. Fixed-width fields only. Any change bumps PROTOCOL_VERSION and CONTROL_BLOCK_BYTES, which both
// builds assert.

#include <cstddef>
#include <cstdint>

namespace uplift::ipc {

inline constexpr uint32_t MAGIC = 0x464C5055u;    // "UPLF"
// 2: Target::block_mapped; 3: Api::D3D10, Share::kmt_handle, mask/motion blocks; 4: Api::VULKAN, Transport::SHARED_CPU;
// 5 (Plan 12): Api::OPENGL, Api::D3D12, Handles::color_bytes (and FENCED's mask_bytes and motion_bytes), Reply::to12_completed and to11_completed;
// 6 (Plan 14): Status::placement, motion_source, dlss_motion_gap, launchpad_gap, resolution_applied, upsampling and the work, canvas and frame sizes
// (12 uint32_t, +48 bytes): what the 32-bit add-on's Setup and status card read; 7 (Plan 17): Status::exposure_line (+256 bytes).
inline constexpr uint32_t PROTOCOL_VERSION = 7u;
inline constexpr size_t TEXT_BYTES = 256u;
inline constexpr size_t PATH_BYTES = 1024u;
inline constexpr size_t BUILD_ID_BYTES = 64u;
inline constexpr size_t SETTINGS_BYTES = 16384u;
inline constexpr size_t LOG_BYTES = 32768u;

enum class HelperState : uint32_t {
  STARTING,
  READY,
  FAILED,
};
enum class Api : uint32_t {
  D3D9,
  D3D11,
  D3D10,   // Plan 10: a 32-bit Direct3D 10 game's relay device; the helper only logs it
  VULKAN,  // Plan 11: a Vulkan game's device (DXVK in a 32-bit game); the helper only logs it
  OPENGL,  // Plan 12: an OpenGL game's context (a 32-bit game); the helper only logs it
  D3D12,   // Plan 12: a 32-bit Direct3D 12 game's device (dgVoodoo2's D3D12 output); the helper only logs it
};
enum class Transport : uint32_t {
  NONE,
  BLOCK,
  KMT,
  FENCED,
  SHARED_CPU,  // Plan 11: FENCED's NT textures with no fences, CPU-ordered by the add-on (a Vulkan device without VK_KHR_external_semaphore_win32)
};
enum class RequestKind : uint32_t {
  NONE,
  ATTACH,
  FRAME,
  RUN,
  SHARE_MASK,
  SHARE_MOTION,
  STOP,
  DETACH,
  QUIT,
};

struct Attach {
  uint32_t luid_low = 0u;
  int32_t luid_high = 0;
  Api api = Api::D3D9;
  Transport transport = Transport::NONE;
  char snippet_path[PATH_BYTES] = {};        // UTF-8: addon::LocateSnippet in the add-on; empty = not found
  char ngx_data_directory[PATH_BYTES] = {};  // UTF-8: UpliftStateDirectory() / "ngx"
  char addon_file[TEXT_BYTES] = {};          // "gitc-uplift.addon32" or "gitc-uplift.addon64", for the runtime card
};

struct Target {
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t dxgi_format = 0u;      // the twin's (design §2.8)
  uint32_t color_space = 0u;      // color::ColorSpace
  uint64_t kmt_handle = 0u;       // KMT: the add-on's shared texture; 0 while it has none
  uint32_t kmt_generation = 0u;   // bumped whenever the add-on replaced it
  uint32_t block_mapped = 0u;     // BLOCK: this add-on has a frame block mapped for this size; while 0 the helper (re)makes and hands one out
  char problem[TEXT_BYTES] = {};  // why this image cannot reach NR (the add-on's side); empty = usable
};

struct Frame {
  uint64_t settings_generation = 0u;
  uint64_t game_budget = 0u;  // the game process's DXGI local budget and usage; 0 = not measured this frame
  uint64_t game_usage = 0u;
  uint32_t drag_bits = 0u;  // ui::DragState, bit i = its i-th field in declaration order
  uint32_t defaults_view = 0u;
  uint32_t marker_expected = 0u;
  uint32_t nr_allowed = 1u;
  uint32_t unreported_point = 0u;  // addon::TriggerPoint the add-on reached last frame without a RUN
  uint32_t retry_now = 0u;
  uint32_t mask_running = 0u;  // an enabled technique writes UPLIFT_MASK (helper_front.hpp's MaskEffectRunning); Plan 10: every transport
  uint32_t reserved = 0u;
  Target target;
};

struct Run {
  uint64_t in = 0u;          // FENCED: the to12 value D3D11 signalled and flushed
  uint64_t out = 0u;         // FENCED: the to11 value D3D11 will wait for, once the reply says it was submitted
  uint32_t point = 0u;       // addon::TriggerPoint
  uint32_t mask_fresh = 0u;  // UPLIFT_MASK was copied into the shared mask since the last RUN (Plan 10: BLOCK and KMT too)
  uint32_t motion = 0u;      // UPLIFT_MV was copied into the shared motion for this RUN (Plan 10: BLOCK and KMT too)
  uint32_t reserved = 0u;
};

struct Share {  // SHARE_MASK, SHARE_MOTION
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t dxgi_format = 0u;
  uint32_t reserved = 0u;
  uint64_t kmt_handle = 0u;  // KMT: the add-on's shared texture, which the helper opens; 0: the helper makes it (FENCED, BLOCK)
};

struct Stop {
  uint64_t waited11 = 0u;  // the highest to11 value a D3D11 wait was queued for
  char reason[TEXT_BYTES] = {};
};

struct Request {
  RequestKind kind = RequestKind::NONE;
  uint32_t sequence = 0u;
  Attach attach;
  Frame frame;
  Run run;
  Share share;
  Stop stop;
};

// NT handle values in the helper's table; the add-on pulls them (DuplicateHandle with DUPLICATE_CLOSE_SOURCE). 0 = none new.
struct Handles {
  uint64_t color = 0u;
  uint64_t mask = 0u;
  uint64_t motion = 0u;
  uint64_t to12 = 0u;
  uint64_t to11 = 0u;
  uint64_t block = 0u;  // BLOCK: the frame's file mapping
  uint64_t block_bytes = 0u;
  uint32_t block_row_pitch = 0u;
  uint32_t reserved = 0u;
  uint64_t mask_bytes = 0u;  // BLOCK: `mask` is a file mapping of this size (the helper opened it as a heap); FENCED and SHARED_CPU (Plan 12): the NT texture's allocation size
  uint64_t motion_bytes = 0u;  // the same
  uint32_t mask_row_pitch = 0u;
  uint32_t motion_row_pitch = 0u;
  uint64_t color_bytes = 0u;  // FENCED and SHARED_CPU (Plan 12): the colour NT texture's allocation size (GetResourceAllocationInfo): OpenGL's memory-object import needs it
};

// What the overlay and the status card need (design §2.1); texts are NUL-terminated UTF-8, truncated.
struct Status {
  uint64_t intermediate_bytes = 0u;  // DeviceContext's, plus the helper's transport surfaces
  uint64_t runtime_bytes = 0u;
  uint64_t busy_skips = 0u;
  int64_t grace_remaining_ms = 0;
  int64_t retry_in_ms = -1;     // -1 = none
  uint32_t session_state = 0u;  // nr::SessionState
  uint32_t has_runtime_bytes = 0u;
  uint32_t suspended = 0u;
  uint32_t passes_requested = 1u;
  uint32_t passes_run = 0u;
  uint32_t retries_exhausted = 0u;
  uint32_t device_lost = 0u;
  uint32_t nr_applied = 0u;
  uint32_t skip_from_session = 0u;
  uint32_t blocked_stage = 0u;  // ui::CardStage
  uint32_t trigger = 0u;        // addon::TriggerPoint used most recently
  float diffuse_white_nits = 0.f;
  // Plan 14 (protocol 6): the context's enums as numbers (addon::Placement, sources::MotionSource, ui::MotionGap x2, ui::ResolutionMode, color::Upsampling).
  uint32_t placement = 0u;
  uint32_t motion_source = 0u;
  uint32_t dlss_motion_gap = 0u;
  uint32_t launchpad_gap = 0u;
  uint32_t resolution_applied = 0u;
  uint32_t upsampling = 0u;
  uint32_t work_width = 0u;  // ContextStatus::work: the settled work image
  uint32_t work_height = 0u;
  uint32_t canvas_width = 0u;  // ContextStatus::canvas: NR's size
  uint32_t canvas_height = 0u;
  uint32_t frame_width = 0u;  // ContextStatus::frame: what the resolution mode scales
  uint32_t frame_height = 0u;
  char session_message[TEXT_BYTES] = {};
  char blocked[TEXT_BYTES] = {};
  char output_problem[TEXT_BYTES] = {};
  char placement_note[TEXT_BYTES] = {};
  char skip_reason[TEXT_BYTES] = {};
  char status_line[TEXT_BYTES] = {};
  char placement_line[TEXT_BYTES] = {};
  char motion_line[TEXT_BYTES] = {};
  char work_line[TEXT_BYTES] = {};
  char ui_correction_note[TEXT_BYTES] = {};
  char transport_line[TEXT_BYTES] = {};  // the helper's half of the Details line
  char exposure_line[TEXT_BYTES] = {};   // Plan 17: ContextStatus::exposure_line
};

struct Reply {
  uint32_t sequence = 0u;
  uint32_t ok = 0u;
  uint32_t frame_ready = 0u;  // FRAME: DeviceContext::FrameReady()
  uint32_t running = 0u;      // FRAME: NR's state is not OFF
  uint32_t waited = 0u;       // RUN
  uint32_t submitted = 0u;
  uint32_t wrote = 0u;
  uint32_t signalled = 0u;
  uint32_t busy = 0u;
  uint32_t reserved = 0u;
  // Plan 12: FENCED's two fences' completed values as the helper read them when it wrote this reply (UINT64_MAX: its device was removed; 0 on every other
  // transport). A GL semaphore cannot be read from the game's side, so the OpenGL client gets the helper's view of them in every reply.
  uint64_t to12_completed = 0u;
  uint64_t to11_completed = 0u;
  char error[TEXT_BYTES] = {};
  Handles handles;
  Status status;
};

struct LogRing {
  uint32_t written = 0u;  // bytes ever written (wraps); interlocked
  uint32_t read = 0u;     // bytes ever read; interlocked
  char bytes[LOG_BYTES] = {};
};

struct ControlBlock {
  uint32_t magic = MAGIC;
  uint32_t protocol = PROTOCOL_VERSION;
  uint32_t block_bytes = 0u;
  uint32_t helper_pid = 0u;
  char build_id[BUILD_ID_BYTES] = {};
  uint32_t helper_state = 0u;  // HelperState
  uint32_t luid_low = 0u;      // the game's adapter, written before launch: the helper makes D3D12Side before READY
  int32_t luid_high = 0;
  uint32_t reserved = 0u;
  uint64_t heartbeat = 0u;  // grows with the helper's progress: requests finished, its GPU progress, and a thread's beat during a long handler
  char helper_error[TEXT_BYTES] = {};
  uint64_t settings_generation = 0u;
  char settings_text[SETTINGS_BYTES] = {};
  Request request;
  Reply reply;
  LogRing log;
};

inline constexpr size_t CONTROL_BLOCK_BYTES = 56136u;  // protocol 6's 55880 + 256 (Status::exposure_line); the x64 and x86 builds both assert it
static_assert(sizeof(ControlBlock) == CONTROL_BLOCK_BYTES, "the add-on and the helper disagree on the control block");
static_assert(offsetof(ControlBlock, request) % 8u == 0u && offsetof(ControlBlock, reply) % 8u == 0u);
// Both processes use the Interlocked*64 functions on it, which need 8 alignment on x86 too.
static_assert(offsetof(ControlBlock, heartbeat) % 8u == 0u);

}  // namespace uplift::ipc
