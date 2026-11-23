#include "addon/hud_present_d3d12.hpp"

#include <mutex>
#include <utility>

#include "nr/log.hpp"

namespace uplift::addon {
namespace {

// Direct3D 12 tags carry no description that Uplift trusts: the resource itself is asked (design §2).
void Describe(TaggedImage& image) {
  if (image.native == 0u) return;
  auto* const resource = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(image.native));
  const D3D12_RESOURCE_DESC description = resource->GetDesc();
  image.described = true;
  image.width = static_cast<uint32_t>(description.Width);
  image.height = description.Height;
  image.format = static_cast<uint32_t>(description.Format);
  image.mips = description.MipLevels;
  image.layers = description.DepthOrArraySize;
  image.samples = description.SampleDesc.Count;
  image.denies_shader_reads = ((description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0);
}

}  // namespace

HudBegun BeginHudD3D12(const HudPresentD3D12& present, ID3D12GraphicsCommandList* list, ID3D12Resource* back_buffer, uint64_t present_number,
                       std::chrono::steady_clock::time_point now) {
  HudBegun begun;
  if (present.tags == nullptr || present.input == nullptr || present.settle == nullptr || present.composer == nullptr || back_buffer == nullptr) return begun;
  TakeResult taken = present.tags->Take();
  if (taken.frame) {
    Describe(taken.frame->hudless);
    Describe(taken.frame->opacity);
  }
  const D3D12_RESOURCE_DESC target = back_buffer->GetDesc();
  const HudFrameFacts facts = {.width = static_cast<uint32_t>(target.Width), .height = target.Height, .format = static_cast<uint32_t>(target.Format),
                               .api = TagApi::D3D12, .early = present.early, .effects_on = present.effects_on,
                               .technique_expected = present.technique_expected};
  begun.reason = DecideHudFrame(present.use_wanted, present.input->FrameGenerationReads(now), taken, facts);
  NoteHudFormats(begun.reason, taken, facts);
  const bool admitted = present.settle->Admit(begun.reason);
  if (begun.reason == HudReason::USABLE && !admitted) begun.reason = HudReason::SETTLING;
  if (begun.reason != HudReason::USABLE) return begun;
  const HudFrame& frame = *taken.frame;
  const color::HudComposerD3D12::Inputs inputs = {
      .back_buffer = back_buffer,
      .hudless = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(frame.hudless.native)),
      .hudless_state = static_cast<D3D12_RESOURCE_STATES>(frame.hudless.state),
      .opacity = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(frame.opacity.native)),
      .opacity_state = static_cast<D3D12_RESOURCE_STATES>(frame.opacity.state),
      .opacity_from_ui_colour = frame.opacity_is_ui_colour,
  };
  begun.n = present.composer->Begin(list, inputs, present_number);
  if (begun.n == nullptr) {
    begun.reason = HudReason::NO_FREE_IMAGE;
    static_cast<void>(present.settle->Admit(begun.reason));  // the next use waits out the settle count again
    return begun;
  }
  // Once per process (design §9): what DLSS-G is given and how the game tags it.
  static std::once_flag logged;
  std::call_once(logged, [&] {
    nr::Logf(nr::LogLevel::INFO, "HUD-less: NR runs on the game's HUD-less image (Direct3D 12): {}x{} format {} state {:#x} lifecycle {}; opacity {} format {} state {:#x}",
             frame.hudless.width, frame.hudless.height, frame.hudless.format, frame.hudless.state, frame.hudless.lifecycle,
             (frame.opacity_is_ui_colour ? "UI colour and alpha" : "UI alpha"), frame.opacity.format, frame.opacity.state);
  });
  begun.inputs = {.hudless = frame.hudless.native, .opacity = frame.opacity.native};
  begun.hold = std::move(taken.hold);
  return begun;
}

}  // namespace uplift::addon
