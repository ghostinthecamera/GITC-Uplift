#include "addon/dred.hpp"

#include <Windows.h>

#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "nr/log.hpp"

namespace uplift::addon {
namespace {

using Microsoft::WRL::ComPtr;

// CLSID_D3D12DeviceRemovedExtendedData (d3d12.h declares it; no import library defines it).
constexpr GUID DRED_CONFIGURATION = {0x4a75bbc4u, 0x9ff4u, 0x4ad8u, {0x9fu, 0x18u, 0xabu, 0xaeu, 0x84u, 0xdcu, 0x5fu, 0xf2u}};
constexpr size_t MAX_LISTS = 4u;        // command lists in flight named, the rest counted
constexpr size_t MAX_ALLOCATIONS = 4u;  // allocations named per kind at a page fault

// The process's choice, made by the first DecideDred; UNDECIDED before it.
constexpr int UNDECIDED = -1;
std::atomic<int> g_mode{UNDECIDED};
std::atomic<bool> g_global_applied{false};

void Apply(ID3D12DeviceRemovedExtendedDataSettings* settings, DredMode mode) {
  const auto enablement = [](bool on) { return (on ? D3D12_DRED_ENABLEMENT_FORCED_ON : D3D12_DRED_ENABLEMENT_FORCED_OFF); };
  settings->SetAutoBreadcrumbsEnablement(enablement(mode == DredMode::BREADCRUMBS_AND_PAGE_FAULTS));
  settings->SetPageFaultEnablement(enablement(mode != DredMode::OFF));
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings1;
  if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings1)))) {
    settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);  // the op names are enough; context strings cost more
  }
}

std::string Utf8(const wchar_t* text) {
  if (text == nullptr || *text == L'\0') return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0u, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return {};
  std::string utf8(static_cast<size_t>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0u, text, -1, utf8.data(), bytes, nullptr, nullptr);
  return utf8;
}

std::string NameOf(const wchar_t* wide, const char* narrow, std::string_view unnamed) {
  if (std::string name = Utf8(wide); !name.empty()) return name;
  if (narrow != nullptr && *narrow != '\0') return narrow;
  return std::string(unnamed);
}

std::string_view OpName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:                 return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:                return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                  return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:             return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:      return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:           return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                  return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:          return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:         return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:              return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES:                 return "CopyTiles";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:        return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:     return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:  return "ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:     return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:           return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:             return "ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                   return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:          return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:           return "BeginSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:             return "EndSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT:      return "AtomicCopyBufferUINT";
    case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT64:    return "AtomicCopyBufferUINT64";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION:  return "ResolveSubresourceRegion";
    case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE:      return "WriteBufferImmediate";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND:     return "InitializeMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND:        return "ExecuteMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1:         return "SetPipelineState1";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEEXTENSIONCOMMAND: return "InitializeExtensionCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEEXTENSIONCOMMAND:   return "ExecuteExtensionCommand";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH:              return "DispatchMesh";
    case D3D12_AUTO_BREADCRUMB_OP_BARRIER:                   return "Barrier";
    case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST:        return "BeginCommandList";
    default:                                                 return "another op";
  }
}

// "Uplift model A (texture), Uplift look pyramid (texture), and 3 more", or "none".
std::string AllocationNames(const D3D12_DRED_ALLOCATION_NODE1* node) {
  std::string names;
  size_t count = 0u;
  for (; node != nullptr; node = node->pNext, ++count) {
    if (count < MAX_ALLOCATIONS) {
      names += std::format("{}{}", (count == 0u ? "" : ", "), NameOf(node->ObjectNameW, node->ObjectNameA, "an unnamed allocation"));
    }
  }
  if (count == 0u) return "none";
  if (count > MAX_ALLOCATIONS) {
    names += std::format(" and {} more", count - MAX_ALLOCATIONS);
  }
  return names;
}

}  // namespace

void DecideDred(DredMode mode) {
  int undecided = UNDECIDED;
  if (!g_mode.compare_exchange_strong(undecided, static_cast<int>(mode))) return;  // decided already
  if (mode == DredMode::OFF) {
    nr::Log(nr::LogLevel::INFO, "DRED off for Uplift's private Direct3D 12 devices");
    return;
  }
  nr::Logf(nr::LogLevel::INFO, "DRED on for Uplift's private Direct3D 12 devices ({}page faults)",
           (mode == DredMode::BREADCRUMBS_AND_PAGE_FAULTS ? "breadcrumbs and " : ""));
}

void ApplyDredGlobal() {
  const int mode = g_mode.load();
  if (mode == UNDECIDED || static_cast<DredMode>(mode) == DredMode::OFF || g_global_applied.exchange(true)) return;
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) return;  // an older runtime: no DRED
  Apply(settings.Get(), static_cast<DredMode>(mode));
}

void ApplyDred(ID3D12DeviceFactory* factory) {
  const int mode = g_mode.load();
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
  if (mode == UNDECIDED || factory == nullptr || FAILED(factory->GetConfigurationInterface(DRED_CONFIGURATION, IID_PPV_ARGS(&settings)))) return;
  Apply(settings.Get(), static_cast<DredMode>(mode));  // OFF is set explicitly: the factory's copy of the global state may carry another choice
}

void LogDred(ID3D12Device* device) {
  ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
  if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    nr::Log(nr::LogLevel::ERR, "DRED: no data for this device");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
  if (const HRESULT result = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs); FAILED(result)) {
    nr::Logf(nr::LogLevel::ERR, "DRED: no breadcrumbs ({:#010x})", static_cast<uint32_t>(result));
  } else {
    size_t in_flight = 0u;
    for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext) {
      const uint32_t completed = (node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0u);
      if (node->BreadcrumbCount == 0u || completed >= node->BreadcrumbCount) continue;  // finished, or never started
      if (++in_flight > MAX_LISTS) continue;
      nr::Logf(nr::LogLevel::ERR, "DRED: {} on {}: completed {}/{}, next: {}",
               NameOf(node->pCommandListDebugNameW, node->pCommandListDebugNameA, "an unnamed command list"),
               NameOf(node->pCommandQueueDebugNameW, node->pCommandQueueDebugNameA, "an unnamed queue"), completed, node->BreadcrumbCount,
               OpName(node->pCommandHistory[completed]));
    }
    if (in_flight == 0u) {
      nr::Log(nr::LogLevel::ERR, "DRED: no command list was in flight");
    } else if (in_flight > MAX_LISTS) {
      nr::Logf(nr::LogLevel::ERR, "DRED: and {} more command lists in flight", in_flight - MAX_LISTS);
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA != 0u) {
    nr::Logf(nr::LogLevel::ERR, "DRED: page fault at {:#018x}; allocations there: {}; recently freed there: {}", fault.PageFaultVA,
             AllocationNames(fault.pHeadExistingAllocationNode), AllocationNames(fault.pHeadRecentFreedAllocationNode));
  }
}

}  // namespace uplift::addon
