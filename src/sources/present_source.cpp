#include "sources/present_source.hpp"

namespace uplift::sources {

PresentSource::PresentSource(ID3D12Device* device, nr::Session& session, nr::Timeline& timeline)
    : pipeline_(device, session, timeline) {}

bool PresentSource::Initialize(std::string* error) {
  return pipeline_.Initialize(error);
}

PresentResult PresentSource::Record(ID3D12GraphicsCommandList* list, const PresentTarget& target,
                                    const nr::Controls& controls, bool reset_hint, const WorkLayout& layout) {
  return pipeline_.RecordPresent(list, target, controls, reset_hint, layout);
}

void PresentSource::ReleaseIntermediates() {
  pipeline_.ReleaseIntermediates();
}

uint64_t PresentSource::HeldBytes() const {
  return pipeline_.HeldBytes();
}

}  // namespace uplift::sources
