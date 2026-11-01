#include "addon/before_fg_vk.hpp"
#include <algorithm>
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
  std::erase_if(chains_, [device](const auto& entry) { return entry.second.binding.device == device; });
  std::erase_if(hosts_, [device](auto& entry) {
    if (entry.second.device != device) return false;
    if (entry.second.host) entry.second.host->Destroy();
    return true;
  });
}
BeforeFgVk::Frame BeforeFgVk::Prepare(VkQueue queue, const VkPresentInfoKHR& app, color::Encoding encoding) {
  if (app.swapchainCount != 1u || !app.pSwapchains || !app.pImageIndices) return {.reason = "Cannot isolate the primary Vulkan swapchain"};
  if (!app.waitSemaphoreCount || !app.pWaitSemaphores) return {.reason = "Streamline Present has no game-rendered completion signal"};
  const auto it = chains_.find(app.pSwapchains[0]);
  if (it == chains_.end()) return {.reason = "No verified game-facing Vulkan swapchain"};
  auto& chain = it->second;
  if (!chain.simple || (chain.space == color::ColorSpace::UNKNOWN && encoding == color::Encoding::AUTO) || chain.format == DXGI_FORMAT_UNKNOWN)
    return {.reason = "Unsupported outer Vulkan image or color space"};
  constexpr VkImageUsageFlags required = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if ((chain.usage & required) != required) return {.reason = "Outer Vulkan image lacks copy-source or copy-destination usage"};
  const uint32_t index = app.pImageIndices[0];
  if (index >= chain.images.size() || !chain.images[index].acquired) return {.reason = "No matching real-frame image acquisition"};
  for (auto* ext = static_cast<const VkBaseInStructure*>(app.pNext); ext; ext = ext->pNext) {
    if (ext->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR) continue;
    const auto* group = reinterpret_cast<const VkDeviceGroupPresentInfoKHR*>(ext);
    if (group->mode != VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR || !group->pDeviceMasks || group->pDeviceMasks[0] != 1u)
      return {.reason = "Unsupported device-group presentation"};
  }
  if (chain.verified_queue != queue) {
    const auto record = vk::DeviceHook::Find(chain.binding.device);
    if (!record) return {.reason = "The Vulkan device was not observed at creation"};
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
    if (!compatible) return {.reason = "The outer Vulkan queue uses an unsupported queue family"};
    chain.verified_queue = queue;
    chain.family = record->graphics_family;
  }
  auto& image = chain.images[index];
  image.acquired = false;
  return {.binding = chain.binding,
          .image = {.image = image.resource, .size = chain.size, .format = chain.format, .copy_dest = true},
          .space = chain.space, .family = chain.family};
}

vk::PresentRelayHost* BeforeFgVk::BridgeHost(VkQueue queue, const Frame& frame) {
  auto& owner = hosts_[queue];
  if (owner.host) return owner.device == frame.binding.device ? owner.host.get() : nullptr;
  const auto functions = vk::NrFunctions::Open(frame.binding.device, false);
  if (!functions) return nullptr;
  std::string error;
  owner.host = vk::PresentRelayHost::Create(*functions, frame.binding.device, queue, frame.family, &error);
  owner.device = frame.binding.device;
  return owner.host.get();
}

BeforeFgVk::Result BeforeFgVk::Record(VkQueue queue, void* queue_identity, const VkPresentInfoKHR& app,
                                      VkDlssContext& context, const FrameConfig& config, bool holds_nr, std::chrono::steady_clock::time_point now) {
  const Frame frame = Prepare(queue, app, config.encoding);
  if (!frame.reason.empty()) return {.reason = frame.reason};
  const VkPresentFrame target{.back_buffer = frame.image, .color_space = frame.space};
  const auto trigger = context.BeginFrame(queue_identity, config, config.nr_allowed, holds_nr, now, target);
  if (trigger == TriggerPoint::NONE || context.NrState() != nr::SessionState::ACTIVE) return {};
  const VkPresentQueues queues = {.queue = queue, .flush_effects = [] {}, .waits = {app.pWaitSemaphores, app.waitSemaphoreCount}};
  const auto result = context.RunPresent(trigger, vk::Usage::PRESENT, queues, {}, now);
  return {.passes = result.nr_applied ? result.passes_run : 0u, .reason = result.reason};
}
}  // namespace uplift::addon
