#include "gl/state_guard.hpp"

namespace uplift::gl {

DefaultFramebufferGuard::DefaultFramebufferGuard(const Functions& gl) : gl_(gl) {
  gl_.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_binding_);
  gl_.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_binding_);
  gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0u);  // FB0's read and draw buffer are its own state: readable only while it is bound
  gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0u);
  gl_.GetIntegerv(GL_READ_BUFFER, &read_buffer_);
  gl_.GetIntegerv(GL_DRAW_BUFFER, &draw_buffer_);
  gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_binding_));
  gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_binding_));
  discard_ = (gl_.IsEnabled(GL_RASTERIZER_DISCARD) != GL_FALSE);
  if (discard_) {
    gl_.Disable(GL_RASTERIZER_DISCARD);
  }
}

DefaultFramebufferGuard::~DefaultFramebufferGuard() {
  // ReShade's copy restored the bindings, the scissor test and sRGB, and left FB0's read or draw buffer at GL_BACK.
  gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0u);
  gl_.ReadBuffer(static_cast<GLenum>(read_buffer_));
  gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0u);
  gl_.DrawBuffer(static_cast<GLenum>(draw_buffer_));
  gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_binding_));
  gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_binding_));
  if (discard_) {
    gl_.Enable(GL_RASTERIZER_DISCARD);
  }
}

}  // namespace uplift::gl
