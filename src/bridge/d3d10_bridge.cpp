#include "bridge/d3d10_bridge.hpp"

#include <format>
#include <utility>

#include "color/encoding.hpp"
#include "memory_units.hpp"
#include "nr/log.hpp"

namespace uplift::bridge {
namespace {

using Microsoft::WRL::ComPtr;

// Bytes per texel of the motion share: DXGI_FORMAT_R16G16_FLOAT, two 16-bit channels.
constexpr uint32_t MOTION_BYTES_PER_TEXEL = 4u;
// The relay's latch reason when the D3D11 bridge stopped first.
constexpr char BRIDGE_STOPPED_FIRST[] = "the Direct3D 11 bridge stopped";

}  // namespace

std::unique_ptr<D3D10Bridge> D3D10Bridge::Create(ID3D10Device* device, ComPtr<ID3D11Device>* relay_created,
                                                 ComPtr<ID3D12Device>* created, std::string* error) {
  std::unique_ptr<D3D10Bridge> bridge(new D3D10Bridge());
  bridge->game_ = device;
  bridge->relay_ = KeyedRelay::Create(device, relay_created, error);
  if (!bridge->relay_) return nullptr;  // `error` says why
  bridge->bridge_ = D3D11Bridge::Create(bridge->relay_->Relay(), created, error, "Direct3D 10");
  if (!bridge->bridge_) return nullptr;  // `error` says why
  // The two latches are one: each side passes its own to the other inside the call that sets it, and the Stopped() checks end the recursion.
  D3D10Bridge* const self = bridge.get();
  bridge->relay_->OnLatch([self] {
    if (!self->bridge_->Stopped()) {
      std::string reason = KeyedRelay::Sentence(self->relay_->Reason());
      nr::Log(nr::LogLevel::ERR, reason);
      self->bridge_->Stop(std::move(reason));  // the D3D11 bridge's latch is the one, its fences signalled
    }
  });
  bridge->bridge_->OnLatch([self] {
    if (!self->relay_->Stopped()) {
      self->relay_->Stop(BRIDGE_STOPPED_FIRST);
    }
  });
  nr::Log(nr::LogLevel::INFO, "Direct3D 10: frames cross through a Direct3D 11 relay device (keyed mutex)");
  return bridge;
}

D3D10Bridge::~D3D10Bridge() {
  // The handlers name both members: they go before either is destroyed. Create may have returned half made.
  if (relay_) relay_->OnLatch({});
  if (bridge_) bridge_->OnLatch({});
}

void D3D10Bridge::FlushRelay() {
  relay_->Flush();
  bridge_->TakeReleased();  // this flush destroys what the D3D11 bridge released too
}

BridgeFrame D3D10Bridge::BeginFrame(ID3D10Resource* back_buffer, bool enabled, bool running,
                                    std::chrono::steady_clock::time_point now) {
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
    bridge_problem_.reset();
  }
  // The keyed textures live only while NR runs on a usable image and no bridge stopped. D3D12 never
  // touches them, so they go at once; the relay's releases are destroyed by the flush at the end of this call.
  const auto retire_keyed = [this] {
    relay_->Drop(&color_);
    relay_->Drop(&mask_);
    relay_->Drop(&motion_);
  };
  std::string problem;
  const std::optional<D3D10_TEXTURE2D_DESC> description = TextureDesc(back_buffer);
  const std::optional<color::FormatInfo> format = (description ? color::DescribeFormat(description->Format) : std::nullopt);
  const nr::Size size = (description ? nr::Size{description->Width, description->Height} : nr::Size{});
  const DXGI_FORMAT back_buffer_format = (description ? description->Format : DXGI_FORMAT_UNKNOWN);
  if (!description) {
    problem = "The back buffer is not a 2D texture";
  } else if (description->SampleDesc.Count != 1u) {
    problem = "Multisampled back buffers are not supported on Direct3D 10";
  } else if (!format) {
    problem = std::format("Unsupported back-buffer format (DXGI_FORMAT {})", static_cast<int>(description->Format));
  }
  if (!(running && problem.empty()) || Stopped()) {
    retire_keyed();
  } else {
    if (color_.relay && (color_.size != size || color_.format != back_buffer_format)) {
      relay_->Drop(&color_);
    }
    if (!color_.relay && !(color_failure_ && color_failure_->Matches(size, back_buffer_format))) {
      HRESULT result = S_OK;
      if (relay_->CreateKeyed(size, back_buffer_format, format->bytes_per_pixel, &color_, &result)) {
        color_failure_.reset();
      } else {
        color_failure_ = ShareFailure{.size = size, .format = back_buffer_format, .cause = {.result = result}};
      }
    }
  }
  // Reported while NR is off too, so the Session does not load for a back buffer the relay cannot share, nor for
  // one the D3D11 bridge could not share with the private device: it describes a back buffer only while NR runs.
  if (problem.empty() && color_failure_ && color_failure_->Matches(size, back_buffer_format)) {
    problem = std::format("The back buffer could not be shared with the Direct3D 11 relay (DXGI_FORMAT {}, HRESULT {:#010x})",
                          static_cast<int>(back_buffer_format), static_cast<uint32_t>(color_failure_->cause.result));
  } else if (problem.empty() && bridge_problem_ && bridge_problem_->size == size && bridge_problem_->format == back_buffer_format) {
    problem = bridge_problem_->text;
  }
  // The D3D11 bridge runs its watchdog and retires its own surfaces at every present; no keyed colour: no copy this frame.
  const BridgeFrame frame = bridge_->BeginFrame(color_.relay.Get(), enabled, color_.relay != nullptr, now);
  shared_ready_ = (frame.color != nullptr);
  // Its verdict on the colour it was handed replaces the last one (a stopped bridge's frame.problem is the latch, not this).
  if (color_.relay && !Stopped()) {
    if (frame.problem.empty()) {
      bridge_problem_.reset();
    } else {
      bridge_problem_ = BridgeProblem{.size = size, .format = back_buffer_format, .text = std::string(frame.problem)};
    }
  }
  if (problem != problem_) {
    if (!problem.empty()) {
      nr::Log(nr::LogLevel::WARN, problem);
    }
    problem_ = std::move(problem);
  }
  const bool stopped = Stopped();
  if (stopped) {
    // Stopped inside this very call (the watchdog): the keyed textures go with the latch, as the shared ones.
    retire_keyed();
  }
  // D3D11 destroys a released object only at its context's next Flush, and the relay has no Present. With
  // NR off, or after a latch, no Run or copy comes to flush it, so the keyed textures and the relay's opened references
  // to the shared colour, motion and mask (the D3D11 bridge's releases) would stay in VRAM. Only while something is pending.
  const bool released = bridge_->TakeReleased();
  if (released || relay_->Dirty()) {
    FlushRelay();
  }
  if (stopped) return {.problem = frame.problem};  // the latch's text, which views the D3D11 bridge's own
  if (!problem_.empty()) return {.size = size, .format = back_buffer_format, .problem = problem_};
  return {.color = frame.color, .size = size, .format = back_buffer_format, .problem = frame.problem};
}

bool D3D10Bridge::Run(ID3D10Resource* back_buffer, ID3D10Resource* motion, const Recorder& record) {
  if (!shared_ready_ || Stopped() || !color_.relay || back_buffer == nullptr) return false;
  // LaunchPad's UPLIFT_MV crosses like the colour. Kept through a run of up to MOTION_KEEP_PRESENTS recordings without one, and retired past it
  // (LaunchPad off, or NR at the present or after the effects); a kept keyed texture is not handed over or taken, so it holds only its last use.
  Keyed* motion_keyed = nullptr;
  const std::optional<D3D10_TEXTURE2D_DESC> motion_description = TextureDesc(motion);
  if (motion_description && motion_description->Format == DXGI_FORMAT_R16G16_FLOAT
      && motion_description->SampleDesc.Count == 1u) {
    const nr::Size size = {motion_description->Width, motion_description->Height};
    if (motion_.relay && motion_.size != size) {
      relay_->Drop(&motion_);
    }
    if (!motion_.relay && !(motion_failure_ && motion_failure_->Matches(size, DXGI_FORMAT_R16G16_FLOAT))) {
      HRESULT result = S_OK;
      if (relay_->CreateKeyed(size, DXGI_FORMAT_R16G16_FLOAT, MOTION_BYTES_PER_TEXEL, &motion_, &result)) {
        motion_failure_.reset();
      } else {
        // Logged once: not retried for this size.
        motion_failure_ = ShareFailure{.size = size, .format = DXGI_FORMAT_R16G16_FLOAT, .cause = {.result = result}};
        nr::Logf(nr::LogLevel::WARN,
                 "UPLIFT_MV could not be shared with the Direct3D 11 relay (HRESULT {:#010x}); NR runs without LaunchPad's motion",
                 static_cast<uint32_t>(result));
      }
    }
    motion_keyed = (motion_.relay ? &motion_ : nullptr);
    motion_keep_.Arrived();
  } else if (motion_.relay && motion_keep_.RetireOnMiss()) {
    relay_->Drop(&motion_);  // the motion stopped: the keyed texture goes
  }
  // 1. The game's side hands the frame over (key 0 -> 1), then submits: the relay's GPU never waits for a release that
  //    was not submitted.
  if (!relay_->Hand(color_, back_buffer) || (motion_keyed != nullptr && !relay_->Hand(*motion_keyed, motion))) {
    return false;
  }
  game_->Flush();
  // 2. The relay takes it (key 1), runs the D3D11 bridge, and gives back every key it took (1 -> 0) whatever the bridge
  //    did, then submits: D3D10's GPU never waits for an unsubmitted release either.
  std::optional<KeyedRelay::KeyGuard> color_key;
  std::optional<KeyedRelay::KeyGuard> motion_key;
  if (!relay_->Take(color_, &color_key)) {
    return false;
  }
  const bool motion_taken = (motion_keyed != nullptr && relay_->Take(*motion_keyed, &motion_key));
  const bool wrote = (motion_keyed == nullptr || motion_taken)
                     && bridge_->Run(color_.relay.Get(), (motion_taken ? motion_keyed->relay.Get() : nullptr), record);
  color_key.reset();  // each key goes back here on the way out of the D3D11 bridge, and by unwinding if it threw
  motion_key.reset();
  FlushRelay();
  // 3. The game's side takes NR's result back (key 0). Its GPU waits for the relay's copy out, which waits for NR, whose
  //    D3D12 signal RunBridgedFrame submitted first. A key that could not be given stopped the bridge: no acquire.
  if (!wrote || Stopped()) return false;
  const bool copied_back = relay_->GiveBack(color_, back_buffer);
  return copied_back;
}

MaskCopy D3D10Bridge::CopyMask(ID3D10Resource* mask) {
  if (Stopped()) return {};
  const std::optional<D3D10_TEXTURE2D_DESC> description = TextureDesc(mask);
  const std::optional<color::MaskFormatInfo> format =
      (description ? color::DescribeMaskFormat(description->Format) : std::nullopt);
  if (!format || description->SampleDesc.Count != 1u) {
    ReleaseMask();
    return {.unsupported = true};
  }
  const nr::Size size = {description->Width, description->Height};
  if (mask_.relay && (mask_.size != size || mask_.format != format->view_format)) {
    relay_->Drop(&mask_);
  }
  if (!mask_.relay) {
    // Which step failed decides the mask note's wording.
    if (mask_failure_ && mask_failure_->Matches(size, format->view_format)) return {.relay_failed = !mask_failure_->cause.bridge};
    HRESULT result = S_OK;
    // In the typed view format: a D3D10 copy stays inside one typeless group, as on D3D11.
    if (!relay_->CreateKeyed(size, format->view_format, format->bytes_per_pixel, &mask_, &result)) {
      // Logged once: not retried for this size and format.
      mask_failure_ = ShareFailure{.size = size, .format = format->view_format, .cause = {.result = result}};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with the Direct3D 11 relay (DXGI_FORMAT {}, HRESULT {:#010x})",
               static_cast<int>(format->view_format), static_cast<uint32_t>(result));
      return {.relay_failed = true};
    }
    mask_failure_.reset();
  }
  // The colour's hand-off.
  if (!relay_->Hand(mask_, mask)) {
    return {};
  }
  game_->Flush();
  std::optional<KeyedRelay::KeyGuard> key;
  if (!relay_->Take(mask_, &key)) {
    return {};
  }
  const MaskCopy copied = bridge_->CopyMask(mask_.relay.Get());
  key.reset();
  if (!copied.size && !copied.unsupported) {
    // The D3D11 bridge could not share it with the private device (RGBA32F, for one): not handed over again for this
    // size and format until a re-enable, instead of two full-frame copies a frame for nothing.
    mask_failure_ = ShareFailure{.size = size, .format = format->view_format, .cause = {.result = E_FAIL, .bridge = true}};
    relay_->Drop(&mask_);
  }
  FlushRelay();
  return copied;
}

void D3D10Bridge::ReleaseMask() {
  relay_->Drop(&mask_);
  bridge_->ReleaseMask();  // its release is flushed by the next BeginFrame
}

void D3D10Bridge::Stop(std::string reason) {
  // The D3D11 bridge first: its latch is the one that signals the fences and the one the card shows, and its handler stops the relay. Stopping the relay
  // first would make the relay's handler stop the D3D11 bridge with the relay's sentence and an ERR line, which an external stop never writes.
  bridge_->Stop(reason);
  relay_->Stop(std::move(reason));
}

uint64_t D3D10Bridge::SharedBytes() const {
  return bridge_->SharedBytes() + color_.bytes + mask_.bytes + motion_.bytes;
}

std::string D3D10Bridge::StatusLine() const {
  std::string line =
      std::format("Direct3D 10: NR runs on a private Direct3D 12 device through a Direct3D 11 relay (shared {:.1f} MiB)",
                  static_cast<double>(SharedBytes()) / BYTES_PER_MIB);
  if (bridge_->BusySkips() > 0u) {
    line += std::format(", {} frame(s) without NR while the bridge was busy", bridge_->BusySkips());
  }
  return line;
}

}  // namespace uplift::bridge
