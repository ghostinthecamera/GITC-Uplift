#include "addon/before_fg_vk.hpp"
#include <algorithm>
#include "text/before_fg_messages.hpp"
#include "vk/loader.hpp"
#include "nr/log.hpp"

namespace uplift::addon {
namespace {
DXGI_FORMAT PresentFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
color::ColorSpace PresentColorSpace(VkColorSpaceKHR value) {
  switch (value) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return color::ColorSpace::SRGB_NONLINEAR;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return color::ColorSpace::EXTENDED_SRGB_LINEAR;
    case VK_COLOR_SPACE_HDR10_ST2084_EXT: return color::ColorSpace::HDR10_ST2084;
    default: return color::ColorSpace::UNKNOWN;
  }
}
template<class Fn>
Fn Function(VkDevice device, const char* name) {
  const auto* loader = vk::Loader::Get();
  return loader && loader->get_device_proc_addr ? reinterpret_cast<Fn>(loader->get_device_proc_addr(device, name)) : nullptr;
}
}  // namespace

void BeforeFgVk::Created(VkDevice device, VkSwapchainKHR chain, const VkSwapchainCreateInfoKHR& info, void* inner) {
  auto& entry = chains_[chain];
  entry.images.clear();
  entry.verified_queue = VK_NULL_HANDLE;
  entry.binding = {device, inner};
  entry.size = {info.imageExtent.width, info.imageExtent.height};
  entry.format = PresentFormat(info.imageFormat);
  entry.vk_format = info.imageFormat;
  entry.space = PresentColorSpace(info.imageColorSpace);
  entry.usage = info.imageUsage;
  entry.simple = info.imageArrayLayers == 1u && !(info.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
      && info.presentMode != VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR && info.presentMode != VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR;
}
void BeforeFgVk::Images(VkDevice device, VkSwapchainKHR chain, uint32_t count, const VkImage* images) {
  auto found = chains_.find(chain);
  if (found == chains_.end() || found->second.binding.device != device || !images) return;
  const auto& current = found->second.images;
  const bool same = current.size() == count && std::equal(current.begin(), current.end(), images,
                                                         [](const Image& item, VkImage resource) { return item.resource == resource; });
  if (!same) {
    found->second.images.clear();
    found->second.images.resize(count);
    for (uint32_t i = 0; i < count; ++i) found->second.images[i].resource = images[i];
  }
}
uint64_t BeforeFgVk::ImageOf(VkSwapchainKHR chain, uint32_t index) const {
  const auto found = chains_.find(chain);
  if (found == chains_.end() || index >= found->second.images.size()) return 0u;
  return reinterpret_cast<uint64_t>(found->second.images[index].resource);
}
void BeforeFgVk::Acquired(VkDevice device, VkSwapchainKHR chain, uint32_t index) {
  auto found = chains_.find(chain);
  if (found != chains_.end() && found->second.binding.device == device && index < found->second.images.size()) found->second.images[index].acquired = true;
}
BeforeFgVk::Binding BeforeFgVk::Find(VkSwapchainKHR chain) const {
  const auto found = chains_.find(chain);
  return found == chains_.end() ? Binding{} : found->second.binding;
}
void BeforeFgVk::Destroyed(VkDevice device, VkSwapchainKHR chain) {
  const auto found = chains_.find(chain);
  if (found != chains_.end() && found->second.binding.device == device) chains_.erase(found);
}
void BeforeFgVk::DestroyDevice(VkDevice device) {
  if (const auto found = composers_.find(device); found != composers_.end()) {
    if (found->second) found->second->Destroy();
    composers_.erase(found);
  }
  if (hud_.composer != nullptr && !composers_.contains(device)) hud_ = {};
  std::erase_if(chains_, [device](const auto& entry) { return entry.second.binding.device == device; });
  std::erase_if(hosts_, [device](auto& entry) {
    if (entry.second.device != device) return false;
    if (entry.second.host) entry.second.host->Destroy();
    return true;
  });
}
BeforeFgVk::Frame BeforeFgVk::Prepare(VkQueue queue, const VkPresentInfoKHR& app, color::Encoding encoding, const HudPresentVk* hud) {
  hud_ = {.wanted = (hud != nullptr && hud->use_wanted)};
  // This Present's tags are taken first, whatever happens below, so none outlive it.
  TakeResult taken;
  if (hud_.wanted && hud->tags != nullptr) taken = hud->tags->Take();
  if (app.swapchainCount != 1u || !app.pSwapchains || !app.pImageIndices) return {.reason = uplift::text::PRIMARY_VK_SWAPCHAIN_LINE};
  if (!app.waitSemaphoreCount || !app.pWaitSemaphores) return {.reason = uplift::text::GAME_COMPLETION_SIGNAL_LINE};
  const auto it = chains_.find(app.pSwapchains[0]);
  if (it == chains_.end()) return {.reason = uplift::text::VERIFIED_VK_SWAPCHAIN_LINE};
  auto& chain = it->second;
  if (!chain.simple || (chain.space == color::ColorSpace::UNKNOWN && encoding == color::Encoding::AUTO) || chain.format == DXGI_FORMAT_UNKNOWN)
    return {.reason = uplift::text::OUTER_VK_IMAGE_LINE};
  constexpr VkImageUsageFlags required = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if ((chain.usage & required) != required) return {.reason = uplift::text::OUTER_VK_IMAGE_USAGE_LINE};
  const uint32_t index = app.pImageIndices[0];
  if (index >= chain.images.size() || !chain.images[index].acquired) return {.reason = uplift::text::REAL_FRAME_ACQUISITION_LINE};
  for (auto* ext = static_cast<const VkBaseInStructure*>(app.pNext); ext; ext = ext->pNext) {
    if (ext->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR) continue;
    const auto* group = reinterpret_cast<const VkDeviceGroupPresentInfoKHR*>(ext);
    if (group->mode != VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR || !group->pDeviceMasks || group->pDeviceMasks[0] != 1u)
      return {.reason = uplift::text::DEVICE_GROUP_PRESENT_LINE};
  }
  if (chain.verified_queue != queue) {
    const auto record = vk::DeviceHook::Find(chain.binding.device);
    if (!record) return {.reason = uplift::text::VK_DEVICE_UNOBSERVED_LINE};
    const auto get_queue = Function<PFN_vkGetDeviceQueue>(chain.binding.device, "vkGetDeviceQueue");
    bool compatible = false;
    if (get_queue) {
      for (const auto& family : record->queue_families) {
        if (family.index != record->graphics_family || family.flags != 0u) continue;
        for (uint32_t i = 0; i < family.count && !compatible; ++i) {
          VkQueue candidate{};
          get_queue(chain.binding.device, family.index, i, &candidate);
          compatible = candidate == queue;
        }
      }
    }
    if (!compatible) return {.reason = uplift::text::OUTER_VK_QUEUE_FAMILY_LINE};
    chain.verified_queue = queue;
    chain.family = record->graphics_family;
  }
  auto& image = chain.images[index];
  image.acquired = false;
  Frame frame = {.binding = chain.binding,
                 .image = {.image = image.resource, .size = chain.size, .format = chain.format, .copy_dest = true},
                 .space = chain.space, .family = chain.family};
  frame.record_image = frame.image;
  if (hud_.wanted) BeginHud(*hud, queue, app, chain, std::move(taken), &frame);
  return frame;
}

color::HudComposerVk* BeforeFgVk::ComposerFor(const Chain& chain) {
  const VkDevice device = chain.binding.device;
  if (const auto found = composers_.find(device); found != composers_.end()) return found->second.get();
  auto& slot = composers_[device];
  const auto record = vk::DeviceHook::Find(device);
  const vk::Loader* const loader = vk::Loader::Get();
  const std::optional<vk::NrFunctions> functions = vk::NrFunctions::Open(device, /*pipeline=*/false);
  if (!record || loader == nullptr || loader->get_memory_properties == nullptr || !functions) {
    nr::Log(nr::LogLevel::WARN, "HUD-less (Vulkan): the device's functions or record are missing; the HUD-less image is hidden from frame generation");
    return nullptr;
  }
  VkPhysicalDeviceMemoryProperties memory = {};
  loader->get_memory_properties(record->physical, &memory);
  std::vector<uint32_t> families;
  for (const auto& family : record->queue_families) families.push_back(family.index);
  auto composer = std::make_unique<color::HudComposerVk>();
  std::string error;
  if (!composer->Initialize(*functions, device, memory, chain.family, families, &error)) {
    nr::Logf(nr::LogLevel::WARN, "HUD-less (Vulkan): {}; the HUD-less image is hidden from frame generation", error);
    composer->Destroy();
    return nullptr;
  }
  slot = std::move(composer);
  return slot.get();
}

void BeforeFgVk::BeginHud(const HudPresentVk& hud, VkQueue queue, const VkPresentInfoKHR& app, const Chain& chain, TakeResult taken, Frame* frame) {
  if (hud.input == nullptr || hud.settle == nullptr) return;
  const HudFrameFacts facts = {.width = chain.size.width, .height = chain.size.height, .format = static_cast<uint32_t>(chain.vk_format),
                               .api = TagApi::VULKAN};
  hud_.reason = DecideHudFrame(true, hud.input->FrameGenerationReads(hud.now), taken, facts);
  NoteHudFormats(hud_.reason, taken, facts);
  const bool admitted = hud.settle->Admit(hud_.reason);
  if (hud_.reason == HudReason::USABLE && !admitted) hud_.reason = HudReason::SETTLING;
  color::HudComposerVk* const composer = (hud_.reason == HudReason::USABLE ? ComposerFor(chain) : nullptr);
  if (composer != nullptr) composer->Retire(hud.input->Presents());
  if (hud_.reason != HudReason::USABLE) return;
  const HudFrame* const unused = nullptr;
  static_cast<void>(unused);
  const auto& tagged = *taken.frame;
  const color::HudComposerVk::Inputs inputs = {
      .back_buffer = frame->image.image, .format = chain.vk_format, .size = {chain.size.width, chain.size.height},
      .hudless = reinterpret_cast<VkImage>(static_cast<uintptr_t>(tagged.hudless.native)), .hudless_format = static_cast<VkFormat>(tagged.hudless.format),
      .hudless_layout = static_cast<VkImageLayout>(tagged.hudless.state),
      .opacity = reinterpret_cast<VkImage>(static_cast<uintptr_t>(tagged.opacity.native)), .opacity_format = static_cast<VkFormat>(tagged.opacity.format),
      .opacity_layout = static_cast<VkImageLayout>(tagged.opacity.state), .opacity_from_ui_colour = tagged.opacity_is_ui_colour};
  const std::span<const VkSemaphore> waits = {app.pWaitSemaphores, app.waitSemaphoreCount};
  const auto begun = (composer != nullptr ? composer->Begin(queue, waits, inputs, hud.input->Presents()) : std::nullopt);
  if (!begun) {
    hud_.reason = HudReason::NO_FREE_IMAGE;
    static_cast<void>(hud.settle->Admit(hud_.reason));  // the next use waits out the settle count again
    return;
  }
  frame->record_image = {.image = begun->image, .size = chain.size, .format = chain.format, .copy_dest = true};
  hud_.composer = composer;
  hud_.queue = queue;
  hud_.waits.assign(waits.begin(), waits.end());
  hud_.inputs = {.hudless = tagged.hudless.native, .opacity = tagged.opacity.native};
  hud_.ngx = begun->ngx;
}

void BeforeFgVk::ReleaseHud(uint64_t present_number) {
  for (auto& [device, composer] : composers_) {
    if (!composer) continue;
    composer->Release(present_number);
    composer->Retire(present_number);
  }
}

BeforeFgVk::HudOutcome BeforeFgVk::FinishHud(bool applied, bool forwarding_failed) {
  HudOutcome outcome = {.wanted = hud_.wanted, .reason = hud_.reason};
  if (hud_.composer != nullptr) {
    const std::span<const VkSemaphore> waits = (forwarding_failed ? std::span<const VkSemaphore>() : std::span<const VkSemaphore>(hud_.waits));
    const bool submitted = (hud_.composer->EndAndClose(hud_.queue, waits, applied) == VK_SUCCESS);
    if (applied && submitted) {
      outcome.replacement = {.owner = hud_.composer->Owner(), .native = const_cast<NVSDK_NGX_Resource_VK*>(hud_.ngx)};
      outcome.inputs = hud_.inputs;
    }
  }
  hud_ = {};
  return outcome;
}

BeforeFgVk::HostResult BeforeFgVk::BridgeHost(VkQueue queue, const Frame& frame) {
  auto& owner = hosts_[queue];
  if (owner.host) {
    if (owner.device != frame.binding.device) return {.reason = text::REAL_FRAME_VK_RECORDING_LINE};
    return {.host = owner.host.get()};
  }
  if (owner.device != frame.binding.device) owner = {.device = frame.binding.device};
  const auto functions = vk::NrFunctions::Open(frame.binding.device, false);
  if (functions) {
    owner.host = vk::PresentRelayHost::Create(*functions, frame.binding.device, queue, frame.family, &owner.creation_error);
  } else {
    owner.creation_error = text::VK_RECORDING_FUNCTIONS_LINE;
  }
  if (!owner.host) {
    if (!owner.warned) {
      owner.warned = true;
      nr::Logf(nr::LogLevel::WARN, "Before FG: Vulkan recording host could not be created on queue {} (device {}): {}; later presents will retry",
               static_cast<void*>(queue), static_cast<void*>(frame.binding.device), owner.creation_error);
    }
    return {.reason = owner.creation_error};
  }
  owner.creation_error.clear();
  owner.warned = false;
  return {.host = owner.host.get()};
}

BeforeFgVk::Result BeforeFgVk::Record(VkQueue queue, void* queue_identity, const VkPresentInfoKHR& app,
                                      VkDlssContext& context, const FrameSettings& settings, const FrameFacts& facts, bool holds_nr, bool& outer_ticked, std::chrono::steady_clock::time_point now,
                                      const HudPresentVk* hud) {
  const Frame frame = Prepare(queue, app, settings.encoding, hud);
  if (!frame.reason.empty()) return {.reason = frame.reason};
  const VkPresentFrame target{.back_buffer = frame.record_image, .color_space = frame.space};  // N when the HUD-less image is used
  const auto trigger = context.BeginOuterFrame(outer_ticked, queue_identity, settings, facts, facts.nr_allowed, holds_nr, now, target);
  if (trigger == TriggerPoint::NONE || context.NrState() != nr::SessionState::ACTIVE) {
    context.CancelFrame();  // HUD-less (design §5): nothing stays armed for a later point
    return {.image = reinterpret_cast<uint64_t>(frame.image.image)};
  }
  const VkPresentQueues queues = {.queue = queue, .flush_effects = [] {}, .waits = {app.pWaitSemaphores, app.waitSemaphoreCount}};
  const auto result = context.RunPresent(trigger, vk::Usage::PRESENT, queues, {}, now);
  return {.passes = result.nr_applied ? result.passes_run : 0u, .reason = nr::SkipReasonText(result.reason),
          .image = reinterpret_cast<uint64_t>(frame.image.image)};
}
}  // namespace uplift::addon
