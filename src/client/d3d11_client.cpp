#include "client/d3d11_client.hpp"

#include <dxgi.h>

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

constexpr auto GPU_CAP = std::chrono::seconds(2);  // the design's R29 cap on the game thread's waits
constexpr double MIB = 1024.0 * 1024.0;

// The description behind a D3D11 resource; nullopt for anything that is not a 2D texture.
std::optional<D3D11_TEXTURE2D_DESC> TextureDesc(ID3D11Resource* resource) {
  ComPtr<ID3D11Texture2D> texture;
  if (resource == nullptr || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return std::nullopt;
  D3D11_TEXTURE2D_DESC description = {};
  texture->GetDesc(&description);
  return description;
}

std::string ShareFailureText(const char* what, DXGI_FORMAT format, HRESULT result) {
  return std::format("{} could not be shared with Uplift's helper (DXGI_FORMAT {}, HRESULT {:#010x})", what, static_cast<int>(format),
                     static_cast<uint32_t>(result));
}

}  // namespace

std::unique_ptr<D3D11Client> D3D11Client::Create(ID3D11Device* device, bool allow_fences, std::string* error, std::string_view label) {
  const auto fail = [error](std::string text) {
    *error = std::move(text);
    return std::unique_ptr<D3D11Client>();
  };
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adapter_description = {};
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) || FAILED(dxgi_device->GetAdapter(&adapter))
      || FAILED(adapter->GetDesc(&adapter_description))) {
    return fail("the game's adapter could not be read");
  }
  // WARP and other vendors are refused, as on D3D12 (addon::IsNvidiaDevice); cross-adapter sharing is out of scope.
  if (adapter_description.VendorId != addon::NVIDIA_VENDOR_ID) {
    return fail("Uplift needs an NVIDIA GPU; this game renders on another adapter");
  }
  std::unique_ptr<D3D11Client> client(new D3D11Client());
  client->label_ = label;
  client->device_ = device;
  device->GetImmediateContext(&client->context_);
  client->luid_ = adapter_description.AdapterLuid;
  // Design §2.3: D3D11 fences need ID3D11Device5 and ID3D11DeviceContext4 (Windows 10 1703); without them the frame crosses
  // through a legacy shared texture, ordered on the CPU.
  client->fenced_ = (allow_fences && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&client->device5_)))
                     && SUCCEEDED(client->context_.As(&client->context4_)));
  if (!client->fenced_) {
    client->device5_.Reset();
    client->context4_.Reset();
  }
  return client;
}

D3D11Client::~D3D11Client() {
  ReleaseAll();
}

void D3D11Client::ReleaseShared() {
  color_ = {};
  mask_ = {};
  motion_ = {};
  mask_fresh_ = false;
  kmt_texture_.Reset();
  kmt_handle_ = nullptr;
  kmt_size_ = {};
  kmt_format_ = DXGI_FORMAT_UNKNOWN;
  kmt_bytes_ = 0u;
  floor_probed_ = false;
}

void D3D11Client::ReleaseMask() {
  mask_ = {};
  mask_fresh_ = false;
}

bool D3D11Client::ReleaseAll() {
  const bool held = (color_.texture || mask_.texture || motion_.texture || kmt_texture_ || to12_11_ || to11_11_ || query_);
  ReleaseShared();
  query_.Reset();
  gpu_pending_ = false;
  to12_11_.Reset();
  to11_11_.Reset();
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

void D3D11Client::Stop(std::string_view reason, NrLink& link) {
  if (latch_.empty()) {
    latch_ = std::format("The {} bridge stopped: {}. Restart the game to use NR again", label_, reason);
    nr::Log(nr::LogLevel::ERR, latch_);
  }
  link.Stop(waited11_, latch_);
  ReleaseShared();
}

bool D3D11Client::Open(HANDLE handle, Shared* shared, HRESULT* result) {
  Shared opened;
  *result = device5_->OpenSharedResource1(handle, IID_PPV_ARGS(&opened.texture));
  if (FAILED(*result)) return false;
  D3D11_TEXTURE2D_DESC description = {};
  opened.texture->GetDesc(&description);
  opened.size = {description.Width, description.Height};
  opened.format = description.Format;
  const std::optional<color::FormatInfo> format = color::DescribeFormat(description.Format);
  const std::optional<color::MaskFormatInfo> mask_format = color::DescribeMaskFormat(description.Format);
  opened.bytes = opened.size.Pixels() * (format ? format->bytes_per_pixel : mask_format ? mask_format->bytes_per_pixel
                                                                                        : 4u);
  *shared = std::move(opened);
  return true;
}

ipc::Target D3D11Client::Describe(ID3D11Resource* back_buffer, bool enabled, bool running) {
  return Describe(TextureDesc(back_buffer), enabled, running);
}

ipc::Target D3D11Client::Describe(const std::optional<D3D11_TEXTURE2D_DESC>& description, bool enabled, bool running) {
  ipc::Target target;
  const nr::Size size = (description ? nr::Size{description->Width, description->Height} : nr::Size{});
  const DXGI_FORMAT format = (description ? description->Format : DXGI_FORMAT_UNKNOWN);
  described_size_ = size;
  described_format_ = format;
  target.width = size.width;
  target.height = size.height;
  target.dxgi_format = static_cast<uint32_t>(format);
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
  }
  const std::optional<color::FormatInfo> info = (description ? color::DescribeFormat(format) : std::nullopt);
  std::string problem;
  if (!latch_.empty()) {
    problem = latch_;
  } else if (!description) {
    problem = "The back buffer is not a 2D texture";
  } else if (description->SampleDesc.Count != 1u) {
    problem = std::format("Multisampled back buffers are not supported on {}", label_);
  } else if (!info) {
    problem = std::format("Unsupported back-buffer format (DXGI_FORMAT {})", static_cast<int>(format));
  }
  if (!running || !problem.empty()) {
    ReleaseShared();  // NR is off, or this image cannot go: the shared surfaces go with it (Plan 7's final review C-1)
  }
  if (problem.empty() && color_failure_ && color_failure_->Matches(size, format)) {
    problem = ShareFailureText("The back buffer", format, color_failure_->result);
  }
  if (problem.empty() && running && !fenced_ && (!kmt_texture_ || kmt_size_ != size || kmt_format_ != format)) {
    // KMT: this side makes the shared texture (design §2.3); the helper opens it by its global handle.
    ReleaseShared();
    const D3D11_TEXTURE2D_DESC shared = {
        .Width = size.width,
        .Height = size.height,
        .MipLevels = 1u,
        .ArraySize = 1u,
        .Format = format,
        .SampleDesc = {.Count = 1u, .Quality = 0u},
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = static_cast<UINT>(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE),
        .CPUAccessFlags = 0u,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED,
    };
    ComPtr<IDXGIResource> resource;
    HRESULT result = device_->CreateTexture2D(&shared, nullptr, &kmt_texture_);
    if (SUCCEEDED(result)) {
      result = kmt_texture_.As(&resource);
    }
    if (SUCCEEDED(result)) {
      result = resource->GetSharedHandle(&kmt_handle_);
    }
    if (FAILED(result)) {
      ReleaseShared();
      color_failure_ = ShareFailure{.size = size, .format = format, .result = result};
      problem = ShareFailureText("The back buffer", format, result);
    } else {
      kmt_size_ = size;
      kmt_format_ = format;
      kmt_bytes_ = size.Pixels() * info->bytes_per_pixel;
      ++kmt_generation_;
    }
  }
  if (kmt_texture_ != nullptr && problem.empty()) {
    target.kmt_handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(kmt_handle_));
    target.kmt_generation = kmt_generation_;
  }
  if (problem != problem_) {
    if (!problem.empty()) {
      nr::Log(nr::LogLevel::WARN, problem);
    }
    problem_ = std::move(problem);
  }
  ipc::CopyText(target.problem, problem_);
  return target;
}

bool D3D11Client::Apply(const ipc::Reply& reply, NrLink& link) {
  in_flight_ = false;  // a FRAME reply means every earlier request has replied
  bool fell_back = false;
  const ipc::Handles& handles = reply.handles;
  // Every NT handle in the reply is taken and closed below, whatever this client does with it, so the helper's table never
  // keeps one.
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
    ReleaseShared();
    to12_11_.Reset();
    to11_11_.Reset();
    watchdog_ = {};
    last_value_ = 0u;
    waited11_ = 0u;
    pending_out_ = 0u;
    const HRESULT first = device5_->OpenSharedFence(to12, IID_PPV_ARGS(&to12_11_));
    const HRESULT second = device5_->OpenSharedFence(to11, IID_PPV_ARGS(&to11_11_));
    if (FAILED(first) || FAILED(second)) {
      // An unusual device (a wrapper such as dgVoodoo2 can refuse): no restart would help, so the frame crosses through the legacy
      // shared texture, ordered on the CPU, which needs no fences (design §2.3). The caller re-attaches the helper with KMT.
      nr::Logf(nr::LogLevel::WARN,
               "The Direct3D 11 device could not open the helper's shared fences (HRESULT {:#010x}); frames cross through a "
               "CPU-ordered shared texture instead",
               static_cast<uint32_t>(FAILED(first) ? first : second));
      to12_11_.Reset();
      to11_11_.Reset();
      device5_.Reset();
      context4_.Reset();
      fenced_ = false;
      fell_back = true;
    }
  }
  if (fenced_) {
    HRESULT result = S_OK;
    if (color != nullptr && latch_.empty()) {
      if (Open(color, &color_, &result)) {
        color_failure_.reset();
      } else {
        color_failure_ = ShareFailure{.size = described_size_, .format = described_format_, .result = result};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("The back buffer", described_format_, result));
      }
    }
    if (mask != nullptr && latch_.empty()) {
      if (Open(mask, &mask_, &result)) {
        mask_failure_.reset();
      } else {
        // The texture is the one the last SHARE_MASK asked for (this reply landed late), not the back buffer's size and format.
        mask_failure_ = ShareFailure{.size = mask_ask_.size, .format = mask_ask_.format, .result = result};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("UPLIFT_MASK", mask_ask_.format, result));
      }
    }
    if (motion != nullptr && latch_.empty()) {
      if (!Open(motion, &motion_, &result)) {
        motion_failure_ = ShareFailure{.size = motion_ask_size_, .format = DXGI_FORMAT_R16G16_FLOAT, .result = result};
        nr::Log(nr::LogLevel::WARN, ShareFailureText("UPLIFT_MV", DXGI_FORMAT_R16G16_FLOAT, result));
      }
    }
  }
  for (const HANDLE handle : {to12, to11, color, mask, motion}) {
    if (handle != nullptr) {
      CloseHandle(handle);
    }
  }
  if (fenced_ && latch_.empty() && to11_11_ != nullptr && to12_11_ != nullptr) {
    // Both directions count as progress, so a frame shows two steps: the game's work up to the copy-in, then NR's. A fence at
    // UINT64_MAX with the helper still answering is its device removed; a dead helper never gets here (the launcher sees it).
    const uint64_t completed = to11_11_->GetCompletedValue();
    if (const std::optional<std::string_view> stopped =
            watchdog_.Check(completed, completed + to12_11_->GetCompletedValue(), std::chrono::steady_clock::now())) {
      Stop(*stopped, link);
    }
  }
  if (reply.running == 0u) {
    ReleaseShared();
  }
  return fell_back;
}

bool D3D11Client::Run(ID3D11Resource* back_buffer, ID3D11Resource* motion, addon::TriggerPoint point, NrLink& link) {
  run_sent_ = false;
  if (!latch_.empty() || in_flight_ || back_buffer == nullptr) return false;
  const std::optional<D3D11_TEXTURE2D_DESC> description = TextureDesc(back_buffer);
  if (!description || description->SampleDesc.Count != 1u) return false;
  const nr::Size size = {description->Width, description->Height};
  ipc::Run run = {.point = static_cast<uint32_t>(point)};

  if (!fenced_) {
    // KMT (design §2.4): CopyResource into the shared texture, an event query (capped), RUN, and back.
    if (!kmt_texture_ || kmt_size_ != size || kmt_format_ != description->Format) return false;
    if (!nr::MeetsNrFloor(size)) {
      // Below NR's floor nothing can come back: after the first RUN at this size (whose reply's status says why) no RUN at all.
      if (floor_probed_) return false;
      const bool replied = link.Run(run).has_value();
      run_sent_ = link.RunSent();
      floor_probed_ = run_sent_;  // a RUN that was refused (another request outstanding) is tried again
      if (!replied) {
        in_flight_ = true;
      }
      return false;
    }
    if (!query_) {
      const D3D11_QUERY_DESC query = {.Query = D3D11_QUERY_EVENT, .MiscFlags = 0u};
      if (FAILED(device_->CreateQuery(&query, &query_))) return false;
    }
    if (gpu_pending_) {
      // An earlier frame's 2 s wait ran out, so its copy may still be queued on the game's GPU. No RUN is out for it (the helper is
      // not using the shared texture), and this frame sends and touches nothing until the query shows the GPU passed it.
      if (context_->GetData(query_.Get(), nullptr, 0u, 0u) == S_FALSE) return false;
      gpu_pending_ = false;
    }
    context_->CopyResource(kmt_texture_.Get(), back_buffer);
    context_->End(query_.Get());
    context_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + GPU_CAP;
    HRESULT waited = context_->GetData(query_.Get(), nullptr, 0u, 0u);
    while (waited == S_FALSE && std::chrono::steady_clock::now() < deadline) {
      Sleep(0u);
      waited = context_->GetData(query_.Get(), nullptr, 0u, 0u);
    }
    if (waited != S_OK) {
      gpu_pending_ = (waited == S_FALSE);
      return false;
    }
    const std::optional<ipc::Reply> reply = link.Run(run);
    run_sent_ = link.RunSent();
    if (!reply) {
      in_flight_ = true;  // the helper may still be working on the shared texture
      return false;
    }
    if (reply->ok == 0u || reply->wrote == 0u) return false;  // ok = 0: the helper's catch path, the texture may still be written
    context_->CopyResource(back_buffer, kmt_texture_.Get());
    return true;
  }

  // FENCED: Plan 7's hand-off, split across the IPC. RunBridgedFrame owns the order of the steps.
  if (!color_.texture || color_.size != size || color_.format != description->Format || to12_11_ == nullptr || to11_11_ == nullptr) {
    return false;
  }
  const uint64_t completed = to11_11_->GetCompletedValue();
  if (completed == std::numeric_limits<uint64_t>::max()) {
    // The helper is gone or its device was removed: no RUN goes to a dead peer and no copy-in is made for it. The launcher shows
    // an exit at the next present (ReleaseAll), and Apply's watchdog latches a removed device.
    return false;
  }
  if (pending_out_ != 0u) {
    // A RUN whose reply never came may still be on the helper's queue: nothing touches the shared textures until the fence shows
    // its signal (a CPU poll; no GPU wait is queued for a signal nobody confirmed), or its late reply says it never signalled.
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
  const std::optional<D3D11_TEXTURE2D_DESC> motion_description = TextureDesc(motion);
  ID3D11Resource* shared_motion_source = nullptr;
  if (motion_description && motion_description->Format == DXGI_FORMAT_R16G16_FLOAT && motion_description->SampleDesc.Count == 1u) {
    const nr::Size motion_size = {motion_description->Width, motion_description->Height};
    if (!motion_.texture || motion_.size != motion_size) {
      motion_ = {};
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
        const bool opened = (shared->ok != 0u && shared->handles.motion != 0u && link.Pull(shared->handles.motion, &handle)
                             && Open(handle, &motion_, &result));
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
    if (motion_.texture && motion_.size == motion_size) {
      shared_motion_source = motion;
    }
  } else if (motion_.texture) {
    motion_ = {};
  }
  run.motion = (shared_motion_source != nullptr ? 1u : 0u);
  run.mask_fresh = ((mask_fresh_ && mask_.texture) ? 1u : 0u);

  class Steps final : public bridge::BridgeSteps {
   public:
    Steps(D3D11Client& client, ID3D11Resource* back_buffer, ID3D11Resource* motion, const ipc::Run& run, NrLink& link)
        : client_(client), back_buffer_(back_buffer), motion_(motion), run_(run), link_(link) {}
    bool sent = false;       // link.Run put its request on the wire: the helper has seen this point
    bool timed_out = false;  // and it did not answer in time (or is gone), or the link refused it
    ipc::Reply reply;        // what WaitD3D12 got, for the three steps that ask the helper's side

    void CopyIn() override {
      client_.context_->CopyResource(client_.color_.texture.Get(), back_buffer_);
      if (motion_ != nullptr) {
        client_.context_->CopyResource(client_.motion_.texture.Get(), motion_);
      }
    }
    bool SignalD3D11(uint64_t value) override {
      const HRESULT signalled = client_.context4_->Signal(client_.to12_11_.Get(), value);
      client_.context_->Flush();  // the helper's queue wait must not sit behind an unsubmitted D3D11 signal
      return SUCCEEDED(signalled);
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
      client_.waited11_ = value;  // for Stop, whether or not the context took it
      return SUCCEEDED(client_.context4_->Wait(client_.to11_11_.Get(), value));
    }
    void CopyOut() override { client_.context_->CopyResource(back_buffer_, client_.color_.texture.Get()); }

   private:
    D3D11Client& client_;
    ID3D11Resource* back_buffer_;
    ID3D11Resource* motion_;
    ipc::Run run_;
    NrLink& link_;
  };
  const uint64_t in = ++last_value_;  // monotonic even when a step fails
  const uint64_t out = ++last_value_;
  run.out = out;
  Steps steps(*this, back_buffer, shared_motion_source, run, link);
  const bridge::BridgedFrame frame = bridge::RunBridgedFrame(steps, in, out);
  run_sent_ = steps.sent;
  if (steps.timed_out) {
    // No reply in 2 s (or the helper is gone, which the launcher shows next frame). Nothing was queued on this side's GPU after
    // the copy-in, and no D3D11 wait ever will be for `out`; the helper's list may still run, so until to11 shows its signal
    // (or a late reply says there is none) nothing touches the shared textures. A RUN the link refused never reached the helper.
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
    Stop(frame.failure, link);
  }
  return frame.wrote;
}

MaskCopy D3D11Client::CopyMask(ID3D11Resource* mask, NrLink& link) {
  if (!fenced_) return {.unsupported = true};  // no mask or motion on the CPU-ordered path (the notes say so)
  if (!latch_.empty() || to11_11_ == nullptr) return {};
  if (in_flight_ || (pending_out_ != 0u && to11_11_->GetCompletedValue() < pending_out_)) return {.busy = true};
  const std::optional<D3D11_TEXTURE2D_DESC> description = TextureDesc(mask);
  const std::optional<color::MaskFormatInfo> format =
      (description ? color::DescribeMaskFormat(description->Format) : std::nullopt);
  if (!format || description->SampleDesc.Count != 1u) {
    ReleaseMask();
    return {.unsupported = true};
  }
  const nr::Size size = {description->Width, description->Height};
  // Shared in the typed view format: D3D11 copies between members of one typeless group (a typeless RGBA8 mask too).
  if (!mask_.texture || mask_.size != size || mask_.format != format->view_format) {
    ReleaseMask();
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
    const bool opened = (shared->ok != 0u && shared->handles.mask != 0u && link.Pull(shared->handles.mask, &handle)
                         && Open(handle, &mask_, &result));
    if (handle != nullptr) {
      CloseHandle(handle);
    }
    if (!opened) {
      // Logged once: not retried for this size and format (final review Minor 3).
      mask_failure_ = ShareFailure{.size = size, .format = format->view_format, .result = result};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with Uplift's helper (DXGI_FORMAT {}, HRESULT {:#010x})",
               static_cast<int>(format->view_format), static_cast<uint32_t>(result));
      return {};
    }
    mask_failure_.reset();
  }
  context_->CopyResource(mask_.texture.Get(), mask);
  mask_fresh_ = true;
  return {.size = size};
}

std::string D3D11Client::Line() const {
  if (fenced_) {
    return std::format("Direct3D 11 ({}): NR runs in Uplift's 64-bit helper (shared textures and fences, {:.1f} MiB)", BITNESS,
                       static_cast<double>(color_.bytes + mask_.bytes + motion_.bytes) / MIB);
  }
  return std::format("Direct3D 11 ({}): NR runs in Uplift's 64-bit helper (shared texture, CPU-ordered, {:.1f} MiB)", BITNESS,
                     static_cast<double>(kmt_bytes_) / MIB);
}

}  // namespace uplift::client
