#include "color/hud_composer_vk.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "hud_compose_vk_rgb10a2.h"
#include "hud_compose_vk_rgba16f.h"
#include "hud_compose_vk_rgba8.h"
#include "nr/log.hpp"
#include "vk/loader.hpp"

namespace uplift::color {
namespace {

constexpr uint32_t SAMPLED_BINDINGS = 4u;  // F, N, H, A: t0..t3
constexpr uint32_t STORAGE_BINDING = 16u;  // u0, after -fvk-u-shift 16
constexpr uint32_t GROUP = 8u;
constexpr uint32_t SETS_PER_POOL = HudComposerVk::RING * 8u;  // the live ring and up to seven retiring ones
constexpr uint32_t ALPHA_IN_RED = 0u;
constexpr uint32_t ALPHA_IN_ALPHA = 3u;
constexpr VkImageLayout READ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageSubresourceRange COLOR_RANGE = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
constexpr VkImageSubresourceLayers COLOR_LAYERS = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};

struct HudCompose {
  uint32_t alpha_channel;
  uint32_t clamp_unit;
  uint32_t swap_rb;
  uint32_t width;
  uint32_t height;
};

enum class Storage : uint8_t { RGBA8, RGB10A2, RGBA16F, NONE };

// The back buffer's pass: the storage format it is written through, its stored-value (UNORM or float) format, and whether channels swap.
struct Pass {
  Storage storage = Storage::NONE;
  VkFormat storage_format = VK_FORMAT_UNDEFINED;
  VkFormat stored = VK_FORMAT_UNDEFINED;
  bool swap_rb = false;
  bool unit = true;
};

Pass PassFor(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return {Storage::RGBA8, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, false, true};
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: return {Storage::RGBA8, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, true, true};
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return {Storage::RGB10A2, format, format, false, true};
    case VK_FORMAT_R16G16B16A16_SFLOAT: return {Storage::RGBA16F, format, format, false, false};
    default: return {};
  }
}

// The UNORM sibling a pass reads an sRGB-typed image's stored values through.
VkFormat Stored(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return format;
  }
}

bool IsSrgb(VkFormat format) { return (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB); }

uint32_t AlphaChannel(VkFormat format, bool ui_colour) {
  if (ui_colour) return ALPHA_IN_ALPHA;
  switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return ALPHA_IN_RED;
    default: return ALPHA_IN_ALPHA;
  }
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to) {
  return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = COLOR_RANGE};
}

}  // namespace

bool HudComposerVk::Initialize(const vk::NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, uint32_t family,
                               std::span<const uint32_t> queue_families, std::string* error) {
  functions_ = functions;
  device_ = device;
  memory_ = memory;
  families_.assign(queue_families.begin(), queue_families.end());
  const vk::Loader* const loader = vk::Loader::Get();
  if (loader == nullptr) {
    *error = "the HUD-less composer found no Vulkan loader";
    return false;
  }
  const auto resolve = [&](const char* name) { return loader->get_device_proc_addr(device, name); };
  vkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(resolve("vkCreateDescriptorPool"));
  vkDestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(resolve("vkDestroyDescriptorPool"));
  vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(resolve("vkAllocateDescriptorSets"));
  vkFreeDescriptorSets = reinterpret_cast<PFN_vkFreeDescriptorSets>(resolve("vkFreeDescriptorSets"));
  vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(resolve("vkUpdateDescriptorSets"));
  vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(resolve("vkCmdBindDescriptorSets"));
  vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(resolve("vkCmdPushConstants"));
  if (!vkCreateDescriptorPool || !vkDestroyDescriptorPool || !vkAllocateDescriptorSets || !vkFreeDescriptorSets || !vkUpdateDescriptorSets
      || !vkCmdBindDescriptorSets || !vkCmdPushConstants) {
    *error = "the HUD-less composer's descriptor functions are missing";
    return false;
  }
  begin_commands_ = vk::CommandRing::Create(functions_, device_, family, error);
  end_commands_ = (begin_commands_ ? vk::CommandRing::Create(functions_, device_, family, error) : nullptr);
  if (!begin_commands_ || !end_commands_) return false;
  std::array<VkDescriptorSetLayoutBinding, SAMPLED_BINDINGS + 1u> bindings = {};
  for (uint32_t i = 0; i < SAMPLED_BINDINGS; ++i) {
    bindings[i] = {.binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  }
  bindings[SAMPLED_BINDINGS] = {.binding = STORAGE_BINDING, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1u,
                                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  const VkDescriptorSetLayoutCreateInfo set_layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                      .bindingCount = static_cast<uint32_t>(bindings.size()), .pBindings = bindings.data()};
  const VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0u, .size = sizeof(HudCompose)};
  if (functions_.vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_) != VK_SUCCESS) {
    *error = "the HUD-less composer's descriptor set layout could not be made";
    return false;
  }
  const VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1u,
                                                      .pSetLayouts = &set_layout_, .pushConstantRangeCount = 1u, .pPushConstantRanges = &push};
  const std::array<VkDescriptorPoolSize, 2> sizes = {{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS_PER_POOL * SAMPLED_BINDINGS},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS_PER_POOL}}};
  const VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                           .maxSets = SETS_PER_POOL, .poolSizeCount = static_cast<uint32_t>(sizes.size()), .pPoolSizes = sizes.data()};
  if (functions_.vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_) != VK_SUCCESS
      || vkCreateDescriptorPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) {
    *error = "the HUD-less composer's pipeline layout or descriptor pool could not be made";
    return false;
  }
  // One pipeline per storage format. The header's array is bytes; vkCreateShaderModule wants 4-byte-aligned words.
  const std::array<std::span<const unsigned char>, 3> shaders = {std::span<const unsigned char>(g_hud_compose_vk_rgba8),
                                                                 std::span<const unsigned char>(g_hud_compose_vk_rgb10a2),
                                                                 std::span<const unsigned char>(g_hud_compose_vk_rgba16f)};
  for (size_t index = 0; index < shaders.size(); ++index) {
    std::vector<uint32_t> words((shaders[index].size() + 3u) / 4u);
    std::memcpy(words.data(), shaders[index].data(), shaders[index].size());
    const VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shaders[index].size(),
                                                  .pCode = words.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (functions_.vkCreateShaderModule(device_, &module_info, nullptr, &module) != VK_SUCCESS) continue;
    const VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = pipeline_layout_};
    if (functions_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1u, &pipeline_info, nullptr, &pipelines_[index]) != VK_SUCCESS) {
      pipelines_[index] = VK_NULL_HANDLE;
    }
    functions_.vkDestroyShaderModule(device_, module, nullptr);
  }
  return true;
}

bool HudComposerVk::MakeImage(VkFormat format, VkExtent2D size, VkImageUsageFlags usage, bool mutable_format, VkFormat view_format, Owned* out) {
  const bool concurrent = (families_.size() > 1u);
  const VkImageCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = (mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0u), .imageType = VK_IMAGE_TYPE_2D,
      .format = format, .extent = {size.width, size.height, 1u}, .mipLevels = 1u, .arrayLayers = 1u, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = (concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE),
      .queueFamilyIndexCount = (concurrent ? static_cast<uint32_t>(families_.size()) : 0u), .pQueueFamilyIndices = (concurrent ? families_.data() : nullptr),
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  if (functions_.vkCreateImage(device_, &create, nullptr, &out->image) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements = {};
  functions_.vkGetImageMemoryRequirements(device_, out->image, &requirements);
  std::optional<uint32_t> type;
  for (uint32_t i = 0; i < memory_.memoryTypeCount && !type; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) != 0u && (memory_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) type = i;
  }
  const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type.value_or(0u)};
  if (!type || functions_.vkAllocateMemory(device_, &allocate, nullptr, &out->memory) != VK_SUCCESS
      || functions_.vkBindImageMemory(device_, out->image, out->memory, 0u) != VK_SUCCESS) {
    return false;
  }
  out->view = MakeView(out->image, view_format);
  return (out->view != VK_NULL_HANDLE);
}

VkImageView HudComposerVk::MakeView(VkImage image, VkFormat format) {
  const VkImageViewCreateInfo create = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = format, .subresourceRange = COLOR_RANGE};
  VkImageView view = VK_NULL_HANDLE;
  if (functions_.vkCreateImageView(device_, &create, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
  return view;
}

void HudComposerVk::Free(Owned& owned) {
  if (owned.ngx_view) functions_.vkDestroyImageView(device_, owned.ngx_view, nullptr);
  if (owned.view) functions_.vkDestroyImageView(device_, owned.view, nullptr);
  if (owned.image) functions_.vkDestroyImage(device_, owned.image, nullptr);
  if (owned.memory) functions_.vkFreeMemory(device_, owned.memory, nullptr);
  owned = {};
}

void HudComposerVk::FreeRing(Ring& ring) {
  for (Slot& slot : ring.slots) {
    for (VkImageView view : slot.transient) functions_.vkDestroyImageView(device_, view, nullptr);
    slot.transient.clear();
    if (slot.set) vkFreeDescriptorSets(device_, pool_, 1u, &slot.set);
    if (slot.n) Free(*slot.n);
    slot = {};
  }
  Free(ring.final_copy);
  Free(ring.hudless_copy);
  Free(ring.output);
}

bool HudComposerVk::SlotDone(const Slot& slot) const { return (!slot.ring_slot || end_commands_->SlotFree(*slot.ring_slot)); }

bool HudComposerVk::EnsureRing(const Inputs& inputs, uint64_t present_number) {
  if (ring_ && ring_->format == inputs.format && ring_->hudless_format == inputs.hudless_format && ring_->size.width == inputs.size.width
      && ring_->size.height == inputs.size.height) {
    return true;
  }
  if (allocation_failed_ && failed_format_ == inputs.format && failed_size_.width == inputs.size.width && failed_size_.height == inputs.size.height) return false;
  allocation_failed_ = false;
  const Pass pass = PassFor(inputs.format);
  if (pass.storage == Storage::NONE || pipelines_[static_cast<size_t>(pass.storage)] == VK_NULL_HANDLE) return false;
  if (ring_) {
    ring_->retired_at = present_number;
    retiring_.push_back(std::move(ring_));
  }
  auto ring = std::make_unique<Ring>();
  ring->format = inputs.format;
  ring->hudless_format = inputs.hudless_format;
  ring->size = inputs.size;
  // No storage usage: DLSS-G's view of N may be sRGB-typed, which storage cannot have; nothing writes N as a storage image.
  constexpr VkImageUsageFlags N_USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  bool made = true;
  for (Slot& slot : ring->slots) {
    slot.n = std::make_shared<Owned>();
    // N has the back buffer's stored-value format, as NR's routes see a back buffer; DLSS-G's view has the game's HUD-less format.
    made = made && MakeImage(pass.stored, inputs.size, N_USAGE, true, pass.stored, slot.n.get());
    if (made) {
      slot.n->ngx_view = MakeView(slot.n->image, inputs.hudless_format);
      made = (slot.n->ngx_view != VK_NULL_HANDLE);
      slot.n->ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
      slot.n->ngx.Resource.ImageViewInfo = {.ImageView = slot.n->ngx_view, .Image = slot.n->image, .SubresourceRange = COLOR_RANGE,
                                            .Format = inputs.hudless_format, .Width = inputs.size.width, .Height = inputs.size.height};
      slot.n->ngx.ReadWrite = false;
    }
    const VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool_, .descriptorSetCount = 1u,
                                                  .pSetLayouts = &set_layout_};
    made = made && vkAllocateDescriptorSets(device_, &allocate, &slot.set) == VK_SUCCESS;
  }
  made = made && MakeImage(pass.stored, inputs.size, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, pass.stored, &ring->final_copy);
  made = made && MakeImage(pass.storage_format, inputs.size, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, pass.storage_format,
                           &ring->output);
  if (made && IsSrgb(inputs.hudless_format)) {
    made = MakeImage(Stored(inputs.hudless_format), inputs.size, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false,
                     Stored(inputs.hudless_format), &ring->hudless_copy);
  }
  if (!made) {
    nr::Logf(nr::LogLevel::WARN, "HUD-less composer (Vulkan): its images ({}x{}) could not be made; the HUD-less image is hidden until the size or format changes",
             inputs.size.width, inputs.size.height);
    FreeRing(*ring);
    allocation_failed_ = true;
    failed_format_ = inputs.format;
    failed_size_ = inputs.size;
    return false;
  }
  ring_ = std::move(ring);
  next_slot_ = 0u;
  return true;
}

std::optional<HudComposerVk::Begun> HudComposerVk::Begin(VkQueue queue, std::span<const VkSemaphore> present_waits, const Inputs& inputs,
                                                          uint64_t present_number) {
  if (!begin_commands_ || begun_ || inputs.back_buffer == VK_NULL_HANDLE || inputs.hudless == VK_NULL_HANDLE || inputs.opacity == VK_NULL_HANDLE) return std::nullopt;
  if (!begin_commands_->SlotsFree(1u) || !end_commands_->SlotsFree(1u)) return std::nullopt;  // never waited for
  if (!EnsureRing(inputs, present_number)) return std::nullopt;
  std::optional<uint32_t> free;
  for (uint32_t step = 0u; step < RING && !free; ++step) {
    const uint32_t index = (next_slot_ + step) % RING;
    const Slot& slot = ring_->slots[index];
    if (slot.n.use_count() == 1 && SlotDone(slot)) free = index;
  }
  if (!free) return std::nullopt;
  Slot& slot = ring_->slots[*free];
  for (VkImageView view : slot.transient) functions_.vkDestroyImageView(device_, view, nullptr);
  slot.transient.clear();
  VkCommandBuffer buffer = VK_NULL_HANDLE;
  if (begin_commands_->Begin(&buffer) != vk::CommandRing::Status::READY) return std::nullopt;
  const VkImageLayout n_from = (slot.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : N_REST_LAYOUT);
  std::vector<VkImageMemoryBarrier> before = {Barrier(inputs.hudless, inputs.hudless_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                                              Barrier(slot.n->image, n_from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
  if (ring_->hudless_copy.image) before.push_back(Barrier(ring_->hudless_copy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                  static_cast<uint32_t>(before.size()), before.data());
  const VkImageCopy copy = {.srcSubresource = COLOR_LAYERS, .dstSubresource = COLOR_LAYERS, .extent = {inputs.size.width, inputs.size.height, 1u}};
  functions_.vkCmdCopyImage(buffer, inputs.hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.n->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
  if (ring_->hudless_copy.image) {
    functions_.vkCmdCopyImage(buffer, inputs.hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ring_->hudless_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              1u, &copy);
  }
  std::vector<VkImageMemoryBarrier> after = {Barrier(inputs.hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, inputs.hudless_layout),
                                             Barrier(slot.n->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)};
  if (ring_->hudless_copy.image) after.push_back(Barrier(ring_->hudless_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, READ));
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                  static_cast<uint32_t>(after.size()), after.data());
  if (begin_commands_->Submit(queue, present_waits) != VK_SUCCESS) return std::nullopt;
  slot.fresh = false;
  slot_ = *free;
  next_slot_ = (slot_ + 1u) % RING;
  frame_ = inputs;
  begun_ = true;
  return Begun{.image = slot.n->image, .format = PassFor(inputs.format).stored, .ngx = &slot.n->ngx};
}

VkResult HudComposerVk::EndAndClose(VkQueue queue, std::span<const VkSemaphore> present_waits, bool applied) {
  if (!begun_) return VK_ERROR_INITIALIZATION_FAILED;
  begun_ = false;
  Ring& ring = *ring_;
  Slot& slot = ring.slots[slot_];
  VkCommandBuffer buffer = VK_NULL_HANDLE;
  if (end_commands_->Begin(&buffer) != vk::CommandRing::Status::READY) return VK_NOT_READY;
  if (!applied) {
    const VkImageMemoryBarrier rest = Barrier(slot.n->image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, N_REST_LAYOUT);
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &rest);
  } else {
    const Pass pass = PassFor(ring.format);
    const VkImage hudless = (ring.hudless_copy.image ? ring.hudless_copy.image : frame_.hudless);
    const VkImageLayout hudless_layout = (ring.hudless_copy.image ? READ : frame_.hudless_layout);
    // Views of the game's images, made for this use and kept until the slot's submission finished.
    const VkImageView hudless_view = (ring.hudless_copy.image ? ring.hudless_copy.view : MakeView(frame_.hudless, frame_.hudless_format));
    const VkImageView opacity_view = MakeView(frame_.opacity, frame_.opacity_format);
    if (!ring.hudless_copy.image) slot.transient.push_back(hudless_view);
    slot.transient.push_back(opacity_view);
    const std::array<VkImageMemoryBarrier, 6> before = {
        Barrier(frame_.back_buffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        Barrier(ring.final_copy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        Barrier(slot.n->image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, READ),
        Barrier(hudless, hudless_layout, READ),
        Barrier(frame_.opacity, frame_.opacity_layout, READ),
        Barrier(ring.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
    };
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u,
                                    nullptr, 0u, nullptr, static_cast<uint32_t>(before.size()), before.data());
    const VkImageCopy copy = {.srcSubresource = COLOR_LAYERS, .dstSubresource = COLOR_LAYERS, .extent = {ring.size.width, ring.size.height, 1u}};
    functions_.vkCmdCopyImage(buffer, frame_.back_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ring.final_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u,
                              &copy);
    const VkImageMemoryBarrier final_read = Barrier(ring.final_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, READ);
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &final_read);
    const std::array<VkDescriptorImageInfo, SAMPLED_BINDINGS> reads = {{{VK_NULL_HANDLE, ring.final_copy.view, READ},
                                                                        {VK_NULL_HANDLE, slot.n->view, READ},
                                                                        {VK_NULL_HANDLE, hudless_view, READ},
                                                                        {VK_NULL_HANDLE, opacity_view, READ}}};
    const VkDescriptorImageInfo storage = {VK_NULL_HANDLE, ring.output.view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, SAMPLED_BINDINGS + 1u> writes = {};
    for (uint32_t i = 0; i < SAMPLED_BINDINGS; ++i) {
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = slot.set, .dstBinding = i, .descriptorCount = 1u,
                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &reads[i]};
    }
    writes[SAMPLED_BINDINGS] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = slot.set, .dstBinding = STORAGE_BINDING, .descriptorCount = 1u,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &storage};
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0u, nullptr);
    const HudCompose constants = {.alpha_channel = AlphaChannel(frame_.opacity_format, frame_.opacity_from_ui_colour), .clamp_unit = (pass.unit ? 1u : 0u),
                                  .swap_rb = (pass.swap_rb ? 1u : 0u), .width = ring.size.width, .height = ring.size.height};
    functions_.vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[static_cast<size_t>(pass.storage)]);
    vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0u, 1u, &slot.set, 0u, nullptr);
    vkCmdPushConstants(buffer, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(constants), &constants);
    functions_.vkCmdDispatch(buffer, (ring.size.width + GROUP - 1u) / GROUP, (ring.size.height + GROUP - 1u) / GROUP, 1u);
    const std::array<VkImageMemoryBarrier, 2> to_copy = {Barrier(ring.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                                                         Barrier(frame_.back_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr,
                                    0u, nullptr, static_cast<uint32_t>(to_copy.size()), to_copy.data());
    functions_.vkCmdCopyImage(buffer, ring.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame_.back_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    const std::array<VkImageMemoryBarrier, 3> after = {Barrier(frame_.back_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                                                       Barrier(hudless, READ, hudless_layout),
                                                       Barrier(frame_.opacity, READ, frame_.opacity_layout)};
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u,
                                    nullptr, 0u, nullptr, static_cast<uint32_t>(after.size()), after.data());
    // N stays in READ, which is N_REST_LAYOUT.
  }
  const uint32_t ring_slot = end_commands_->Current();
  const VkResult submitted = end_commands_->Submit(queue, present_waits);
  if (submitted == VK_SUCCESS) slot.ring_slot = ring_slot;
  return submitted;
}

std::shared_ptr<const void> HudComposerVk::Owner() const { return (ring_ ? std::shared_ptr<const void>(ring_->slots[slot_].n) : nullptr); }

void HudComposerVk::Retire(uint64_t present_number) {
  std::erase_if(retiring_, [&](const std::unique_ptr<Ring>& ring) {
    if (present_number < ring->retired_at + RETIRE_PRESENTS) return false;
    for (const Slot& slot : ring->slots) {
      if ((slot.n && slot.n.use_count() != 1) || !SlotDone(slot)) return false;
    }
    FreeRing(*ring);
    return true;
  });
}

void HudComposerVk::Release(uint64_t present_number) {
  if (!ring_ || begun_) return;
  ring_->retired_at = present_number;
  retiring_.push_back(std::move(ring_));
}

void HudComposerVk::Destroy() {
  if (device_ == VK_NULL_HANDLE) return;
  if (ring_) FreeRing(*ring_);
  ring_.reset();
  for (auto& ring : retiring_) FreeRing(*ring);
  retiring_.clear();
  for (VkPipeline& pipeline : pipelines_) {
    if (pipeline) functions_.vkDestroyPipeline(device_, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
  }
  if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (pipeline_layout_) functions_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (set_layout_) functions_.vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  pool_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  if (begin_commands_) begin_commands_->Destroy();
  if (end_commands_) end_commands_->Destroy();
  begin_commands_.reset();
  end_commands_.reset();
  device_ = VK_NULL_HANDLE;
}

}  // namespace uplift::color
