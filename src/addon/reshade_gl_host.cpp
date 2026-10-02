#include "addon/reshade_gl_host.hpp"

#include <dxgi.h>
#include <wrl/client.h>

#include <optional>

#include "addon/environment.hpp"
#include "gl/state_guard.hpp"

namespace uplift::addon {
namespace {

namespace api = reshade::api;

}  // namespace

gl::ImageInfo GlImageInfo(api::device* device, api::resource resource) {
  if (resource.handle == 0u) return {};
  const api::resource_desc description = device->get_resource_desc(resource);
  return {
      .handle = resource.handle,
      .size = {description.texture.width, description.texture.height},
      .format = static_cast<DXGI_FORMAT>(description.texture.format),
      .samples = description.texture.samples,
  };
}

bool GlAdapterLuid(const gl::Functions& gl, LUID* luid, std::string* error) {
  const auto* const vendor = reinterpret_cast<const char*>(gl.GetString(GL_VENDOR));
  if (vendor == nullptr || std::string_view(vendor).find("NVIDIA") == std::string_view::npos) {
    *error = "Uplift needs an NVIDIA GPU; this game renders on another adapter";
    return false;
  }
  bool found = false;
  if (gl.GetUnsignedBytevEXT != nullptr) {
    LUID native = {};
    gl.GetUnsignedBytevEXT(gl::GL_DEVICE_LUID_EXT, reinterpret_cast<GLubyte*>(&native));
    for (int drained = 0; drained < 16 && gl.GetError() != GL_NO_ERROR; ++drained) {
      // A driver that refuses the query leaves GL_INVALID_ENUM in the game's flag: drained here, as around an import (design §3.1), once per device.
    }
    if (native.LowPart != 0u || native.HighPart != 0) {
      *luid = native;
      found = true;
    }
  }
  if (!found) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description = {};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      for (UINT index = 0u; !found && SUCCEEDED(factory->EnumAdapters1(index, &adapter)); ++index) {
        if (SUCCEEDED(adapter->GetDesc1(&description)) && description.VendorId == NVIDIA_VENDOR_ID) {
          *luid = description.AdapterLuid;
          found = true;
        }
      }
    }
  }
  if (!found) {
    *error = "the game's adapter could not be identified";
  }
  return found;
}

RuntimeContext CheckRuntimeContext(api::command_queue* queue, api::command_queue* present_queue) {
  if (queue == nullptr || queue->get_immediate_command_list() == nullptr) return RuntimeContext::NONE;
  if (present_queue != nullptr && present_queue != queue) return RuntimeContext::OTHER;
  const HGLRC current = gl::CurrentContext();
  if (current == nullptr) return RuntimeContext::NONE;  // a present with no context current is not "another OpenGL context" (R63)
  return (current == reinterpret_cast<HGLRC>(static_cast<uintptr_t>(queue->get_native())) ? RuntimeContext::CURRENT : RuntimeContext::OTHER);
}

ReshadeGlHost::ReshadeGlHost(const gl::Functions& gl, api::command_queue* queue, api::command_queue* present_queue)
    : gl_(gl),
      queue_(queue),
      list_(queue != nullptr ? queue->get_immediate_command_list() : nullptr),
      context_(CheckRuntimeContext(queue, present_queue)) {}

void ReshadeGlHost::Copy(const gl::ImageInfo& source, const gl::ImageInfo& destination) {
  if (!Valid()) return;
  std::optional<gl::DefaultFramebufferGuard> guard;
  if (source.DefaultFramebuffer() || destination.DefaultFramebuffer()) {
    guard.emplace(gl_);
  }
  list_->copy_texture_region(api::resource{source.handle}, 0u, nullptr, api::resource{destination.handle}, 0u, nullptr);
}

void ReshadeGlHost::TextureDeleted(GLuint name) {
  if (!Valid()) return;
  // (GL_TEXTURE_2D << 40) | name without the standalone bit (bit 32): no object is destroyed, no GL call is made and no event is raised; ReShade only bumps
  // its FBO cache's version, which makes the next blit through a cached FBO build a fresh one.
  queue_->get_device()->destroy_resource_view(api::resource_view{(uint64_t{GL_TEXTURE_2D} << 40u) | name});
}

}  // namespace uplift::addon
