#include "addon/reshade_vk_host.hpp"

#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>

#include "addon/environment.hpp"

namespace uplift::addon {
namespace {

namespace api = reshade::api;

// vk::Usage is ReShade's api::resource_usage, numbered the same (reshade_api_resource.hpp).
static_assert(static_cast<uint32_t>(vk::Usage::UNDEFINED) == static_cast<uint32_t>(api::resource_usage::undefined));
static_assert(static_cast<uint32_t>(vk::Usage::RENDER_TARGET) == static_cast<uint32_t>(api::resource_usage::render_target));
static_assert(static_cast<uint32_t>(vk::Usage::SHADER_RESOURCE) == static_cast<uint32_t>(api::resource_usage::shader_resource));
static_assert(static_cast<uint32_t>(vk::Usage::UNORDERED_ACCESS) == static_cast<uint32_t>(api::resource_usage::unordered_access));
static_assert(static_cast<uint32_t>(vk::Usage::COPY_DEST) == static_cast<uint32_t>(api::resource_usage::copy_dest));
static_assert(static_cast<uint32_t>(vk::Usage::COPY_SOURCE) == static_cast<uint32_t>(api::resource_usage::copy_source));
static_assert(static_cast<uint32_t>(vk::Usage::GENERAL) == static_cast<uint32_t>(api::resource_usage::general));
static_assert(static_cast<uint32_t>(vk::Usage::PRESENT) == static_cast<uint32_t>(api::resource_usage::present));

api::resource_usage UsageOf(vk::Usage usage) {
  return static_cast<api::resource_usage>(static_cast<uint32_t>(usage));
}

}  // namespace

vk::ImageInfo VulkanImageInfo(api::device* device, api::resource resource) {
  if (resource.handle == 0u) return {};
  const api::resource_desc description = device->get_resource_desc(resource);
  return {
      .image = VulkanHandleOf<VkImage>(resource.handle),
      .size = {description.texture.width, description.texture.height},
      .format = static_cast<DXGI_FORMAT>(description.texture.format),
      .samples = description.texture.samples,
      .copy_dest = (static_cast<uint32_t>(description.usage & api::resource_usage::copy_dest) != 0u),
  };
}

bool VulkanAdapterLuid(api::device* device, LUID* luid, std::string* error) {
  uint32_t vendor_id = 0u;
  if (!device->get_property(api::device_properties::vendor_id, &vendor_id) || vendor_id != NVIDIA_VENDOR_ID) {
    *error = "Uplift needs an NVIDIA GPU; this game renders on another adapter";
    return false;
  }
  bool found = device->get_property(static_cast<api::device_properties>(9), luid);
  if (!found) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description = {};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      for (UINT index = 0u; !found && SUCCEEDED(factory->EnumAdapters1(index, &adapter)); ++index) {
        if (SUCCEEDED(adapter->GetDesc1(&description)) && description.VendorId == NVIDIA_VENDOR_ID) {
          *luid = description.AdapterLuid;
          found = true;
        }
      }
    }
  }
  if (!found) {
    *error = "the game's adapter could not be identified";
  }
  return found;
}

void ReshadeVkHost::Barrier(VkImage image, vk::Usage before, vk::Usage after) {
  if (list_ == nullptr) return;
  // A null image is ReShade's global memory barrier (resource 0), ALL_COMMANDS to ALL_COMMANDS.
  list_->barrier(api::resource{ReshadeHandleOf(image)}, UsageOf(before), UsageOf(after));
}

void ReshadeVkHost::BarrierWithGlobal(VkImage image, vk::Usage before, vk::Usage after, vk::Usage global_before, vk::Usage global_after) {
  if (list_ == nullptr) return;
  // One call, so ReShade ORs both entries' stage masks into one vkCmdPipelineBarrier (resource 0 is the global memory barrier).
  const api::resource resources[] = {api::resource{0u}, api::resource{ReshadeHandleOf(image)}};
  const api::resource_usage old_states[] = {UsageOf(global_before), UsageOf(before)};
  const api::resource_usage new_states[] = {UsageOf(global_after), UsageOf(after)};
  list_->barrier(2u, resources, old_states, new_states);
}

void ReshadeVkHost::Copy(VkImage source, VkImage destination) {
  if (list_ == nullptr) return;
  list_->copy_texture_region(api::resource{ReshadeHandleOf(source)}, 0u, nullptr, api::resource{ReshadeHandleOf(destination)}, 0u, nullptr);
}

bool ReshadeVkHost::Signal(VkSemaphore timeline, uint64_t value) {
  return queue_ != nullptr && queue_->signal(api::fence{ReshadeHandleOf(timeline)}, value);
}

bool ReshadeVkHost::Wait(VkSemaphore timeline, uint64_t value) {
  return queue_ != nullptr && queue_->wait(api::fence{ReshadeHandleOf(timeline)}, value);
}

bool ReshadeVkHost::FlushWithFence(VkFence fence) {
  if (queue_ == nullptr || submit_ == nullptr) return false;
  queue_->flush_immediate_command_list();
  return submit_(VulkanHandleOf<VkQueue>(queue_->get_native()), 0u, nullptr, fence) == VK_SUCCESS;
}

void ReshadeVkHost::DestroyImage(VkImage image) {
  if (device_ != nullptr) {
    device_->destroy_resource(api::resource{ReshadeHandleOf(image)});
  }
}

}  // namespace uplift::addon
