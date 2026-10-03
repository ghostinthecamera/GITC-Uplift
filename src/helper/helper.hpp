#pragma once

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "addon/device_context.hpp"
#include "addon/nr_heartbeat.hpp"
#include "bridge/d3d12_side.hpp"
#include "helper/transports.hpp"
#include "ipc/protocol.hpp"
#include "nr/budget.hpp"
#include "nr/log.hpp"
#include "ui/controls_coalescer.hpp"
#include "ui/settings.hpp"

namespace uplift::helper {

// Routes nr:: logging into the control block's log ring (the add-on re-logs each line as "helper: <message>"), up to
// `max_level`. Each record is the level as an ASCII digit, then the message.
void InstallLogSink(ipc::ControlBlock* block, nr::LogLevel max_level);

// The control block's heartbeat. Beat() is the loop's own (a request finished, the GPU moved). A handler that can enter NR's
// runtime (ATTACH, FRAME, RUN) may run long: a cold NGX load takes more than the add-on's 20 s "hung" limit. Around such a
// handler this class's thread beats every 100 ms, but only up to `limit` after the handler began, so a helper that is truly
// stuck in one is still found (`limit` + 20 s), and one stuck in any other handler after 20 s, since nothing beats for those.
class Heartbeat {
 public:
  explicit Heartbeat(ipc::ControlBlock* block);
  ~Heartbeat();
  Heartbeat(const Heartbeat&) = delete;
  Heartbeat& operator=(const Heartbeat&) = delete;

  void Beat();
  void BeginLongHandler(std::chrono::milliseconds limit);
  void EndLongHandler();

 private:
  ipc::ControlBlock* block_;
  HANDLE stop_;
  std::atomic<int64_t> beat_until_{0};  // steady_clock milliseconds; 0 = no long handler is running
  std::thread thread_;
};

// The two inherited events: the add-on sets `request`, and this side sets `reply` exactly once per request.
struct Events {
  HANDLE request = nullptr;
  HANDLE reply = nullptr;
};

// Plan 9 (design §2.1, §2.2): the helper's request loop. It hosts addon::DeviceContext unchanged (bridged) on the D3D12Side
// the process made, and a Transport for the API the add-on attached. One thread; the add-on has at most one request
// outstanding.
class Helper {
 public:
  Helper(ipc::ControlBlock* block, Events events, std::unique_ptr<bridge::D3D12Side> side);
  ~Helper();
  Helper(const Helper&) = delete;
  Helper& operator=(const Helper&) = delete;

  // Serves requests until QUIT (0) or 30 s with no request at all (0); 1 when the request event fails.
  int Loop();

 private:
  void Attach(const ipc::Request& request, ipc::Reply* reply);
  void Frame(const ipc::Request& request, ipc::Reply* reply);
  void Run(const ipc::Request& request, ipc::Reply* reply, bool* deferred);
  // The FrameTrigger's two events the add-on reached, applied to this side's copy so the two never diverge (design §2.1):
  // MARKER -> OnTechnique(true), AFTER_EFFECTS -> OnFinishEffects(); PRESENT and NONE change nothing.
  void ReplayPoint(uint32_t point);
  void ApplySettings(uint64_t generation);
  void FillStatus(ipc::Status* status) const;
  void Teardown();

  ipc::ControlBlock* block_;
  Events events_;
  // Declared in this order so they go the other way: the transport before the context, the context before its device.
  std::unique_ptr<bridge::D3D12Side> side_;
  std::unique_ptr<addon::DeviceContext> context_;
  std::unique_ptr<Transport> transport_;
  ui::Settings settings_;
  uint64_t settings_generation_ = 0u;
  bool settings_loaded_ = false;
  ui::ControlsCoalescer coalescer_;
  std::optional<nr::MemoryInfo> last_game_memory_;  // the add-on's figures, for RealHost::QueryMemory (design §2.10)
  bool snippet_found_ = false;
  uint64_t busy_skips_ = 0u;
  // 1.0.1 (F2): the once-a-minute "NR running" line and the "NR stopped after" one, with this (the helper's) process's private bytes and VRAM.
  addon::NrHeartbeat nr_heartbeat_;
  addon::ProcessVram nr_vram_;
  Heartbeat heartbeat_;  // declared last: its thread stops first
};

}  // namespace uplift::helper
