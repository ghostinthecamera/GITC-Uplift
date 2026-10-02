#pragma once

#include <MinHook.h>

namespace uplift::hooks {

// Plan 11 (key decision c): MinHook is initialised once per process and shared by the NGX hooks (x64) and the Vulkan device hook (both
// halves). MinHook keeps the state itself; these three only make "already initialised" a success and keep MH_Uninitialize away from a
// hook that lives for the process.

// MH_Initialize, where MH_OK and MH_ERROR_ALREADY_INITIALIZED both count as started. Returns MH_OK or the status that failed.
[[nodiscard]] MH_STATUS EnsureMinHook();
// A hook that outlives its installer (the Vulkan device hook pins the add-on) holds MinHook: ReleaseMinHook is then a no-op, for good.
void HoldMinHook();
// MH_Uninitialize, unless MinHook is held. Test cleanup only (HookInstaller::Shutdown); the add-ons never call it.
[[nodiscard]] MH_STATUS ReleaseMinHook();

}  // namespace uplift::hooks
