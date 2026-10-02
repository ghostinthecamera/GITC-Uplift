#include "client/remote_nr.hpp"

#include <array>
#include <format>
#include <utility>

#include "ipc/control_block.hpp"
#include "nr/log.hpp"
#include "nr/session_status.hpp"

namespace uplift::client {
namespace {

constexpr uint32_t FRAME_CAP_MS = 100u;        // FRAME, ATTACH, DETACH, STOP, QUIT (design §2.2)
constexpr uint32_t RUN_CAP_MS = 2000u;         // RUN, SHARE_*
constexpr std::chrono::seconds START_CAP{10};  // STARTING without READY: the helper hung, or never resumed, while starting
constexpr std::chrono::seconds QUIT_CAP{2};    // QUIT answered, the process still there: ended (its Teardown is bounded by 2 s too)
// AddonUninit runs at every last-device destruction, and a game that recreates its device (a resolution change) waits for it. An idle
// helper exits within milliseconds of QUIT; a busy one is ended by the job, as it is when the game itself exits.
constexpr std::chrono::milliseconds QUIT_EXIT_WAIT{250};

// The NT handles a reply can hand out, besides the block's group (mapping, size and row pitch). Plan 12 (protocol 5): a texture's handle carries its
// allocation size (`bytes`), which an OpenGL import needs, so a late handle keeps it on the way to the FRAME reply that delivers it; a fence has none.
using HandleField = uint64_t ipc::Handles::*;
struct NtHandle {
  HandleField handle;
  HandleField bytes;  // null: none
};
constexpr std::array<NtHandle, 5> NT_HANDLES = {{
    {&ipc::Handles::color, &ipc::Handles::color_bytes},
    {&ipc::Handles::mask, &ipc::Handles::mask_bytes},
    {&ipc::Handles::motion, &ipc::Handles::motion_bytes},
    {&ipc::Handles::to12, nullptr},
    {&ipc::Handles::to11, nullptr},
}};

}  // namespace

RemoteNr::RemoteNr(RemoteNrConfig config) : config_(std::move(config)) {}

void RemoteNr::DrainLog() {
  if (launcher_ != nullptr && log_) {
    launcher_->DrainLog(log_);
  }
}

void RemoteNr::DiscardRemote(uint64_t remote) {
  HANDLE local = nullptr;
  if (launcher_ != nullptr && launcher_->Pull(remote, &local)) {
    CloseHandle(local);
  }
}

void RemoteNr::DiscardLate() {
  for (const auto& [field, bytes] : NT_HANDLES) {
    if (late_.*field != 0u) {
      DiscardRemote(late_.*field);
      late_.*field = 0u;
      if (bytes != nullptr) {
        late_.*bytes = 0u;
      }
    }
  }
  if (late_.block != 0u) {
    DiscardRemote(late_.block);
    late_.block = 0u;
    late_.block_bytes = 0u;
    late_.block_row_pitch = 0u;
  }
}

void RemoteNr::Forget() {
  launcher_.reset();  // closes the job: kill-on-close ends a helper that still runs
  attached_ = false;
  needs_detach_ = false;
  stale_outstanding_ = false;
  quit_sent_ = false;
  announced_ = false;
  outstanding_ = {};
  last_status_ = {};
  have_status_ = false;
  text_written_ = false;
  late_ = {};
  late_run_signalled_.reset();
}

void RemoteNr::Fail(std::string text) {
  DrainLog();
  failure_ = std::move(text);
  failed_ = true;
  nr::Logf(nr::LogLevel::ERR, "helper stopped: {}", failure_);
  if (launcher_ != nullptr) {
    launcher_->Kill();
  }
  Forget();
}

std::optional<ipc::Reply> RemoteNr::Transact(const ipc::Request& request, uint32_t cap_ms) {
  switch (launcher_->Send(request, cap_ms)) {
    case ipc::HelperLauncher::Wait::REPLIED:
      outstanding_ = {};
      return launcher_->Block()->reply;
    case ipc::HelperLauncher::Wait::PENDING:
      outstanding_ = {request.kind, (request.kind == ipc::RequestKind::FRAME ? request.frame.settings_generation : frame_generation_)};
      return std::nullopt;
    case ipc::HelperLauncher::Wait::GONE:
      outstanding_ = {};  // the next Present sees the exit
      return std::nullopt;
  }
  return std::nullopt;
}

void RemoteNr::Consume(const Outstanding& request, const ipc::Reply& reply, bool late) {
  switch (request.kind) {
    case ipc::RequestKind::ATTACH:
      if (reply.ok != 0u) {
        attached_ = true;
      } else {
        const std::string_view error = ipc::TextOf(reply.error);
        Fail(error.empty() ? std::string("The helper could not attach to the game's device") : std::string(error));
      }
      break;
    case ipc::RequestKind::FRAME:
    case ipc::RequestKind::RUN:
      // RUN's status is the recording's outcome (nr_applied, the skip reason); FRAME's predates it (design §2.2, batch 2).
      if (reply.ok != 0u) {
        last_status_ = reply.status;
        status_generation_ = request.generation;
        have_status_ = true;
      }
      break;
    case ipc::RequestKind::NONE:
    case ipc::RequestKind::SHARE_MASK:
    case ipc::RequestKind::SHARE_MOTION:
    case ipc::RequestKind::STOP:
    case ipc::RequestKind::DETACH:
    case ipc::RequestKind::QUIT:
      break;
  }
  if (late && request.kind == ipc::RequestKind::RUN) {
    late_run_signalled_ = (reply.signalled != 0u);  // a FENCED client waits for that RUN's `out` until it hears this
  }
  // Each NT handle is handed out exactly once, in the reply of the request that made it. A reply nobody passes on to a client
  // (one that landed late, or one that says ok = 0: the helper's catch path keeps what it made in it) keeps its handles here for
  // the next FRAME reply; a newer handle of the same kind replaces the older, which is taken and closed.
  if (launcher_ == nullptr || (!late && reply.ok != 0u)) return;
  for (const auto& [field, bytes] : NT_HANDLES) {
    if (reply.handles.*field == 0u) continue;
    if (late_.*field != 0u) {
      DiscardRemote(late_.*field);
    }
    late_.*field = reply.handles.*field;
    if (bytes != nullptr) {
      late_.*bytes = reply.handles.*bytes;
    }
  }
  if (reply.handles.block != 0u) {
    if (late_.block != 0u) {
      DiscardRemote(late_.block);
    }
    late_.block = reply.handles.block;
    late_.block_bytes = reply.handles.block_bytes;
    late_.block_row_pitch = reply.handles.block_row_pitch;
  }
}

std::optional<ipc::Reply> RemoteNr::Present(bool enabled, const ipc::Frame& frame, std::string_view settings_text,
                                            const std::function<void(std::string_view)>& log) {
  frame_sent_ = false;
  log_ = log;
  const auto now = std::chrono::steady_clock::now();
  const LUID wanted = {.LowPart = config_.attach.luid_low, .HighPart = config_.attach.luid_high};
  if (launcher_ != nullptr && (wanted.LowPart != launched_for_.LowPart || wanted.HighPart != launched_for_.HighPart)) {
    nr::Log(nr::LogLevel::INFO, "the game moved to another adapter; the helper restarts");
    launcher_->Kill();
    Forget();
  }
  if (launcher_ == nullptr) {
    if (!enabled || failed_) return std::nullopt;
    std::string error;
    launcher_ = ipc::HelperLauncher::Start(config_.helper_exe, ipc::TextOf(config_.attach.addon_file), config_.build_id, wanted, &error);
    if (launcher_ == nullptr) {
      Fail(std::move(error));
      return std::nullopt;
    }
    pid_ = launcher_->Pid();
    launched_for_ = wanted;
  }
  DrainLog();
  if (launcher_->State() == ipc::HelperState::FAILED) {
    const std::string_view error = ipc::TextOf(launcher_->Block()->helper_error);
    Fail(error.empty() ? std::string("The helper reported a failure") : std::string(error));
    return std::nullopt;
  }
  if (const std::optional<DWORD> code = launcher_->ExitCode()) {
    DrainLog();  // the helper's last lines (its unload) may have been written since the drain above
    if (quit_sent_ || *code == 0u) {
      // A QUIT, or its own 30 s idle exit (a game that stopped presenting): NR's memory went with the process.
      nr::Log(nr::LogLevel::INFO, "helper exited (NR released)");
      Forget();
    } else {
      Fail(std::format("It exited with code {:#010x}", static_cast<uint32_t>(*code)));
    }
    return std::nullopt;
  }
  if (launcher_->State() == ipc::HelperState::STARTING) {
    if (launcher_->StartedFor() > START_CAP) {
      Fail("It did not start within 10 s");
    }
    return std::nullopt;  // "Starting Uplift's 64-bit helper"
  }
  if (!announced_) {
    announced_ = true;
    nr::Logf(nr::LogLevel::INFO, "helper started (pid {}, ready in {} ms)", pid_, launcher_->StartedFor().count());
  }
  if (quit_sent_) {
    if (now - quit_at_ > QUIT_CAP) {
      nr::Log(nr::LogLevel::WARN, "the helper did not exit after QUIT; it was ended");
      launcher_->Kill();
      Forget();
    }
    return std::nullopt;
  }
  if (launcher_->Pending()) {
    switch (launcher_->Poll()) {
      case ipc::HelperLauncher::Wait::REPLIED:
        Consume(Outstanding(outstanding_), launcher_->Block()->reply, true);  // a copy: a refused ATTACH clears outstanding_
        outstanding_ = {};
        if (std::exchange(stale_outstanding_, false)) {
          DiscardLate();  // asked of a transport Detach() has dropped: nothing in it is of use to the next one
        }
        break;
      case ipc::HelperLauncher::Wait::PENDING:
        // "Busy" is a helper that could not keep up while NR was up. The first FRAME's NGX load (no status yet, or a Session that is
        // not ACTIVE) answers late by design and is not counted, or every first enable would show the note.
        if (have_status_ && last_status_.session_state == static_cast<uint32_t>(nr::SessionState::ACTIVE)) {
          ++busy_frames_;
        }
        if (const std::optional<std::chrono::seconds> waited = launcher_->Hung(now)) {
          Fail(std::format("It did not answer a request for {} s", waited->count()));
        }
        return std::nullopt;
      case ipc::HelperLauncher::Wait::GONE:
        return std::nullopt;
    }
    if (launcher_ == nullptr) return std::nullopt;  // the late reply was a refused ATTACH
  }
  if (needs_detach_) {
    needs_detach_ = false;
    ipc::Request detach = {.kind = ipc::RequestKind::DETACH};
    Transact(detach, FRAME_CAP_MS);
    if (launcher_->Pending()) return std::nullopt;
  }
  if (!enabled) {
    // NR is off: once the Session is back at OFF under these very settings, everything it held is unloaded, and the helper's
    // exit returns all of its VRAM (design §2.7). A helper that never attached has nothing to wait for.
    const bool session_off = (have_status_ && status_generation_ == frame.settings_generation
                              && last_status_.session_state == static_cast<uint32_t>(nr::SessionState::OFF));
    if (!attached_ || session_off) {
      ipc::Request quit = {.kind = ipc::RequestKind::QUIT};
      Transact(quit, FRAME_CAP_MS);  // whatever the answer, the helper is going
      quit_sent_ = true;
      quit_at_ = now;
      return std::nullopt;
    }
  }
  if (!attached_) {
    ipc::Request attach = {.kind = ipc::RequestKind::ATTACH, .attach = config_.attach};
    if (const std::optional<ipc::Reply> reply = Transact(attach, FRAME_CAP_MS)) {
      Consume({ipc::RequestKind::ATTACH, 0u}, *reply, false);
    }
    if (launcher_ == nullptr || !attached_) return std::nullopt;
  }
  if (!launcher_->Pending() && (!text_written_ || frame.settings_generation != written_generation_)) {
    // Only while no request is pending: the helper reads the text during a FRAME (review of batch 2, minor 8).
    ipc::CopyText(launcher_->Block()->settings_text, settings_text);
    written_generation_ = frame.settings_generation;
    text_written_ = true;
  }
  ipc::Request request = {.kind = ipc::RequestKind::FRAME};
  request.frame = frame;
  frame_generation_ = frame.settings_generation;
  frame_sent_ = true;
  const std::optional<ipc::Reply> answered = Transact(request, FRAME_CAP_MS);
  if (!answered) return std::nullopt;
  ipc::Reply reply = *answered;
  Consume({ipc::RequestKind::FRAME, frame.settings_generation}, reply, false);  // the status; an ok = 0 reply's handles are kept
  if (reply.ok == 0u) {
    if (const std::string_view error = ipc::TextOf(reply.error); error != last_error_) {
      last_error_ = std::string(error);
      nr::Logf(nr::LogLevel::WARN, "helper: FRAME failed: {}", error);
    }
    return std::nullopt;
  }
  // Handles of an earlier reply that landed late ride with this one (a newer one of a kind replaces the older).
  for (const auto& [field, bytes] : NT_HANDLES) {
    if (late_.*field == 0u) continue;
    if (reply.handles.*field == 0u) {
      reply.handles.*field = late_.*field;
      if (bytes != nullptr) {
        reply.handles.*bytes = late_.*bytes;
      }
    } else {
      DiscardRemote(late_.*field);
    }
    late_.*field = 0u;
    if (bytes != nullptr) {
      late_.*bytes = 0u;
    }
  }
  if (late_.block != 0u) {
    if (reply.handles.block == 0u) {
      reply.handles.block = late_.block;
      reply.handles.block_bytes = late_.block_bytes;
      reply.handles.block_row_pitch = late_.block_row_pitch;
    } else {
      DiscardRemote(late_.block);
    }
    late_.block = 0u;
    late_.block_bytes = 0u;
    late_.block_row_pitch = 0u;
  }
  return reply;
}

void RemoteNr::Detach() {
  attached_ = false;
  if (launcher_ == nullptr) return;
  DiscardLate();  // handles of the transport being dropped are of no use to the next one
  if (launcher_->Pending()) {
    needs_detach_ = true;       // sent at the next present that finds the request done
    stale_outstanding_ = true;  // and what is outstanding was asked of the dropped transport: its handles go the same way
    return;
  }
  ipc::Request detach = {.kind = ipc::RequestKind::DETACH};
  Transact(detach, FRAME_CAP_MS);
}

void RemoteNr::RetryNow() {
  failed_ = false;
  failure_.clear();
}

void RemoteNr::Quit() {
  if (launcher_ != nullptr && !quit_sent_ && !launcher_->Pending() && launcher_->State() == ipc::HelperState::READY) {
    ipc::Request quit = {.kind = ipc::RequestKind::QUIT};
    Transact(quit, FRAME_CAP_MS);
    // Closing the job at once would kill an idle helper inside its Teardown: give it a moment to exit by itself (short: the
    // game is going away or recreating its device, and the kill-on-close job is the backstop).
    const auto deadline = std::chrono::steady_clock::now() + QUIT_EXIT_WAIT;
    while (!launcher_->ExitCode() && std::chrono::steady_clock::now() < deadline) {
      Sleep(5u);
    }
  }
  DrainLog();
  Forget();
}

std::optional<ipc::Reply> RemoteNr::Run(const ipc::Run& run) {
  run_sent_ = false;
  late_run_signalled_.reset();  // a newer RUN supersedes what an older one's late reply said
  if (launcher_ == nullptr || !attached_ || launcher_->Pending()) return std::nullopt;
  run_sent_ = true;
  ipc::Request request = {.kind = ipc::RequestKind::RUN};
  request.run = run;
  const std::optional<ipc::Reply> reply = Transact(request, RUN_CAP_MS);
  if (reply) {
    Consume({ipc::RequestKind::RUN, frame_generation_}, *reply, false);
  }
  return reply;
}

std::optional<ipc::Reply> RemoteNr::Share(ipc::RequestKind kind, const ipc::Share& share) {
  if (launcher_ == nullptr || !attached_ || launcher_->Pending()) return std::nullopt;
  ipc::Request request = {.kind = kind};
  request.share = share;
  const std::optional<ipc::Reply> reply = Transact(request, RUN_CAP_MS);
  if (reply) {
    Consume({kind, 0u}, *reply, false);  // an ok = 0 reply's handles are kept; an ok one's go to the caller
  }
  return reply;
}

bool RemoteNr::Pull(uint64_t remote, HANDLE* local) {
  return launcher_ != nullptr && launcher_->Pull(remote, local);
}

void RemoteNr::Stop(uint64_t waited11, std::string_view reason) {
  if (launcher_ == nullptr || !attached_ || launcher_->Pending()) return;
  ipc::Request request = {.kind = ipc::RequestKind::STOP};
  request.stop.waited11 = waited11;
  ipc::CopyText(request.stop.reason, reason);
  Transact(request, FRAME_CAP_MS);
}

}  // namespace uplift::client
