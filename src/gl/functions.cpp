#include "gl/functions.hpp"

#include <algorithm>
#include <atomic>
#include <string_view>
#include <type_traits>
#include <vector>

namespace uplift::gl {
namespace {

// The system opengl32.dll, found with GetModuleHandleW and never loaded (key decision b); null, with `missing` set, when it is not there.
HMODULE SystemOpenGl(std::string* missing) {
  wchar_t directory[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
  if (length == 0u || length >= MAX_PATH) {
    *missing = "GetSystemDirectoryW failed";
    return nullptr;
  }
  const HMODULE system = GetModuleHandleW((std::wstring(directory, length) + L"\\opengl32.dll").c_str());
  if (system == nullptr) {
    *missing = "the system opengl32.dll is not loaded";
  }
  return system;
}

}  // namespace

HGLRC CurrentContext() {
  static std::atomic<HGLRC(WINAPI*)()> get_current_context = nullptr;  // looked up again until the module is found: it is not loaded at process start
  auto function = get_current_context.load(std::memory_order_relaxed);
  if (function == nullptr) {
    std::string unused;
    const HMODULE system = SystemOpenGl(&unused);
    function = (system != nullptr ? reinterpret_cast<HGLRC(WINAPI*)()>(GetProcAddress(system, "wglGetCurrentContext")) : nullptr);
    if (function == nullptr) return nullptr;
    get_current_context.store(function, std::memory_order_relaxed);
  }
  return function();
}

std::optional<Functions> Functions::Load(std::string* missing) {
  Functions gl;
  const HMODULE system = SystemOpenGl(missing);
  if (system == nullptr) return std::nullopt;
  std::string absent_exports;
  const auto resolve_export = [system, &absent_exports](auto& target, const char* name) {
    target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(system, name));
    if (target == nullptr) {
      absent_exports += std::string(" ") + name;
    }
  };
  resolve_export(gl.GetCurrentContext, "wglGetCurrentContext");
  resolve_export(gl.GetError, "glGetError");
  resolve_export(gl.GetIntegerv, "glGetIntegerv");
  resolve_export(gl.GetString, "glGetString");
  resolve_export(gl.IsEnabled, "glIsEnabled");
  resolve_export(gl.Enable, "glEnable");
  resolve_export(gl.Disable, "glDisable");
  resolve_export(gl.ReadBuffer, "glReadBuffer");
  resolve_export(gl.DrawBuffer, "glDrawBuffer");
  resolve_export(gl.DeleteTextures, "glDeleteTextures");
  resolve_export(gl.Flush, "glFlush");
  using GetProcAddressFn = PROC(WINAPI*)(LPCSTR);
  const auto get_proc_address = reinterpret_cast<GetProcAddressFn>(GetProcAddress(system, "wglGetProcAddress"));
  if (!absent_exports.empty() || get_proc_address == nullptr) {
    *missing = "the system opengl32.dll lacks:" + absent_exports + (get_proc_address == nullptr ? " wglGetProcAddress" : "");
    return std::nullopt;
  }
  if (gl.GetCurrentContext() == nullptr) {
    *missing = "no OpenGL context is current";
    return std::nullopt;
  }

  // Everything later, through the system wglGetProcAddress with the context current. Some drivers answer 1, 2, 3 or -1 for a name they do not know.
  std::string absent_functions;
  const auto resolve_function = [&get_proc_address, &absent_functions](auto& target, const char* name) {
    const auto address = reinterpret_cast<uintptr_t>(get_proc_address(name));
    const bool usable = (address > 3u && address != static_cast<uintptr_t>(-1));
    target = (usable ? reinterpret_cast<std::remove_reference_t<decltype(target)>>(address) : nullptr);
    if (!usable) {
      absent_functions += std::string(" ") + name;
    }
  };
  resolve_function(gl.BindFramebuffer, "glBindFramebuffer");
  resolve_function(gl.GetStringi, "glGetStringi");
  resolve_function(gl.CreateTextures, "glCreateTextures");
  resolve_function(gl.FenceSync, "glFenceSync");
  resolve_function(gl.ClientWaitSync, "glClientWaitSync");
  resolve_function(gl.DeleteSync, "glDeleteSync");
  if (!absent_functions.empty()) {
    *missing = "OpenGL 4.5's functions are missing:" + absent_functions + " (glCreateTextures needs OpenGL 4.5)";
    return std::nullopt;
  }

  // The driver's own list decides what is offered.
  std::vector<std::string> extensions;
  GLint count = 0;
  gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint index = 0; index < count; ++index) {
    if (const GLubyte* const name = gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(index))) {
      extensions.emplace_back(reinterpret_cast<const char*>(name));
    }
  }
  const auto offered = [&extensions](std::string_view name) { return std::ranges::find(extensions, name) != extensions.end(); };
  if (!offered("GL_EXT_memory_object") || !offered("GL_EXT_memory_object_win32")) {
    *missing = "GL_EXT_memory_object_win32 is not offered, so the context cannot import Uplift's textures";
    return std::nullopt;
  }
  absent_functions.clear();
  resolve_function(gl.TextureStorageMem2DEXT, "glTextureStorageMem2DEXT");
  resolve_function(gl.CreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT");
  resolve_function(gl.DeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT");
  resolve_function(gl.MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT");
  resolve_function(gl.ImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT");
  if (!absent_functions.empty()) {
    *missing = "GL_EXT_memory_object_win32 is offered but its functions are missing:" + absent_functions;
    return std::nullopt;
  }
  resolve_function(gl.GetUnsignedBytevEXT, "glGetUnsignedBytevEXT");  // optional (GL_EXT_memory_object's LUID query): DXGI's NVIDIA adapter otherwise

  if (!offered("GL_EXT_semaphore") || !offered("GL_EXT_semaphore_win32")) {
    gl.cpu_reason = "this driver does not offer GL_EXT_semaphore_win32";
    return gl;
  }
  absent_functions.clear();
  resolve_function(gl.GenSemaphoresEXT, "glGenSemaphoresEXT");
  resolve_function(gl.DeleteSemaphoresEXT, "glDeleteSemaphoresEXT");
  resolve_function(gl.SemaphoreParameterui64vEXT, "glSemaphoreParameterui64vEXT");
  resolve_function(gl.ImportSemaphoreWin32HandleEXT, "glImportSemaphoreWin32HandleEXT");
  resolve_function(gl.SignalSemaphoreEXT, "glSignalSemaphoreEXT");
  resolve_function(gl.WaitSemaphoreEXT, "glWaitSemaphoreEXT");
  if (absent_functions.empty()) {
    gl.semaphores = true;
  } else {
    gl.cpu_reason = "this driver offers GL_EXT_semaphore_win32 but its functions are missing:" + absent_functions;
  }
  return gl;
}

}  // namespace uplift::gl
