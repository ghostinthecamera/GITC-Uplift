#include "ipc/helper_launcher.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <new>
#include <system_error>
#include <vector>

#include "ipc/control_block.hpp"

namespace uplift::ipc {
namespace {

void CloseIf(HANDLE* handle) {
  if (*handle != nullptr) {
    CloseHandle(*handle);
    *handle = nullptr;
  }
}

}  // namespace

std::unique_ptr<HelperLauncher> HelperLauncher::Start(const std::filesystem::path& exe, std::string_view addon_file,
                                                      std::string_view build_id, LUID luid, std::string* error) {
  const auto fail = [error](std::string text) {
    *error = std::move(text);
    return std::unique_ptr<HelperLauncher>();
  };
  const auto failed_call = [&fail](const char* call, DWORD code) {
    return fail(std::format("Uplift's 64-bit helper could not start ({} {:#010x})", call, static_cast<uint32_t>(code)));
  };
  std::error_code ignored;
  if (!std::filesystem::exists(exe, ignored)) {
    return fail(std::format("{} was not found next to {}", exe.filename().string(), addon_file));
  }
  // Every failure below returns with `launcher` going out of scope: its destructor closes what was opened, and closing the
  // job ends a helper that was already assigned to it.
  std::unique_ptr<HelperLauncher> launcher(new HelperLauncher());
  SECURITY_ATTRIBUTES inherit = {.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
  // The block, both events and a SYNCHRONIZE handle to this process: the only handles the helper inherits.
  launcher->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0u, sizeof(ControlBlock), nullptr);
  if (launcher->mapping_ == nullptr) return failed_call("CreateFileMapping", GetLastError());
  void* const view = MapViewOfFile(launcher->mapping_, FILE_MAP_ALL_ACCESS, 0u, 0u, sizeof(ControlBlock));
  if (view == nullptr) return failed_call("MapViewOfFile", GetLastError());
  launcher->block_ = new (view) ControlBlock();
  ControlBlock* const block = launcher->block_;
  block->block_bytes = sizeof(ControlBlock);
  CopyText(block->build_id, build_id);
  block->luid_low = luid.LowPart;
  block->luid_high = luid.HighPart;
  launcher->request_event_ = CreateEventW(&inherit, FALSE, FALSE, nullptr);
  launcher->reply_event_ = CreateEventW(&inherit, FALSE, FALSE, nullptr);
  if (launcher->request_event_ == nullptr || launcher->reply_event_ == nullptr) return failed_call("CreateEvent", GetLastError());
  HANDLE game = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &game, SYNCHRONIZE, TRUE, 0u) == FALSE) {
    return failed_call("DuplicateHandle", GetLastError());
  }
  launcher->job_ = CreateJobObjectW(nullptr, nullptr);
  if (launcher->job_ == nullptr) {
    const DWORD failure = GetLastError();
    CloseHandle(game);
    return failed_call("CreateJobObject", failure);
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  if (SetInformationJobObject(launcher->job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == FALSE) {
    const DWORD failure = GetLastError();
    CloseHandle(game);
    return failed_call("SetInformationJobObject", failure);
  }
  HANDLE inherited[4] = {launcher->mapping_, launcher->request_event_, launcher->reply_event_, game};
  SIZE_T list_bytes = 0u;
  InitializeProcThreadAttributeList(nullptr, 1u, 0u, &list_bytes);
  std::vector<std::byte> list_storage(list_bytes);
  auto* const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
  if (InitializeProcThreadAttributeList(list, 1u, 0u, &list_bytes) == FALSE) {
    const DWORD failure = GetLastError();
    CloseHandle(game);
    return failed_call("InitializeProcThreadAttributeList", failure);
  }
  if (UpdateProcThreadAttribute(list, 0u, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) == FALSE) {
    const DWORD failure = GetLastError();
    DeleteProcThreadAttributeList(list);
    CloseHandle(game);
    return failed_call("UpdateProcThreadAttribute", failure);
  }
  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = list;
  std::wstring command = std::format(L"\"{}\" --block {:x} --request {:x} --reply {:x} --game {:x}", exe.wstring(),
                                     reinterpret_cast<uintptr_t>(launcher->mapping_),
                                     reinterpret_cast<uintptr_t>(launcher->request_event_),
                                     reinterpret_cast<uintptr_t>(launcher->reply_event_), reinterpret_cast<uintptr_t>(game));
  PROCESS_INFORMATION process = {};
  const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                      CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                      exe.parent_path().c_str(), &startup.StartupInfo, &process);
  const DWORD start_failure = GetLastError();
  DeleteProcThreadAttributeList(list);
  CloseHandle(game);  // the helper holds its own copy; this process needs none
  if (started == FALSE) return failed_call("CreateProcess", start_failure);
  launcher->process_ = process.hProcess;
  launcher->pid_ = process.dwProcessId;
  if (AssignProcessToJobObject(launcher->job_, process.hProcess) == FALSE) {
    const DWORD failure = GetLastError();
    TerminateProcess(process.hProcess, 1u);  // our own child, still suspended
    CloseHandle(process.hThread);
    return fail(std::format("Uplift's 64-bit helper could not be tied to the game (AssignProcessToJobObject {:#010x})",
                            static_cast<uint32_t>(failure)));
  }
  if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
    const DWORD failure = GetLastError();
    TerminateProcess(process.hProcess, 1u);  // our own child, never run: it must not linger suspended
    CloseHandle(process.hThread);
    return failed_call("ResumeThread", failure);
  }
  CloseHandle(process.hThread);
  // A later child of the game must not inherit the channel.
  for (HANDLE handle : {launcher->mapping_, launcher->request_event_, launcher->reply_event_}) {
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0u);
  }
  launcher->started_ = std::chrono::steady_clock::now();
  return launcher;
}

HelperLauncher::~HelperLauncher() {
  if (block_ != nullptr) {
    UnmapViewOfFile(block_);
  }
  CloseIf(&request_event_);
  CloseIf(&reply_event_);
  CloseIf(&mapping_);
  CloseIf(&process_);
  CloseIf(&job_);  // last: kill-on-close ends a helper that still runs
}

HelperState HelperLauncher::State() const {
  return static_cast<HelperState>(
      InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&block_->helper_state), 0, 0));
}

std::optional<DWORD> HelperLauncher::ExitCode() const {
  DWORD code = 0u;
  if (process_ == nullptr || WaitForSingleObject(process_, 0u) != WAIT_OBJECT_0 || GetExitCodeProcess(process_, &code) == FALSE) {
    return std::nullopt;
  }
  return code;
}

uint64_t HelperLauncher::Heartbeat() const {
  return static_cast<uint64_t>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&block_->heartbeat), 0, 0));
}

HelperLauncher::Wait HelperLauncher::Send(const Request& request, uint32_t timeout_ms) {
  if (pending_) return Wait::PENDING;
  if (ExitCode()) return Wait::GONE;
  block_->request = request;
  block_->request.sequence = ++sequence_;
  pending_ = true;
  heartbeat_seen_ = Heartbeat();
  sent_at_ = std::chrono::steady_clock::now();
  heartbeat_time_ = sent_at_;
  SetEvent(request_event_);  // a full barrier: the request is complete before the helper sees the event
  return Await(timeout_ms);
}

HelperLauncher::Wait HelperLauncher::Await(uint32_t timeout_ms) {
  const HANDLE handles[2] = {reply_event_, process_};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const DWORD result = WaitForMultipleObjects(2u, handles, FALSE, static_cast<DWORD>(std::max<int64_t>(left.count(), 0)));
    if (result == WAIT_OBJECT_0) {
      if (block_->reply.sequence == sequence_) {
        pending_ = false;
        return Wait::REPLIED;
      }
      continue;  // a signal for nothing this request is waiting for
    }
    if (result == WAIT_TIMEOUT) return Wait::PENDING;
    pending_ = false;  // the process ended (or the wait itself failed): there is nothing left to wait for
    return Wait::GONE;
  }
}

HelperLauncher::Wait HelperLauncher::Poll() {
  if (!pending_) return Wait::REPLIED;
  return Await(0u);
}

std::optional<std::chrono::seconds> HelperLauncher::Hung(std::chrono::steady_clock::time_point now) const {
  if (!pending_) return std::nullopt;
  if (const uint64_t heartbeat = Heartbeat(); heartbeat != heartbeat_seen_) {
    heartbeat_seen_ = heartbeat;
    heartbeat_time_ = now;
    return std::nullopt;
  }
  if (now - heartbeat_time_ < HUNG_AFTER) return std::nullopt;
  return std::chrono::duration_cast<std::chrono::seconds>(now - sent_at_);
}

bool HelperLauncher::Pull(uint64_t remote, HANDLE* local) {
  if (remote == 0u || process_ == nullptr) return false;
  return DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(remote)), GetCurrentProcess(), local, 0u, FALSE,
                         DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)
         != FALSE;
}

void HelperLauncher::DrainLog(const std::function<void(std::string_view line)>& sink) {
  LogDrain(&block_->log, sink);
}

void HelperLauncher::Kill() {
  if (process_ != nullptr) {
    TerminateProcess(process_, 1u);
  }
  pending_ = false;
  CloseIf(&job_);
}

std::chrono::milliseconds HelperLauncher::StartedFor() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_);
}

}  // namespace uplift::ipc
