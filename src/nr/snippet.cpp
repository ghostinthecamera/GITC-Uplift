#include "nr/snippet.hpp"

#include <Windows.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "nr/driver_store.hpp"
#include "nr/log.hpp"
#include "nr/pe_imports.hpp"

namespace uplift::nr {
namespace {

using InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(
    unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
// The driver core exports the snippet-style 4-argument Init. The 5-argument
// FeatureCommonInfo overload exists only in the static NGX loader library, and
// calling the core with it breaks it.
using CoreInit = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version);
using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using AllocateParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using PopulateParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
// Matches the 310.8 image (GetStatsCommon, rva 0x15060): it writes SizeInBytes
// (u64) into the block and returns Success, FAIL_InvalidParameter for a null
// block, or FAIL_NotInitialized when it finds no initialised instance.
using StatsCallbackFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);

// Plan 13: the Vulkan exports of the snippet and of the core. Both Init_Ext2 take the same nine arguments (the research probe, Plan 11's
// case 1): the core's last one is never read when it is null, as the snippet's.
using VulkanInitExt2 = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                                                     PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version,
                                                     const NVSDK_NGX_Parameter*);
using VulkanCreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VulkanEvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                              PFN_NVSDK_NGX_ProgressCallback);
using VulkanShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice);

// Private data on a device whose NGX core Uplift initialised (force-loaded, or adopted for the D3D11
// bridge), so the core is never initialised twice for one device.
constexpr GUID UPLIFT_CORE_INIT_GUID = {0xfb71ea06, 0xb99c, 0x4d65, {0x9e, 0x59, 0x26, 0x19, 0x63, 0x62, 0x8b, 0x74}};

// The 310.8 image keeps its NGX log callback in this .data slot. The layout is
// identified by SizeOfImage, which every known 310.8 build shares (the user's,
// the compat and the ADA build); TimeDateStamp is not pinned.
constexpr uintptr_t SNIPPET_310_8_LOG_CALLBACK_RVA = 0x1142750u;
constexpr DWORD SNIPPET_310_8_IMAGE_SIZE = 0x9e42000u;

struct ModuleReleaser {
  void operator()(HMODULE module) const { FreeLibrary(module); }
};

// One module reference, released on scope exit unless ownership is taken.
using ModuleReference = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleReleaser>;

// Every snippet export resolves its return address to a module and serves the
// call only if that module's GetModuleFileNameW name contains "nvngx.dll".
// Patching the snippet's import of that function makes Uplift's module answer
// "nvngx.dll". The snippet image, and so the slot, is process-wide: a non-null
// slot means one Snippet holds the spoof.
struct IdentitySpoof {
  void** slot = nullptr;
  void* original = nullptr;
  HMODULE caller = nullptr;
};

IdentitySpoof identity_spoof;

// The snippet's log-callback slot while Uplift's callback is patched in.
struct LogHook {
  void** slot = nullptr;
  void* original = nullptr;
};

LogHook log_hook;

// The runtime modules this Uplift module mapped (M9): the loaded Snippet's, and every module a
// device loss abandoned. Read by IsUpliftRuntimeModule from game threads, hence the lock.
struct OwnRuntimeModules {
  std::shared_mutex mutex;
  HMODULE live = nullptr;
  std::vector<HMODULE> abandoned;
};
OwnRuntimeModules own_runtime_modules;

// The Vulkan devices the NGX core was initialised for by Uplift (Plan 13), so a core is never initialised twice for one VkDevice.
// A device that the vkDestroyDevice detour saw destroyed is forgotten: a new VkDevice at its address is a new device.
struct VulkanCoreDevices {
  std::mutex mutex;
  std::vector<VkDevice> devices;
};
VulkanCoreDevices vulkan_core_devices;

DWORD WINAPI SpoofedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
  if (identity_spoof.caller != nullptr && module == identity_spoof.caller && filename != nullptr && size != 0u) {
    constexpr wchar_t IDENTITY[] = L"nvngx.dll";
    constexpr DWORD LENGTH = static_cast<DWORD>(std::size(IDENTITY) - 1u);
    if (LENGTH >= size) {
      filename[0] = L'\0';
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return size;
    }
    std::memcpy(filename, IDENTITY, sizeof(IDENTITY));
    return LENGTH;
  }
  // A snippet thread can still hold the patched pointer after the slot is
  // restored and `original` cleared.
  auto* forward = reinterpret_cast<decltype(&GetModuleFileNameW)>(identity_spoof.original);
  if (forward == nullptr) {
    forward = &GetModuleFileNameW;
  }
  return forward(module, filename, size);
}
static_assert(std::is_same_v<decltype(&SpoofedGetModuleFileNameW), decltype(&GetModuleFileNameW)>,
              "SpoofedGetModuleFileNameW must match GetModuleFileNameW's signature");

// A stale or self-referencing "original" must never be trusted as the
// forward target: a leaked spoof from a prior Uplift instance can leave the
// slot holding an address that no longer resolves to any loaded module (the
// module that owned it was freed), or, if the add-on reloads at the same
// base address, an address that is once again `&SpoofedGetModuleFileNameW`
// in the *new* instance -- which would forward into itself forever. Refuse
// both and forward to the real export instead.
void* SafeOriginal(void* candidate, HMODULE uplift_module) {
  HMODULE resolved = nullptr;
  const BOOL resolved_ok = GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(candidate), &resolved);
  if (resolved_ok == FALSE || resolved == nullptr) {
    if (candidate != nullptr) {
      Log(LogLevel::WARN,
          "the snippet's GetModuleFileNameW import held a stale pointer; restoring the real export instead");
    }
    return reinterpret_cast<void*>(&GetModuleFileNameW);
  }
  if (resolved == uplift_module) {
    Log(LogLevel::WARN, "the snippet's GetModuleFileNameW import already pointed at Uplift's own spoof "
                         "(a leaked prior instance); restoring the real export instead");
    return reinterpret_cast<void*>(&GetModuleFileNameW);
  }
  return candidate;
}

// NVIDIA calls this from the snippet's own worker threads as well as from
// calls Uplift makes. It must never let a C++ exception unwind into NVIDIA
// frames: on a snippet thread that reaches std::terminate with no useful
// trail, and mid-unwind through a foreign frame it can leave a lock held or
// state corrupted. Sinks may allocate (the default sink and Plan 2's ReShade
// bridge both do), so std::bad_alloc alone is reachable here.
void NVSDK_CONV SnippetLog(
    const char* message, NVSDK_NGX_Logging_Level /*level*/, NVSDK_NGX_Feature /*source*/) noexcept {
  try {
    if (message == nullptr) return;
    std::string_view line(message);
    if (line.ends_with('\n')) {
      line.remove_suffix(1u);
    }
    Log(LogLevel::TRACE, line);
  } catch (...) {
    // Never let an exception reach the NVIDIA frame that invoked this callback.
  }
}
// SnippetLog is noexcept, which the NGX callback type is not; a noexcept
// function pointer converts to a non-noexcept one of the same signature, so
// is_same_v would wrongly fail here where is_convertible_v checks what
// actually matters: the slot can hold this function and call it correctly.
static_assert(std::is_convertible_v<decltype(&SnippetLog), NVSDK_NGX_AppLogCallback>,
              "SnippetLog must match NVSDK_NGX_AppLogCallback's signature");

// Writes one pointer-sized slot of a loaded image (an IAT entry or .data).
bool WritePointer(void** slot, void* value) {
  DWORD old_protection = 0u;
  if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection) == FALSE) return false;
  *slot = value;
  DWORD ignored = 0u;
  VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
  return true;
}

void* ResolveExport(HMODULE module, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(module, name));
}

// Snippet and core entry points can throw C++ exceptions across the module
// boundary; report them as FAIL_PlatformError, as openNR does.
template <class Call>
NVSDK_NGX_Result GuardedCall(const Call& call) {
  try {
    return call();
  } catch (...) {
    return NVSDK_NGX_Result_FAIL_PlatformError;
  }
}

// Log text is UTF-8. path::string() would throw for characters outside the
// ANSI code page.
std::string ToUtf8(const std::filesystem::path& path) {
  try {
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
  } catch (const std::system_error&) {
    return "<path not representable as UTF-8>";
  }
}

}  // namespace

bool IsUpliftRuntimeModule(HMODULE module) {
  if (module == nullptr) return false;
  const std::shared_lock lock(own_runtime_modules.mutex);
  return module == own_runtime_modules.live
         || std::ranges::find(own_runtime_modules.abandoned, module) != own_runtime_modules.abandoned.end();
}

std::filesystem::path RuntimeMappedElsewherePath(const std::filesystem::path& snippet_path) {
  for (const std::filesystem::path& name : {snippet_path.filename(), std::filesystem::path(L"nvngx_dlssnr.dll")}) {
    if (name.empty()) continue;
    const HMODULE mapped = GetModuleHandleW(name.c_str());
    if (mapped == nullptr || IsUpliftRuntimeModule(mapped)) continue;
    std::wstring path(4096u, L'\0');
    const DWORD length = GetModuleFileNameW(mapped, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return (path.empty() ? name : std::filesystem::path(path));
  }
  return {};
}

bool IsRuntimeMappedElsewhere(const std::filesystem::path& snippet_path) {
  return !RuntimeMappedElsewherePath(snippet_path).empty();
}

Snippet::~Snippet() {
  Unload();
}

NVSDK_NGX_Result Snippet::Load(const SnippetConfig& config) {
  if (abandoned_) {
    Log(LogLevel::ERR, "this Snippet was abandoned after a device loss and cannot be reused");
    return NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
  }
  if (state_ != SnippetState::UNLOADED) return NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
  if (identity_spoof.slot != nullptr) {
    Log(LogLevel::ERR, "another Snippet is loaded; only one can hold the caller-identity spoof");
    return NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
  }

  std::error_code error;
  const auto snippet_path = std::filesystem::canonical(config.snippet_path, error);
  if (error) {
    Logf(LogLevel::ERR, "snippet {} not found ({})", ToUtf8(config.snippet_path), error.message());
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  if (const std::filesystem::path mapped = RuntimeMappedElsewherePath(snippet_path); !mapped.empty()) {
    std::error_code same_error;
    const bool same_file = std::filesystem::equivalent(mapped, snippet_path, same_error) && !same_error;
    if (!(config.share_idle_mapped_runtime && same_file)) {
      Logf(LogLevel::ERR,
           "{} is already loaded in this game by another component, or stayed mapped after Uplift unloaded it; Uplift will "
           "not initialise it a second time (the loaded copy: {})",
           ToUtf8(snippet_path.filename()), ToUtf8(mapped));
      return NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
    }
    // 1.1.3: the same file, loaded by NVIDIA's DLSS (on RTX 50 cards it maps every nvngx_*.dll next to the game's .exe) and running no NR: LoadLibraryW
    // below takes another reference to that copy, and Unload gives it back.
    Logf(LogLevel::INFO, "{} was already loaded in this game (NVIDIA's DLSS loads it from the game's folder) and no other tool runs NR: Uplift uses that copy",
         ToUtf8(snippet_path.filename()));
  }
  ModuleReference module(LoadLibraryW(snippet_path.c_str()));
  if (!module) {
    const DWORD load_error = GetLastError();
    Logf(LogLevel::ERR, "LoadLibraryW failed for {} ({})", ToUtf8(snippet_path), load_error);
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  void* const init_ext = ResolveExport(module.get(), "NVSDK_NGX_D3D12_Init_Ext");
  void* const create_feature = ResolveExport(module.get(), "NVSDK_NGX_D3D12_CreateFeature");
  void* const evaluate_feature = ResolveExport(module.get(), "NVSDK_NGX_D3D12_EvaluateFeature");
  void* const release_feature = ResolveExport(module.get(), "NVSDK_NGX_D3D12_ReleaseFeature");
  void* const shutdown = ResolveExport(module.get(), "NVSDK_NGX_D3D12_Shutdown1");
  void* const populate_parameters = ResolveExport(module.get(), "NVSDK_NGX_D3D12_PopulateParameters_Impl");
  if (init_ext == nullptr || create_feature == nullptr || evaluate_feature == nullptr || release_feature == nullptr
      || shutdown == nullptr) {
    Logf(LogLevel::ERR, "{} lacks a required NVSDK_NGX_D3D12 export", ToUtf8(snippet_path));
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  // Plan 13: the Vulkan exports are optional here (null when absent); only BindVulkan requires them.
  void* const vk_init_ext2 = ResolveExport(module.get(), "NVSDK_NGX_VULKAN_Init_Ext2");
  void* const vk_create_feature = ResolveExport(module.get(), "NVSDK_NGX_VULKAN_CreateFeature");
  void* const vk_evaluate_feature = ResolveExport(module.get(), "NVSDK_NGX_VULKAN_EvaluateFeature");
  void* const vk_release_feature = ResolveExport(module.get(), "NVSDK_NGX_VULKAN_ReleaseFeature");
  void* const vk_shutdown = ResolveExport(module.get(), "NVSDK_NGX_VULKAN_Shutdown1");
  void* const vk_populate_parameters = ResolveExport(module.get(), "NVSDK_NGX_VULKAN_PopulateParameters_Impl");

  std::string file_version = "unknown";
  bool layout_310_8 = false;
  DWORD version_handle = 0u;
  if (const DWORD version_size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, snippet_path.c_str(), &version_handle);
      version_size != 0u) {
    std::vector<std::byte> version_data(version_size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_size = 0u;
    if (GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, snippet_path.c_str(), 0u, version_size, version_data.data()) != FALSE
        && VerQueryValueW(version_data.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_size) != FALSE
        && fixed != nullptr && fixed_size >= sizeof(VS_FIXEDFILEINFO)) {
      const uint32_t major = HIWORD(fixed->dwFileVersionMS);
      const uint32_t minor = LOWORD(fixed->dwFileVersionMS);
      const uint32_t patch = HIWORD(fixed->dwFileVersionLS);
      const uint32_t build = LOWORD(fixed->dwFileVersionLS);
      file_version = std::format("{}.{}.{}.{}", major, minor, patch, build);
      layout_310_8 = (major == 310u && minor == 8u && patch == 0u);
    }
  }
  SnippetConfig loaded_config = config;
  loaded_config.snippet_path = snippet_path;

  // Nothing between installing the process-wide spoof and committing the state
  // below may throw, or an exception would leave the spoof held by a freed module.
  void** const identity_slot = FindImportAddress(module.get(), "GetModuleFileNameW");
  if (identity_slot == nullptr) {
    Log(LogLevel::ERR, "the snippet has no by-name GetModuleFileNameW import to spoof");
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  HMODULE caller = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&SpoofedGetModuleFileNameW), &caller)
      == FALSE) {
    Log(LogLevel::ERR, "cannot resolve Uplift's own module for the caller-identity spoof");
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  // A stale or self-referencing slot value must never be trusted as the
  // original: SafeOriginal() detects that and substitutes the real export.
  identity_spoof = {.slot = identity_slot, .original = SafeOriginal(*identity_slot, caller), .caller = caller};
  if (!WritePointer(identity_slot, reinterpret_cast<void*>(&SpoofedGetModuleFileNameW))) {
    identity_spoof = {};
    Log(LogLevel::ERR, "cannot patch the snippet's GetModuleFileNameW import");
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  config_ = std::move(loaded_config);
  module_ = module.release();
  init_ext_ = init_ext;
  create_feature_ = create_feature;
  evaluate_feature_ = evaluate_feature;
  release_feature_ = release_feature;
  shutdown_ = shutdown;
  populate_parameters_ = populate_parameters;
  vk_init_ext2_ = vk_init_ext2;
  vk_create_feature_ = vk_create_feature;
  vk_evaluate_feature_ = vk_evaluate_feature;
  vk_release_feature_ = vk_release_feature;
  vk_shutdown_ = vk_shutdown;
  vk_populate_parameters_ = vk_populate_parameters;
  state_ = SnippetState::LOADED;
  {
    const std::unique_lock lock(own_runtime_modules.mutex);
    own_runtime_modules.live = module_;
  }

  if (config_.enable_snippet_log) {
    const auto base = reinterpret_cast<uintptr_t>(module_);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const uintptr_t image_end = base + nt->OptionalHeader.SizeOfImage;
    const std::span<const IMAGE_SECTION_HEADER> sections(IMAGE_FIRST_SECTION(nt), nt->FileHeader.NumberOfSections);
    const bool slot_in_data_section = std::ranges::any_of(sections, [](const IMAGE_SECTION_HEADER& section) {
      const bool writable = (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0u;
      const bool executable = (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0u;
      return writable && !executable && section.VirtualAddress <= SNIPPET_310_8_LOG_CALLBACK_RVA
             && SNIPPET_310_8_LOG_CALLBACK_RVA + sizeof(void*)
                    <= uintptr_t{section.VirtualAddress} + section.Misc.VirtualSize;
    });
    auto* const slot = reinterpret_cast<void**>(base + SNIPPET_310_8_LOG_CALLBACK_RVA);
    // A write at the wrong address would corrupt an unrelated global of NVIDIA
    // code in a game. Patch only the exact 310.8 layout, only inside a writable,
    // non-executable section, and only while the slot is empty or points into
    // the image; the checks run in this order so the slot is read last.
    if (!layout_310_8) {
      Logf(LogLevel::WARN, "the snippet version {} is not 310.8.0.x; snippet logging stays off", file_version);
    } else if (nt->OptionalHeader.SizeOfImage != SNIPPET_310_8_IMAGE_SIZE) {
      Logf(LogLevel::WARN, "the snippet image size {:#x} differs from the 310.8 layout ({:#x}); snippet logging stays off",
           nt->OptionalHeader.SizeOfImage, SNIPPET_310_8_IMAGE_SIZE);
    } else if (!slot_in_data_section) {
      Log(LogLevel::WARN,
          "the snippet log slot is not inside a writable, non-executable section; snippet logging stays off");
    } else if (const auto current = reinterpret_cast<uintptr_t>(*slot);
               current != 0u && (current < base || current >= image_end)) {
      Log(LogLevel::WARN, "the snippet log slot holds a foreign pointer; snippet logging stays off");
    } else {
      log_hook = {.slot = slot, .original = *slot};
      if (WritePointer(slot, reinterpret_cast<void*>(&SnippetLog))) {
        Log(LogLevel::INFO, "snippet log callback hooked");
      } else {
        Log(LogLevel::WARN, "cannot patch the snippet's log-callback slot; snippet logging stays off");
        log_hook = {};
      }
    }
  }
  Logf(LogLevel::INFO, "snippet loaded {} version {}", ToUtf8(snippet_path), file_version);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result Snippet::Bind(ID3D12Device* device) {
  if (state_ == SnippetState::BOUND) {
    if (api_ == SnippetApi::D3D12 && device == device_) return NVSDK_NGX_Result_Success;
    Log(LogLevel::WARN, "Bind rejected a second device; the snippet stays bound to its first device");
    return NVSDK_NGX_Result_FAIL_NotInitialized;
  }
  if (state_ != SnippetState::LOADED) return NVSDK_NGX_Result_FAIL_NotInitialized;
  if (device == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (!AttachCore(device)) return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;

  const NVSDK_NGX_Result result = GuardedCall([&] {
    return reinterpret_cast<InitExt>(init_ext_)(
        APPLICATION_ID, config_.snippet_path.parent_path().c_str(), device, NVSDK_NGX_Version_API, nullptr);
  });
  if (NVSDK_NGX_FAILED(result)) {
    Logf(LogLevel::ERR, "Init_Ext failed with {:#010x}", static_cast<uint32_t>(result));
    ReleaseCore();
    return result;
  }
  device->AddRef();
  device_ = device;
  api_ = SnippetApi::D3D12;
  state_ = SnippetState::BOUND;
  PrepareStats();
  Logf(LogLevel::INFO, "Init_Ext ok, core {}", ToUtf8(core_path_));
  return result;
}

NVSDK_NGX_Result Snippet::BindVulkan(const VulkanBinding& binding) {
  if (state_ == SnippetState::BOUND) {
    if (api_ == SnippetApi::VULKAN && binding.device == vk_device_) return NVSDK_NGX_Result_Success;
    Log(LogLevel::WARN, "BindVulkan rejected a second device or API; the snippet stays bound to its first");
    return NVSDK_NGX_Result_FAIL_NotInitialized;
  }
  if (state_ != SnippetState::LOADED) return NVSDK_NGX_Result_FAIL_NotInitialized;
  if (binding.instance == VK_NULL_HANDLE || binding.physical == VK_NULL_HANDLE || binding.device == VK_NULL_HANDLE
      || binding.get_instance_proc_addr == nullptr || binding.get_device_proc_addr == nullptr) {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  if (vk_init_ext2_ == nullptr || vk_create_feature_ == nullptr || vk_evaluate_feature_ == nullptr || vk_release_feature_ == nullptr
      || vk_shutdown_ == nullptr) {
    Logf(LogLevel::ERR, "{} lacks a required NVSDK_NGX_VULKAN export", ToUtf8(config_.snippet_path));
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  if (!AttachVulkanCore(binding)) return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;

  const NVSDK_NGX_Result result = GuardedCall([&] {
    return reinterpret_cast<VulkanInitExt2>(vk_init_ext2_)(APPLICATION_ID, config_.snippet_path.parent_path().c_str(), binding.instance,
                                                           binding.physical, binding.device, binding.get_instance_proc_addr,
                                                           binding.get_device_proc_addr, NVSDK_NGX_Version_API, nullptr);
  });
  if (NVSDK_NGX_FAILED(result)) {
    Logf(LogLevel::ERR, "VULKAN_Init_Ext2 failed with {:#010x}", static_cast<uint32_t>(result));
    ReleaseCore();
    return result;
  }
  // From here the shared entry points (release, shutdown, stats) are the Vulkan ones; the Direct3D 12 ones are never called on this binding.
  release_feature_ = vk_release_feature_;
  shutdown_ = vk_shutdown_;
  populate_parameters_ = vk_populate_parameters_;
  vk_device_ = binding.device;
  api_ = SnippetApi::VULKAN;
  state_ = SnippetState::BOUND;
  PrepareStats();
  Logf(LogLevel::INFO, "VULKAN_Init_Ext2 ok, core {}", ToUtf8(core_path_));
  return result;
}

void Snippet::ForgetVulkanDevice(VkDevice device) {
  const std::scoped_lock lock(vulkan_core_devices.mutex);
  std::erase(vulkan_core_devices.devices, device);
}

void Snippet::Unload() {
  if (state_ == SnippetState::BOUND) {
    DestroyParameters(stats_parameters_);
    const NVSDK_NGX_Result result = GuardedCall([&] {
      return api_ == SnippetApi::VULKAN ? reinterpret_cast<VulkanShutdownFn>(shutdown_)(vk_device_)
                                        : reinterpret_cast<ShutdownFn>(shutdown_)(device_);
    });
    Logf((NVSDK_NGX_SUCCEED(result) ? LogLevel::INFO : LogLevel::ERR), "Shutdown1 returned {:#010x}",
         static_cast<uint32_t>(result));
    if (api_ == SnippetApi::D3D12) {
      device_->Release();
    }
    ReleaseCore();
  }
  if (state_ != SnippetState::UNLOADED) {
    // Restore both patches first: if another reference keeps the image mapped,
    // it must not call into Uplift after Uplift unloads.
    RestorePatches();
    {
      const std::unique_lock lock(own_runtime_modules.mutex);
      own_runtime_modules.live = nullptr;
    }
    FreeLibrary(module_);
    if (GetModuleHandleW(config_.snippet_path.c_str()) == nullptr) {
      Log(LogLevel::INFO, "snippet unloaded");
    } else {
      Log(LogLevel::WARN, "the snippet module stayed loaded: another reference holds it");
    }
  }

  config_ = {};
  state_ = SnippetState::UNLOADED;
  api_ = SnippetApi::NONE;
  module_ = nullptr;
  device_ = nullptr;
  vk_device_ = VK_NULL_HANDLE;
  init_ext_ = nullptr;
  create_feature_ = nullptr;
  evaluate_feature_ = nullptr;
  release_feature_ = nullptr;
  shutdown_ = nullptr;
  populate_parameters_ = nullptr;
  vk_init_ext2_ = nullptr;
  vk_create_feature_ = nullptr;
  vk_evaluate_feature_ = nullptr;
  vk_release_feature_ = nullptr;
  vk_shutdown_ = nullptr;
  vk_populate_parameters_ = nullptr;
  core_module_ = nullptr;
  core_path_.clear();
  core_initialized_by_uplift_ = false;
  allocate_parameters_ = nullptr;
  destroy_parameters_ = nullptr;
  stats_parameters_ = nullptr;
  stats_callback_ = nullptr;
}

void Snippet::Abandon() {
  if (abandoned_) return;
  abandoned_ = true;
  if (state_ == SnippetState::UNLOADED) return;
  // Capture what the cleanup below needs, then reset every instance field to
  // its final "abandoned" value *before* RestorePatches() or the summary log
  // below run -- both can log, and a throwing sink (log.hpp says none should
  // throw, but Log() only started guarding against it, not preventing it at
  // the source) must never catch this object with some fields reset and
  // others not.
  const bool was_bound = (state_ == SnippetState::BOUND);
  ID3D12Device* const device = device_;
  const HMODULE module = module_;

  config_ = {};
  state_ = SnippetState::UNLOADED;
  api_ = SnippetApi::NONE;
  module_ = nullptr;
  device_ = nullptr;
  vk_device_ = VK_NULL_HANDLE;
  init_ext_ = nullptr;
  create_feature_ = nullptr;
  evaluate_feature_ = nullptr;
  release_feature_ = nullptr;
  shutdown_ = nullptr;
  populate_parameters_ = nullptr;
  vk_init_ext2_ = nullptr;
  vk_create_feature_ = nullptr;
  vk_evaluate_feature_ = nullptr;
  vk_release_feature_ = nullptr;
  vk_shutdown_ = nullptr;
  vk_populate_parameters_ = nullptr;
  core_module_ = nullptr;
  core_path_.clear();
  core_initialized_by_uplift_ = false;
  allocate_parameters_ = nullptr;
  destroy_parameters_ = nullptr;
  stats_parameters_ = nullptr;
  stats_callback_ = nullptr;

  // The restore half of Unload(), and nothing else: never Shutdown1,
  // ReleaseFeature or DestroyParameters, and never FreeLibrary the snippet
  // (or the core) -- both stay mapped, exactly as a leaked Session would have
  // left them, but with both patches safely restored first.
  {
    const std::unique_lock lock(own_runtime_modules.mutex);
    own_runtime_modules.live = nullptr;
    if (std::ranges::find(own_runtime_modules.abandoned, module) == own_runtime_modules.abandoned.end()) {
      own_runtime_modules.abandoned.push_back(module);
    }
  }
  RestorePatches();
  if (was_bound && device != nullptr) {
    // Releasing Uplift's own AddRef on the device is not a call into the
    // snippet or NGX; skipping it would leak the device interface forever.
    device->Release();
  }
  Log(LogLevel::WARN, "snippet abandoned after a device loss: module and core left mapped, no NGX calls made");
}

void Snippet::RestorePatches() {
  if (log_hook.slot != nullptr) {
    void** const slot = log_hook.slot;
    void* const original = log_hook.original;
    const bool ours = (*slot == reinterpret_cast<void*>(&SnippetLog));
    const auto foreign = reinterpret_cast<uintptr_t>(*slot);
    log_hook = {};  // reset before any logging below
    if (ours) {
      if (WritePointer(slot, original)) {
        Log(LogLevel::INFO, "snippet log-callback slot restored");
      } else {
        Log(LogLevel::ERR, "cannot restore the snippet's log-callback slot");
      }
    } else {
      Logf(LogLevel::WARN,
           "the snippet log-callback slot no longer holds Uplift's hook (foreign pointer {:#x}); leaving it alone",
           foreign);
    }
  }
  if (identity_spoof.slot != nullptr) {
    void** const slot = identity_spoof.slot;
    void* const original = identity_spoof.original;
    const bool ours = (*slot == reinterpret_cast<void*>(&SpoofedGetModuleFileNameW));
    const auto foreign = reinterpret_cast<uintptr_t>(*slot);
    identity_spoof = {};  // reset before any logging below
    if (ours) {
      if (WritePointer(slot, original)) {
        Log(LogLevel::INFO, "snippet GetModuleFileNameW import restored");
      } else {
        Log(LogLevel::ERR, "cannot restore the snippet's GetModuleFileNameW import");
      }
    } else {
      Logf(LogLevel::WARN,
           "the snippet's GetModuleFileNameW import no longer holds Uplift's spoof (foreign pointer {:#x}, "
           "spec R4: another NR host); leaving it alone",
           foreign);
    }
  }
}

NVSDK_NGX_Parameter* Snippet::AllocateParameters() const {
  if (state_ != SnippetState::BOUND) return nullptr;
  NVSDK_NGX_Parameter* parameters = nullptr;
  const NVSDK_NGX_Result result =
      GuardedCall([&] { return reinterpret_cast<AllocateParametersFn>(allocate_parameters_)(&parameters); });
  if (NVSDK_NGX_FAILED(result)) {
    Logf(LogLevel::ERR, "AllocateParameters failed with {:#010x}", static_cast<uint32_t>(result));
    return nullptr;
  }
  return parameters;
}

void Snippet::DestroyParameters(NVSDK_NGX_Parameter* parameters) const {
  if (state_ != SnippetState::BOUND || parameters == nullptr) return;
  const NVSDK_NGX_Result result =
      GuardedCall([&] { return reinterpret_cast<DestroyParametersFn>(destroy_parameters_)(parameters); });
  if (NVSDK_NGX_FAILED(result)) {
    Logf(LogLevel::ERR, "DestroyParameters failed with {:#010x}", static_cast<uint32_t>(result));
  }
}

NVSDK_NGX_Result Snippet::CreateFeature(
    ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) const {
  if (state_ != SnippetState::BOUND || api_ != SnippetApi::D3D12) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return GuardedCall([&] {
    return reinterpret_cast<CreateFeatureFn>(create_feature_)(list, NVSDK_NGX_Feature_Reserved18, parameters, handle);
  });
}

NVSDK_NGX_Result Snippet::EvaluateFeature(
    ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) const {
  if (state_ != SnippetState::BOUND || api_ != SnippetApi::D3D12) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return GuardedCall([&] {
    return reinterpret_cast<EvaluateFeatureFn>(evaluate_feature_)(list, handle, parameters, nullptr);
  });
}

NVSDK_NGX_Result Snippet::CreateFeatureVulkan(
    VkCommandBuffer buffer, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) const {
  if (state_ != SnippetState::BOUND || api_ != SnippetApi::VULKAN) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return GuardedCall([&] {
    return reinterpret_cast<VulkanCreateFeatureFn>(vk_create_feature_)(buffer, NVSDK_NGX_Feature_Reserved18, parameters, handle);
  });
}

NVSDK_NGX_Result Snippet::EvaluateFeatureVulkan(
    VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) const {
  if (state_ != SnippetState::BOUND || api_ != SnippetApi::VULKAN) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return GuardedCall([&] {
    return reinterpret_cast<VulkanEvaluateFeatureFn>(vk_evaluate_feature_)(buffer, handle, parameters, nullptr);
  });
}

NVSDK_NGX_Result Snippet::ReleaseFeature(NVSDK_NGX_Handle* handle) const {
  if (state_ != SnippetState::BOUND) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return GuardedCall([&] { return reinterpret_cast<ReleaseFeatureFn>(release_feature_)(handle); });
}

std::optional<uint64_t> Snippet::QueryAllocatedBytes() const {
  if (state_ != SnippetState::BOUND || stats_callback_ == nullptr) return std::nullopt;
  unsigned long long bytes = 0u;
  // A failed call leaves SizeInBytes untouched, so a previous value would be stale.
  const NVSDK_NGX_Result result = GuardedCall([&] {
    const NVSDK_NGX_Result queried = reinterpret_cast<StatsCallbackFn>(stats_callback_)(stats_parameters_);
    if (NVSDK_NGX_FAILED(queried)) return queried;
    return stats_parameters_->Get("SizeInBytes", &bytes);
  });
  if (NVSDK_NGX_FAILED(result)) return std::nullopt;
  return bytes;
}

bool Snippet::AttachCore(ID3D12Device* device) {
  // Once per device: the mark on the device itself keeps a core from being initialised twice for it.
  const auto initialize_once = [&](CoreInit initialize) {
    uint32_t marker = 0u;
    UINT marker_size = sizeof(marker);
    if (SUCCEEDED(device->GetPrivateData(UPLIFT_CORE_INIT_GUID, &marker_size, &marker))) return true;
    if (config_.application_data_path.empty()) {
      Log(LogLevel::ERR, "cannot initialise the NGX core: no application-data path is configured");
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories(config_.application_data_path, error);
    if (error) {
      Logf(LogLevel::ERR, "cannot create the NGX application-data directory {} ({})",
           ToUtf8(config_.application_data_path), error.message());
      return false;
    }
    const NVSDK_NGX_Result result = GuardedCall([&] {
      return initialize(APPLICATION_ID, config_.application_data_path.c_str(), device, NVSDK_NGX_Version_API);
    });
    if (NVSDK_NGX_FAILED(result)) {
      Logf(LogLevel::ERR, "NGX core Init failed with {:#010x}", static_cast<uint32_t>(result));
      return false;
    }
    constexpr uint32_t INITIALIZED = 1u;
    device->SetPrivateData(UPLIFT_CORE_INIT_GUID, sizeof(INITIALIZED), &INITIALIZED);
    return true;
  };

  for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
    HMODULE loaded = nullptr;
    if (GetModuleHandleExW(0u, name, &loaded) == FALSE) continue;
    ModuleReference core(loaded);
    void* const allocate = ResolveExport(core.get(), "NVSDK_NGX_D3D12_AllocateParameters");
    void* const destroy = ResolveExport(core.get(), "NVSDK_NGX_D3D12_DestroyParameters");
    if (allocate == nullptr || destroy == nullptr) continue;
    core_module_ = core.release();
    allocate_parameters_ = allocate;
    destroy_parameters_ = destroy;
    break;
  }

  if (core_module_ != nullptr && config_.initialize_core_for_device) {
    // Plan 7 (D3D11 design R15): a D3D11 game's core was initialised for its own D3D11 device only.
    const auto initialize = reinterpret_cast<CoreInit>(ResolveExport(core_module_, "NVSDK_NGX_D3D12_Init"));
    if (initialize == nullptr || !initialize_once(initialize)) {
      Log(LogLevel::ERR, "the game's NGX core could not be initialised for Uplift's private Direct3D 12 device");
      ReleaseCore();
      return false;
    }
    Log(LogLevel::INFO, "the game's NGX core is initialised for Uplift's private Direct3D 12 device");
  }

  if (core_module_ == nullptr) {
    if (!config_.allow_core_force_load) {
      Log(LogLevel::ERR, "no loaded NGX core exports D3D12 parameter allocation, and force-loading is disabled");
      return false;
    }
    const auto core_file = FindNgxCore(config_.core_path_override);
    if (core_file.empty()) {
      Log(LogLevel::ERR, "could not locate an installed NVIDIA NGX core to force-load");
      return false;
    }
    ModuleReference core(LoadLibraryExW(core_file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (!core) {
      const DWORD load_error = GetLastError();
      Logf(LogLevel::ERR, "LoadLibraryExW failed for NGX core {} ({})", ToUtf8(core_file), load_error);
      return false;
    }
    const auto initialize = reinterpret_cast<CoreInit>(ResolveExport(core.get(), "NVSDK_NGX_D3D12_Init"));
    void* const allocate = ResolveExport(core.get(), "NVSDK_NGX_D3D12_AllocateParameters");
    void* const destroy = ResolveExport(core.get(), "NVSDK_NGX_D3D12_DestroyParameters");
    if (initialize == nullptr || allocate == nullptr || destroy == nullptr) {
      Logf(LogLevel::ERR, "NGX core {} lacks D3D12 Init or parameter allocation", ToUtf8(core_file));
      return false;
    }
    if (!initialize_once(initialize)) return false;
    // Never FreeLibrary this core or call its Shutdown1: Shutdown1 after the
    // snippet's teardown crashed in _nvngx.dll+0x44e54 (driver 617.14). It stays
    // initialised for the device's lifetime, as a game would leave it.
    core_module_ = core.release();
    allocate_parameters_ = allocate;
    destroy_parameters_ = destroy;
    core_initialized_by_uplift_ = true;
  }

  std::array<wchar_t, 32768u> module_path = {};
  const DWORD length = GetModuleFileNameW(core_module_, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length != 0u && length < module_path.size()) {
    core_path_ = std::filesystem::path(module_path.data(), module_path.data() + length);
  }
  Logf(LogLevel::INFO, "{} NGX core {}", (core_initialized_by_uplift_ ? "force-loaded" : "adopted the loaded"),
       ToUtf8(core_path_));
  return true;
}

bool Snippet::AttachVulkanCore(const VulkanBinding& binding) {
  // Once per VkDevice: the process-wide set keeps a core from being initialised twice for one device (D3D12 keeps its mark in the device's
  // private data; a VkDevice has none). Snippet is serialised by its caller, so a check-then-insert is safe.
  const auto initialize_once = [&](VulkanInitExt2 initialize) {
    {
      const std::scoped_lock lock(vulkan_core_devices.mutex);
      if (std::ranges::find(vulkan_core_devices.devices, binding.device) != vulkan_core_devices.devices.end()) return true;
    }
    if (config_.application_data_path.empty()) {
      Log(LogLevel::ERR, "cannot initialise the NGX core: no application-data path is configured");
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories(config_.application_data_path, error);
    if (error) {
      Logf(LogLevel::ERR, "cannot create the NGX application-data directory {} ({})", ToUtf8(config_.application_data_path),
           error.message());
      return false;
    }
    const NVSDK_NGX_Result result = GuardedCall([&] {
      return initialize(APPLICATION_ID, config_.application_data_path.c_str(), binding.instance, binding.physical, binding.device,
                        binding.get_instance_proc_addr, binding.get_device_proc_addr, NVSDK_NGX_Version_API, nullptr);
    });
    if (NVSDK_NGX_FAILED(result)) {
      Logf(LogLevel::ERR, "NGX core VULKAN_Init_Ext2 failed with {:#010x}", static_cast<uint32_t>(result));
      return false;
    }
    const std::scoped_lock lock(vulkan_core_devices.mutex);
    vulkan_core_devices.devices.push_back(binding.device);
    return true;
  };

  for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
    HMODULE loaded = nullptr;
    if (GetModuleHandleExW(0u, name, &loaded) == FALSE) continue;
    ModuleReference core(loaded);
    void* const allocate = ResolveExport(core.get(), "NVSDK_NGX_VULKAN_AllocateParameters");
    void* const destroy = ResolveExport(core.get(), "NVSDK_NGX_VULKAN_DestroyParameters");
    if (allocate == nullptr || destroy == nullptr) continue;
    core_module_ = core.release();
    allocate_parameters_ = allocate;
    destroy_parameters_ = destroy;
    break;
  }

  if (core_module_ != nullptr && config_.initialize_core_for_device) {
    const auto initialize = reinterpret_cast<VulkanInitExt2>(ResolveExport(core_module_, "NVSDK_NGX_VULKAN_Init_Ext2"));
    if (initialize == nullptr || !initialize_once(initialize)) {
      Log(LogLevel::ERR, "the loaded NGX core could not be initialised for this Vulkan device");
      ReleaseCore();
      return false;
    }
  }

  if (core_module_ == nullptr) {
    if (!config_.allow_core_force_load) {
      Log(LogLevel::ERR, "no loaded NGX core exports Vulkan parameter allocation, and force-loading is disabled");
      return false;
    }
    const auto core_file = FindNgxCore(config_.core_path_override);
    if (core_file.empty()) {
      Log(LogLevel::ERR, "could not locate an installed NVIDIA NGX core to force-load");
      return false;
    }
    ModuleReference core(LoadLibraryExW(core_file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (!core) {
      const DWORD load_error = GetLastError();
      Logf(LogLevel::ERR, "LoadLibraryExW failed for NGX core {} ({})", ToUtf8(core_file), load_error);
      return false;
    }
    const auto initialize = reinterpret_cast<VulkanInitExt2>(ResolveExport(core.get(), "NVSDK_NGX_VULKAN_Init_Ext2"));
    void* const allocate = ResolveExport(core.get(), "NVSDK_NGX_VULKAN_AllocateParameters");
    void* const destroy = ResolveExport(core.get(), "NVSDK_NGX_VULKAN_DestroyParameters");
    if (initialize == nullptr || allocate == nullptr || destroy == nullptr) {
      Logf(LogLevel::ERR, "NGX core {} lacks Vulkan Init or parameter allocation", ToUtf8(core_file));
      return false;
    }
    if (!initialize_once(initialize)) return false;
    // Never FreeLibrary this core or call its Shutdown1 (spec §6.1; Shutdown1 after the snippet's teardown crashed on D3D12, driver 617.14):
    // it stays initialised for the device's lifetime, as a game would leave it.
    core_module_ = core.release();
    allocate_parameters_ = allocate;
    destroy_parameters_ = destroy;
    core_initialized_by_uplift_ = true;
  }

  std::array<wchar_t, 32768u> module_path = {};
  const DWORD length = GetModuleFileNameW(core_module_, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length != 0u && length < module_path.size()) {
    core_path_ = std::filesystem::path(module_path.data(), module_path.data() + length);
  }
  Logf(LogLevel::INFO, "{} NGX core {}", (core_initialized_by_uplift_ ? "force-loaded" : "adopted the loaded"), ToUtf8(core_path_));
  return true;
}

void Snippet::ReleaseCore() {
  if (core_module_ != nullptr && !core_initialized_by_uplift_) {
    FreeLibrary(core_module_);
  }
  core_module_ = nullptr;
  core_path_.clear();
  core_initialized_by_uplift_ = false;
  allocate_parameters_ = nullptr;
  destroy_parameters_ = nullptr;
}

void Snippet::PrepareStats() {
  if (populate_parameters_ == nullptr) {
    Logf(LogLevel::INFO, "stats unavailable: no NVSDK_NGX_{}_PopulateParameters_Impl export",
         (api_ == SnippetApi::VULKAN ? "VULKAN" : "D3D12"));
    return;
  }
  NVSDK_NGX_Parameter* const parameters = AllocateParameters();
  if (parameters == nullptr) {
    Log(LogLevel::INFO, "stats unavailable: no parameter block");
    return;
  }
  void* callback = nullptr;
  // PopulateParameters_Impl serves only a caller named nvngx.dll, which the
  // identity spoof provides.
  const NVSDK_NGX_Result result = GuardedCall([&] {
    const NVSDK_NGX_Result populated = reinterpret_cast<PopulateParametersFn>(populate_parameters_)(parameters);
    if (NVSDK_NGX_FAILED(populated)) return populated;
    return parameters->Get("DLSSNRGetStatsCallback", &callback);
  });
  if (NVSDK_NGX_FAILED(result) || callback == nullptr) {
    DestroyParameters(parameters);
    Logf(LogLevel::INFO, "stats unavailable ({:#010x})", static_cast<uint32_t>(result));
    return;
  }
  stats_parameters_ = parameters;
  stats_callback_ = callback;
}

}  // namespace uplift::nr
