#pragma once

#include <dxgiformat.h>

#include <cstdint>

#include "gl/functions.hpp"
#include "nr/types.hpp"

namespace uplift::gl {

// One image as ReShade describes it on OpenGL: its handle is (target << 40) | name; the default framebuffer is FB0, (GL_FRAMEBUFFER_DEFAULT << 40) | GL_BACK.
struct ImageInfo {
  uint64_t handle = 0u;  // 0: none
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // ReShade's label (design §3.5): not the memory layout
  uint32_t samples = 1u;
  [[nodiscard]] bool DefaultFramebuffer() const { return (handle >> 40u) == GL_FRAMEBUFFER_DEFAULT; }
};

// Design §3.6, R63: the card's text when the present event's context is not the one ReShade's effect runtime runs on.
inline constexpr char WRONG_CONTEXT_PROBLEM[] =
    "The game presents from another OpenGL context than the one ReShade's effects run on; NR stays off";

// What one context's GL work needs from its host. The add-on's host is ReShade's API on the effect runtime's queue (on OpenGL that queue's immediate
// list is the context itself); the smokes' is a native one that copies the way ReShade does. No GL call is ever made while Valid() is false.
// Not thread-safe: the caller's lock.
class GameHost {
 public:
  virtual ~GameHost() = default;
  // Design §3.1: the present event's queue is the effect runtime's, and its HGLRC is current on this thread.
  [[nodiscard]] virtual bool Valid() const = 0;
  // The runtime's queue exists but this is not its context (R63): the card says so. False while there is no runtime yet, where nothing is said.
  [[nodiscard]] virtual bool WrongContext() const = 0;
  // The whole image, raw bits, Y flipped when either side is FB0 (ReShade's copy_texture_region). Every GL state as it was afterwards.
  virtual void Copy(const ImageInfo& source, const ImageInfo& destination) = 0;
  // Right after Uplift deleted the GL texture `name`: ReShade's blit FBO cache is keyed on handle, size and format and does not drop a deleted texture, so
  // the same name coming back at the same size would copy into a stale FBO (Plan 12 Task 1, G-FLIP). The host makes it forget.
  virtual void TextureDeleted(GLuint name) = 0;
};

}  // namespace uplift::gl
