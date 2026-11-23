#pragma once

#include <cstddef>
#include <cstdint>

// HUD-less (design §2): the parts of Streamline's tag structures Uplift reads, hand-written as streamline_present.cpp's
// prefixes are (no Streamline header is vendored). Layouts and values are Streamline 2.14.16's (sl_struct.h,
// sl_core_types.h, sl_consts.h); the structures are append-only ABI, so these prefixes hold for every v2 interposer.
namespace uplift::addon::sl_layout {

struct Guid {
  uint32_t data1 = 0;
  uint16_t data2 = 0;
  uint16_t data3 = 0;
  uint8_t data4[8] = {};
  friend bool operator==(const Guid&, const Guid&) = default;
};

// sl::BaseStructure: next, structType, structVersion.
struct BaseStructure {
  void* next = nullptr;
  Guid type;
  size_t version = 0;
};

// sl::Extent: zero width or height means the whole resource.
struct Extent {
  uint32_t top = 0;
  uint32_t left = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// sl::Resource. Only `native` and `state` are read when the header is not Resource's (Unreal leaves it empty).
struct Resource {
  BaseStructure base;
  char type = 0;  // sl::ResourceType, eTex2d = 0
  void* native = nullptr;
  void* memory = nullptr;
  void* view = nullptr;
  uint32_t state = UINT32_MAX;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t native_format = 0;
  uint32_t mip_levels = 0;
  uint32_t array_layers = 0;
  uint64_t gpu_virtual_address = 0;
  uint32_t flags = 0;
  uint32_t usage = 0;
};

// sl::ResourceTag.
struct ResourceTag {
  BaseStructure base;
  Resource* resource = nullptr;
  uint32_t type = 0;   // sl::BufferType
  int lifecycle = 0;   // sl::ResourceLifecycle, a plain enum
  Extent extent;
};

inline constexpr Guid RESOURCE_TYPE = {0x3a9d70cfu, 0x2418u, 0x4b72u, {0x83u, 0x91u, 0x13u, 0xf8u, 0x72u, 0x1cu, 0x72u, 0x61u}};
inline constexpr Guid RESOURCE_TAG_TYPE = {0x4c6a5aadu, 0xb445u, 0x496cu, {0x87u, 0xffu, 0x1au, 0xf3u, 0x84u, 0x5bu, 0xe6u, 0x53u}};

inline constexpr uint32_t HUDLESS_COLOR = 2u;        // kBufferTypeHUDLessColor
inline constexpr uint32_t UI_COLOR_AND_ALPHA = 23u;  // kBufferTypeUIColorAndAlpha
inline constexpr uint32_t UI_ALPHA = 69u;            // kBufferTypeUIAlpha

inline constexpr int ONLY_VALID_NOW = 0;
inline constexpr int VALID_UNTIL_PRESENT = 1;
inline constexpr int VALID_UNTIL_EVALUATE = 2;

inline constexpr uint32_t UNKNOWN_STATE = UINT32_MAX;
// sl::ViewportHandle is BaseStructure then its uint32_t id.
inline constexpr size_t VIEWPORT_ID_OFFSET = 32u;

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(BaseStructure) == 32u);
static_assert(offsetof(Resource, type) == 32u && offsetof(Resource, native) == 40u && offsetof(Resource, memory) == 48u);
static_assert(offsetof(Resource, view) == 56u && offsetof(Resource, state) == 64u && offsetof(Resource, width) == 68u);
static_assert(offsetof(Resource, height) == 72u && offsetof(Resource, native_format) == 76u && offsetof(Resource, mip_levels) == 80u);
static_assert(offsetof(Resource, array_layers) == 84u && offsetof(Resource, gpu_virtual_address) == 88u);
static_assert(offsetof(Resource, flags) == 96u && offsetof(Resource, usage) == 100u && sizeof(Resource) == 104u);
static_assert(offsetof(ResourceTag, resource) == 32u && offsetof(ResourceTag, type) == 40u && offsetof(ResourceTag, lifecycle) == 44u);
static_assert(offsetof(ResourceTag, extent) == 48u && sizeof(ResourceTag) == 64u);
#endif

}  // namespace uplift::addon::sl_layout
