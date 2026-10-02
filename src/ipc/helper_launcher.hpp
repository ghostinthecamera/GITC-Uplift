#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ipc/protocol.hpp"

namespace uplift::ipc {

// Plan 9 (design §2.2): starts gitc-uplift-helper64.exe in a kill-on-close job and owns the channel to it. The game thread
// never blocks here beyond the caps its callers pass. Not thread-safe: the add-on's lock.
class HelperLauncher {
 public:
  enum class Wait : uint8_t {
    REPLIED,
    PENDING,
    GONE,
  };

  // nullptr + `error` when a step fails; a started helper is terminated then. The helper's own start-up (D3D12 device,
  // READY) is not waited for: State() says when it is up. `build_id` goes into the header the helper checks; `addon_file` ("gitc-uplift.addon32"
  // or "gitc-uplift.addon64") only names the add-on in the "was not found next to" text.
  static std::unique_ptr<HelperLauncher> Start(const std::filesystem::path& exe, std::string_view addon_file, std::string_view build_id,
                                               LUID luid, std::string* error);
  ~HelperLauncher();  // closes the job last: kill-on-close ends a helper that still runs
  HelperLauncher(const HelperLauncher&) = delete;
  HelperLauncher& operator=(const HelperLauncher&) = delete;

  [[nodiscard]] ControlBlock* Block() const { return block_; }
  [[nodiscard]] HelperState State() const;              // STARTING until the helper wrote READY or FAILED
  [[nodiscard]] std::optional<DWORD> ExitCode() const;  // set once the process has ended (a zero-timeout wait)
  // Writes `request` (its sequence stamped here) and waits up to `timeout_ms` for its reply event; PENDING leaves it
  // outstanding, and no new request may be sent until Poll returns REPLIED (a Send while one is pending sends nothing and
  // returns PENDING). GONE: the process ended.
  Wait Send(const Request& request, uint32_t timeout_ms);
  // The outstanding request's reply, without waiting. REPLIED also when nothing is outstanding.
  Wait Poll();
  [[nodiscard]] bool Pending() const { return pending_; }
  // Pending with the heartbeat unchanged for 20 s (design §2.2): how long the outstanding request has been waiting for its reply
  // (a long handler that beat for 120 s first counts them all). nullopt while the helper is not hung.
  [[nodiscard]] std::optional<std::chrono::seconds> Hung(std::chrono::steady_clock::time_point now) const;
  // DuplicateHandle(helper -> this process, DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS): takes over an NT handle by
  // its value in the helper's table, closing the helper's copy in the same call. The caller closes `local`.
  bool Pull(uint64_t remote, HANDLE* local);
  // Every complete line the helper logged since the last call, oldest first: its level as an ASCII digit (nr::LogLevel),
  // then the message.
  void DrainLog(const std::function<void(std::string_view line)>& sink);
  void Kill();  // TerminateProcess on this child, then closes the job
  [[nodiscard]] DWORD Pid() const { return pid_; }
  [[nodiscard]] std::chrono::milliseconds StartedFor() const;

  static constexpr std::chrono::seconds HUNG_AFTER{20};

 private:
  HelperLauncher() = default;
  Wait Await(uint32_t timeout_ms);
  [[nodiscard]] uint64_t Heartbeat() const;

  HANDLE job_ = nullptr;
  HANDLE process_ = nullptr;
  HANDLE mapping_ = nullptr;
  ControlBlock* block_ = nullptr;
  HANDLE request_event_ = nullptr;
  HANDLE reply_event_ = nullptr;
  DWORD pid_ = 0u;
  uint32_t sequence_ = 0u;                         // the newest request's
  bool pending_ = false;                           // that request has no reply yet
  std::chrono::steady_clock::time_point sent_at_;  // when it was sent
  std::chrono::steady_clock::time_point started_;
  mutable uint64_t heartbeat_seen_ = 0u;
  mutable std::chrono::steady_clock::time_point heartbeat_time_;
};

}  // namespace uplift::ipc
