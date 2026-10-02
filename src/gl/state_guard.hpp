#pragma once

#include "gl/functions.hpp"

namespace uplift::gl {

// Design §3.3 (R64), proven by Plan 12 Task 1's G-STATE: around every copy where one side is the default framebuffer, i.e. only at the present event, where
// changed GL state persists as the game's own (ReShade captures it there). ReShade's copy restores the READ and DRAW framebuffer bindings, the scissor test
// and GL_FRAMEBUFFER_SRGB itself (opengl_impl_command_list.cpp:1649-1696) and leaves FB0's read and draw buffer at GL_BACK; it does not know
// GL_RASTERIZER_DISCARD, which would swallow the blit.
//
// The guard saves the two bindings and FB0's read and draw buffer (readable only while FB0 is bound), turns GL_RASTERIZER_DISCARD off, and puts all of it
// back. The probe measured the rest: ReShade alone already loses a GL_FRONT draw buffer (it comes back GL_BACK) and, on some runs, the colour mask, so the
// state a host sees after SwapBuffers is compared with ReShade's own baseline, not with what the host set.
class DefaultFramebufferGuard {
 public:
  explicit DefaultFramebufferGuard(const Functions& gl);
  DefaultFramebufferGuard(const DefaultFramebufferGuard&) = delete;
  DefaultFramebufferGuard& operator=(const DefaultFramebufferGuard&) = delete;
  ~DefaultFramebufferGuard();

 private:
  const Functions& gl_;
  GLint read_binding_ = 0;
  GLint draw_binding_ = 0;
  GLint read_buffer_ = 0;
  GLint draw_buffer_ = 0;
  bool discard_ = false;
};

}  // namespace uplift::gl
