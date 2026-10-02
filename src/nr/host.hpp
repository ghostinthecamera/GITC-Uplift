#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "nr/budget.hpp"
#include "nr/feature.hpp"

namespace uplift::nr {

// The Session's view of the outside world: RealHost binds it to the snippet,
// DXGI and D3D12; unit tests substitute a fake.
class Host {
 public:
  virtual ~Host() = default;
  virtual NVSDK_NGX_Result LoadRuntime() = 0;
  virtual void UnloadRuntime() = 0;
  // Stops using the runtime after a device loss: restores whatever the
  // runtime patched, keeps it loaded, and never calls into it again.
  virtual void AbandonRuntime() = 0;
  virtual std::unique_ptr<FeatureInterface> NewFeature() = 0;
  virtual MemoryInfo QueryMemory() = 0;
  virtual std::optional<uint64_t> QueryRuntimeBytes() = 0;
  virtual std::chrono::steady_clock::time_point Now() = 0;
  virtual void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                          D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) = 0;
};

}  // namespace uplift::nr
