#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include <vulkan/vulkan.h>

#include "nr/ngx.hpp"

namespace uplift::nr {

inline constexpr uint64_t APPLICATION_ID = 0x876232cULL;

// Set while Uplift's NR runtime is loaded, so other NR hosts can see it (v2 design §3.19).
inline constexpr wchar_t UPLIFT_NR_RUNTIME_MARKER[] = L"UPLIFT_NR_RUNTIME_LOADED";

// True for a runtime module Uplift mapped: the loaded Snippet's, or one a device loss abandoned
// (Abandon keeps it mapped). Thread-safe; the NGX hooks call it from game threads.
[[nodiscard]] bool IsUpliftRuntimeModule(HMODULE module);
// Plan 2 final review M9: true when `snippet_path`'s file name, or nvngx_dlssnr.dll, is mapped by a
// module that is not Uplift's own runtime (another NR host, or an Uplift instance that ReShade
// unloaded after a device loss). Such a runtime is already initialised; Load never initialises it again.
[[nodiscard]] bool IsRuntimeMappedElsewhere(const std::filesystem::path& snippet_path);
// 1.1.1: the full path of that other mapping (empty when there is none), so the log says who loaded it.
[[nodiscard]] std::filesystem::path RuntimeMappedElsewherePath(const std::filesystem::path& snippet_path);

struct SnippetConfig {
  std::filesystem::path snippet_path;           // the user's nvngx_dlssnr.dll
  std::filesystem::path application_data_path;  // NGX core app data, used only if Uplift initialises the core
  std::filesystem::path core_path_override;     // optional explicit _nvngx.dll
  bool allow_core_force_load = true;            // games without DLSS have no NGX core loaded
  bool enable_snippet_log = true;               // route the 310.8 snippet's log callback into nr::Log
  // Plan 7 (D3D11 design R15): also initialise an adopted core (one the game loaded) for this device, once, when the
  // device carries no UPLIFT_CORE_INIT_GUID mark. The D3D11 bridge's private device is one a D3D11 game's core never saw.
  // Plan 13: BindVulkan reads it too, once per VkDevice: a smoke's "game" device was never initialised by a core that a sibling
  // test loaded; a real game's core is adopted as it is (the game initialised it for its own device).
  bool initialize_core_for_device = false;
};

// Plan 13 (design §3.1): the game's Vulkan device and the loader's exports (the game's own chain, ReShade's layer included).
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

// Which API a BOUND Snippet serves (Plan 13): one API and one device per Snippet.
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
  // Plan 13: the Vulkan twin of Bind. Adopts the loaded core's VULKAN parameter functions (the game initialised it for its
  // device), or force-loads the core and runs its VULKAN_Init_Ext2 once per VkDevice, then runs the snippet's own
  // NVSDK_NGX_VULKAN_Init_Ext2 with the loader's exports (the research probe's exact call). A snippet is bound to one API and one device.
  [[nodiscard]] NVSDK_NGX_Result BindVulkan(const VulkanBinding& binding);
  // The vkDestroyDevice detour: forget a device whose core Uplift initialised (a new VkDevice at the same address is a new device).
  static void ForgetVulkanDevice(VkDevice device);
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
      ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) const;
  [[nodiscard]] NVSDK_NGX_Result EvaluateFeature(
      ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) const;
  // Plan 13: the Vulkan binding's create and evaluate on a command buffer. A distinct name rather than an overload: `nullptr` must stay
  // unambiguous for the Direct3D 12 forwarders above. Both refuse (FAIL_NotInitialized) unless bound to Vulkan.
  [[nodiscard]] NVSDK_NGX_Result CreateFeatureVulkan(
      VkCommandBuffer buffer, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) const;
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
