#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

#include "nr/session.hpp"
#include "nr/timeline.hpp"
#include "sources/nr_pipeline.hpp"

namespace uplift::sources {

using PresentResult = PipelineResult;

// Spec §8.1's Present source: NrPipeline::RecordPresent on the primary swap chain's back buffer.
// It owns the device's one NrPipeline; the After-DLSS source (Task 9) shares it through Pipeline().
class PresentSource {
 public:
  PresentSource(ID3D12Device* device, nr::Session& session, nr::Timeline& timeline);
  PresentSource(const PresentSource&) = delete;
  PresentSource& operator=(const PresentSource&) = delete;

  bool Initialize(std::string* error);
  PresentResult Record(ID3D12GraphicsCommandList* list, const PresentTarget& target, const nr::Controls& controls,
                       bool reset_hint, const WorkLayout& layout = {});
  void ReleaseIntermediates();
  [[nodiscard]] uint64_t HeldBytes() const;
  [[nodiscard]] NrPipeline& Pipeline() { return pipeline_; }

 private:
  NrPipeline pipeline_;
};

}  // namespace uplift::sources
