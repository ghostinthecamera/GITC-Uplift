#include "bridge/d3d11_bridge.hpp"

#include <dxgi1_6.h>

#include <format>
#include <utility>

#include "addon/environment.hpp"
#include "color/encoding.hpp"
#include "nr/log.hpp"

namespace uplift::bridge {

using Microsoft::WRL::ComPtr;

constexpr ULONGLONG PENDING_WAIT_MS = 2000u;  // the destructor's wait for the game's D3D11 context, as D3D12Side::Drain's

// The description behind a D3D11 resource; nullopt for anything that is not a 2D texture. Plan 18: public (the DLSS hand-off reads DLSS's textures).
std::optional<D3D11_TEXTURE2D_DESC> TextureDesc11(ID3D11Resource* resource) {
  ComPtr<ID3D11Texture2D> texture;
  if (resource == nullptr || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return std::nullopt;
  D3D11_TEXTURE2D_DESC description = {};
  texture->GetDesc(&description);
  return description;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE};
  barrier.Transition = {.pResource = resource, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = before, .StateAfter = after};
  list->ResourceBarrier(1u, &barrier);
}

std::unique_ptr<D3D11Bridge> D3D11Bridge::Create(ID3D11Device* device, ComPtr<ID3D12Device>* created,
                                                 std::string* error, std::string_view label) {
  const auto fail = [error](std::string text) {
    *error = std::move(text);
    return std::unique_ptr<D3D11Bridge>();
  };
  std::unique_ptr<D3D11Bridge> bridge(new D3D11Bridge());
  bridge->label_ = label;
  // D3D11 design §1.3: D3D11 fences need ID3D11Device5 and ID3D11DeviceContext4 (Windows 10 1703).
  ComPtr<ID3D11DeviceContext> immediate;
  device->GetImmediateContext(&immediate);
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&bridge->device11_))) || FAILED(immediate.As(&bridge->context11_))) {
    return fail("Direct3D 11 fences need Windows 10 1703 or newer and a current driver");
  }
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
  // Plan 9: the private device, queue, ring, list and progress fence are D3D12Side's; `created` is the device as it made it (on the fallback,
  // D3D12CreateDevice's: under ReShade its proxy, whose last release raises destroy_device).
  bridge->side_ = D3D12Side::Create(adapter_description.AdapterLuid, created, error);
  if (bridge->side_ == nullptr) return nullptr;
  const std::pair<ComPtr<ID3D12Fence>*, ID3D11Fence**> fences[] = {{&bridge->to12_, bridge->to12_11_.GetAddressOf()},
                                                                   {&bridge->to11_, bridge->to11_11_.GetAddressOf()}};
  for (const auto& [fence, fence11] : fences) {
    HANDLE fence_handle = nullptr;
    if (FAILED(bridge->side_->CreateSharedFence(fence, &fence_handle))) return fail("the shared fence could not be created");
    const HRESULT opened = bridge->device11_->OpenSharedFence(fence_handle, IID_PPV_ARGS(fence11));
    CloseHandle(fence_handle);  // the opened fence keeps it alive
    if (FAILED(opened)) return fail("the Direct3D 11 device could not open the shared fence");
  }
  nr::Logf(nr::LogLevel::INFO, "{}: NR runs on a private Direct3D 12 device", bridge->label_);
  return bridge;
}

D3D11Bridge::~D3D11Bridge() {
  // Nothing either queue may still use is released before it finishes: first the game's D3D11 context, then the private queue,
  // each at most 2 s (as DeviceContext::Teardown).
  if (side_ != nullptr) {
    const ULONGLONG deadline = GetTickCount64() + PENDING_WAIT_MS;
    while (HandOverFinished(), !pending_.empty() && GetTickCount64() < deadline) {
      Sleep(1u);
    }
    for (Pending& pending : pending_) {
      side_->Retire(std::move(pending.d3d12), std::move(pending.d3d11), pending.last_use);
    }
    pending_.clear();
    side_->Drain();
  }
}

bool D3D11Bridge::CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, uint32_t bytes_per_pixel,
                               const wchar_t* name, Shared* shared, HRESULT* result) {
  Shared created = {.size = size, .format = format, .bytes = size.Pixels() * bytes_per_pixel, .flags = flags};
  HANDLE handle = nullptr;
  *result = side_->CreateShared(size, format, flags, name, &created.d3d12, &handle);
  if (FAILED(*result)) return false;
  *result = device11_->OpenSharedResource1(handle, IID_PPV_ARGS(&created.d3d11));
  CloseHandle(handle);  // the opened texture keeps the allocation
  if (FAILED(*result)) return false;
  *shared = std::move(created);
  return true;
}

void D3D11Bridge::Retire(Shared* shared) {
  // Two queues use a share: the game's D3D11 context copies into and out of it, the private queue runs NR on it. The pair waits for
  // an event query ended here, behind every D3D11 use so far, and HandOverFinished then gives it to the side, which keeps it until
  // the private queue's last use. D3D11 destroys a released object at its context's next Flush.
  if (shared->d3d12) {
    Pending pending = {.d3d12 = std::move(shared->d3d12), .d3d11 = std::move(shared->d3d11), .last_use = shared->last_use};
    const D3D11_QUERY_DESC query = {.Query = D3D11_QUERY_EVENT, .MiscFlags = 0u};
    if (SUCCEEDED(device11_->CreateQuery(&query, &pending.done))) {
      context11_->End(pending.done.Get());
      pending_.push_back(std::move(pending));
    } else if (side_->Retire(std::move(pending.d3d12), std::move(pending.d3d11), pending.last_use)) {
      // No query: the private queue's progress alone, as before 1.1.6.
      released_ = true;
    }
  }
  *shared = {};
}

bool D3D11Bridge::HandOverFinished() {
  // GetData without DONOTFLUSH submits the query if it is still in the command buffer (the D3D10 relay has no Present to do it).
  // A failure is a removed device, which runs nothing either. The side decides from one read of its progress whether it kept the
  // pair or dropped it at once, and the answer is returned as is: a second read could pass `last_use` in between and lose the Flush
  // the D3D10 relay needs.
  bool released = false;
  std::erase_if(pending_, [&](Pending& pending) {
    if (context11_->GetData(pending.done.Get(), nullptr, 0u, 0u) == S_FALSE) return false;
    released = side_->Retire(std::move(pending.d3d12), std::move(pending.d3d11), pending.last_use) || released;
    return true;
  });
  return released;
}

void D3D11Bridge::CheckWatchdog(std::chrono::steady_clock::time_point now) {
  if (!latch_.empty()) return;
  // Both directions count as progress, so a frame shows two steps: the game's work up to the copy-in, then NR's.
  const uint64_t completed = to11_->GetCompletedValue();
  if (const std::optional<std::string_view> stopped = watchdog_.Check(completed, completed + to12_->GetCompletedValue(), now)) {
    std::string reason = std::format("The {} bridge stopped: {}", label_, *stopped);
    nr::Log(nr::LogLevel::ERR, reason);
    Stop(std::move(reason));
  }
}

BridgeFrame D3D11Bridge::BeginFrame(ID3D11Resource* back_buffer, bool enabled, bool running,
                                    std::chrono::steady_clock::time_point now, bool present_copy) {
  ++presents_;  // Plan 18: the frame being rendered from here is presents_ + 1 (the ring's slots)
  if (HandOverFinished()) {
    released_ = true;
  }
  if (side_->FreeFinished()) {
    released_ = true;
  }
  CheckWatchdog(now);
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
    dlss_image_failure_.reset();  // Plan 18: re-enabling retries the DLSS stages' shares too
    dlss_motion_failure_.reset();
    dlss_exposure_failure_.reset();
    dlss_swap_failure_.reset();
    ring_failure_.reset();
  }
  if (!running || !latch_.empty()) {
    // NR is off, or the bridge stopped (final review C-1): the shared surfaces go with NR's own (the stutter fix).
    // Retire keeps each one until the side's progress passes its last submitted use, so nothing still in flight is freed, and
    // it never waits: after Stop a wedged private queue gets its waits released and drains on its own.
    Retire(&color_);
    Retire(&mask_);
    Retire(&motion_);
    mask_fresh_ = false;
    ReleaseDlss();  // Plan 18
    ReleasePresentMotion();
  }
  if (!present_copy) {
    // Plan 18: NR runs at a DLSS stage, inside the game's frame: the Present path's copies are not needed.
    Retire(&color_);
    Retire(&motion_);
  }
  if (!latch_.empty()) return {.problem = latch_};
  if (back_buffer == nullptr) return {};  // Plan 8: housekeeping only (the D3D10 relay has no copy while NR is off)
  std::string problem;
  const std::optional<D3D11_TEXTURE2D_DESC> description = TextureDesc11(back_buffer);
  const std::optional<color::FormatInfo> format = (description ? color::DescribeFormat(description->Format) : std::nullopt);
  const nr::Size size = (description ? nr::Size{description->Width, description->Height} : nr::Size{});
  if (!description) {
    problem = "The back buffer is not a 2D texture";
  } else if (description->SampleDesc.Count != 1u) {
    // ReShade renders effects into its own resolved copy of these (design §1.3); rare in D3D11.
    problem = std::format("Multisampled back buffers are not supported on {}", label_);
  } else if (!format) {
    problem = std::format("Unsupported back-buffer format (DXGI_FORMAT {})", static_cast<int>(description->Format));
  } else {
    if (running && present_copy && !(color_failure_ && color_failure_->Matches(size, description->Format))
        && (!color_.d3d12 || color_.size != size || color_.format != description->Format)) {
      Retire(&color_);
      HRESULT share_result = S_OK;
      if (CreateShared(size, description->Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, format->bytes_per_pixel,
                       L"Uplift shared back buffer", &color_, &share_result)) {
        color_failure_.reset();
      } else {
        color_failure_ = ShareFailure{.size = size, .format = description->Format, .result = share_result};
      }
    }
    // Reported while NR is off too, so the Session does not load for a back buffer the bridge cannot share.
    if (color_failure_ && color_failure_->Matches(size, description->Format)) {
      problem = std::format("The back buffer could not be shared with the private Direct3D 12 device (DXGI_FORMAT {}, HRESULT {:#010x})",
                            static_cast<int>(description->Format), static_cast<uint32_t>(color_failure_->result));
    }
  }
  if (problem != problem_) {
    if (!problem.empty()) {
      nr::Log(nr::LogLevel::WARN, problem);
    }
    problem_ = std::move(problem);
  }
  const DXGI_FORMAT back_buffer_format = (description ? description->Format : DXGI_FORMAT_UNKNOWN);
  if (!problem_.empty()) return {.size = size, .format = back_buffer_format, .problem = problem_};
  return {.color = (present_copy ? color_.d3d12.Get() : nullptr), .size = size, .format = back_buffer_format};
}

bool D3D11Bridge::Run(ID3D11Resource* back_buffer, ID3D11Resource* motion, const Recorder& record, bool dlss_motion) {
  if (!latch_.empty() || !color_.d3d12 || back_buffer == nullptr) return false;
  if (!side_->SlotFree()) {
    ++busy_skips_;  // this frame goes without NR rather than waiting
    return false;
  }
  // Plan 18 (design §4): with `dlss_motion`, this frame's ring slot (copied in the game's evaluate) is DLSS's vectors for the Present path, before Launchpad's.
  RingSlot* ring_slot = nullptr;
  if (dlss_motion) {
    RingSlot& slot = ring_[presents_ % RING_SLOTS];
    if (slot.shared.d3d12 && slot.frame == presents_) ring_slot = &slot;
  }
  // Decision 3: LaunchPad's UPLIFT_MV, shared like the mask (RG16F, +32 MiB at 4K). The sharing rule (design §9 g):
  // D3D11 can only open a D3D12 shared texture made ALLOW_RENDER_TARGET, and outside R8, R16 and the display formats
  // it also needs ALLOW_SIMULTANEOUS_ACCESS. Retired when a recording comes without one: LaunchPad off, or NR at the
  // present or after the effects.
  ID3D12Resource* motion12 = nullptr;
  const std::optional<D3D11_TEXTURE2D_DESC> motion_description = (ring_slot == nullptr ? TextureDesc11(motion) : std::nullopt);
  if (ring_slot != nullptr) {
    motion12 = ring_slot->shared.d3d12.Get();  // Plan 18: DLSS's own vectors this frame; UPLIFT_MV does not cross
    Retire(&motion_);
  } else if (motion_description && motion_description->Format == DXGI_FORMAT_R16G16_FLOAT
      && motion_description->SampleDesc.Count == 1u) {
    const nr::Size size = {motion_description->Width, motion_description->Height};
    if (!motion_.d3d12 || motion_.size != size) {
      Retire(&motion_);
      if (!(motion_failure_ && motion_failure_->Matches(size, DXGI_FORMAT_R16G16_FLOAT))) {
        HRESULT share_result = S_OK;
        if (CreateShared(size, DXGI_FORMAT_R16G16_FLOAT,
                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, 4u,
                         L"Uplift shared motion", &motion_, &share_result)) {
          motion_failure_.reset();
        } else {
          // Logged once: not retried for this size (final review Minor 3).
          motion_failure_ = ShareFailure{.size = size, .format = DXGI_FORMAT_R16G16_FLOAT, .result = share_result};
          nr::Logf(nr::LogLevel::WARN, "UPLIFT_MV could not be shared with the private Direct3D 12 device (HRESULT {:#010x}); NR runs without LaunchPad's motion",
                   static_cast<uint32_t>(share_result));
        }
      }
    }
    motion12 = motion_.d3d12.Get();  // null when the share failed: NR runs without motion
  } else if (motion_.d3d12) {
    Retire(&motion_);
  }
  // The steps on this bridge's objects; RunBridgedFrame owns their order.
  class Steps final : public BridgeSteps {
   public:
    Steps(D3D11Bridge& bridge, ID3D11Resource* back_buffer, ID3D11Resource* motion, ID3D12Resource* motion12,
          const Recorder& record, const RingSlot* ring_slot)
        : bridge_(bridge), back_buffer_(back_buffer), motion11_(motion), motion12_(motion12), record_(record), ring_slot_(ring_slot) {}
    bool used_mask = false;
    bool executed = false;  // the list was submitted: its surfaces are in use until the side's progress passes it

    void CopyIn() override {
      bridge_.context11_->CopyResource(bridge_.color_.d3d11.Get(), back_buffer_);
      if (motion12_ != nullptr && motion11_ != nullptr) {  // Plan 18: a ring slot was copied in the game's evaluate
        bridge_.context11_->CopyResource(bridge_.motion_.d3d11.Get(), motion11_);
      }
    }
    bool SignalD3D11(uint64_t value) override {
      const HRESULT signalled = bridge_.context11_->Signal(bridge_.to12_11_.Get(), value);
      bridge_.context11_->Flush();  // the private queue's wait must not sit behind an unsubmitted D3D11 signal
      return SUCCEEDED(signalled);
    }
    bool WaitD3D12(uint64_t value) override {
      bridge_.waited12_ = value;  // for Stop, whether or not the queue took it
      return SUCCEEDED(bridge_.side_->Queue()->Wait(bridge_.to12_.Get(), value));
    }
    bool RecordAndExecute(bool* wrote) override {
      ID3D12GraphicsCommandList* const list = bridge_.side_->BeginList();
      if (list == nullptr) return false;
      const BridgeTargets targets = {
          .color = bridge_.color_.d3d12.Get(),
          .mask = (bridge_.mask_fresh_ ? bridge_.mask_.d3d12.Get() : nullptr),
          .motion = motion12_,
          .motion_is_dlss = (ring_slot_ != nullptr),
          .motion_scale_x = (ring_slot_ != nullptr ? ring_slot_->scale_x : 1.f),
          .motion_scale_y = (ring_slot_ != nullptr ? ring_slot_->scale_y : 1.f),
          .motion_region = (ring_slot_ != nullptr ? ring_slot_->region : nr::Rect{}),  // fix round 1 (M-4): the slot is the vectors' whole size
      };
      if (targets.mask != nullptr) {
        Transition(list, targets.mask, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      if (targets.motion != nullptr) {
        Transition(list, targets.motion, D3D12_RESOURCE_STATE_COMMON, SHADER_READ);
      }
      bool recorded = true;
      try {
        *wrote = record_(list, targets);
      } catch (...) {
        // Half a recording never executes; the queue still signals, so D3D11 never waits in vain.
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
        bridge_.side_->CloseList();
        return false;
      }
      if (!bridge_.side_->ExecuteList()) return false;
      executed = true;
      used_mask = (targets.mask != nullptr);
      return true;
    }
    // progress first: when the queue refuses it, to11_ is never signalled, so D3D11 never waits for this frame.
    bool SignalD3D12(uint64_t value) override {
      return bridge_.side_->SignalProgress(value) && SUCCEEDED(bridge_.side_->Queue()->Signal(bridge_.to11_.Get(), value));
    }
    bool WaitD3D11(uint64_t value) override {
      bridge_.waited11_ = value;  // for Stop, whether or not the context took it
      return SUCCEEDED(bridge_.context11_->Wait(bridge_.to11_11_.Get(), value));
    }
    void CopyOut() override { bridge_.context11_->CopyResource(back_buffer_, bridge_.color_.d3d11.Get()); }

   private:
    D3D11Bridge& bridge_;
    ID3D11Resource* back_buffer_;
    ID3D11Resource* motion11_;
    ID3D12Resource* motion12_;
    const Recorder& record_;
    const RingSlot* ring_slot_;
  };
  Steps steps(*this, back_buffer, (ring_slot != nullptr ? nullptr : motion), motion12, record, ring_slot);
  const uint64_t in = ++last_value_;  // monotonic even when a step fails
  const uint64_t out = ++last_value_;
  const BridgedFrame frame = RunBridgedFrame(steps, in, out);
  // Final review C-1: stamped whenever the list was submitted, even when the D3D12 signal then failed. Its surfaces
  // are then held until the side's progress passes `out` (for good, if that signal never comes), never freed under the
  // list. The slot's own stamp is D3D12Side::SignalProgress's, by the same rule.
  if (steps.executed || frame.d3d12_signalled) {
    color_.last_use = out;
    if (steps.used_mask) {
      mask_.last_use = out;
      mask_fresh_ = false;
    }
    if (ring_slot != nullptr) {
      ring_slot->shared.last_use = out;  // Plan 18: the slot, in place of UPLIFT_MV's share
    } else if (motion12 != nullptr) {
      motion_.last_use = out;
    }
  }
  if (frame.d3d12_signalled) {
    watchdog_.Submitted(out, std::chrono::steady_clock::now());  // final review I-2: from submission, after recording
  }
  if (!frame.failure.empty()) {
    latch_ = std::format("The {} bridge stopped: {}", label_, frame.failure);
    nr::Log(nr::LogLevel::ERR, latch_);
  }
  return frame.wrote;
}

MaskCopy D3D11Bridge::CopyMask(ID3D11Resource* mask) {
  if (!latch_.empty()) return {};
  const std::optional<D3D11_TEXTURE2D_DESC> description = TextureDesc11(mask);
  const std::optional<color::MaskFormatInfo> format =
      (description ? color::DescribeMaskFormat(description->Format) : std::nullopt);
  if (!format || description->SampleDesc.Count != 1u) {
    ReleaseMask();
    return {.unsupported = true};
  }
  const nr::Size size = {description->Width, description->Height};
  // Shared in the typed view format: D3D11 copies between members of one typeless group (a typeless RGBA8 mask too).
  // OpenSharedResource1 refuses (E_INVALIDARG) a D3D12 texture made without ALLOW_RENDER_TARGET, whatever its format,
  // and one outside the display formats unless it is also simultaneous-access (the shareable-formats gate). RGBA32F
  // does not open even then, and stays unsupported.
  if (!mask_.d3d12 || mask_.size != size || mask_.format != format->view_format) {
    Retire(&mask_);
    mask_fresh_ = false;
    if (mask_failure_ && mask_failure_->Matches(size, format->view_format)) return {};
    HRESULT share_result = S_OK;
    if (!CreateShared(size, format->view_format,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                      format->bytes_per_pixel, L"Uplift shared mask", &mask_, &share_result)) {
      // Logged once: not retried for this size and format (final review Minor 3).
      mask_failure_ = ShareFailure{.size = size, .format = format->view_format, .result = share_result};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with the private Direct3D 12 device (DXGI_FORMAT {}, HRESULT {:#010x})",
               static_cast<int>(format->view_format), static_cast<uint32_t>(share_result));
      return {};
    }
    mask_failure_.reset();
  }
  context11_->CopyResource(mask_.d3d11.Get(), mask);
  mask_fresh_ = true;
  return {.size = size};
}

void D3D11Bridge::ReleaseMask() {
  Retire(&mask_);
  mask_fresh_ = false;
}

void D3D11Bridge::Stop(std::string reason) {
  if (latch_.empty()) {
    latch_ = std::move(reason);
  }
  // Each fence up to the highest value a wait on it was queued for; nothing waits beyond those. A refused signal (a
  // removed private device) needs no retry: that fence already reads UINT64_MAX, above every wait.
  if (to11_->GetCompletedValue() < waited11_) {
    to11_->Signal(waited11_);
  }
  if (to12_->GetCompletedValue() < waited12_) {
    to12_->Signal(waited12_);
  }
}

bool D3D11Bridge::DiagnosticWaitForGameSignal(DWORD cap_ms) {
  context11_->Flush();
  const ULONGLONG deadline = GetTickCount64() + cap_ms;
  while (to12_->GetCompletedValue() < waited12_) {
    if (GetTickCount64() >= deadline) return false;
    Sleep(1u);
  }
  return true;
}

uint64_t D3D11Bridge::SharedBytes() const {
  return color_.bytes + mask_.bytes + motion_.bytes + dlss_image_.bytes + dlss_motion_.bytes + dlss_exposure_.bytes + dlss_swap_.bytes + RingBytes();
}

uint64_t D3D11Bridge::RingBytes() const {
  uint64_t bytes = 0u;
  for (const RingSlot& slot : ring_) {
    bytes += slot.shared.bytes;
  }
  return bytes;
}

std::string D3D11Bridge::StatusLine(addon::Placement placement) const {
  // Plan 18 (design §5): the Details line names the DLSS stage NR runs at.
  const std::string_view where = (placement == addon::Placement::AFTER_DLSS        ? "NR after DLSS on Uplift's private Direct3D 12 device"
                                  : placement == addon::Placement::BEFORE_UPSCALING ? "NR before upscaling on Uplift's private Direct3D 12 device"
                                                                                    : "NR runs on a private Direct3D 12 device");
  std::string line = std::format("{}: {} (shared {:.1f} MiB)", label_, where, static_cast<double>(SharedBytes()) / (1024.0 * 1024.0));
  if (busy_skips_ > 0u) {
    line += std::format(", {} frame(s) without NR while the bridge was busy", busy_skips_);
  }
  if (const uint64_t ring = RingBytes(); ring > 0u) {
    // Plan 18 (design §4): as Vulkan's Details line says its copies.
    line += std::format("; DLSS's motion vectors copied in the game's frame ({:.1f} MiB)", static_cast<double>(ring) / (1024.0 * 1024.0));
  }
  return line;
}

}  // namespace uplift::bridge
