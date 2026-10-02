#pragma once

#include <Windows.h>

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nr/ngx.hpp"

namespace uplift::ngx_hooks {

// Plan 13 (design §5): which of the core's APIs a hooked call came through. On Vulkan the command list is a VkCommandBuffer and every
// resource pointer an NVSDK_NGX_Resource_VK*, carried as the D3D12-typed pointers below (nr/vk_handles.hpp's pun); never dereference one
// as a D3D12 object.
enum class NgxApi : uint8_t {
  D3D12,
  VULKAN,
};

// One value the create hook changed on the game's parameter block, with the game's own value to
// put back (0 when the game never set it: NGX's default for every key Uplift overrides).
struct SavedParameter {
  enum class Type : uint8_t {
    INT,
    UINT,
  };
  const char* key = nullptr;
  Type type = Type::INT;
  int64_t value = 0;
};

// Writes the saved values back, last change first, with the type each was read with.
void RestoreParameters(NVSDK_NGX_Parameter* parameters, std::span<const SavedParameter> saved);

// One hooked CreateFeature call as the observer sees it.
struct CreateCall {
  NgxApi api = NgxApi::D3D12;
  ID3D12GraphicsCommandList* list = nullptr;  // on Vulkan, the VkCommandBuffer
  VkDevice vk_device = VK_NULL_HANDLE;        // NVSDK_NGX_VULKAN_CreateFeature1's device; null otherwise
  NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_Reserved_Unknown;
  NVSDK_NGX_Parameter* parameters = nullptr;
  // The outermost call's return address. MinHook enters the detour by a jump, so this is the code
  // that called the export: the game, an interposer, or a runtime Uplift loaded itself.
  const void* caller = nullptr;
  std::vector<SavedParameter> saved;  // BeforeCreate records every value it changes here
  bool override_rejected = false;     // the create failed with the overrides, so it ran again without them
};

// One hooked EvaluateFeature call before the original runs (v2 design §3.9).
struct EvaluateCall {
  NgxApi api = NgxApi::D3D12;
  ID3D12GraphicsCommandList* list = nullptr;  // on Vulkan, the VkCommandBuffer
  const NVSDK_NGX_Handle* handle = nullptr;
  const NVSDK_NGX_Parameter* parameters = nullptr;
  // Set by BeforeEvaluate: what DLSS reads as Color for this call only. On Vulkan an NVSDK_NGX_Resource_VK* (nr::AsResource), which the
  // detour writes into the block as a void*, as the game sets its own.
  ID3D12Resource* color = nullptr;
};

// Receives the hooked NGX calls (spec §10, v2 design §3.1). Every method runs on the game's calling
// thread, with no lock held by the detour. A method may throw: the detour catches, logs once, and
// the NGX call goes through unchanged.
class NgxObserver {
 public:
  virtual ~NgxObserver() = default;
  // Before the original CreateFeature. May change `call->parameters`, recording each change in
  // `call->saved`; the detour restores them after the create.
  virtual void BeforeCreate(CreateCall* call) = 0;
  // After the original CreateFeature, also when it failed (`handle` is then null).
  virtual void AfterCreate(const CreateCall& call, NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result) = 0;
  // Before the original EvaluateFeature or EvaluateFeature_C, once per outermost call, with no lock held
  // by the detour. Setting `call->color` swaps DLSS's Color for this call: the detour writes it into the
  // game's block and puts the game's own Color back after the original, on every path.
  virtual void BeforeEvaluate(EvaluateCall* /*call*/) {}
  // Minor 9 (Plan 4 fix round 4): BeforeEvaluate set `call->color`, but the game's own parameter block
  // had no readable Color for the detour to swap it into, so the swap did not happen. Same call, same
  // thread as BeforeEvaluate; may throw, caught and logged once like the others.
  virtual void OnColorSwapRejected(NgxApi /*api*/, ID3D12GraphicsCommandList* /*list*/, const NVSDK_NGX_Handle* /*handle*/) {}
  // After the original EvaluateFeature or EvaluateFeature_C, once per outermost call.
  virtual void AfterEvaluate(NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                             const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Result result) = 0;
  // Before the original ReleaseFeature.
  virtual void BeforeRelease(NVSDK_NGX_Handle* handle) = 0;
  // Batch 3 review I-1: before the original NVSDK_NGX_VULKAN_Shutdown1 of an outermost call (Streamline's slShutdown, before the game destroys
  // `device`, a VkDevice), with no lock held by the detour. `caller` is the call's return address (the game, an interposer, or a runtime Uplift
  // loaded itself). Every NGX call Uplift still has to make for `device` must happen here: after the original the core no longer serves it.
  // Plan 15: NVSDK_NGX_D3D12_Shutdown1 too (`api` D3D12, `device` the ID3D12Device the caller passed: ReShade's proxy, or the device itself).
  virtual void BeforeCoreShutdown(NgxApi /*api*/, void* /*device*/, const void* /*caller*/) {}
};

// `_nvngx.dll` or `nvngx.dll`, in any case: the NGX core file names Uplift hooks.
[[nodiscard]] bool IsNgxCoreFileName(std::wstring_view file_name);

struct HookedModule {
  std::wstring file_name;
  HMODULE module = nullptr;
  bool evaluate_c = false;  // the module exports D3D12 EvaluateFeature_C, and it is hooked too
  bool d3d12 = false;       // Plan 13: the D3D12 entry points are hooked
  bool vulkan = false;      // Plan 13: the Vulkan entry points are hooked
  bool vulkan_shutdown = false;  // batch 3 review I-1: NVSDK_NGX_VULKAN_Shutdown1 is hooked (Uplift releases NR on a device before it)
  bool d3d12_shutdown = false;   // Plan 15: NVSDK_NGX_D3D12_Shutdown1 is hooked (the same, on the game's Direct3D 12 device)
};

// Plan 13 (design R80, the "violator" observation): true on a thread inside the original NVSDK_NGX_VULKAN_EvaluateFeature(_C) of an
// outermost hooked call, so a ReShade event raised by NGX's own commands (a pipeline bind) can tell it came from DLSS. One TLS read.
[[nodiscard]] bool InsideVulkanEvaluate();

// MinHook detours on CreateFeature, EvaluateFeature, EvaluateFeature_C (where exported) and
// ReleaseFeature of every loaded NGX core copy (spec §10 as amended by the v2 design §3.1), and, Plan 13,
// on the same copy's NVSDK_NGX_VULKAN_ CreateFeature, CreateFeature1, EvaluateFeature, EvaluateFeature_C,
// ReleaseFeature (design §5) and Shutdown1 (batch 3 review I-1, where exported); Plan 15: NVSDK_NGX_D3D12_Shutdown1
// too, where exported (optional, as EvaluateFeature_C). A copy is hooked when it
// exports either API's create, evaluate and release. The detours are process-wide, so one installer runs
// at a time. Uplift's module and every hooked module are pinned, so no detour, trampoline or notification
// ever dangles.
class HookInstaller {
 public:
  static constexpr size_t MAX_MODULES = 8u;

  HookInstaller() = default;
  ~HookInstaller();
  HookInstaller(const HookInstaller&) = delete;
  HookInstaller& operator=(const HookInstaller&) = delete;

  // Starts MinHook, pins Uplift, registers the DLL-load notification and hooks every NGX core
  // loaded now. False, with `error` set, when MinHook cannot start or another installer runs.
  bool Start(NgxObserver* observer, std::string* error);
  // Any thread; a relaxed load makes the no-op case (nothing pending) a single read, with an atomic
  // exchange only when the DLL-load notification saw an NGX core load since the last call, which it
  // then hooks. Call it from entry points, never under the loader lock; Task 11 calls it on every
  // reset and execute, so the common case must stay cheap.
  void Poll();
  // Hooks every loaded NGX core copy that is not hooked yet; returns how many were newly hooked.
  size_t Scan();
  // Test cleanup: unhooks everything, drops the observer and the notification, stops MinHook. The
  // add-on never calls it; its hooks live as long as the process.
  void Shutdown();

  [[nodiscard]] bool Started() const;
  [[nodiscard]] std::vector<HookedModule> Modules() const;

 private:
  struct Installed {
    HookedModule module;
    std::vector<void*> targets;  // the hooked export addresses
    size_t slot = 0;             // the DETOURS/g_originals index; never reused while poisoned
  };

  size_t ScanLocked();

  mutable std::shared_mutex mutex_;
  std::vector<Installed> installed_;
  std::vector<HMODULE> rejected_;  // NGX-named modules that cannot be hooked; never retried
  std::atomic<bool> pending_{false};
  void* notification_cookie_ = nullptr;
  bool started_ = false;
};

}  // namespace uplift::ngx_hooks
