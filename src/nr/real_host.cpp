#include "nr/real_host.hpp"

#include <dxgi1_4.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "nr/feature.hpp"
#include "nr/log.hpp"

namespace uplift::nr {

RealHost::RealHost(ID3D12Device* device, SnippetConfig config, std::function<std::optional<MemoryInfo>()> game_memory)
    : device_(device), config_(std::move(config)), game_memory_(std::move(game_memory)) {
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  if (const HRESULT factory_result = CreateDXGIFactory2(0u, IID_PPV_ARGS(&factory)); FAILED(factory_result)) {
    Logf(LogLevel::WARN, "CreateDXGIFactory2 failed with {:#010x}; QueryMemory will report unlimited headroom",
         static_cast<uint32_t>(factory_result));
    return;
  }
  const HRESULT result = factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter_));
  if (FAILED(result)) {
    Logf(LogLevel::WARN, "EnumAdapterByLuid failed with {:#010x}; QueryMemory will report unlimited headroom",
         static_cast<uint32_t>(result));
  }
}

RealHost::~RealHost() {
  snippet_.Unload();
}

NVSDK_NGX_Result RealHost::LoadRuntime() {
  const NVSDK_NGX_Result load_result = snippet_.Load(config_);
  if (NVSDK_NGX_FAILED(load_result)) return load_result;
  const NVSDK_NGX_Result bind_result = snippet_.Bind(device_.Get());
  if (NVSDK_NGX_FAILED(bind_result)) {
    snippet_.Unload();
    return bind_result;
  }
  SetEnvironmentVariableW(UPLIFT_NR_RUNTIME_MARKER, L"1");
  return bind_result;
}

void RealHost::UnloadRuntime() {
  snippet_.Unload();
  SetEnvironmentVariableW(UPLIFT_NR_RUNTIME_MARKER, nullptr);
}

void RealHost::AbandonRuntime() {
  snippet_.Abandon();
}

std::unique_ptr<FeatureInterface> RealHost::NewFeature() {
  return std::make_unique<Feature>(snippet_);
}

MemoryInfo RealHost::QueryMemory() {
  // A null adapter (DXGI factory creation failed) or a failed query must never
  // block the budget: report unlimited headroom rather than starving NR.
  if (!adapter_) return {UINT64_MAX, 0u};
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  const HRESULT result = adapter_->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
  if (FAILED(result)) {
    if (!query_memory_failed_logged_) {
      Logf(LogLevel::WARN, "QueryVideoMemoryInfo failed with {:#010x}; reporting unlimited headroom",
           static_cast<uint32_t>(result));
      query_memory_failed_logged_ = true;
    }
    return {UINT64_MAX, 0u};
  }
  // Plan 9 (design §2.10): two processes share the adapter; NR fits, yields and resumes only while both have room.
  if (game_memory_) {
    if (const std::optional<MemoryInfo> game = game_memory_(); game && game->budget != 0u) {
      const uint64_t helper_room = (info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0u);
      const uint64_t game_room = (game->budget > game->usage ? game->budget - game->usage : 0u);
      return {info.CurrentUsage + std::min(helper_room, game_room), info.CurrentUsage};
    }
  }
  return {info.Budget, info.CurrentUsage};
}

std::optional<uint64_t> RealHost::QueryRuntimeBytes() {
  return snippet_.QueryAllocatedBytes();
}

void RealHost::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1u, &barrier);
}

}  // namespace uplift::nr
