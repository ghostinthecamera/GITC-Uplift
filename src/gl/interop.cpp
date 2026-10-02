#include "gl/interop.hpp"

#include <algorithm>
#include <array>
#include <chrono>

#include "gl/format.hpp"

namespace uplift::gl {
namespace {

// A semaphore operation names at most the colour, the motion and the mask.
constexpr size_t MAX_LISTED_TEXTURES = 4u;

}  // namespace

ImportResult Interop::ImportTexture(HANDLE nt_handle, uint64_t bytes, DXGI_FORMAT shared_format, nr::Size size) {
  ImportResult out;
  const GLenum internal_format = InternalFormatOf(shared_format);
  if (internal_format == 0u) {
    out.error = GL_INVALID_ENUM;
    out.step = "the format";
    return out;
  }
  for (int drained = 0; drained < 16 && gl_.GetError() != GL_NO_ERROR; ++drained) {
    // Clears errors an earlier step left, so the ones below are this import's.
  }
  SharedTexture texture = {.size = size, .format = shared_format};
  const auto fail = [&](GLenum error, const char* step) {
    // The one delete outside FlushPending, and it needs no TextureDeleted: this name was never copied through ReShade (the import failed before the first
    // Copy), so no blit FBO is cached for it. Every texture that was ever copied is deleted by FlushPending, which announces it.
    if (texture.texture != 0u) {
      gl_.DeleteTextures(1, &texture.texture);
    }
    if (texture.memory != 0u) {
      gl_.DeleteMemoryObjectsEXT(1, &texture.memory);
    }
    for (int drained = 0; drained < 16 && gl_.GetError() != GL_NO_ERROR; ++drained) {
      // Whatever the failed import left is not the game's.
    }
    out.error = error;
    out.step = step;
    return out;
  };
  gl_.CreateMemoryObjectsEXT(1, &texture.memory);
  const GLint dedicated = GL_TRUE;
  gl_.MemoryObjectParameterivEXT(texture.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
  gl_.ImportMemoryWin32HandleEXT(texture.memory, bytes, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, nt_handle);
  if (const GLenum error = gl_.GetError(); error != GL_NO_ERROR) return fail(error, "glImportMemoryWin32HandleEXT");
  gl_.CreateTextures(GL_TEXTURE_2D, 1, &texture.texture);
  gl_.TextureStorageMem2DEXT(texture.texture, 1, internal_format, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height), texture.memory, 0u);
  if (const GLenum error = gl_.GetError(); error != GL_NO_ERROR) return fail(error, "glTextureStorageMem2DEXT");
  out.texture = texture;
  return out;
}

GLenum Interop::ImportSemaphore(HANDLE nt_handle, GLuint* semaphore) {
  for (int drained = 0; drained < 16 && gl_.GetError() != GL_NO_ERROR; ++drained) {
    // As ImportTexture.
  }
  gl_.GenSemaphoresEXT(1, semaphore);
  gl_.ImportSemaphoreWin32HandleEXT(*semaphore, GL_HANDLE_TYPE_D3D12_FENCE_EXT, nt_handle);
  const GLenum error = gl_.GetError();
  if (error != GL_NO_ERROR) {
    DeleteSemaphore(semaphore);
    for (int drained = 0; drained < 16 && gl_.GetError() != GL_NO_ERROR; ++drained) {
      // The failed import's own errors.
    }
  }
  return error;
}

void Interop::DeleteSemaphore(GLuint* semaphore) {
  if (*semaphore != 0u) {
    gl_.DeleteSemaphoresEXT(1, semaphore);
    gl_.Flush();
  }
  *semaphore = 0u;
}

void Interop::Release(SharedTexture* texture, GameHost& host) {
  if (texture->texture == 0u && texture->memory == 0u) return;
  pending_.push_back({.texture = texture->texture, .memory = texture->memory});
  *texture = {};
  FlushPending(host);
}

void Interop::ReleaseSemaphore(GLuint* semaphore, GameHost& host) {
  if (*semaphore == 0u) return;
  pending_.push_back({.semaphore = *semaphore});
  *semaphore = 0u;
  FlushPending(host);
}

void Interop::FlushPending(GameHost& host) {
  if (pending_.empty() || !host.Valid()) return;
  for (const Pending& pending : pending_) {
    if (pending.texture != 0u) {
      gl_.DeleteTextures(1, &pending.texture);
      host.TextureDeleted(pending.texture);
    }
    if (pending.memory != 0u) {
      gl_.DeleteMemoryObjectsEXT(1, &pending.memory);
    }
    if (pending.semaphore != 0u) {
      gl_.DeleteSemaphoresEXT(1, &pending.semaphore);
    }
  }
  pending_.clear();
  gl_.Flush();  // the driver defers the real release until the queue has passed their last use; the game's own SwapBuffers follows at once
}

void Interop::Forget() {
  pending_.clear();
  sync_ = nullptr;
}

void Interop::Signal(GLuint semaphore, uint64_t value, std::span<const GLuint> textures) {
  std::array<GLenum, MAX_LISTED_TEXTURES> layouts;
  layouts.fill(GL_LAYOUT_GENERAL_EXT);
  const GLuint64 fence_value = value;
  gl_.SemaphoreParameterui64vEXT(semaphore, GL_D3D12_FENCE_VALUE_EXT, &fence_value);
  gl_.SignalSemaphoreEXT(semaphore, 0u, nullptr, static_cast<GLuint>(std::min(textures.size(), layouts.size())), textures.data(), layouts.data());
  gl_.Flush();  // every signal is followed by a flush: the D3D12 side waits for it
}

void Interop::Wait(GLuint semaphore, uint64_t value, std::span<const GLuint> textures) {
  std::array<GLenum, MAX_LISTED_TEXTURES> layouts;
  layouts.fill(GL_LAYOUT_GENERAL_EXT);
  const GLuint64 fence_value = value;
  gl_.SemaphoreParameterui64vEXT(semaphore, GL_D3D12_FENCE_VALUE_EXT, &fence_value);
  gl_.WaitSemaphoreEXT(semaphore, 0u, nullptr, static_cast<GLuint>(std::min(textures.size(), layouts.size())), textures.data(), layouts.data());
}

bool Interop::Busy() {
  return sync_ != nullptr && gl_.ClientWaitSync(sync_, 0u, 0u) == GL_TIMEOUT_EXPIRED;
}

WaitResult Interop::FlushAndWait(uint32_t timeout_ms) {
  if (sync_ != nullptr) {
    const GLenum status = gl_.ClientWaitSync(sync_, 0u, 0u);
    if (status == GL_TIMEOUT_EXPIRED) return WaitResult::BUSY;
    gl_.DeleteSync(sync_);
    sync_ = nullptr;
    if (status == GL_WAIT_FAILED) return WaitResult::FAILED;
  }
  sync_ = gl_.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
  if (sync_ == nullptr) return WaitResult::FAILED;
  gl_.Flush();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    const GLenum status = gl_.ClientWaitSync(sync_, GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000u);  // 1 ms slices
    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
      gl_.DeleteSync(sync_);
      sync_ = nullptr;
      return WaitResult::DONE;
    }
    if (status == GL_WAIT_FAILED) {
      gl_.DeleteSync(sync_);
      sync_ = nullptr;
      return WaitResult::FAILED;
    }
    if (std::chrono::steady_clock::now() >= deadline) return WaitResult::TIMEOUT;
  }
}

}  // namespace uplift::gl
