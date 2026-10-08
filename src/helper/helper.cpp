#include "helper/helper.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "addon/environment.hpp"
#include "addon/frame_config.hpp"
#include "addon/frame_trigger.hpp"
#include "ipc/control_block.hpp"
#include "ipc/settings_text.hpp"
#include "nr/session_status.hpp"
#include "ui/status_text.hpp"

#ifdef UPLIFT_HELPER_IDENTITY_NR
#include "testing/identity_host.hpp"
#endif

namespace uplift::helper {
namespace {

constexpr std::chrono::seconds IDLE_EXIT{30};  // design §2.2: no request at all for this long ends the helper
constexpr DWORD LOOP_WAIT_MS = 100u;           // the request wait, and the heartbeat's period
// How long after it began a handler that can enter NR's runtime keeps the heartbeat moving: far above a cold NGX load, and
// what bounds finding a helper stuck inside one (this plus the add-on's 20 s).
constexpr std::chrono::seconds LONG_HANDLER_BEAT{120};

void RingSink(nr::LogLevel level, std::string_view message, void* user) {
  std::string line;
  line.reserve(message.size() + 1u);
  line.push_back(static_cast<char>('0' + static_cast<int>(level)));
  line.append(message);
  ipc::LogWrite(&static_cast<ipc::ControlBlock*>(user)->log, line);
}

int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Heartbeat::Heartbeat(ipc::ControlBlock* block) : block_(block), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
  thread_ = std::thread([this] {
    while (WaitForSingleObject(stop_, LOOP_WAIT_MS) == WAIT_TIMEOUT) {
      if (const int64_t until = beat_until_.load(); until != 0 && NowMilliseconds() < until) {
        Beat();
      }
    }
  });
}

Heartbeat::~Heartbeat() {
  SetEvent(stop_);
  thread_.join();
  CloseHandle(stop_);
}

// The add-on's process reads the counter with an interlocked read; it is 8-aligned (a static_assert in protocol.hpp).
void Heartbeat::Beat() {
  InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&block_->heartbeat));
}

void Heartbeat::BeginLongHandler(std::chrono::milliseconds limit) {
  beat_until_.store(NowMilliseconds() + limit.count());
}

void Heartbeat::EndLongHandler() {
  beat_until_.store(0);
}

void InstallLogSink(ipc::ControlBlock* block, nr::LogLevel max_level) {
  nr::SetLogSink(&RingSink, block, max_level);
}

Helper::Helper(ipc::ControlBlock* block, Events events, std::unique_ptr<bridge::D3D12Side> side)
    : block_(block), events_(events), side_(std::move(side)), heartbeat_(block) {}

Helper::~Helper() {
  Teardown();
}

void Helper::Teardown() {
  if (transport_) {
    transport_->Detach();  // before the reset: releases what the queue may still wait on, and retires what it may still read
    transport_.reset();
  }
  if (context_) {
    context_->Teardown();  // NR's runtime unloads here (a bounded wait); the process exit returns the rest
  }
}

int Helper::Loop() {
  auto last_request = std::chrono::steady_clock::now();
  uint64_t last_progress = side_->Completed();
  for (;;) {
    const DWORD waited = WaitForSingleObject(events_.request, LOOP_WAIT_MS);
    if (const uint64_t progress = side_->Completed(); progress != last_progress) {
      last_progress = progress;
      heartbeat_.Beat();
    }
    if (waited == WAIT_TIMEOUT) {
      if (std::chrono::steady_clock::now() - last_request >= IDLE_EXIT) {
        nr::Log(nr::LogLevel::INFO, "no request for 30 s; the helper exits");
        Teardown();
        return 0;
      }
      continue;
    }
    if (waited != WAIT_OBJECT_0) return 1;
    last_request = std::chrono::steady_clock::now();
    const ipc::Request request = block_->request;
    if (request.kind == ipc::RequestKind::NONE) continue;
    block_->reply = {};
    ipc::Reply& reply = block_->reply;
    reply.sequence = request.sequence;
    bool deferred = false;
    if (request.kind == ipc::RequestKind::ATTACH || request.kind == ipc::RequestKind::FRAME
        || request.kind == ipc::RequestKind::RUN) {
      heartbeat_.BeginLongHandler(LONG_HANDLER_BEAT);  // these can enter NR's runtime, whose cold load takes more than 20 s
    }
    try {
      switch (request.kind) {
        case ipc::RequestKind::ATTACH:
          Attach(request, &reply);
          break;
        case ipc::RequestKind::FRAME:
          Frame(request, &reply);
          break;
        case ipc::RequestKind::RUN:
          Run(request, &reply, &deferred);
          break;
        case ipc::RequestKind::SHARE_MASK:
        case ipc::RequestKind::SHARE_MOTION:
          if (transport_) {
            transport_->Share(request.kind, request.share, &reply);
          } else {
            ipc::CopyText(reply.error, "not attached");
          }
          break;
        case ipc::RequestKind::STOP:
          if (transport_) {
            transport_->Stop(request.stop.waited11, ipc::TextOf(request.stop.reason));
          }
          reply.ok = 1u;
          break;
        case ipc::RequestKind::DETACH:
          if (transport_) {
            transport_->Detach();
            transport_.reset();
          }
          reply.ok = 1u;
          break;
        case ipc::RequestKind::QUIT:
          reply.ok = 1u;
          break;
        case ipc::RequestKind::NONE:
          break;
      }
    } catch (...) {
      // Never let a request take the helper down: the add-on sees the failure and decides.
      reply.ok = 0u;
      ipc::CopyText(reply.error, "the helper threw while handling the request");
      nr::Log(nr::LogLevel::ERR, "the helper threw while handling a request");
      deferred = false;
    }
    heartbeat_.EndLongHandler();
    if (transport_) {
      // Protocol 5: the fences' completed values ride with every reply (a deferred one included: it is complete before the GPU sets the event), written
      // before the event is set.
      transport_->FenceValues(&reply.to12_completed, &reply.to11_completed);
    }
    if (!deferred) {
      SetEvent(events_.reply);
    }
    heartbeat_.Beat();
    if (request.kind == ipc::RequestKind::QUIT) {
      Teardown();  // after the reply: the add-on does not wait for the unload
      return 0;
    }
  }
}

void Helper::Attach(const ipc::Request& request, ipc::Reply* reply) {
  const ipc::Attach& attach = request.attach;
  if (attach.luid_low != block_->luid_low || attach.luid_high != block_->luid_high) {
    ipc::CopyText(reply->error, "the game moved to another adapter");
    return;
  }
  if (!context_) {
    const std::string_view snippet = ipc::TextOf(attach.snippet_path);
    const std::optional<std::filesystem::path> snippet_path = (snippet.empty() ? std::nullopt : addon::PathFromUtf8(snippet));
    const std::optional<std::filesystem::path> data_path = addon::PathFromUtf8(ipc::TextOf(attach.ngx_data_directory));
    snippet_found_ = snippet_path.has_value();
    std::string error;
    std::unique_ptr<nr::Host> host;
#ifdef UPLIFT_HELPER_IDENTITY_NR
    host = std::make_unique<testing::IdentityHost>();
#endif
    context_ = addon::DeviceContext::Create(
        side_->Device(),
        {.snippet_path = snippet_path.value_or(std::filesystem::path()),
         .application_data_path = data_path.value_or(std::filesystem::path())},
        &error,
        {.bridged = true, .host = std::move(host), .game_memory = [this] { return last_game_memory_; }});
    if (!context_) {
      ipc::CopyText(reply->error, error);
      return;
    }
    context_->SetDlssUnavailableReason("");  // the bridged reason holds anyway
#ifdef UPLIFT_HELPER_IDENTITY_NR
    snippet_found_ = true;  // the stand-in has no runtime file to find
#else
    if (!snippet_found_) {
      context_->SetBlockedReason(std::format("nvngx_dlssnr.dll not found next to {} or the game; copy it there or set Runtime path",
                                             ipc::TextOf(attach.addon_file)));
    }
#endif
  }
  if (transport_) {
    transport_->Detach();  // an ATTACH without a DETACH first replaces the transport
    transport_.reset();
  }
  std::string error;
  transport_ = MakeTransport(attach.transport, *side_, &error);
  if (!transport_) {
    ipc::CopyText(reply->error, error);
    return;
  }
  const char* const api = (attach.api == ipc::Api::D3D9      ? "Direct3D 9"
                           : attach.api == ipc::Api::D3D11   ? "Direct3D 11"
                           : attach.api == ipc::Api::D3D10   ? "Direct3D 10"
                           : attach.api == ipc::Api::VULKAN  ? "Vulkan"
                           : attach.api == ipc::Api::OPENGL  ? "OpenGL"
                           : attach.api == ipc::Api::D3D12   ? "Direct3D 12"
                                                             : "an unknown API");
  nr::Logf(nr::LogLevel::INFO, "attached: {} ({})", transport_->Line(), api);
  reply->ok = 1u;
}

void Helper::ApplySettings(uint64_t generation) {
  std::vector<std::string> warnings;
  settings_ = ui::LoadSettings(ipc::TextConfigStore::Parse(ipc::TextOf(block_->settings_text)), &warnings);
  for (const std::string& warning : warnings) {
    nr::Logf(nr::LogLevel::WARN, "settings: {}", warning);
  }
  // What addon.cpp's ApplyProcessSettings applies in the process that loads NR: the log level, and the NGX runtime's
  // indicator, which it reads when the snippet next loads.
  InstallLogSink(block_, settings_.log_level);
  SetEnvironmentVariableW(L"__NGX_SHOW_INDICATOR", (settings_.show_nv_indicator ? L"1024" : nullptr));
  settings_generation_ = generation;
  settings_loaded_ = true;
}

void Helper::ReplayPoint(uint32_t point) {
  switch (static_cast<addon::TriggerPoint>(point)) {
    case addon::TriggerPoint::MARKER:
      context_->OnTechnique(true);
      break;
    case addon::TriggerPoint::AFTER_EFFECTS:
      context_->OnFinishEffects();
      break;
    case addon::TriggerPoint::NONE:
    case addon::TriggerPoint::PRESENT:
      break;
  }
}

void Helper::Frame(const ipc::Request& request, ipc::Reply* reply) {
  if (!context_ || !transport_) {
    ipc::CopyText(reply->error, "not attached");
    return;
  }
  const ipc::Frame& frame = request.frame;
  ReplayPoint(frame.unreported_point);
  if (!settings_loaded_ || frame.settings_generation != settings_generation_) {
    ApplySettings(frame.settings_generation);
  }
  if (frame.retry_now != 0u) {
    context_->RetryNow();
  }
  if (frame.game_budget != 0u) {
    last_game_memory_ = nr::MemoryInfo{.budget = frame.game_budget, .usage = frame.game_usage};
  }
  if (frame.mask_running == 0u) {
    // No enabled effect writes UPLIFT_MASK, so a mask whose effect stopped is neither bound nor kept.
    context_->PrepareMaskCopy(nullptr);
    transport_->ReleaseMask();
  }
  const bool running = (context_->NrState() != nr::SessionState::OFF);
  const auto now = std::chrono::steady_clock::now();
  // 1.0.1 (F2): a line a minute while NR runs here, from the context's count of NR recordings (cheap accessors, no Status), and the figures once more
  // when this helper's device is found removed (by BeginFrame below, or earlier). The memory is the helper's own: NR lives in this process.
  const auto log_figures = [this](std::string_view lead, const addon::NrHeartbeat::Figures& figures) {
    nr::Log(nr::LogLevel::INFO, addon::HeartbeatLine(lead, figures, addon::ProcessPrivateMiB(), nr_vram_.MiB(side_->Device()->GetAdapterLuid())));
  };
  if (const std::optional<addon::NrHeartbeat::Figures> figures = nr_heartbeat_.Note(context_->NrRecordings(), context_->LatestMotionSource(), now)) {
    log_figures("NR running:", *figures);
  }
  const addon::TargetInfo target = transport_->Describe(frame.target, settings_.enabled, running, &reply->handles);
  addon::FrameConfig config = addon::BuildFrameConfig(
      {
          .settings = &settings_,
          .drag = ipc::DragFrom(frame.drag_bits),
          .defaults_view = (frame.defaults_view != 0u),
          .ngx_frame_generation = 0u,
          .nr_allowed = (frame.nr_allowed != 0u),
          .settings_generation = settings_generation_,
          .uplift_mv_lumenite = (frame.uplift_mv_lumenite != 0u),  // 2026-10-08: the readouts name UPLIFT_MV's source
      },
      &coalescer_, now);
  if (frame.drain_now != 0u) {
    config.session.grace = std::chrono::milliseconds(0);  // transitions: NR's owner moved away from the helper: it drains at once, never after the grace
  }
  context_->BeginFrame(side_->Queue(), config, target, frame.marker_expected != 0u, now);
  if (context_->DeviceLost()) {
    if (const std::optional<addon::NrHeartbeat::Figures> figures = nr_heartbeat_.Stop(now)) {
      log_figures("NR stopped after", *figures);
    }
  }
  reply->ok = 1u;
  reply->frame_ready = context_->FrameReady() ? 1u : 0u;
  reply->running = (context_->NrState() != nr::SessionState::OFF) ? 1u : 0u;
  FillStatus(&reply->status);
}

void Helper::Run(const ipc::Request& request, ipc::Reply* reply, bool* deferred) {
  if (!context_ || !transport_) {
    ipc::CopyText(reply->error, "not attached");
    return;
  }
  ReplayPoint(request.run.point);
  uint64_t complete_at = 0u;
  transport_->Run(request.run, *context_, reply, &complete_at);
  if (reply->busy != 0u) {
    ++busy_skips_;
  }
  // The status the add-on shows is this RUN's outcome (nr_applied, the skip reason): BeginFrame's copy predates it.
  FillStatus(&reply->status);
  if (complete_at == 0u) return;
  // BLOCK and KMT: the reply is complete, so the runtime may set the event once the GPU has passed the signal. That is the
  // only place that sets it for this request (`deferred`), so it is set exactly once.
  if (SUCCEEDED(side_->Progress()->SetEventOnCompletion(complete_at, events_.reply))) {
    *deferred = true;
  } else {
    reply->wrote = 0u;  // nothing would tell the add-on when the GPU is done: it must not read the block
  }
}

void Helper::FillStatus(ipc::Status* out) const {
  const addon::ContextStatus status = context_->Status();
  const nr::SessionStatus& session = status.session;
  out->intermediate_bytes = status.intermediate_bytes + transport_->SurfaceBytes();
  out->runtime_bytes = session.runtime_bytes.value_or(0u);
  out->has_runtime_bytes = session.runtime_bytes.has_value() ? 1u : 0u;
  out->busy_skips = busy_skips_;
  out->grace_remaining_ms = session.grace_remaining.count();
  out->retry_in_ms = session.retry_in ? session.retry_in->count() : -1;
  out->session_state = static_cast<uint32_t>(session.state);
  out->suspended = session.suspended ? 1u : 0u;
  out->passes_requested = session.passes_requested;
  out->passes_run = status.passes_run;
  out->retries_exhausted = session.retries_exhausted ? 1u : 0u;
  out->device_lost = status.device_lost ? 1u : 0u;
  out->nr_applied = status.nr_applied ? 1u : 0u;
  out->skip_from_session = status.skip_from_session ? 1u : 0u;
  out->blocked_stage = static_cast<uint32_t>(snippet_found_ ? ui::CardStage::CONFLICTS : ui::CardStage::RUNTIME);
  out->trigger = static_cast<uint32_t>(status.trigger);
  out->diffuse_white_nits = status.diffuse_white_nits;
  // Plan 14 (protocol 6): what the add-on's Setup and status card read about the helper's context.
  out->placement = static_cast<uint32_t>(status.placement);
  out->motion_source = static_cast<uint32_t>(status.motion_source);
  out->dlss_motion_gap = static_cast<uint32_t>(status.dlss_motion_gap);
  out->launchpad_gap = static_cast<uint32_t>(status.launchpad_gap);
  out->resolution_applied = static_cast<uint32_t>(status.resolution_applied);
  out->upsampling = static_cast<uint32_t>(status.upsampling);
  out->work_width = status.work.width;
  out->work_height = status.work.height;
  out->canvas_width = status.canvas.width;
  out->canvas_height = status.canvas.height;
  out->frame_width = status.frame.width;
  out->frame_height = status.frame.height;
  ipc::CopyText(out->session_message, session.message);
  ipc::CopyText(out->blocked, status.blocked);
  ipc::CopyText(out->output_problem, status.output_problem);
  ipc::CopyText(out->placement_note, status.placement_note);
  ipc::CopyText(out->skip_reason, status.skip_reason);
  // As addon.cpp's OnDrawOverlay builds it, so the card's "working" line and the overlay's status line are the 64-bit ones.
  ipc::CopyText(out->status_line,
                ui::FormatStatusLine({
                    .state = session.state,
                    .suspended = session.suspended,
                    .passes_requested = session.passes_requested,
                    .passes_run = status.passes_run,
                    .frame = status.frame,
                    .trigger = (status.trigger == addon::TriggerPoint::NONE ? std::string_view()
                                                                            : addon::TriggerPointName(status.trigger)),
                    .encoding = status.encoding,
                    .diffuse_white_nits = status.diffuse_white_nits,
                    .grace_remaining = session.grace_remaining,
                }));
  ipc::CopyText(out->placement_line, status.placement_line);
  ipc::CopyText(out->motion_line, status.motion_line);
  ipc::CopyText(out->work_line, status.work_line);
  ipc::CopyText(out->exposure_line, status.exposure_line);  // Plan 17
  ipc::CopyText(out->ui_correction_note, status.ui_correction_note);
  ipc::CopyText(out->transport_line, transport_->Line());
}

}  // namespace uplift::helper
