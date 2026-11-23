#include "addon/hud_tags.hpp"

#include <algorithm>
#include <cstring>

#include <unknwn.h>

#include "addon/sl_tag_layout.hpp"

namespace uplift::addon {
namespace {

// Design §2: the newest frames kept per viewport.
constexpr size_t HUD_FRAMES = 3u;
constexpr uintptr_t NO_FRAME = 0u;

IUnknown* AsUnknown(uint64_t native) { return reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(native)); }

uint32_t ViewportId(const void* viewport) {
  if (viewport == nullptr) return 0u;
  uint32_t id = 0u;
  std::memcpy(&id, static_cast<const uint8_t*>(viewport) + sl_layout::VIEWPORT_ID_OFFSET, sizeof(id));
  return id;
}

TaggedImage Read(const sl_layout::ResourceTag& tag) {
  const sl_layout::Resource& resource = *tag.resource;
  TaggedImage image = {
      .native = reinterpret_cast<uint64_t>(resource.native),
      .state = resource.state,
      .lifecycle = tag.lifecycle,
      .extent_x = tag.extent.left,
      .extent_y = tag.extent.top,
      .extent_w = tag.extent.width,
      .extent_h = tag.extent.height,
  };
  if (resource.base.type == sl_layout::RESOURCE_TYPE) {
    image.described = true;
    image.width = resource.width;
    image.height = resource.height;
    image.format = resource.native_format;
    image.mips = resource.mip_levels;
    image.layers = resource.array_layers;
    image.samples = 1u;  // sl::Resource carries no sample count; Direct3D 12 asks the resource itself
    image.usage = resource.usage;
  }
  return image;
}

}  // namespace

HudTags::~HudTags() { ClearLocked(); }

void HudTags::SetApi(TagApi api) noexcept {
  const std::lock_guard lock(mutex_);
  if (api_ == api) return;
  ClearLocked();
  api_ = api;
}

void HudTags::SetStoring(bool storing) noexcept {
  const std::lock_guard lock(mutex_);
  storing_ = storing;
  if (!storing) ClearLocked();
}

void HudTags::Record(const void* viewport, const void* tags, uint32_t count) noexcept {
  const std::lock_guard lock(mutex_);
  RecordLocked(NO_FRAME, viewport, tags, count);
}

void HudTags::RecordForFrame(const void* frame, const void* viewport, const void* tags, uint32_t count) noexcept {
  // The token arrives as a reference to an abstract Streamline object; a game may pass a null one (MFG Unlocker
  // records a crash from reading it). Test its address through a volatile so the null test is not folded away.
  const void* volatile address = frame;
  if (address == nullptr) return;
  const std::lock_guard lock(mutex_);
  RecordLocked(reinterpret_cast<uintptr_t>(address), viewport, tags, count);
}

void HudTags::RecordLocked(uintptr_t key, const void* viewport, const void* tags, uint32_t count) {
  if (!storing_ || api_ == TagApi::UNKNOWN || tags == nullptr) return;
  const uint32_t viewport_id = ViewportId(viewport);
  const auto* const list = static_cast<const sl_layout::ResourceTag*>(tags);
  for (uint32_t index = 0u; index < count; ++index) {
    const sl_layout::ResourceTag& tag = list[index];
    if (!(tag.base.type == sl_layout::RESOURCE_TAG_TYPE)) continue;
    size_t slot = SLOTS;
    if (tag.type == sl_layout::HUDLESS_COLOR) slot = SLOT_HUDLESS;
    if (tag.type == sl_layout::UI_ALPHA) slot = SLOT_ALPHA;
    if (tag.type == sl_layout::UI_COLOR_AND_ALPHA) slot = SLOT_COLOUR;
    if (slot == SLOTS) continue;
    auto frame = std::ranges::find_if(frames_, [&](const Frame& f) { return f.viewport == viewport_id && f.key == key; });
    if (frame == frames_.end()) {
      frames_.push_back({.viewport = viewport_id, .key = key, .sequence = ++sequence_});
      frame = std::prev(frames_.end());
    }
    const bool empty = (tag.resource == nullptr || tag.resource->native == nullptr);
    ReleaseLocked(frame->images[slot]);
    if (empty) {
      if (slot == SLOT_HUDLESS) ++counts_.empty_hudless;
      continue;
    }
    frame->images[slot] = Read(tag);
    if (api_ == TagApi::D3D12) AsUnknown(frame->images[slot].native)->AddRef();
    if (slot == SLOT_HUDLESS) ++counts_.hudless;
    if (slot == SLOT_ALPHA) ++counts_.opacity;
    if (slot == SLOT_COLOUR) ++counts_.ui_colour;
  }
  // Keep the newest HUD_FRAMES frames of each viewport.
  const auto in_viewport = [&](const Frame& f) { return f.viewport == viewport_id; };
  while (static_cast<size_t>(std::ranges::count_if(frames_, in_viewport)) > HUD_FRAMES) {
    auto oldest = frames_.end();
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
      if (in_viewport(*it) && (oldest == frames_.end() || it->sequence < oldest->sequence)) oldest = it;
    }
    for (TaggedImage& image : oldest->images) ReleaseLocked(image);
    frames_.erase(oldest);
  }
}

void HudTags::ReleaseLocked(TaggedImage& image) noexcept {
  if (image.native != 0u && api_ == TagApi::D3D12) AsUnknown(image.native)->Release();
  image = {};
}

void HudTags::ClearLocked() noexcept {
  for (Frame& frame : frames_) {
    for (TaggedImage& image : frame.images) ReleaseLocked(image);
  }
  frames_.clear();
}

TakeResult HudTags::Take() noexcept {
  const std::lock_guard lock(mutex_);
  TakeResult result;
  const Frame* only = nullptr;
  for (const Frame& frame : frames_) {
    if (frame.images[SLOT_HUDLESS].native == 0u) continue;
    ++result.waiting;
    only = &frame;
  }
  counts_.frames_waiting_max = std::max<uint64_t>(counts_.frames_waiting_max, result.waiting);
  if (result.waiting == 1u) {
    const bool alpha = (only->images[SLOT_ALPHA].native != 0u);
    result.frame = HudFrame{
        .hudless = only->images[SLOT_HUDLESS],
        .opacity = (alpha ? only->images[SLOT_ALPHA] : only->images[SLOT_COLOUR]),
        .opacity_is_ui_colour = (!alpha && only->images[SLOT_COLOUR].native != 0u),
        .viewport = only->viewport,
    };
    result.reason = HudReason::USABLE;
    if (api_ == TagApi::D3D12) {
      // The taken images stay referenced for the caller; every reference the store holds is released below.
      std::vector<IUnknown*> held;
      for (const TaggedImage* image : {&result.frame->hudless, &result.frame->opacity}) {
        if (image->native == 0u) continue;
        AsUnknown(image->native)->AddRef();
        held.push_back(AsUnknown(image->native));
      }
      result.hold = std::shared_ptr<const void>(nullptr, [held](const void*) {
        for (IUnknown* unknown : held) unknown->Release();
      });
    }
  } else if (result.waiting > 1u) {
    result.reason = HudReason::MORE_THAN_ONE_FRAME;
  }
  ClearLocked();
  return result;
}

void HudTags::Clear() noexcept {
  const std::lock_guard lock(mutex_);
  ClearLocked();
}

HudTags::Counts HudTags::Totals() const noexcept {
  const std::lock_guard lock(mutex_);
  return counts_;
}

}  // namespace uplift::addon
