#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

#include "addon/reshade_api.hpp"
#include "gl/functions.hpp"
#include "gl/game_host.hpp"

namespace uplift::addon {

// ReShade's description of an OpenGL image as the ReShade-free side wants it (api::format is DXGI-numbered; the handle is ReShade's own). A null resource is an
// empty description.
[[nodiscard]] gl::ImageInfo GlImageInfo(reshade::api::device* device, reshade::api::resource resource);

// Requires the GL context to be current. GL_VENDOR must identify NVIDIA or the error uses NVIDIA_GPU_REQUIRED_LINE.
// Read GL_DEVICE_LUID_EXT when available, otherwise DXGI's NVIDIA adapter as in VulkanAdapterLuid. Return false with `error` set if no LUID is found.
[[nodiscard]] bool GlAdapterLuid(const gl::Functions& gl, LUID* luid, std::string* error);

// Whether the present event is on the effect runtime's context, decided before any GL call and before gl::Functions exists:
// only the queues and the system wglGetCurrentContext() (gl::CurrentContext), so it is safe on a foreign context. `queue` is the effect runtime's,
// `present_queue` the present event's, null in the effect events.
enum class RuntimeContext {
  NONE,     // no effect runtime yet (or no immediate list), or no context current on this thread: nothing runs, and nothing is said
  CURRENT,  // both are the same queue, and wglGetCurrentContext() is that queue's HGLRC
  OTHER,    // a runtime exists but this is not its context: nothing runs, and the card says so (ReShade itself does not handle it, runtime.cpp:373)
};
[[nodiscard]] RuntimeContext CheckRuntimeContext(reshade::api::command_queue* queue, reshade::api::command_queue* present_queue);

// gl::GameHost on ReShade's API. `queue` is the effect runtime's (on OpenGL its immediate command list is the context
// itself: a command recorded there runs on the spot), `present_queue` the present event's, null in the effect events. Valid only when CheckRuntimeContext says
// CURRENT.
//
// Copy is ReShade's copy_texture_region: raw bits, Y flipped when one side is FB0, through ReShade's per-context FBO cache. Every copy that touches FB0 runs in
// gl::DefaultFramebufferGuard. TextureDeleted invalidates the FBO cache: device::destroy_resource_view on a handle without
// the standalone bit (no GL call, no event) only bumps the cache's version.
class ReshadeGlHost final : public gl::GameHost {
 public:
  ReshadeGlHost(const gl::Functions& gl, reshade::api::command_queue* queue, reshade::api::command_queue* present_queue = nullptr);

  [[nodiscard]] bool Valid() const override { return context_ == RuntimeContext::CURRENT; }
  [[nodiscard]] bool WrongContext() const override { return context_ == RuntimeContext::OTHER; }
  void Copy(const gl::ImageInfo& source, const gl::ImageInfo& destination) override;
  void TextureDeleted(GLuint name) override;

 private:
  const gl::Functions& gl_;
  reshade::api::command_queue* queue_;
  reshade::api::command_list* list_;
  RuntimeContext context_;
};

}  // namespace uplift::addon
