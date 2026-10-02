#pragma once

#include <d3d12.h>

#include "addon/device_context.hpp"
#include "addon/frame_trigger.hpp"
#include "bridge/d3d11_bridge.hpp"

namespace uplift::bridge {

// Plan 9: one bridged recording on the private list -- a fresh UPLIFT_MASK into NR's own copy, then DeviceContext::Run
// on `targets.color` (COMMON in and out). True when NR wrote targets.color. The 64-bit bridges and the helper share it.
bool RecordNr(addon::DeviceContext& context, addon::TriggerPoint point, ID3D12GraphicsCommandList* list,
              const BridgeTargets& targets);

}  // namespace uplift::bridge
