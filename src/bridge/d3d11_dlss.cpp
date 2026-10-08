#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include "bridge/d3d11_bridge.hpp"
#include "color/encoding.hpp"
#include "nr/log.hpp"

// Plan 18 (design §3, §4, §6): the Direct3D 11 bridge's DLSS stages, inside the game's hooked evaluate on its immediate context: the mid-frame hand-off
// (RunDlss) and the shares it keeps, and DLSS's vectors copied for the Present path (CopyPresentMotion: copies only, no fence). The hand-off's order is
// RunBridgedFrame's, the one that cannot leave the game's GPU waiting on a value nobody signals.

namespace uplift::bridge {
namespace {

// Plan 18: a typed share format for DLSS's vectors (RG16F, RG32F, and RGBA16F since fix round 1, M-6, as Direct3D 12's MotionViewFormat reads them) and
// exposure (R32F, R16F), their typeless families included. RGBA32F vectors are not shared: Direct3D 11 opens no shared Direct3D 12 RGBA32F texture
// (bridge_gpu_test.cpp's UPLIFT_MASK finding); NR then runs without them.
struct ShareFormat {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t bytes_per_pixel = 0u;
};
std::optional<ShareFormat> MotionShareFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:          return ShareFormat{DXGI_FORMAT_R16G16_FLOAT, 4u};
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:          return ShareFormat{DXGI_FORMAT_R32G32_FLOAT, 8u};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return ShareFormat{DXGI_FORMAT_R16G16B16A16_FLOAT, 8u};
    default:                                return std::nullopt;
  }
}
std::optional<ShareFormat> ExposureShareFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: return ShareFormat{DXGI_FORMAT_R32_FLOAT, 4u};
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: return ShareFormat{DXGI_FORMAT_R16_FLOAT, 2u};
    default:                    return std::nullopt;
  }
}
// D3D11 opens a shared D3D12 texture only when it allows render targets, and outside R8, R16 and the display formats only when it is also simultaneous-access
// (Plan 7 design §9 g).
constexpr D3D12_RESOURCE_FLAGS SHARE_FLAGS = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

// Plan 18 (fix round 1, M-5): a share the API refused for its format (or format and flags), as RGBA32F is: the stage cannot run on this image. Anything else
// (out of memory, a removed device) is not the format's: that evaluate goes without NR, and the stage stays offered.
bool IsFormatRefusal(HRESULT result) {
  return result == E_INVALIDARG || result == DXGI_ERROR_UNSUPPORTED;
}

// `rect` as a D3D11 copy box.
D3D11_BOX BoxOf(const nr::Rect& rect) {
  return {.left = rect.x, .top = rect.y, .front = 0u, .right = rect.x + rect.width, .bottom = rect.y + rect.height, .back = 1u};
}

}  // namespace

nr::Rect ResolveRegion11(ID3D11Resource* texture, nr::Rect region) {
  const std::optional<D3D11_TEXTURE2D_DESC> description = TextureDesc11(texture);
  if (!description) return {};
  if (region.width == 0u || region.height == 0u) return {.x = 0u, .y = 0u, .width = description->Width, .height = description->Height};
  if (uint64_t{region.x} + region.width > description->Width || uint64_t{region.y} + region.height > description->Height) return {};
  return region;
}

bool D3D11Bridge::EnsureShared(Shared* shared, std::optional<ShareFailure>* failure, nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                               uint32_t bytes_per_pixel, std::string_view what) {
  if (shared->d3d12 && shared->size == size && shared->format == format && shared->flags == flags) return true;
  Retire(shared);
  if (*failure && (*failure)->Matches(size, format, flags)) return false;
  HRESULT result = S_OK;
  if (!CreateShared(size, format, flags, bytes_per_pixel, L"Uplift shared DLSS texture", shared, &result)) {
    *failure = ShareFailure{.size = size, .format = format, .result = result, .flags = flags};
    nr::Logf(nr::LogLevel::WARN, "{} could not be shared with the private Direct3D 12 device (DXGI_FORMAT {}, HRESULT {:#010x})", what,
             static_cast<int>(format), static_cast<uint32_t>(result));
    return false;
  }
  failure->reset();
  return true;
}

void D3D11Bridge::ReleaseDlss() {
  Retire(&dlss_image_);
  Retire(&dlss_motion_);
  Retire(&dlss_exposure_);
  Retire(&dlss_swap_);
}

DlssHandoff D3D11Bridge::RunDlss(const DlssHandoffInput& input, const DlssRecorder& record, std::chrono::steady_clock::time_point now) {
  CheckWatchdog(now);  // Plan 18 (design §6): the mid-frame waits are covered as the present's are
  if (!latch_.empty()) return {.skipped = DlssSkip::STOPPED};
  if (input.region.width == 0u || input.region.height == 0u) return {.skipped = DlssSkip::NO_INPUT};
  const bool after = (input.stage == DlssStage::AFTER_DLSS);
  const std::optional<D3D11_TEXTURE2D_DESC> image = TextureDesc11(input.image);
  const std::optional<color::UavFormatInfo> format = (image ? color::DescribeUavFormat(image->Format) : std::nullopt);
  const auto unsupported = [&image](HRESULT result) {
    return DlssHandoff{
        .skipped = DlssSkip::UNSUPPORTED_FORMAT,
        .problem = std::format("Direct3D 11 cannot share DLSS's image (DXGI_FORMAT {}{}) with Uplift's Direct3D 12 device", (image ? static_cast<int>(image->Format) : 0),
                               (FAILED(result) ? std::format(", HRESULT {:#010x}", static_cast<uint32_t>(result)) : std::string()))};
  };
  if (!image || !format || image->SampleDesc.Count != 1u || image->ArraySize != 1u) return unsupported(S_OK);
  if (!side_->SlotFree()) {
    ++busy_skips_;  // this evaluate goes without NR rather than waiting
    return {.skipped = DlssSkip::BUSY};
  }
  // After DLSS: the region at (0, 0), written by NR's decode (a UAV). Before upscaling: the whole colour, so the region keeps its place, only read.
  const nr::Size image_size = (after ? nr::Size{input.region.width, input.region.height} : nr::Size{image->Width, image->Height});
  const D3D12_RESOURCE_FLAGS image_flags = (SHARE_FLAGS | (after ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE));
  if (!EnsureShared(&dlss_image_, &dlss_image_failure_, image_size, format->uav_format, image_flags, format->bytes_per_pixel, "DLSS's image")) {
    const HRESULT result = (dlss_image_failure_ ? dlss_image_failure_->result : E_FAIL);
    // Fix round 1 (M-5): out of memory is not the format's; the stage is not greyed for the context's life (EnsureShared logged it once).
    if (!IsFormatRefusal(result)) return {.skipped = DlssSkip::SHARE_FAILED};
    return unsupported(result);
  }
  if (after) {
    Retire(&dlss_swap_);
  } else if (!EnsureShared(&dlss_swap_, &dlss_swap_failure_, image_size, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 8u,
                           "Before upscaling's colour")) {
    return {.skipped = DlssSkip::SHARE_FAILED};
  }
  // DLSS's vectors and exposure cross when they can be shared; NR runs without them otherwise (EnsureShared logged why).
  ID3D12Resource* motion12 = nullptr;
  const std::optional<D3D11_TEXTURE2D_DESC> motion = TextureDesc11(input.motion);
  const std::optional<ShareFormat> motion_format = (motion ? MotionShareFormat(motion->Format) : std::nullopt);
  // Fix round 1 (M-4): at the vectors' whole size, so a dynamic render size (a region that changes every evaluate) never remakes the share here.
  if (motion_format && motion->SampleDesc.Count == 1u && input.motion_region.width != 0u && input.motion_region.height != 0u
      && EnsureShared(&dlss_motion_, &dlss_motion_failure_, {motion->Width, motion->Height}, motion_format->format, SHARE_FLAGS,
                      motion_format->bytes_per_pixel, "DLSS's motion vectors")) {
    motion12 = dlss_motion_.d3d12.Get();
  } else if (input.motion == nullptr) {
    Retire(&dlss_motion_);
  }
  ID3D12Resource* exposure12 = nullptr;
  const std::optional<D3D11_TEXTURE2D_DESC> exposure = TextureDesc11(input.exposure);
  const std::optional<ShareFormat> exposure_format = (exposure ? ExposureShareFormat(exposure->Format) : std::nullopt);
  if (exposure_format && EnsureShared(&dlss_exposure_, &dlss_exposure_failure_, {1u, 1u}, exposure_format->format, SHARE_FLAGS,
                                      exposure_format->bytes_per_pixel, "DLSS's exposure")) {
    exposure12 = dlss_exposure_.d3d12.Get();
  } else if (input.exposure == nullptr) {
    Retire(&dlss_exposure_);
  }
  // The steps on this bridge's objects; RunBridgedFrame owns their order (D3D11 design §1.3).
  class Steps final : public BridgeSteps {
   public:
    Steps(D3D11Bridge& bridge, const DlssHandoffInput& input, bool after, ID3D12Resource* motion12, ID3D12Resource* exposure12, const DlssRecorder& record)
        : bridge_(bridge), input_(input), after_(after), motion12_(motion12), exposure12_(exposure12), record_(record) {}
    bool used_mask = false;
    bool executed = false;  // the list was submitted: its surfaces are in use until the side's progress passes it

    void CopyIn() override {
      const nr::Rect& region = input_.region;
      const D3D11_BOX region_box = BoxOf(region);
      bridge_.context11_->CopySubresourceRegion(bridge_.dlss_image_.d3d11.Get(), 0u, (after_ ? 0u : region.x), (after_ ? 0u : region.y), 0u, input_.image,
                                                0u, &region_box);
      if (motion12_ != nullptr) {
        const D3D11_BOX motion_box = BoxOf(input_.motion_region);
        bridge_.context11_->CopySubresourceRegion(bridge_.dlss_motion_.d3d11.Get(), 0u, 0u, 0u, 0u, input_.motion, 0u, &motion_box);
      }
      if (exposure12_ != nullptr) {
        const D3D11_BOX texel = BoxOf({.x = 0u, .y = 0u, .width = 1u, .height = 1u});
        bridge_.context11_->CopySubresourceRegion(bridge_.dlss_exposure_.d3d11.Get(), 0u, 0u, 0u, 0u, input_.exposure, 0u, &texel);
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
      const nr::Rect& region = input_.region;
      const DlssHandoffTargets targets = {
          .image = bridge_.dlss_image_.d3d12.Get(),
          .region = (after_ ? nr::Rect{.x = 0u, .y = 0u, .width = region.width, .height = region.height} : region),
          .motion = motion12_,
          .motion_region = (motion12_ != nullptr ? nr::Rect{.x = 0u, .y = 0u, .width = input_.motion_region.width, .height = input_.motion_region.height}
                                                 : nr::Rect{}),
          .exposure = exposure12_,
          .mask = (bridge_.mask_fresh_ ? bridge_.mask_.d3d12.Get() : nullptr),
          .swap = (after_ ? nullptr : bridge_.dlss_swap_.d3d12.Get()),
      };
      if (targets.mask != nullptr) {
        Transition(list, targets.mask, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      bool recorded = true;
      try {
        *wrote = record_(list, targets);
      } catch (...) {
        // Half a recording never executes; the queue still signals, so D3D11 never waits in vain.
        nr::Log(nr::LogLevel::ERR, "NR's recording of a DLSS stage on the private Direct3D 12 device threw; this evaluate goes without NR");
        recorded = false;
        *wrote = false;
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
    // progress first: when the queue refuses it, to11_ is never signalled, so D3D11 never waits for this evaluate.
    bool SignalD3D12(uint64_t value) override {
      return bridge_.side_->SignalProgress(value) && SUCCEEDED(bridge_.side_->Queue()->Signal(bridge_.to11_.Get(), value));
    }
    bool WaitD3D11(uint64_t value) override {
      bridge_.waited11_ = value;  // for Stop, whether or not the context took it
      return SUCCEEDED(bridge_.context11_->Wait(bridge_.to11_11_.Get(), value));
    }
    // After DLSS: NR's result back into DLSS's output region. Before upscaling copies nothing: DLSS reads the swap texture itself.
    void CopyOut() override {
      if (!after_) return;
      const D3D11_BOX box = BoxOf({.x = 0u, .y = 0u, .width = input_.region.width, .height = input_.region.height});
      bridge_.context11_->CopySubresourceRegion(input_.image, 0u, input_.region.x, input_.region.y, 0u, bridge_.dlss_image_.d3d11.Get(), 0u, &box);
    }

   private:
    D3D11Bridge& bridge_;
    const DlssHandoffInput& input_;
    bool after_;
    ID3D12Resource* motion12_;
    ID3D12Resource* exposure12_;
    const DlssRecorder& record_;
  };
  Steps steps(*this, input, after, motion12, exposure12, record);
  const uint64_t in = ++last_value_;  // monotonic even when a step fails
  const uint64_t out = ++last_value_;
  const BridgedFrame frame = RunBridgedFrame(steps, in, out);
  // As Run's (final review C-1): stamped whenever the list was submitted, even when the D3D12 signal then failed, so nothing it reads is freed under it.
  if (steps.executed || frame.d3d12_signalled) {
    dlss_image_.last_use = out;
    if (!after) {
      dlss_swap_.last_use = out;
    }
    if (motion12 != nullptr) {
      dlss_motion_.last_use = out;
    }
    if (exposure12 != nullptr) {
      dlss_exposure_.last_use = out;
    }
    if (steps.used_mask) {
      mask_.last_use = out;
      mask_fresh_ = false;
    }
  }
  if (frame.d3d12_signalled) {
    // The clock starts here, at this evaluate's time (from before the recording, a few milliseconds earlier than Run's submission time of final review
    // I-2): the hand-off's own `now`, so a test can drive the 2 s with its own times.
    watchdog_.Submitted(out, now);
  }
  if (!frame.failure.empty()) {
    latch_ = std::format("The {} bridge stopped: {}", label_, frame.failure);
    nr::Log(nr::LogLevel::ERR, latch_);
  }
  return {.wrote = frame.wrote, .executed = steps.executed, .color = ((!after && frame.wrote) ? dlss_swap_.d3d11.Get() : nullptr)};
}

bool D3D11Bridge::CopyPresentMotion(ID3D11Resource* motion, nr::Rect region, float scale_x, float scale_y) {
  if (!latch_.empty()) return false;
  const std::optional<D3D11_TEXTURE2D_DESC> description = TextureDesc11(motion);
  const std::optional<ShareFormat> format = (description ? MotionShareFormat(description->Format) : std::nullopt);
  region = ResolveRegion11(motion, region);
  if (!format || description->SampleDesc.Count != 1u || region.width == 0u || region.height == 0u) return false;
  const uint64_t frame = presents_ + 1u;
  RingSlot& slot = ring_[sources::PresentMotionSlotOf(frame, ring_slots_)];
  if (slot.shared.d3d12 && slot.shared.last_use > side_->Completed()) {
    // Still read: no copy this frame (its present binds an older one). Fix round 1 (M6): the ring grows once; the slots in use keep their copies and marks.
    if (sources::PresentMotionGrows(ring_slots_, sources::PresentMotionWrite::SLOT_BUSY)) {
      nr::Logf(nr::LogLevel::INFO, "{}: the private queue runs too far behind for four slots: DLSS's motion copies for Present grow from {} to {} slots",
               label_, ring_slots_, RING_SLOTS);
      ring_slots_ = RING_SLOTS;
    }
    return false;
  }
  // Fix round 1 (M-4): at the vectors' whole size; the region is copied to (0, 0) and the slot keeps it, for the Present path's subrect.
  if (!EnsureShared(&slot.shared, &ring_failure_, {description->Width, description->Height}, format->format, SHARE_FLAGS, format->bytes_per_pixel,
                    "DLSS's motion vectors for Present")) {
    return false;
  }
  const D3D11_BOX box = BoxOf(region);
  context11_->CopySubresourceRegion(slot.shared.d3d11.Get(), 0u, 0u, 0u, 0u, motion, 0u, &box);
  slot.region = {.x = 0u, .y = 0u, .width = region.width, .height = region.height};
  slot.frame = frame;
  slot.scale_x = scale_x;
  slot.scale_y = scale_y;
  return true;
}

void D3D11Bridge::ReleasePresentMotion() {
  // Test phase (the final review's ring question, four slots or three): what the ring held, once per use, as it goes. Retire empties each slot, so a later
  // call finds nothing and logs nothing.
  if (const uint64_t bytes = RingBytes(); bytes > 0u) {
    const auto slots = std::count_if(ring_.begin(), ring_.end(), [](const RingSlot& slot) { return slot.shared.bytes > 0u; });
    nr::Logf(nr::LogLevel::INFO, "{}: DLSS's motion vectors' copies for Present released ({} of {} slots, {:.1f} MiB)", label_, slots, ring_slots_,
             static_cast<double>(bytes) / (1024.0 * 1024.0));
  }
  for (RingSlot& slot : ring_) {
    Retire(&slot.shared);
    slot.region = {};
    slot.frame = 0u;
  }
  ring_slots_ = sources::PRESENT_MOTION_MIN_SLOTS;  // fix round 1 (M6): the next copies start small again
}

}  // namespace uplift::bridge
