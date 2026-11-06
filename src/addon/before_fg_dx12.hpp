#pragma once
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

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
  void ColorSpace(DXGI_COLOR_SPACE_TYPE space);
  void Retry() { retry_.store(true); }
  void* Inner() const { return inner_.load(); }
  struct Result { uint32_t passes = 0; std::string_view reason; };
  Result Record(IDXGISwapChain* chain, ID3D12Device* device, DeviceContext& context, const FrameSettings& settings, const FrameFacts& facts,
                std::chrono::steady_clock::time_point now);
 private:
  struct Queue { Microsoft::WRL::ComPtr<ID3D12CommandQueue> native; std::unique_ptr<bridge::D3D12Side> ring; };
  std::atomic<ULONG> refs_{1u};
  std::atomic<void*> inner_{};
  std::atomic<bool> retry_{false};
  IDXGISwapChain* identity_ = nullptr;
  std::mutex mutex_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> creation_queue_;
  std::vector<Queue> queues_;
  color::ColorSpace space_ = color::ColorSpace::UNKNOWN;
  bool supported_space_ = true;
};

}  // namespace uplift::addon
