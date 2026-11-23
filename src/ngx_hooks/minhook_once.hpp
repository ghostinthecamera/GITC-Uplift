#pragma once

#include <MinHook.h>

#include <array>
#include <cstddef>

namespace uplift::hooks {

#if defined(_WIN64)
// How many jumps BehindPatch follows beyond the code a hot-patch leads to, and so how many places it can name.
inline constexpr size_t MAX_FOLLOWED_JUMPS = 4u;
inline constexpr size_t BEHIND_PATCH_PLACES = 1u + MAX_FOLLOWED_JUMPS;
// How many places a call passes behind a patch that BehindPatch can name: a relay or a MinHook stub, and as many again as it can hook.
inline constexpr size_t WAY_PLACES = 1u + BEHIND_PATCH_PLACES;
#endif

// MinHook is initialised once per process and shared by the NGX hooks (x64) and the Vulkan device hook (both
// halves). MinHook keeps the state itself; these three only make "already initialised" a success and keep MH_Uninitialize away from a
// hook that lives for the process.

// MH_Initialize, where MH_OK and MH_ERROR_ALREADY_INITIALIZED both count as started. Returns MH_OK or the status that failed.
[[nodiscard]] MH_STATUS EnsureMinHook();
// A hook that outlives its installer (the Vulkan device hook pins the add-on) holds MinHook: ReleaseMinHook is then a no-op, for good.
void HoldMinHook();
// MH_Uninitialize, unless MinHook is held. Test cleanup only (HookInstaller::Shutdown); the add-ons never call it.
[[nodiscard]] MH_STATUS ReleaseMinHook();

#if defined(_WIN64)
// Another tool's patch on a function, as a hook of Uplift's should meet it: the tool may check its patch on every call (MFG Unlocker does, and refuses
// the call when it changed), so no byte of the patch may change while anything behind it can be hooked.
struct ToolPatch {
  std::array<void*, BEHIND_PATCH_PLACES> code = {};  // where to hook, in the order to try; null past the last, all null without a patch
  void* relay = nullptr;                             // the patch's own jump stub: the last resort when none of `code` can be hooked
  // Every place a call to the entry passes, in order, null past the last: the relay and `code`, or MinHook's stub and where its jumps lead. A hook of
  // another function's on any of them takes this entry's calls too.
  std::array<void*, WAY_PLACES> way = {};
};

// - The Windows hot-patch, `jmp short -7` (EB F9) at the entry into a `jmp rel32` (E9) over the five bytes above it: where the E9 lands, or, when that
//   is a relay `jmp [rip+0]` (FF 25 00000000) with its address behind it, that address. Code that starts with an unconditional jump may leave MinHook
//   no room: where each such jump leads comes next.
// - A `jmp rel32` at the entry, as MinHook writes it: where it lands, when that is private memory, and no further. MinHook makes one such stub for each
//   hook. A landing in a module may be a function that serves several entries, or the module's own thunk: no patch. The `way` goes on where the
//   stub's jumps lead, since the calls do.
// A MinHook stub has the relay's bytes too. Only the way to it tells them apart: a tool that writes a hot-patch made the relay itself.
[[nodiscard]] ToolPatch BehindPatch(const void* entry);
#endif

}  // namespace uplift::hooks
