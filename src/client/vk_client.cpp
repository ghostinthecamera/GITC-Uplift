#include "client/vk_client.hpp"

#include <chrono>
#include <format>
#include <limits>
#include <utility>

#include "color/encoding.hpp"
#include "ipc/control_block.hpp"
#include "nr/log.hpp"
#include "vk/format.hpp"

namespace uplift::client {
namespace {

constexpr uint32_t WAIT_CAP_MS = 2000u;  // the design's R29 cap on the game thread's waits
constexpr uint32_t MAX_CONSECUTIVE_TIMEOUTS = 3u;
constexpr double MIB = 1024.0 * 1024.0;

std::string ShareFailureText(const char* what, DXGI_FORMAT format, std::string_view failure) {
  return std::format("{} could not be shared with Uplift's helper (DXGI_FORMAT {}, {})", what, static_cast<int>(format), failure);
}

}  // namespace

VkClient::VkClient(vk::Device device, LUID luid, bool allow_fences) : interop_(std::move(device)), luid_(luid) {
  const vk::Device& functions = interop_.Functions();
  const vk::DeviceRecord& record = functions.record;
  // Design §2.5: GPU-ordered needs both Win32 extensions and timeline semaphores (adjusted, or the game's own, as DXVK 2.5.2's); without them the frame
  // crosses through shared textures, ordered on the CPU.
  fenced_ = (allow_fences && record.semaphore_win32 && record.timeline && record.memory_win32 && functions.vkImportSemaphoreWin32HandleKHR != nullptr
             && functions.vkGetSemaphoreCounterValue != nullptr);
  if (!fenced_) {
    cpu_reason_ = !allow_fences                  ? "fences were turned off"
                  : !record.not_adjusted.empty() ? record.not_adjusted
                                                 : "the game's device lacks VK_KHR_external_semaphore_win32 or timeline semaphores";
  }
}

void VkClient::Retire(vk::GameHost& host, Shared* shared) {
  interop_.Retire(&shared->vulkan, host);
  *shared = {};
}

void VkClient::ReleaseShared(vk::GameHost& host) {
  Retire(host, &color_);
  Retire(host, &mask_);
  Retire(host, &motion_);
  mask_fresh_ = false;
  floor_probed_ = false;
}

void VkClient::ReleaseMask(vk::GameHost& host) {
  Retire(host, &mask_);
  mask_fresh_ = false;
}

bool VkClient::ReleaseAll(vk::GameHost& host) {
  const bool held = (color_.vulkan.image != VK_NULL_HANDLE || mask_.vulkan.image != VK_NULL_HANDLE || motion_.vulkan.image != VK_NULL_HANDLE
                     || to12_vk_ != VK_NULL_HANDLE || to11_vk_ != VK_NULL_HANDLE);
  ReleaseShared(host);
  // Work this side queued may still wait on a fence the helper's exit has just released (the counter reads UINT64_MAX, so it passes): the semaphores go
  // behind a fence too, never under a submission.
  interop_.RetireSemaphore(&to12_vk_, host);
  interop_.RetireSemaphore(&to11_vk_, host);
  watchdog_ = {};
  last_value_ = 0u;
  waited11_ = 0u;
  pending_out_ = 0u;
  color_failure_.reset();
  mask_failure_.reset();
  motion_failure_.reset();
  in_flight_ = false;
  return held;
}

void VkClient::FreeVulkan(vk::GameHost& host) {
  interop_.FreeNow(&color_.vulkan, host);
  interop_.FreeNow(&mask_.vulkan, host);
  interop_.FreeNow(&motion_.vulkan, host);
  color_ = {};
  mask_ = {};
  motion_ = {};
  interop_.FreeAll(host);
  interop_.DestroySemaphore(&to12_vk_);
  interop_.DestroySemaphore(&to11_vk_);
}

void VkClient::Stop(std::string_view reason, NrLink& link, vk::GameHost& host) {
  if (latch_.empty()) {
    latch_ = std::format("The Vulkan bridge stopped: {}. Restart the game to use NR again",
                         (interop_.DeviceLost() ? std::string_view("the game's Vulkan device was lost") : reason));
    nr::Log(nr::LogLevel::ERR, latch_);
  }
  link.Stop(waited11_, latch_);
  ReleaseShared(host);
}

void VkClient::NoteTimeout(uint32_t* consecutive, std::string_view what, NrLink& link, vk::GameHost& host) {
  if (++*consecutive >= MAX_CONSECUTIVE_TIMEOUTS) {
    Stop(std::format("{} did not finish within 2 s, {} times in a row", what, MAX_CONSECUTIVE_TIMEOUTS), link, host);
  }
}

bool VkClient::Import(vk::GameHost& host, HANDLE handle, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, Shared* shared,
                      std::string* failure) {
  vk::ImportResult imported = interop_.ImportImage(handle, format, size);
  if (imported.result != VK_SUCCESS) {
    *failure = std::format("{} returned VkResult {}", imported.step, static_cast<int>(imported.result));
    return false;
  }
  Retire(host, shared);  // a newer texture of this kind replaces the older: it goes behind a fence
  *shared = {.vulkan = imported.image, .size = size, .format = format, .bytes = size.Pixels() * bytes_per_pixel};
  return true;
}

ipc::Target VkClient::Describe(vk::GameHost& host, const vk::ImageInfo& back_buffer, bool enabled, bool running) {
  interop_.FreeFinished(host);
  present_synced_ = false;  // a new frame: its present-queue wait (design §3.4 case 2) is still to do
  ipc::Target target;
  target.width = back_buffer.size.width;
  target.height = back_buffer.size.height;
  target.dxgi_format = static_cast<uint32_t>(vk::SharedFormatOf(back_buffer.format));  // the helper makes the shared colour in it
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
  }
  std::string problem;
  if (!latch_.empty()) {
    problem = latch_;
  } else if (back_buffer.image == VK_NULL_HANDLE) {
    problem = "The back buffer is not a Vulkan image";
  } else {
    problem = vk::BackBufferProblem(back_buffer, interop_.Functions().record.memory_win32);
  }
  if (!running || !problem.empty()) {
    ReleaseShared(host);  // NR is off, or this image cannot go: the shared surfaces go with it (Plan 7's final review C-1)
  }
  if (problem.empty() && color_failure_ && color_failure_->Matches(back_buffer.size, back_buffer.format)) {
    problem = ShareFailureText("The back buffer", back_buffer.format, color_failure_->what);
  }
  if (problem.empty() && running && host.Valid()) {
    // Batch 2 review I-1 (a), as VkBridge::BeginFrame: this event comes before ReShade records its effects, so the barrier leads the immediate list.
    vk::RecordOpeningBarrier(host);
  }
  if (problem != problem_) {
    if (!problem.empty()) {
      nr::Log(nr::LogLevel::WARN, problem);
    }
    problem_ = std::move(problem);
  }
  ipc::CopyText(target.problem, problem_);
  frame_ask_ = {.size = back_buffer.size, .format = back_buffer.format};
  frame_asks_colour_ = problem_.empty();
  return target;
}

void VkClient::NoteFrameSent() {
  if (frame_asks_colour_) {
    color_ask_ = frame_ask_;
  }
}

bool VkClient::Apply(const ipc::Reply& reply, NrLink& link, vk::GameHost& host) {
  in_flight_ = false;  // a FRAME reply means every earlier request has replied
  bool fell_back = false;
  const ipc::Handles& handles = reply.handles;
  // Every NT handle in the reply is taken and closed below, whatever this client does with it, so the helper's table never keeps one.
  const auto pull = [&link](uint64_t remote) {
    HANDLE local = nullptr;
    return ((remote != 0u && link.Pull(remote, &local)) ? local : HANDLE(nullptr));
  };
  const HANDLE to12 = pull(handles.to12);
  const HANDLE to11 = pull(handles.to11);
  const HANDLE color = pull(handles.color);
  const HANDLE mask = pull(handles.mask);
  const HANDLE motion = pull(handles.motion);
  if (fenced_ && to12 != nullptr && to11 != nullptr) {
    // A new transport, so new fences: every value restarts (the helper's `progress` stays monotonic on its own).
    ReleaseShared(host);
    interop_.RetireSemaphore(&to12_vk_, host);
    interop_.RetireSemaphore(&to11_vk_, host);
    watchdog_ = {};
    last_value_ = 0u;
    waited11_ = 0u;
    pending_out_ = 0u;
    const char* step = "";
    VkResult imported = interop_.ImportTimeline(to12, &to12_vk_, &step);
    if (imported == VK_SUCCESS) {
      imported = interop_.ImportTimeline(to11, &to11_vk_, &step);
    }
    if (imported != VK_SUCCESS) {
      // An unusual driver or wrapper refuses the import: no restart would help, so the frame crosses through shared textures, ordered on the CPU, which
      // need no fences. The caller re-attaches the helper with SHARED_CPU (Plan 9 M-4's lesson).
      interop_.DestroySemaphore(&to12_vk_);
      interop_.DestroySemaphore(&to11_vk_);
      cpu_reason_ = std::format("the driver refused the fence import ({} returned VkResult {})", step, static_cast<int>(imported));
      nr::Logf(nr::LogLevel::WARN,
               "The Vulkan device could not import the helper's shared fences ({} returned VkResult {}); frames cross through CPU-ordered shared textures instead",
               step, static_cast<int>(imported));
      fenced_ = false;
      fell_back = true;
    }
  }
  if (!fell_back) {
    std::string failure;
    if (color != nullptr && latch_.empty()) {
      // The colour of the FRAME that was sent (color_ask_), which this reply may only be delivering from an earlier one (batch 3 review, minor 1).
      if (Import(host, color, color_ask_.size, vk::SharedFormatOf(color_ask_.format),
                 color::DescribeFormat(color_ask_.format).value_or(color::FormatInfo{}).bytes_per_pixel, &color_, &failure)) {
        color_failure_.reset();
      } else {
        color_failure_ = ShareFailure{.size = color_ask_.size, .format = color_ask_.format, .what = failure};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("The back buffer", color_ask_.format, failure));
      }
    }
    if (mask != nullptr && latch_.empty()) {
      // The texture is the one the last SHARE_MASK asked for (this reply may have landed late), not the back buffer's size and format.
      const uint32_t bytes = color::DescribeMaskFormat(mask_ask_.format).value_or(color::MaskFormatInfo{.bytes_per_pixel = 4u}).bytes_per_pixel;
      if (Import(host, mask, mask_ask_.size, mask_ask_.format, bytes, &mask_, &failure)) {
        mask_failure_.reset();
      } else {
        mask_failure_ = ShareFailure{.size = mask_ask_.size, .format = mask_ask_.format, .what = failure};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("UPLIFT_MASK", mask_ask_.format, failure));
      }
    }
    if (motion != nullptr && latch_.empty()) {
      if (!Import(host, motion, motion_ask_size_, DXGI_FORMAT_R16G16_FLOAT, 4u, &motion_, &failure)) {
        motion_failure_ = ShareFailure{.size = motion_ask_size_, .format = DXGI_FORMAT_R16G16_FLOAT, .what = failure};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("UPLIFT_MV", DXGI_FORMAT_R16G16_FLOAT, failure));
      }
    }
  }
  for (const HANDLE handle : {to12, to11, color, mask, motion}) {
    if (handle != nullptr) {
      CloseHandle(handle);
    }
  }
  if (fenced_ && latch_.empty() && to11_vk_ != VK_NULL_HANDLE && to12_vk_ != VK_NULL_HANDLE) {
    // Both directions count as progress, so a frame shows two steps: the game's work up to the copy-in, then NR's. A counter at UINT64_MAX with the helper
    // still answering is its device removed; a dead helper never gets here (the launcher sees it).
    const uint64_t completed = interop_.Counter(to11_vk_);
    if (interop_.DeviceLost()) {
      Stop("the game's Vulkan device was lost", link, host);
    } else if (const std::optional<std::string_view> stopped =
                   watchdog_.Check(completed, completed + interop_.Counter(to12_vk_), std::chrono::steady_clock::now())) {
      Stop(*stopped, link, host);
    }
  }
  if (reply.running == 0u) {
    ReleaseShared(host);
  }
  return fell_back;
}

bool VkClient::SyncPresentQueue(vk::GameHost* present_host, vk::GameHost& host, NrLink& link) {
  if (present_host == nullptr || present_synced_) return true;
  switch (interop_.FlushAndWait(vk::WaitSlot::PRESENT_QUEUE, *present_host, WAIT_CAP_MS)) {
    case vk::WaitResult::DONE:
      present_queue_timeouts_ = 0u;
      present_synced_ = true;
      return true;
    case vk::WaitResult::TIMEOUT:
      NoteTimeout(&present_queue_timeouts_, "the game's present queue", link, host);  // this frame goes without NR
      return false;
    case vk::WaitResult::BUSY:   return false;
    case vk::WaitResult::FAILED: break;
  }
  Stop("a Vulkan submission on the game's present queue failed", link, host);
  return false;
}

vk::ImageInfo VkClient::PrepareMotion(vk::GameHost& host, const vk::ImageInfo& motion, NrLink& link) {
  // Plan 7 decision 3: LaunchPad's UPLIFT_MV, shared like the mask (RG16F). Retired when a recording comes without one.
  if (motion.image == VK_NULL_HANDLE || motion.format != DXGI_FORMAT_R16G16_FLOAT || motion.samples != 1u) {
    // 1.1.2 (a player's freeze, NFS Underground 2): a frame without LaunchPad's motion (the Uplift technique skipped a frame) keeps the share; it goes
    // with NR (Release), at a new size, or when the helper says so. Releasing and re-sharing it around one such frame coincided with a GPU fault.
    return {};
  }
  if (motion_.vulkan.image == VK_NULL_HANDLE || motion_.size != motion.size) {
    Retire(host, &motion_);
    if (!(motion_failure_ && motion_failure_->Matches(motion.size, DXGI_FORMAT_R16G16_FLOAT))) {
      const ipc::Share share = {.width = motion.size.width, .height = motion.size.height, .dxgi_format = static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT)};
      motion_ask_size_ = motion.size;
      const std::optional<ipc::Reply> shared = link.Share(ipc::RequestKind::SHARE_MOTION, share);
      if (!shared) {
        in_flight_ = true;  // late: its handle arrives with a later reply; nothing has been touched yet
        return {};
      }
      HANDLE handle = nullptr;
      std::string failure = "the helper refused";
      const bool opened = (shared->ok != 0u && shared->handles.motion != 0u && link.Pull(shared->handles.motion, &handle)
                           && Import(host, handle, motion.size, DXGI_FORMAT_R16G16_FLOAT, 4u, &motion_, &failure));
      if (handle != nullptr) {
        CloseHandle(handle);
      }
      if (opened) {
        motion_failure_.reset();
      } else {
        // Logged once: not retried for this size (Plan 7's final review Minor 3).
        motion_failure_ = ShareFailure{.size = motion.size, .format = DXGI_FORMAT_R16G16_FLOAT, .what = failure};
        nr::Logf(nr::LogLevel::WARN, "UPLIFT_MV could not be shared with Uplift's helper ({}); NR runs without LaunchPad's motion", failure);
      }
    }
  }
  return (motion_.vulkan.image != VK_NULL_HANDLE && motion_.size == motion.size) ? motion : vk::ImageInfo{};
}

bool VkClient::Run(vk::GameHost& host, const vk::ImageInfo& back_buffer, vk::Usage entry_state, const vk::ImageInfo& motion, addon::TriggerPoint point,
                   NrLink& link, vk::GameHost* present_host) {
  run_sent_ = false;
  if (!latch_.empty() || in_flight_ || back_buffer.image == VK_NULL_HANDLE || back_buffer.samples != 1u || !host.Valid()) return false;
  if (color_.vulkan.image == VK_NULL_HANDLE || color_.size != back_buffer.size || color_.format != vk::SharedFormatOf(back_buffer.format)) return false;
  ipc::Run run = {.point = static_cast<uint32_t>(point)};
  if (!fenced_ && !nr::MeetsNrFloor(back_buffer.size)) {
    // SHARED_CPU: below NR's floor nothing can come back, and a RUN costs a CPU wait. After the first RUN at this size (whose reply's status says why)
    // no RUN at all.
    if (floor_probed_) return false;
    const bool replied = link.Run(run).has_value();
    run_sent_ = link.RunSent();
    floor_probed_ = run_sent_;  // a RUN that was refused (another request outstanding) is tried again
    if (!replied) {
      in_flight_ = true;
    }
    return false;
  }
  return fenced_ ? RunFenced(host, back_buffer, entry_state, motion, run, link, present_host)
                 : RunCpuOrdered(host, back_buffer, entry_state, motion, run, link, present_host);
}

bool VkClient::RunFenced(vk::GameHost& host, const vk::ImageInfo& back_buffer, vk::Usage entry_state, const vk::ImageInfo& motion, ipc::Run run,
                         NrLink& link, vk::GameHost* present_host) {
  if (to12_vk_ == VK_NULL_HANDLE || to11_vk_ == VK_NULL_HANDLE) return false;
  const uint64_t completed = interop_.Counter(to11_vk_);
  if (completed == std::numeric_limits<uint64_t>::max()) {
    // The helper is gone or its device was removed (or the Vulkan device was lost): no RUN goes to a dead peer and no copy in is made for it. The
    // launcher shows an exit at the next present (ReleaseAll), and Apply's watchdog latches the rest.
    return false;
  }
  if (pending_out_ != 0u) {
    // A RUN whose reply never came may still be on the helper's queue: nothing touches the shared textures until the counter shows its signal (a CPU
    // poll; no GPU wait is queued for a signal nobody confirmed), or its late reply says it never signalled.
    if (const std::optional<bool> signalled = link.TakeLateRunSignalled(); signalled && !*signalled) {
      pending_out_ = 0u;
    }
    if (pending_out_ != 0u && completed < pending_out_) {
      ++busy_skips_;
      return false;
    }
    pending_out_ = 0u;
  }
  // Final review, minor 4: after the skips above, and before anything below can submit (PrepareMotion's retire fence flushes the immediate list).
  if (!SyncPresentQueue(present_host, host, link)) return false;
  const vk::ImageInfo shared_motion_source = PrepareMotion(host, motion, link);
  if (in_flight_) return false;
  run.motion = (shared_motion_source.image != VK_NULL_HANDLE ? 1u : 0u);
  run.mask_fresh = ((mask_fresh_ && mask_.vulkan.image != VK_NULL_HANDLE) ? 1u : 0u);

  class Steps final : public bridge::BridgeSteps {
   public:
    Steps(VkClient& client, vk::GameHost& host, const vk::ImageInfo& back_buffer, vk::Usage entry_state, const vk::ImageInfo& motion, const ipc::Run& run,
          NrLink& link)
        : client_(client), host_(host), back_buffer_(back_buffer), entry_state_(entry_state), motion_(motion), run_(run), link_(link) {}
    bool sent = false;       // link.Run put its request on the wire: the helper has seen this point
    bool timed_out = false;  // and it did not answer in time (or is gone), or the link refused it
    ipc::Reply reply;        // what WaitD3D12 got, for the three steps that ask the helper's side
    std::string vulkan_failure;

    void CopyIn() override {
      vk::RecordCopyIn(host_, back_buffer_.image, entry_state_, client_.color_.vulkan.image, motion_.image, client_.motion_.vulkan.image);
    }
    bool SignalD3D11(uint64_t value) override {
      if (host_.Signal(client_.to12_vk_, value)) return true;
      client_.interop_.Counter(client_.to12_vk_);  // notes a lost device
      vulkan_failure = "the Vulkan timeline semaphore signal failed";
      return false;
    }
    // The IPC call: the helper's queue waits for `value`, records NR and signals `out`, and the reply says which of those it did.
    bool WaitD3D12(uint64_t value) override {
      run_.in = value;
      const std::optional<ipc::Reply> answered = link_.Run(run_);
      sent = link_.RunSent();
      if (!answered) {
        timed_out = true;
        return false;
      }
      reply = *answered;
      return reply.waited != 0u;
    }
    bool RecordAndExecute(bool* wrote) override {
      *wrote = (reply.ok != 0u && reply.wrote != 0u);  // ok = 0: the helper's catch path
      return reply.submitted != 0u;
    }
    bool SignalD3D12(uint64_t /*value*/) override { return reply.signalled != 0u; }
    // R17: only after the reply said the signal was submitted (RunBridgedFrame calls this after SignalD3D12 returned true).
    bool WaitD3D11(uint64_t value) override {
      client_.waited11_ = value;  // for Stop, whether or not the queue took it
      if (host_.Wait(client_.to11_vk_, value)) return true;
      client_.interop_.Counter(client_.to11_vk_);
      vulkan_failure = "the Vulkan timeline semaphore wait failed";
      return false;
    }
    void CopyOut() override { vk::RecordCopyOut(host_, back_buffer_.image, entry_state_, client_.color_.vulkan.image); }

   private:
    VkClient& client_;
    vk::GameHost& host_;
    const vk::ImageInfo& back_buffer_;
    vk::Usage entry_state_;
    const vk::ImageInfo& motion_;
    ipc::Run run_;
    NrLink& link_;
  };
  const uint64_t in = ++last_value_;  // monotonic even when a step fails
  const uint64_t out = ++last_value_;
  run.out = out;
  Steps steps(*this, host, back_buffer, entry_state, shared_motion_source, run, link);
  const bridge::BridgedFrame frame = bridge::RunBridgedFrame(steps, in, out);
  run_sent_ = steps.sent;
  if (steps.timed_out) {
    // No reply in 2 s (or the helper is gone, which the launcher shows next frame). Nothing was queued on this side's GPU after the copy in, and no Vulkan
    // wait ever will be for `out`; the helper's list may still run, so until to11 shows its signal (or a late reply says there is none) nothing touches the
    // shared textures. A RUN the link refused never reached the helper.
    if (steps.sent) {
      pending_out_ = out;
      in_flight_ = true;
      nr::Log(nr::LogLevel::WARN, "Uplift's 64-bit helper did not answer within 2 s; frames go without NR until it does");
    }
    return false;
  }
  if (steps.reply.busy != 0u) {
    ++busy_skips_;  // this frame goes without NR rather than waiting
  }
  if (steps.reply.submitted != 0u && run.mask_fresh != 0u) {
    mask_fresh_ = false;
  }
  if (frame.d3d12_signalled) {
    watchdog_.Submitted(out, std::chrono::steady_clock::now());  // from submission, after the helper's recording
  }
  if (!frame.failure.empty()) {
    Stop(steps.vulkan_failure.empty() ? frame.failure : std::string_view(steps.vulkan_failure), link, host);
  }
  return frame.wrote;
}

bool VkClient::RunCpuOrdered(vk::GameHost& host, const vk::ImageInfo& back_buffer, vk::Usage entry_state, const vk::ImageInfo& motion, ipc::Run run,
                             NrLink& link, vk::GameHost* present_host) {
  // Design §3.3, in this order: the copy in, its completion on the CPU (capped), RUN (whose reply comes when the helper's GPU work is done), the copy out.
  // Nothing waits on the GPU, so there is no fence to strand; a timeout skips this frame and three in a row stop the client.
  if (!SyncPresentQueue(present_host, host, link)) return false;  // final review, minor 4: before anything below can submit
  const vk::ImageInfo shared_motion_source = PrepareMotion(host, motion, link);
  if (in_flight_) return false;
  vk::RecordCopyIn(host, back_buffer.image, entry_state, color_.vulkan.image, shared_motion_source.image, motion_.vulkan.image);
  switch (interop_.FlushAndWait(vk::WaitSlot::CPU_ORDER, host, WAIT_CAP_MS)) {
    case vk::WaitResult::DONE: break;
    case vk::WaitResult::TIMEOUT:
      NoteTimeout(&cpu_timeouts_, "the game's queue", link, host);
      return false;
    case vk::WaitResult::BUSY: return false;
    case vk::WaitResult::FAILED:
      Stop("a Vulkan submission or wait failed", link, host);
      return false;
  }
  run.motion = (shared_motion_source.image != VK_NULL_HANDLE ? 1u : 0u);
  run.mask_fresh = ((mask_fresh_ && mask_.vulkan.image != VK_NULL_HANDLE) ? 1u : 0u);
  const std::optional<ipc::Reply> reply = link.Run(run);
  run_sent_ = link.RunSent();
  if (!reply) {
    in_flight_ = true;  // the helper may still be working on the shared textures
    return false;
  }
  cpu_timeouts_ = 0u;
  if (reply->busy != 0u) {
    ++busy_skips_;
  }
  if (reply->submitted != 0u && run.mask_fresh != 0u) {
    mask_fresh_ = false;
  }
  if (reply->ok == 0u || reply->wrote == 0u) return false;  // ok = 0: the helper's catch path, the texture may still be written
  vk::RecordCopyOut(host, back_buffer.image, entry_state, color_.vulkan.image);
  return true;
}

MaskCopy VkClient::CopyMask(vk::GameHost& host, const vk::ImageInfo& mask, NrLink& link) {
  if (!latch_.empty()) return {};
  if (fenced_ && to11_vk_ == VK_NULL_HANDLE) return {};
  if (in_flight_ || (pending_out_ != 0u && interop_.Counter(to11_vk_) < pending_out_)) return {.busy = true};
  const std::optional<color::MaskFormatInfo> format = color::DescribeMaskFormat(mask.format);
  if (!format || mask.samples != 1u || vk::VkFormatOf(format->view_format) == VK_FORMAT_UNDEFINED) {
    ReleaseMask(host);
    return {.unsupported = true};
  }
  if (mask.image == VK_NULL_HANDLE || !host.Valid()) return {};
  // Shared in the typed view format; the source keeps its own (a sRGB RGBA8 mask copies as raw texels).
  if (mask_.vulkan.image == VK_NULL_HANDLE || mask_.size != mask.size || mask_.format != format->view_format) {
    ReleaseMask(host);
    if (mask_failure_ && mask_failure_->Matches(mask.size, format->view_format)) return {};
    const ipc::Share share = {.width = mask.size.width, .height = mask.size.height, .dxgi_format = static_cast<uint32_t>(format->view_format)};
    mask_ask_ = {.size = mask.size, .format = format->view_format};
    const std::optional<ipc::Reply> shared = link.Share(ipc::RequestKind::SHARE_MASK, share);
    if (!shared) {
      in_flight_ = true;  // late: the handle arrives with a later reply
      return {.busy = true};
    }
    HANDLE handle = nullptr;
    std::string failure = "the helper refused";
    const bool opened = (shared->ok != 0u && shared->handles.mask != 0u && link.Pull(shared->handles.mask, &handle)
                         && Import(host, handle, mask.size, format->view_format, format->bytes_per_pixel, &mask_, &failure));
    if (handle != nullptr) {
      CloseHandle(handle);
    }
    if (!opened) {
      // Logged once: not retried for this size and format (final review Minor 3).
      mask_failure_ = ShareFailure{.size = mask.size, .format = format->view_format, .what = failure};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with Uplift's helper (DXGI_FORMAT {}, {})", static_cast<int>(format->view_format),
               failure);
      return {};
    }
    mask_failure_.reset();
  }
  vk::RecordMaskCopy(host, mask.image, mask_.vulkan.image);
  mask_fresh_ = true;
  return {.size = mask.size};
}

bool VkClient::FreeRetired(vk::GameHost& host) {
  interop_.FreeFinished(host);
  if (!interop_.Idle()) return false;
  interop_.FreeAll(host);  // nothing is retired any more: the wait fences, none of them in flight
  return true;
}

std::string VkClient::Line(std::string_view route_reason) const {
  const double mib = static_cast<double>(ImportedBytes()) / MIB;
  // T5: gitc-uplift.addon64 names the route of a Vulkan device whose NR runs in the helper, and why the chain got there.
  const std::string lead = (route_reason.empty() ? std::format("Vulkan ({}): NR runs in Uplift's 64-bit helper", BITNESS)
                                                 : std::format("Vulkan: NR runs in Uplift's helper process: {}", route_reason));
  std::string line = (fenced_ ? std::format("{} (shared textures and fences, {:.1f} MiB)", lead, mib)
                              : std::format("{} (shared textures, CPU-ordered, {:.1f} MiB): {}", lead, mib, cpu_reason_));
  if (second_queue_) {
    line += ", presents from a second queue";
  }
  return line;
}

}  // namespace uplift::client
