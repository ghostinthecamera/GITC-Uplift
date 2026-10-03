#pragma once

#include <d3d12.h>

#include <chrono>
#include <string_view>

#include "addon/device_context.hpp"
#include "addon/frame_trigger.hpp"
#include "bridge/d3d11_bridge.hpp"
#include "ngx_hooks/dlss_capture.hpp"

namespace uplift::bridge {

// Plan 9: one bridged recording on the private list -- a fresh UPLIFT_MASK into NR's own copy, then DeviceContext::Run
// on `targets.color` (COMMON in and out). True when NR wrote targets.color. The 64-bit bridges and the helper share it.
bool RecordNr(addon::DeviceContext& context, addon::TriggerPoint point, ID3D12GraphicsCommandList* list,
              const BridgeTargets& targets);

// Plan 18 (design §3): one DLSS stage's hand-off for a decided AFTER_DLSS or BEFORE_UPSCALING, as the add-on's hooked evaluate runs it: the game's Direct3D 11
// resources from `frame` (punned), their regions resolved, D3D11Bridge::RunDlss with the stage's recording, the recording's tokens handed to the context
// (submitted and stamped on the private queue, or dropped when nothing was submitted), and an image the bridge cannot share noted as the stage's problem.
// With the add-on's lock held; never waits on the CPU.
DlssHandoff RunDlssStage(D3D11Bridge& bridge, addon::DeviceContext& context, addon::BridgedDlssWork work, const ngx_hooks::DlssFrame& frame,
                         std::chrono::steady_clock::time_point now);

// Plan 18 (fix round 1, I-1): why a decided stage's hand-off did not bring NR's result back (static text, for DeviceContext::NoteBridgedSkip), or empty
// when the recording was submitted and its own result stands (it wrote, or it said why it did not apply NR). `nr_applied`: the recording's own result.
[[nodiscard]] std::string_view HandoffSkipReason(const DlssHandoff& handoff, bool nr_applied);

}  // namespace uplift::bridge
