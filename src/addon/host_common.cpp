#include "addon/host_common.hpp"

#include <atomic>
#include <string>

#include "addon/removal_latch.hpp"
#include "nr/log.hpp"

namespace uplift::addon::host {

namespace {

// The longest path GetModuleFileNameW can return, in wide characters (the extended-length path limit).
constexpr DWORD MAX_MODULE_PATH_CHARS = 32768u;
// The Vulkan loader's module: a process without it makes no Vulkan device yet.
constexpr wchar_t VULKAN_LOADER[] = L"vulkan-1.dll";
// Logged when ReShade's module cannot be pinned; the argument is GetLastError's code.
constexpr char PIN_FAILED_LINE[] =
    "could not pin ReShade's module (error {}): if ReShade unloads with the last Vulkan instance, Uplift is registered again at the next one";

// ReShade's module is pinned (once) whenever Uplift pins itself, the NGX hooks or the vkCreateDevice detour, as ReShade's own
// [INSTALL] PreventUnloading does (dll_main.cpp:268-271). The Vulkan loader closes ReShade's layer library with the last instance (a probe instance
// often comes first), and ReShade unloads it then. Uplift would stay, pinned, while the next ReShade starts empty: a second AddonInit that returns
// without registering gets no events at all, and this module's cached ReShade function pointers (the log bridge among them, which the detour logs
// through) would point into the old image. Pinned, ReShade keeps Uplift registered across unload_addons and load_addons (unload_addons marks it
// external and keeps it, addon_manager.cpp:357-368; the next load_addons calls its AddonInit again, :190-209), which is what the second AddonInit's
// early return assumes. The pin is best effort: GetModuleHandleExW on the address of a loaded module does not fail in practice. If it did, then
// while ReShade stays loaded a second register_addon is refused (two ERROR lines in ReShade.log) and the first registration keeps working.
// One per add-on module: each add-on compiles this file.
std::atomic<bool> g_reshade_pinned{false};

}  // namespace

bool PinReshade(HMODULE reshade_module) {
  if (g_reshade_pinned.load(std::memory_order_acquire)) return true;
  HMODULE pinned = nullptr;
  if (reshade_module == nullptr
      || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(reshade_module), &pinned)
             == FALSE) {
    nr::Logf(nr::LogLevel::WARN, PIN_FAILED_LINE, GetLastError());
    return false;
  }
  g_reshade_pinned.store(true, std::memory_order_release);
  return true;
}

bool ReshadePinned() { return g_reshade_pinned.load(std::memory_order_acquire); }

std::filesystem::path ModulePath(HMODULE handle) {
  std::wstring path(MAX_MODULE_PATH_CHARS, wchar_t{});
  path.resize(GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size())));
  return std::filesystem::path(path);
}

bool VulkanLoaderLoaded() { return GetModuleHandleW(VULKAN_LOADER) != nullptr; }

std::filesystem::path VkDevicePendingMarkerPath(const std::filesystem::path& state_directory, const std::filesystem::path& game_exe_path) {
  return LatchMarkerPath(state_directory, game_exe_path).replace_extension(VK_DEVICE_PENDING_EXTENSION);
}

}  // namespace uplift::addon::host
