#include "addon/before_fg_dx12.hpp"
#include <algorithm>
#include "text/before_fg_messages.hpp"

namespace uplift::addon {
namespace {
using Microsoft::WRL::ComPtr;
// Identifies the BeforeFgDx12 route stored on the game-facing swapchain by Attach() and read by Find().
constexpr GUID ROUTE_ID = {0xfe799951,0xa813,0x4df1,{0xa9,0x0f,0xab,0x91,0xf2,0x11,0xcc,0x7a}};
// Requests the underlying COM object from a ReShade wrapper. If unsupported, Native() silently queries the original
// object for the requested interface instead, retaining the proxy when one was supplied.
constexpr GUID RESHADE_UNWRAP = {0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};

template<class T>
ComPtr<T> Native(IUnknown* object) {
  ComPtr<IUnknown> original;
  ComPtr<T> result;
  if (object && SUCCEEDED(object->QueryInterface(RESHADE_UNWRAP, reinterpret_cast<void**>(original.GetAddressOf())))) object = original.Get();
  if (object) object->QueryInterface(IID_PPV_ARGS(&result));
  return result;
}

class GameList final : public FrameHost {
 public:
  GameList(ID3D12GraphicsCommandList* list, ID3D12Resource* target) : list_(list), target_(target) {}
  void FlushPending() override {}
  ID3D12GraphicsCommandList* NativeList() override { return list_; }
  void TargetBarrier(D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) override {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {target_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list_->ResourceBarrier(1u, &barrier);
    touched = true;
  }
  bool touched = false;
 private:
  ID3D12GraphicsCommandList* list_;
  ID3D12Resource* target_;
};
}  // namespace

ULONG BeforeFgDx12::AddRef() { return ++refs_; }
ULONG BeforeFgDx12::Release() {
  const ULONG refs = --refs_;
  if (!refs) delete this;
  return refs;
}
HRESULT BeforeFgDx12::QueryInterface(REFIID iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
  *out = static_cast<IUnknown*>(this);
  AddRef();
  return S_OK;
}

std::string_view BeforeFgDx12::Attach(IDXGISwapChain* chain, IUnknown* queue, void* inner) {
  auto native = Native<ID3D12CommandQueue>(queue);
  if (!native) return "cannot unwrap D3D12 creation queue";
  if (native->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return "creation queue is not direct";
  if (!inner) return "no ReShade swapchain observed during creation";
  ComPtr<BeforeFgDx12> route;
  route.Attach(new BeforeFgDx12());
  route->creation_queue_ = std::move(native);
  route->inner_.store(inner);
  route->identity_ = chain;
  route->queues_.push_back({route->creation_queue_});
  if (FAILED(chain->SetPrivateDataInterface(ROUTE_ID, route.Get()))) return "could not store route on swapchain";
  return {};
}

ComPtr<BeforeFgDx12> BeforeFgDx12::Find(IDXGISwapChain* chain) {
  ComPtr<BeforeFgDx12> result;
  UINT size = sizeof(void*);
  if (FAILED(chain->GetPrivateData(ROUTE_ID, &size, result.GetAddressOf())) || !result || result->identity_ != chain) result.Reset();
  return result;
}

void BeforeFgDx12::Resize(UINT count, IUnknown* const* queues, void* inner) {
  std::lock_guard lock(mutex_);
  retry_.store(false);
  if (inner) inner_.store(inner);
  if (queues) {
    queues_.clear();
    for (UINT i = 0; i < count; ++i) queues_.push_back({Native<ID3D12CommandQueue>(queues[i])});
  } else if (queues_.size() == 1u) {
    queues_[0].ring.reset();
  } else if (count != 0u && count != queues_.size()) {
    queues_.clear();
  }
}

void BeforeFgDx12::ColorSpace(DXGI_COLOR_SPACE_TYPE space) {
  std::lock_guard lock(mutex_);
  supported_space_ = true;
  switch (space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: space_ = color::ColorSpace::SRGB_NONLINEAR; break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: space_ = color::ColorSpace::EXTENDED_SRGB_LINEAR; break;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: space_ = color::ColorSpace::HDR10_ST2084; break;
    default: supported_space_ = false; break;
  }
}

BeforeFgDx12::Result BeforeFgDx12::Record(IDXGISwapChain* chain, ID3D12Device* device, DeviceContext& context,
                                           const FrameSettings& settings, const FrameFacts& facts, std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(mutex_);
  if (retry_.exchange(false)) return {.reason = uplift::text::WAIT_SUCCESSFUL_PRESENT_LINE};
  if (!supported_space_ && settings.encoding == color::Encoding::AUTO) return {.reason = uplift::text::OUTER_COLOR_SPACE_LINE};
  ComPtr<IDXGISwapChain3> chain3;
  if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) return {.reason = uplift::text::REAL_BACKBUFFER_UNKNOWN_LINE};
  const UINT index = chain3->GetCurrentBackBufferIndex();
  const size_t slot = queues_.size() == 1u ? 0u : index;
  if (slot >= queues_.size() || !queues_[slot].native) return {.reason = uplift::text::REAL_BACKBUFFER_QUEUE_LINE};
  auto& queue = queues_[slot];
  ComPtr<ID3D12Device> queue_device;
  if (FAILED(queue.native->GetDevice(IID_PPV_ARGS(&queue_device))) || Native<ID3D12Device>(queue_device.Get()).Get() != device) return {.reason = uplift::text::OUTER_QUEUE_DEVICE_LINE};
  ComPtr<ID3D12Resource> backbuffer;
  if (FAILED(chain3->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) return {.reason = uplift::text::REAL_BACKBUFFER_UNAVAILABLE_LINE};
  ComPtr<ID3D12Resource> native = Native<ID3D12Resource>(backbuffer.Get());
  ComPtr<ID3D12Device> buffer_device;
  if (!native || FAILED(native->GetDevice(IID_PPV_ARGS(&buffer_device))) || Native<ID3D12Device>(buffer_device.Get()).Get() != device) return {.reason = uplift::text::OUTER_IMAGE_DEVICE_LINE};
  const auto trigger = context.BeginFrame(queue.native.Get(), settings, facts, {.resource = native.Get(), .color_space = space_}, false, now);
  if (trigger == TriggerPoint::NONE || context.NrState() != nr::SessionState::ACTIVE) return {};
  if (!queue.ring) queue.ring = bridge::D3D12Side::OnGameQueue(queue.native.Get());
  if (!queue.ring) return {.reason = uplift::text::REAL_FRAME_COMMAND_LIST_LINE};
  auto* list = queue.ring->BeginList();
  if (!list) return {.reason = uplift::text::REAL_FRAME_RECORDING_BUSY_LINE};
  GameList host(list, native.Get());
  struct Close { bridge::D3D12Side& ring; ~Close() { ring.CloseList(); } } close{*queue.ring};
  const auto result = context.Run(&host, D3D12_RESOURCE_STATE_PRESENT, trigger);
  if (host.touched && (!queue.ring->ExecuteList() || !queue.ring->SignalProgress(queue.ring->LastSignalled() + 1u))) return {.reason = uplift::text::REAL_FRAME_SUBMISSION_LINE};
  return {.passes = result.nr_applied ? result.passes_run : 0u, .reason = nr::SkipReasonText(result.reason)};
}

}  // namespace uplift::addon
