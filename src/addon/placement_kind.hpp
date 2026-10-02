#pragma once

#include <cstdint>

namespace uplift::addon {

// Where NR runs. In its own header so the 32-bit add-on, which has no NGX headers, can read the 64-bit helper's placement (ipc::Status::placement).
enum class Placement : uint8_t {
  NONE,              // no NR this session: `reason` says why
  PRESENT,           // spec §8.1
  AFTER_DLSS,        // v2 design §3.4
  BEFORE_UPSCALING,  // v2 design §3.9: NR on the render image before DLSS-SR upscales it
};

}  // namespace uplift::addon
