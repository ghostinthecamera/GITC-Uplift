#include "client/d3d10_client.hpp"

#include <format>
#include <utility>

#include "color/encoding.hpp"
#include "ipc/control_block.hpp"
#include "nr/log.hpp"

namespace uplift::client {
namespace {

using Microsoft::WRL::ComPtr;

constexpr double MIB = 1024.0 * 1024.0;

}  // namespace

std::unique_ptr<D3D10Client> D3D10Client::Create(ID3D10Device* device, ComPtr<ID3D11Device>* relay_created, std::string* error) {
  std::unique_ptr<D3D10Client> client(new D3D10Client());
  client->game_ = device;
  client->relay_ = bridge::KeyedRelay::Create(device, relay_created, error);
  if (!client->relay_) return nullptr;  // `error` says why
  // Fences on the relay (Windows 10 1703) were checked by KeyedRelay::Create; a relay that cannot open the helper's own falls back to KMT
  // inside the D3D11 client, as Plan 9 does for a D3D11 game.
  client->d3d11_ = D3D11Client::Create(client->relay_->Relay(), true, error, "Direct3D 10");
  if (!client->d3d11_) return nullptr;  // `error` says why
  nr::Log(nr::LogLevel::INFO, "Direct3D 10: frames cross through a Direct3D 11 relay device (keyed mutex)");
  return client;
}

void D3D10Client::SyncLatches(NrLink& link) {
  if (relay_->Stopped() && d3d11_->Latch().empty()) {
    d3d11_->Stop(relay_->Reason(), link);  // the helper latches and CPU-signals its fences; the latch text is "The Direct3D 10 bridge stopped: ..."
  } else if (!d3d11_->Latch().empty() && !relay_->Stopped()) {
    relay_->Stop(std::string(d3d11_->Latch()));
  }
}

ipc::Target D3D10Client::Describe(ID3D10Resource* back_buffer, bool enabled, bool running) {
  if (!enabled) {
    color_failure_.reset();
    mask_failure_.reset();
    motion_failure_.reset();
  }
  const std::optional<D3D10_TEXTURE2D_DESC> description = bridge::TextureDesc(back_buffer);
  const std::optional<color::FormatInfo> format = (description ? color::DescribeFormat(description->Format) : std::nullopt);
  const nr::Size size = (description ? nr::Size{description->Width, description->Height} : nr::Size{});
  const DXGI_FORMAT back_buffer_format = (description ? description->Format : DXGI_FORMAT_UNKNOWN);
  const bool usable = (description && description->SampleDesc.Count == 1u && format);
  // The keyed colour lives only while NR runs on a usable image and nothing stopped (D3D10Bridge::BeginFrame's rule): the helper's
  // shared textures are its own, and D3D12 never touches the keyed ones, so they go at once.
  if (!(running && usable) || Stopped()) {
    relay_->Drop(&color_);
    relay_->Drop(&mask_);
    relay_->Drop(&motion_);
  } else {
    if (color_.relay && (color_.size != size || color_.format != back_buffer_format)) {
      relay_->Drop(&color_);
    }
    if (!color_.relay && !(color_failure_ && color_failure_->Matches(size, back_buffer_format))) {
      HRESULT result = S_OK;
      if (relay_->CreateKeyed(size, back_buffer_format, format->bytes_per_pixel, &color_, &result)) {
        color_failure_.reset();
      } else {
        color_failure_ = ShareFailure{.size = size, .format = back_buffer_format, .result = result};
      }
    }
  }
  if (relay_->Dirty()) {
    relay_->Flush();  // the relay has no Present: what it just released is destroyed only by a Flush (Plan 8's final review C-1)
  }
  // The D3D11 client describes the D3D10 image as it is (its multisampled, unsupported-format and shared-colour problems read the same);
  // it never sees the relay's keyed texture, which does not exist while NR is off. While NR is off its shared textures go.
  const std::optional<D3D11_TEXTURE2D_DESC> described =
      (description ? std::optional<D3D11_TEXTURE2D_DESC>(D3D11_TEXTURE2D_DESC{
                         .Width = description->Width,
                         .Height = description->Height,
                         .MipLevels = description->MipLevels,
                         .ArraySize = description->ArraySize,
                         .Format = description->Format,
                         .SampleDesc = description->SampleDesc,
                     })
                   : std::nullopt);
  ipc::Target target = d3d11_->Describe(described, enabled, running && color_.relay != nullptr);
  // What only this side knows: the relay could not share the colour, or it stopped (its latch reaches the D3D11 client at the next Apply).
  std::string problem;
  if (usable && color_failure_ && color_failure_->Matches(size, back_buffer_format)) {
    problem = std::format("The back buffer could not be shared with the Direct3D 11 relay (DXGI_FORMAT {}, HRESULT {:#010x})",
                          static_cast<int>(back_buffer_format), static_cast<uint32_t>(color_failure_->result));
  } else if (relay_->Stopped() && d3d11_->Latch().empty()) {
    problem = bridge::KeyedRelay::Sentence(relay_->Reason());
  }
  if (problem != problem_) {
    if (!problem.empty()) {
      nr::Log(nr::LogLevel::WARN, problem);
    }
    problem_ = std::move(problem);
  }
  if (!problem_.empty() && ipc::TextOf(target.problem).empty()) {
    ipc::CopyText(target.problem, problem_);
  }
  return target;
}

bool D3D10Client::Apply(const ipc::Reply& reply, NrLink& link) {
  SyncLatches(link);  // a key that did not come while Describe made the keyed colour
  const bool fell_back = d3d11_->Apply(reply, link);
  SyncLatches(link);
  relay_->Flush();  // Apply frees the opened shared textures (the helper's NR is off, a transport changed, a latch): only a Flush destroys them
  return fell_back;
}

bool D3D10Client::Run(ID3D10Resource* back_buffer, ID3D10Resource* motion, addon::TriggerPoint point, NrLink& link) {
  run_sent_ = false;
  SyncLatches(link);
  if (Stopped() || !color_.relay || back_buffer == nullptr) return false;
  // Plan 7's decision 3 on D3D10: LaunchPad's UPLIFT_MV crosses like the colour, and is retired when a recording comes without one.
  Keyed* motion_keyed = nullptr;
  const std::optional<D3D10_TEXTURE2D_DESC> motion_description = bridge::TextureDesc(motion);
  if (motion_description && motion_description->Format == DXGI_FORMAT_R16G16_FLOAT && motion_description->SampleDesc.Count == 1u) {
    const nr::Size size = {motion_description->Width, motion_description->Height};
    if (motion_.relay && motion_.size != size) {
      relay_->Drop(&motion_);
    }
    if (!motion_.relay && !(motion_failure_ && motion_failure_->Matches(size, DXGI_FORMAT_R16G16_FLOAT))) {
      HRESULT result = S_OK;
      if (relay_->CreateKeyed(size, DXGI_FORMAT_R16G16_FLOAT, 4u, &motion_, &result)) {
        motion_failure_.reset();
      } else {
        // Logged once: not retried for this size.
        motion_failure_ = ShareFailure{.size = size, .format = DXGI_FORMAT_R16G16_FLOAT, .result = result};
        nr::Logf(nr::LogLevel::WARN,
                 "UPLIFT_MV could not be shared with the Direct3D 11 relay (HRESULT {:#010x}); NR runs without LaunchPad's motion",
                 static_cast<uint32_t>(result));
      }
    }
    motion_keyed = (motion_.relay ? &motion_ : nullptr);
  } else {
    relay_->Drop(&motion_);
  }
  SyncLatches(link);  // a key that did not come while the keyed motion was made
  // 1. The game's side hands the frame over (key 0 -> 1), then submits: the relay's GPU never waits for a release that was not submitted.
  if (!relay_->Hand(color_, back_buffer) || (motion_keyed != nullptr && !relay_->Hand(*motion_keyed, motion))) {
    SyncLatches(link);
    return false;
  }
  game_->Flush();
  // 2. The relay takes it (key 1), runs the D3D11 client (R17 across the IPC is its own), and gives back every key it took (1 -> 0)
  //    whatever the client did, then submits: D3D10's GPU never waits for an unsubmitted release either.
  std::optional<bridge::KeyedRelay::KeyGuard> color_key;
  std::optional<bridge::KeyedRelay::KeyGuard> motion_key;
  if (!relay_->Take(color_, &color_key)) {
    SyncLatches(link);
    return false;
  }
  const bool motion_taken = (motion_keyed != nullptr && relay_->Take(*motion_keyed, &motion_key));
  SyncLatches(link);
  bool wrote = false;
  if (motion_keyed == nullptr || motion_taken) {
    wrote = d3d11_->Run(color_.relay.Get(), (motion_taken ? motion_keyed->relay.Get() : nullptr), point, link);
    run_sent_ = d3d11_->LastRunSent();
  }
  SyncLatches(link);  // the D3D11 client's own latch (a failed recording, the watchdog) is the relay's too: a key released below says nothing more
  color_key.reset();  // each key goes back here on the way out of the D3D11 client, and by unwinding if it threw
  motion_key.reset();
  SyncLatches(link);
  relay_->Flush();
  // 3. The game's side takes NR's result back (key 0). Its GPU waits for the relay's copy out, which waits for the helper's NR, whose
  //    signal the reply said was submitted before any D3D11 wait was queued. A latch: no acquire.
  if (!wrote || Stopped()) return false;
  const bool copied_back = relay_->GiveBack(color_, back_buffer);
  SyncLatches(link);
  return copied_back;
}

MaskCopy D3D10Client::CopyMask(ID3D10Resource* mask, NrLink& link) {
  SyncLatches(link);
  if (d3d11_->Kind() != ipc::Transport::FENCED) return {.unsupported = true};  // no mask or motion on the CPU-ordered path (the notes say so)
  if (Stopped()) return {};
  const std::optional<D3D10_TEXTURE2D_DESC> description = bridge::TextureDesc(mask);
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
    if (mask_failure_ && mask_failure_->Matches(size, format->view_format)) return {.relay_failed = !mask_failure_->helper};
    HRESULT result = S_OK;
    // In the typed view format: a D3D10 copy stays inside one typeless group, as on D3D11.
    if (!relay_->CreateKeyed(size, format->view_format, format->bytes_per_pixel, &mask_, &result)) {
      // Logged once: not retried for this size and format.
      mask_failure_ = ShareFailure{.size = size, .format = format->view_format, .result = result};
      nr::Logf(nr::LogLevel::WARN, "UPLIFT_MASK could not be shared with the Direct3D 11 relay (DXGI_FORMAT {}, HRESULT {:#010x})",
               static_cast<int>(format->view_format), static_cast<uint32_t>(result));
      return {.relay_failed = true};
    }
    mask_failure_.reset();
  }
  SyncLatches(link);  // a key that did not come while the keyed mask was made
  // The colour's hand-off (design §3.1).
  if (!relay_->Hand(mask_, mask)) {
    SyncLatches(link);
    return {};
  }
  game_->Flush();
  std::optional<bridge::KeyedRelay::KeyGuard> key;
  if (!relay_->Take(mask_, &key)) {
    SyncLatches(link);
    return {};
  }
  const MaskCopy copied = d3d11_->CopyMask(mask_.relay.Get(), link);
  key.reset();
  SyncLatches(link);
  if (!copied.size && !copied.unsupported && !copied.busy) {
    // The helper could not share it (RGBA32F, for one): not handed over again for this size and format until a re-enable, instead of two
    // full-frame copies a frame for nothing.
    mask_failure_ = ShareFailure{.size = size, .format = format->view_format, .result = E_FAIL, .helper = true};
    relay_->Drop(&mask_);
  }
  relay_->Flush();
  return copied;
}

void D3D10Client::ReleaseMask() {
  relay_->Drop(&mask_);
  d3d11_->ReleaseMask();
  if (relay_->Dirty()) {
    relay_->Flush();  // the D3D11 client's own mask surface is destroyed by a flush too, at the latest this present's Apply
  }
}

void D3D10Client::ReleaseAll() {
  relay_->Drop(&color_);
  relay_->Drop(&mask_);
  relay_->Drop(&motion_);
  const bool held = d3d11_->ReleaseAll();
  color_failure_.reset();
  mask_failure_.reset();
  motion_failure_.reset();
  // HelperFront::Present calls this every present while the helper is not running: an empty flush is a kernel call each time (batch 3 review,
  // minor 4). The relay flushes what it released (Dirty) and what the D3D11 client released here (held).
  if (held || relay_->Dirty()) {
    relay_->Flush();
  }
}

std::string D3D10Client::Line() const {
  const std::string_view transport = (d3d11_->Kind() == ipc::Transport::FENCED ? "shared textures and fences" : "shared texture, CPU-ordered");
  return std::format("Direct3D 10 ({}): NR runs in Uplift's 64-bit helper through a Direct3D 11 relay ({}, {:.1f} MiB)", BITNESS, transport,
                     static_cast<double>(SharedBytes()) / MIB);
}

}  // namespace uplift::client
