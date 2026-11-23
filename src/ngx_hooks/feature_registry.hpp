#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "ngx_hooks/ngx_api.hpp"
#include "nr/ngx.hpp"
#include "nr/types.hpp"

namespace uplift::ngx_hooks {

// How long a DLSS feature goes without an evaluate before it counts as paused. One interval for the whole add-on: a main feature the game has stopped
// evaluating (MainHandle) and the game's DLSS paused in a context (no main evaluate) share it.
// Independent of GraceSeconds: a zero grace must not classify the evaluate preceding every Present as idle. The same minimum also debounces the game's
// release before DLSS counts as switched off.
inline constexpr std::chrono::milliseconds DLSS_IDLE_MINIMUM{250};

// The create-time keys of a DLSS feature.
struct CreateSnapshot {
  nr::Size render;        // Width × Height: the largest render size
  nr::Size output;        // OutWidth × OutHeight: the output region's size
  int perf_quality = -1;  // PerfQualityValue; -1 when not set
  int flags = 0;          // DLSS.Feature.Create.Flags

  [[nodiscard]] bool IsHdr() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0; }
  [[nodiscard]] bool MvLowRes() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0; }
  [[nodiscard]] bool MvJittered() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0; }
  [[nodiscard]] bool DepthInverted() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0; }
  [[nodiscard]] bool AutoExposure() const { return (flags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0; }
};

[[nodiscard]] CreateSnapshot ReadCreateSnapshot(const NVSDK_NGX_Parameter& parameters);

enum class FeatureKind : uint8_t {
  OTHER,
  UPSCALER,          // SuperSampling (1) or RayReconstruction (13): the After-DLSS input
  FRAME_GENERATION,  // FrameGeneration (11)
  NEURAL_RENDERING,  // Reserved18: another NR producer, since Uplift never creates NR through the core
};

[[nodiscard]] FeatureKind KindOf(NVSDK_NGX_Feature feature);

struct FeatureRecord {
  NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_Reserved_Unknown;
  const void* device = nullptr;  // opaque key of the device the feature was created on; null = unknown
  CreateSnapshot snapshot;
  uint64_t serial = 0u;  // creation order, for stable ties
  NgxApi api = NgxApi::D3D12;  // the API the feature was created through
  // Its create, its adoption, or its latest successful evaluate; when its current run began, its evaluates since with no gap over
  // DLSS_IDLE_MINIMUM; and the gaps before its last two evaluates (zero before them), whose shorter is its cadence. MainHandle reads them all:
  // whether the game still evaluates the feature, whether steadily, and how long its own cadence lets it go quiet.
  std::chrono::steady_clock::time_point last_active;
  std::chrono::steady_clock::time_point run_start;
  std::chrono::steady_clock::duration gap{};
  std::chrono::steady_clock::duration previous_gap{};
};

// The live NGX features, fed by the create and release hooks from any thread.
class FeatureRegistry {
 public:
  void OnCreate(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device, const CreateSnapshot& snapshot,
                NgxApi api = NgxApi::D3D12);
  // A feature the game created before Uplift's hooks were in place, adopted at its first hooked evaluate: registered as OnCreate
  // registers it, unless `handle` already is (a hooked create on another thread won the race, and its record is the right one). True when this call
  // registered it.
  bool TryAdopt(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Feature feature, const void* device, const CreateSnapshot& snapshot, NgxApi api);
  // The unknown handles whose evaluate was looked at and not adopted, so they are not probed again every frame. A create, a
  // release or a core shutdown forgets them: the core reuses handles.
  void NoteNotAdopted(const NVSDK_NGX_Handle* handle);
  [[nodiscard]] bool NotAdopted(const NVSDK_NGX_Handle* handle) const;
  void OnRelease(const NVSDK_NGX_Handle* handle);
  // A successful evaluate of `handle` returned at `when`. A registered upscaler's record keeps it as its last activity with the gap before it, and
  // starts a new run after a gap over DLSS_IDLE_MINIMUM (MainHandle); a `when` no later than the record's changes nothing. A registered NEURAL_RENDERING feature's is
  // kept for LastEvaluated (Uplift never registers its own NR). Any other handle, known or not, is ignored. Any game thread may call it; it takes
  // the exclusive lock, since it writes a record.
  void OnEvaluate(const NVSDK_NGX_Handle* handle, std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now());
  // When a registered NEURAL_RENDERING feature last evaluated; nullopt when none has. A release keeps the time (the reader ages it out), and so
  // does a core shutdown. Limit: a feature the game created before Uplift's hooks were installed is never registered, so its
  // evaluations are not seen. Other kinds read as nullopt (an upscaler's activity is its record's).
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> LastEvaluated(FeatureKind kind) const;
  // The game shut NGX down for `api`: drops `api`'s features of `device` and of the unknown device (null); a null `device` drops every
  // feature of `api`. Another API's features are never touched, and the sticky create counts stay (as after a release). Returns how many went.
  size_t OnCoreShutdown(NgxApi api, const void* device);
  // The device is gone: UpscalerCreated forgets it, so a device recreated at the same address does not start as "DLSS seen".
  void OnDeviceDestroyed(const void* device);
  [[nodiscard]] std::optional<FeatureRecord> Find(const NVSDK_NGX_Handle* handle) const;
  [[nodiscard]] uint32_t LiveCount(FeatureKind kind) const;
  // The game created a DLSS-SR or Ray Reconstruction feature on `device` at some point this session. Sticky: a release does not clear
  // it. Fed by every create, so the Vulkan passthrough (no watching contexts) learns it too.
  [[nodiscard]] bool UpscalerCreated(const void* device) const;
  // How many DLSS-SR or Ray Reconstruction features the game has created on `device` this session (0 for none, or once the device went). A
  // create after the game's own NGX shutdown proves it re-initialised NGX there (a DLSS setting changed), which ends Uplift's hold on the device.
  [[nodiscard]] uint64_t UpscalerCreates(const void* device) const;
  // The game's DLSS is live on `device`: a DLSS-SR or Ray Reconstruction feature of `api` is created and not yet released there, or on an
  // unknown device (MainHandle's rule: it may be this device's main handle). A pause (a live feature the game does not evaluate) is live; the release of the
  // last one is not, nor a Direct3D 11 NGX shutdown (OnCoreShutdown), the only shutdown that reaches the registry. An unknown device's upscaler that is
  // never released keeps every device of its API live (fail-safe: today's wait).
  [[nodiscard]] bool UpscalerLive(const void* device, NgxApi api) const;
  // The serial of the newest feature created so far, on any device (0 before the first create). A feature whose serial is above the value
  // read at the game's NGX shutdown was created after it.
  [[nodiscard]] uint64_t LastSerial() const;
  // The upscaler on `device` (or of an unknown device) whose output region
  // best matches the swap chain: an aspect within 1 % first, then the largest output, then the
  // earliest create. With an empty `swapchain`, the largest output. Null when no upscaler is live. Only `api`'s features compete (an unknown device's
  // Direct3D 12 feature, a mod's, is never a Direct3D 11 device's main handle).
  // That winner stays unless the game has stopped evaluating it: it is not `evaluating`, and has gone DLSS_IDLE_MINIMUM past its own cadence (the
  // shorter of its last two gaps) without an evaluate. Then a feature of the same view, one sharing an output width or height with the winner or
  // with `swapchain`, that the game has evaluated with no gap over DLSS_IDLE_MINIMUM for longer than that takes its place (the best of them by the
  // same ranking). So a DLSS feature a mod never releases does not stay main, also after a resolution change, while a winner on a settled slow
  // rhythm (two gaps alike), a pause with no same-view feature running, or a view sharing no dimension changes nothing. Not told from abandonment,
  // so they may move main: a slowdown's first two slow gaps, a winner evaluated in bursts (two evaluates close together, then a gap over
  // DLSS_IDLE_MINIMUM), and a frame over DLSS_IDLE_MINIMUM with a same-view feature evaluated twice inside it. A winner whose last two evaluates
  // were each isolated by long gaps gives way only that long after its last.
  // On Direct3D 12 and Vulkan the evaluating handle may resolve an exact nonempty output-size tie; Direct3D 11 retains the creation-order tie-break.
  [[nodiscard]] const NVSDK_NGX_Handle* MainHandle(const void* device, nr::Size swapchain, NgxApi api = NgxApi::D3D12, const NVSDK_NGX_Handle* evaluating = nullptr,
                                                   std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<const NVSDK_NGX_Handle*, FeatureRecord> features_;
  std::unordered_map<const void*, uint64_t> upscaler_creates_;  // devices an upscaler was ever created on, with how many
  std::unordered_set<const NVSDK_NGX_Handle*> not_adopted_;     // unknown handles already probed; reset when an identity can be reused
  uint64_t next_serial_ = 1u;
  std::optional<std::chrono::steady_clock::time_point> last_nr_evaluate_;  // the last NEURAL_RENDERING evaluate; nullopt: none yet
};

}  // namespace uplift::ngx_hooks
