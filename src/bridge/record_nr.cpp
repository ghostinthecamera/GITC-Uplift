#include "bridge/record_nr.hpp"

#include <Windows.h>

#include <cstdint>
#include <vector>

#include "nr/d3d11_handles.hpp"
#include "sources/after_dlss_source.hpp"

namespace uplift::bridge {
namespace {

// Plan 18 (design §3): the private list as a DLSS stage's recording sees it: Uplift's own recording only (state known, nothing of the game's to replay), its
// tokens kept for the context until the list is submitted.
class BridgeDlssHost final : public sources::DlssFrameHost {
 public:
  ID3D12GraphicsCommandList* NativeList() override { return list; }
  bool StateKnown() override { return true; }
  void ReplayState(state::RestoreMode /*mode*/) override {}
  void AttachToken(uint64_t token) override { tokens.push_back(token); }
  ID3D12GraphicsCommandList* list = nullptr;
  std::vector<uint64_t> tokens;
};

// One DLSS stage's recording on the private list: a fresh UPLIFT_MASK into NR's own copy (RecordNr's step), the frame NR reads moved onto the shared copies,
// DeviceContext::RecordBridgedStage, and Before upscaling NR's result (the pipeline's private colour) copied into targets.swap. Every target COMMON in and out.
sources::PipelineResult RecordDlssStage(addon::DeviceContext& context, addon::BridgedDlssWork work, const ngx_hooks::DlssFrame& game_frame,
                                        ID3D12GraphicsCommandList* list, const DlssHandoffTargets& targets, BridgeDlssHost& host,
                                        std::chrono::steady_clock::time_point now) {
  constexpr D3D12_RESOURCE_STATES READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;  // DLSS's input state, and NR's pipeline's
  if (targets.mask != nullptr) {
    const D3D12_RESOURCE_DESC description = targets.mask->GetDesc();
    if (ID3D12Resource* const copy = context.PrepareMaskCopy(&description)) {
      Transition(list, copy, SHADER_READ, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(copy, targets.mask);
      Transition(list, copy, D3D12_RESOURCE_STATE_COPY_DEST, SHADER_READ);
      context.NoteMaskCopied();
    }
  }
  const bool after = (work == addon::BridgedDlssWork::AFTER_DLSS);
  // The game's Direct3D 11 pointers never reach a Direct3D 12 call: every resource NR reads is the bridge's copy, or null.
  ngx_hooks::DlssFrame frame = game_frame;
  frame.color = (after ? nullptr : targets.image);
  frame.output = (after ? targets.image : nullptr);
  frame.depth = nullptr;
  frame.motion_vectors = targets.motion;
  frame.motion_region = targets.motion_region;  // fix round 1 (M-4): at (0, 0) inside the vectors' whole-size share
  frame.exposure_texture = targets.exposure;
  if (after) {
    frame.output_region = {0u, 0u, targets.region.width, targets.region.height};
  } else {
    frame.color_region = targets.region;
  }
  const D3D12_RESOURCE_STATES image_state = (after ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : READ);
  Transition(list, targets.image, D3D12_RESOURCE_STATE_COMMON, image_state);
  for (ID3D12Resource* const input : {targets.motion, targets.exposure}) {
    if (input != nullptr) Transition(list, input, D3D12_RESOURCE_STATE_COMMON, READ);
  }
  const sources::PipelineResult result = context.RecordBridgedStage(work, frame, host, now);
  for (ID3D12Resource* const input : {targets.motion, targets.exposure}) {
    if (input != nullptr) Transition(list, input, READ, D3D12_RESOURCE_STATE_COMMON);
  }
  Transition(list, targets.image, image_state, D3D12_RESOURCE_STATE_COMMON);
  if (!after && result.nr_applied) {
    ID3D12Resource* const color = context.PrivateColor();  // NON_PIXEL_SHADER_RESOURCE after RecordPreSr (nr_pipeline.cpp's COMPUTE_READ)
    Transition(list, color, READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list, targets.swap, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(targets.swap, color);
    Transition(list, targets.swap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, READ);
  }
  return result;
}

}  // namespace

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
  // Plan 18: Direct3D 11's ring slot carries DLSS's scale as values (1 for every other source).
  return context
      .Run(&host, D3D12_RESOURCE_STATE_COMMON, point, targets.motion, targets.motion_is_dlss, targets.motion_scale_x, targets.motion_scale_y,
           targets.motion_region)
      .nr_applied;
}

DlssHandoff RunDlssStage(D3D11Bridge& bridge, addon::DeviceContext& context, addon::BridgedDlssWork work, const ngx_hooks::DlssFrame& frame,
                         std::chrono::steady_clock::time_point now) {
  const bool after = (work == addon::BridgedDlssWork::AFTER_DLSS);
  ID3D11Resource* const image = nr::D3D11ResourceOf(after ? frame.output : frame.color);
  ID3D11Resource* const motion = nr::D3D11ResourceOf(frame.motion_vectors);
  const DlssHandoffInput input = {
      .stage = (after ? DlssStage::AFTER_DLSS : DlssStage::BEFORE_UPSCALING),
      .image = image,
      .region = ResolveRegion11(image, (after ? frame.output_region : frame.color_region)),
      .motion = motion,
      .motion_region = ResolveRegion11(motion, frame.motion_region),
      .exposure = nr::D3D11ResourceOf(frame.exposure_texture),
  };
  BridgeDlssHost host;
  sources::PipelineResult recorded;
  const DlssHandoff handoff = bridge.RunDlss(
      input,
      [&](ID3D12GraphicsCommandList* list, const DlssHandoffTargets& targets) {
        host.list = list;
        recorded = RecordDlssStage(context, work, frame, list, targets, host, now);
        return recorded.nr_applied;
      },
      now);
  if (!host.tokens.empty()) {
    if (handoff.executed) {
      context.SubmitTokens(host.tokens, bridge.Queue(), GetCurrentThreadId(), now);  // after the native submit: stamped at once
      context.StampThread(GetCurrentThreadId());
    } else {
      context.DropTokens(host.tokens);
    }
  }
  if (handoff.skipped == DlssSkip::UNSUPPORTED_FORMAT) {
    context.NoteStageProblem(work, handoff.problem);
  } else if (recorded.reason == "unsupported format") {
    context.NoteStageProblem(work, "the private Direct3D 12 device cannot write DLSS's output format");
  }
  // Fix round 1 (I-1): an evaluate whose NR never came back says so, and owes NR's history a reset, as Direct3D 12's own skips do.
  if (const std::string_view reason = HandoffSkipReason(handoff, recorded.nr_applied); !reason.empty()) {
    context.NoteBridgedSkip(work, reason);
  }
  return handoff;
}

std::string_view HandoffSkipReason(const DlssHandoff& handoff, bool nr_applied) {
  switch (handoff.skipped) {
    case DlssSkip::STOPPED:            return "bridge stopped";
    case DlssSkip::BUSY:               return "bridge busy";
    case DlssSkip::NO_INPUT:           return "DLSS's region lies outside its texture";
    case DlssSkip::UNSUPPORTED_FORMAT: return "unsupported format";
    case DlssSkip::SHARE_FAILED:       return "the bridge could not share DLSS's image";
    case DlssSkip::NONE:               break;
  }
  if (!handoff.executed) return "the private list was not submitted";  // the recording failed or threw: nothing ran
  if (nr_applied && !handoff.wrote) return "NR's result did not come back";  // the game's wait was refused (the bridge stopped)
  return {};
}

}  // namespace uplift::bridge
