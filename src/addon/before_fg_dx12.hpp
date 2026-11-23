#pragma once
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "addon/fg_input.hpp"
#include "addon/hud_present_d3d12.hpp"
#include "addon/device_context.hpp"
#include "bridge/d3d12_side.hpp"

namespace uplift::addon {

// Attached to the game-facing swapchain through DXGI private data. It never retains a backbuffer or the swapchain itself.
class BeforeFgDx12 final : public IUnknown {
 public:
  static std::string_view Attach(IDXGISwapChain* chain, IUnknown* queue, void* inner);
  static Microsoft::WRL::ComPtr<BeforeFgDx12> Find(IDXGISwapChain* chain);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  void Resize(UINT count, IUnknown* const* queues, void* inner);
  void Retry() { retry_.store(true); }
  void* Inner() const { return inner_.load(); }
  // `image`: the native back buffer NR recorded into (HUD-less: the image noted for DLSS-G's evaluate); 0 when none.
  // HUD-less (design §5): with `hud`, N for the noted back buffer when NR ran on the game's HUD-less image, and why it did not otherwise.
  struct Result {
    uint32_t passes = 0;
    std::string_view reason;
    uint64_t image = 0;
    Replacement replacement;
    HudInputs hud_inputs;
    HudReason hud_reason = HudReason::CORRECTION_OFF;
  };
  // HUD-less (design §1): the chain's current back buffer as a native handle, for the drop every game-facing Present makes; 0 when it
  // cannot be named.
  [[nodiscard]] static uint64_t CurrentBackBuffer(IDXGISwapChain* chain);
  // space: the colour space ReShade reported for the ReShade chain beneath the route (Inner()) at its latest present.
  // The game sets it on the outer chain, and Streamline's proxy hands SetColorSpace1 to the chain it wraps unchanged.
  // It may trail the game's call by one frame (the outer Present comes before ReShade's present of the inner chain), and
  // is UNKNOWN before ReShade's first present; both by design.
  Result Record(IDXGISwapChain* chain, ID3D12Device* device, DeviceContext& context, const FrameSettings& settings, const FrameFacts& facts,
                color::ColorSpace space, std::chrono::steady_clock::time_point now, const HudPresentD3D12* hud = nullptr);
 private:
  struct Queue { Microsoft::WRL::ComPtr<ID3D12CommandQueue> native; std::unique_ptr<bridge::D3D12Side> ring; };
  std::atomic<ULONG> refs_{1u};
  std::atomic<void*> inner_{};
  std::atomic<bool> retry_{false};
  IDXGISwapChain* identity_ = nullptr;
  std::mutex mutex_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> creation_queue_;
  std::vector<Queue> queues_;
};

}  // namespace uplift::addon
