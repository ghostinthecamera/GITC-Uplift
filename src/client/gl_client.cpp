#include "client/gl_client.hpp"

#include <array>
#include <chrono>
#include <format>
#include <limits>
#include <span>
#include <utility>

#include "color/encoding.hpp"
#include "ipc/control_block.hpp"
#include "nr/log.hpp"

namespace uplift::client {
namespace {

constexpr uint32_t WAIT_CAP_MS = 2000u;  // the design's R29 cap on the game thread's waits
constexpr uint32_t MAX_CONSECUTIVE_TIMEOUTS = 3u;
constexpr double MIB = 1024.0 * 1024.0;

std::string ShareFailureText(const char* what, DXGI_FORMAT format, std::string_view failure) {
  return std::format("{} could not be shared with Uplift's helper (DXGI_FORMAT {}, {})", what, static_cast<int>(format), failure);
}

}  // namespace

GlClient::GlClient(gl::Functions functions, LUID luid, bool allow_fences) : interop_(std::move(functions)), luid_(luid) {
  const gl::Functions& gl = interop_.Gl();
  // Design §5: GPU-ordered needs GL_EXT_semaphore_win32 (Functions::Load decided it from the driver's list); without it the frame crosses through shared
  // textures, ordered on the CPU.
  fenced_ = (allow_fences && gl.semaphores);
  if (!fenced_) {
    cpu_reason_ = (!allow_fences                ? std::string("fences were turned off")
                   : !gl.cpu_reason.empty()     ? gl.cpu_reason
                                                : std::string("this driver does not offer GL_EXT_semaphore_win32"));
  }
}

GlClient::~GlClient() {
  CloseHeldFences();
}

void GlClient::CloseHeldFences() {
  for (HANDLE* const held : {&held_to12_, &held_to11_}) {
    if (*held != nullptr) {
      CloseHandle(*held);
      *held = nullptr;
    }
  }
}

void GlClient::Retire(gl::GameHost& host, Shared* shared) {
  interop_.Release(&shared->gl, host);
  *shared = {};
}

void GlClient::ReleaseShared(gl::GameHost& host) {
  Retire(host, &color_);
  Retire(host, &mask_);
  Retire(host, &motion_);
  mask_fresh_ = false;
  floor_probed_ = false;
}

void GlClient::ReleaseMask(gl::GameHost& host) {
  Retire(host, &mask_);
  mask_fresh_ = false;
}

bool GlClient::ReleaseAll(gl::GameHost& host) {
  const bool held = (color_.gl.texture != 0u || mask_.gl.texture != 0u || motion_.gl.texture != 0u || to12_gl_ != 0u || to11_gl_ != 0u
                     || held_to12_ != nullptr || held_to11_ != nullptr);
  ReleaseShared(host);
  // Work this side queued may still wait on a fence the helper's exit has just released (its value reads UINT64_MAX in the kernel, so the wait passes): the
  // semaphores are deleted, which the driver defers until the queue has passed their last use.
  interop_.ReleaseSemaphore(&to12_gl_, host);
  interop_.ReleaseSemaphore(&to11_gl_, host);
  CloseHeldFences();
  watchdog_ = {};
  last_value_ = 0u;
  waited11_ = 0u;
  pending_out_ = 0u;
  helper_to11_ = 0u;
  color_failure_.reset();
  mask_failure_.reset();
  motion_failure_.reset();
  in_flight_ = false;
  return held;
}

void GlClient::ForgetGl() {
  interop_.Forget();
  color_.gl = {};
  mask_.gl = {};
  motion_.gl = {};
  to12_gl_ = 0u;
  to11_gl_ = 0u;
  CloseHeldFences();
}

void GlClient::Stop(std::string_view reason, NrLink& link, gl::GameHost& host) {
  if (latch_.empty()) {
    latch_ = std::format("The OpenGL bridge stopped: {}. Restart the game to use NR again", reason);
    nr::Log(nr::LogLevel::ERR, latch_);
  }
  link.Stop(waited11_, latch_);
  ReleaseShared(host);
}

void GlClient::NoteTimeout(uint32_t* consecutive, std::string_view what, NrLink& link, gl::GameHost& host) {
  if (++*consecutive >= MAX_CONSECUTIVE_TIMEOUTS) {
    Stop(std::format("{} did not finish within 2 s, {} times in a row", what, MAX_CONSECUTIVE_TIMEOUTS), link, host);
  }
}

bool GlClient::Import(gl::GameHost& host, HANDLE handle, uint64_t allocation_bytes, nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, Shared* shared,
                      std::string* failure) {
  if (!host.Valid()) {
    *failure = "the OpenGL context was not the runtime's when the texture arrived";
    return false;
  }
  if (allocation_bytes == 0u) {
    *failure = "the helper did not say how large the texture's allocation is";
    return false;
  }
  const gl::ImportResult imported = interop_.ImportTexture(handle, allocation_bytes, format, size);
  if (imported.error != GL_NO_ERROR) {
    *failure = std::format("{} set GL error {:#06x}", imported.step, imported.error);
    return false;
  }
  Retire(host, shared);  // a newer texture of this kind replaces the older
  *shared = {.gl = imported.texture, .size = size, .format = format, .bytes = size.Pixels() * bytes_per_pixel};
  return true;
}

ipc::Target GlClient::Describe(gl::GameHost& host, const gl::ImageInfo& back_buffer, bool at_present, bool enabled, bool running) {
  interop_.FlushPending(host);  // the deletes that waited for a valid host (a no-op while it is not)
  ipc::Target target;
  target.width = back_buffer.size.width;
  target.height = back_buffer.size.height;
  target.dxgi_format = static_cast<uint32_t>(gl::SharedFormatOf(back_buffer.format));  // the helper makes the shared colour in it (R66: not ReShade's label)
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
  }
  std::string problem;
  if (!latch_.empty()) {
    problem = latch_;
  } else if (back_buffer.handle == 0u) {
    problem = "The back buffer is not an OpenGL image";
  } else {
    problem = gl::BackBufferProblem(back_buffer, at_present);
  }
  if (problem.empty() && host.WrongContext()) {
    problem = gl::WRONG_CONTEXT_PROBLEM;  // R63: the present is not on the runtime's context, so nothing runs
  }
  if (!running || !problem.empty()) {
    ReleaseShared(host);  // NR is off, or this image cannot go: the shared surfaces go with it (Plan 7's final review C-1)
  }
  if (problem.empty() && color_failure_ && color_failure_->Matches(back_buffer.size, back_buffer.format)) {
    problem = ShareFailureText("The back buffer", back_buffer.format, color_failure_->what);
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

void GlClient::NoteFrameSent() {
  if (frame_asks_colour_) {
    color_ask_ = frame_ask_;
  }
}

bool GlClient::Apply(const ipc::Reply& reply, NrLink& link, gl::GameHost& host) {
  in_flight_ = false;  // a FRAME reply means every earlier request has replied
  helper_to11_ = reply.to11_completed;
  bool fell_back = false;
  const ipc::Handles& handles = reply.handles;
  // Every NT handle in the reply is taken below, whatever this client does with it, so the helper's table never keeps one; each is closed once (the two
  // fences' are kept until a valid frame imported them).
  const auto pull = [&link](uint64_t remote) {
    HANDLE local = nullptr;
    return ((remote != 0u && link.Pull(remote, &local)) ? local : HANDLE(nullptr));
  };
  HANDLE to12 = pull(handles.to12);
  HANDLE to11 = pull(handles.to11);
  const HANDLE color = pull(handles.color);
  const HANDLE mask = pull(handles.mask);
  const HANDLE motion = pull(handles.motion);
  if (fenced_ && to12 != nullptr && to11 != nullptr) {
    // A new transport, so new fences: every value restarts (the helper's `progress` stays monotonic on its own).
    ReleaseShared(host);
    interop_.ReleaseSemaphore(&to12_gl_, host);
    interop_.ReleaseSemaphore(&to11_gl_, host);
    CloseHeldFences();
    held_to12_ = std::exchange(to12, nullptr);
    held_to11_ = std::exchange(to11, nullptr);
    watchdog_ = {};
    last_value_ = 0u;
    waited11_ = 0u;
    pending_out_ = 0u;
  }
  if (fenced_ && held_to12_ != nullptr && held_to11_ != nullptr && host.Valid()) {
    const GLenum first = interop_.ImportSemaphore(held_to12_, &to12_gl_);
    const GLenum second = (first == GL_NO_ERROR ? interop_.ImportSemaphore(held_to11_, &to11_gl_) : first);
    CloseHeldFences();  // the imported semaphores keep the fences alive
    if (second != GL_NO_ERROR) {
      // An unusual driver refuses the import: no restart would help, so the frame crosses through shared textures, ordered on the CPU, which need no fences.
      // The caller re-attaches the helper with SHARED_CPU (Plan 9 M-4's lesson).
      interop_.DeleteSemaphore(&to12_gl_);
      interop_.DeleteSemaphore(&to11_gl_);
      cpu_reason_ = std::format("the driver refused the fence import (glImportSemaphoreWin32HandleEXT set GL error {:#06x})", second);
      nr::Logf(nr::LogLevel::WARN,
               "The OpenGL context could not import the helper's shared fences (glImportSemaphoreWin32HandleEXT set GL error {:#06x}); frames cross through "
               "CPU-ordered shared textures instead",
               second);
      fenced_ = false;
      fell_back = true;
    }
  }
  if (!fell_back) {
    std::string failure;
    if (color != nullptr && latch_.empty()) {
      // The colour of the FRAME that was sent (color_ask_), which this reply may only be delivering from an earlier one.
      const DXGI_FORMAT shared_format = gl::SharedFormatOf(color_ask_.format);
      if (Import(host, color, handles.color_bytes, color_ask_.size, shared_format,
                 color::DescribeFormat(shared_format).value_or(color::FormatInfo{}).bytes_per_pixel, &color_, &failure)) {
        color_failure_.reset();
      } else {
        color_failure_ = ShareFailure{.size = color_ask_.size, .format = color_ask_.format, .what = failure};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("The back buffer", color_ask_.format, failure));
      }
    }
    if (mask != nullptr && latch_.empty()) {
      // The texture is the one the last SHARE_MASK asked for (this reply may have landed late), not the back buffer's size and format.
      const uint32_t bytes = color::DescribeMaskFormat(mask_ask_.format).value_or(color::MaskFormatInfo{.bytes_per_pixel = 4u}).bytes_per_pixel;
      if (Import(host, mask, handles.mask_bytes, mask_ask_.size, mask_ask_.format, bytes, &mask_, &failure)) {
        mask_failure_.reset();
      } else {
        mask_failure_ = ShareFailure{.size = mask_ask_.size, .format = mask_ask_.format, .what = failure};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("UPLIFT_MASK", mask_ask_.format, failure));
      }
    }
    if (motion != nullptr && latch_.empty()) {
      if (!Import(host, motion, handles.motion_bytes, motion_ask_size_, DXGI_FORMAT_R16G16_FLOAT, 4u, &motion_, &failure)) {
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
  if (fenced_ && latch_.empty() && to11_gl_ != 0u && to12_gl_ != 0u) {
    // Both directions count as progress, so a frame shows two steps: the game's work up to the copy-in, then NR's. A to11 value at UINT64_MAX is the helper's
    // device removed (the watchdog says so); a dead helper never gets here (the launcher sees it). The values are the helper's own reading in this reply: GL
    // cannot read a semaphore.
    if (const std::optional<std::string_view> stopped =
            watchdog_.Check(reply.to11_completed, reply.to11_completed + reply.to12_completed, std::chrono::steady_clock::now())) {
      Stop(*stopped, link, host);
    }
  }
  if (reply.running == 0u) {
    ReleaseShared(host);
  }
  return fell_back;
}

gl::ImageInfo GlClient::PrepareMotion(gl::GameHost& host, const gl::ImageInfo& motion, NrLink& link) {
  // Plan 7 decision 3: LaunchPad's UPLIFT_MV, shared like the mask (RG16F). Retired when a recording comes without one.
  if (motion.handle == 0u || motion.format != DXGI_FORMAT_R16G16_FLOAT || motion.samples != 1u) {
    if (motion_.gl.texture != 0u) {
      Retire(host, &motion_);
    }
    return {};
  }
  if (motion_.gl.texture == 0u || motion_.size != motion.size) {
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
                           && Import(host, handle, shared->handles.motion_bytes, motion.size, DXGI_FORMAT_R16G16_FLOAT, 4u, &motion_, &failure));
      if (handle != nullptr) {
        CloseHandle(handle);
      }
      if (!opened) {
        // Logged once: not retried for this size (Plan 7's final review Minor 3).
        motion_failure_ = ShareFailure{.size = motion.size, .format = DXGI_FORMAT_R16G16_FLOAT, .what = failure};
        nr::Logf(nr::LogLevel::WARN, "UPLIFT_MV could not be shared with Uplift's helper ({}); NR runs without LaunchPad's motion", failure);
      }
    }
  }
  return (motion_.gl.texture != 0u && motion_.size == motion.size) ? motion : gl::ImageInfo{};
}

bool GlClient::Run(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, addon::TriggerPoint point, NrLink& link) {
  run_sent_ = false;
  if (!latch_.empty() || in_flight_ || frame.handle == 0u || frame.samples != 1u || !host.Valid()) return false;
  if (color_.gl.texture == 0u || color_.size != frame.size || color_.format != gl::SharedFormatOf(frame.format)) return false;
  ipc::Run run = {.point = static_cast<uint32_t>(point)};
  if (!fenced_ && !nr::MeetsNrFloor(frame.size)) {
    // SHARED_CPU: below NR's floor nothing can come back, and a RUN costs a CPU wait. After the first RUN at this size (whose reply's status says why) no RUN
    // at all.
    if (floor_probed_) return false;
    const bool replied = link.Run(run).has_value();
    run_sent_ = link.RunSent();
    floor_probed_ = run_sent_;  // a RUN that was refused (another request outstanding) is tried again
    if (!replied) {
      in_flight_ = true;
    }
    return false;
  }
  return fenced_ ? RunFenced(host, frame, motion, run, link) : RunCpuOrdered(host, frame, motion, run, link);
}

bool GlClient::RunFenced(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ipc::Run run, NrLink& link) {
  if (to12_gl_ == 0u || to11_gl_ == 0u) return false;
  if (helper_to11_ == std::numeric_limits<uint64_t>::max()) {
    // The helper's device was removed (its fence reads UINT64_MAX; a dead helper is the launcher's): no RUN goes to a dead peer and no copy in is made for it.
    // Apply's watchdog latches the rest.
    return false;
  }
  if (pending_out_ != 0u) {
    // A RUN whose reply never came may still be on the helper's queue: nothing touches the shared textures until the helper's own reading of to11 (in the
    // newest FRAME reply) shows its signal (no GPU wait is queued for a signal nobody confirmed), or its late reply says it never signalled.
    if (const std::optional<bool> signalled = link.TakeLateRunSignalled(); signalled && !*signalled) {
      pending_out_ = 0u;
    }
    if (pending_out_ != 0u && helper_to11_ < pending_out_) {
      ++busy_skips_;
      return false;
    }
    pending_out_ = 0u;
  }
  const gl::ImageInfo shared_motion_source = PrepareMotion(host, motion, link);
  if (in_flight_) return false;
  run.motion = (shared_motion_source.handle != 0u ? 1u : 0u);
  run.mask_fresh = ((mask_fresh_ && mask_.gl.texture != 0u) ? 1u : 0u);

  class Steps final : public bridge::BridgeSteps {
   public:
    Steps(GlClient& client, gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, const ipc::Run& run, NrLink& link)
        : client_(client), host_(host), frame_(frame), motion_(motion), run_(run), link_(link) {}
    bool sent = false;       // link.Run put its request on the wire: the helper has seen this point
    bool timed_out = false;  // and it did not answer in time (or is gone), or the link refused it
    ipc::Reply reply;        // what WaitD3D12 got, for the three steps that ask the helper's side

    void CopyIn() override {
      host_.Copy(frame_, client_.color_.Image());
      if (motion_.handle != 0u) {
        host_.Copy(motion_, client_.motion_.Image());
      }
    }
    bool SignalD3D11(uint64_t value) override {
      // What the GL context wrote since the last signal, and the helper's queue is about to read: the colour, the motion when copied, the mask when fresh.
      std::array<GLuint, 3> textures = {client_.color_.gl.texture};
      size_t count = 1u;
      if (motion_.handle != 0u) {
        textures[count++] = client_.motion_.gl.texture;
      }
      if (run_.mask_fresh != 0u) {
        textures[count++] = client_.mask_.gl.texture;
      }
      client_.interop_.Signal(client_.to12_gl_, value, std::span<const GLuint>(textures.data(), count));
      return true;
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
      client_.helper_to11_ = reply.to11_completed;
      return reply.waited != 0u;
    }
    bool RecordAndExecute(bool* wrote) override {
      *wrote = (reply.ok != 0u && reply.wrote != 0u);  // ok = 0: the helper's catch path
      return reply.submitted != 0u;
    }
    bool SignalD3D12(uint64_t /*value*/) override { return reply.signalled != 0u; }
    // R17: only after the reply said the signal was submitted (RunBridgedFrame calls this after SignalD3D12 returned true).
    bool WaitD3D11(uint64_t value) override {
      client_.waited11_ = value;  // for Stop, whether or not the context took it
      const GLuint color = client_.color_.gl.texture;
      client_.interop_.Wait(client_.to11_gl_, value, std::span<const GLuint>(&color, 1u));
      return true;
    }
    void CopyOut() override { host_.Copy(client_.color_.Image(), frame_); }

   private:
    GlClient& client_;
    gl::GameHost& host_;
    const gl::ImageInfo& frame_;
    const gl::ImageInfo& motion_;
    ipc::Run run_;
    NrLink& link_;
  };
  const uint64_t in = ++last_value_;  // monotonic even when a step fails
  const uint64_t out = ++last_value_;
  run.out = out;
  Steps steps(*this, host, frame, shared_motion_source, run, link);
  const bridge::BridgedFrame result = bridge::RunBridgedFrame(steps, in, out);
  run_sent_ = steps.sent;
  if (steps.timed_out) {
    // No reply in 2 s (or the helper is gone, which the launcher shows next frame). Nothing was queued on this side's GPU after the copy in, and no GL wait
    // ever will be for `out`; the helper's list may still run, so until its to11 shows its signal (or a late reply says there is none) nothing touches the
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
  if (result.d3d12_signalled) {
    watchdog_.Submitted(out, std::chrono::steady_clock::now());  // from submission, after the helper's recording
  }
  if (!result.failure.empty()) {
    Stop(result.failure, link, host);
  }
  return result.wrote;
}

bool GlClient::RunCpuOrdered(gl::GameHost& host, const gl::ImageInfo& frame, const gl::ImageInfo& motion, ipc::Run run, NrLink& link) {
  // Design §5, in this order: the copy in, its completion on the CPU (a capped GLsync wait), RUN (whose reply comes when the helper's GPU work is done), the
  // copy out. Nothing waits on the GPU, so there is no fence to strand; a timeout skips this frame and three in a row stop the client.
  if (interop_.Busy()) {
    // An earlier wait's 2 s ran out and its sync has not signalled: nothing is queued behind a stuck context.
    ++busy_skips_;
    return false;
  }
  const gl::ImageInfo shared_motion_source = PrepareMotion(host, motion, link);
  if (in_flight_) return false;
  host.Copy(frame, color_.Image());
  if (shared_motion_source.handle != 0u) {
    host.Copy(shared_motion_source, motion_.Image());
  }
  switch (interop_.FlushAndWait(WAIT_CAP_MS)) {
    case gl::WaitResult::DONE: break;
    case gl::WaitResult::TIMEOUT:
      NoteTimeout(&cpu_timeouts_, "the game's GL context", link, host);
      return false;
    case gl::WaitResult::BUSY: return false;
    case gl::WaitResult::FAILED:
      Stop("an OpenGL wait failed", link, host);
      return false;
  }
  run.motion = (shared_motion_source.handle != 0u ? 1u : 0u);
  run.mask_fresh = ((mask_fresh_ && mask_.gl.texture != 0u) ? 1u : 0u);
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
  host.Copy(color_.Image(), frame);
  return true;
}

MaskCopy GlClient::CopyMask(gl::GameHost& host, const gl::ImageInfo& mask, NrLink& link) {
  if (!latch_.empty()) return {};
  if (fenced_ && to11_gl_ == 0u) return {};
  if (in_flight_ || (fenced_ && pending_out_ != 0u && helper_to11_ < pending_out_)) return {.busy = true};
  const std::optional<color::MaskFormatInfo> format = color::DescribeMaskFormat(mask.format);
  if (!format || mask.samples != 1u || gl::InternalFormatOf(format->view_format) == 0u) {
    ReleaseMask(host);
    return {.unsupported = true};
  }
  if (mask.handle == 0u || !host.Valid()) return {};
  // Shared in the typed view format; the source keeps its own (a sRGB RGBA8 mask copies as raw texels).
  if (mask_.gl.texture == 0u || mask_.size != mask.size || mask_.format != format->view_format) {
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
                         && Import(host, handle, shared->handles.mask_bytes, mask.size, format->view_format, format->bytes_per_pixel, &mask_, &failure));
    if (handle != nullptr) {
      CloseHandle(handle);
    }
    if (!opened) {
      // Logged once: not retried for this size and format (final review Minor 3).
      mask_failure_ = ShareFailure{.size = mask.size, .format = format->view_format, .what = failure};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with Uplift's helper (DXGI_FORMAT {}, {})", static_cast<int>(format->view_format), failure);
      return {};
    }
    mask_failure_.reset();
  }
  host.Copy(mask, mask_.Image());
  mask_fresh_ = true;
  return {.size = mask.size};
}

std::string GlClient::Line() const {
  const double mib = static_cast<double>(ImportedBytes()) / MIB;
  return (fenced_ ? std::format("OpenGL ({}): NR runs in Uplift's 64-bit helper (shared textures and fences, {:.1f} MiB)", BITNESS, mib)
                  : std::format("OpenGL ({}): NR runs in Uplift's 64-bit helper (shared textures, CPU-ordered, {:.1f} MiB): {}", BITNESS, mib, cpu_reason_));
}

}  // namespace uplift::client
