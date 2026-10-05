#pragma once

#include <d3d12.h>

#include <cstdint>

namespace uplift::nr {

struct Size {
  uint32_t width = 0u;
  uint32_t height = 0u;

  [[nodiscard]] constexpr uint64_t Pixels() const { return static_cast<uint64_t>(width) * height; }
  [[nodiscard]] constexpr bool Empty() const { return width == 0u || height == 0u; }
  friend constexpr bool operator==(Size, Size) = default;
};

// The NR floor (spec §6.2): no NR network runs on a frame whose shorter side is below 360 or whose
// longer side is below 640. A 64x64 frame hung the GPU in Plan 2; 1.1.5's probes (RTX 4090, NR 310.8, 1000 frames of
// two chained passes with motion vectors each) ran clean at 640x360, 640x480 and 854x480. It was 1280x720 until then.
// Set at build time (CMake: UPLIFT_NR_MIN_SHORT_SIDE, UPLIFT_NR_MIN_LONG_SIDE); the defaults below are for a translation unit built without them.
#ifndef UPLIFT_NR_MIN_SHORT_SIDE
#define UPLIFT_NR_MIN_SHORT_SIDE 360
#endif
#ifndef UPLIFT_NR_MIN_LONG_SIDE
#define UPLIFT_NR_MIN_LONG_SIDE 640
#endif
#define UPLIFT_NR_FLOOR_STRING_(value) #value
#define UPLIFT_NR_FLOOR_STRING(value) UPLIFT_NR_FLOOR_STRING_(value)
// "640x360" (landscape: longer side first), a string literal for user-facing texts that name the floor.
#define UPLIFT_NR_FLOOR_TEXT UPLIFT_NR_FLOOR_STRING(UPLIFT_NR_MIN_LONG_SIDE) "x" UPLIFT_NR_FLOOR_STRING(UPLIFT_NR_MIN_SHORT_SIDE)
inline constexpr uint32_t MIN_NR_SHORT_SIDE = UPLIFT_NR_MIN_SHORT_SIDE;
inline constexpr uint32_t MIN_NR_LONG_SIDE = UPLIFT_NR_MIN_LONG_SIDE;
static_assert(MIN_NR_SHORT_SIDE > 0u && MIN_NR_SHORT_SIDE <= MIN_NR_LONG_SIDE, "the NR floor's shorter side must be positive and at most its longer side");

[[nodiscard]] constexpr bool MeetsNrFloor(Size size) {
  const uint32_t shorter = (size.width < size.height ? size.width : size.height);
  const uint32_t longer = (size.width < size.height ? size.height : size.width);
  return shorter >= MIN_NR_SHORT_SIDE && longer >= MIN_NR_LONG_SIDE;
}

// All zero means "the whole resource".
struct Rect {
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
};

struct BoundResource {
  ID3D12Resource* resource = nullptr;
  Rect rect = {};
};

// Per-frame quality controls. None of them rebuild a feature.
struct Controls {
  float intensity = 1.f;
  float local_tone = 1.f;
  float local_structure = 1.f;
  float global_tone = 1.f;     // forwarded; the 310.8 runtime never reads it
  bool auto_mask = true;
  float skin_structure = 1.f;  // -1 = same as local_structure
  uint32_t style = 0u;         // 0..2 = Model A/B/C
};

}  // namespace uplift::nr
