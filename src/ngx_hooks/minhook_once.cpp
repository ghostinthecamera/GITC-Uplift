#include "ngx_hooks/minhook_once.hpp"

#include <atomic>

namespace uplift::hooks {
namespace {

std::atomic<bool> g_held{false};

}  // namespace

MH_STATUS EnsureMinHook() {
  const MH_STATUS status = MH_Initialize();
  return (status == MH_ERROR_ALREADY_INITIALIZED ? MH_OK : status);
}

void HoldMinHook() {
  g_held.store(true, std::memory_order_release);
}

MH_STATUS ReleaseMinHook() {
  if (g_held.load(std::memory_order_acquire)) return MH_OK;
  return MH_Uninitialize();
}

}  // namespace uplift::hooks
