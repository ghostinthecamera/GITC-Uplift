#pragma once

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <memory>
#include <optional>

#include "nr/host.hpp"
#include "nr/snippet.hpp"
#include "vk/nr_functions.hpp"

namespace uplift::nr {

// Plan 13 (design §3.2): the Session's Host on the game's Vulkan device. The snippet is bound with VULKAN_Init_Ext2 on the GAME's device; the
// budget reads the DXGI adapter with the device's LUID (VidMm counts every API's allocations, so the process's local usage is what the game
// uses). The Session that owns a VkHost is built with SessionConfig{.budget = {.first_use_bytes = 640 MiB}} (key decision h): the per-device
// reservation NGX keeps on a Vulkan device (design §1.2). The Session passes the command list and the pass textures through as the opaque
// handles nr/vk_handles.hpp defines; only VkHost, VkFeature and the pipelines turn them back.
class VkHost final : public Host {
 public:
  // `adapter`: the DXGI adapter with the device's LUID (QueryMemory). `functions`: the device's own (Transition's barrier); copied.
  VkHost(SnippetConfig config, VulkanBinding binding, Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter, const vk::NrFunctions& functions);
  ~VkHost() override;
  VkHost(const VkHost&) = delete;
  VkHost& operator=(const VkHost&) = delete;

  NVSDK_NGX_Result LoadRuntime() override;                  // snippet_.Load(config_), then BindVulkan; Unload on failure
  void UnloadRuntime() override;                            // snippet_.Unload()
  void AbandonRuntime() override;                           // snippet_.Abandon()
  std::unique_ptr<FeatureInterface> NewFeature() override;  // VkFeature(snippet_)
  MemoryInfo QueryMemory() override;                        // the adapter's LOCAL segment, logged once on failure
  std::optional<uint64_t> QueryRuntimeBytes() override;
  std::chrono::steady_clock::time_point Now() override { return std::chrono::steady_clock::now(); }
  // Every Uplift image stays in GENERAL: one global memory barrier, ALL_COMMANDS to ALL_COMMANDS, whatever the D3D12 states say.
  void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                  D3D12_RESOURCE_STATES after) override;

  [[nodiscard]] const Snippet& GetSnippet() const { return snippet_; }

 private:
  SnippetConfig config_;
  VulkanBinding binding_;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
  vk::NrFunctions functions_;
  Snippet snippet_;
  bool query_memory_failed_logged_ = false;  // log a failed QueryVideoMemoryInfo once, not every frame
};

}  // namespace uplift::nr
