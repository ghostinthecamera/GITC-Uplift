#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "nr/ngx.hpp"

namespace uplift::nr {

inline constexpr uint64_t APPLICATION_ID = 0x876232cULL;

// Set while Uplift's NR runtime is loaded, so other NR hosts can see it.
inline constexpr wchar_t UPLIFT_NR_RUNTIME_MARKER[] = L"UPLIFT_NR_RUNTIME_LOADED";

// True for a runtime module Uplift mapped: the loaded Snippet's, or one a device loss abandoned
// (Abandon keeps it mapped). Thread-safe; the NGX hooks call it from game threads.
[[nodiscard]] bool IsUpliftRuntimeModule(HMODULE module);

// The full paths of every module in this process whose file name is `snippet_path`'s or nvngx_dlssnr.dll (any case),
// except those `is_own` calls Uplift's own runtime (the loaded Snippet's, or one a device loss abandoned). What is left
// belongs to another NR host, or is the copy of the file Uplift would load. Unlike GetModuleHandleW by name, which
// returns one module, this lists every module that has the name. `is_own` is a seam for unit tests.
[[nodiscard]] std::vector<std::filesystem::path> MappedRuntimeCopies(const std::filesystem::path& snippet_path,
                                                                    bool (*is_own)(HMODULE) = &IsUpliftRuntimeModule);

// How many times Snippet::Load has run in this process, whatever it returned. A cached MappedRuntimeDecision is stale once this moves.
[[nodiscard]] uint64_t SnippetLoadCount();

enum class MappedRuntimeRule : uint8_t {
  NONE,       // no foreign copy is mapped
  LOAD_OWN,   // a foreign copy is mapped from other files: Uplift's own copy loads as a separate module
  SAME_FILE,  // a foreign copy is the snippet's own file (or cannot be told apart from it): LoadLibraryW would return that initialised module
};

struct MappedRuntimeDecision {
  MappedRuntimeRule rule = MappedRuntimeRule::NONE;
  std::filesystem::path same_file;            // the first copy that is the snippet's file; empty unless SAME_FILE
  std::vector<std::filesystem::path> others;  // the copies that are other files
};

// Pure. A copy is the same file when std::filesystem::equivalent says so, or fails to say anything: a path that cannot be
// compared is refused like the same file, because loading it could hand back a module that is already initialised.
[[nodiscard]] MappedRuntimeDecision DecideMappedRuntime(const std::filesystem::path& snippet_path,
                                                        const std::vector<std::filesystem::path>& copies);

struct SnippetConfig {
  std::filesystem::path snippet_path;           // the user's nvngx_dlssnr.dll
  std::filesystem::path application_data_path;  // NGX core app data, used only if Uplift initialises the core
  std::filesystem::path core_path_override;     // optional explicit _nvngx.dll
  bool allow_core_force_load = true;            // games without DLSS have no NGX core loaded
  bool enable_snippet_log = true;               // route a verified snippet layout's log callback into nr::Log
  // Also initialise an adopted core (one the game loaded) for this device, once, when the
  // device carries no UPLIFT_CORE_INIT_GUID mark. The D3D11 bridge's private device is one a D3D11 game's core never saw.
  // BindVulkan reads it too, once per VkDevice: a smoke's "game" device was never initialised by a core that a sibling
  // test loaded; a real game's core is adopted as it is (the game initialised it for its own device).
  bool initialize_core_for_device = false;
};

// The game's Vulkan device and the loader's exports (the game's own chain, ReShade's layer included).
struct VulkanBinding {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
};

enum class SnippetState : uint8_t {
  UNLOADED,
  LOADED,
  BOUND,
};

// Which API a BOUND Snippet serves: one API and one device per Snippet.
enum class SnippetApi : uint8_t {
  NONE,
  D3D12,
  VULKAN,
};

// Owns the snippet module. At most one Snippet may be LOADED per process
// because the caller-identity spoof patches a single IAT slot. Not
// thread-safe: callers serialise all calls.
class Snippet {
 public:
  Snippet() = default;
  ~Snippet();
  Snippet(const Snippet&) = delete;
  Snippet& operator=(const Snippet&) = delete;

  // LoadLibrary + export resolution + identity spoof (+ snippet log hook).
  [[nodiscard]] NVSDK_NGX_Result Load(const SnippetConfig& config);
  // Adopts a loaded NGX core or force-loads and initialises one, then calls
  // the snippet's NVSDK_NGX_D3D12_Init_Ext for `device`.
  [[nodiscard]] NVSDK_NGX_Result Bind(ID3D12Device* device);
  // The Vulkan twin of Bind. Adopts the loaded core's VULKAN parameter functions (the game initialised it for its
  // device), or force-loads the core and runs its VULKAN_Init_Ext2 once per VkDevice, then runs the snippet's own
  // NVSDK_NGX_VULKAN_Init_Ext2 with the loader's exports (the research probe's exact call). A snippet is bound to one API and one device.
  [[nodiscard]] NVSDK_NGX_Result BindVulkan(const VulkanBinding& binding);
  // The vkDestroyDevice detour, before the device goes: shut down the core's initialisation on a device Uplift initialised it on (the core's
  // two-argument Shutdown1), and forget the device (a new VkDevice at the same address is a new device). Nothing for any other device.
  static void ForgetVulkanDevice(VkDevice device);
  // NR's context on `device` is torn down, NR unloaded, the device still held: shut down the core's initialisation on it if Uplift initialised it
  // (the core's two-argument Shutdown1), so nothing of the core's outlives the device. Nothing for a device Uplift did not initialise it on.
  static void ShutdownD3D12Core(ID3D12Device* device);
  // Snippet Shutdown1, identity restore, FreeLibrary. A core that Uplift
  // initialised stays loaded and initialised for the device's lifetime.
  void Unload();
  // Device-lost path: restores the IAT and log-callback slots to their
  // originals, keeps the snippet module mapped (no FreeLibrary), and never
  // calls Shutdown1, ReleaseFeature or DestroyParameters. This Snippet is
  // unusable afterwards; a fresh Snippet may load in the same process.
  void Abandon();

  [[nodiscard]] NVSDK_NGX_Parameter* AllocateParameters() const;
  void DestroyParameters(NVSDK_NGX_Parameter* parameters) const;
  [[nodiscard]] NVSDK_NGX_Result CreateFeature(
      ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle, std::string* network = nullptr) const;
  [[nodiscard]] NVSDK_NGX_Result EvaluateFeature(
      ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) const;
  // The Vulkan binding's create and evaluate on a command buffer. A distinct name rather than an overload: `nullptr` must stay
  // unambiguous for the Direct3D 12 forwarders above. Both refuse (FAIL_NotInitialized) unless bound to Vulkan.
  [[nodiscard]] NVSDK_NGX_Result CreateFeatureVulkan(
      VkCommandBuffer buffer, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle, std::string* network = nullptr) const;
  [[nodiscard]] NVSDK_NGX_Result EvaluateFeatureVulkan(
      VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) const;
  // Shared by both bindings: the export it calls is the bound API's own.
  [[nodiscard]] NVSDK_NGX_Result ReleaseFeature(NVSDK_NGX_Handle* handle) const;
  // Bytes reported by the snippet's DLSSNRGetStatsCallback; nullopt when unavailable.
  [[nodiscard]] std::optional<uint64_t> QueryAllocatedBytes() const;

  [[nodiscard]] SnippetState State() const { return state_; }
  [[nodiscard]] SnippetApi Api() const { return api_; }
  [[nodiscard]] HMODULE Module() const { return module_; }
  [[nodiscard]] ID3D12Device* Device() const { return device_; }
  [[nodiscard]] VkDevice VulkanDevice() const { return vk_device_; }
  [[nodiscard]] const std::filesystem::path& CorePath() const { return core_path_; }
  [[nodiscard]] bool CoreInitializedByUplift() const { return core_initialized_by_uplift_; }
  [[nodiscard]] bool LegacyRuntime() const { return legacy_runtime_; }

 private:
  bool AttachCore(ID3D12Device* device);
  bool AttachVulkanCore(const VulkanBinding& binding);
  void ReleaseCore();
  void PrepareStats();
  // Compare-and-restores the log-callback and IAT slots to their captured
  // originals, logging install/restore results and any foreign pointer left
  // by another module. Shared by Unload() and Abandon().
  void RestorePatches();

  SnippetConfig config_;
  SnippetState state_ = SnippetState::UNLOADED;
  SnippetApi api_ = SnippetApi::NONE;
  bool abandoned_ = false;  // Abandon() ran: this instance refuses to Load() again
  bool legacy_runtime_ = false;  // Last loaded version; kept across unload so the controls stay unavailable while NR is off.
  HMODULE module_ = nullptr;
  ID3D12Device* device_ = nullptr;
  VkDevice vk_device_ = VK_NULL_HANDLE;

  // Snippet exports.
  void* init_ext_ = nullptr;
  void* create_feature_ = nullptr;
  void* evaluate_feature_ = nullptr;
  void* release_feature_ = nullptr;
  void* shutdown_ = nullptr;
  void* populate_parameters_ = nullptr;
  // The Vulkan exports (null when the snippet has none; only BindVulkan requires them). BindVulkan makes release_feature_,
  // shutdown_ and populate_parameters_ above the Vulkan ones.
  void* vk_init_ext2_ = nullptr;
  void* vk_create_feature_ = nullptr;
  void* vk_evaluate_feature_ = nullptr;
  void* vk_release_feature_ = nullptr;
  void* vk_shutdown_ = nullptr;
  void* vk_populate_parameters_ = nullptr;

  // NGX core (parameter provider).
  HMODULE core_module_ = nullptr;
  std::filesystem::path core_path_;
  bool core_initialized_by_uplift_ = false;
  void* allocate_parameters_ = nullptr;
  void* destroy_parameters_ = nullptr;

  // Stats.
  NVSDK_NGX_Parameter* stats_parameters_ = nullptr;
  void* stats_callback_ = nullptr;
};

}  // namespace uplift::nr
