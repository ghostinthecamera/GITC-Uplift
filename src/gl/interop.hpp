#pragma once

#include <Windows.h>

#include <dxgiformat.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "gl/functions.hpp"
#include "gl/game_host.hpp"
#include "nr/types.hpp"

namespace uplift::gl {

// A Direct3D 12 texture imported into the context's share group: a memory object on the D3D12 allocation and a texture made on it. Nothing is mapped into
// the process, and Uplift makes no FBO of its own (FBOs are per context; ReShade's copy makes its own).
struct SharedTexture {
  GLuint texture = 0u;
  GLuint memory = 0u;
  nr::Size size;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  // ReShade's handle for the texture, (GL_TEXTURE_2D << 40) | name.
  [[nodiscard]] uint64_t Handle() const { return (uint64_t{GL_TEXTURE_2D} << 40u) | texture; }
};

struct ImportResult {
  SharedTexture texture;  // valid only when `error` is GL_NO_ERROR
  GLenum error = GL_NO_ERROR;
  const char* step = "";  // the call that set it
};

enum class WaitResult {
  DONE,
  TIMEOUT,  // the cap ran out: skip this frame (Plan 9 M-2). A single one never latches; the bridge stops after a few in a row
  BUSY,     // an earlier wait is still in flight and nothing was submitted or waited for: skip this frame, never latch
  FAILED,   // the driver's wait reported a failure
};

// Plan 12 (OpenGL design §3.4, §3.6): one context share group's imports, semaphores and the capped CPU wait. ReShade-free. Every call is made with the
// runtime's context current, which the caller has checked (GameHost::Valid()). Nothing in it waits on the GPU except FlushAndWait, which is capped.
//
// Frees: GL defers the real release until the queue has passed a name's last use, so Release is a delete and a glFlush, with no wait. A delete needs the
// context (key decision g): while the host is invalid the names wait in `pending_`, and FlushPending deletes them at the next valid frame. After every
// texture delete the host is told (GameHost::TextureDeleted), so ReShade's blit FBO cache forgets the name. Forget is destroy_device: every name is dropped
// without a GL call (they die with the share group).
// Not thread-safe: the caller's lock.
class Interop {
 public:
  explicit Interop(Functions functions) : gl_(std::move(functions)) {}
  Interop(const Interop&) = delete;
  Interop& operator=(const Interop&) = delete;
  // Frees nothing, and makes no GL call: a caller that still holds names runs Release (valid host) or Forget first.
  ~Interop() = default;

  [[nodiscard]] const Functions& Gl() const { return gl_; }

  // `nt_handle`: a Direct3D 12 texture's NT handle (the caller closes it); `bytes`: its allocation size (GetResourceAllocationInfo). Dedicated import, then
  // glCreateTextures and glTextureStorageMem2DEXT. Drains glGetError first (import time only, the game's own flag is not read per frame) and leaves nothing
  // behind on failure.
  ImportResult ImportTexture(HANDLE nt_handle, uint64_t bytes, DXGI_FORMAT shared_format, nr::Size size);
  // A Direct3D 12 fence's NT handle into a new semaphore (the caller closes the handle): the GL error, GL_NO_ERROR on success; nothing is left behind on failure.
  GLenum ImportSemaphore(HANDLE nt_handle, GLuint* semaphore);
  // The semaphore goes at once (glDeleteSemaphoresEXT, glFlush) and is emptied.
  void DeleteSemaphore(GLuint* semaphore);

  // Empties `texture` into `pending_` and deletes what can be deleted now.
  void Release(SharedTexture* texture, GameHost& host);
  // The same for a semaphore (the 32-bit client's imported fences, which a new transport or the helper's exit replaces while the game runs): emptied into
  // `pending_` and deleted now while the host is valid, else at the next valid frame.
  void ReleaseSemaphore(GLuint* semaphore, GameHost& host);
  // Deletes (and flushes, and tells `host` about) every pending name while the host is valid.
  void FlushPending(GameHost& host);
  // destroy_device: every name dropped, no GL call.
  void Forget();

  // One semaphore operation, as the draft's probe measured it (bit-exact): the fence value, then the call with `textures` listed in GL_LAYOUT_GENERAL_EXT.
  // Signal also flushes.
  void Signal(GLuint semaphore, uint64_t value, std::span<const GLuint> textures);
  void Wait(GLuint semaphore, uint64_t value, std::span<const GLuint> textures);

  // A fence sync after everything issued so far, a glFlush, and a glClientWaitSync in 1 ms slices, at most `timeout_ms`. A sync that timed out stays: the
  // next call says BUSY until it has signalled, instead of queueing another behind it.
  WaitResult FlushAndWait(uint32_t timeout_ms);
  // True while an earlier FlushAndWait's sync has timed out and not signalled yet (a poll, no wait): a caller asks before it queues copies behind it.
  [[nodiscard]] bool Busy();

 private:
  struct Pending {
    GLuint texture = 0u;
    GLuint memory = 0u;
    GLuint semaphore = 0u;
  };

  Functions gl_;
  std::vector<Pending> pending_;
  GLsync sync_ = nullptr;
};

}  // namespace uplift::gl
