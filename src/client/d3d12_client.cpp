#include "client/d3d12_client.hpp"

#include <dxgi1_4.h>

#include <chrono>
#include <format>
#include <limits>
#include <utility>

#include "addon/environment.hpp"
#include "color/encoding.hpp"
#include "ipc/control_block.hpp"
#include "nr/log.hpp"

namespace uplift::client {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD DESTROY_WAIT_MS = 2000u;  // the design's R29 cap on the waits that can stall the game thread

std::string ShareFailureText(const char* what, DXGI_FORMAT format, HRESULT result) {
  return std::format("{} could not be shared with Uplift's helper (DXGI_FORMAT {}, HRESULT {:#010x})", what, static_cast<int>(format),
                     static_cast<uint32_t>(result));
}

}  // namespace

std::unique_ptr<D3D12Client> D3D12Client::Create(ID3D12Device* device, std::string* error) {
  const auto fail = [error](std::string text) {
    *error = std::move(text);
    return std::unique_ptr<D3D12Client>();
  };
  const LUID luid = device->GetAdapterLuid();
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adapter_description = {};
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))
      || FAILED(adapter->GetDesc(&adapter_description))) {
    return fail("the game's adapter could not be read");
  }
  // WARP and other vendors are refused, as on every other API; cross-adapter sharing is out of scope.
  if (adapter_description.VendorId != addon::NVIDIA_VENDOR_ID) {
    return fail("Uplift needs an NVIDIA GPU; this game renders on another adapter");
  }
  std::unique_ptr<D3D12Client> client(new D3D12Client());
  client->device_ = device;
  client->luid_ = luid;
  if (const HRESULT result = device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&client->retire_fence_)); FAILED(result)) {
    return fail(std::format("a Direct3D 12 fence could not be made (HRESULT {:#010x})", static_cast<uint32_t>(result)));
  }
  return client;
}

D3D12Client::~D3D12Client() {
  DestroyDevice(/*wait=*/false);  // reached with a device still held only when the game ended with no destroy_device: the process is going, so no 2 s wait here
}

void D3D12Client::Retire(D3D12Host& host, std::vector<ComPtr<IUnknown>> objects) {
  if (host.Valid() && retire_fence_ != nullptr) {
    for (ComPtr<IUnknown>& waiting : unsignalled_) {
      objects.push_back(std::move(waiting));
    }
    unsignalled_.clear();
  }
  std::erase_if(objects, [](const ComPtr<IUnknown>& object) { return object == nullptr; });
  if (objects.empty()) return;
  if (!host.Valid() || retire_fence_ == nullptr) {
    // No queue to signal on (ReShade gave this one no immediate list): what the queue may still use waits for a valid host.
    for (ComPtr<IUnknown>& object : objects) {
      unsignalled_.push_back(std::move(object));
    }
    return;
  }
  const uint64_t value = ++retire_value_;
  host.Signal(retire_fence_.Get(), value);  // a refused signal leaves the value unreached: the objects are held, never released under the queue
  for (ComPtr<IUnknown>& object : objects) {
    retired_.push_back({.object = std::move(object), .value = value});
  }
}

void D3D12Client::RetireShared(D3D12Host& host, std::initializer_list<Shared*> surfaces) {
  std::vector<ComPtr<IUnknown>> objects;
  for (Shared* const surface : surfaces) {
    if (surface->resource != nullptr) {
      objects.emplace_back(surface->resource.Get());
    }
    *surface = {};
  }
  mask_fresh_ = (mask_fresh_ && mask_.resource != nullptr);  // only a retired mask loses its freshness
  Retire(host, std::move(objects));
}

void D3D12Client::FreeFinished(D3D12Host& host) {
  if (host.Valid() && !unsignalled_.empty()) {
    Retire(host, {});  // gives the objects that waited for a queue their retire value
  }
  if (retire_fence_ == nullptr || retired_.empty()) return;
  const uint64_t completed = retire_fence_->GetCompletedValue();  // UINT64_MAX once the device was removed: everything is free
  std::erase_if(retired_, [completed](const Retired& item) { return item.value <= completed; });
}

void D3D12Client::DestroyDevice(bool wait) {
  if (device_ == nullptr) return;
  // The game's queue is idle by now (ReShade waited for it when the swap chain went), so what is held and what was retired can go; a retire value the fence has
  // not reached means it is not, and nothing is released under it: the objects are leaked. Without `wait` (no destroy_device came, so nothing says the queue was
  // waited for) the fence is only read, a value it has not reached leaks at once, and so does whatever is still held (no fence covers it): the process is ending.
  bool idle = true;
  if (retire_fence_ != nullptr && retire_value_ > retire_fence_->GetCompletedValue()) {
    idle = false;
    if (wait) {
      const HANDLE reached_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      idle = (reached_event != nullptr && SUCCEEDED(retire_fence_->SetEventOnCompletion(retire_value_, reached_event))
              && WaitForSingleObject(reached_event, DESTROY_WAIT_MS) == WAIT_OBJECT_0);
      if (reached_event != nullptr) {
        CloseHandle(reached_event);
      }
    }
  }
  if (!unsignalled_.empty()) {
    idle = false;  // never signalled on a queue: nothing says the GPU is done with them
  }
  if (!wait && (color_.resource || mask_.resource || motion_.resource || to12_ || to11_)) {
    idle = false;  // still held, and the queue was not waited for
  }
  if (!idle) {
    nr::Log(nr::LogLevel::WARN, "the game's Direct3D 12 queue did not finish with Uplift's shared textures; they are left until the process ends");
    for (Retired& item : retired_) {
      item.object.Detach();
    }
    for (ComPtr<IUnknown>& object : unsignalled_) {
      object.Detach();
    }
    color_.resource.Detach();
    mask_.resource.Detach();
    motion_.resource.Detach();
    to12_.Detach();
    to11_.Detach();
  }
  retired_.clear();
  unsignalled_.clear();
  color_ = {};
  mask_ = {};
  motion_ = {};
  to12_.Reset();
  to11_.Reset();
  retire_fence_.Reset();
  device_.Reset();  // inside destroy_device, before ReShade releases its own reference
}

void D3D12Client::ReleaseMask(D3D12Host& host) {
  RetireShared(host, {&mask_});
}

bool D3D12Client::ReleaseAll(D3D12Host& host) {
  const bool held = (color_.resource || mask_.resource || motion_.resource || to12_ || to11_);
  std::vector<ComPtr<IUnknown>> fences;
  fences.emplace_back(to12_.Get());
  fences.emplace_back(to11_.Get());
  to12_.Reset();
  to11_.Reset();
  RetireShared(host, {&color_, &mask_, &motion_});
  // Work queued on this side may still wait on a fence the helper's exit has just released (it reads UINT64_MAX, so the wait passes): the fences go behind the
  // retire fence too, never under a submission.
  Retire(host, std::move(fences));
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

void D3D12Client::Stop(std::string_view reason, NrLink& link, D3D12Host& host) {
  if (latch_.empty()) {
    latch_ = std::format("The Direct3D 12 bridge stopped: {}. Restart the game to use NR again", reason);
    nr::Log(nr::LogLevel::ERR, latch_);
  }
  link.Stop(waited11_, latch_);
  RetireShared(host, {&color_, &mask_, &motion_});
}

bool D3D12Client::Open(D3D12Host& host, HANDLE handle, Shared* shared, HRESULT* result) {
  Shared opened;
  *result = device_->OpenSharedHandle(handle, IID_PPV_ARGS(&opened.resource));
  if (FAILED(*result)) return false;
  const D3D12_RESOURCE_DESC description = opened.resource->GetDesc();
  opened.size = {static_cast<uint32_t>(description.Width), description.Height};
  opened.format = description.Format;
  RetireShared(host, {shared});  // what it replaces may still be in the game's queue: behind the retire fence, never released at once
  *shared = std::move(opened);
  return true;
}

uint64_t D3D12Client::HeldBytes() const {
  uint64_t bytes = 0u;
  const auto add = [this, &bytes](IUnknown* object) {
    ComPtr<ID3D12Resource> resource;
    if (object != nullptr && device_ != nullptr && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
      const D3D12_RESOURCE_DESC description = resource->GetDesc();
      bytes += device_->GetResourceAllocationInfo(0u, 1u, &description).SizeInBytes;
    }
  };
  add(color_.resource.Get());
  add(mask_.resource.Get());
  add(motion_.resource.Get());
  for (const Retired& item : retired_) {
    add(item.object.Get());
  }
  for (const ComPtr<IUnknown>& object : unsignalled_) {
    add(object.Get());
  }
  return bytes;
}

ipc::Target D3D12Client::Describe(D3D12Host& host, const D3D12_RESOURCE_DESC& back_buffer, bool enabled, bool running) {
  FreeFinished(host);
  ipc::Target target;
  const bool texture = (back_buffer.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
  const nr::Size size = (texture ? nr::Size{static_cast<uint32_t>(back_buffer.Width), back_buffer.Height} : nr::Size{});
  const DXGI_FORMAT format = (texture ? back_buffer.Format : DXGI_FORMAT_UNKNOWN);
  target.width = size.width;
  target.height = size.height;
  target.dxgi_format = static_cast<uint32_t>(format);
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
  }
  std::string problem;
  if (!latch_.empty()) {
    problem = latch_;
  } else if (!texture) {
    problem = "The back buffer is not a 2D texture";
  } else if (back_buffer.SampleDesc.Count != 1u) {
    problem = "Multisampled back buffers are not supported on Direct3D 12";
  } else if (!color::DescribeFormat(format)) {
    problem = std::format("Unsupported back-buffer format (DXGI_FORMAT {})", static_cast<int>(format));
  }
  if (!running || !problem.empty()) {
    RetireShared(host, {&color_, &mask_, &motion_});  // NR is off, or this image cannot go: the shared surfaces go with it (Plan 7's final review C-1)
  }
  if (problem.empty() && color_failure_ && color_failure_->Matches(size, format)) {
    problem = ShareFailureText("The back buffer", format, color_failure_->result);
  }
  if (problem != problem_) {
    if (!problem.empty()) {
      nr::Log(nr::LogLevel::WARN, problem);
    }
    problem_ = std::move(problem);
  }
  ipc::CopyText(target.problem, problem_);
  frame_ask_ = {.size = size, .format = format};
  frame_asks_colour_ = problem_.empty();
  return target;
}

void D3D12Client::NoteFrameSent() {
  if (frame_asks_colour_) {
    color_ask_ = frame_ask_;
  }
}

void D3D12Client::Apply(const ipc::Reply& reply, NrLink& link, D3D12Host& host) {
  in_flight_ = false;  // a FRAME reply means every earlier request has replied
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
  if (to12 != nullptr && to11 != nullptr && device_ != nullptr) {
    // A new transport, so new fences: every value restarts (the helper's `progress` stays monotonic on its own).
    std::vector<ComPtr<IUnknown>> old_fences;
    old_fences.emplace_back(to12_.Get());
    old_fences.emplace_back(to11_.Get());
    to12_.Reset();
    to11_.Reset();
    RetireShared(host, {&color_, &mask_, &motion_});
    Retire(host, std::move(old_fences));
    watchdog_ = {};
    last_value_ = 0u;
    waited11_ = 0u;
    pending_out_ = 0u;
    const HRESULT first = device_->OpenSharedHandle(to12, IID_PPV_ARGS(&to12_));
    const HRESULT second = device_->OpenSharedHandle(to11, IID_PPV_ARGS(&to11_));
    if (FAILED(first) || FAILED(second)) {
      // No fallback: Direct3D 12 always has shared fences, so a refusal is a fault of this device (or a wrapper's), named with its HRESULT.
      to12_.Reset();
      to11_.Reset();
      Stop(std::format("the helper's shared fences could not be opened (HRESULT {:#010x})", static_cast<uint32_t>(FAILED(first) ? first : second)), link, host);
    }
  }
  HRESULT result = S_OK;
  if (color != nullptr && latch_.empty()) {
    // The colour of the FRAME that was sent (color_ask_), which this reply may only be delivering from an earlier one.
    if (Open(host, color, &color_, &result)) {
      color_failure_.reset();
    } else {
      color_failure_ = ShareFailure{.size = color_ask_.size, .format = color_ask_.format, .result = result};
      nr::Log(nr::LogLevel::WARN, ShareFailureText("The back buffer", color_ask_.format, result));
    }
  }
  if (mask != nullptr && latch_.empty()) {
    if (Open(host, mask, &mask_, &result)) {
      mask_failure_.reset();
    } else {
      // The texture is the one the last SHARE_MASK asked for (this reply landed late), not the back buffer's size and format.
      mask_failure_ = ShareFailure{.size = mask_ask_.size, .format = mask_ask_.format, .result = result};
      nr::Log(nr::LogLevel::WARN, ShareFailureText("UPLIFT_MASK", mask_ask_.format, result));
    }
  }
  if (motion != nullptr && latch_.empty()) {
    if (!Open(host, motion, &motion_, &result)) {
      motion_failure_ = ShareFailure{.size = motion_ask_size_, .format = DXGI_FORMAT_R16G16_FLOAT, .result = result};
      nr::Log(nr::LogLevel::WARN, ShareFailureText("UPLIFT_MV", DXGI_FORMAT_R16G16_FLOAT, result));
    }
  }
  for (const HANDLE handle : {to12, to11, color, mask, motion}) {
    if (handle != nullptr) {
      CloseHandle(handle);
    }
  }
  if (latch_.empty() && to11_ != nullptr && to12_ != nullptr) {
    // Both directions count as progress, so a frame shows two steps: the game's work up to the copy-in, then NR's. A fence at UINT64_MAX with the helper still
    // answering is its device removed; a dead helper never gets here (the launcher sees it).
    const uint64_t completed = to11_->GetCompletedValue();
    if (const std::optional<std::string_view> stopped = watchdog_.Check(completed, completed + to12_->GetCompletedValue(), std::chrono::steady_clock::now())) {
      Stop(*stopped, link, host);
    }
  }
  if (reply.running == 0u) {
    RetireShared(host, {&color_, &mask_, &motion_});
  }
}

bool D3D12Client::Run(D3D12Host& host, ID3D12Resource* back_buffer, D3D12Usage entry_state, ID3D12Resource* motion, addon::TriggerPoint point, NrLink& link) {
  run_sent_ = false;
  if (!latch_.empty() || in_flight_ || back_buffer == nullptr || !host.Valid()) return false;
  const D3D12_RESOURCE_DESC description = back_buffer->GetDesc();
  if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.SampleDesc.Count != 1u) return false;
  const nr::Size size = {static_cast<uint32_t>(description.Width), description.Height};
  if (!color_.resource || color_.size != size || color_.format != description.Format || to12_ == nullptr || to11_ == nullptr) return false;
  const ipc::Run run = {.point = static_cast<uint32_t>(point)};
  return RunFenced(host, back_buffer, entry_state, motion, run, link);
}

bool D3D12Client::RunFenced(D3D12Host& host, ID3D12Resource* back_buffer, D3D12Usage entry_state, ID3D12Resource* motion, ipc::Run run, NrLink& link) {
  const uint64_t completed = to11_->GetCompletedValue();
  if (completed == std::numeric_limits<uint64_t>::max()) {
    // The helper is gone or its device was removed: no RUN goes to a dead peer and no copy in is made for it. The launcher shows an exit at the next present
    // (ReleaseAll), and Apply's watchdog latches a removed device.
    return false;
  }
  if (pending_out_ != 0u) {
    // A RUN whose reply never came may still be on the helper's queue: nothing touches the shared textures until the fence shows its signal (a CPU poll; no GPU
    // wait is queued for a signal nobody confirmed), or its late reply says it never signalled.
    if (const std::optional<bool> signalled = link.TakeLateRunSignalled(); signalled && !*signalled) {
      pending_out_ = 0u;
    }
    if (pending_out_ != 0u && completed < pending_out_) {
      ++busy_skips_;
      return false;
    }
    pending_out_ = 0u;
  }
  // Plan 7 decision 3: LaunchPad's UPLIFT_MV, shared like the mask (RG16F). Retired when a recording comes without one.
  ID3D12Resource* shared_motion_source = nullptr;
  const D3D12_RESOURCE_DESC motion_description = (motion != nullptr ? motion->GetDesc() : D3D12_RESOURCE_DESC{});
  if (motion != nullptr && motion_description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && motion_description.Format == DXGI_FORMAT_R16G16_FLOAT
      && motion_description.SampleDesc.Count == 1u) {
    const nr::Size motion_size = {static_cast<uint32_t>(motion_description.Width), motion_description.Height};
    if (!motion_.resource || motion_.size != motion_size) {
      RetireShared(host, {&motion_});
      if (!(motion_failure_ && motion_failure_->Matches(motion_size, DXGI_FORMAT_R16G16_FLOAT))) {
        const ipc::Share share = {.width = motion_size.width, .height = motion_size.height, .dxgi_format = static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT)};
        motion_ask_size_ = motion_size;
        const std::optional<ipc::Reply> shared = link.Share(ipc::RequestKind::SHARE_MOTION, share);
        if (!shared) {
          in_flight_ = true;  // late: its handle arrives with a later reply; nothing has been touched yet
          return false;
        }
        HANDLE handle = nullptr;
        HRESULT result = E_FAIL;
        const bool opened = (shared->ok != 0u && shared->handles.motion != 0u && link.Pull(shared->handles.motion, &handle) && Open(host, handle, &motion_, &result));
        if (handle != nullptr) {
          CloseHandle(handle);
        }
        if (opened) {
          motion_failure_.reset();
        } else {
          // Logged once: not retried for this size (Plan 7's final review Minor 3).
          motion_failure_ = ShareFailure{.size = motion_size, .format = DXGI_FORMAT_R16G16_FLOAT, .result = result};
          nr::Logf(nr::LogLevel::WARN, "UPLIFT_MV could not be shared with Uplift's helper (HRESULT {:#010x}); NR runs without LaunchPad's motion",
                   static_cast<uint32_t>(result));
        }
      }
    }
    if (motion_.resource && motion_.size == motion_size) {
      shared_motion_source = motion;
    }
  }
  // 1.1.2 (a player's freeze, NFS Underground 2): a frame without LaunchPad's motion (the Uplift technique skipped a frame) keeps the share; it goes
  // with NR (Release), at a new size, or when the helper says so. Releasing and re-sharing it around one such frame coincided with a GPU fault.
  run.motion = (shared_motion_source != nullptr ? 1u : 0u);
  run.mask_fresh = ((mask_fresh_ && mask_.resource) ? 1u : 0u);

  class Steps final : public bridge::BridgeSteps {
   public:
    Steps(D3D12Client& client, D3D12Host& host, ID3D12Resource* back_buffer, D3D12Usage entry_state, ID3D12Resource* motion, const ipc::Run& run, NrLink& link)
        : client_(client), host_(host), back_buffer_(back_buffer), entry_state_(entry_state), motion_(motion), run_(run), link_(link) {}
    bool sent = false;       // link.Run put its request on the wire: the helper has seen this point
    bool timed_out = false;  // and it did not answer in time (or is gone), or the link refused it
    ipc::Reply reply;        // what WaitD3D12 got, for the three steps that ask the helper's side

    void CopyIn() override {
      // The frame into the shared colour, in the states the helper's side expects (general = COMMON), and UPLIFT_MV (resting in shader_resource) the same way.
      host_.Barrier(back_buffer_, entry_state_, D3D12Usage::COPY_SOURCE);
      host_.Barrier(client_.color_.resource.Get(), D3D12Usage::GENERAL, D3D12Usage::COPY_DEST);
      host_.Copy(back_buffer_, client_.color_.resource.Get());
      host_.Barrier(client_.color_.resource.Get(), D3D12Usage::COPY_DEST, D3D12Usage::GENERAL);
      host_.Barrier(back_buffer_, D3D12Usage::COPY_SOURCE, entry_state_);
      if (motion_ != nullptr) {
        host_.Barrier(motion_, D3D12Usage::SHADER_RESOURCE, D3D12Usage::COPY_SOURCE);
        host_.Barrier(client_.motion_.resource.Get(), D3D12Usage::GENERAL, D3D12Usage::COPY_DEST);
        host_.Copy(motion_, client_.motion_.resource.Get());
        host_.Barrier(client_.motion_.resource.Get(), D3D12Usage::COPY_DEST, D3D12Usage::GENERAL);
        host_.Barrier(motion_, D3D12Usage::COPY_SOURCE, D3D12Usage::SHADER_RESOURCE);
      }
    }
    // command_queue::signal flushes the immediate list (the copy in), then signals: the helper's queue wait must not sit behind an unsubmitted signal.
    bool SignalD3D11(uint64_t value) override { return host_.Signal(client_.to12_.Get(), value); }
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
    // R17: only after the reply said the signal was submitted (RunBridgedFrame calls this after SignalD3D12 returned true). command_queue::wait does not flush,
    // and nothing is recorded between the signal above and this wait; what the copy out records after it runs after the wait, whenever ReShade flushes.
    bool WaitD3D11(uint64_t value) override {
      client_.waited11_ = value;  // for Stop, whether or not the queue took it
      return host_.Wait(client_.to11_.Get(), value);
    }
    void CopyOut() override {
      host_.Barrier(client_.color_.resource.Get(), D3D12Usage::GENERAL, D3D12Usage::COPY_SOURCE);
      host_.Barrier(back_buffer_, entry_state_, D3D12Usage::COPY_DEST);
      host_.Copy(client_.color_.resource.Get(), back_buffer_);
      host_.Barrier(client_.color_.resource.Get(), D3D12Usage::COPY_SOURCE, D3D12Usage::GENERAL);
      host_.Barrier(back_buffer_, D3D12Usage::COPY_DEST, entry_state_);
    }

   private:
    D3D12Client& client_;
    D3D12Host& host_;
    ID3D12Resource* back_buffer_;
    D3D12Usage entry_state_;
    ID3D12Resource* motion_;
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
    // No reply in 2 s (or the helper is gone, which the launcher shows next frame). Nothing was queued on this side's GPU after the copy in, and no wait ever
    // will be for `out`; the helper's list may still run, so until to11 shows its signal (or a late reply says there is none) nothing touches the shared
    // textures. A RUN the link refused never reached the helper.
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
    Stop(frame.failure, link, host);
  }
  return frame.wrote;
}

MaskCopy D3D12Client::CopyMask(D3D12Host& host, ID3D12Resource* mask, NrLink& link) {
  if (!latch_.empty() || to11_ == nullptr) return {};
  if (in_flight_ || (pending_out_ != 0u && to11_->GetCompletedValue() < pending_out_)) return {.busy = true};
  const D3D12_RESOURCE_DESC description = (mask != nullptr ? mask->GetDesc() : D3D12_RESOURCE_DESC{});
  const std::optional<color::MaskFormatInfo> format =
      ((mask != nullptr && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) ? color::DescribeMaskFormat(description.Format) : std::nullopt);
  if (!format || description.SampleDesc.Count != 1u) {
    ReleaseMask(host);
    return {.unsupported = true};
  }
  if (!host.Valid()) return {};
  const nr::Size size = {static_cast<uint32_t>(description.Width), description.Height};
  // Shared in the typed view format: D3D12 copies between members of one typeless group (a typeless RGBA8 mask too).
  if (!mask_.resource || mask_.size != size || mask_.format != format->view_format) {
    ReleaseMask(host);
    if (mask_failure_ && mask_failure_->Matches(size, format->view_format)) return {};
    const ipc::Share share = {.width = size.width, .height = size.height, .dxgi_format = static_cast<uint32_t>(format->view_format)};
    mask_ask_ = {.size = size, .format = format->view_format};
    const std::optional<ipc::Reply> shared = link.Share(ipc::RequestKind::SHARE_MASK, share);
    if (!shared) {
      in_flight_ = true;  // late: the handle arrives with a later reply
      return {.busy = true};
    }
    HANDLE handle = nullptr;
    HRESULT result = E_FAIL;
    const bool opened = (shared->ok != 0u && shared->handles.mask != 0u && link.Pull(shared->handles.mask, &handle) && Open(host, handle, &mask_, &result));
    if (handle != nullptr) {
      CloseHandle(handle);
    }
    if (!opened) {
      // Logged once: not retried for this size and format (final review Minor 3).
      mask_failure_ = ShareFailure{.size = size, .format = format->view_format, .result = result};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with Uplift's helper (DXGI_FORMAT {}, HRESULT {:#010x})", static_cast<int>(format->view_format),
               static_cast<uint32_t>(result));
      return {};
    }
    mask_failure_.reset();
  }
  // UPLIFT_MASK rests in shader_resource between techniques; the next Run's signal flushes this copy ahead of the helper's wait.
  host.Barrier(mask, D3D12Usage::SHADER_RESOURCE, D3D12Usage::COPY_SOURCE);
  host.Barrier(mask_.resource.Get(), D3D12Usage::GENERAL, D3D12Usage::COPY_DEST);
  host.Copy(mask, mask_.resource.Get());
  host.Barrier(mask_.resource.Get(), D3D12Usage::COPY_DEST, D3D12Usage::GENERAL);
  host.Barrier(mask, D3D12Usage::COPY_SOURCE, D3D12Usage::SHADER_RESOURCE);
  mask_fresh_ = true;
  return {.size = size};
}

std::string D3D12Client::Line() const {
  return std::format("Direct3D 12 ({}): NR runs in Uplift's 64-bit helper (shared textures and fences)", BITNESS);
}

}  // namespace uplift::client
