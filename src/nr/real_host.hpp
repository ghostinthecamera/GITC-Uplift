#pragma once

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <functional>
#include <optional>

#include "nr/host.hpp"
#include "nr/snippet.hpp"

namespace uplift::nr {

class RealHost final : public Host {
 public:
  // Plan 9 (design §2.10): `game_memory`, when set and it returns a value, is the game process's own DXGI budget and
  // usage. The 32-bit helper is a second process on the same adapter, so QueryMemory reports the tighter of the two.
  RealHost(ID3D12Device* device, SnippetConfig config, std::function<std::optional<MemoryInfo>()> game_memory = {});
  ~RealHost() override;

  NVSDK_NGX_Result LoadRuntime() override;
  void UnloadRuntime() override;
  void AbandonRuntime() override;
  std::unique_ptr<FeatureInterface> NewFeature() override;
  MemoryInfo QueryMemory() override;
  std::optional<uint64_t> QueryRuntimeBytes() override;
  std::chrono::steady_clock::time_point Now() override { return std::chrono::steady_clock::now(); }
  void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                  D3D12_RESOURCE_STATES after) override;

  [[nodiscard]] const Snippet& GetSnippet() const { return snippet_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
  SnippetConfig config_;
  std::function<std::optional<MemoryInfo>()> game_memory_;
  Snippet snippet_;
  bool query_memory_failed_logged_ = false;  // log a failed QueryVideoMemoryInfo once, not every frame
};

}  // namespace uplift::nr
