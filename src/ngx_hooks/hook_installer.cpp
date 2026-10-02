#include "ngx_hooks/hook_installer.hpp"

#include <intrin.h>
#include <psapi.h>
#include <winternl.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>

#include "ngx_hooks/minhook_once.hpp"
#include "nr/log.hpp"
#include "nr/vk_handles.hpp"

namespace uplift::ngx_hooks {
namespace {

using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                               NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                 const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using EvaluateCFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                  const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
// Plan 13 (design §1.3, §5): the core's Vulkan entry points (nvsdk_ngx_vk.h). ReleaseFeature has the Direct3D 12 signature.
using VkCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VkCreate1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VkEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                   PFN_NVSDK_NGX_ProgressCallback);
using VkEvaluateCFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                    PFN_NVSDK_NGX_ProgressCallback_C);
// Batch 3 review I-1: the core's NVSDK_NGX_VULKAN_Shutdown1 is NOT the SDK header's one-argument function. Read from the driver's own code
// (_nvngx.dll 32.0.16.1714, the export at RVA 0xCFD0): it keeps RCX (the VkDevice) and RDX, and tail-jumps to the shared shutdown routine
// (RVA 0x44DC0) with RDX as that routine's fourth argument, a pointer it writes a 32-bit count through (`mov dword ptr [r13], eax` at
// +0x44E54, the crash of a one-argument call). It reads neither R8, R9 nor any stack argument. Streamline 2.12's NGX library (sl.common.dll)
// calls it so: `mov rcx, device; lea rdx, [remaining]; call`, then checks the count. The detour takes four pointer-sized parameters (x64: RCX,
// RDX, R8, R9) and forwards them unchanged, so whatever the caller passed reaches the original as it was.
using VkShutdown1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, void*, void*, void*);
// Plan 15: NVSDK_NGX_D3D12_Shutdown1 has the same shape (the same core, the export at RVA 0x682B0): it keeps RCX (the ID3D12Device) and RDX, calls the
// device's GetAdapterLuid (vtable +0x158) for its LUID, and calls the shared shutdown routine (RVA 0x44DC0) with the device, the LUID and RDX as the count
// pointer; it reads neither R8, R9 nor any stack argument. Detoured with the same four pointer-sized parameters, forwarded unchanged.
using D3D12Shutdown1Fn = VkShutdown1Fn;

// The trampolines of one hooked module copy; stored before its detours are enabled. `poisoned`
// latches when MinHook fails to disable or remove one of this slot's hooks: the hook may still be
// live, so its slot is never reused and its originals are never nulled (a live hook must never call
// a null original).
struct SlotOriginals {
  std::atomic<CreateFn> create{nullptr};
  std::atomic<EvaluateFn> evaluate{nullptr};
  std::atomic<EvaluateCFn> evaluate_c{nullptr};
  std::atomic<ReleaseFn> release{nullptr};
  std::atomic<D3D12Shutdown1Fn> shutdown1{nullptr};
  std::atomic<VkCreateFn> vk_create{nullptr};
  std::atomic<VkCreate1Fn> vk_create1{nullptr};
  std::atomic<VkEvaluateFn> vk_evaluate{nullptr};
  std::atomic<VkEvaluateCFn> vk_evaluate_c{nullptr};
  std::atomic<ReleaseFn> vk_release{nullptr};
  std::atomic<VkShutdown1Fn> vk_shutdown1{nullptr};
  std::atomic<bool> poisoned{false};

  void Clear() {
    create.store(nullptr, std::memory_order_release);
    evaluate.store(nullptr, std::memory_order_release);
    evaluate_c.store(nullptr, std::memory_order_release);
    release.store(nullptr, std::memory_order_release);
    shutdown1.store(nullptr, std::memory_order_release);
    vk_create.store(nullptr, std::memory_order_release);
    vk_create1.store(nullptr, std::memory_order_release);
    vk_evaluate.store(nullptr, std::memory_order_release);
    vk_evaluate_c.store(nullptr, std::memory_order_release);
    vk_release.store(nullptr, std::memory_order_release);
    vk_shutdown1.store(nullptr, std::memory_order_release);
  }
};

std::array<SlotOriginals, HookInstaller::MAX_MODULES> g_originals;
std::atomic<NgxObserver*> g_observer{nullptr};
std::atomic<bool> g_installer_running{false};
thread_local uint32_t t_depth = 0u;
thread_local bool t_vulkan_evaluate = false;  // inside the original of an outermost Vulkan evaluate (InsideVulkanEvaluate)

// Counts hooked calls on this thread, so one NGX copy forwarding to another is seen once.
class DepthGuard {
 public:
  DepthGuard() { ++t_depth; }
  ~DepthGuard() { --t_depth; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;
  [[nodiscard]] bool Nested() const { return t_depth > 1u; }
};

// Which hooked call an observer exception came from; each gets its own "logged once" latch, so a
// failure in one never silences the (independent) first report for the others.
enum class ObserverCall : uint8_t {
  CREATE,
  EVALUATE,
  RELEASE,
  SHUTDOWN,
};

// `what` is the caught exception's `.what()`, or null when it was not a std::exception. Logs at
// most once per `kind` for the life of the process: an observer that keeps throwing must never
// flood the log.
void LogObserverFailure(ObserverCall kind, const char* what) noexcept {
  static std::array<std::atomic<bool>, 4> logged{false, false, false, false};
  const size_t index = static_cast<size_t>(kind);
  if (logged[index].exchange(true)) return;
  constexpr std::array<const char*, 4> NAMES = {"create", "evaluate", "release", "shutdown"};
  try {
    if (what != nullptr) {
      nr::Logf(nr::LogLevel::ERR, "the NGX {} observer threw ({}); the call went to NGX unchanged", NAMES[index], what);
    } else {
      nr::Logf(nr::LogLevel::ERR, "the NGX {} observer threw; the call went to NGX unchanged", NAMES[index]);
    }
  } catch (...) {
    // No-op: logging here is best effort; the NGX call itself is never affected.
  }
}

// Every create detour's body (Plan 13: shared by the Direct3D 12 and Vulkan entry points). `call` is the call as the observer sees it;
// `original()` runs the hooked copy's own entry point with the game's arguments.
template <class Original>
NVSDK_NGX_Result RunCreate(CreateCall call, NVSDK_NGX_Handle** out_handle, Original&& original) {
  const DepthGuard depth;
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (depth.Nested() || observer == nullptr) return original();
  NVSDK_NGX_Parameter* const parameters = call.parameters;
  // Restores the values BeforeCreate changed, last change first. Its own failure never says "the
  // call went to NGX unchanged": the create itself may already have gone through either way.
  const auto restore_quietly = [](NVSDK_NGX_Parameter* restore_parameters, CreateCall* call) noexcept {
    if (call->saved.empty() || restore_parameters == nullptr) return;
    static std::atomic<bool> logged{false};
    try {
      RestoreParameters(restore_parameters, call->saved);
    } catch (const std::exception& e) {
      if (!logged.exchange(true)) {
        try {
          nr::Logf(nr::LogLevel::ERR,
                   "restoring the game's own NGX parameters threw ({}); some may not have been restored", e.what());
        } catch (...) {
        }
      }
    } catch (...) {
      if (!logged.exchange(true)) {
        try {
          nr::Logf(nr::LogLevel::ERR, "restoring the game's own NGX parameters threw; some may not have been restored");
        } catch (...) {
        }
      }
    }
    call->saved.clear();
  };
  try {
    observer->BeforeCreate(&call);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::CREATE, e.what());
    restore_quietly(parameters, &call);
  } catch (...) {
    LogObserverFailure(ObserverCall::CREATE, nullptr);
    restore_quietly(parameters, &call);
  }
  NVSDK_NGX_Result result = original();
  if (!call.saved.empty() && NVSDK_NGX_FAILED(result)) {
    // A DLSS-SR override NGX rejects must never cost the game its DLSS: ask again with its own values.
    restore_quietly(parameters, &call);
    call.override_rejected = true;
    result = original();
  }
  try {
    observer->AfterCreate(call, ((out_handle != nullptr && NVSDK_NGX_SUCCEED(result)) ? *out_handle : nullptr), result);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::CREATE, e.what());
  } catch (...) {
    LogObserverFailure(ObserverCall::CREATE, nullptr);
  }
  restore_quietly(parameters, &call);
  return result;
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV CreateDetour(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                                         NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** out_handle) {
  const CreateFn original = g_originals[SLOT].create.load(std::memory_order_acquire);
  return RunCreate({.list = list, .feature = feature, .parameters = parameters, .caller = _ReturnAddress()}, out_handle,
                   [&] { return original(list, feature, parameters, out_handle); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkCreateDetour(VkCommandBuffer buffer, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters,
                                           NVSDK_NGX_Handle** out_handle) {
  const VkCreateFn original = g_originals[SLOT].vk_create.load(std::memory_order_acquire);
  return RunCreate({.api = NgxApi::VULKAN, .list = nr::AsList(buffer), .feature = feature, .parameters = parameters, .caller = _ReturnAddress()},
                   out_handle, [&] { return original(buffer, feature, parameters, out_handle); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkCreate1Detour(VkDevice device, VkCommandBuffer buffer, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters,
                                            NVSDK_NGX_Handle** out_handle) {
  const VkCreate1Fn original = g_originals[SLOT].vk_create1.load(std::memory_order_acquire);
  return RunCreate(
      {.api = NgxApi::VULKAN, .list = nr::AsList(buffer), .vk_device = device, .feature = feature, .parameters = parameters, .caller = _ReturnAddress()},
      out_handle, [&] { return original(device, buffer, feature, parameters, out_handle); });
}

void NotifyEvaluate(NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                    NVSDK_NGX_Result result) noexcept {
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (observer == nullptr) return;
  try {
    observer->AfterEvaluate(api, list, handle, parameters, result);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::EVALUATE, e.what());
  } catch (...) {
    LogObserverFailure(ObserverCall::EVALUATE, nullptr);
  }
}

// v2 design §3.9 step 7: pre-SR's Color swap for one evaluate. The observer decides before the original
// runs; the destructor puts the game's own Color back on every path, so the game's block never keeps a
// texture of Uplift's past this call. It writes back with the type the game used. Plan 13: a Vulkan block
// holds its resources as void* (NGX_VULKAN_EVALUATE_DLSS_EXT), so only that type is read and written there.
class ColorSwap {
 public:
  ColorSwap(NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) noexcept {
    NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
    if (observer == nullptr || parameters == nullptr) return;
    try {
      EvaluateCall call = {.api = api, .list = list, .handle = handle, .parameters = parameters};
      observer->BeforeEvaluate(&call);
      if (call.color == nullptr) return;
      // NGX takes the block as const, but it is the game's own mutable object, set again every frame.
      auto* const block = const_cast<NVSDK_NGX_Parameter*>(parameters);
      if (api == NgxApi::D3D12 && NVSDK_NGX_SUCCEED(block->Get(NVSDK_NGX_Parameter_Color, &game_color_))) {
        block->Set(NVSDK_NGX_Parameter_Color, call.color);
      } else if (NVSDK_NGX_SUCCEED(block->Get(NVSDK_NGX_Parameter_Color, &game_pointer_))) {
        as_pointer_ = true;
        block->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(call.color));
      } else {
        // Minor 9: pre-SR already applied NR into call.color, on the assumption the swap below would
        // put it where DLSS reads it. It cannot: the game set no Color for this evaluate at all, so
        // there is nothing to swap into, and the caller's status must not say NR applied.
        observer->OnColorSwapRejected(api, list, handle);
        return;
      }
      block_ = block;
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::EVALUATE, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::EVALUATE, nullptr);
    }
  }
  ~ColorSwap() {
    if (block_ == nullptr) return;
    if (as_pointer_) {
      block_->Set(NVSDK_NGX_Parameter_Color, game_pointer_);
    } else {
      block_->Set(NVSDK_NGX_Parameter_Color, game_color_);
    }
  }
  ColorSwap(const ColorSwap&) = delete;
  ColorSwap& operator=(const ColorSwap&) = delete;

 private:
  NVSDK_NGX_Parameter* block_ = nullptr;
  ID3D12Resource* game_color_ = nullptr;
  void* game_pointer_ = nullptr;
  bool as_pointer_ = false;
};

// Every evaluate detour's body (Plan 13: shared by both APIs and both callback kinds). `original()` runs the hooked copy's own entry
// point with the game's arguments.
template <class Original>
NVSDK_NGX_Result RunEvaluate(NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                             Original&& original) {
  const DepthGuard depth;
  if (depth.Nested()) return original();
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  {
    const ColorSwap swap(api, list, handle, parameters);
    t_vulkan_evaluate = (api == NgxApi::VULKAN);
    result = original();
    t_vulkan_evaluate = false;
  }  // the game's Color is back before AfterEvaluate reads the block
  NotifyEvaluate(api, list, handle, parameters, result);
  return result;
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV EvaluateDetour(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                           const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const EvaluateFn original = g_originals[SLOT].evaluate.load(std::memory_order_acquire);
  return RunEvaluate(NgxApi::D3D12, list, handle, parameters, [&] { return original(list, handle, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV EvaluateCDetour(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                            const NVSDK_NGX_Parameter* parameters,
                                            PFN_NVSDK_NGX_ProgressCallback_C callback) {
  const EvaluateCFn original = g_originals[SLOT].evaluate_c.load(std::memory_order_acquire);
  return RunEvaluate(NgxApi::D3D12, list, handle, parameters, [&] { return original(list, handle, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkEvaluateDetour(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                                             PFN_NVSDK_NGX_ProgressCallback callback) {
  const VkEvaluateFn original = g_originals[SLOT].vk_evaluate.load(std::memory_order_acquire);
  return RunEvaluate(NgxApi::VULKAN, nr::AsList(buffer), handle, parameters, [&] { return original(buffer, handle, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkEvaluateCDetour(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                                              PFN_NVSDK_NGX_ProgressCallback_C callback) {
  const VkEvaluateCFn original = g_originals[SLOT].vk_evaluate_c.load(std::memory_order_acquire);
  return RunEvaluate(NgxApi::VULKAN, nr::AsList(buffer), handle, parameters, [&] { return original(buffer, handle, parameters, callback); });
}

// Both APIs' ReleaseFeature: BeforeRelease, then the hooked copy's own entry point.
NVSDK_NGX_Result RunRelease(NVSDK_NGX_Handle* handle, ReleaseFn original) {
  const DepthGuard depth;
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (!depth.Nested() && observer != nullptr) {
    try {
      observer->BeforeRelease(handle);
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::RELEASE, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::RELEASE, nullptr);
    }
  }
  return original(handle);
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV ReleaseDetour(NVSDK_NGX_Handle* handle) {
  return RunRelease(handle, g_originals[SLOT].release.load(std::memory_order_acquire));
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkReleaseDetour(NVSDK_NGX_Handle* handle) {
  return RunRelease(handle, g_originals[SLOT].vk_release.load(std::memory_order_acquire));
}

// Batch 3 review I-1: BeforeCoreShutdown on an outermost call, then the core's own Shutdown1 with the caller's four registers (VkShutdown1Fn).
// Nested calls (one copy forwarding to another, or the NR runtime's teardown reaching the core inside BeforeCoreShutdown) go straight through.
// Plan 15: both APIs' Shutdown1 (D3D12Shutdown1Fn is the same shape); `caller` is the detour's own return address.
NVSDK_NGX_Result RunShutdown1(NgxApi api, VkShutdown1Fn original, const void* caller, void* device, void* second, void* third, void* fourth) {
  const DepthGuard depth;
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (!depth.Nested() && observer != nullptr) {
    try {
      observer->BeforeCoreShutdown(api, device, caller);
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::SHUTDOWN, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::SHUTDOWN, nullptr);
    }
  }
  return original(device, second, third, fourth);
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV Shutdown1Detour(void* device, void* second, void* third, void* fourth) {
  return RunShutdown1(NgxApi::D3D12, g_originals[SLOT].shutdown1.load(std::memory_order_acquire), _ReturnAddress(), device, second, third, fourth);
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkShutdown1Detour(void* device, void* second, void* third, void* fourth) {
  return RunShutdown1(NgxApi::VULKAN, g_originals[SLOT].vk_shutdown1.load(std::memory_order_acquire), _ReturnAddress(), device, second, third, fourth);
}

struct DetourSet {
  CreateFn create;
  EvaluateFn evaluate;
  EvaluateCFn evaluate_c;
  ReleaseFn release;
  D3D12Shutdown1Fn shutdown1;
  VkCreateFn vk_create;
  VkCreate1Fn vk_create1;
  VkEvaluateFn vk_evaluate;
  VkEvaluateCFn vk_evaluate_c;
  ReleaseFn vk_release;
  VkShutdown1Fn vk_shutdown1;
};

// One detour set per module copy: each calls its own copy's trampolines.
constexpr std::array<DetourSet, HookInstaller::MAX_MODULES> DETOURS =
    []<size_t... SLOTS>(std::index_sequence<SLOTS...> /*slots*/) {
      return std::array<DetourSet, sizeof...(SLOTS)>{
          DetourSet{
              .create = &CreateDetour<SLOTS>,
              .evaluate = &EvaluateDetour<SLOTS>,
              .evaluate_c = &EvaluateCDetour<SLOTS>,
              .release = &ReleaseDetour<SLOTS>,
              .shutdown1 = &Shutdown1Detour<SLOTS>,
              .vk_create = &VkCreateDetour<SLOTS>,
              .vk_create1 = &VkCreate1Detour<SLOTS>,
              .vk_evaluate = &VkEvaluateDetour<SLOTS>,
              .vk_evaluate_c = &VkEvaluateCDetour<SLOTS>,
              .vk_release = &VkReleaseDetour<SLOTS>,
              .vk_shutdown1 = &VkShutdown1Detour<SLOTS>,
          }...,
      };
    }(std::make_index_sequence<HookInstaller::MAX_MODULES>());

// LDR_DLL_NOTIFICATION_DATA: its Loaded and Unloaded members share this layout (MSDN,
// "LdrRegisterDllNotification"). The Windows SDK headers do not declare these types.
struct DllNotificationData {
  ULONG flags;
  const UNICODE_STRING* full_dll_name;
  const UNICODE_STRING* base_dll_name;
  void* dll_base;
  ULONG size_of_image;
};
using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, void* context);
using RegisterDllNotificationFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn callback, void* context, void** cookie);
using UnregisterDllNotificationFn = LONG(NTAPI*)(void* cookie);
constexpr ULONG DLL_NOTIFICATION_LOADED = 1u;

// Runs under the loader lock: it only flags the load; Poll() hooks the module later, outside it.
VOID CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, void* context) {
  if (reason != DLL_NOTIFICATION_LOADED || data == nullptr || data->base_dll_name == nullptr || context == nullptr) return;
  const UNICODE_STRING& name = *data->base_dll_name;
  if (name.Buffer == nullptr) return;
  if (IsNgxCoreFileName(std::wstring_view(name.Buffer, name.Length / sizeof(wchar_t)))) {
    static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
  }
}

FARPROC NtdllExport(const char* name) {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return (ntdll == nullptr ? nullptr : GetProcAddress(ntdll, name));
}

}  // namespace

void RestoreParameters(NVSDK_NGX_Parameter* parameters, std::span<const SavedParameter> saved) {
  for (auto entry = saved.rbegin(); entry != saved.rend(); ++entry) {
    switch (entry->type) {
      case SavedParameter::Type::INT:  parameters->Set(entry->key, static_cast<int>(entry->value)); break;
      case SavedParameter::Type::UINT: parameters->Set(entry->key, static_cast<unsigned int>(entry->value)); break;
    }
  }
}

bool InsideVulkanEvaluate() {
  return t_vulkan_evaluate;
}

bool IsNgxCoreFileName(std::wstring_view file_name) {
  if (file_name.empty()) return false;
  const auto equals = [file_name](const wchar_t* candidate) {
    return CompareStringOrdinal(file_name.data(), static_cast<int>(file_name.size()), candidate, -1, TRUE) == CSTR_EQUAL;
  };
  return equals(L"_nvngx.dll") || equals(L"nvngx.dll");
}

HookInstaller::~HookInstaller() {
  Shutdown();
}

bool HookInstaller::Start(NgxObserver* observer, std::string* error) {
  const std::unique_lock lock(mutex_);
  if (started_) return true;
  if (g_installer_running.exchange(true)) {
    *error = "another NGX hook installer is already running";
    return false;
  }
  // Plan 11: one MH_Initialize per process, shared with the Vulkan device hook (which may have started it already).
  if (const MH_STATUS status = hooks::EnsureMinHook(); status != MH_OK) {
    g_installer_running.store(false);
    *error = std::format("MinHook failed to start: {}", MH_StatusToString(status));
    return false;
  }
  // The detours and the DLL-load notification live in this module: it must outlive them (spec §10).
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         reinterpret_cast<LPCWSTR>(&OnDllNotification), &self)
      == FALSE) {
    nr::Logf(nr::LogLevel::WARN, "could not pin Uplift's module ({})", GetLastError());
  }
  g_observer.store(observer, std::memory_order_release);
  const auto register_notification =
      reinterpret_cast<RegisterDllNotificationFn>(NtdllExport("LdrRegisterDllNotification"));
  if (register_notification == nullptr
      || register_notification(0u, &OnDllNotification, &pending_, &notification_cookie_) < 0) {
    notification_cookie_ = nullptr;
    nr::Log(nr::LogLevel::WARN, "no DLL-load notification: an NGX core loaded later is hooked only at an explicit Scan");
  }
  started_ = true;
  ScanLocked();
  return true;
}

void HookInstaller::Poll() {
  if (!pending_.load(std::memory_order_relaxed)) return;
  if (!pending_.exchange(false, std::memory_order_acq_rel)) return;
  Scan();
}

size_t HookInstaller::Scan() {
  const std::unique_lock lock(mutex_);
  return ScanLocked();
}

size_t HookInstaller::ScanLocked() {
  if (!started_) return 0u;

  // Every loaded module named like an NGX core (formerly the free function LoadedNgxCores; its only
  // caller was this one, so the controller's helper ruling folds it in here).
  std::vector<HMODULE> loaded_modules(256u);
  while (true) {
    DWORD needed_bytes = 0u;
    if (K32EnumProcessModules(GetCurrentProcess(), loaded_modules.data(),
                              static_cast<DWORD>(loaded_modules.size() * sizeof(HMODULE)), &needed_bytes)
        == FALSE) {
      loaded_modules.clear();
      break;
    }
    const size_t count = needed_bytes / sizeof(HMODULE);
    const bool complete = (count <= loaded_modules.size());
    loaded_modules.resize(count);
    if (complete) break;
  }
  std::vector<std::pair<HMODULE, std::wstring>> cores;
  std::wstring path(32768u, L'\0');
  for (const HMODULE module : loaded_modules) {
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0u || length >= path.size()) continue;
    const std::wstring_view full_path(path.data(), length);
    const size_t separator = full_path.find_last_of(L"\\/");
    const std::wstring_view file_name =
        (separator == std::wstring_view::npos ? full_path : full_path.substr(separator + 1u));
    if (IsNgxCoreFileName(file_name)) {
      cores.emplace_back(module, std::wstring(file_name));
    }
  }

  size_t hooked = 0u;
  for (const auto& [module, file_name] : cores) {
    const bool known =
        std::ranges::any_of(installed_, [module](const Installed& entry) { return entry.module.module == module; })
        || std::ranges::find(rejected_, module) != rejected_.end();
    if (known) continue;

    // --- formerly HookInstaller::InstallOn(module, file_name); one caller (here), so inlined. ---
    // P3: formerly a `utf8_name` helper; ScanLocked is its one call site now that InstallOn is inlined
    // above too, so inlined here as well (the controller's helper rule).
    const std::string name = [&file_name] {
      if (file_name.empty()) return std::string();
      const int length = WideCharToMultiByte(CP_UTF8, 0u, file_name.data(), static_cast<int>(file_name.size()), nullptr,
                                             0, nullptr, nullptr);
      if (length <= 0) return std::string("<unnamed module>");
      std::string utf8(static_cast<size_t>(length), '\0');
      WideCharToMultiByte(CP_UTF8, 0u, file_name.data(), static_cast<int>(file_name.size()), utf8.data(), length, nullptr,
                          nullptr);
      return utf8;
    }();
    const auto export_address = [module](const char* export_name) {
      return reinterpret_cast<void*>(GetProcAddress(module, export_name));
    };
    const auto hooked_already = [this](void* target) {
      return std::ranges::any_of(installed_, [target](const Installed& entry) {
        return std::ranges::find(entry.targets, target) != entry.targets.end();
      });
    };
    // Plan 13 (design §5): either API's entry points make a copy worth hooking. Each API is taken when its create (Vulkan: either of
    // its two), evaluate and release are exported and none of them is hooked already (a copy that forwards to a hooked one).
    void* const create = export_address("NVSDK_NGX_D3D12_CreateFeature");
    void* const evaluate = export_address("NVSDK_NGX_D3D12_EvaluateFeature");
    void* const evaluate_c = export_address("NVSDK_NGX_D3D12_EvaluateFeature_C");
    void* const release = export_address("NVSDK_NGX_D3D12_ReleaseFeature");
    void* const shutdown1 = export_address("NVSDK_NGX_D3D12_Shutdown1");
    void* const vk_create = export_address("NVSDK_NGX_VULKAN_CreateFeature");
    void* const vk_create1 = export_address("NVSDK_NGX_VULKAN_CreateFeature1");
    void* const vk_evaluate = export_address("NVSDK_NGX_VULKAN_EvaluateFeature");
    void* const vk_evaluate_c = export_address("NVSDK_NGX_VULKAN_EvaluateFeature_C");
    void* const vk_release = export_address("NVSDK_NGX_VULKAN_ReleaseFeature");
    void* const vk_shutdown1 = export_address("NVSDK_NGX_VULKAN_Shutdown1");
    const bool d3d12_exported = (create != nullptr && evaluate != nullptr && release != nullptr);
    const bool vulkan_exported = ((vk_create != nullptr || vk_create1 != nullptr) && vk_evaluate != nullptr && vk_release != nullptr);
    if (!d3d12_exported && !vulkan_exported) {
      nr::Logf(nr::LogLevel::INFO, "{} exports no D3D12 or Vulkan feature entry points; not hooked", name);
      rejected_.push_back(module);
      continue;
    }
    const bool hook_d3d12 =
        (d3d12_exported && !hooked_already(create) && !hooked_already(evaluate) && !hooked_already(release));
    const bool hook_vulkan = (vulkan_exported && !hooked_already(vk_create) && !hooked_already(vk_create1) && !hooked_already(vk_evaluate)
                              && !hooked_already(vk_release));
    if (!hook_d3d12 && !hook_vulkan) {
      nr::Logf(nr::LogLevel::INFO, "{} forwards to an NGX core that is hooked already", name);
      rejected_.push_back(module);
      continue;
    }

    size_t slot = MAX_MODULES;
    for (size_t candidate = 0u; candidate < MAX_MODULES; ++candidate) {
      if (g_originals[candidate].poisoned.load(std::memory_order_acquire)) continue;
      const bool taken =
          std::ranges::any_of(installed_, [candidate](const Installed& entry) { return entry.slot == candidate; });
      if (!taken) {
        slot = candidate;
        break;
      }
    }
    if (slot == MAX_MODULES) {
      nr::Logf(nr::LogLevel::WARN, "no free NGX hook slot for {} (at most {} module copies are hooked at once)", name,
               MAX_MODULES);
      rejected_.push_back(module);
      continue;
    }

    // The entry points this copy gets detours on, in SlotOriginals' order: null where the copy does not export one, or its API is not
    // taken. `required`: a hook that fails costs its API (only EvaluateFeature_C, which no core exports today, is skipped instead). Batch 3
    // review, minor 4: every exported Vulkan create is required, so a copy that exports both never stays hooked without one of them (its
    // features would never register). Shutdown1 (I-1) is hooked where exported, and left to the hook of a copy it forwards to. Plan 15: the
    // Direct3D 12 Shutdown1 is optional, as EvaluateFeature_C: a copy where it cannot be hooked keeps its DLSS placements, without the guard
    // (Plan 14's behaviour), and the log line says so.
    const DetourSet& detours = DETOURS[slot];
    struct EntryPoint {
      void* target = nullptr;
      void* detour = nullptr;
      bool required = false;
      void* original = nullptr;  // MinHook's trampoline, once hooked
    };
    std::array<EntryPoint, 11> entry_points = {{
        {(hook_d3d12 ? create : nullptr), reinterpret_cast<void*>(detours.create), true},
        {(hook_d3d12 ? evaluate : nullptr), reinterpret_cast<void*>(detours.evaluate), true},
        {(hook_d3d12 ? evaluate_c : nullptr), reinterpret_cast<void*>(detours.evaluate_c), false},
        {(hook_d3d12 ? release : nullptr), reinterpret_cast<void*>(detours.release), true},
        {(hook_d3d12 ? shutdown1 : nullptr), reinterpret_cast<void*>(detours.shutdown1), false},
        {(hook_vulkan ? vk_create : nullptr), reinterpret_cast<void*>(detours.vk_create), true},
        {(hook_vulkan ? vk_create1 : nullptr), reinterpret_cast<void*>(detours.vk_create1), true},
        {(hook_vulkan ? vk_evaluate : nullptr), reinterpret_cast<void*>(detours.vk_evaluate), true},
        {(hook_vulkan ? vk_evaluate_c : nullptr), reinterpret_cast<void*>(detours.vk_evaluate_c), false},
        {(hook_vulkan ? vk_release : nullptr), reinterpret_cast<void*>(detours.vk_release), true},
        {(hook_vulkan && !hooked_already(vk_shutdown1) ? vk_shutdown1 : nullptr), reinterpret_cast<void*>(detours.vk_shutdown1), true},
    }};

    // A game's NGX shutdown may FreeLibrary the core: pinned code can never be unmapped under a
    // detour. hook_installer.hpp promises every hooked module is pinned, so a failed pin rejects it.
    bool pinned_all = true;
    for (const EntryPoint& entry_point : entry_points) {
      if (entry_point.target == nullptr) continue;  // not exported, or its API is not taken
      HMODULE pinned_module = nullptr;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             static_cast<LPCWSTR>(entry_point.target), &pinned_module)
          == FALSE) {
        nr::Logf(nr::LogLevel::WARN, "could not pin {} ({}); not hooked", name, GetLastError());
        pinned_all = false;
        break;
      }
    }
    if (!pinned_all) {
      rejected_.push_back(module);
      continue;
    }

    std::vector<void*> created;
    bool ok = true;
    bool vulkan_hooked = hook_vulkan;
    constexpr size_t FIRST_VULKAN = 5u;  // entry_points[5..10] are the Vulkan ones
    for (size_t index = 0u; index < entry_points.size(); ++index) {
      EntryPoint& entry_point = entry_points[index];
      const bool vulkan_entry = (index >= FIRST_VULKAN);
      // An optional entry point that another copy's hook already covers (a forwarding stub) is left to that hook.
      if (!ok || entry_point.target == nullptr || (vulkan_entry && !vulkan_hooked) || (!entry_point.required && hooked_already(entry_point.target))) {
        continue;
      }
      if (const MH_STATUS status = MH_CreateHook(entry_point.target, entry_point.detour, &entry_point.original); status != MH_OK) {
        nr::Logf(nr::LogLevel::WARN, "MinHook could not hook {}: {}", name, MH_StatusToString(status));
        entry_point.original = nullptr;
        if (!entry_point.required) continue;
        if (!vulkan_entry) {
          ok = false;
          continue;
        }
        // Plan 13: a Vulkan entry point that cannot be hooked costs the Vulkan placements only, never the Direct3D 12 ones. The Vulkan hooks made so
        // far go now: none was enabled yet, so none can be live.
        vulkan_hooked = false;
        for (size_t made = FIRST_VULKAN; made < entry_points.size(); ++made) {
          if (entry_points[made].original == nullptr) continue;
          MH_RemoveHook(entry_points[made].target);
          std::erase(created, entry_points[made].target);
          entry_points[made].original = nullptr;
        }
        continue;
      }
      created.push_back(entry_point.target);
    }
    ok = ok && (hook_d3d12 || vulkan_hooked);  // something is left to hook
    const bool with_evaluate_c = (entry_points[2].original != nullptr);
    if (ok) {
      SlotOriginals& originals = g_originals[slot];
      originals.create.store(reinterpret_cast<CreateFn>(entry_points[0].original), std::memory_order_release);
      originals.evaluate.store(reinterpret_cast<EvaluateFn>(entry_points[1].original), std::memory_order_release);
      originals.evaluate_c.store(reinterpret_cast<EvaluateCFn>(entry_points[2].original), std::memory_order_release);
      originals.release.store(reinterpret_cast<ReleaseFn>(entry_points[3].original), std::memory_order_release);
      originals.shutdown1.store(reinterpret_cast<D3D12Shutdown1Fn>(entry_points[4].original), std::memory_order_release);
      originals.vk_create.store(reinterpret_cast<VkCreateFn>(entry_points[5].original), std::memory_order_release);
      originals.vk_create1.store(reinterpret_cast<VkCreate1Fn>(entry_points[6].original), std::memory_order_release);
      originals.vk_evaluate.store(reinterpret_cast<VkEvaluateFn>(entry_points[7].original), std::memory_order_release);
      originals.vk_evaluate_c.store(reinterpret_cast<VkEvaluateCFn>(entry_points[8].original), std::memory_order_release);
      originals.vk_release.store(reinterpret_cast<ReleaseFn>(entry_points[9].original), std::memory_order_release);
      originals.vk_shutdown1.store(reinterpret_cast<VkShutdown1Fn>(entry_points[10].original), std::memory_order_release);
      for (void* const target : created) {
        if (const MH_STATUS status = MH_QueueEnableHook(target); status != MH_OK) {
          nr::Logf(nr::LogLevel::WARN, "MinHook could not queue {} for enabling: {}", name, MH_StatusToString(status));
          ok = false;
        }
      }
      if (ok) {
        if (const MH_STATUS status = MH_ApplyQueued(); status != MH_OK) {
          nr::Logf(nr::LogLevel::WARN, "MinHook could not apply the queued hooks on {}: {}", name, MH_StatusToString(status));
          ok = false;
        }
      }
    }
    if (!ok) {
      bool poisoned = false;
      for (void* const target : created) {
        if (const MH_STATUS status = MH_DisableHook(target); status != MH_OK) {
          nr::Logf(nr::LogLevel::WARN, "MinHook could not disable a hook rolling back {}: {}", name,
                   MH_StatusToString(status));
          poisoned = true;
        }
        if (const MH_STATUS status = MH_RemoveHook(target); status != MH_OK) {
          nr::Logf(nr::LogLevel::WARN, "MinHook could not remove a hook rolling back {}: {}", name,
                   MH_StatusToString(status));
          poisoned = true;
        }
      }
      if (poisoned) {
        // The hook may still be live: never null its originals, and never hand this slot to another
        // module.
        g_originals[slot].poisoned.store(true, std::memory_order_release);
      } else {
        g_originals[slot].Clear();
      }
      nr::Logf(nr::LogLevel::WARN, "NGX hooks on {} failed; the DLSS placements cannot use it", name);
      rejected_.push_back(module);
      continue;
    }
    const bool with_d3d12_shutdown = (hook_d3d12 && entry_points[4].original != nullptr);
    const bool with_vulkan_shutdown = (vulkan_hooked && entry_points[10].original != nullptr);
    installed_.push_back({.module = {.file_name = file_name,
                                     .module = module,
                                     .evaluate_c = with_evaluate_c,
                                     .d3d12 = hook_d3d12,
                                     .vulkan = vulkan_hooked,
                                     .vulkan_shutdown = with_vulkan_shutdown,
                                     .d3d12_shutdown = with_d3d12_shutdown},
                          .targets = std::move(created),
                          .slot = slot});
    const std::string_view apis = (hook_d3d12 && vulkan_hooked ? "Direct3D 12 and Vulkan" : (hook_d3d12 ? "Direct3D 12" : "Vulkan"));
    nr::Logf(nr::LogLevel::INFO, "NGX hooks installed on {} ({} entry points; EvaluateFeature_C {}{}{})", name, apis,
             (with_evaluate_c ? "hooked" : (evaluate_c == nullptr ? "not exported" : "not hooked")),
             (!hook_d3d12 ? "" : (with_d3d12_shutdown ? "; D3D12_Shutdown1 hooked" : "; D3D12_Shutdown1 not hooked")),
             (!vulkan_hooked ? "" : (with_vulkan_shutdown ? "; VULKAN_Shutdown1 hooked" : "; VULKAN_Shutdown1 not hooked")));
    ++hooked;
  }
  return hooked;
}

void HookInstaller::Shutdown() {
  const std::unique_lock lock(mutex_);
  if (!started_) return;
  g_observer.store(nullptr, std::memory_order_release);
  if (notification_cookie_ != nullptr) {
    const auto unregister_notification =
        reinterpret_cast<UnregisterDllNotificationFn>(NtdllExport("LdrUnregisterDllNotification"));
    if (unregister_notification != nullptr) {
      unregister_notification(notification_cookie_);
    }
    notification_cookie_ = nullptr;
  }
  for (const Installed& entry : installed_) {
    bool poisoned = false;
    for (void* const target : entry.targets) {
      if (const MH_STATUS status = MH_DisableHook(target); status != MH_OK) {
        nr::Logf(nr::LogLevel::WARN, "MinHook could not disable a hook during shutdown (slot {}): {}", entry.slot,
                 MH_StatusToString(status));
        poisoned = true;
      }
      if (const MH_STATUS status = MH_RemoveHook(target); status != MH_OK) {
        nr::Logf(nr::LogLevel::WARN, "MinHook could not remove a hook during shutdown (slot {}): {}", entry.slot,
                 MH_StatusToString(status));
        poisoned = true;
      }
    }
    if (poisoned) {
      // The hook may still be live: never null its originals, and never hand this slot to another
      // module, even to a HookInstaller started later in this process.
      g_originals[entry.slot].poisoned.store(true, std::memory_order_release);
    } else {
      g_originals[entry.slot].Clear();
    }
  }
  installed_.clear();
  rejected_.clear();
  // Plan 11: MinHook stays initialised while the Vulkan device hook (which lives for the process) holds it.
  if (const MH_STATUS status = hooks::ReleaseMinHook(); status != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "MinHook failed to uninitialize: {}", MH_StatusToString(status));
  }
  pending_.store(false, std::memory_order_release);
  started_ = false;
  g_installer_running.store(false);
}

bool HookInstaller::Started() const {
  const std::shared_lock lock(mutex_);
  return started_;
}

std::vector<HookedModule> HookInstaller::Modules() const {
  const std::shared_lock lock(mutex_);
  std::vector<HookedModule> modules;
  modules.reserve(installed_.size());
  for (const Installed& entry : installed_) {
    modules.push_back(entry.module);
  }
  return modules;
}

}  // namespace uplift::ngx_hooks
