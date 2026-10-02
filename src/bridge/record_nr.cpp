#include "bridge/record_nr.hpp"

namespace uplift::bridge {

bool RecordNr(addon::DeviceContext& context, addon::TriggerPoint point, ID3D12GraphicsCommandList* list,
              const BridgeTargets& targets) {
  if (targets.mask != nullptr) {
    // Key decision b: a fresh UPLIFT_MASK into NR's own copy, which it binds from this recording on.
    const D3D12_RESOURCE_DESC description = targets.mask->GetDesc();
    if (ID3D12Resource* const copy = context.PrepareMaskCopy(&description)) {
      Transition(list, copy, SHADER_READ, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(copy, targets.mask);
      Transition(list, copy, D3D12_RESOURCE_STATE_COPY_DEST, SHADER_READ);
      context.NoteMaskCopied();
    }
  }
  ListFrameHost host(list, targets.color);
  return context.Run(&host, D3D12_RESOURCE_STATE_COMMON, point, targets.motion, targets.motion_is_dlss).nr_applied;
}

}  // namespace uplift::bridge
