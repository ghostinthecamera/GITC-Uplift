#include "nr/vk_host.hpp"

#include <cstdint>
#include <utility>

#include "nr/log.hpp"
#include "nr/vk_feature.hpp"
#include "nr/vk_handles.hpp"

namespace uplift::nr {

VkHost::VkHost(SnippetConfig config, VulkanBinding binding, Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter, const vk::NrFunctions& functions)
    : config_(std::move(config)), binding_(binding), adapter_(std::move(adapter)), functions_(functions) {}

VkHost::~VkHost() {
  snippet_.Unload();
}

NVSDK_NGX_Result VkHost::LoadRuntime() {
  const NVSDK_NGX_Result load_result = snippet_.Load(config_);
  if (NVSDK_NGX_FAILED(load_result)) return load_result;
  const NVSDK_NGX_Result bind_result = snippet_.BindVulkan(binding_);
  if (NVSDK_NGX_FAILED(bind_result)) {
    snippet_.Unload();
    return bind_result;
  }
  SetEnvironmentVariableW(UPLIFT_NR_RUNTIME_MARKER, L"1");
  return bind_result;
}

void VkHost::UnloadRuntime() {
  snippet_.Unload();
  SetEnvironmentVariableW(UPLIFT_NR_RUNTIME_MARKER, nullptr);
}

void VkHost::AbandonRuntime() {
  snippet_.Abandon();
}

std::unique_ptr<FeatureInterface> VkHost::NewFeature() {
  return std::make_unique<VkFeature>(snippet_);
}

MemoryInfo VkHost::QueryMemory() {
  // A null adapter or a failed query must never block the budget: report unlimited headroom rather than starving NR (RealHost's rule).
  if (!adapter_) return {UINT64_MAX, 0u};
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  const HRESULT result = adapter_->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
  if (FAILED(result)) {
    if (!query_memory_failed_logged_) {
      Logf(LogLevel::WARN, "QueryVideoMemoryInfo failed with {:#010x}; reporting unlimited headroom", static_cast<uint32_t>(result));
      query_memory_failed_logged_ = true;
    }
    return {UINT64_MAX, 0u};
  }
  return {info.Budget, info.CurrentUsage};
}

std::optional<uint64_t> VkHost::QueryRuntimeBytes() {
  return snippet_.QueryAllocatedBytes();
}

void VkHost::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* /*resource*/, D3D12_RESOURCE_STATES /*before*/,
                        D3D12_RESOURCE_STATES /*after*/) {
  // Every Uplift image rests in GENERAL, so only the memory dependency matters: everything written before is visible to everything after.
  const VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
  };
  functions_.vkCmdPipelineBarrier(VkListOf(list), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &barrier,
                                  0u, nullptr, 0u, nullptr);
}

}  // namespace uplift::nr
