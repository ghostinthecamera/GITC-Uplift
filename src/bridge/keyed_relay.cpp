#include "bridge/keyed_relay.hpp"

#include <dxgi1_6.h>

#include <format>
#include <utility>

#include "addon/environment.hpp"

namespace uplift::bridge {
namespace {

using Microsoft::WRL::ComPtr;

// D3D10 design §2 rule 3: each key is released on this thread just before it is acquired, so an acquire never waits for a frame, however
// slow; only a broken hand-off reaches this. As the watchdog's limit. (Verdict "A with a CPU wait": 10000.)
constexpr DWORD KEY_TIMEOUT_MS = 2000u;

}  // namespace

std::optional<D3D10_TEXTURE2D_DESC> TextureDesc(ID3D10Resource* resource) {
  ComPtr<ID3D10Texture2D> texture;
  if (resource == nullptr || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return std::nullopt;
  D3D10_TEXTURE2D_DESC description = {};
  texture->GetDesc(&description);
  return description;
}

std::unique_ptr<KeyedRelay> KeyedRelay::Create(ID3D10Device* game, ComPtr<ID3D11Device>* relay_created, std::string* error) {
  const auto fail = [error](std::string text) {
    *error = std::move(text);
    return std::unique_ptr<KeyedRelay>();
  };
  std::unique_ptr<KeyedRelay> relay(new KeyedRelay());
  relay->game_ = game;
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adapter_description = {};
  if (FAILED(game->QueryInterface(IID_PPV_ARGS(&dxgi_device))) || FAILED(dxgi_device->GetAdapter(&adapter))
      || FAILED(adapter->GetDesc(&adapter_description))) {
    return fail("the game's adapter could not be read");
  }
  if (adapter_description.VendorId != addon::NVIDIA_VENDOR_ID) {
    return fail("Uplift needs an NVIDIA GPU; this game renders on another adapter");
  }
  relay->luid_ = adapter_description.AdapterLuid;
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> same_adapter;
  if (FAILED(CreateDXGIFactory2(0u, IID_PPV_ARGS(&factory)))
      || FAILED(factory->EnumAdapterByLuid(adapter_description.AdapterLuid, IID_PPV_ARGS(&same_adapter)))) {
    return fail("the game's adapter could not be found by its LUID");
  }
  // Under ReShade this returns its proxy and raises init_device for it (reshade d3d11/d3d11.cpp), like any D3D11 device.
  static constexpr D3D_FEATURE_LEVEL LEVELS[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (const HRESULT result = D3D11CreateDevice(same_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0u, LEVELS, 2u,
                                               D3D11_SDK_VERSION, relay_created->ReleaseAndGetAddressOf(), nullptr, nullptr);
      FAILED(result)) {
    return fail(std::format("the Direct3D 11 relay device could not be created ({:#010x})", static_cast<uint32_t>(result)));
  }
  relay->relay_created_ = *relay_created;
  // The D3D11 design's §9 a, one API down: ReShade forwards CreateFence unwrapped and hooks GetDevice only on buffer and
  // texture vtables, so a fence's own device is the native relay, and the relay's copies raise no ReShade event.
  ComPtr<ID3D11Device5> proxy5;
  ComPtr<ID3D11Fence> fence;
  if (FAILED(relay_created->As(&proxy5)) || FAILED(proxy5->CreateFence(0u, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    return fail("Direct3D 11 fences need Windows 10 1703 or newer and a current driver");
  }
  fence->GetDevice(&relay->relay_);
  relay->relay_->GetImmediateContext(&relay->relay_context_);
  return relay;
}

KeyedRelay::~KeyedRelay() {
  // A partly made relay (Create failed) has no context yet. D3D11 destroys what was released only at a Flush, and the relay has no
  // Present: flush once more, so nothing waits for the relay's own destruction to be freed.
  if (relay_context_) {
    relay_context_->Flush();
  }
}

std::string KeyedRelay::Sentence(std::string_view what) {
  return std::format("The Direct3D 10 bridge stopped: {}", what);
}

void KeyedRelay::Fail(std::string_view what) {
  Stop(std::string(what));  // the owner logs it when it passes the latch on (D3D10Bridge's SyncLatches, D3D11Client::Stop)
}

void KeyedRelay::Stop(std::string reason) {
  if (latch_.empty()) {
    latch_ = std::move(reason);
  }
}

void KeyedRelay::Drop(Keyed* keyed) {
  dirty_ = dirty_ || keyed->relay != nullptr;
  *keyed = {};
}

void KeyedRelay::Flush() {
  relay_context_->Flush();
  dirty_ = false;
}

bool KeyedRelay::CreateKeyed(nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, Keyed* keyed, HRESULT* result) {
  // No initial data: an upload into a keyed texture is not ordered against D3D10's first write into it (the probe's first run), so
  // nothing writes into one except under a key.
  const D3D11_TEXTURE2D_DESC description = {
      .Width = size.width,
      .Height = size.height,
      .MipLevels = 1u,
      .ArraySize = 1u,
      .Format = format,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = static_cast<UINT>(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE),
      .CPUAccessFlags = 0u,
      .MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
  };
  Keyed created = {.size = size, .format = format, .bytes = size.Pixels() * bytes_per_pixel};
  dirty_ = true;  // a failed attempt drops what it made; the flush below (success) or the owner's (failure) frees it
  ComPtr<IDXGIResource> resource;
  HANDLE handle = nullptr;  // a KMT handle: a global name, not a kernel handle, so it is never closed
  const auto step = [result](HRESULT value) {
    *result = value;
    return SUCCEEDED(value);
  };
  if (!step(relay_->CreateTexture2D(&description, nullptr, &created.relay)) || !step(created.relay.As(&resource))
      || !step(resource->GetSharedHandle(&handle)) || !step(game_->OpenSharedResource(handle, IID_PPV_ARGS(&created.game)))
      || !step(created.relay.As(&created.relay_mutex)) || !step(created.game.As(&created.game_mutex))) {
    return false;
  }
  // The relay's own turn first: it takes key 0, gives it back and flushes, so nothing it did to make the texture can race the game's
  // first write, whose acquire then waits behind the relay's submitted work (Plan 8's batch 2 review Minor 3).
  if (const HRESULT taken = created.relay_mutex->AcquireSync(0u, KEY_TIMEOUT_MS); taken != S_OK) {
    *result = taken;  // only S_OK counts: WAIT_TIMEOUT is a success code
    return false;
  }
  {
    const KeyGuard key(this, created.relay_mutex.Get(), 0u);
  }
  Flush();
  *keyed = std::move(created);
  return true;
}

KeyedRelay::KeyGuard::~KeyGuard() {
  // A destructor must not throw, and Fail formats and logs.
  try {
    const HRESULT result = mutex_->ReleaseSync(release_key_);
    // Logged once: Fail latches, and a release that fails after a latch is its consequence.
    if (result != S_OK && !owner_->Stopped()) {
      owner_->Fail(std::format("a keyed-mutex release failed ({:#010x})", static_cast<uint32_t>(result)));
    }
  } catch (...) {
    // Out of memory while formatting: the key is released, and nothing more can be said.
  }
}

bool KeyedRelay::Hand(Keyed& keyed, ID3D10Resource* source) {
  // Only S_OK means the key came: WAIT_TIMEOUT and WAIT_ABANDONED are success codes.
  if (keyed.game_mutex->AcquireSync(0u, KEY_TIMEOUT_MS) != S_OK) {
    Fail("the game's side of a keyed-mutex hand-off timed out");
    return false;
  }
  {
    const KeyGuard key(this, keyed.game_mutex.Get(), 1u);
    game_->CopyResource(keyed.game.Get(), source);
  }
  return !Stopped();  // a key that could not be given stopped it: the relay's acquire would only time out
}

bool KeyedRelay::Take(Keyed& keyed, std::optional<KeyGuard>* key) {
  if (keyed.relay_mutex->AcquireSync(1u, KEY_TIMEOUT_MS) != S_OK) {
    Fail("the relay's side of a keyed-mutex hand-off timed out");
    return false;
  }
  key->emplace(this, keyed.relay_mutex.Get(), 0u);
  return true;
}

bool KeyedRelay::GiveBack(Keyed& keyed, ID3D10Resource* destination) {
  if (keyed.game_mutex->AcquireSync(0u, KEY_TIMEOUT_MS) != S_OK) {
    Fail("the game's side of a keyed-mutex hand-off timed out");
    return false;
  }
  const KeyGuard key(this, keyed.game_mutex.Get(), 0u);
  game_->CopyResource(destination, keyed.game.Get());
  return true;
}

}  // namespace uplift::bridge
