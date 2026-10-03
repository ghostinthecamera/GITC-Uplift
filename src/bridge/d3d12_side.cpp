#include "bridge/d3d12_side.hpp"

#include <dxgi1_6.h>

#include <atomic>
#include <format>
#include <utility>

#include "addon/dred.hpp"
#include "addon/environment.hpp"
#include "nr/log.hpp"

namespace uplift::bridge {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD DRAIN_WAIT_MS = 2000u;  // as DeviceContext::Teardown's cap
// Plan 17 (1.0.1 design §4): DRED's auto-breadcrumbs on Uplift's private devices; page-fault reporting is always on.
constexpr bool DRED_BREADCRUMBS = true;
// Plan 17: CLSID_D3D12DeviceFactory (d3d12.h declares it; no import library defines it).
constexpr GUID DEVICE_FACTORY = {0x114863bfu, 0xc386u, 0x4aeeu, {0xb3u, 0x9du, 0x8fu, 0x0bu, 0xbbu, 0x06u, 0x29u, 0x55u}};

using GetInterfaceFunction = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);

// D3D12GetInterface, resolved at run time and never imported: a d3d12.dll without that export (Windows 10 before build 20348, without the Agility SDK
// servicing) would otherwise keep the add-on and the helper from loading at all, as 1.0.0 did not. d3d12.dll is loaded already (D3D12CreateDevice is
// imported). Null without it, which is the fallback; said once.
GetInterfaceFunction D3D12GetInterfaceOrNull() {
  static std::atomic<bool> said{false};
  const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  const auto function = (d3d12 != nullptr ? reinterpret_cast<GetInterfaceFunction>(GetProcAddress(d3d12, "D3D12GetInterface")) : nullptr);
  if (function == nullptr && !said.exchange(true)) {
    nr::Log(nr::LogLevel::INFO, "this d3d12.dll has no D3D12GetInterface, so no device factory");
  }
  return function;
}

// UPLIFT_NO_DEVICE_FACTORY=1, a test's switch (the e2e case of the fallback, as UPLIFT_STATE_DIR is one): no device factory, as on an older runtime.
bool DeviceFactoryDisabledForTest() {
  wchar_t value[8] = {};
  return GetEnvironmentVariableW(L"UPLIFT_NO_DEVICE_FACTORY", value, 8u) == 1u && value[0] == L'1';
}

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
  // Plan 17: DRED on Uplift's private devices (this is the only place they are made), decided before the first one exists (the DRED cost case's control
  // has decided it off already). The settings are written below, on the factory, or process-wide only on the fallback.
  addon::DecideDred(DRED_BREADCRUMBS ? addon::DredMode::BREADCRUMBS_AND_PAGE_FAULTS : addon::DredMode::PAGE_FAULTS);
  // Plan 17 (Retry now), test phase: every private device is an independent one from the device factory where the D3D12 runtime and the driver have
  // independent devices (Windows 11 24H2's inbox runtime, or a game's newer Agility SDK), never the adapter's singleton. A removed singleton lives on while
  // the NR runtime abandoned on it holds it (spec §13: nothing of it is called again), and while it does the runtime refuses EVERY new device on that adapter
  // with DXGI_ERROR_DEVICE_REMOVED, D3D12CreateDevice and the device factory alike (whatever its flags, measured on 617.14). A removed independent device
  // blocks nothing. Not a ReShade proxy either (ReShade does not wrap the factory): the native device is used for all work anyway.
  HRESULT factory_result = E_NOINTERFACE;
  if (DeviceFactoryDisabledForTest()) {
    nr::Log(nr::LogLevel::INFO, "UPLIFT_NO_DEVICE_FACTORY: no device factory (a test's switch)");
  } else if (const GetInterfaceFunction get_interface = D3D12GetInterfaceOrNull()) {
    ComPtr<ID3D12DeviceFactory> device_factory;
    factory_result = get_interface(DEVICE_FACTORY, IID_PPV_ARGS(&device_factory));
    if (SUCCEEDED(factory_result)) {
      // The process's global D3D12 state (the debug layer, experimental features), as D3D12CreateDevice's device would have had: the gpu_debug_layer
      // entry's validation reaches the bridge's device through it. Without it NR still runs; it is said once.
      if (const HRESULT copied = device_factory->InitializeFromGlobalState(); FAILED(copied)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
          nr::Logf(nr::LogLevel::WARN, "the device factory could not take the process's global D3D12 state ({:#010x}): a debug layer turned on for the "
                   "game does not reach Uplift's private device", static_cast<uint32_t>(copied));
        }
      }
      // Without DISALLOW_STORING the factory's new device may become the singleton, the state this path avoids: a failed SetFlags is no factory.
      factory_result = device_factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);
      if (SUCCEEDED(factory_result)) {
        addon::ApplyDred(device_factory.Get());  // its devices read the factory's own DRED settings: the process's choice, on or off
        factory_result = device_factory->CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(created->ReleaseAndGetAddressOf()));
      }
    }
  }
  if (SUCCEEDED(factory_result)) {
    side->independent_ = true;
    nr::Log(nr::LogLevel::INFO, "the private Direct3D 12 device is an independent one (the device factory)");
  } else {
    // The fallback: the adapter's singleton (under ReShade its proxy, reshade d3d12.cpp, which raises init_device for it), shared with any other
    // D3D12CreateDevice in the process. After its removal no new device can be made on the adapter in this process: a stop is final (Independent()).
    // Only here are DRED's settings process-wide, right before Uplift's own D3D12CreateDevice.
    addon::ApplyDredGlobal();
    const HRESULT result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(created->ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
      return fail(std::format("the device factory could not make an independent device ({:#010x}), and D3D12CreateDevice failed with {:#010x}",
                              static_cast<uint32_t>(factory_result), static_cast<uint32_t>(result)));
    }
    nr::Logf(nr::LogLevel::INFO, "the private Direct3D 12 device is the adapter's shared one: the device factory could not make an independent device "
             "({:#010x}), so a stop of NR on it is final until the game restarts", static_cast<uint32_t>(factory_result));
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
  side->progress_->SetName(L"Uplift progress fence");  // Plan 17: names DRED's breadcrumbs and page faults can show
  for (Slot& slot : side->ring_) {
    if (FAILED(native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))) {
      return fail("the private command allocators could not be created");
    }
    slot.allocator->SetName(L"Uplift NR frame allocator");
  }
  if (FAILED(native->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, side->ring_[0].allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&side->list_)))
      || FAILED(side->list_->Close())) {
    return fail("the private command list could not be created");
  }
  side->list_->SetName(L"Uplift NR frame");
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
  created->SetName(L"Uplift shared fence");
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
