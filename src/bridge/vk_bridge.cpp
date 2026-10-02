#include "bridge/vk_bridge.hpp"

#include <format>
#include <utility>

#include "color/encoding.hpp"
#include "nr/log.hpp"
#include "vk/format.hpp"

namespace uplift::bridge {
namespace {

using Microsoft::WRL::ComPtr;

// Key decision h: every CPU wait on this path is capped, and a timeout skips that frame; a single one never latches (Plan 9 M-2), and
// MAX_CONSECUTIVE_TIMEOUTS in a row do.
constexpr uint32_t WAIT_CAP_MS = 2000u;
// Batch 2 review, minor 4: this many 2 s timeouts of one kind of wait in a row stop the bridge for the session (6 s of stalls), instead of 2 s per frame.
constexpr uint32_t MAX_CONSECUTIVE_TIMEOUTS = 3u;

}  // namespace

std::unique_ptr<VkBridge> VkBridge::Create(vk::Device device, LUID luid, ComPtr<ID3D12Device>* created, std::string* error) {
  std::unique_ptr<VkBridge> bridge(new VkBridge(std::move(device)));
  bridge->side_ = D3D12Side::Create(luid, created, error);
  if (bridge->side_ == nullptr) return nullptr;
  const vk::DeviceRecord& record = bridge->interop_.Functions().record;
  if (record.semaphore_win32 && record.timeline && record.memory_win32) {
    // GPU-ordered: the two shared D3D12 fences, imported as timeline semaphores. A refused import builds CPU-ordered and says why (Plan 9 M-4's
    // lesson: the fallback is decided here, before any frame).
    ComPtr<ID3D12Fence>* const fences[] = {&bridge->to12_, &bridge->to11_};
    VkSemaphore* const semaphores[] = {&bridge->to12_vk_, &bridge->to11_vk_};
    for (size_t index = 0u; index < 2u && bridge->cpu_reason_.empty(); ++index) {
      HANDLE handle = nullptr;
      if (const HRESULT result = bridge->side_->CreateSharedFence(fences[index], &handle); FAILED(result)) {
        bridge->cpu_reason_ = std::format("the shared fence could not be created (HRESULT {:#010x})", static_cast<uint32_t>(result));
        break;
      }
      const char* step = "";
      const VkResult imported = bridge->interop_.ImportTimeline(handle, semaphores[index], &step);
      CloseHandle(handle);  // the imported semaphore keeps the fence alive
      if (imported != VK_SUCCESS) {
        bridge->cpu_reason_ = std::format("the driver refused the fence import ({} returned VkResult {})", step, static_cast<int>(imported));
      }
    }
    if (bridge->cpu_reason_.empty()) {
      bridge->gpu_ordered_ = true;
    } else {
      bridge->interop_.DestroySemaphore(&bridge->to12_vk_);
      bridge->interop_.DestroySemaphore(&bridge->to11_vk_);
      bridge->to12_.Reset();
      bridge->to11_.Reset();
    }
  } else {
    bridge->cpu_reason_ = (record.not_adjusted.empty() ? std::string("the device lacks VK_KHR_external_memory_win32") : record.not_adjusted);
  }
  if (bridge->gpu_ordered_) {
    nr::Log(nr::LogLevel::INFO, "Vulkan: NR runs on a private Direct3D 12 device");
  } else {
    nr::Logf(nr::LogLevel::INFO, "Vulkan: NR runs on a private Direct3D 12 device (CPU-ordered: {})", bridge->cpu_reason_);
  }
  return bridge;
}

VkBridge::~VkBridge() {
  // Nothing the private queue may still read is released before it finishes: at most 2 s, as DeviceContext::Teardown. The imported
  // semaphores go here when FreeVulkan did not (a bridge that never ran); they are functions the layer does not intercept.
  if (side_ != nullptr) {
    side_->Drain();
  }
  interop_.DestroySemaphore(&to12_vk_);
  interop_.DestroySemaphore(&to11_vk_);
  if (progress_event_ != nullptr) {
    CloseHandle(progress_event_);
  }
}

bool VkBridge::CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, uint32_t bytes_per_pixel, const wchar_t* name,
                            Shared* shared, std::string* failure) {
  Shared created = {.size = size, .format = format, .bytes = size.Pixels() * bytes_per_pixel};
  HANDLE handle = nullptr;
  if (const HRESULT result = side_->CreateShared(size, format, flags, name, &created.d3d12, &handle); FAILED(result)) {
    *failure = std::format("Direct3D 12 returned HRESULT {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  vk::ImportResult imported = interop_.ImportImage(handle, format, size);
  CloseHandle(handle);  // the imported memory keeps the allocation
  if (imported.result != VK_SUCCESS) {
    *failure = std::format("{} returned VkResult {}", imported.step, static_cast<int>(imported.result));
    return false;
  }
  created.vulkan = imported.image;
  *shared = std::move(created);
  return true;
}

void VkBridge::Retire(vk::GameHost& host, Shared* shared) {
  // D3D12: held until the side's progress passes the last recording that used it. Vulkan: behind a fence submitted now, freed at a later frame.
  if (shared->d3d12) {
    side_->Retire(std::move(shared->d3d12), nullptr, shared->last_use);
  }
  interop_.Retire(&shared->vulkan, host);
  *shared = {};
}

void VkBridge::LatchVulkan(std::string_view failure) {
  if (!latch_.empty()) return;
  std::string reason = std::format("The Vulkan bridge stopped: {}. Restart the game to use NR again",
                                   (interop_.DeviceLost() ? std::string_view("the game's Vulkan device was lost") : failure));
  nr::Log(nr::LogLevel::ERR, reason);
  // Batch 2 review, minor 1: after a failed Vulkan wait on a lost device the private queue would otherwise wait on `to12` until destroy_device.
  Stop(std::move(reason));
}

void VkBridge::NoteTimeout(uint32_t* consecutive, std::string_view what) {
  if (++*consecutive >= MAX_CONSECUTIVE_TIMEOUTS) {
    LatchVulkan(std::format("{} did not finish within 2 s, {} times in a row", what, MAX_CONSECUTIVE_TIMEOUTS));
  }
}

BridgeFrame VkBridge::BeginFrame(vk::GameHost& host, const VkImageInfo& back_buffer, bool enabled, bool running,
                                 std::chrono::steady_clock::time_point now) {
  present_synced_ = false;  // a new frame: its present-queue wait (design §3.4 case 2) is still to do
  side_->FreeFinished();
  interop_.FreeFinished(host);
  if (latch_.empty() && gpu_ordered_) {
    // Both directions count as progress, so a frame shows two steps: the game's work up to the copy-in, then NR's.
    const uint64_t completed = to11_->GetCompletedValue();
    if (const std::optional<std::string_view> stopped = watchdog_.Check(completed, completed + to12_->GetCompletedValue(), now)) {
      std::string reason = std::format("The Vulkan bridge stopped: {}. Restart the game to use NR again", *stopped);
      nr::Log(nr::LogLevel::ERR, reason);
      Stop(std::move(reason));
    }
  }
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
  }
  if (!running || !latch_.empty()) {
    // NR is off, or the bridge stopped (Plan 7 C-1, above the latch return): the shared surfaces go with NR's own (the stutter fix). Retire keeps
    // each one until the queues have passed its last use, so nothing still in flight is freed, and it never waits.
    Retire(host, &color_);
    Retire(host, &mask_);
    Retire(host, &motion_);
    mask_fresh_ = false;
    ran_logged_ = false;  // the next enable logs that NR ran on the game's frame again
  }
  if (!latch_.empty()) return {.problem = latch_};
  if (back_buffer.image == VK_NULL_HANDLE) return {};
  std::string problem = vk::BackBufferProblem(back_buffer, interop_.Functions().record.memory_win32);
  if (problem.empty()) {
    const std::optional<color::FormatInfo> format = color::DescribeFormat(back_buffer.format);  // BackBufferProblem found it usable
    const DXGI_FORMAT shared_format = vk::SharedFormatOf(back_buffer.format);  // ReShade reports a Vulkan back buffer typeless
    if (running && !(color_failure_ && color_failure_->Matches(back_buffer.size, back_buffer.format))
        && (!color_.d3d12 || color_.size != back_buffer.size || color_.format != shared_format)) {
      Retire(host, &color_);
      std::string failure;
      if (CreateShared(back_buffer.size, shared_format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, format->bytes_per_pixel,
                       L"Uplift shared back buffer", &color_, &failure)) {
        color_failure_.reset();
      } else {
        color_failure_ = ShareFailure{.size = back_buffer.size, .format = back_buffer.format, .what = std::move(failure)};
      }
    }
    // Reported while NR is off too, so the Session does not load for a back buffer the bridge cannot share.
    if (color_failure_ && color_failure_->Matches(back_buffer.size, back_buffer.format)) {
      problem = std::format("The back buffer could not be shared with the private Direct3D 12 device (DXGI_FORMAT {}, {})",
                            static_cast<int>(back_buffer.format), color_failure_->what);
    }
  }
  if (problem != problem_) {
    if (!problem.empty()) {
      nr::Log(nr::LogLevel::WARN, problem);
    }
    problem_ = std::move(problem);
  }
  if (!problem_.empty()) return {.size = back_buffer.size, .format = back_buffer.format, .problem = problem_};
  if (running && host.Valid()) {
    // Batch 2 review I-1 (a): the present event comes before ReShade records its effects, so this barrier leads the immediate list. It orders
    // the game's whole frame on this queue before the effects (Uplift's Signal flushes them without the game's present semaphores) and before
    // the copy in, at every trigger point.
    vk::RecordOpeningBarrier(host);
  }
  return {.color = color_.d3d12.Get(), .size = back_buffer.size, .format = back_buffer.format};
}

bool VkBridge::RecordAndExecute(const Recorder& record, ID3D12Resource* motion12, bool motion_is_dlss, bool* wrote, bool* used_mask) {
  ID3D12GraphicsCommandList* const list = side_->BeginList();
  if (list == nullptr) return false;
  const BridgeTargets targets = {
      .color = color_.d3d12.Get(),
      .mask = (mask_fresh_ ? mask_.d3d12.Get() : nullptr),
      .motion = motion12,
      .motion_is_dlss = motion_is_dlss,
  };
  if (targets.mask != nullptr) {
    Transition(list, targets.mask, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
  }
  if (targets.motion != nullptr) {
    Transition(list, targets.motion, D3D12_RESOURCE_STATE_COMMON, SHADER_READ);
  }
  bool recorded = true;
  try {
    *wrote = record(list, targets);
  } catch (...) {
    // Half a recording never executes; the queue still signals, so the game never waits in vain.
    nr::Log(nr::LogLevel::ERR, "NR's recording on the private Direct3D 12 device threw; this frame goes without NR");
    recorded = false;
    *wrote = false;
  }
  if (targets.motion != nullptr) {
    Transition(list, targets.motion, SHADER_READ, D3D12_RESOURCE_STATE_COMMON);
  }
  if (targets.mask != nullptr) {
    Transition(list, targets.mask, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  }
  if (!recorded) {
    side_->CloseList();
    return false;
  }
  if (!side_->ExecuteList()) return false;
  *used_mask = (targets.mask != nullptr);
  return true;
}

void VkBridge::StampUsed(uint64_t value, bool used_mask, bool used_motion) {
  color_.last_use = value;
  if (used_mask) {
    mask_.last_use = value;
    mask_fresh_ = false;
  }
  if (used_motion) {
    motion_.last_use = value;
  }
}

bool VkBridge::Run(vk::GameHost& host, const VkImageInfo& back_buffer, vk::Usage entry_state, const VkImageInfo& motion, const Recorder& record,
                   vk::GameHost* present_host, bool motion_is_dlss) {
  if (!latch_.empty() || !color_.d3d12 || back_buffer.image == VK_NULL_HANDLE || !host.Valid()) return false;
  // Batch 2 re-review, minors 3 and 4. A full ring, or, CPU-ordered, an earlier recording still in flight (its 2 s wait ran out): this frame goes
  // without NR rather than waiting, and it is counted. While a recording is in flight nothing is copied in: the copy in would write the shared colour
  // under an NR that still reads and writes it, and the stale completion of that recording would end this frame's wait before its own NR finished.
  if (!side_->SlotFree() || (!gpu_ordered_ && side_->Completed() < side_->LastSignalled())) {
    ++busy_skips_;
    return false;
  }
  // Design §3.4 case 2 (final review, minor 4): only for a frame that will go on, and before anything below can submit (a motion share's retire fence
  // flushes the immediate list, which at the technique holds the effects before the marker: they must not reach the queue ahead of the game's frame).
  if (present_host != nullptr && !present_synced_) {
    switch (interop_.FlushAndWait(vk::WaitSlot::PRESENT_QUEUE, *present_host, WAIT_CAP_MS)) {
      case vk::WaitResult::DONE:
        present_queue_timeouts_ = 0u;
        present_synced_ = true;
        break;
      case vk::WaitResult::TIMEOUT:
        NoteTimeout(&present_queue_timeouts_, "the game's present queue");  // this frame goes without NR
        return false;
      case vk::WaitResult::BUSY: return false;
      case vk::WaitResult::FAILED:
        LatchVulkan("a Vulkan submission on the game's present queue failed");
        return false;
    }
  }
  // LaunchPad's UPLIFT_MV, or (Plan 14) DLSS's copied vectors, shared like the mask (Plan 7 §9 g's flags). Retired when a recording comes without one.
  ID3D12Resource* motion12 = nullptr;
  if (motion.image != VK_NULL_HANDLE && motion.format == DXGI_FORMAT_R16G16_FLOAT && motion.samples == 1u) {
    if (!motion_.d3d12 || motion_.size != motion.size) {
      Retire(host, &motion_);
      if (!(motion_failure_ && motion_failure_->Matches(motion.size, DXGI_FORMAT_R16G16_FLOAT))) {
        std::string failure;
        if (CreateShared(motion.size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                         4u, L"Uplift shared motion", &motion_, &failure)) {
          motion_failure_.reset();
        } else {
          // Logged once: not retried for this size (Plan 7 Minor 3).
          motion_failure_ = ShareFailure{.size = motion.size, .format = DXGI_FORMAT_R16G16_FLOAT, .what = failure};
          nr::Logf(nr::LogLevel::WARN, "{} could not be shared with the private Direct3D 12 device ({}); NR runs without {} motion",
                   (motion_is_dlss ? "DLSS's motion vectors" : "UPLIFT_MV"), failure, (motion_is_dlss ? "DLSS's" : "LaunchPad's"));
        }
      }
    }
    motion12 = motion_.d3d12.Get();  // null when the share failed: NR runs without motion
  } else if (motion_.d3d12) {
    Retire(host, &motion_);
  }
  const VkImageInfo motion_source = (motion12 != nullptr ? motion : VkImageInfo{});
  const bool wrote = (gpu_ordered_ ? RunGpuOrdered(host, back_buffer, entry_state, motion_source, motion12, motion_is_dlss, record)
                                   : RunCpuOrdered(host, back_buffer, entry_state, motion_source, motion12, motion_is_dlss, record));
  if (wrote && !ran_logged_) {
    // Once per enable (BeginFrame clears it when NR goes off): the e2e cases and the checklist read it as "a frame came back through the bridge".
    ran_logged_ = true;
    nr::Log(nr::LogLevel::INFO, "NR ran on the game's frame (Vulkan, 64-bit)");
  }
  return wrote;
}

bool VkBridge::RunGpuOrdered(vk::GameHost& host, const VkImageInfo& back_buffer, vk::Usage entry_state, const VkImageInfo& motion,
                             ID3D12Resource* motion12, bool motion_is_dlss, const Recorder& record) {
  // The steps on this bridge's objects; RunBridgedFrame owns their order (R17: the Vulkan wait is queued only for a signal that was submitted).
  class Steps final : public BridgeSteps {
   public:
    Steps(VkBridge& bridge, vk::GameHost& host, const VkImageInfo& back_buffer, vk::Usage entry_state, const VkImageInfo& motion,
          ID3D12Resource* motion12, bool motion_is_dlss, const Recorder& record)
        : bridge_(bridge),
          host_(host),
          back_buffer_(back_buffer),
          entry_state_(entry_state),
          motion_(motion),
          motion12_(motion12),
          motion_is_dlss_(motion_is_dlss),
          record_(record) {}
    bool used_mask = false;
    bool executed = false;  // the list was submitted: its surfaces are in use until the side's progress passes it
    std::string vulkan_failure;

    void CopyIn() override {
      vk::RecordCopyIn(host_, back_buffer_.image, entry_state_, bridge_.color_.vulkan.image, motion_.image, bridge_.motion_.vulkan.image);
    }
    bool SignalD3D11(uint64_t value) override {
      if (host_.Signal(bridge_.to12_vk_, value)) return true;
      bridge_.interop_.Counter(bridge_.to12_vk_);  // notes a lost device
      vulkan_failure = "the Vulkan timeline semaphore signal failed";
      return false;
    }
    bool WaitD3D12(uint64_t value) override {
      bridge_.waited12_ = value;  // for Stop, whether or not the queue took it
      return SUCCEEDED(bridge_.side_->Queue()->Wait(bridge_.to12_.Get(), value));
    }
    bool RecordAndExecute(bool* wrote) override {
      executed = bridge_.RecordAndExecute(record_, motion12_, motion_is_dlss_, wrote, &used_mask);
      return executed;
    }
    // progress first: when the queue refuses it, to11_ is never signalled, so Vulkan never waits for this frame.
    bool SignalD3D12(uint64_t value) override {
      return bridge_.side_->SignalProgress(value) && SUCCEEDED(bridge_.side_->Queue()->Signal(bridge_.to11_.Get(), value));
    }
    bool WaitD3D11(uint64_t value) override {
      bridge_.waited11_ = value;  // for Stop, whether or not the queue took it
      if (host_.Wait(bridge_.to11_vk_, value)) return true;
      bridge_.interop_.Counter(bridge_.to11_vk_);
      vulkan_failure = "the Vulkan timeline semaphore wait failed";
      return false;
    }
    void CopyOut() override { vk::RecordCopyOut(host_, back_buffer_.image, entry_state_, bridge_.color_.vulkan.image); }

   private:
    VkBridge& bridge_;
    vk::GameHost& host_;
    const VkImageInfo& back_buffer_;
    vk::Usage entry_state_;
    const VkImageInfo& motion_;
    ID3D12Resource* motion12_;
    bool motion_is_dlss_;
    const Recorder& record_;
  };
  Steps steps(*this, host, back_buffer, entry_state, motion, motion12, motion_is_dlss, record);
  const uint64_t in = ++last_value_;  // monotonic even when a step fails
  const uint64_t out = ++last_value_;
  const BridgedFrame frame = RunBridgedFrame(steps, in, out);
  // Plan 7 C-1: stamped whenever the list was submitted, even when the D3D12 signal then failed: its surfaces are then held until the side's
  // progress passes `out` (for good, if that signal never comes), never freed under the list.
  if (steps.executed || frame.d3d12_signalled) {
    StampUsed(out, steps.used_mask, motion12 != nullptr);
  }
  if (frame.d3d12_signalled) {
    watchdog_.Submitted(out, std::chrono::steady_clock::now());  // Plan 7 I-2: from submission, after recording
  }
  if (!frame.failure.empty()) {
    LatchVulkan(steps.vulkan_failure.empty() ? frame.failure : std::string_view(steps.vulkan_failure));
  }
  return frame.wrote;
}

bool VkBridge::RunCpuOrdered(vk::GameHost& host, const VkImageInfo& back_buffer, vk::Usage entry_state, const VkImageInfo& motion,
                             ID3D12Resource* motion12, bool motion_is_dlss, const Recorder& record) {
  // Design §3.3, in this order: the copy in, the copy's completion on the CPU, NR, NR's completion on the CPU, the copy out. Nothing waits on the
  // GPU, so there is no fence to strand and no watchdog; a timeout skips this frame, and three in a row stop the bridge (minor 4). Run has skipped
  // the frames on which an earlier recording is still in flight (batch 2 review I-2).
  vk::RecordCopyIn(host, back_buffer.image, entry_state, color_.vulkan.image, motion.image, motion_.vulkan.image);
  switch (interop_.FlushAndWait(vk::WaitSlot::CPU_ORDER, host, WAIT_CAP_MS)) {
    case vk::WaitResult::DONE: break;
    case vk::WaitResult::TIMEOUT:
      NoteTimeout(&cpu_timeouts_, "the game's queue");
      return false;
    case vk::WaitResult::BUSY: return false;
    case vk::WaitResult::FAILED:
      LatchVulkan("a Vulkan submission or wait failed");
      return false;
  }
  const uint64_t value = ++last_value_;
  bool wrote = false;
  bool used_mask = false;
  const bool executed = RecordAndExecute(record, motion12, motion_is_dlss, &wrote, &used_mask);
  const bool signalled = side_->SignalProgress(value);
  if (executed || signalled) {
    StampUsed(value, used_mask, motion12 != nullptr);
  }
  if (!signalled) {
    LatchVulkan("the private Direct3D 12 queue refused to signal");
    return false;
  }
  if (progress_event_ == nullptr) {
    progress_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  // An earlier frame's wait that timed out left its completion registered on this (auto-reset) event: cleared now, which is safe because the
  // check above guarantees that nothing was in flight, so no completion of an older value can still arrive.
  if (progress_event_ == nullptr || ResetEvent(progress_event_) == FALSE
      || FAILED(side_->Progress()->SetEventOnCompletion(value, progress_event_))) {
    LatchVulkan("the private Direct3D 12 queue's completion could not be waited for");
    return false;
  }
  // Batch 2 re-review, minor 3: the event is set later (from the interrupt path) than the fence's value reaches the GPU, so a stale completion of an
  // older recording could still end this wait early; the fence's own value, which only `value` reaching it can satisfy, decides.
  if (WaitForSingleObject(progress_event_, WAIT_CAP_MS) != WAIT_OBJECT_0 || side_->Completed() < value) {
    NoteTimeout(&cpu_timeouts_, "NR on the private Direct3D 12 queue");  // the surfaces stay stamped; Run skips frames until it ends
    return false;
  }
  cpu_timeouts_ = 0u;
  if (!(executed && wrote)) return false;
  vk::RecordCopyOut(host, back_buffer.image, entry_state, color_.vulkan.image);
  return true;
}

MaskCopy VkBridge::CopyMask(vk::GameHost& host, const VkImageInfo& mask) {
  if (!latch_.empty()) return {};
  const std::optional<color::MaskFormatInfo> format = color::DescribeMaskFormat(mask.format);
  if (!format || mask.samples != 1u || vk::VkFormatOf(format->view_format) == VK_FORMAT_UNDEFINED) {
    ReleaseMask(host);
    return {.unsupported = true};
  }
  if (mask.image == VK_NULL_HANDLE || !host.Valid()) return {};
  // Shared in the typed view format; the source keeps its own (a sRGB RGBA8 mask copies as raw texels).
  if (!mask_.d3d12 || mask_.size != mask.size || mask_.format != format->view_format) {
    Retire(host, &mask_);
    mask_fresh_ = false;
    if (mask_failure_ && mask_failure_->Matches(mask.size, format->view_format)) return {};
    std::string failure;
    if (!CreateShared(mask.size, format->view_format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                      format->bytes_per_pixel, L"Uplift shared mask", &mask_, &failure)) {
      // Logged once: not retried for this size and format (Plan 7 Minor 3).
      mask_failure_ = ShareFailure{.size = mask.size, .format = format->view_format, .what = failure};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with the private Direct3D 12 device (DXGI_FORMAT {}, {})",
               static_cast<int>(format->view_format), failure);
      return {};
    }
    mask_failure_.reset();
  }
  vk::RecordMaskCopy(host, mask.image, mask_.vulkan.image);
  mask_fresh_ = true;
  return {.size = mask.size};
}

void VkBridge::ReleaseMask(vk::GameHost& host) {
  Retire(host, &mask_);
  mask_fresh_ = false;
}

void VkBridge::Stop(std::string reason) {
  if (latch_.empty()) {
    latch_ = std::move(reason);
  }
  // Each fence up to the highest value a wait on it was queued for; nothing waits beyond those. A refused signal (a removed private device) needs
  // no retry: that fence already reads UINT64_MAX, above every wait. CPU-ordered has no fence, so nothing to release.
  // Batch 2 review, minor 6: `to12` is the fence the game's Vulkan queue signals (imported as a timeline semaphore), so a Vulkan signal of
  // `waited12_` that is still pending then finds the value reached already, while Vulkan wants a timeline signal to exceed the current value
  // (VUID-VkSubmitInfo-pSignalSemaphores-03242). With a D3D12 fence underneath that is benign on NVIDIA's driver, and it only happens here, after a
  // latch, a device loss or destroy_device; the alternative is a private queue that waits for a signal that never comes.
  if (to11_ && to11_->GetCompletedValue() < waited11_) {
    to11_->Signal(waited11_);
  }
  if (to12_ && to12_->GetCompletedValue() < waited12_) {
    to12_->Signal(waited12_);
  }
}

void VkBridge::FreeVulkan(vk::GameHost& host) {
  interop_.FreeNow(&color_.vulkan, host);
  interop_.FreeNow(&mask_.vulkan, host);
  interop_.FreeNow(&motion_.vulkan, host);
  interop_.FreeAll(host);
  interop_.DestroySemaphore(&to12_vk_);
  interop_.DestroySemaphore(&to11_vk_);
}

uint64_t VkBridge::SharedBytes() const {
  return color_.bytes + mask_.bytes + motion_.bytes;
}

std::string VkBridge::StatusLine() const {
  const double mib = static_cast<double>(SharedBytes()) / (1024.0 * 1024.0);
  std::string line = (gpu_ordered_ ? std::format("Vulkan: NR runs on a private Direct3D 12 device (shared textures and fences, {:.1f} MiB)", mib)
                                   : std::format("Vulkan: NR runs on a private Direct3D 12 device (shared textures, CPU-ordered, {:.1f} MiB): {}",
                                                 mib, cpu_reason_));
  if (busy_skips_ > 0u) {
    line += std::format(", {} frame(s) without NR while the bridge was busy", busy_skips_);
  }
  if (second_queue_) {
    line += ", presents from a second queue";
  }
  return line;
}

}  // namespace uplift::bridge
