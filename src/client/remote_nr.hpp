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
#include <utility>

#include "client/nr_link.hpp"
#include "ipc/helper_launcher.hpp"
#include "ipc/protocol.hpp"

namespace uplift::client {

struct RemoteNrConfig {
  std::filesystem::path helper_exe;  // <add-on folder>\gitc-uplift-helper64.exe
  std::string build_id;              // UPLIFT_BUILD_ID
  ipc::Attach attach;                // LUID, API, transport, snippet path, NGX data folder, add-on file name
};

// Plan 9 (design §2.2, §2.7): the helper's lifetime and the per-present exchange. Not thread-safe: the add-on's lock.
//
// Every CPU wait here is capped (FRAME 100 ms, RUN 2 s, the rest 100 ms or 2 s), so the game thread is never blocked
// for long. One request is outstanding at a time: a reply that lands late is taken at the next present, and until then nothing
// else is sent (and the clients touch no shared memory).
//
// The helper never restarts by itself after a failure: Failure() stays until RetryNow(). It restarts after a normal exit (a
// QUIT, or its 30 s idle exit), when NR is turned on again.
class RemoteNr final : public NrLink {
 public:
  explicit RemoteNr(RemoteNrConfig config);
  RemoteNr(const RemoteNr&) = delete;
  RemoteNr& operator=(const RemoteNr&) = delete;

  // The ATTACH the next (re)attach sends, and the adapter a helper is started for (a running helper on another adapter is
  // replaced). Called at every present before Present(): the game's device may have been replaced (a D3D9 Reset).
  void SetAttach(const ipc::Attach& attach) { config_.attach = attach; }

  // First thing at every present of the claimed device.
  // - Starts the helper when `enabled` and none runs, unless it failed (then only RetryNow does).
  // - Drains its log into `log` (one line per call: its level as an ASCII digit, then the message), detects its exit or hang,
  //   takes a late reply, and ATTACHes once READY.
  // - Sends FRAME (100 ms cap) and returns its reply; nullopt when there is none this frame (starting, pending, failed).
  //   The settings text goes into the block when `frame.settings_generation` changed, only while no request is pending.
  // - Sends QUIT once `enabled` is false and the last status, taken under the current settings, said the Session is OFF.
  // The returned reply's handles include those of a reply that landed late (each NT handle is handed out exactly once, in the
  // reply of the request that made it, so a late one would be lost otherwise).
  std::optional<ipc::Reply> Present(bool enabled, const ipc::Frame& frame, std::string_view settings_text,
                                    const std::function<void(std::string_view)>& log);
  // The last Present sent a FRAME (answered or not): the helper has seen `frame.unreported_point`.
  [[nodiscard]] bool FrameSent() const { return frame_sent_; }
  void Detach();                                                       // DETACH (destroy_device, D3D9 Reset); the next Present re-ATTACHes
  void RetryNow();                                                     // after a failure: forget it, so the next Present starts a fresh helper
  // Plan 17: ends the helper as a failure with `text` for the card (its log drained first): its own Direct3D 12 device was removed, so a fresh helper is
  // the only way NR runs again (Retry now).
  void Abort(std::string text) { Fail(std::move(text)); }
  void Quit();                                                         // AddonUninit: QUIT, then close (kill-on-close ends a helper that does not exit)
  [[nodiscard]] bool Running() const { return launcher_ != nullptr; }  // a helper process exists (until it has exited)
  [[nodiscard]] bool Attached() const { return attached_; }
  [[nodiscard]] std::string_view Failure() const { return failure_; }  // "It exited with code 0x...", ...; empty while healthy
  [[nodiscard]] const ipc::Status& LastStatus() const { return last_status_; }
  // Helper hand-over round: the helper's NR may be loaded. True from the first FRAME sent with nr_allowed until a FRAME sent without it is answered with
  // its Session OFF (no FRAME since could load it again), or the helper process is gone. A status that is stale, missing or still pending never clears it.
  [[nodiscard]] bool NrMayRun() const { return nr_may_run_; }
  // The helper's PID: the current one, else the last (0 before the first start).
  [[nodiscard]] DWORD Pid() const { return pid_; }
  // Presents that could not send a FRAME because a request was still outstanding (the helper was busy) while its Session was ACTIVE:
  // the loads that answer late by design are not counted.
  [[nodiscard]] uint64_t BusyFrames() const { return busy_frames_; }

  // NrLink:
  std::optional<ipc::Reply> Run(const ipc::Run& run) override;
  [[nodiscard]] bool RunSent() const override { return run_sent_; }
  std::optional<bool> TakeLateRunSignalled() override { return std::exchange(late_run_signalled_, std::nullopt); }
  std::optional<ipc::Reply> Share(ipc::RequestKind kind, const ipc::Share& share) override;
  bool Pull(uint64_t remote, HANDLE* local) override;
  void Stop(uint64_t waited11, std::string_view reason) override;

 private:
  // What the outstanding request (one the caller stopped waiting for) was, for its reply when it lands.
  struct Outstanding {
    ipc::RequestKind kind = ipc::RequestKind::NONE;
    uint64_t generation = 0u;  // the settings generation it was sent under (FRAME)
    bool nr_allowed = false;   // a FRAME's nr_allowed: one sent without it that answers OFF proves the helper's NR unloaded
  };

  // Send + wait `cap_ms`: the reply when it came in time; nullopt when it is pending (recorded) or the helper is gone.
  std::optional<ipc::Reply> Transact(const ipc::Request& request, uint32_t cap_ms);
  // A reply's effect on this side. `late`: it landed after its request's cap, so nobody holds its handles yet.
  void Consume(const Outstanding& request, const ipc::Reply& reply, bool late);
  void Fail(std::string text);  // kills the helper, keeps `text` for the card, forgets nothing else
  void Kill();                  // ends the helper and waits (capped) for its process to go, so its NR's memory is released before anything loads
  void Forget();                // the helper is gone: drops its per-process state
  void DiscardRemote(uint64_t remote);
  void DiscardLate();  // closes and forgets the handles in late_
  void DrainLog();

  RemoteNrConfig config_;
  std::function<void(std::string_view)> log_;
  std::unique_ptr<ipc::HelperLauncher> launcher_;
  DWORD pid_ = 0u;
  LUID launched_for_ = {};
  bool announced_ = false;  // "helper started" logged for this helper
  bool attached_ = false;
  bool needs_detach_ = false;       // Detach() could not send: a request was outstanding
  bool stale_outstanding_ = false;  // the outstanding request predates Detach(): its reply's handles belong to the dropped transport
  bool quit_sent_ = false;
  std::chrono::steady_clock::time_point quit_at_;
  bool failed_ = false;
  std::string failure_;
  Outstanding outstanding_;
  bool frame_sent_ = false;
  uint64_t busy_frames_ = 0u;
  ipc::Status last_status_;
  uint64_t status_generation_ = 0u;  // the settings generation the last status was produced under
  uint64_t frame_generation_ = 0u;   // the settings generation of the newest FRAME sent (a RUN follows its FRAME's)
  bool have_status_ = false;
  bool nr_may_run_ = false;  // NrMayRun()
  uint64_t written_generation_ = 0u;
  bool text_written_ = false;
  ipc::Handles late_;                       // handles of replies that landed late, for the next FRAME reply
  bool run_sent_ = false;                   // the last Run() put its request on the wire
  std::optional<bool> late_run_signalled_;  // `signalled` of a RUN reply that landed late, until a client takes it
  std::string last_error_;                  // the last FRAME error logged (once per text)
};

}  // namespace uplift::client
