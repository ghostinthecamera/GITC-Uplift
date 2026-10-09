#include "sources/vk_nr_pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include "color/color_pipeline.hpp"
#include "nr/log.hpp"
#include "nr/vk_handles.hpp"

namespace uplift::sources {
namespace {

static_assert(color::VkColorPipeline::RESOLVE_PASSES == LATER_PASSES, "the colour pipeline has one resolve per later pass");

constexpr uint64_t MODEL_BYTES_PER_PIXEL = 8u;   // RGBA16F
constexpr uint64_t MOTION_BYTES_PER_PIXEL = 4u;  // RG16F
constexpr uint64_t STATE_BYTES = 16u;            // a 1x1 RGBA32F state image (the stabiliser's scene cut)
// Plan 17: the meter's state is 2x1 (the second texel is Auto's check), one 256 B slot of the readback ring each.
constexpr uint32_t EXPOSURE_STATE_WIDTH = 2u;
constexpr VkDeviceSize CHECK_STRIDE = 256u;
// A and B are NR's ping-pong images (colour in, output out) and the identity smokes' copies read and write them: storage, sampled, transfer. Before upscaling's
// private colour is the same (its smoke reads it back), and so is the change field (the look smoke reads it).
constexpr VkImageUsageFlags MODEL_USAGE =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags MOTION_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
// Plan 14: the look's images are written as storage images and read as sampled ones; the scene-cut state only as a storage image.
constexpr VkImageUsageFlags LOOK_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
// Plan 14: the meter's state is written as a storage image and read by the encode and the decode as a sampled one. Plan 17: and copied out for Auto's check.
constexpr VkImageUsageFlags EXPOSURE_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
// Plan 14: the mask copy is a copy's destination (ReShade's command list) and the decode samples it.
constexpr VkImageUsageFlags MASK_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// A global dependency: whatever was written is visible to whatever reads or writes next.
constexpr VkMemoryBarrier GLOBAL_DEPENDENCY = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
};
// The game's motion vectors in a canvas are sampled as floats (D3D12's MotionViewFormat: unconverted ones cannot be copied there, E9).
constexpr std::array<VkFormat, 4> READABLE_MOTION_FORMATS = {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                             VK_FORMAT_R32G32B32A32_SFLOAT};
// The Present motion copies (Plan 14): RG16F, written by the motion copy, handed over sampled, and read by the bridge's copy-in.
constexpr VkImageUsageFlags PRESENT_MOTION_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
// Plan 19: native Present's staging image is only copied and blitted; SAMPLED is there because an image view (CreateNrImage makes one) needs a view usage.
constexpr VkImageUsageFlags PRESENT_STAGING_USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageSubresourceRange COLOR_RANGE = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
constexpr VkImageSubresourceLayers COLOR_LAYERS = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};

// Plan 19: the bytes per pixel of a swap-chain copy format (vk::VkFormatOf(vk::SharedFormatOf(...))'s display formats); 0 for any other.
uint64_t PresentBytesPerPixel(VkFormat format) {
  switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4u;
    case VK_FORMAT_R16G16B16A16_SFLOAT:      return 8u;
    case VK_FORMAT_R32G32B32A32_SFLOAT:      return 16u;
    default:                                 return 0u;
  }
}

// Plan 19: one layout transition of a whole single-mip, single-layer colour image.
VkImageMemoryBarrier ImageTransition(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags source_access,
                                     VkAccessFlags destination_access) {
  return {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = source_access,
      .dstAccessMask = destination_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = COLOR_RANGE,
  };
}

// Plan 14: the formats the mask copy takes (color::DescribeMaskFormat's view formats, as Vulkan names them: the decode samples them raw), and their bytes.
struct MaskFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint64_t bytes_per_pixel = 0u;
};
constexpr std::array<MaskFormat, 10> MASK_FORMATS = {{
    {VK_FORMAT_R8_UNORM, 1u},
    {VK_FORMAT_R16_SFLOAT, 2u},
    {VK_FORMAT_R16_UNORM, 2u},
    {VK_FORMAT_R32_SFLOAT, 4u},
    {VK_FORMAT_R8G8_UNORM, 2u},
    {VK_FORMAT_R16G16_SFLOAT, 4u},
    {VK_FORMAT_R8G8B8A8_UNORM, 4u},
    {VK_FORMAT_B8G8R8A8_UNORM, 4u},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4u},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 8u},
}};

// A recording at `output` runs at: an empty image is the output itself, an empty canvas the image (Direct3D 12's Resolve).
WorkLayout Resolve(const WorkLayout& layout, nr::Size output) {
  const nr::Size image = (layout.image.Empty() ? output : layout.image);
  return {.image = image, .canvas = (layout.canvas.Empty() ? image : layout.canvas), .upsampling = layout.upsampling};
}

// Plan 17: the readback ring's buffer and memory (either may be null). Device idle for them, or the GPU past their last copy.
void DestroyCheckBuffer(const vk::NrFunctions& functions, VkDevice device, VkBuffer buffer, VkDeviceMemory memory) {
  if (buffer != VK_NULL_HANDLE) {
    functions.vkDestroyBuffer(device, buffer, nullptr);
  }
  if (memory != VK_NULL_HANDLE) {
    functions.vkFreeMemory(device, memory, nullptr);  // unmaps it
  }
}

// Session::Evaluate wrote a fresh message for exactly these reasons; "resizing" and the rest leave it stale, so ContextMessage must not trust it there.
bool SessionWroteMessage(std::string_view reason) {
  return reason == "frame too small" || reason == "budget" || reason == "create failed" || reason == "evaluate failed";
}

}  // namespace

VkNrPipeline::VkNrPipeline(const vk::NrFunctions& functions, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, nr::Session& session,
                           nr::Timeline& timeline)
    : functions_(functions), device_(device), session_(session), timeline_(timeline), memory_(memory) {}

VkNrPipeline::~VkNrPipeline() {
  for (vk::NrImage* image : {&intermediates_.a, &intermediates_.b, &intermediates_.zero_motion, &intermediates_.canvas_motion, &intermediates_.private_color,
                             &intermediates_.change, &look_.change, &look_.basis, &look_.gauss, &look_.peaks, &look_.history[0], &look_.history[1],
                             &look_.detail_history[0], &look_.detail_history[1], &look_.state, &mask_.image, &exposure_.state, &present_.staging,
                             &present_.color, &present_.launchpad, &present_.stand_in, &faces_.twin, &faces_.detail, &faces_.atlas, &faces_.fill}) {
    vk::DestroyNrImage(functions_, device_, image);
  }
  for (PresentMotionSlot& slot : present_motion_) {
    vk::DestroyNrImage(functions_, device_, &slot.image);
  }
  for (RetiredImage& retired : reshade_retired_) {
    vk::DestroyNrImage(functions_, device_, &retired.image);
  }
  DestroyCheckBuffer(functions_, device_, check_.buffer, check_.memory);
}

bool VkNrPipeline::Initialize(std::string* error) {
  return color_.Initialize(functions_, device_, memory_, error);
}

bool VkNrPipeline::PreSrColorFormatSupported(VkFormat format) {
  return color::VkColorPipeline::OutputVariantOf(format).has_value() || format == VK_FORMAT_B8G8R8A8_UNORM;
}

bool VkNrPipeline::MotionFormatReadable(VkFormat format) {
  return std::ranges::find(READABLE_MOTION_FORMATS, format) != READABLE_MOTION_FORMATS.end();
}

void VkNrPipeline::SetLookConfig(const LookConfig& config) {
  // v2 design §3.12: a new Detail radius or Stabilize mode starts the stabiliser afresh.
  if (config.look.detail_radius != config_.look.detail_radius || config.look.stabilize != config_.look.stabilize
      || config.look.stabilize_detail != config_.look.stabilize_detail) {
    look_.history_valid = false;
  }
  if (config.fixes != config_.fixes) {
    exposure_.snap = true;  // v2 design §3.14: the governor snaps on a settings change
  }
  config_ = config;
}

void VkNrPipeline::RetireSet() {
  if (intermediates_.a.image == VK_NULL_HANDLE) return;
  // Plan 3's rule: every recording takes its mark before its first command, so once the GPU has passed the newest one the set goes at once; otherwise the
  // timeline's callback keeps the handles until then.
  auto destroy = [functions = functions_, device = device_,
                  images = std::array<vk::NrImage, 6>{intermediates_.a, intermediates_.b, intermediates_.zero_motion, intermediates_.canvas_motion,
                                                      intermediates_.private_color, intermediates_.change}]() mutable {  // all but A and B may be null
    for (vk::NrImage& image : images) {
      vk::DestroyNrImage(functions, device, &image);
    }
  };
  if (timeline_.IsComplete(intermediates_.last_use)) {
    destroy();
  } else {
    timeline_.ReleaseAfter(intermediates_.last_use, std::move(destroy));
  }
  intermediates_ = {};
}

void VkNrPipeline::RetireLook() {
  if (look_.plan == LookPlan{}) return;
  auto destroy = [functions = functions_, device = device_,
                  images = std::array<vk::NrImage, 9>{look_.change, look_.basis, look_.gauss, look_.peaks, look_.history[0], look_.history[1],
                                                      look_.detail_history[0], look_.detail_history[1], look_.state}]() mutable {  // the plan's only
    for (vk::NrImage& image : images) {
      vk::DestroyNrImage(functions, device, &image);
    }
  };
  if (timeline_.IsComplete(look_.last_use)) {
    destroy();
  } else {
    timeline_.ReleaseAfter(look_.last_use, std::move(destroy));
  }
  look_ = {};
}

void VkNrPipeline::RetireFaces() {
  if (faces_.twin.image == VK_NULL_HANDLE) return;
  auto destroy = [functions = functions_, device = device_, images = std::array<vk::NrImage, 4>{faces_.twin, faces_.detail, faces_.atlas, faces_.fill}]() mutable {
    for (vk::NrImage& image : images) {
      vk::DestroyNrImage(functions, device, &image);
    }
  };
  if (timeline_.IsComplete(faces_.last_use)) {
    destroy();
  } else {
    timeline_.ReleaseAfter(faces_.last_use, std::move(destroy));
  }
  faces_ = {};
}

bool VkNrPipeline::EnsureFaces(nr::Size canvas) {
  if (faces_.twin.image != VK_NULL_HANDLE && faces_.size == canvas) return true;
  RetireFaces();
  FaceSurfaces created = {.layout = look::MakeAtlas(canvas), .size = canvas};
  created.bytes = FaceSurfaceBytes(canvas);
  // The twin is an NGX output like A and B (read_write); the atlas is the faces pass's own, like the look's.
  const bool made =
      vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, canvas.width, canvas.height, MODEL_USAGE, true, &created.twin)
      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, canvas.width, canvas.height, LOOK_USAGE, false, &created.detail)
      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, created.layout.size.width, created.layout.size.height, LOOK_USAGE,
                           false, &created.atlas)
      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, created.layout.size.width, created.layout.size.height, LOOK_USAGE,
                           false, &created.fill);
  if (!made) {
    for (vk::NrImage* image : {&created.twin, &created.detail, &created.atlas, &created.fill}) {  // never used by the GPU: freed here
      vk::DestroyNrImage(functions_, device_, image);
    }
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} Keep faces surfaces (Vulkan)", canvas.width, canvas.height);
      allocation_failure_logged_ = true;
    }
    return false;
  }
  allocation_failure_logged_ = false;
  faces_ = std::move(created);
  return true;
}

void VkNrPipeline::PrepareFaces(nr::Size canvas, const nr::Controls& controls, nr::FrameInputs* frame_inputs) {
  faces_ran_ = false;
  // Fix round 1: paused by the Session, its surfaces go (I3); a frame whose surfaces cannot be made only goes without the twin (M4).
  if (config_.keep_faces.enabled && look_storage_ && color_.FacesReady()) {
    frame_inputs->faces = {.wanted = true, .controls = FaceTwinControls(controls, config_.keep_faces.protection), .surface_bytes = FaceSurfaceBytes(canvas)};
    if (session_.FacesPaused()) {
      RetireFaces();
    } else if (EnsureFaces(canvas)) {
      frame_inputs->faces.output = nr::AsResource(&faces_.twin.ngx);
    }
  } else {
    RetireFaces();
  }
}

void VkNrPipeline::RecordFaces(VkCommandBuffer buffer, color::VkFacesPass pass) {
  pass.atlas = faces_.atlas.view;
  pass.detail = faces_.detail.view;
  pass.fill = faces_.fill.view;
  pass.layout = faces_.layout;
  pass.parameters = look::MakeFacesParameters(config_.keep_faces.lighting_scale, config_.keep_faces.tuning, intermediates_.image.height, faces_.layout.levels);
  faces_.last_use = slot_marks_[resolve_slot_];
  Barrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);  // what wrote the pass's images (NGX, or the pass's own strengths) is visible to the pyramid
  color_.RecordFacesPyramid(buffer, resolve_slot_, pass);
  if (pass.parameters.speck_radius != 0u) {
    color_.RecordFacesDespike(buffer, resolve_slot_, pass);  // fix round 4: before the combine rewrites `changed` (and pass 1's twin) in place
  }
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  color_.RecordFacesCombine(buffer, resolve_slot_, pass);  // the Session's next transition, or the barrier after NR, makes its writes visible
}

void VkNrPipeline::CombineFaces(ID3D12GraphicsCommandList* list, ID3D12Resource* lighting, ID3D12Resource* faces) {
  RecordFaces(nr::VkListOf(list), {.changed = nr::VkResourceOf(lighting)->Resource.ImageViewInfo.ImageView,
                                   .reference = nr::VkResourceOf(faces)->Resource.ImageViewInfo.ImageView});
  faces_ran_ = true;
}

void VkNrPipeline::RetireMask() {
  RetireReshadeImage(mask_.image, mask_.last_use);  // the add-on copies into it on ReShade's queue
  mask_ = {};
}

void VkNrPipeline::RetireReshadeImage(const vk::NrImage& image, const nr::Mark& mark) {
  if (image.image == VK_NULL_HANDLE) return;
  if (timeline_.IsComplete(mark)) {
    vk::NrImage destroyed = image;
    vk::DestroyNrImage(functions_, device_, &destroyed);
  } else {
    reshade_retired_.push_back({.image = image, .mark = mark});
  }
}

size_t VkNrPipeline::FreeFinished() {
  std::erase_if(reshade_retired_, [this](const RetiredImage& retired) {
    if (!timeline_.IsComplete(retired.mark)) return false;
    vk::NrImage destroyed = retired.image;
    vk::DestroyNrImage(functions_, device_, &destroyed);
    return true;
  });
  return reshade_retired_.size();
}

void VkNrPipeline::RetireExposure() {
  if (exposure_.state.image != VK_NULL_HANDLE) {
    auto destroy = [functions = functions_, device = device_, image = exposure_.state]() mutable { vk::DestroyNrImage(functions, device, &image); };
    if (timeline_.IsComplete(exposure_.last_use)) {
      destroy();
    } else {
      timeline_.ReleaseAfter(exposure_.last_use, std::move(destroy));
    }
  }
  exposure_ = {};
  RetireExposureCheck();
}

void VkNrPipeline::RetireExposureCheck() {
  if (check_.buffer != VK_NULL_HANDLE) {
    // The samples still in flight are dropped; the ring goes once the GPU has passed the newest copy it has not finished.
    std::optional<nr::Mark> in_flight;
    for (uint32_t step = 1u; step <= CHECK_SLOTS && !in_flight; ++step) {
      const uint32_t index = (check_.next + CHECK_SLOTS - step) % CHECK_SLOTS;  // the newest first
      if (check_.pending[index] && !timeline_.IsComplete(check_.marks[index])) {
        in_flight = check_.marks[index];
      }
    }
    auto destroy = [functions = functions_, device = device_, buffer = check_.buffer, memory = check_.memory] { DestroyCheckBuffer(functions, device, buffer, memory); };
    if (in_flight) {
      timeline_.ReleaseAfter(*in_flight, std::move(destroy));
    } else {
      destroy();
    }
  }
  check_ = {};
}

bool VkNrPipeline::EnsureExposureCheck() {
  if (check_.buffer != VK_NULL_HANDLE) return true;
  const VkBufferCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = CHECK_SLOTS * CHECK_STRIDE,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  ExposureCheck made;
  bool worked = (functions_.vkCreateBuffer(device_, &create, nullptr, &made.buffer) == VK_SUCCESS);
  if (worked) {
    VkMemoryRequirements requirements = {};
    functions_.vkGetBufferMemoryRequirements(device_, made.buffer, &requirements);
    uint32_t type = vk::FindMemoryType(memory_, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX) {
      type = vk::FindMemoryType(memory_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
    void* mapped = nullptr;
    worked = (type != UINT32_MAX && functions_.vkAllocateMemory(device_, &allocate, nullptr, &made.memory) == VK_SUCCESS
              && functions_.vkBindBufferMemory(device_, made.buffer, made.memory, 0u) == VK_SUCCESS
              && functions_.vkMapMemory(device_, made.memory, 0u, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS);
    made.mapped = static_cast<const std::byte*>(mapped);
  }
  if (!worked) {
    DestroyCheckBuffer(functions_, device_, made.buffer, made.memory);  // never used by the GPU
    return false;
  }
  check_ = made;
  return true;
}

void VkNrPipeline::PollExposureChecks() {
  if (check_.buffer == VK_NULL_HANDLE) return;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  for (uint32_t step = 0u; step < CHECK_SLOTS; ++step) {
    const uint32_t index = (check_.next + step) % CHECK_SLOTS;  // the oldest first
    if (!check_.pending[index] || !timeline_.IsComplete(check_.marks[index])) continue;
    check_.pending[index] = false;
    std::array<float, 8> texels = {};  // (2^E, E, the anchor, set), (the game's texel, its factor, the game's exposure, the meter's target)
    std::memcpy(texels.data(), check_.mapped + index * CHECK_STRIDE, sizeof(texels));
    const ExposureSample sample = {.texture_value = texels[4], .factor = texels[5], .game = texels[6], .metered_stops = texels[1], .target_stops = texels[7],
                                   .anchor_stops = texels[2]};
    if (auto_exposure_.Observe(sample, seconds)) {
      nr::Log(nr::LogLevel::INFO, AutoLatchMessage(sample, auto_exposure_));
    } else if (!auto_exposure_.Latched() && std::isfinite(AutoExposure::OutsideSpan(sample)) && auto_exposure_.DebugDue(seconds)) {
      nr::Log(nr::LogLevel::TRACE, AutoDebugMessage(sample, config_.fixes.auto_mode, config_.fixes.auto_blend, auto_exposure_.HeldThroughMove()));  // 2026-10-09: once a second
    }
  }
}

void VkNrPipeline::ReleaseIntermediates() {
  ReleaseNrSurfaces();
  RetireMask();
  RetirePresent();
}

void VkNrPipeline::RetirePresent() {
  // Plan 19: used in Uplift's own command buffers on ReShade's queue alone, which no completion token covers (final review C-1's rule).
  RetireReshadeImage(present_.staging, present_.last_use);
  RetireReshadeImage(present_.color, present_.last_use);
  RetireReshadeImage(present_.stand_in, present_.last_use);
  RetirePresentLaunchpad();
  present_ = {};
}

void VkNrPipeline::RetirePresentLaunchpad() {
  RetireReshadeImage(present_.launchpad, present_.launchpad_last_use);
  present_.launchpad = {};
  present_.launchpad_size = {};
  present_.launchpad_bytes = 0u;
}

void VkNrPipeline::ReleaseNrSurfaces() {
  RetireSet();
  RetireLook();
  RetireFaces();
  RetireExposure();
}

void VkNrPipeline::LogAllocationFailure(nr::Size size) {
  if (allocation_failure_logged_) return;
  nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} NR surfaces (Vulkan)", size.width, size.height);
  allocation_failure_logged_ = true;
}

LookPlan VkNrPipeline::LookPlanFor(nr::Size image, bool reduced) const {
  return PlanLook(config_, image, reduced, intermediates_.change.image != VK_NULL_HANDLE, mask_.copied, look_storage_);
}

bool VkNrPipeline::EnsureLook(const LookPlan& plan) {
  if (plan == look_.plan) return true;
  RetireLook();
  if (plan == LookPlan{}) return true;
  LookSurfaces created = {.plan = plan, .atlas = (plan.shape ? look::MakeAtlas(plan.image) : look::Atlas{})};
  bool made = true;
  const auto make = [&](bool wanted, nr::Size size, VkFormat format, uint64_t bytes_per_pixel, VkImageUsageFlags usage, vk::NrImage* image) {
    if (!wanted || !made) return;
    created.bytes += size.Pixels() * bytes_per_pixel;
    made = vk::CreateNrImage(functions_, device_, memory_, format, size.width, size.height, usage, false, image);
  };
  make(plan.own_change, plan.image, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &created.change);
  make(plan.shape, plan.image, VK_FORMAT_R16G16_SFLOAT, 4u, LOOK_USAGE, &created.basis);
  make(plan.shape, created.atlas.size, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &created.gauss);
  make(plan.shape, created.atlas.size, VK_FORMAT_R16_SFLOAT, 2u, LOOK_USAGE, &created.peaks);
  for (vk::NrImage& history : created.history) {
    make(plan.stabilize, created.atlas.size, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &history);
  }
  for (vk::NrImage& history : created.detail_history) {
    make(plan.detail, plan.image, VK_FORMAT_R16G16B16A16_SFLOAT, MODEL_BYTES_PER_PIXEL, LOOK_USAGE, &history);
  }
  make(plan.stabilize, {1u, 1u}, VK_FORMAT_R32G32B32A32_SFLOAT, STATE_BYTES, VK_IMAGE_USAGE_STORAGE_BIT, &created.state);
  if (!made) {
    // Never used by the GPU: freed here.
    for (vk::NrImage* image : {&created.change, &created.basis, &created.gauss, &created.peaks, &created.history[0], &created.history[1],
                               &created.detail_history[0], &created.detail_history[1], &created.state}) {
      vk::DestroyNrImage(functions_, device_, image);
    }
    if (!allocation_failure_logged_) {
      nr::Logf(nr::LogLevel::ERR, "could not allocate the {}x{} look surfaces (Vulkan)", plan.image.width, plan.image.height);
      allocation_failure_logged_ = true;
    }
    return false;
  }
  allocation_failure_logged_ = false;
  look_ = std::move(created);
  return true;
}

uint32_t VkNrPipeline::ShaderOptions() const {
  return static_cast<uint32_t>(config_.fixes.primaries)  // the shaders resolve AUTO per encoding
         | (config_.fixes.near_black_guard ? color::shader_options::NEAR_BLACK_GUARD : 0u)
         | (config_.fixes.transfer == color::NeuralTransfer::CONSISTENT ? color::shader_options::CONSISTENT : 0u);
}

color::VkSampledView VkNrPipeline::BoundMask(uint32_t slot) {
  if (!config_.mask || !mask_.copied) return {};
  mask_.last_use = slot_marks_[slot];
  return {.view = mask_.image.view, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkImage VkNrPipeline::PrepareMaskCopy(VkFormat format, nr::Size size, bool* first) {
  const auto found = std::ranges::find(MASK_FORMATS, format, &MaskFormat::format);
  if (found == MASK_FORMATS.end() || size.Empty()) {
    RetireMask();
    return VK_NULL_HANDLE;
  }
  if (mask_.image.image == VK_NULL_HANDLE || mask_.size != size || mask_.format != format) {
    RetireMask();
    MaskCopy created = {.size = size, .format = format, .bytes = size.Pixels() * found->bytes_per_pixel};
    if (!vk::CreateNrImage(functions_, device_, memory_, format, size.width, size.height, MASK_USAGE, false, &created.image)) {
      LogAllocationFailure(size);
      return VK_NULL_HANDLE;
    }
    allocation_failure_logged_ = false;
    mask_ = std::move(created);
  }
  mask_.last_use = timeline_.MarkNow();  // the add-on copies into it on this frame's list of ReShade's queue
  *first = !mask_.copied;
  return mask_.image.image;
}

void VkNrPipeline::Barrier(VkCommandBuffer buffer, VkPipelineStageFlags source_stage) const {
  functions_.vkCmdPipelineBarrier(buffer, source_stage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &GLOBAL_DEPENDENCY, 0u, nullptr, 0u, nullptr);
}

void VkNrPipeline::OpenRecording(VkCommandBuffer buffer, std::span<const vk::NrImage* const> images, bool with_look) {
  std::array<VkImageMemoryBarrier, MAX_OPENED> transitions = {};
  uint32_t count = 0u;
  const auto open = [&transitions, &count](const vk::NrImage& image) {
    if (image.image == VK_NULL_HANDLE || count == transitions.size()) return;
    transitions[count++] = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
    };
  };
  for (const vk::NrImage* image : images) {
    open(*image);
  }
  if (with_look) {
    for (const vk::NrImage* image : {&look_.change, &look_.basis, &look_.gauss, &look_.peaks}) {
      open(*image);
    }
    // The stabiliser's histories and scene-cut state carry across frames: GENERAL from their first recording on (a reset clears them in the shader).
    if (!look_.carried) {
      for (const vk::NrImage* image : {&look_.history[0], &look_.history[1], &look_.detail_history[0], &look_.detail_history[1], &look_.state}) {
        open(*image);
      }
      look_.carried = true;
    }
  }
  // The meter's state carries across frames likewise (its first meter snaps, whatever it holds).
  if (exposure_.state.image != VK_NULL_HANDLE && !exposure_.carried) {
    open(exposure_.state);
    exposure_.carried = true;
  }
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &GLOBAL_DEPENDENCY, 0u, nullptr,
                                  count, transitions.data());
  color_.PrepareRecording(buffer);
}

void VkNrPipeline::HandToNgx(VkCommandBuffer buffer, const vk::NrImage& image) const {
  const VkImageMemoryBarrier handed = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
  };
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &GLOBAL_DEPENDENCY, 0u, nullptr,
                                  1u, &handed);
}

color::VkSampledView VkNrPipeline::GameExposureView(const NVSDK_NGX_Resource_VK* exposure) {
  if (exposure == nullptr || exposure->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {};
  return {.view = exposure->Resource.ImageViewInfo.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkNrPipeline::ExposureChoice VkNrPipeline::ChooseExposure(color::Encoding encoding, const NVSDK_NGX_Resource_VK* game_texture, float game_factor) {
  PollExposureChecks();
  const bool game_exposure = (game_texture != nullptr || game_factor != 1.f);
  // v2 design §3.14: only relative scene-linear images are metered, and the meter needs the look's storage formats (its state is RGBA32F).
  const bool can_meter = (color::Meterable(encoding) && look_storage_);
  const bool automatic = (config_.fixes.input_exposure == color::InputExposure::AUTO);
  bool use_game = false;
  bool metered = false;
  bool probe = false;
  bool blend = false;
  switch (config_.fixes.input_exposure) {
    case color::InputExposure::AUTO:
      // Plan 17: Direct3D 12's rule. The game's exposure while it is valid against the meter (which runs beside it), the meter once it has not been for a
      // sustained second, the meter without one, and the game's where the meter cannot run. 2026-10-09: Blend, the blend of the two in the meter's state.
      if (!game_exposure) {
        metered = true;
      } else if (can_meter && auto_exposure_.Latched()) {
        metered = true;
      } else if (can_meter && config_.fixes.auto_mode == color::AutoExposureMode::BLEND) {
        metered = true;
        probe = true;
        blend = true;
      } else {
        use_game = true;
        probe = can_meter;
      }
      break;
    case color::InputExposure::GAME:    use_game = true; break;
    case color::InputExposure::METERED: metered = true; break;
    case color::InputExposure::MANUAL:  break;
  }
  metered = (metered && can_meter);
  if (!metered && !probe && exposure_.state.image != VK_NULL_HANDLE) {
    RetireExposure();  // InputExposure left Metered or auto-metered: the state is no longer read
  }
  if ((metered || probe) && exposure_.state.image == VK_NULL_HANDLE) {
    if (vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R32G32B32A32_SFLOAT, EXPOSURE_STATE_WIDTH, 1u, EXPOSURE_USAGE, false, &exposure_.state)) {
      exposure_.snap = true;  // undefined until the meter's first snap
    } else {
      LogAllocationFailure({EXPOSURE_STATE_WIDTH, 1u});
    }
  }
  const bool have_state = (exposure_.state.image != VK_NULL_HANDLE);
  const bool probing = (probe && have_state);
  const color::VkSampledView state_view = {.view = exposure_.state.view, .layout = VK_IMAGE_LAYOUT_GENERAL};
  if (blend && have_state) {
    exposure_report_ = {.use = ExposureUse::BLEND, .blend = config_.fixes.auto_blend, .gap = auto_exposure_.LastGap()};
    return {.view = state_view, .factor = 1.f, .metered = true, .probe = true, .game_view = GameExposureView(game_texture), .game_factor = game_factor,
            .game_weight = 1.f - std::clamp(config_.fixes.auto_blend, 0.f, 1.f)};
  }
  if (metered && have_state) {
    const bool latched = (automatic && game_exposure && auto_exposure_.Latched());
    exposure_report_ = {.use = ExposureUse::METERED,
                        .stops_off = (latched ? std::optional<float>(auto_exposure_.StopsOff()) : std::nullopt),
                        .latch = (latched ? auto_exposure_.Reason() : AutoLatch::NONE)};
    return {.view = state_view, .factor = 1.f, .metered = true};
  }
  if (use_game || blend) {  // Blend without its state: the game's
    exposure_report_ = {.use = (game_exposure ? ExposureUse::GAME : ExposureUse::NONE)};
    const color::VkSampledView game_view = GameExposureView(game_texture);
    return {.view = game_view, .factor = game_factor, .probe = probing, .game_view = game_view, .game_factor = game_factor};
  }
  exposure_report_ = {};
  return {};
}

bool VkNrPipeline::RecordMeter(VkCommandBuffer buffer, uint32_t slot, color::VkMeterPass pass, const ExposureChoice& exposure) {
  pass.state = exposure_.state.view;
  pass.snap = (exposure_.snap || intermediates_.reset_pending);  // the first frame, a settings change, a rebuild
  pass.smooth = config_.fixes.smooth_adapt;
  pass.brighter_rate = config_.fixes.adapt_brighter;
  pass.darker_rate = config_.fixes.adapt_darker;
  pass.frame_seconds = config_.frame_seconds;
  pass.probe = exposure.probe;
  pass.game_exposure = (exposure.probe ? exposure.game_view : color::VkSampledView{});
  pass.game_exposure_factor = (exposure.probe ? exposure.game_factor : 1.f);
  pass.game_weight = (exposure.probe ? exposure.game_weight : 0.f);
  exposure_.last_use = slot_marks_[slot];
  if (!color_.RecordMeter(buffer, slot, pass)) return false;
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);  // the encode reads the state (and Auto's check copies it)
  exposure_.snap = false;
  if (!exposure.probe) return true;
  // Plan 17: Auto's check. The state's two texels into the next readback slot (GENERAL is a legal copy source), unless the GPU has not passed that slot's
  // last copy yet (skipped); then the host may read it once the recording completes.
  const uint32_t index = check_.next;
  if (check_.pending[index] || !EnsureExposureCheck()) return true;
  const VkBufferImageCopy copy = {
      .bufferOffset = index * CHECK_STRIDE,
      .bufferRowLength = 0u,
      .bufferImageHeight = 0u,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
      .imageOffset = {0, 0, 0},
      .imageExtent = {EXPOSURE_STATE_WIDTH, 1u, 1u},
  };
  functions_.vkCmdCopyImageToBuffer(buffer, exposure_.state.image, VK_IMAGE_LAYOUT_GENERAL, check_.buffer, 1u, &copy);
  const VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &to_host, 0u, nullptr, 0u, nullptr);
  check_.marks[index] = slot_marks_[slot];
  check_.pending[index] = true;
  check_.next = (index + 1u) % CHECK_SLOTS;
  return true;
}

void VkNrPipeline::ResolvePass(ID3D12GraphicsCommandList* list, uint32_t index, ID3D12Resource* given, ID3D12Resource* returned) {
  if (index == 0u || index > LATER_PASSES || !look_storage_) return;
  const VkImageView given_view = nr::VkResourceOf(given)->Resource.ImageViewInfo.ImageView;
  const VkImageView returned_view = nr::VkResourceOf(returned)->Resource.ImageViewInfo.ImageView;
  if (const PassStrengths& strengths = config_.later_strengths[index - 1u]; strengths != PassStrengths{}) {
    // The Session's transition after NR's pass made its writes visible to this dispatch (VkHost::Transition is a global barrier), and the one before the next
    // pass, or RecordAfterNr's barrier, makes this one's visible to what reads it.
    color_.RecordResolve(nr::VkListOf(list), resolve_slot_, index,
                         {.given = given_view,
                          .returned = returned_view,
                          .size = intermediates_.size,
                          .transfer_strength = strengths.transfer,
                          .color_strength = strengths.color});
  }
  if (faces_ran_) {
    // Keep faces (design "Passes"): inside pass 1's face mask only this pass's broad change is kept.
    RecordFaces(nr::VkListOf(list), {.changed = returned_view, .reference = given_view, .mask = faces_.twin.view});
  }
}

color::VkStabilizeMotion VkNrPipeline::StabilizeMotionOf(const nr::FrameInputs& inputs) {
  if (inputs.motion.resource == nullptr) return {};
  const NVSDK_NGX_Resource_VK& motion = *nr::VkResourceOf(inputs.motion.resource);
  if (motion.Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {};
  const NVSDK_NGX_ImageViewInfo_VK& image = motion.Resource.ImageViewInfo;
  nr::Rect rect = inputs.motion.rect;
  if (rect.width == 0u || rect.height == 0u) {
    rect = {.x = 0u, .y = 0u, .width = image.Width, .height = image.Height};
  }
  if (!MotionFormatReadable(image.Format) || uint64_t{rect.x} + rect.width > image.Width || uint64_t{rect.y} + rect.height > image.Height) return {};
  return {
      .vectors = {.view = image.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      .rect = rect,
      .scale_x = inputs.motion_scale_x,
      .scale_y = inputs.motion_scale_y,
  };
}

float VkNrPipeline::NoteEncoding(const VkDlssTarget& target) {
  const float input_scale = color::InputScale(target.encoding, target.diffuse_white_nits, config_.fixes.linear_unit_nits);
  // Plan 2 final review M4: NR's history belongs to one encoding and input scale; a change starts it afresh.
  if (target.encoding != intermediates_.encoding || input_scale != intermediates_.input_scale) {
    intermediates_.encoding = target.encoding;
    intermediates_.input_scale = input_scale;
    intermediates_.reset_pending = true;
  }
  return input_scale;
}

VkNrPipeline::AfterNr VkNrPipeline::RecordAfterNr(VkCommandBuffer buffer, uint32_t slot, const color::VkEncodePass& encode, VkImageView nr_output,
                                                  bool reduced, bool look_ready, color::Upsampling upsampling, const color::VkStabilizeMotion& motion,
                                                  bool reset) {
  // At the output size with nothing acting on NR's change: Plan 4's restore, bit for bit (key decision 3).
  if (!reduced && !(look_ready && NeedsChangePath(config_, mask_.copied, look_storage_))) return {.view = nr_output};
  if (look_.plan != LookPlan{}) {
    look_.last_use = slot_marks_[slot];
  }
  const VkImageView change = (intermediates_.change.image != VK_NULL_HANDLE ? intermediates_.change.view : look_.change.view);
  const bool shape = (look_ready && look_.plan.shape);
  // 1. NR's change field (v2 design §3.7) and, for the look, NR's input chroma κ_M.
  if (!color_.RecordChange(buffer, slot, {.encode = encode, .nr_output = nr_output, .change = change, .basis = (shape ? look_.basis.view : VK_NULL_HANDLE)})) {
    return {.ok = false};
  }
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  // At the output size the compose reads C texel for texel: Classic at 1:1 samples each texel exactly.
  const AfterNr after = {.view = change, .upsampling = (reduced ? upsampling : color::Upsampling::CLASSIC)};
  if (!shape) return after;
  // 2. The pyramids.
  const look::Atlas& atlas = look_.atlas;
  const look::Bands bands = look::MakeBands(config_.look.detail_radius, atlas.image.height, atlas.levels);
  color_.RecordPyramid(buffer, slot, {.change = change, .gauss = look_.gauss.view, .peaks = look_.peaks.view, .atlas = atlas});
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  // 3. The stabiliser (key decision 5): the two levels the low band reads, then the fine band.
  VkImageView history = VK_NULL_HANDLE;
  VkImageView detail = VK_NULL_HANDLE;
  if (look_.plan.stabilize) {
    const uint32_t current = look_.current;
    history = look_.history[current].view;
    color::VkStabilizePass stabilize = {
        .gauss = look_.gauss.view,
        .previous = look_.history[1u - current].view,
        .current = history,
        .state = look_.state.view,
        .atlas = atlas,
        .bands = bands,
        .rate = look::StabilizeRate(config_.frame_seconds, config_.look.stabilize_ms),
        .motion_mode = (config_.look.stabilize == look::StabilizeMode::MOTION),
        .reset = (reset || !look_.history_valid),
        .motion = motion,
    };
    color_.RecordStabilize(buffer, slot, stabilize);
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (look_.plan.detail) {
      detail = look_.detail_history[current].view;
      stabilize.change = change;
      stabilize.previous = look_.detail_history[1u - current].view;
      stabilize.current = detail;
      color_.RecordDetail(buffer, slot, stabilize);
      Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    look_.current = 1u - current;
    look_.history_valid = true;
  }
  // 4. The shape pass rewrites C in place.
  color_.RecordShape(buffer, slot,
                     {.change = change,
                      .basis = look_.basis.view,
                      .gauss = look_.gauss.view,
                      .peaks = look_.peaks.view,
                      .history = history,
                      .detail = detail,
                      .atlas = atlas,
                      .bands = bands,
                      .settings = config_.look.shape,
                      .sdr = color::IsSdr(encode.encoding)});
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  return after;
}

PipelineResult VkNrPipeline::RecordAfterDlss(VkCommandBuffer buffer, const VkDlssTarget& target, const nr::FrameInputs& inputs, const nr::Controls& controls,
                                             const WorkLayout& layout) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = "inactive"};
  // Batch 2 review, minor 5: the union is read only for an image view (NGX's resource can also be a buffer).
  if (buffer == VK_NULL_HANDLE || target.resource == nullptr || target.resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) {
    return {.reason = "invalid input"};
  }
  const NVSDK_NGX_ImageViewInfo_VK& output = target.resource->Resource.ImageViewInfo;
  nr::Rect region = target.region;
  if (region.width == 0u && region.height == 0u) {
    region = {.x = 0u, .y = 0u, .width = output.Width, .height = output.Height};
  }
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > output.Width || uint64_t{region.y} + region.height > output.Height) {
    return {.reason = "invalid input"};
  }
  const WorkLayout work = Resolve(layout, size);
  if (work.canvas.width < work.image.width || work.canvas.height < work.image.height) return {.reason = "invalid input"};
  // Eligibility, in AfterDlssSource::Run's order. Nothing is recorded for any of these. The floor holds for the output and for the canvas NR runs at.
  if (!nr::MeetsNrFloor(size) || !nr::MeetsNrFloor(work.canvas)) return {.reason = "frame too small"};
  if (!color::VkColorPipeline::OutputVariantOf(output.Format)) return {.reason = "unsupported format"};
  if (controls.intensity <= 0.f) return {.reason = "intensity 0"};  // exact pass-through: the Output stays bit-identical
  // Batch 2 review, minor 3: every pipeline exists before the encode is recorded, so a decode that cannot be built never leaves NR running unused.
  if (!color_.Prepare(output.Format)) return {.reason = "pipeline failed"};

  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = "descriptors busy"};
  const bool game_motion = (inputs.motion.resource != nullptr);
  const bool reduced = (work.image != size);
  // The set: A and B at the canvas, and (Plan 14) the change field at the work image below the output size. A different layout (or the other placement's set)
  // reallocates it (the old one retires at its mark).
  if (intermediates_.a.image == VK_NULL_HANDLE || intermediates_.pre_sr || intermediates_.size != work.canvas || intermediates_.image != work.image
      || intermediates_.output != size) {
    RetireSet();
    Intermediates created = {
        .size = work.canvas,
        .image = work.image,
        .output = size,
        .bytes = work.canvas.Pixels() * 2u * MODEL_BYTES_PER_PIXEL + (reduced ? work.image.Pixels() * MODEL_BYTES_PER_PIXEL : 0u),
        .motion_bytes = work.canvas.Pixels() * MOTION_BYTES_PER_PIXEL,
    };
    const bool made =
        vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, work.canvas.width, work.canvas.height, MODEL_USAGE, true, &created.a)
        && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, work.canvas.width, work.canvas.height, MODEL_USAGE, true, &created.b)
        && (!reduced
            || vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, work.image.width, work.image.height, MODEL_USAGE, false,
                                 &created.change));
    if (!made) {
      for (vk::NrImage* image : {&created.a, &created.b, &created.change}) {
        vk::DestroyNrImage(functions_, device_, image);
      }
      LogAllocationFailure(work.canvas);
      return {.reason = "out of memory"};
    }
    allocation_failure_logged_ = false;
    intermediates_ = std::move(created);
  }
  // Batch 2 review, minor 4: the zero motion only once a frame has no game motion (a DLSS game nearly always passes its own).
  if (!game_motion && intermediates_.zero_motion.image == VK_NULL_HANDLE
      && !vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, work.canvas.width, work.canvas.height, MOTION_USAGE, false,
                            &intermediates_.zero_motion)) {
    LogAllocationFailure(work.canvas);
    return {.reason = "out of memory"};
  }
  const bool look_ready = EnsureLook(LookPlanFor(work.image, reduced));  // before the fit, like the set
  nr::FrameInputs frame_inputs = inputs;
  PrepareFaces(work.canvas, controls, &frame_inputs);  // Keep faces: its surfaces before the fit and the opening, like the look's
  // The GPU reads this slot's constants, and the intermediates, until this recording completes: the current frame, and the token the caller issued before
  // recording (Plan 3's rule: every mark taken from here on includes it).
  slot_marks_[slot] = timeline_.MarkNow();
  intermediates_.last_use = slot_marks_[slot];
  // Fix round 1 (C1): OpenRecording below transitions the look's and Keep faces' images whenever they exist, whether or not NR then runs and uses them, so
  // this recording is their last use too.
  if (look_.plan != LookPlan{}) {
    look_.last_use = slot_marks_[slot];
  }
  if (faces_.twin.image != VK_NULL_HANDLE) {
    faces_.last_use = slot_marks_[slot];
  }
  next_slot_ = (slot + 1u) % color::VkColorPipeline::RING_SLOTS;

  const float input_scale = NoteEncoding(target);
  const uint32_t options = ShaderOptions();
  const ExposureChoice exposure = ChooseExposure(target.encoding, target.exposure, target.exposure_factor);

  // 1. The opening barrier: DLSS's write of the Output (and whatever else the game recorded before) is visible to Uplift's reads and writes, and A, B, the
  // change field, the zero motion and the look's surfaces move UNDEFINED -> GENERAL (their contents are rewritten in full every recording).
  const std::array<const vk::NrImage*, 8> opened = {&intermediates_.a, &intermediates_.b, &intermediates_.zero_motion, &intermediates_.change, &faces_.twin,
                                                    &faces_.detail, &faces_.atlas, &faces_.fill};
  OpenRecording(buffer, opened, true);

  // 2. Encode the output region, in place, into A at the work image with the game's (or the meter's) exposure; MotionVectors = None clears the zero motion in
  // the same dispatch.
  const MotionSource motion_source = (game_motion ? MotionSource::DIRECT : MotionSource::NONE);
  const color::VkEncodePass encode = {
      .source = output.ImageView,
      .source_format = output.Format,
      .model = intermediates_.a.view,
      .motion = (game_motion ? VK_NULL_HANDLE : intermediates_.zero_motion.view),
      .width = work.image.width,
      .height = work.image.height,
      .encoding = target.encoding,
      .input_scale = input_scale,
      .exposure = exposure.view,
      .exposure_factor = exposure.factor,
      .region = region,
      .canvas = work.canvas,
      .options = options,
  };
  // InputExposure = Metered: the meter reads the region the encode reads, in place, and the encode and the decode read its state as their exposure. Plan 17:
  // it also runs for Auto's check, where a failed dispatch only skips that sample.
  if (exposure.metered || exposure.probe) {
    const bool recorded = RecordMeter(buffer, slot,
                                      {.source = output.ImageView,
                                       .source_format = output.Format,
                                       .region = region,
                                       .encoding = target.encoding,
                                       .primaries = (options & color::shader_options::PRIMARIES_MASK),
                                       .input_scale = input_scale},
                                      exposure);
    if (!recorded && exposure.metered) return {.recorded = true, .reason = "encode failed", .motion_source = motion_source};
  }
  if (!color_.RecordEncode(buffer, slot, encode)) {
    return {.recorded = true, .reason = "encode failed", .motion_source = motion_source};
  }

  // 3. The NR passes, with the game's motion vectors bound in place (the zero motion otherwise), the per-pass model controls, and each later pass's own
  // Transfer/Colour strength (the resolver, between the passes).
  if (game_motion) {
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  } else {
    // Batch 2 review, minor 1: NGX reads the zero motion as an input (ReadWrite false), so it is handed over in SHADER_READ_ONLY_OPTIMAL, as a game's own
    // motion vectors are (R79); the next recording starts it from UNDEFINED again.
    HandToNgx(buffer, intermediates_.zero_motion);
  }
  if (!game_motion) {
    frame_inputs.motion = {.resource = nr::AsResource(&intermediates_.zero_motion.ngx)};
    frame_inputs.motion_scale_x = 1.f;
    frame_inputs.motion_scale_y = 1.f;
  }
  frame_inputs.reset_hint = (inputs.reset_hint || intermediates_.reset_pending);
  frame_inputs.later_passes = config_.later_controls;
  frame_inputs.resolver = this;
  resolve_slot_ = slot;
  const uint64_t held = HeldBytes();  // allocated before the fit: charged as the chain's surfaces and credited as held
  const nr::EvaluateResult evaluated = session_.Evaluate(
      nr::AsList(buffer),
      {.a = nr::AsResource(&intermediates_.a.ngx), .b = nr::AsResource(&intermediates_.b.ngx), .size = work.canvas, .surface_bytes = held, .held_intermediate_bytes = held},
      frame_inputs, controls);
  const bool from_session = SessionWroteMessage(evaluated.reason);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
  }
  intermediates_.reset_pending = false;  // Plan 3: only a frame that ran NR pays off the owed reset

  // 4. Plan 14: the change field and the look stage (Direct3D 12's RecordAfterNr), or NR's own output for the full-size restore. NR's writes are visible first.
  Barrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  const VkImageView nr_output = (evaluated.output == nr::AsResource(&intermediates_.a.ngx) ? intermediates_.a.view : intermediates_.b.view);
  if (faces_ran_ && config_.keep_faces.show_mask) {
    // Show the face mask: the mask tinted into NR's result, which the change field and the decode then carry to the image.
    color_.RecordFacesShow(buffer, slot, nr_output, faces_.twin.view, work.canvas);
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }
  const AfterNr after = RecordAfterNr(buffer, slot, encode, nr_output, reduced, look_ready, work.upsampling, StabilizeMotionOf(inputs), frame_inputs.reset_hint);
  if (!after.ok) {
    return {.recorded = true, .reason = "encode failed", .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
  }

  // 5. The decode, in place, from NR's output or the change field with the original pixel read from the Output itself, then the closing barrier: whatever the
  // game does next sees Uplift's writes, whatever source stage its own barrier names.
  if (!color_.RecordDecode(buffer, slot,
                           {.target = output.ImageView,
                            .target_format = output.Format,
                            .nr_output = after.view,
                            .origin_x = region.x,
                            .origin_y = region.y,
                            .width = size.width,
                            .height = size.height,
                            .encoding = target.encoding,
                            .input_scale = input_scale,
                            .transfer_strength = target.transfer_strength,
                            .color_strength = target.color_strength,
                            .exposure = exposure.view,
                            .exposure_factor = exposure.factor,
                            .source_x = region.x,
                            .source_y = region.y,
                            .upsampling = after.upsampling,
                            .options = options,
                            .chroma_clamp = config_.fixes.chroma_clamp,
                            .mask = BoundMask(slot)})) {
    return {.recorded = true, .reason = "decode failed", .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
  }
  Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = frame_inputs.motion_scale_x, .motion_scale_y = frame_inputs.motion_scale_y};
}

PipelineResult VkNrPipeline::RecordPreSr(VkCommandBuffer buffer, const VkDlssTarget& color, const nr::FrameInputs& inputs, const nr::Controls& controls,
                                         const WorkLayout& layout) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = "inactive"};
  if (buffer == VK_NULL_HANDLE || color.resource == nullptr || color.resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) {
    return {.reason = "invalid input"};
  }
  const NVSDK_NGX_ImageViewInfo_VK& game = color.resource->Resource.ImageViewInfo;
  const nr::Rect region = color.region;
  const nr::Size size = {region.width, region.height};
  if (size.Empty() || uint64_t{region.x} + region.width > game.Width || uint64_t{region.y} + region.height > game.Height) {
    return {.reason = "invalid input"};
  }
  // v2 design §3.9 step 1: a colour format the sampled encode reads; Intensity 0 swaps nothing either (exact pass-through).
  if (!PreSrColorFormatSupported(game.Format)) return {.reason = "unsupported format"};
  if (controls.intensity <= 0.f) return {.reason = "intensity 0"};
  // Plan 4's I-1: the set and the feature are keyed on the settled work image, never the per-frame render region. Plan 14 (R88): a region that is not the
  // settled image is resampled into it, and the decode upsamples NR's change back to the region, as on Direct3D 12.
  const WorkLayout work = Resolve(layout, size);
  const nr::Size image = work.image;
  const nr::Size canvas = work.canvas;
  if (canvas.width < image.width || canvas.height < image.height) return {.reason = "invalid input"};
  if (!nr::MeetsNrFloor(canvas)) return {.reason = "frame too small"};  // the floor holds for the canvas, never below it
  // The game's motion vectors: bound in place at the image's own size, copied into a canvas texture in a canvas (E9: only float formats can be copied; the
  // rest bind the zero texture there). v2 design §3.9 step 1: their region lies inside their texture.
  const bool game_motion = (inputs.motion.resource != nullptr);
  const bool in_canvas = (canvas != image);
  nr::Rect motion_region = inputs.motion.rect;
  bool motion_readable = false;
  if (game_motion) {
    const NVSDK_NGX_Resource_VK& motion = *nr::VkResourceOf(inputs.motion.resource);
    if (motion.Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return {.reason = "invalid input"};
    const NVSDK_NGX_ImageViewInfo_VK& motion_image = motion.Resource.ImageViewInfo;
    if (motion_region.width == 0u || motion_region.height == 0u) {
      motion_region = {.x = 0u, .y = 0u, .width = motion_image.Width, .height = motion_image.Height};
    }
    if (uint64_t{motion_region.x} + motion_region.width > motion_image.Width || uint64_t{motion_region.y} + motion_region.height > motion_image.Height) {
      return {.reason = "motion vectors outside their texture"};
    }
    motion_readable = MotionFormatReadable(motion_image.Format);
  }
  const bool copy_motion = (in_canvas && game_motion && motion_readable);
  const bool use_zero_motion = (!game_motion || (in_canvas && !motion_readable));
  if (!color_.PreparePreSr()) return {.reason = "pipeline failed"};

  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return {.reason = "descriptors busy"};
  const nr::Size color_size = {game.Width, game.Height};
  // The change field always exists here (I-1): whether a frame needs it depends on its render region, which the set is not keyed on.
  if (intermediates_.a.image == VK_NULL_HANDLE || !intermediates_.pre_sr || intermediates_.size != canvas || intermediates_.image != image
      || intermediates_.color_size != color_size) {
    RetireSet();
    Intermediates created = {
        .size = canvas,
        .image = image,
        .output = image,
        .color_size = color_size,
        .pre_sr = true,
        .bytes = (canvas.Pixels() * 2u + color_size.Pixels() + image.Pixels()) * MODEL_BYTES_PER_PIXEL,
        .motion_bytes = canvas.Pixels() * MOTION_BYTES_PER_PIXEL,
    };
    const bool made =
        vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, canvas.width, canvas.height, MODEL_USAGE, true, &created.a)
        && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, canvas.width, canvas.height, MODEL_USAGE, true, &created.b)
        && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, color_size.width, color_size.height, MODEL_USAGE, false,
                             &created.private_color)
        && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, image.width, image.height, MODEL_USAGE, false, &created.change);
    if (!made) {
      for (vk::NrImage* made_image : {&created.a, &created.b, &created.private_color, &created.change}) {
        vk::DestroyNrImage(functions_, device_, made_image);
      }
      LogAllocationFailure(canvas);
      return {.reason = "out of memory"};
    }
    allocation_failure_logged_ = false;
    intermediates_ = std::move(created);
  }
  // The motion images, made at the first frame that needs each, at the canvas size, and freed with the set.
  const bool motion_failed =
      (use_zero_motion && intermediates_.zero_motion.image == VK_NULL_HANDLE
       && !vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, canvas.width, canvas.height, MOTION_USAGE, false, &intermediates_.zero_motion))
      || (copy_motion && intermediates_.canvas_motion.image == VK_NULL_HANDLE
          && !vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, canvas.width, canvas.height, MOTION_USAGE, false,
                                &intermediates_.canvas_motion));
  if (motion_failed) {
    LogAllocationFailure(canvas);
    return {.reason = "out of memory"};
  }
  const bool reduced = (image != size);
  const bool look_ready = EnsureLook(LookPlanFor(image, reduced));  // before the fit, like the set; I-1: C stays the main set's
  nr::FrameInputs frame_inputs = inputs;
  PrepareFaces(canvas, controls, &frame_inputs);  // Keep faces: its surfaces before the fit and the opening, like the look's
  slot_marks_[slot] = timeline_.MarkNow();
  intermediates_.last_use = slot_marks_[slot];
  // Fix round 1 (C1): OpenRecording below transitions the look's and Keep faces' images whenever they exist, whether or not NR then runs and uses them, so
  // this recording is their last use too.
  if (look_.plan != LookPlan{}) {
    look_.last_use = slot_marks_[slot];
  }
  if (faces_.twin.image != VK_NULL_HANDLE) {
    faces_.last_use = slot_marks_[slot];
  }
  next_slot_ = (slot + 1u) % color::VkColorPipeline::RING_SLOTS;

  const float input_scale = NoteEncoding(color);
  const uint32_t options = ShaderOptions();
  const ExposureChoice exposure = ChooseExposure(color.encoding, color.exposure, color.exposure_factor);
  const color::VkSampledView game_color = {.view = game.ImageView, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};  // the game's: only read (R79)

  // 1. The opening barrier; A, B, the private colour, the change field, the motion images and the look's surfaces move UNDEFINED -> GENERAL.
  const std::array<const vk::NrImage*, 10> opened = {&intermediates_.a, &intermediates_.b, &intermediates_.private_color, &intermediates_.change,
                                                     &intermediates_.zero_motion, &intermediates_.canvas_motion, &faces_.twin, &faces_.detail,
                                                     &faces_.atlas, &faces_.fill};
  OpenRecording(buffer, opened, true);

  // 2. Encode the region straight from the game's Color, resampled to the work image and mirror-padded into the canvas; no Uplift copy of it, and no
  // transition of it.
  const MotionSource motion_source = ((game_motion && (!in_canvas || motion_readable)) ? MotionSource::DIRECT : MotionSource::NONE);
  const color::VkEncodePass encode = {
      .source = game.ImageView,
      .source_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .source_format = VK_FORMAT_UNDEFINED,
      .model = intermediates_.a.view,
      .motion = (use_zero_motion ? intermediates_.zero_motion.view : VK_NULL_HANDLE),
      .width = image.width,
      .height = image.height,
      .encoding = color.encoding,
      .input_scale = input_scale,
      .exposure = exposure.view,
      .exposure_factor = exposure.factor,
      .region = region,
      .canvas = canvas,
      .options = options,
  };
  if (exposure.metered || exposure.probe) {
    const bool recorded = RecordMeter(buffer, slot,
                                      {.source = game.ImageView,
                                       .source_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       .region = region,
                                       .encoding = color.encoding,
                                       .primaries = (options & color::shader_options::PRIMARIES_MASK),
                                       .input_scale = input_scale},
                                      exposure);
    if (!recorded && exposure.metered) return {.recorded = true, .reason = "encode failed"};  // Plan 17: a failed check only skips its sample
  }
  if (!color_.RecordEncode(buffer, slot, encode)) {
    return {.recorded = true, .reason = "encode failed"};
  }

  // 3. In a canvas the game's motion vectors move into canvas pixels, mirrored like the image (negating the mirrored axis), bound with the canvas as their
  // subrect and a scale of (1, 1). The motion image NGX reads is handed over in SHADER_READ_ONLY_OPTIMAL, as a game's own vectors are (R79).
  if (copy_motion) {
    color_.RecordMotion(buffer, slot,
                        {.source = {.view = nr::VkResourceOf(inputs.motion.resource)->Resource.ImageViewInfo.ImageView,
                                    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         .region = motion_region,
                         .target = intermediates_.canvas_motion.view,
                         .image = image,
                         .canvas = canvas,
                         .scale_x = inputs.motion_scale_x * static_cast<float>(image.width) / static_cast<float>(motion_region.width),
                         .scale_y = inputs.motion_scale_y * static_cast<float>(image.height) / static_cast<float>(motion_region.height)});
    HandToNgx(buffer, intermediates_.canvas_motion);
    frame_inputs.motion = {.resource = nr::AsResource(&intermediates_.canvas_motion.ngx)};
    frame_inputs.motion_scale_x = 1.f;
    frame_inputs.motion_scale_y = 1.f;
  } else if (use_zero_motion) {
    HandToNgx(buffer, intermediates_.zero_motion);
    frame_inputs.motion = {.resource = nr::AsResource(&intermediates_.zero_motion.ngx)};
    frame_inputs.motion_scale_x = 1.f;
    frame_inputs.motion_scale_y = 1.f;
  } else {
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);  // the game's own vectors, bound in place
  }
  frame_inputs.reset_hint = (inputs.reset_hint || intermediates_.reset_pending);
  frame_inputs.later_passes = config_.later_controls;
  frame_inputs.resolver = this;
  resolve_slot_ = slot;
  // 4. NR on the canvas (or the image), with each later pass's own strengths (the resolver).
  const uint64_t held = HeldBytes();
  const nr::EvaluateResult evaluated = session_.Evaluate(
      nr::AsList(buffer),
      {.a = nr::AsResource(&intermediates_.a.ngx), .b = nr::AsResource(&intermediates_.b.ngx), .size = canvas, .surface_bytes = held, .held_intermediate_bytes = held},
      frame_inputs, controls);
  const bool from_session = SessionWroteMessage(evaluated.reason);
  if (evaluated.output == nullptr) {
    return {.recorded = true, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  intermediates_.reset_pending = false;  // Plan 3: only a frame that ran NR pays off the owed reset

  // 5. Plan 14: the change field and the look stage, or NR's own output for the full-size restore. The stabiliser runs on the game's own vectors (`inputs`, not
  // the canvas copy), since it acts on the image, as on Direct3D 12.
  Barrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  const VkImageView nr_output = (evaluated.output == nr::AsResource(&intermediates_.a.ngx) ? intermediates_.a.view : intermediates_.b.view);
  if (faces_ran_ && config_.keep_faces.show_mask) {
    color_.RecordFacesShow(buffer, slot, nr_output, faces_.twin.view, canvas);  // Show the face mask, as after DLSS
    Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }
  const AfterNr after = RecordAfterNr(buffer, slot, encode, nr_output, reduced, look_ready, work.upsampling, StabilizeMotionOf(inputs), frame_inputs.reset_hint);
  if (!after.ok) {
    return {.recorded = true, .reason = "encode failed", .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }

  // 6. The compose into the private colour's region (the game's pixel read from its Color, NR's result or change), then the private colour is handed to DLSS
  // in SHADER_READ_ONLY_OPTIMAL; the same dependency finishes the writes.
  if (!color_.RecordDecode(buffer, slot,
                           {.source = game_color,
                            .target = intermediates_.private_color.view,
                            .nr_output = after.view,
                            .origin_x = region.x,
                            .origin_y = region.y,
                            .width = size.width,
                            .height = size.height,
                            .encoding = color.encoding,
                            .input_scale = input_scale,
                            .transfer_strength = color.transfer_strength,
                            .color_strength = color.color_strength,
                            .exposure = exposure.view,
                            .exposure_factor = exposure.factor,
                            .source_x = region.x,
                            .source_y = region.y,
                            .upsampling = after.upsampling,
                            .options = options,
                            .chroma_clamp = config_.fixes.chroma_clamp,
                            .mask = BoundMask(slot)})) {
    return {.recorded = true, .reason = "decode failed", .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
  }
  HandToNgx(buffer, intermediates_.private_color);
  return {.recorded = true, .nr_applied = true, .passes_run = evaluated.passes_run, .reason = evaluated.reason, .from_session = from_session, .motion_source = motion_source, .motion_scale_x = inputs.motion_scale_x, .motion_scale_y = inputs.motion_scale_y};
}

PresentMotionWrite VkNrPipeline::RecordPresentMotion(VkCommandBuffer buffer, const color::VkSampledView& motion, nr::Rect region, float scale_x,
                                                     float scale_y, uint64_t frame, bool flip_y) {
  const nr::Size size = {region.width, region.height};
  if (buffer == VK_NULL_HANDLE || motion.view == VK_NULL_HANDLE || size.Empty() || frame == 0u || !color_.PrepareMotion()) return PresentMotionWrite::NO_INPUT;
  const uint32_t ring_slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[ring_slot])) return PresentMotionWrite::RING_BUSY;
  // A new size: the ring is made again at its smallest (the old slots retire at their marks). A copy that is still in use is never overwritten: the frame goes
  // without one (its present binds an older copy: PresentMotion).
  if (present_motion_size_ != size || present_motion_[0].image.image == VK_NULL_HANDLE) {
    ReleasePresentMotion();
    if (!MakePresentMotionSlots(0u, PRESENT_MOTION_MIN_SLOTS, size)) return PresentMotionWrite::NO_INPUT;
    present_motion_slots_ = PRESENT_MOTION_MIN_SLOTS;
    present_motion_size_ = size;
    present_motion_bytes_ = present_motion_slots_ * size.Pixels() * MOTION_BYTES_PER_PIXEL;
  }
  PresentMotionSlot& copy = present_motion_[PresentMotionSlotOf(frame, present_motion_slots_)];
  if (!timeline_.IsComplete(copy.last_use)) {
    // Fix round 1 (M6): the GPU runs too far behind for four slots (a lag past 3): the ring grows once, its new slots made beside the first ones (none of those is freed or
    // moved, and the frame mapping change only picks other slots, each still behind its own last use). This frame goes without its copy.
    if (PresentMotionGrows(present_motion_slots_, PresentMotionWrite::SLOT_BUSY)
        && MakePresentMotionSlots(present_motion_slots_, PRESENT_MOTION_SLOTS, size)) {
      nr::Logf(nr::LogLevel::INFO, "Vulkan: the GPU runs too far behind for four slots: DLSS's motion copies for Present grow from {} to {} slots ({:.1f} MiB)",
               present_motion_slots_, PRESENT_MOTION_SLOTS,
               static_cast<double>(PRESENT_MOTION_SLOTS * size.Pixels() * MOTION_BYTES_PER_PIXEL) / (1024.0 * 1024.0));
      present_motion_slots_ = PRESENT_MOTION_SLOTS;
      present_motion_bytes_ = present_motion_slots_ * size.Pixels() * MOTION_BYTES_PER_PIXEL;
    }
    return PresentMotionWrite::SLOT_BUSY;
  }
  // The GPU reads this ring slot's constants until this recording completes: the current frame, and the token the caller issued before recording.
  slot_marks_[ring_slot] = timeline_.MarkNow();
  next_slot_ = (ring_slot + 1u) % color::VkColorPipeline::RING_SLOTS;
  // The slot's contents are rewritten in full: UNDEFINED -> GENERAL, the motion copy at the region's own size (an image = canvas = region copy), then handed over
  // in SHADER_READ_ONLY_OPTIMAL for the bridge's copy-in.
  const std::array<const vk::NrImage*, 1> opened = {&copy.image};
  OpenRecording(buffer, opened, false);
  color_.RecordMotion(buffer, ring_slot,
                      {.source = motion, .region = region, .target = copy.image.view, .image = size, .canvas = size, .scale_x = scale_x, .scale_y = scale_y,
                       .flip_y = flip_y});
  HandToNgx(buffer, copy.image);
  copy.frame = frame;
  copy.last_use = slot_marks_[ring_slot];
  return PresentMotionWrite::WRITTEN;
}

std::array<uint64_t, VkNrPipeline::PRESENT_MOTION_SLOTS> VkNrPipeline::PresentMotionFrames() const {
  std::array<uint64_t, PRESENT_MOTION_SLOTS> frames = {};
  for (size_t index = 0u; index < PRESENT_MOTION_SLOTS; ++index) {
    frames[index] = (present_motion_[index].image.image != VK_NULL_HANDLE ? present_motion_[index].frame : 0u);
  }
  return frames;
}

vk::NrImage VkNrPipeline::PresentMotion(uint64_t frame, uint64_t* age) {
  const std::array<uint64_t, PRESENT_MOTION_SLOTS> frames = PresentMotionFrames();
  const PresentMotionPick pick = PickPresentMotion(frames, frame);
  if (!pick.slot) return {};
  PresentMotionSlot& copy = present_motion_[*pick.slot];
  copy.last_use = timeline_.MarkNow();  // NR or the bridge's copy-in reads it in this frame
  if (age != nullptr) {
    *age = pick.age;
  }
  return copy.image;
}

bool VkNrPipeline::HasPresentMotion(uint64_t frame) const {
  const std::array<uint64_t, PRESENT_MOTION_SLOTS> frames = PresentMotionFrames();
  return PickPresentMotion(frames, frame).slot.has_value();
}

bool VkNrPipeline::MakePresentMotionSlots(size_t first, size_t end, nr::Size size) {
  bool made = true;
  for (size_t index = first; index < end; ++index) {
    made = made && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, size.width, size.height, PRESENT_MOTION_USAGE, false,
                                     &present_motion_[index].image);
  }
  if (!made) {
    for (size_t index = first; index < end; ++index) {
      vk::DestroyNrImage(functions_, device_, &present_motion_[index].image);  // never used: none was handed to a recording
      present_motion_[index] = {};
    }
    LogAllocationFailure(size);
    return false;
  }
  allocation_failure_logged_ = false;
  return true;
}

void VkNrPipeline::ReleasePresentMotion() {
  for (const PresentMotionSlot& slot : present_motion_) {
    RetireReshadeImage(slot.image, slot.last_use);  // the bridge's copy-in reads it on ReShade's queue
  }
  present_motion_ = {};
  present_motion_slots_ = PRESENT_MOTION_MIN_SLOTS;  // fix round 1 (M6): the next copies start small again
  present_motion_size_ = {};
  present_motion_bytes_ = 0u;
}

bool VkNrPipeline::ConvertLaunchpadMotion(VkCommandBuffer buffer, const VkPresentTarget& target, const WorkLayout& work) {
  const nr::Rect region = {.x = 0u, .y = 0u, .width = target.launchpad_size.width, .height = target.launchpad_size.height};
  if (target.launchpad_motion.view == VK_NULL_HANDLE || !MotionFormatReadable(target.launchpad_format) || region.width == 0u || region.height == 0u
      || work.image.Empty() || work.canvas.Empty() || !color_.PrepareMotion()) {
    return false;
  }
  const uint32_t slot = next_slot_;
  if (!timeline_.IsComplete(slot_marks_[slot])) return false;
  if (present_.launchpad.image == VK_NULL_HANDLE || present_.launchpad_size != work.canvas) {
    RetirePresentLaunchpad();
    if (!vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, work.canvas.width, work.canvas.height, MOTION_USAGE, false,
                           &present_.launchpad)) {
      present_.launchpad = {};
      LogAllocationFailure(work.canvas);
      return false;
    }
    present_.launchpad_size = work.canvas;
    present_.launchpad_bytes = work.canvas.Pixels() * MOTION_BYTES_PER_PIXEL;
  }
  slot_marks_[slot] = timeline_.MarkNow();
  present_.launchpad_last_use = slot_marks_[slot];
  next_slot_ = (slot + 1u) % color::VkColorPipeline::RING_SLOTS;
  // Its contents are rewritten in full: UNDEFINED -> GENERAL, the copy, then handed to NGX in SHADER_READ_ONLY_OPTIMAL as a game's own vectors are (R79).
  // UPLIFT_MV holds back-buffer pixels: times MotionScale, and times image / region into the work image's pixels (Resolution below Full), as Direct3D 12's.
  const std::array<const vk::NrImage*, 1> opened = {&present_.launchpad};
  OpenRecording(buffer, opened, false);
  const bool recorded = color_.RecordMotion(buffer, slot,
                                            {.source = target.launchpad_motion,
                                             .region = region,
                                             .target = present_.launchpad.view,
                                             .image = work.image,
                                             .canvas = work.canvas,
                                             .scale_x = target.motion_scale_x * static_cast<float>(work.image.width) / static_cast<float>(region.width),
                                             .scale_y = target.motion_scale_y * static_cast<float>(work.image.height) / static_cast<float>(region.height)});
  HandToNgx(buffer, present_.launchpad);
  return recorded;
}

const NVSDK_NGX_Resource_VK* VkNrPipeline::ZeroStandIn(VkCommandBuffer buffer, nr::Size size) {
  if (size.Empty()) return nullptr;
  if (present_.stand_in.image == VK_NULL_HANDLE || present_.stand_in_size != size) {
    RetireReshadeImage(present_.stand_in, present_.last_use);
    present_.stand_in = {};
    if (!vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16_SFLOAT, size.width, size.height,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, &present_.stand_in)) {
      present_.stand_in = {};
      LogAllocationFailure(size);
      return nullptr;
    }
    present_.stand_in_size = size;
  }
  // Rewritten in full at each use: UNDEFINED -> TRANSFER_DST, zeros, then SHADER_READ_ONLY_OPTIMAL as a game's own vectors are handed over (R79).
  const VkImageMemoryBarrier to_clear =
      ImageTransition(present_.stand_in.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &to_clear);
  const VkClearColorValue zero = {};
  functions_.vkCmdClearColorImage(buffer, present_.stand_in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1u, &COLOR_RANGE);
  const VkImageMemoryBarrier to_read = ImageTransition(present_.stand_in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &to_read);
  return &present_.stand_in.ngx;
}

PipelineResult VkNrPipeline::RecordPresent(VkCommandBuffer buffer, const VkPresentTarget& target, const nr::Controls& controls, bool reset_hint,
                                           const WorkLayout& layout) {
  if (session_.State() != nr::SessionState::ACTIVE) return {.reason = "inactive"};
  const uint64_t bytes_per_pixel = PresentBytesPerPixel(target.copy_format);
  if (buffer == VK_NULL_HANDLE || target.image == VK_NULL_HANDLE || target.size.Empty() || bytes_per_pixel == 0u) return {.reason = "invalid input"};
  const WorkLayout work = Resolve(layout, target.size);
  // RecordAfterDlss's own checks, made before anything is recorded, so a frame it would skip copies nothing either.
  if (!nr::MeetsNrFloor(target.size) || !nr::MeetsNrFloor(work.canvas)) return {.reason = "frame too small"};
  if (controls.intensity <= 0.f) return {.reason = "intensity 0"};  // exact pass-through: the image is never touched
  if (!color_.Prepare(VK_FORMAT_R16G16B16A16_SFLOAT)) return {.reason = "pipeline failed"};
  const uint32_t following = (next_slot_ + 1u) % color::VkColorPipeline::RING_SLOTS;  // Launchpad's conversion may take the first
  if (!timeline_.IsComplete(slot_marks_[next_slot_]) || !timeline_.IsComplete(slot_marks_[following])) return {.reason = "descriptors busy"};
  if (present_.color.image == VK_NULL_HANDLE || present_.size != target.size || present_.format != target.copy_format) {
    RetirePresent();
    PresentSurfaces created = {.size = target.size, .format = target.copy_format,
                               .bytes = target.size.Pixels() * (bytes_per_pixel + MODEL_BYTES_PER_PIXEL)};
    const bool made = vk::CreateNrImage(functions_, device_, memory_, target.copy_format, target.size.width, target.size.height, PRESENT_STAGING_USAGE,
                                        false, &created.staging)
                      && vk::CreateNrImage(functions_, device_, memory_, VK_FORMAT_R16G16B16A16_SFLOAT, target.size.width, target.size.height,
                                           MODEL_USAGE, true, &created.color);
    if (!made) {
      for (vk::NrImage* image : {&created.staging, &created.color}) {
        vk::DestroyNrImage(functions_, device_, image);
      }
      LogAllocationFailure(target.size);
      return {.reason = "out of memory"};
    }
    allocation_failure_logged_ = false;
    present_ = std::move(created);
  }
  // Both are rewritten in full every frame and used on ReShade's queue alone: the frame's signal there follows this buffer's submission.
  present_.last_use = timeline_.MarkNow();
  const VkImage staging = present_.staging.image;
  const VkImage color = present_.color.image;
  const VkExtent3D extent = {target.size.width, target.size.height, 1u};
  const VkImageCopy copy = {.srcSubresource = COLOR_LAYERS, .dstSubresource = COLOR_LAYERS, .extent = extent};
  const VkImageBlit blit = {
      .srcSubresource = COLOR_LAYERS,
      .srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1}},
      .dstSubresource = COLOR_LAYERS,
      .dstOffsets = {{0, 0, 0}, {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1}},
  };

  // 1. The copy in. A real execution and memory dependency on everything submitted earlier on this queue (the game's frame, and ReShade's effects so far,
  // which the caller flushed): a transition from the present layout alone orders nothing. The image's bits go raw into the staging image (size-compatible
  // formats), whose blit converts them into the RGBA16F intermediate (an sRGB-created image's stored values, never linearised: the staging format is UNORM).
  const VkMemoryBarrier earlier_writes = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
  const std::array<VkImageMemoryBarrier, 2> into_copy = {
      ImageTransition(target.image, target.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
      ImageTransition(staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &earlier_writes, 0u, nullptr,
                                  static_cast<uint32_t>(into_copy.size()), into_copy.data());
  functions_.vkCmdCopyImage(buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
  const std::array<VkImageMemoryBarrier, 2> into_blit = {
      ImageTransition(staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT),
      ImageTransition(color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                  static_cast<uint32_t>(into_blit.size()), into_blit.data());
  functions_.vkCmdBlitImage(buffer, staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_NEAREST);
  const VkImageMemoryBarrier into_nr =
      ImageTransition(color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &into_nr);

  // 2. The motion vectors, as Direct3D 12's Present path chooses them: DLSS's copy for this present, else Launchpad's UPLIFT_MV, else none.
  nr::FrameInputs inputs = {.ui_correction = config_.present_ui_correction, .reset_hint = reset_hint};
  MotionSource motion_source = MotionSource::NONE;
  bool launchpad_failed = false;  // review 97f99e3: NR then skips this frame rather than run its features on another kind of motion input
  if (target.dlss_motion != nullptr) {
    inputs.motion = {.resource = nr::AsResource(target.dlss_motion)};  // the full subrect, a scale of (1, 1)
    motion_source = MotionSource::PRESENT_COPY;
  } else if (!target.dlss_motion_gap_size.Empty()) {
    // Stress round: DLSS's copy is this load's input but missed this present: zeros of the copies' own shape, so NR runs (no skip, no reload).
    if (const NVSDK_NGX_Resource_VK* stand_in = ZeroStandIn(buffer, target.dlss_motion_gap_size); stand_in != nullptr) {
      inputs.motion = {.resource = nr::AsResource(stand_in)};
      motion_source = MotionSource::PRESENT_COPY;
    } else {
      launchpad_failed = true;  // no stand-in could be made: skip rather than change the input's kind
    }
  } else if (target.launchpad_motion.view != VK_NULL_HANDLE) {
    if (ConvertLaunchpadMotion(buffer, target, work)) {
      inputs.motion = {.resource = nr::AsResource(&present_.launchpad.ngx)};
      motion_source = MotionSource::LAUNCHPAD;
    } else if (const NVSDK_NGX_Resource_VK* stand_in = ZeroStandIn(buffer, work.canvas); stand_in != nullptr) {
      // Stress round: a conversion that missed this present (a busy slot) runs on zeros of the conversion's own shape instead of skipping.
      inputs.motion = {.resource = nr::AsResource(stand_in)};
      motion_source = MotionSource::LAUNCHPAD;
    } else {
      launchpad_failed = true;
    }
  }
  if (motion_source != MotionSource::LAUNCHPAD && present_.launchpad.image != VK_NULL_HANDLE) {
    RetirePresentLaunchpad();  // Launchpad's vectors stopped coming: its conversion goes, behind the recordings that used it
  }
  inputs.reset_hint = (inputs.reset_hint || motion_source != present_motion_source_);  // spec §9: a provider switch
  present_motion_source_ = motion_source;

  // 3. NR in place on the intermediate, as on DLSS's Output (the encode, the passes, the change path and the look, the decode), with no game exposure.
  PipelineResult result = (launchpad_failed ? PipelineResult{.reason = "no Launchpad vectors this frame"}
                                            : RecordAfterDlss(buffer,
                                                              {.resource = &present_.color.ngx,
                                                               .encoding = target.encoding,
                                                               .diffuse_white_nits = target.diffuse_white_nits,
                                                               .transfer_strength = target.transfer_strength,
                                                               .color_strength = target.color_strength},
                                                              inputs, controls, layout));
  result.recorded = true;
  result.motion_source = motion_source;

  // 4. The copy back, only when NR applied (otherwise the image keeps its own bits), then the image's return to its layout behind a global dependency: what
  // ReShade records next starts from that layout with no source scope of its own (its present state orders nothing).
  const VkMemoryBarrier later_reads = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
  VkImageLayout image_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (result.nr_applied) {
    const std::array<VkImageMemoryBarrier, 2> out_of_nr = {
        ImageTransition(color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        ImageTransition(staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                    static_cast<uint32_t>(out_of_nr.size()), out_of_nr.data());
    functions_.vkCmdBlitImage(buffer, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit,
                              VK_FILTER_NEAREST);
    const std::array<VkImageMemoryBarrier, 2> into_image = {
        ImageTransition(staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT),
        ImageTransition(target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                                    static_cast<uint32_t>(into_image.size()), into_image.data());
    functions_.vkCmdCopyImage(buffer, staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    image_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }
  const VkImageMemoryBarrier returned =
      ImageTransition(target.image, image_layout, target.layout, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  functions_.vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 1u, &later_reads, 0u, nullptr, 1u,
                                  &returned);
  return result;
}

}  // namespace uplift::sources
