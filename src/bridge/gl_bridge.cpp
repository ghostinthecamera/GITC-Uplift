#include "bridge/gl_bridge.hpp"

#include <array>
#include <format>
#include <span>
#include <utility>

#include "color/encoding.hpp"
#include "nr/log.hpp"

namespace uplift::bridge {
namespace {

using Microsoft::WRL::ComPtr;

// Key decision h: every CPU wait on this path is capped, and a timeout skips that frame; a single one never latches (Plan 9 M-2), and
// MAX_CONSECUTIVE_TIMEOUTS in a row do.
constexpr uint32_t WAIT_CAP_MS = 2000u;
constexpr uint32_t MAX_CONSECUTIVE_TIMEOUTS = 3u;

}  // namespace

std::unique_ptr<GlBridge> GlBridge::Create(gl::Functions functions, LUID luid, ComPtr<ID3D12Device>* created, std::string* error) {
  const bool semaphores = functions.semaphores;
  std::string no_semaphores = functions.cpu_reason;
  std::unique_ptr<GlBridge> bridge(new GlBridge(std::move(functions)));
  bridge->side_ = D3D12Side::Create(luid, created, error);
  if (bridge->side_ == nullptr) return nullptr;
  if (semaphores) {
    // GPU-ordered: the two shared D3D12 fences, imported as GL semaphores. A refused import builds CPU-ordered and says why (Plan 9 M-4's lesson: the fallback
    // is decided here, before any frame). The context is current: the caller made it so for Functions::Load.
    ComPtr<ID3D12Fence>* const fences[] = {&bridge->to12_, &bridge->to11_};
    GLuint* const semaphore_names[] = {&bridge->to12_gl_, &bridge->to11_gl_};
    for (size_t index = 0u; index < 2u && bridge->cpu_reason_.empty(); ++index) {
      HANDLE handle = nullptr;
      if (const HRESULT result = bridge->side_->CreateSharedFence(fences[index], &handle); FAILED(result)) {
        bridge->cpu_reason_ = std::format("the shared fence could not be created (HRESULT {:#010x})", static_cast<uint32_t>(result));
        break;
      }
      const GLenum imported = bridge->interop_.ImportSemaphore(handle, semaphore_names[index]);
      CloseHandle(handle);  // the imported semaphore keeps the fence alive
      if (imported != GL_NO_ERROR) {
        bridge->cpu_reason_ = std::format("the driver refused the fence import (glImportSemaphoreWin32HandleEXT set GL error {:#06x})", imported);
      }
    }
    if (bridge->cpu_reason_.empty()) {
      bridge->gpu_ordered_ = true;
    } else {
      bridge->ReleaseSemaphores();
      bridge->to12_.Reset();
      bridge->to11_.Reset();
    }
  } else {
    bridge->cpu_reason_ = (no_semaphores.empty() ? std::string("this driver does not offer GL_EXT_semaphore_win32") : std::move(no_semaphores));
  }
  if (bridge->gpu_ordered_) {
    nr::Log(nr::LogLevel::INFO, "OpenGL: NR runs on a private Direct3D 12 device");
  } else {
    nr::Logf(nr::LogLevel::INFO, "OpenGL: NR runs on a private Direct3D 12 device (CPU-ordered: {})", bridge->cpu_reason_);
  }
  return bridge;
}

GlBridge::~GlBridge() {
  // Nothing the private queue may still read is released before it finishes: at most 2 s, as DeviceContext::Teardown. No GL call: the context may be gone
  // (ForgetGl ran at destroy_device, and the names die with the share group).
  if (side_ != nullptr) {
    side_->Drain();
  }
  if (progress_event_ != nullptr) {
    CloseHandle(progress_event_);
  }
}

bool GlBridge::CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, uint32_t bytes_per_pixel, const wchar_t* name, Shared* shared,
                            std::string* failure) {
  Shared created = {.size = size, .format = format, .bytes = size.Pixels() * bytes_per_pixel};
  HANDLE handle = nullptr;
  if (const HRESULT result = side_->CreateShared(size, format, flags, name, &created.d3d12, &handle); FAILED(result)) {
    *failure = std::format("Direct3D 12 returned HRESULT {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  // The import needs the allocation's size, not the texture's: GL_EXT_memory_object's dedicated import takes the D3D12 resource's allocation.
  const D3D12_RESOURCE_DESC description = created.d3d12->GetDesc();
  created.bytes = side_->Device()->GetResourceAllocationInfo(0u, 1u, &description).SizeInBytes;
  const gl::ImportResult imported = interop_.ImportTexture(handle, created.bytes, format, size);
  CloseHandle(handle);  // the imported memory keeps the allocation
  if (imported.error != GL_NO_ERROR) {
    *failure = std::format("{} set GL error {:#06x}", imported.step, imported.error);
    return false;
  }
  created.gl = imported.texture;
  *shared = std::move(created);
  return true;
}

void GlBridge::ReleaseSemaphores() {
  interop_.DeleteSemaphore(&to12_gl_);
  interop_.DeleteSemaphore(&to11_gl_);
}

void GlBridge::Retire(gl::GameHost& host, Shared* shared) {
  // D3D12: held until the side's progress passes the last recording that used it. GL: deleted now (and ReShade's FBO cache told), or at the next valid frame;
  // the driver defers the real release until the queue has passed the last use.
  if (shared->d3d12) {
    side_->Retire(std::move(shared->d3d12), nullptr, shared->last_use);
  }
  interop_.Release(&shared->gl, host);
  *shared = {};
}

void GlBridge::LatchGl(std::string_view failure) {
  if (!latch_.empty()) return;
  std::string reason = std::format("The OpenGL bridge stopped: {}. Restart the game to use NR again", failure);
  nr::Log(nr::LogLevel::ERR, reason);
  // After a failed wait the private queue would otherwise wait on `to12` until destroy_device.
  Stop(std::move(reason));
}

void GlBridge::NoteTimeout(uint32_t* consecutive, std::string_view what) {
  if (++*consecutive >= MAX_CONSECUTIVE_TIMEOUTS) {
    LatchGl(std::format("{} did not finish within 2 s, {} times in a row", what, MAX_CONSECUTIVE_TIMEOUTS));
  }
}

BridgeFrame GlBridge::BeginFrame(gl::GameHost& host, const gl::ImageInfo& back_buffer, bool at_present, bool enabled, bool running,
                                 std::chrono::steady_clock::time_point now) {
  side_->FreeFinished();
  interop_.FlushPending(host);
  if (latch_.empty() && gpu_ordered_) {
    // Both directions count as progress, so a frame shows two steps: the game's work up to the copy-in, then NR's.
    const uint64_t completed = to11_->GetCompletedValue();
    if (const std::optional<std::string_view> stopped = watchdog_.Check(completed, completed + to12_->GetCompletedValue(), now)) {
      std::string reason = std::format("The OpenGL bridge stopped: {}. Restart the game to use NR again", *stopped);
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
    // NR is off, or the bridge stopped (Plan 7 C-1, above the latch return): the shared surfaces go with NR's own (the stutter fix). Retire keeps each D3D12
    // one until the queue has passed its last use, deletes the GL one at once (or at the next valid frame), and never waits.
    Retire(host, &color_);
    Retire(host, &mask_);
    Retire(host, &motion_);
    mask_fresh_ = false;
    ran_logged_ = false;  // the next enable logs that NR ran on the game's frame again
  }
  if (!latch_.empty()) return {.problem = latch_};
  if (back_buffer.handle == 0u) return {};
  std::string problem = gl::BackBufferProblem(back_buffer, at_present);
  const DXGI_FORMAT shared_format = gl::SharedFormatOf(back_buffer.format);  // ReShade's label is not the memory layout (R66)
  if (problem.empty() && host.WrongContext()) {
    problem = gl::WRONG_CONTEXT_PROBLEM;  // R63: the present is not on the runtime's context, so nothing runs
  }
  if (problem.empty()) {
    const std::optional<color::FormatInfo> format = color::DescribeFormat(shared_format);  // BackBufferProblem found the format usable
    if (running && host.Valid() && !(color_failure_ && color_failure_->Matches(back_buffer.size, back_buffer.format))
        && (!color_.d3d12 || color_.size != back_buffer.size || color_.format != shared_format)) {
      Retire(host, &color_);
      std::string failure;
      if (CreateShared(back_buffer.size, shared_format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, format->bytes_per_pixel, L"Uplift shared back buffer",
                       &color_, &failure)) {
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
  return {.color = color_.d3d12.Get(), .size = back_buffer.size, .format = shared_format};
}

bool GlBridge::RecordAndExecute(const Recorder& record, ID3D12Resource* motion12, bool* wrote, bool* used_mask) {
  ID3D12GraphicsCommandList* const list = side_->BeginList();
  if (list == nullptr) return false;
  const BridgeTargets targets = {
      .color = color_.d3d12.Get(),
      .mask = (mask_fresh_ ? mask_.d3d12.Get() : nullptr),
      .motion = motion12,
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

void GlBridge::StampUsed(uint64_t value, bool used_mask, bool used_motion) {
  color_.last_use = value;
  if (used_mask) {
    mask_.last_use = value;
    mask_fresh_ = false;
  }
  if (used_motion) {
    motion_.last_use = value;
  }
}

bool GlBridge::Run(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, const Recorder& record) {
  // The frame's format family must be the shared colour's too (R66: FB0's label and ReShade's intermediate share a family today): a copy between two formats
  // that are not compatible would raise GL_INVALID_OPERATION in the game's own error flag.
  if (!latch_.empty() || !color_.d3d12 || frame.handle == 0u || frame.size != color_.size || frame.samples != 1u || gl::SharedFormatOf(frame.format) != color_.format
      || !host.Valid()) {
    return false;
  }
  // A full ring, or, CPU-ordered, an earlier recording still in flight (its 2 s wait ran out) or an earlier GL wait that has not signalled yet: this frame goes
  // without NR rather than waiting, and it is counted. While either is in flight nothing is copied in: the copy in would write the shared colour under an NR
  // that still reads and writes it, and a stuck GL queue would otherwise get a full-frame copy (and a motion copy) queued behind it at every skipped frame.
  if (!side_->SlotFree() || (!gpu_ordered_ && (side_->Completed() < side_->LastSignalled() || interop_.Busy()))) {
    ++busy_skips_;
    return false;
  }
  // LaunchPad's UPLIFT_MV, shared like the mask (Plan 7 §9 g's flags). Retired when a recording comes without one.
  ID3D12Resource* motion12 = nullptr;
  if (motion.handle != 0u && motion.format == DXGI_FORMAT_R16G16_FLOAT && motion.samples == 1u) {
    if (!motion_.d3d12 || motion_.size != motion.size) {
      Retire(host, &motion_);
      if (!(motion_failure_ && motion_failure_->Matches(motion.size, DXGI_FORMAT_R16G16_FLOAT))) {
        std::string failure;
        if (CreateShared(motion.size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, 4u,
                         L"Uplift shared motion", &motion_, &failure)) {
          motion_failure_.reset();
        } else {
          // Logged once: not retried for this size (Plan 7 Minor 3).
          motion_failure_ = ShareFailure{.size = motion.size, .format = DXGI_FORMAT_R16G16_FLOAT, .what = failure};
          nr::Logf(nr::LogLevel::WARN, "UPLIFT_MV could not be shared with the private Direct3D 12 device ({}); NR runs without LaunchPad's motion", failure);
        }
      }
    }
    motion12 = motion_.d3d12.Get();  // null when the share failed: NR runs without motion
  } else if (motion_.d3d12) {
    Retire(host, &motion_);
  }
  const gl::ImageInfo motion_source = (motion12 != nullptr ? motion : gl::ImageInfo{});
  const bool wrote =
      (gpu_ordered_ ? RunGpuOrdered(host, frame, motion_source, motion12, record) : RunCpuOrdered(host, frame, motion_source, motion12, record));
  if (wrote && !ran_logged_) {
    // Once per enable (BeginFrame clears it when NR goes off): the e2e cases and the checklist read it as "a frame came back through the bridge".
    ran_logged_ = true;
    nr::Log(nr::LogLevel::INFO, "NR ran on the game's frame (OpenGL, 64-bit)");
  }
  return wrote;
}

bool GlBridge::RunGpuOrdered(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ID3D12Resource* motion12, const Recorder& record) {
  // The steps on this bridge's objects; RunBridgedFrame owns their order (R17: the GL wait is queued only for a signal that was submitted).
  class Steps final : public BridgeSteps {
   public:
    Steps(GlBridge& bridge, gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ID3D12Resource* motion12, const Recorder& record)
        : bridge_(bridge), host_(host), frame_(frame), motion_(motion), motion12_(motion12), record_(record) {}
    bool used_mask = false;
    bool executed = false;  // the list was submitted: its surfaces are in use until the side's progress passes it

    void CopyIn() override {
      host_.Copy(frame_, bridge_.color_.Image());
      if (motion_.handle != 0u) {
        host_.Copy(motion_, bridge_.motion_.Image());
      }
    }
    bool SignalD3D11(uint64_t value) override {
      // What the GL context wrote since the last signal, and the D3D12 side is about to read: the colour, the motion when copied, the mask when fresh.
      std::array<GLuint, 3> textures = {bridge_.color_.gl.texture};
      size_t count = 1u;
      if (motion_.handle != 0u) {
        textures[count++] = bridge_.motion_.gl.texture;
      }
      if (bridge_.mask_fresh_) {
        textures[count++] = bridge_.mask_.gl.texture;
      }
      bridge_.interop_.Signal(bridge_.to12_gl_, value, std::span<const GLuint>(textures.data(), count));
      return true;
    }
    bool WaitD3D12(uint64_t value) override {
      bridge_.waited12_ = value;  // for Stop, whether or not the queue took it
      return SUCCEEDED(bridge_.side_->Queue()->Wait(bridge_.to12_.Get(), value));
    }
    bool RecordAndExecute(bool* wrote) override {
      executed = bridge_.RecordAndExecute(record_, motion12_, wrote, &used_mask);
      return executed;
    }
    // progress first: when the queue refuses it, to11_ is never signalled, so GL never waits for this frame.
    bool SignalD3D12(uint64_t value) override {
      return bridge_.side_->SignalProgress(value) && SUCCEEDED(bridge_.side_->Queue()->Signal(bridge_.to11_.Get(), value));
    }
    bool WaitD3D11(uint64_t value) override {
      bridge_.waited11_ = value;  // for Stop, whether or not the context took it
      const GLuint color = bridge_.color_.gl.texture;
      bridge_.interop_.Wait(bridge_.to11_gl_, value, std::span<const GLuint>(&color, 1u));
      return true;
    }
    void CopyOut() override { host_.Copy(bridge_.color_.Image(), frame_); }

   private:
    GlBridge& bridge_;
    gl::GameHost& host_;
    const gl::ImageInfo& frame_;
    const gl::ImageInfo& motion_;
    ID3D12Resource* motion12_;
    const Recorder& record_;
  };
  Steps steps(*this, host, frame, motion, motion12, record);
  const uint64_t in = ++last_value_;  // monotonic even when a step fails
  const uint64_t out = ++last_value_;
  const BridgedFrame result = RunBridgedFrame(steps, in, out);
  // Plan 7 C-1: stamped whenever the list was submitted, even when the D3D12 signal then failed: its surfaces are then held until the side's progress passes
  // `out` (for good, if that signal never comes), never freed under the list.
  if (steps.executed || result.d3d12_signalled) {
    StampUsed(out, steps.used_mask, motion12 != nullptr);
  }
  if (result.d3d12_signalled) {
    watchdog_.Submitted(out, std::chrono::steady_clock::now());  // Plan 7 I-2: from submission, after recording
  }
  if (!result.failure.empty()) {
    LatchGl(result.failure);
  }
  return result.wrote;
}

bool GlBridge::RunCpuOrdered(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ID3D12Resource* motion12, const Recorder& record) {
  // Design §3.4, in this order: the copy in, the copy's completion on the CPU (a GLsync), NR, NR's completion on the CPU, the copy out. Nothing waits on the
  // GPU, so there is no fence to strand and no watchdog; a timeout skips this frame, and three in a row stop the bridge. Run has skipped the frames on which
  // an earlier recording is still in flight. This path is unmeasured on GL (R69): the smoke's CPU-ordered case decides whether it stays.
  host.Copy(frame, color_.Image());
  if (motion.handle != 0u) {
    host.Copy(motion, motion_.Image());
  }
  switch (interop_.FlushAndWait(WAIT_CAP_MS)) {
    case gl::WaitResult::DONE: break;
    case gl::WaitResult::TIMEOUT:
      NoteTimeout(&cpu_timeouts_, "the game's GL context");
      return false;
    case gl::WaitResult::BUSY: return false;  // Run has already skipped the frames on which a sync was in flight
    case gl::WaitResult::FAILED:
      LatchGl("an OpenGL wait failed");
      return false;
  }
  const uint64_t value = ++last_value_;
  bool wrote = false;
  bool used_mask = false;
  const bool executed = RecordAndExecute(record, motion12, &wrote, &used_mask);
  const bool signalled = side_->SignalProgress(value);
  if (executed || signalled) {
    StampUsed(value, used_mask, motion12 != nullptr);
  }
  if (!signalled) {
    LatchGl("the private Direct3D 12 queue refused to signal");
    return false;
  }
  if (progress_event_ == nullptr) {
    progress_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  // An earlier frame's wait that timed out left its completion registered on this (auto-reset) event: cleared now, which is safe because Run guarantees that
  // nothing was in flight, so no completion of an older value can still arrive.
  if (progress_event_ == nullptr || ResetEvent(progress_event_) == FALSE || FAILED(side_->Progress()->SetEventOnCompletion(value, progress_event_))) {
    LatchGl("the private Direct3D 12 queue's completion could not be waited for");
    return false;
  }
  // The event is set later (from the interrupt path) than the fence's value reaches the GPU, so a stale completion of an older recording could still end this
  // wait early; the fence's own value, which only `value` reaching it can satisfy, decides.
  if (WaitForSingleObject(progress_event_, WAIT_CAP_MS) != WAIT_OBJECT_0 || side_->Completed() < value) {
    NoteTimeout(&cpu_timeouts_, "NR on the private Direct3D 12 queue");  // the surfaces stay stamped; Run skips frames until it ends
    return false;
  }
  cpu_timeouts_ = 0u;
  if (!(executed && wrote)) return false;
  host.Copy(color_.Image(), frame);
  return true;
}

MaskCopy GlBridge::CopyMask(gl::GameHost& host, const gl::ImageInfo& mask) {
  if (!latch_.empty()) return {};
  const std::optional<color::MaskFormatInfo> format = color::DescribeMaskFormat(mask.format);
  if (!format || mask.samples != 1u || gl::InternalFormatOf(format->view_format) == 0u) {
    ReleaseMask(host);
    return {.unsupported = true};
  }
  if (mask.handle == 0u || !host.Valid()) return {};
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
  host.Copy(mask, mask_.Image());
  mask_fresh_ = true;
  return {.size = mask.size};
}

void GlBridge::ReleaseMask(gl::GameHost& host) {
  Retire(host, &mask_);
  mask_fresh_ = false;
}

void GlBridge::Stop(std::string reason) {
  if (latch_.empty()) {
    latch_ = std::move(reason);
  }
  // Each fence up to the highest value a wait on it was queued for; nothing waits beyond those. A refused signal (a removed private device) needs no retry:
  // that fence already reads UINT64_MAX, above every wait. CPU-ordered has no fence, so nothing to release.
  if (to11_ && to11_->GetCompletedValue() < waited11_) {
    to11_->Signal(waited11_);
  }
  if (to12_ && to12_->GetCompletedValue() < waited12_) {
    to12_->Signal(waited12_);
  }
}

void GlBridge::ForgetGl() {
  interop_.Forget();
  color_.gl = {};
  mask_.gl = {};
  motion_.gl = {};
  to12_gl_ = 0u;
  to11_gl_ = 0u;
}

uint64_t GlBridge::SharedBytes() const {
  return color_.bytes + mask_.bytes + motion_.bytes;
}

std::string GlBridge::StatusLine() const {
  const double mib = static_cast<double>(SharedBytes()) / (1024.0 * 1024.0);
  std::string line = (gpu_ordered_ ? std::format("OpenGL: NR runs on a private Direct3D 12 device (shared textures and fences, {:.1f} MiB)", mib)
                                   : std::format("OpenGL: NR runs on a private Direct3D 12 device (shared textures, CPU-ordered, {:.1f} MiB): {}", mib,
                                                 cpu_reason_));
  if (busy_skips_ > 0u) {
    line += std::format(", {} frame(s) without NR while the bridge was busy", busy_skips_);
  }
  return line;
}

}  // namespace uplift::bridge
