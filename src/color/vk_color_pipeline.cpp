#include "color/vk_color_pipeline.hpp"

#include <bit>
#include <cstring>
#include <format>
#include <vector>

#include "color/color_pipeline.hpp"
#include "decode_vk_presr.h"
#include "decode_vk_r11g11b10f.h"
#include "decode_vk_rgb10a2.h"
#include "decode_vk_rgba16f.h"
#include "decode_vk_rgba32f.h"
#include "decode_vk_rgba8.h"
#include "encode_vk_r11g11b10f.h"
#include "encode_vk_rgb10a2.h"
#include "encode_vk_rgba16f.h"
#include "encode_vk_rgba32f.h"
#include "encode_vk_rgba8.h"
#include "encode_vk_sampled.h"
#include "look_pyramid_vk.h"
#include "look_shape_vk.h"
#include "look_stabilize_vk.h"
#include "meter_vk_r11g11b10f.h"
#include "meter_vk_rgb10a2.h"
#include "meter_vk_rgba16f.h"
#include "meter_vk_rgba32f.h"
#include "meter_vk_rgba8.h"
#include "meter_vk_sampled.h"
#include "motion_vk.h"
#include "nr/log.hpp"
#include "resolve_vk.h"

namespace uplift::color {
namespace {

constexpr uint32_t PASS_CONSTANTS = 16u;  // the encode's, the decode's, the motion copy's and the stabiliser's constant buffers: 16 dwords each
// Ring slots per recording (Plan 14): one per dispatch, as each dispatch reads its own constants. The encode, the decode, the motion copy, the change
// pass, the stabiliser's scene cut, levels and detail, the shape, the meter, one resolve per later pass, then one per pyramid level (look::MakeAtlas makes at
// most 16).
constexpr uint32_t ENCODE_PASS = 0u;
constexpr uint32_t DECODE_PASS = 1u;
constexpr uint32_t MOTION_PASS = 2u;
constexpr uint32_t CHANGE_PASS = 3u;
constexpr uint32_t STABILIZE_CUT_PASS = 4u;
constexpr uint32_t STABILIZE_LEVELS_PASS = 5u;
constexpr uint32_t DETAIL_PASS = 6u;
constexpr uint32_t SHAPE_PASS = 7u;
constexpr uint32_t METER_PASS = 8u;
constexpr uint32_t RESOLVE_PASS = 9u;                                              // pass index 1 (the second NR pass); index n at RESOLVE_PASS + n - 1
constexpr uint32_t PYRAMID_PASS = RESOLVE_PASS + VkColorPipeline::RESOLVE_PASSES;  // level 1; level k at PYRAMID_PASS + k - 1
constexpr uint32_t PYRAMID_LEVELS = 24u;
constexpr uint32_t PASSES_PER_RECORDING = PYRAMID_PASS + PYRAMID_LEVELS;
constexpr VkDeviceSize CONSTANTS_STRIDE = 256u;  // at least minUniformBufferOffsetAlignment, whose maximum the spec fixes at 256
constexpr VkDeviceSize RING_BYTES = VkDeviceSize{VkColorPipeline::RING_SLOTS} * PASSES_PER_RECORDING * CONSTANTS_STRIDE;
// The set's bindings (the SPIR-V is built with -fvk-u-shift 16 and -fvk-b-shift 32).
constexpr uint32_t SAMPLED_BINDINGS = 5u;  // t0-t4 at 0-4
constexpr uint32_t STORAGE_BINDINGS = 3u;  // u0-u2 at 16-18
constexpr uint32_t STORAGE_BASE = 16u;
constexpr uint32_t CONSTANTS_BINDING = 32u;
constexpr uint32_t GROUP = 8u;  // numthreads(8, 8, 1)
// look_stabilize_cs.hlsl's modes and flags (ColorPipeline's).
constexpr uint32_t STABILIZE_CUT = 0u;
constexpr uint32_t STABILIZE_LEVELS = 1u;
constexpr uint32_t STABILIZE_DETAIL = 2u;
constexpr uint32_t STABILIZE_FLAG_RESET = 1u;
constexpr uint32_t STABILIZE_FLAG_MOTION = 2u;
constexpr uint32_t STABILIZE_FLAG_VECTORS = 4u;

// Pipeline slots: the sampled encode, the in-place encodes (one per output variant), the private-colour decode, the in-place decodes, the motion copy, and
// (Plan 14) the look's pyramid, stabiliser and shape, the resolve, the sampled meter and the in-place meters (one per output variant).
constexpr uint32_t ENCODE_SAMPLED = 0u;
constexpr uint32_t ENCODE_STORAGE = 1u;
constexpr uint32_t DECODE_PRESR = ENCODE_STORAGE + VkColorPipeline::OUTPUT_VARIANTS;
constexpr uint32_t DECODE_STORAGE = DECODE_PRESR + 1u;
constexpr uint32_t MOTION_COPY = DECODE_STORAGE + VkColorPipeline::OUTPUT_VARIANTS;
constexpr uint32_t LOOK_PYRAMID = MOTION_COPY + 1u;
constexpr uint32_t LOOK_STABILIZE = LOOK_PYRAMID + 1u;
constexpr uint32_t LOOK_SHAPE = LOOK_STABILIZE + 1u;
constexpr uint32_t RESOLVE = LOOK_SHAPE + 1u;
constexpr uint32_t METER_SAMPLED = RESOLVE + 1u;
constexpr uint32_t METER_STORAGE = METER_SAMPLED + 1u;

struct Shader {
  const unsigned char* bytes = nullptr;
  size_t size = 0u;
};

// The five output formats, in the order of VkColorPipeline's variants (and of Task 4's headers).
struct OutputShaders {
  VkFormat format;
  Shader encode;
  Shader decode;
  Shader meter;
};
#define UPLIFT_VK_SHADER(name) \
  Shader { name, sizeof(name) }
constexpr OutputShaders OUTPUT_SHADERS[VkColorPipeline::OUTPUT_VARIANTS] = {
    {VK_FORMAT_R16G16B16A16_SFLOAT, UPLIFT_VK_SHADER(g_encode_vk_rgba16f), UPLIFT_VK_SHADER(g_decode_vk_rgba16f), UPLIFT_VK_SHADER(g_meter_vk_rgba16f)},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, UPLIFT_VK_SHADER(g_encode_vk_r11g11b10f), UPLIFT_VK_SHADER(g_decode_vk_r11g11b10f),
     UPLIFT_VK_SHADER(g_meter_vk_r11g11b10f)},
    {VK_FORMAT_R8G8B8A8_UNORM, UPLIFT_VK_SHADER(g_encode_vk_rgba8), UPLIFT_VK_SHADER(g_decode_vk_rgba8), UPLIFT_VK_SHADER(g_meter_vk_rgba8)},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, UPLIFT_VK_SHADER(g_encode_vk_rgb10a2), UPLIFT_VK_SHADER(g_decode_vk_rgb10a2), UPLIFT_VK_SHADER(g_meter_vk_rgb10a2)},
    {VK_FORMAT_R32G32B32A32_SFLOAT, UPLIFT_VK_SHADER(g_encode_vk_rgba32f), UPLIFT_VK_SHADER(g_decode_vk_rgba32f), UPLIFT_VK_SHADER(g_meter_vk_rgba32f)},
};
constexpr Shader ENCODE_SAMPLED_SHADER = UPLIFT_VK_SHADER(g_encode_vk_sampled);
constexpr Shader DECODE_PRESR_SHADER = UPLIFT_VK_SHADER(g_decode_vk_presr);
constexpr Shader MOTION_SHADER = UPLIFT_VK_SHADER(g_motion_vk);
constexpr Shader PYRAMID_SHADER = UPLIFT_VK_SHADER(g_look_pyramid_vk);
constexpr Shader STABILIZE_SHADER = UPLIFT_VK_SHADER(g_look_stabilize_vk);
constexpr Shader SHAPE_SHADER = UPLIFT_VK_SHADER(g_look_shape_vk);
constexpr Shader RESOLVE_SHADER = UPLIFT_VK_SHADER(g_resolve_vk);
constexpr Shader METER_SAMPLED_SHADER = UPLIFT_VK_SHADER(g_meter_vk_sampled);
#undef UPLIFT_VK_SHADER

uint32_t Bits(float value) {
  return std::bit_cast<uint32_t>(value);
}

// shaders/encode_cs.hlsl's constants, as ColorPipeline's EncodeConstants packs them: `change_mode` selects its change-field pass, `second_output` its u1
// write (the zero motion, or the change mode's κ_M).
std::array<uint32_t, PASS_CONSTANTS> EncodeConstants(const VkEncodePass& pass, uint32_t change_mode, bool second_output) {
  const bool whole = (pass.region.width == 0u || pass.region.height == 0u);
  const bool unpadded = pass.canvas.Empty();
  return {
      static_cast<uint32_t>(pass.encoding),
      pass.width,
      pass.height,
      Bits(pass.input_scale),
      (second_output ? 1u : 0u),
      (pass.exposure.view != VK_NULL_HANDLE ? 1u : 0u),
      Bits(pass.exposure_factor),
      (whole ? 0u : pass.region.x),
      (whole ? 0u : pass.region.y),
      (whole ? pass.width : pass.region.width),
      (whole ? pass.height : pass.region.height),
      (unpadded ? pass.width : pass.canvas.width),
      (unpadded ? pass.height : pass.canvas.height),
      change_mode,
      pass.options,
      0u,
  };
}

// look_stabilize_cs.hlsl's constants for one mode (ColorPipeline's StabilizeConstants).
std::array<uint32_t, PASS_CONSTANTS> StabilizeConstants(uint32_t mode, const VkStabilizePass& pass) {
  const VkStabilizeMotion& motion = pass.motion;
  const bool vectors = (motion.vectors.view != VK_NULL_HANDLE);
  const uint32_t flags = (pass.reset ? STABILIZE_FLAG_RESET : 0u) | (pass.motion_mode ? STABILIZE_FLAG_MOTION : 0u) | (vectors ? STABILIZE_FLAG_VECTORS : 0u);
  return {
      pass.atlas.image.width,
      pass.atlas.image.height,
      pass.atlas.levels,
      mode,
      pass.bands.stable_level,
      flags,
      Bits(pass.rate),
      Bits(pass.bands.low_level),
      Bits(motion.scale_x),
      Bits(motion.scale_y),
      motion.rect.x,
      motion.rect.y,
      (vectors ? motion.rect.width : 1u),
      (vectors ? motion.rect.height : 1u),
      0u,
      0u,
  };
}

// An Uplift image read as a sampled image (it rests in GENERAL).
VkSampledView Own(VkImageView view) {
  return {.view = view, .layout = VK_IMAGE_LAYOUT_GENERAL};
}

}  // namespace

VkColorPipeline::~VkColorPipeline() {
  Destroy();
}

std::optional<uint32_t> VkColorPipeline::OutputVariantOf(VkFormat format) {
  for (uint32_t variant = 0u; variant < OUTPUT_VARIANTS; ++variant) {
    if (OUTPUT_SHADERS[variant].format == format) return variant;
  }
  return std::nullopt;
}

bool VkColorPipeline::Initialize(const vk::NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
                                 std::string* error) {
  functions_ = functions;
  device_ = device;
  memory_ = memory;
  const auto fail = [&](const char* what, VkResult result) {
    *error = std::format("could not create the Vulkan colour pipeline's {}", what);
    nr::Logf(nr::LogLevel::ERR, "{} failed: VkResult {}", what, static_cast<int>(result));
    Destroy();
    return false;
  };

  // One push-descriptor set, as NGX itself requires VK_KHR_push_descriptor: sampled t0-t4, storage u0-u2, the constants' uniform buffer.
  std::array<VkDescriptorSetLayoutBinding, SAMPLED_BINDINGS + STORAGE_BINDINGS + 1u> bindings = {};
  for (uint32_t index = 0u; index < SAMPLED_BINDINGS; ++index) {
    bindings[index] = {.binding = index, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  }
  for (uint32_t index = 0u; index < STORAGE_BINDINGS; ++index) {
    bindings[SAMPLED_BINDINGS + index] = {.binding = STORAGE_BASE + index, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  }
  bindings.back() = {.binding = CONSTANTS_BINDING, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
  const VkDescriptorSetLayoutCreateInfo set_layout = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  if (const VkResult result = functions_.vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_); result != VK_SUCCESS) {
    return fail("descriptor set layout", result);
  }
  const VkPipelineLayoutCreateInfo pipeline_layout = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1u,
      .pSetLayouts = &set_layout_,
  };
  if (const VkResult result = functions_.vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_); result != VK_SUCCESS) {
    return fail("pipeline layout", result);
  }

  // The constants ring: host-visible and coherent, mapped for good.
  const VkBufferCreateInfo ring = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = RING_BYTES,
      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (const VkResult result = functions_.vkCreateBuffer(device_, &ring, nullptr, &ring_buffer_); result != VK_SUCCESS) {
    return fail("constants buffer", result);
  }
  VkMemoryRequirements requirements = {};
  functions_.vkGetBufferMemoryRequirements(device_, ring_buffer_, &requirements);
  const uint32_t type = vk::FindMemoryType(memory_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type == UINT32_MAX) return fail("constants memory type", VK_ERROR_FEATURE_NOT_PRESENT);
  const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
  void* mapped = nullptr;
  if (const VkResult result = functions_.vkAllocateMemory(device_, &allocate, nullptr, &ring_memory_); result != VK_SUCCESS) {
    return fail("constants memory", result);
  }
  if (const VkResult result = functions_.vkBindBufferMemory(device_, ring_buffer_, ring_memory_, 0u); result != VK_SUCCESS) {
    return fail("constants binding", result);
  }
  if (const VkResult result = functions_.vkMapMemory(device_, ring_memory_, 0u, VK_WHOLE_SIZE, 0u, &mapped); result != VK_SUCCESS) {
    return fail("constants mapping", result);
  }
  ring_ = static_cast<std::byte*>(mapped);

  if (!vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, 1u, 1u,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, false, &placeholder_color_)
      || !vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, 1u, 1u, VK_IMAGE_USAGE_STORAGE_BIT, false, &placeholder_motion_)) {
    return fail("placeholder images", VK_ERROR_OUT_OF_DEVICE_MEMORY);
  }
  return true;
}

void VkColorPipeline::Destroy() {
  if (device_ == VK_NULL_HANDLE) return;
  for (const VkPipeline pipeline : pipelines_) {
    if (pipeline != VK_NULL_HANDLE) {
      functions_.vkDestroyPipeline(device_, pipeline, nullptr);
    }
  }
  if (pipeline_layout_ != VK_NULL_HANDLE) {
    functions_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  }
  if (set_layout_ != VK_NULL_HANDLE) {
    functions_.vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  }
  if (ring_ != nullptr) {
    functions_.vkUnmapMemory(device_, ring_memory_);
  }
  if (ring_buffer_ != VK_NULL_HANDLE) {
    functions_.vkDestroyBuffer(device_, ring_buffer_, nullptr);
  }
  if (ring_memory_ != VK_NULL_HANDLE) {
    functions_.vkFreeMemory(device_, ring_memory_, nullptr);
  }
  vk::DestroyNrImage(functions_, device_, &placeholder_color_);
  vk::DestroyNrImage(functions_, device_, &placeholder_motion_);
  pipelines_ = {};
  failed_ = {};
  set_layout_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  ring_buffer_ = VK_NULL_HANDLE;
  ring_memory_ = VK_NULL_HANDLE;
  ring_ = nullptr;
  device_ = VK_NULL_HANDLE;
}

void VkColorPipeline::PrepareRecording(VkCommandBuffer buffer) {
  VkImageMemoryBarrier barriers[2] = {};
  const vk::NrImage* const placeholders[2] = {&placeholder_color_, &placeholder_motion_};
  for (size_t index = 0u; index < 2u; ++index) {
    barriers[index] = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = placeholders[index]->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
    };
  }
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u,
                                  barriers);
}

bool VkColorPipeline::PrepareLook() {
  const bool pyramid = (PipelineFor(LOOK_PYRAMID) != VK_NULL_HANDLE);
  const bool stabilize = (PipelineFor(LOOK_STABILIZE) != VK_NULL_HANDLE);
  const bool shape = (PipelineFor(LOOK_SHAPE) != VK_NULL_HANDLE);
  return pyramid && stabilize && shape && PipelineFor(RESOLVE) != VK_NULL_HANDLE;
}

bool VkColorPipeline::Prepare(VkFormat output_format) {
  const std::optional<uint32_t> variant = OutputVariantOf(output_format);
  if (!variant || !Initialized()) return false;
  const bool encode = (PipelineFor(ENCODE_STORAGE + *variant) != VK_NULL_HANDLE);
  const bool decode = (PipelineFor(DECODE_STORAGE + *variant) != VK_NULL_HANDLE);
  const bool meter = (PipelineFor(METER_STORAGE + *variant) != VK_NULL_HANDLE);
  return encode && decode && meter && PrepareLook();
}

bool VkColorPipeline::Prepared(VkFormat output_format) const {
  const std::optional<uint32_t> variant = OutputVariantOf(output_format);
  const bool look = (pipelines_[LOOK_PYRAMID] != VK_NULL_HANDLE && pipelines_[LOOK_STABILIZE] != VK_NULL_HANDLE && pipelines_[LOOK_SHAPE] != VK_NULL_HANDLE
                     && pipelines_[RESOLVE] != VK_NULL_HANDLE);
  return variant && look && pipelines_[ENCODE_STORAGE + *variant] != VK_NULL_HANDLE && pipelines_[DECODE_STORAGE + *variant] != VK_NULL_HANDLE
         && pipelines_[METER_STORAGE + *variant] != VK_NULL_HANDLE;
}

bool VkColorPipeline::PreparePreSr() {
  if (!Initialized()) return false;
  const bool encode = (PipelineFor(ENCODE_SAMPLED) != VK_NULL_HANDLE);
  const bool decode = (PipelineFor(DECODE_PRESR) != VK_NULL_HANDLE);
  const bool meter = (PipelineFor(METER_SAMPLED) != VK_NULL_HANDLE);
  const bool look = PrepareLook();
  return encode && decode && meter && look && PrepareMotion();
}

bool VkColorPipeline::PrepareMotion() {
  return Initialized() && PipelineFor(MOTION_COPY) != VK_NULL_HANDLE;
}

bool VkColorPipeline::SlotInRange(uint32_t slot, const char* pass) const {
  if (slot < RING_SLOTS) return true;
  nr::Logf(nr::LogLevel::ERR, "{}: slot {} is out of range (RING_SLOTS = {})", pass, slot, RING_SLOTS);
  return false;
}

VkPipeline VkColorPipeline::PipelineFor(uint32_t index) {
  if (pipelines_[index] != VK_NULL_HANDLE || failed_[index]) return pipelines_[index];
  Shader shader = MOTION_SHADER;
  if (index == ENCODE_SAMPLED) {
    shader = ENCODE_SAMPLED_SHADER;
  } else if (index < DECODE_PRESR) {
    shader = OUTPUT_SHADERS[index - ENCODE_STORAGE].encode;
  } else if (index == DECODE_PRESR) {
    shader = DECODE_PRESR_SHADER;
  } else if (index < MOTION_COPY) {
    shader = OUTPUT_SHADERS[index - DECODE_STORAGE].decode;
  } else if (index == LOOK_PYRAMID) {
    shader = PYRAMID_SHADER;
  } else if (index == LOOK_STABILIZE) {
    shader = STABILIZE_SHADER;
  } else if (index == LOOK_SHAPE) {
    shader = SHAPE_SHADER;
  } else if (index == RESOLVE) {
    shader = RESOLVE_SHADER;
  } else if (index == METER_SAMPLED) {
    shader = METER_SAMPLED_SHADER;
  } else if (index >= METER_STORAGE) {
    shader = OUTPUT_SHADERS[index - METER_STORAGE].meter;
  }
  failed_[index] = true;  // until it works: a variant that cannot be built is logged once, not every frame
  // The header's array is bytes; vkCreateShaderModule wants 4-byte-aligned words.
  std::vector<uint32_t> code(shader.size / sizeof(uint32_t));
  std::memcpy(code.data(), shader.bytes, code.size() * sizeof(uint32_t));
  const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = code.size() * sizeof(uint32_t),
      .pCode = code.data(),
  };
  VkShaderModule module = VK_NULL_HANDLE;
  if (const VkResult result = functions_.vkCreateShaderModule(device_, &module_info, nullptr, &module); result != VK_SUCCESS) {
    nr::Logf(nr::LogLevel::ERR, "vkCreateShaderModule (variant {}) failed: VkResult {}", index, static_cast<int>(result));
    return VK_NULL_HANDLE;
  }
  const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
      .layout = pipeline_layout_,
  };
  const VkResult result = functions_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1u, &pipeline_info, nullptr, &pipelines_[index]);
  functions_.vkDestroyShaderModule(device_, module, nullptr);
  if (result != VK_SUCCESS) {
    pipelines_[index] = VK_NULL_HANDLE;
    nr::Logf(nr::LogLevel::ERR, "vkCreateComputePipelines (variant {}) failed: VkResult {}", index, static_cast<int>(result));
    return VK_NULL_HANDLE;
  }
  failed_[index] = false;
  return pipelines_[index];
}

VkPipeline VkColorPipeline::EncodePipelineFor(const VkEncodePass& pass) {
  if (pass.source_format == VK_FORMAT_UNDEFINED) return PipelineFor(ENCODE_SAMPLED);
  const std::optional<uint32_t> variant = OutputVariantOf(pass.source_format);
  return (variant ? PipelineFor(ENCODE_STORAGE + *variant) : VK_NULL_HANDLE);
}

void VkColorPipeline::Dispatch(VkCommandBuffer buffer, VkPipeline pipeline, uint32_t slot, uint32_t pass, std::span<const uint32_t> constants,
                               const Bindings& bindings, uint32_t width, uint32_t height) {
  const VkDeviceSize offset = (VkDeviceSize{slot} * PASSES_PER_RECORDING + pass) * CONSTANTS_STRIDE;
  std::memcpy(ring_ + offset, constants.data(), constants.size_bytes());
  const VkDescriptorBufferInfo constants_info = {.buffer = ring_buffer_, .offset = offset, .range = constants.size_bytes()};
  std::array<VkDescriptorImageInfo, SAMPLED_BINDINGS + STORAGE_BINDINGS> images = {};
  std::array<VkWriteDescriptorSet, SAMPLED_BINDINGS + STORAGE_BINDINGS + 1u> writes = {};
  for (uint32_t index = 0u; index < SAMPLED_BINDINGS; ++index) {
    const VkSampledView& sampled = bindings.sampled[index];
    const bool empty = (sampled.view == VK_NULL_HANDLE);
    images[index] = {.imageView = (empty ? placeholder_color_.view : sampled.view), .imageLayout = (empty ? VK_IMAGE_LAYOUT_GENERAL : sampled.layout)};
    writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = index, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &images[index]};
  }
  for (uint32_t index = 0u; index < STORAGE_BINDINGS; ++index) {
    const VkImageView given = bindings.storage[index];
    const VkImageView empty = (index == 1u ? placeholder_motion_.view : placeholder_color_.view);  // u1 is the RG16F motion image
    images[SAMPLED_BINDINGS + index] = {.imageView = (given != VK_NULL_HANDLE ? given : empty), .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    writes[SAMPLED_BINDINGS + index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = STORAGE_BASE + index, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &images[SAMPLED_BINDINGS + index]};
  }
  writes.back() = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = CONSTANTS_BINDING, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &constants_info};
  functions_.vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  functions_.vkCmdPushDescriptorSetKHR(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0u, static_cast<uint32_t>(writes.size()), writes.data());
  functions_.vkCmdDispatch(buffer, (width + GROUP - 1u) / GROUP, (height + GROUP - 1u) / GROUP, 1u);
}

void VkColorPipeline::ComputeBarrier(VkCommandBuffer buffer) const {
  const VkMemoryBarrier written = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
  };
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &written, 0u, nullptr, 0u,
                                  nullptr);
}

bool VkColorPipeline::RecordEncode(VkCommandBuffer buffer, uint32_t slot, const VkEncodePass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordEncode")) return false;  // batch 2 review, minor 5: never after a failed Initialize
  const VkPipeline pipeline = EncodePipelineFor(pass);
  if (pipeline == VK_NULL_HANDLE) return false;
  const bool in_place = (pass.source_format != VK_FORMAT_UNDEFINED);
  Bindings bindings;
  bindings.sampled[0] = (in_place ? VkSampledView{} : VkSampledView{.view = pass.source, .layout = pass.source_layout});
  bindings.sampled[2] = pass.exposure;
  bindings.storage = {pass.model, pass.motion, (in_place ? pass.source : VK_NULL_HANDLE)};
  const bool unpadded = pass.canvas.Empty();
  Dispatch(buffer, pipeline, slot, ENCODE_PASS, EncodeConstants(pass, 0u, pass.motion != VK_NULL_HANDLE), bindings,
           (unpadded ? pass.width : pass.canvas.width), (unpadded ? pass.height : pass.canvas.height));
  return true;
}

bool VkColorPipeline::RecordChange(VkCommandBuffer buffer, uint32_t slot, const VkChangePass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordChange")) return false;
  const VkEncodePass& encode = pass.encode;
  const VkPipeline pipeline = EncodePipelineFor(encode);
  if (pipeline == VK_NULL_HANDLE) return false;
  const bool in_place = (encode.source_format != VK_FORMAT_UNDEFINED);
  Bindings bindings;
  bindings.sampled[0] = (in_place ? VkSampledView{} : VkSampledView{.view = encode.source, .layout = encode.source_layout});
  bindings.sampled[1] = Own(pass.nr_output);
  bindings.sampled[2] = encode.exposure;
  bindings.storage = {pass.change, pass.basis, (in_place ? encode.source : VK_NULL_HANDLE)};
  // The change mode runs at the work image, not the canvas.
  Dispatch(buffer, pipeline, slot, CHANGE_PASS, EncodeConstants(encode, 1u, pass.basis != VK_NULL_HANDLE), bindings, encode.width, encode.height);
  return true;
}

bool VkColorPipeline::RecordDecode(VkCommandBuffer buffer, uint32_t slot, const VkDecodePass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordDecode")) return false;  // batch 2 review, minor 5: never after a failed Initialize
  const bool in_place = (pass.source.view == VK_NULL_HANDLE);
  uint32_t index = DECODE_PRESR;
  if (in_place) {
    const std::optional<uint32_t> variant = OutputVariantOf(pass.target_format);
    if (!variant) return false;
    index = DECODE_STORAGE + *variant;
  }
  const VkPipeline pipeline = PipelineFor(index);
  if (pipeline == VK_NULL_HANDLE) return false;
  // shaders/decode_common.hlsli's constants, as ColorPipeline's RecordComputeDecode packs them.
  const std::array<uint32_t, PASS_CONSTANTS> constants = {
      static_cast<uint32_t>(pass.encoding),
      0u,
      Bits(pass.input_scale),
      Bits(pass.transfer_strength),
      Bits(pass.color_strength),
      (pass.exposure.view != VK_NULL_HANDLE ? 1u : 0u),
      Bits(pass.exposure_factor),
      pass.origin_x,
      pass.origin_y,
      pass.width,
      pass.height,
      (pass.upsampling ? static_cast<uint32_t>(*pass.upsampling) + 1u : 0u),
      pass.source_x,
      pass.source_y,
      pass.options | (pass.mask.view != VK_NULL_HANDLE ? shader_options::MASK : 0u),
      Bits(pass.chroma_clamp),
  };
  Bindings bindings;
  bindings.sampled = {pass.source, Own(pass.nr_output), pass.exposure, pass.mask, VkSampledView{}};
  bindings.storage[0] = pass.target;
  Dispatch(buffer, pipeline, slot, DECODE_PASS, constants, bindings, pass.width, pass.height);
  return true;
}

bool VkColorPipeline::RecordMotion(VkCommandBuffer buffer, uint32_t slot, const VkMotionPass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordMotion")) return false;  // batch 2 review, minor 5: never after a failed Initialize
  const VkPipeline pipeline = PipelineFor(MOTION_COPY);
  if (pipeline == VK_NULL_HANDLE) return false;
  const std::array<uint32_t, PASS_CONSTANTS> constants = {
      pass.region.x,
      pass.region.y,
      pass.region.width,
      pass.region.height,
      pass.image.width,
      pass.image.height,
      pass.canvas.width,
      pass.canvas.height,
      Bits(pass.scale_x),
      Bits(pass.scale_y),
      (pass.flip_y ? 1u : 0u),
      0u,
      0u,
      0u,
      0u,
      0u,
  };
  Bindings bindings;
  bindings.sampled[1] = pass.source;
  bindings.storage[1] = pass.target;
  Dispatch(buffer, pipeline, slot, MOTION_PASS, constants, bindings, pass.canvas.width, pass.canvas.height);
  return true;
}

bool VkColorPipeline::RecordPyramid(VkCommandBuffer buffer, uint32_t slot, const VkPyramidPass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordPyramid")) return false;
  const VkPipeline pipeline = PipelineFor(LOOK_PYRAMID);
  if (pipeline == VK_NULL_HANDLE || pass.atlas.levels > PYRAMID_LEVELS) return false;
  Bindings bindings;
  bindings.sampled[0] = Own(pass.change);
  bindings.storage = {pass.gauss, pass.peaks, VK_NULL_HANDLE};
  for (uint32_t level = 1u; level <= pass.atlas.levels; ++level) {
    if (level > 1u) {
      ComputeBarrier(buffer);  // level reads level - 1
    }
    const nr::Size size = look::LevelSize(pass.atlas.image, level);
    const std::array<uint32_t, 4> constants = {pass.atlas.image.width, pass.atlas.image.height, level, 0u};
    Dispatch(buffer, pipeline, slot, PYRAMID_PASS + level - 1u, constants, bindings, size.width, size.height);
  }
  return true;
}

bool VkColorPipeline::RecordStabilize(VkCommandBuffer buffer, uint32_t slot, const VkStabilizePass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordStabilize")) return false;
  const VkPipeline pipeline = PipelineFor(LOOK_STABILIZE);
  if (pipeline == VK_NULL_HANDLE) return false;
  Bindings bindings;
  bindings.sampled = {Own(pass.gauss), Own(pass.previous), pass.motion.vectors, VkSampledView{}, VkSampledView{}};
  bindings.storage = {pass.current, pass.state, VK_NULL_HANDLE};
  Dispatch(buffer, pipeline, slot, STABILIZE_CUT_PASS, StabilizeConstants(STABILIZE_CUT, pass), bindings, 1u, 1u);
  ComputeBarrier(buffer);  // the levels read the scene cut
  const nr::Size size = look::LevelSize(pass.atlas.image, pass.bands.stable_level);
  Dispatch(buffer, pipeline, slot, STABILIZE_LEVELS_PASS, StabilizeConstants(STABILIZE_LEVELS, pass), bindings, size.width, size.height);
  return true;
}

bool VkColorPipeline::RecordDetail(VkCommandBuffer buffer, uint32_t slot, const VkStabilizePass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordDetail")) return false;
  const VkPipeline pipeline = PipelineFor(LOOK_STABILIZE);
  if (pipeline == VK_NULL_HANDLE) return false;
  Bindings bindings;
  bindings.sampled = {Own(pass.gauss), Own(pass.previous), pass.motion.vectors, Own(pass.change), VkSampledView{}};
  bindings.storage = {pass.current, pass.state, VK_NULL_HANDLE};
  Dispatch(buffer, pipeline, slot, DETAIL_PASS, StabilizeConstants(STABILIZE_DETAIL, pass), bindings, pass.atlas.image.width, pass.atlas.image.height);
  return true;
}

bool VkColorPipeline::RecordShape(VkCommandBuffer buffer, uint32_t slot, const VkShapePass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordShape")) return false;
  const VkPipeline pipeline = PipelineFor(LOOK_SHAPE);
  if (pipeline == VK_NULL_HANDLE) return false;
  Bindings bindings;
  bindings.sampled = {Own(pass.gauss), Own(pass.peaks), Own(pass.history), Own(pass.basis), Own(pass.detail)};
  bindings.storage[0] = pass.change;
  const look::ShapeSettings& shape = pass.settings;
  const uint32_t flags = (pass.sdr ? 1u : 0u) | (pass.history != VK_NULL_HANDLE ? 2u : 0u) | (pass.detail != VK_NULL_HANDLE ? 4u : 0u);
  const std::array<uint32_t, 20> constants = {
      pass.atlas.image.width,
      pass.atlas.image.height,
      pass.atlas.levels,
      flags,
      Bits(pass.bands.low_level),
      pass.bands.peak_level,
      Bits(shape.tone),
      Bits(shape.detail),
      Bits(shape.halo),
      Bits(shape.brighten),
      Bits(shape.darken),
      Bits(shape.color),
      Bits(shape.hue),
      Bits(shape.shadows),
      Bits(shape.midtones),
      Bits(shape.highlights),
      Bits(shape.edit_strength),
      Bits(shape.max_brighten),
      Bits(shape.max_darken),
      Bits(shape.max_color),
  };
  Dispatch(buffer, pipeline, slot, SHAPE_PASS, constants, bindings, pass.atlas.image.width, pass.atlas.image.height);
  return true;
}

bool VkColorPipeline::RecordMeter(VkCommandBuffer buffer, uint32_t slot, const VkMeterPass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordMeter")) return false;
  const bool in_place = (pass.source_format != VK_FORMAT_UNDEFINED);
  uint32_t index = METER_SAMPLED;
  if (in_place) {
    const std::optional<uint32_t> variant = OutputVariantOf(pass.source_format);
    if (!variant) return false;
    index = METER_STORAGE + *variant;
  }
  const VkPipeline pipeline = PipelineFor(index);
  if (pipeline == VK_NULL_HANDLE) return false;
  // shaders/meter_cs.hlsl's constants, as ColorPipeline's RecordMeter packs them.
  const std::array<uint32_t, PASS_CONSTANTS> constants = {
      static_cast<uint32_t>(pass.encoding),
      pass.primaries,
      Bits(pass.input_scale),
      pass.region.x,
      pass.region.y,
      pass.region.width,
      pass.region.height,
      (pass.snap ? 1u : 0u),
      (pass.smooth ? 1u : 0u),
      Bits(pass.brighter_rate),
      Bits(pass.darker_rate),
      Bits(pass.frame_seconds),
      (pass.probe ? 1u : 0u),
      (pass.probe && pass.game_exposure.view != VK_NULL_HANDLE ? 1u : 0u),
      Bits(pass.game_exposure_factor),
      0u,
  };
  Bindings bindings;
  bindings.sampled[0] = (in_place ? VkSampledView{} : VkSampledView{.view = pass.source, .layout = pass.source_layout});
  if (pass.probe) {
    bindings.sampled[2] = pass.game_exposure;  // Plan 17: t2, as the encode binds it; a null view takes the placeholder, which the shader never reads
  }
  bindings.storage = {pass.state, VK_NULL_HANDLE, (in_place ? pass.source : VK_NULL_HANDLE)};
  Dispatch(buffer, pipeline, slot, METER_PASS, constants, bindings, 1u, 1u);  // one group of 256 threads
  return true;
}

bool VkColorPipeline::RecordResolve(VkCommandBuffer buffer, uint32_t slot, uint32_t index, const VkResolvePass& pass) {
  if (!Initialized() || !SlotInRange(slot, "RecordResolve") || index == 0u || index > RESOLVE_PASSES) return false;
  const VkPipeline pipeline = PipelineFor(RESOLVE);
  if (pipeline == VK_NULL_HANDLE) return false;
  const std::array<uint32_t, 4> constants = {pass.size.width, pass.size.height, Bits(pass.transfer_strength), Bits(pass.color_strength)};
  Bindings bindings;
  bindings.sampled[0] = Own(pass.given);
  bindings.storage[0] = pass.returned;
  Dispatch(buffer, pipeline, slot, RESOLVE_PASS + index - 1u, constants, bindings, pass.size.width, pass.size.height);
  return true;
}

}  // namespace uplift::color
