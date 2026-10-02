#pragma once

#include "nr/snippet.hpp"
#include "nr/types.hpp"

namespace uplift::nr {

struct CreateInfo {
  Size capacity;
  uint32_t preset = 1u;       // DLSSNR.Hint.Render.Preset (no effect in 310.8)
  uint32_t performance = 3u;  // PerfQualityValue = performance - 1 (inert in 310.8)
};

struct EvaluateInputs {
  ID3D12Resource* color = nullptr;   // model domain, NON_PIXEL_SHADER_RESOURCE, same size as output
  ID3D12Resource* output = nullptr;  // RGBA16F, UNORDERED_ACCESS
  BoundResource motion;              // optional: no motion vectors = no temporal history
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  BoundResource control_mask;
  BoundResource backbuffer;
  BoundResource ui;
  BoundResource ui_alpha;
  bool reset = false;
  bool bypass = false;  // DLSSNR.Enabled = 0: plain copy, history reset, VRAM kept
  bool ui_correction = false;
  int32_t depth_inverted = 0;
};

class FeatureInterface {
 public:
  virtual ~FeatureInterface() = default;
  virtual NVSDK_NGX_Result Create(ID3D12GraphicsCommandList* list, const CreateInfo& info) = 0;
  virtual NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* list, const EvaluateInputs& inputs, const Controls& controls) = 0;
  // Only after the GPU has finished every command list that used the feature.
  virtual NVSDK_NGX_Result Release() = 0;
  // Device-lost path only: drops the handle and parameters without calling
  // ReleaseFeature or DestroyParameters. The handle is leaked deliberately.
  virtual void Abandon() = 0;
  [[nodiscard]] virtual bool Created() const = 0;
  [[nodiscard]] virtual Size Capacity() const = 0;
};

class Feature final : public FeatureInterface {
 public:
  explicit Feature(const Snippet& snippet);
  ~Feature() override;
  Feature(const Feature&) = delete;
  Feature& operator=(const Feature&) = delete;

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
