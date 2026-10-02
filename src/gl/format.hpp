#pragma once

#include <dxgiformat.h>

#include <string>

#include "gl/functions.hpp"
#include "gl/game_host.hpp"

namespace uplift::gl {

// The family's share (design §3.5). ReShade's GL labels are not the memory layout: a BGRA label sits on GL_RGBA8 memory, the intermediate is typed, sRGB is
// never reported. So every 8-bit RGBA, BGRA or X8 label (typeless, unorm or srgb) shares as R8G8B8A8_UNORM, never BGRA; R10G10B10A2 and R16G16B16A16 by
// their family; UNKNOWN for anything else.
DXGI_FORMAT SharedFormatOf(DXGI_FORMAT reported);

// The GL internal format of a shared texture's DXGI format: RGBA8, RGB10_A2, RGBA16F and the mask and motion formats (R8, R16F, R32F, RG16F); 0 otherwise.
GLenum InternalFormatOf(DXGI_FORMAT shared);

// Why `frame` cannot reach NR (design §7's texts); empty when it can: an unsupported format, or multisampled when `at_present` (a blit cannot read or write a
// multisampled framebuffer; at the technique and after the effects the frame is ReShade's single-sampled intermediate). A context without the memory-object
// extensions is Functions::Load's `missing`, the device card's error.
[[nodiscard]] std::string BackBufferProblem(const ImageInfo& frame, bool at_present);

}  // namespace uplift::gl
