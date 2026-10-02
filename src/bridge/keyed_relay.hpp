#pragma once

#include <d3d10_1.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "nr/types.hpp"

namespace uplift::bridge {

// Plan 10 (design §3.1). Plan 8's relay D3D11 device and its keyed-mutex hand-offs (the D3D10 design §2 rules 2-5), moved out of
// D3D10Bridge so the 64-bit D3D10 bridge and the 32-bit D3D10 client share them. ReShade-free; the CPU never waits for the GPU (an
// acquire only waits for a release this thread made just before, capped at 2 s, and only S_OK counts). One latch. Not thread-safe:
// the add-on's lock. Destroy it outside that lock: the relay is ReShade's proxy, whose last release raises destroy_device.
class KeyedRelay {
 public:
  // A relay texture opened on the game's D3D10 device, with its keyed mutex on each side. Key 0 is the game's side, key 1 the relay's.
  struct Keyed {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> relay;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> relay_mutex;
    Microsoft::WRL::ComPtr<ID3D10Texture2D> game;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> game_mutex;
    nr::Size size;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t bytes = 0u;
  };

  // A key this thread holds: `release_key` goes back when the guard goes, so every acquire that returned S_OK is released on every
  // path, exceptions included. A failed release is logged once and latches the relay.
  class KeyGuard {
   public:
    KeyGuard(KeyedRelay* owner, IDXGIKeyedMutex* mutex, UINT release_key)
        : owner_(owner), mutex_(mutex), release_key_(release_key) {}
    ~KeyGuard();
    KeyGuard(const KeyGuard&) = delete;
    KeyGuard& operator=(const KeyGuard&) = delete;

   private:
    KeyedRelay* owner_;
    IDXGIKeyedMutex* mutex_;
    UINT release_key_;
  };

  // nullptr, with `error` set, when the adapter is not NVIDIA's, or the relay cannot be made or has no fences. `relay_created`
  // receives the relay as D3D11CreateDevice returned it, whether or not this succeeds: under ReShade that is its proxy, whose last
  // release raises destroy_device, so the caller releases it only outside its own lock.
  static std::unique_ptr<KeyedRelay> Create(ID3D10Device* game, Microsoft::WRL::ComPtr<ID3D11Device>* relay_created,
                                            std::string* error);
  // One last Flush (Plan 8's final review C-1: D3D11 destroys a released object only at its context's next Flush, and the relay has
  // no Present), then the relay, its proxy last.
  ~KeyedRelay();
  KeyedRelay(const KeyedRelay&) = delete;
  KeyedRelay& operator=(const KeyedRelay&) = delete;

  [[nodiscard]] ID3D11Device* Relay() const { return relay_.Get(); }  // native: a fence's GetDevice (the D3D11 design's §9 a)
  [[nodiscard]] LUID Luid() const { return luid_; }

  // A keyed texture of `size` and `format`, made without initial data (the probe's lesson: an upload races the first key). The relay
  // then takes and gives back key 0 and flushes, so nothing it did to make the texture can race the game's first write. False,
  // leaving `keyed` untouched, with the failing call's HRESULT.
  bool CreateKeyed(nr::Size size, DXGI_FORMAT format, uint32_t bytes_per_pixel, Keyed* keyed, HRESULT* result);
  // The game's side: key 0, `source` into the keyed texture, key 1 for the relay. False (the relay stopped) when the key did not come
  // or could not be given. The caller flushes the game's device.
  bool Hand(Keyed& keyed, ID3D10Resource* source);
  // The relay's side: key 1, which `key` gives back (key 0) when it goes. False (the relay stopped) when it did not come.
  bool Take(Keyed& keyed, std::optional<KeyGuard>* key);
  // The game's side takes NR's result back: key 0, the keyed texture into `destination`, key 0 given back. False when the key did not come.
  bool GiveBack(Keyed& keyed, ID3D10Resource* destination);
  // Empties `keyed`; the relay then holds a release its context has not flushed (D3D11 destroys it at the next Flush).
  void Drop(Keyed* keyed);
  // Submits the relay's queued work, which is also what destroys what it released: the relay has no Present to do it.
  void Flush();
  [[nodiscard]] bool Dirty() const { return dirty_; }  // the relay released a texture since its last Flush

  // A key that did not come or could not be given: latches with `what`. Nothing acquires a key after it. It logs nothing: the owner
  // passes the latch on (D3D10Bridge into its D3D11 bridge, D3D10Client into its D3D11 client), logs it once there, and stops the fences.
  void Fail(std::string_view what);
  // The owner's own stop latches the relay too; the first reason stays.
  void Stop(std::string reason);
  [[nodiscard]] bool Stopped() const { return !latch_.empty(); }
  [[nodiscard]] std::string_view Reason() const { return latch_; }  // `what` of the Fail, or the owner's reason
  // "The Direct3D 10 bridge stopped: {what}. Restart the game to use NR again": the owners' latch text and log line.
  [[nodiscard]] static std::string Sentence(std::string_view what);

 private:
  KeyedRelay() = default;

  // Declared first, so released last: the relay as D3D11CreateDevice returned it (ReShade's proxy in a game).
  Microsoft::WRL::ComPtr<ID3D11Device> relay_created_;
  Microsoft::WRL::ComPtr<ID3D10Device> game_;                  // the game's device, native
  Microsoft::WRL::ComPtr<ID3D11Device> relay_;                 // native: a fence's GetDevice
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> relay_context_;  // the relay's immediate context, native
  LUID luid_ = {};
  bool dirty_ = false;
  std::string latch_;
};

// The description behind a D3D10 resource; nullopt for anything that is not a 2D texture. Both owners of a KeyedRelay describe the
// game's back buffer, mask and motion with it.
[[nodiscard]] std::optional<D3D10_TEXTURE2D_DESC> TextureDesc(ID3D10Resource* resource);

}  // namespace uplift::bridge
