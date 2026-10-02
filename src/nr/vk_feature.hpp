#pragma once

#include "nr/feature.hpp"
#include "nr/snippet.hpp"

namespace uplift::nr {

// Plan 13 (design §3.2): nr::Feature's body on a Vulkan-bound Snippet. The command list is a VkCommandBuffer and every ID3D12Resource* an
// EvaluateInputs carries is a pointer to an Uplift-owned NVSDK_NGX_Resource_VK that outlives the call (nr/vk_handles.hpp turns them back, and
// nothing else here may). The keys are exactly Feature's (the research probe checked them on Vulkan). Direct3D 12's Feature is untouched.
class VkFeature final : public FeatureInterface {
 public:
  explicit VkFeature(const Snippet& snippet);
  ~VkFeature() override;
  VkFeature(const VkFeature&) = delete;
  VkFeature& operator=(const VkFeature&) = delete;

  NVSDK_NGX_Result Create(ID3D12GraphicsCommandList* list, const CreateInfo& info) override;
  NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* list, const EvaluateInputs& inputs, const Controls& controls) override;
  NVSDK_NGX_Result Release() override;
  void Abandon() override;
  [[nodiscard]] bool Created() const override { return handle_ != nullptr; }
  [[nodiscard]] Size Capacity() const override { return capacity_; }

 private:
  const Snippet& snippet_;
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  NVSDK_NGX_Handle* handle_ = nullptr;
  Size capacity_;
};

}  // namespace uplift::nr
