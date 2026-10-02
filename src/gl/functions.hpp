#pragma once

#include <Windows.h>

#include <GL/gl.h>

#include <cstdint>
#include <optional>
#include <string>

// Plan 12 (OpenGL design §3.1, key decision b): the GL game side, ReShade-free, in both halves. Nothing here links opengl32.lib or imports
// opengl32.dll: the Windows SDK's GL/gl.h supplies the GL 1.1 types and macros, every function is resolved at run time, and the constants it lacks
// are local. A struct member is never named like a GL 1.1 export (GetError, not glGetError), so nothing can call the import by mistake.
namespace uplift::gl {

using GLuint64 = uint64_t;
struct SyncObject;  // the driver's GLsync
using GLsync = SyncObject*;

// Values from Khronos gl.xml (as reshade-main\deps\khronos\gl.xml has them). Only those gl.h does not define.
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
inline constexpr GLenum GL_READ_FRAMEBUFFER_BINDING = 0x8CAA;
inline constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
inline constexpr GLenum GL_RASTERIZER_DISCARD = 0x8C89;
inline constexpr GLenum GL_FRAMEBUFFER_DEFAULT = 0x8218;  // the target ReShade puts in a default framebuffer's pseudo handle (handle >> 40)
inline constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
inline constexpr GLenum GL_R8 = 0x8229;
inline constexpr GLenum GL_R16F = 0x822D;
inline constexpr GLenum GL_R32F = 0x822E;
inline constexpr GLenum GL_RG16F = 0x822F;
inline constexpr GLenum GL_RGBA16F = 0x881A;
inline constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
inline constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x1;
inline constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
inline constexpr GLenum GL_TIMEOUT_EXPIRED = 0x911B;
inline constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;
inline constexpr GLenum GL_WAIT_FAILED = 0x911D;
inline constexpr GLenum GL_DEDICATED_MEMORY_OBJECT_EXT = 0x9581;
inline constexpr GLenum GL_HANDLE_TYPE_D3D12_RESOURCE_EXT = 0x958A;
inline constexpr GLenum GL_LAYOUT_GENERAL_EXT = 0x958D;
inline constexpr GLenum GL_HANDLE_TYPE_D3D12_FENCE_EXT = 0x9594;
inline constexpr GLenum GL_D3D12_FENCE_VALUE_EXT = 0x9595;
inline constexpr GLenum GL_DEVICE_LUID_EXT = 0x9599;

// The native GL functions of the context that is current on the calling thread (key decision b): GL 1.1 and wglGetCurrentContext from the exports of
// the SYSTEM opengl32.dll (found with GetModuleHandleW, never loaded: ReShade's export hooks leave it unpatched), and everything later through that
// module's wglGetProcAddress. Every call is made with the runtime's context current, in the present event or an effect event.
//
// Plan 12 Task 1 (design §8.1, G-EV): the functions resolved through wglGetProcAddress are the driver's, which ReShade has detoured, so a call to one
// may raise ReShade events (init_resource, bind_render_targets_and_depth_stencil) into other add-ons, under Uplift's lock. The rule that follows:
// Uplift registers no handler for such an event that takes its lock (it registers none).
struct Functions {
  HGLRC(WINAPI* GetCurrentContext)() = nullptr;
  // GL 1.1, the system module's exports:
  GLenum(APIENTRY* GetError)() = nullptr;
  void(APIENTRY* GetIntegerv)(GLenum, GLint*) = nullptr;
  const GLubyte*(APIENTRY* GetString)(GLenum) = nullptr;
  GLboolean(APIENTRY* IsEnabled)(GLenum) = nullptr;
  void(APIENTRY* Enable)(GLenum) = nullptr;
  void(APIENTRY* Disable)(GLenum) = nullptr;
  void(APIENTRY* ReadBuffer)(GLenum) = nullptr;
  void(APIENTRY* DrawBuffer)(GLenum) = nullptr;
  void(APIENTRY* DeleteTextures)(GLsizei, const GLuint*) = nullptr;
  void(APIENTRY* Flush)() = nullptr;
  // Through the system module's wglGetProcAddress, with the context current:
  void(APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
  const GLubyte*(APIENTRY* GetStringi)(GLenum, GLuint) = nullptr;
  void(APIENTRY* CreateTextures)(GLenum, GLsizei, GLuint*) = nullptr;
  void(APIENTRY* TextureStorageMem2DEXT)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = nullptr;
  void(APIENTRY* CreateMemoryObjectsEXT)(GLsizei, GLuint*) = nullptr;
  void(APIENTRY* DeleteMemoryObjectsEXT)(GLsizei, const GLuint*) = nullptr;
  void(APIENTRY* MemoryObjectParameterivEXT)(GLuint, GLenum, const GLint*) = nullptr;
  void(APIENTRY* ImportMemoryWin32HandleEXT)(GLuint, GLuint64, GLenum, void*) = nullptr;
  void(APIENTRY* GetUnsignedBytevEXT)(GLenum, GLubyte*) = nullptr;  // optional: GL_DEVICE_LUID_EXT
  GLsync(APIENTRY* FenceSync)(GLenum, GLbitfield) = nullptr;
  GLenum(APIENTRY* ClientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
  void(APIENTRY* DeleteSync)(GLsync) = nullptr;
  // The semaphore set (GPU-ordered):
  void(APIENTRY* GenSemaphoresEXT)(GLsizei, GLuint*) = nullptr;
  void(APIENTRY* DeleteSemaphoresEXT)(GLsizei, const GLuint*) = nullptr;
  void(APIENTRY* SemaphoreParameterui64vEXT)(GLuint, GLenum, const GLuint64*) = nullptr;
  void(APIENTRY* ImportSemaphoreWin32HandleEXT)(GLuint, GLenum, void*) = nullptr;
  void(APIENTRY* SignalSemaphoreEXT)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;
  void(APIENTRY* WaitSemaphoreEXT)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;
  // GL_EXT_semaphore and GL_EXT_semaphore_win32 are in the glGetStringi list and their functions resolved (NVIDIA hands out a pointer for every name,
  // so the list decides). False: CPU-ordered, and `cpu_reason` says why (Load's own text; a smoke that forces it writes its own).
  bool semaphores = false;
  std::string cpu_reason;

  // With a context current. nullopt, with `missing` naming the first gap: the system opengl32.dll, GL 4.5's glCreateTextures, or
  // GL_EXT_memory_object / GL_EXT_memory_object_win32 (the texts the card shows after "Uplift could not start on this OpenGL context: ").
  static std::optional<Functions> Load(std::string* missing);
};

// The system opengl32.dll's wglGetCurrentContext on its own: the HGLRC current on the calling thread, null when none is or the system module is not loaded.
// It reads the thread's state and calls no GL function, so it is safe on a context that is not the runtime's; Functions::Load, which does call GL, runs only
// after this has said the context is the runtime's (Plan 12 batch 2 review I-1).
[[nodiscard]] HGLRC CurrentContext();

}  // namespace uplift::gl
