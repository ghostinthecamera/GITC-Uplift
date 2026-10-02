#include "bridge/d3d12_side.hpp"

#include <dxgi1_6.h>

#include <format>
#include <utility>

#include "addon/environment.hpp"

namespace uplift::bridge {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD DRAIN_WAIT_MS = 2000u;  // as DeviceContext::Teardown's cap

}  // namespace

std::unique_ptr<D3D12Side> D3D12Side::Create(LUID luid, ComPtr<ID3D12Device>* created, std::string* error) {
  const auto fail = [error](std::string text) {
    *error = std::move(text);
    return std::unique_ptr<D3D12Side>();
  };
  std::unique_ptr<D3D12Side> side(new D3D12Side());
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  DXGI_ADAPTER_DESC1 adapter_description = {};
  if (FAILED(CreateDXGIFactory2(0u, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))
      || FAILED(adapter->GetDesc1(&adapter_description))) {
    return fail("the game's adapter could not be found by its LUID");
  }
  // WARP and other vendors are refused, as on D3D12 (addon::IsNvidiaDevice); cross-adapter sharing is out of scope.
  if (adapter_description.VendorId != addon::NVIDIA_VENDOR_ID) {
    return fail("Uplift needs an NVIDIA GPU; this game renders on another adapter");
  }
  // Under ReShade this returns its proxy (reshade d3d12.cpp) and raises init_device for it, like any device.
  if (const HRESULT result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(created->ReleaseAndGetAddressOf()));
      FAILED(result)) {
    return fail(std::format("D3D12CreateDevice failed with {:#010x}", static_cast<uint32_t>(result)));
  }
  side->created_ = *created;
  // Key decision a: ReShade forwards CreateFence unwrapped, so the fence's own device is the native one.
  if (FAILED((*created)->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&side->progress_)))
      || FAILED(side->progress_->GetDevice(IID_PPV_ARGS(&side->device_)))) {
    return fail("the progress fence could not be created");
  }
  ID3D12Device* const native = side->device_.Get();
  const D3D12_COMMAND_QUEUE_DESC queue_description = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  if (FAILED(native->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&side->queue_)))) {
    return fail("the private queue could not be created");
  }
  side->queue_->SetName(L"Uplift private queue");
  for (Slot& slot : side->ring_) {
    if (FAILED(native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))) {
      return fail("the private command allocators could not be created");
    }
  }
  if (FAILED(native->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, side->ring_[0].allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&side->list_)))
      || FAILED(side->list_->Close())) {
    return fail("the private command list could not be created");
  }
  return side;
}

D3D12Side::~D3D12Side() {
  Drain();
}

void D3D12Side::Drain() {
  // Nothing the queue may still read is released before it finishes: at most 2 s, as DeviceContext::Teardown.
  if (progress_ != nullptr && progress_->GetCompletedValue() < last_signalled_) {
    if (const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); event != nullptr) {
      if (SUCCEEDED(progress_->SetEventOnCompletion(last_signalled_, event))) {
        WaitForSingleObject(event, DRAIN_WAIT_MS);
      }
      CloseHandle(event);
    }
  }
}

HRESULT D3D12Side::CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, const wchar_t* name,
                                ComPtr<ID3D12Resource>* resource, HANDLE* handle) {
  const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC description = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Alignment = 0u,
      .Width = size.width,
      .Height = size.height,
      .DepthOrArraySize = 1u,
      .MipLevels = 1u,
      .Format = format,
      .SampleDesc = {.Count = 1u, .Quality = 0u},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = flags,
  };
  ComPtr<ID3D12Resource> created;
  if (const HRESULT result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description,
                                                              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&created));
      FAILED(result)) {
    return result;
  }
  created->SetName(name);
  if (handle != nullptr) {
    if (const HRESULT result = device_->CreateSharedHandle(created.Get(), nullptr, GENERIC_ALL, nullptr, handle);
        FAILED(result)) {
      return result;
    }
  }
  *resource = std::move(created);
  return S_OK;
}

HRESULT D3D12Side::CreateSharedFence(ComPtr<ID3D12Fence>* fence, HANDLE* handle) {
  ComPtr<ID3D12Fence> created;
  if (const HRESULT result = device_->CreateFence(0u, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&created)); FAILED(result)) {
    return result;
  }
  if (handle != nullptr) {
    if (const HRESULT result = device_->CreateSharedHandle(created.Get(), nullptr, GENERIC_ALL, nullptr, handle);
        FAILED(result)) {
      return result;
    }
  }
  *fence = std::move(created);
  return S_OK;
}

bool D3D12Side::SlotFree() const {
  return progress_->GetCompletedValue() >= ring_[next_slot_].done;
}

ID3D12GraphicsCommandList* D3D12Side::BeginList() {
  Slot& slot = ring_[next_slot_];
  if (!SlotFree() || FAILED(slot.allocator->Reset()) || FAILED(list_->Reset(slot.allocator.Get(), nullptr))) return nullptr;
  list_open_ = true;
  return list_.Get();
}

bool D3D12Side::ExecuteList() {
  list_open_ = false;
  if (FAILED(list_->Close())) return false;
  ID3D12CommandList* const lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1u, lists);
  executed_ = true;
  return true;
}

void D3D12Side::CloseList() {
  if (std::exchange(list_open_, false)) {
    list_->Close();
  }
}

bool D3D12Side::SignalProgress(uint64_t value) {
  const bool signalled = SUCCEEDED(queue_->Signal(progress_.Get(), value));
  if (executed_ || signalled) {
    ring_[next_slot_].done = value;
    last_signalled_ = value;
    next_slot_ = (next_slot_ + 1u) % RING;
  }
  executed_ = false;
  return signalled;
}

uint64_t D3D12Side::Completed() const {
  return progress_->GetCompletedValue();
}

bool D3D12Side::Retire(ComPtr<ID3D12Resource> resource, ComPtr<IUnknown> partner, uint64_t last_use) {
  if (!resource) return false;
  // A D3D11 partner is destroyed at its context's next Flush; the D3D12 side must outlive the queue's last use. One read of
  // the fence decides it, so the caller's "released at once" is exactly what happened here.
  if (last_use > progress_->GetCompletedValue()) {
    retired_.push_back({.resource = std::move(resource), .partner = std::move(partner), .release_at = last_use});
    return false;
  }
  return true;
}

bool D3D12Side::FreeFinished() {
  const uint64_t finished = progress_->GetCompletedValue();
  return std::erase_if(retired_, [finished](const Retired& retired) { return retired.release_at <= finished; }) > 0u;
}

}  // namespace uplift::bridge
