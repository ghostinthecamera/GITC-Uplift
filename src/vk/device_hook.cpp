#include "vk/device_hook.hpp"

#include <Windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "addon/removal_latch.hpp"
#include "ngx_hooks/minhook_once.hpp"
#include "nr/log.hpp"
#include "vk/device_adjustment.hpp"
#include "vk/loader.hpp"
#include "vk/marker_ledger.hpp"
#if defined(_WIN64)
#include "nr/snippet.hpp"
#endif

namespace uplift::vk {
namespace {

struct Entry {
  DeviceRecord record;
  uint32_t presents = 0u;  // present events seen, counted up to 2 (the pending marker's proof)
};

struct Config {
  bool adjust = true;
  bool adjust_ngx = true;  // Plan 19 T1b: AdjustVulkanDevicesForNgx, and not turned off by the marker's first tier (64-bit only)
  bool native_nr_chosen = true;  // T5: VulkanNr is Native as read at AddonInit; NGX's additions only then (64-bit only)
  bool native_nr_latched = false;  // T6 fix round (M-2): VulkanNativeNr is 0 as read at AddonInit: no NGX additions either
  bool reshade_adds_memory_win32 = false;
  bool turned_off_by_marker = false;
  bool ngx_turned_off_by_marker = false;
  std::filesystem::path pending_marker;
};

// Plan 19 T1b: the pending marker's two texts. One that names NGX's additions is the first tier: the next start drops only those.
constexpr std::string_view MARKER_TEXT = "a Vulkan device was adjusted; waiting for its first frame";
constexpr std::string_view MARKER_NGX_TEXT = "a Vulkan device was adjusted with NGX's additions; waiting for its first frame";

struct State {
  std::shared_mutex mutex;  // its own: never the add-on's (Plan 10 C-1)
  Config config;
  std::unordered_map<VkDevice, Entry> devices;
  // The pending marker (design §2.3) is one file for every device of the process: who it waits on is this ledger's business (batch 2 review I-3).
  MarkerLedger ledger;
  // Plan 13 (64-bit): each physical device's instance, from the enumerate detours (the latest call wins), and why those detours or the
  // vkDestroyDevice one are not in (empty once all three are).
  std::unordered_map<VkPhysicalDevice, VkInstance> instances;
  std::string native_hooks_error = "the Vulkan device hook is not installed";
  uint32_t instance_api_version = 0u;  // Plan 19: the latest create_device(vulkan)'s, as ReShade codes it; 0 = none seen
};

// Never freed: the detour can run until the process ends.
State& Shared() {
  static State* const state = new State();
  return *state;
}

std::atomic<PFN_vkCreateDevice> g_original{nullptr};
std::atomic<bool> g_installed{false};
#if defined(_WIN64)
std::atomic<PFN_vkEnumeratePhysicalDevices> g_enumerate_original{nullptr};
std::atomic<PFN_vkEnumeratePhysicalDeviceGroups> g_enumerate_groups_original{nullptr};
std::atomic<PFN_vkDestroyDevice> g_destroy_original{nullptr};
std::atomic<void (*)(VkDevice)> g_destroy_callback{nullptr};
// Plan 13: NGX's four device extensions (the snippet's own requirement list, design §1.1); native NR needs them on the device (listed by the game, or
// added by Uplift, Plan 19).
constexpr const char* NGX_EXTENSIONS[] = {"VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_buffer_device_address",
                                          "VK_KHR_push_descriptor"};
// What NGX needs that a request lacks, named ("" for nothing): its four extensions on `names` (VK_KHR_push_descriptor also when ReShade appends it),
// bufferDeviceAddress, and timeline semaphores.
std::string NgxLacks(std::span<const char* const> names, bool push_from_reshade, bool buffer_device_address, bool timeline) {
  std::string lacks;
  for (const char* extension : NGX_EXTENSIONS) {
    const bool listed = std::ranges::any_of(names, [extension](const char* name) { return std::string_view(name) == extension; })
                        || (push_from_reshade && std::string_view(extension) == "VK_KHR_push_descriptor");
    if (!listed) {
      lacks += std::format("{}{}", (lacks.empty() ? "" : ", "), extension);
    }
  }
  if (!buffer_device_address) {
    lacks += std::format("{}bufferDeviceAddress", (lacks.empty() ? "" : ", "));
  }
  if (!timeline) {
    lacks += std::format("{}timeline semaphores", (lacks.empty() ? "" : ", "));
  }
  return lacks;
}

// Plan 19 (a PS5 emulator crashed inside vkCreateDevice with NGX's additions): a request with Uplift's additions is logged before it goes to the
// driver and again when the driver returns. Log lines are flushed one by one, so a crash in the driver leaves the first without the second. The
// feature chain is the final pNext chain's sTypes, by number; `api_version` is the instance's, as ReShade codes it (0: none seen). Never throw.
void LogBeforeCreate(const VkDeviceCreateInfo& request, std::string_view additions, uint32_t api_version) noexcept {
  try {
    std::string chain;
    for (const auto* link = static_cast<const VkBaseInStructure*>(request.pNext); link != nullptr; link = link->pNext) {
      chain += std::format("{}{}", (chain.empty() ? "" : ", "), static_cast<uint32_t>(link->sType));
    }
    const std::string api = (api_version == 0u ? std::string("unknown") : std::format("{}.{}", api_version >> 12u, (api_version >> 8u) & 0xFu));
    nr::Logf(nr::LogLevel::INFO, "Vulkan: creating the game's device with Uplift's additions: {}; feature chain: {}; apiVersion {}", additions,
             (chain.empty() ? std::string("none") : chain), api);
  } catch (...) {
    // No-op: the game's device is created whether or not the line was written.
  }
}

void LogAfterCreate(VkResult result) noexcept {
  try {
    nr::Logf(nr::LogLevel::INFO, "Vulkan: the driver returned VkResult {}", static_cast<int>(result));
  } catch (...) {
    // No-op: as LogBeforeCreate.
  }
}
#endif

constexpr uint32_t RESHADE_VULKAN_1_4 = (1u << 12u) | (4u << 8u);  // Plan 19: ReShade's create_device coding of Vulkan 1.4

// The marker's path for NoteProcessExit, which cannot take the mutex (ExitProcess may have ended a thread that held it): a plain buffer, filled
// by Configure before any device exists, whether the marker is on disk, and how many adjusted creations are between the marker's write and the
// original's return (the ledger's reservations, mirrored: a thread ExitProcess killed inside vkCreateDevice is exactly such a creation).
wchar_t g_marker_file[1024] = {};
std::atomic<bool> g_marker_on_disk{false};
std::atomic<uint32_t> g_creations_under_way{0u};

void DeleteMarker(const std::filesystem::path& marker) {
  if (!marker.empty()) {
    addon::DeleteLatchMarker(marker);
  }
  g_marker_on_disk.store(false, std::memory_order_release);
}

// Plan 19: as ReShade picks its primary graphics queue's family (vulkan_hooks_device.cpp:138-152): the first family in the request with a queue
// that supports graphics. UINT32_MAX when there is none or the loader cannot tell.
uint32_t GraphicsFamily(VkPhysicalDevice physical, const VkDeviceCreateInfo& info) {
  const Loader* const loader = Loader::Get();
  if (loader == nullptr || loader->get_queue_family_properties == nullptr || info.pQueueCreateInfos == nullptr) return UINT32_MAX;
  uint32_t count = 0u;
  loader->get_queue_family_properties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  loader->get_queue_family_properties(physical, &count, families.data());
  for (uint32_t i = 0u; i < info.queueCreateInfoCount; ++i) {
    const VkDeviceQueueCreateInfo& queue = info.pQueueCreateInfos[i];
    if (queue.queueFamilyIndex < count && queue.queueCount > 0u && (families[queue.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
      return queue.queueFamilyIndex;
    }
  }
  return UINT32_MAX;
}

// Vulkan design §2.2. Runs on the game's thread inside its vkCreateDevice: it never throws, holds no lock across the original, and falls back to
// the game's own request when the driver refuses the additions (R49).
VkResult VKAPI_CALL CreateDeviceDetour(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                       VkDevice* device) {
  const PFN_vkCreateDevice original = g_original.load(std::memory_order_acquire);
  if (original == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  if (info == nullptr) return original(physical, info, allocator, device);

  std::vector<const char*> names;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
      .timelineSemaphore = VK_TRUE,
  };
  VkDeviceCreateInfo adjusted = *info;  // the game's memory is never written
#if defined(_WIN64)
  VkPhysicalDeviceFeatures features = {};  // Plan 13: a copy of the game's features with shaderStorageImageExtendedFormats on, when added
  // Plan 19 (native NR at Present): Uplift's own bufferDeviceAddress struct, and the request without NGX's additions, which a refusal tries first
  // (R49), with what the device has then. `ngx_added`: Uplift added something for NGX. The lines are logged once the original returned.
  VkPhysicalDeviceBufferDeviceAddressFeatures address = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
      .bufferDeviceAddress = VK_TRUE,
  };
  VkDeviceCreateInfo without_ngx = *info;
  DeviceRecord without_ngx_record;
  std::string without_ngx_added_text;
  bool without_ngx_features = false;
  bool ngx_added = false;
  bool game_ngx_ready = false;  // the game's own request has everything NGX needs
  std::string ngx_line;         // what NGX needs that the game's own request lacks (every 64-bit device)
  std::string ngx_notes;        // why one of NGX's additions was not made
  uint32_t api_version = 0u;    // the instance's, for the lines around the original
#endif
  DeviceRecord record = {.physical = physical};
  std::string added_text;
  MarkerLedger::Reservation reservation;  // this creation's share of the pending marker, taken before the original runs
  bool failed = false;                    // the adjustment threw: the game's own request goes through
  // Batch 3 review, minor 6: Uplift turned shaderStorageImageExtendedFormats on (Plan 13). It changes the request, so a refusal retries without it, but
  // it is not `record.adjusted`: a feature bit the device already supports never counts toward the pending marker or its presents.
  bool features_added = false;
  try {
    Config config;
    {
      const std::shared_lock lock(Shared().mutex);
      config = Shared().config;
#if defined(_WIN64)
      api_version = Shared().instance_api_version;
#endif
    }
    const Loader* const loader = Loader::Get();
    if (loader == nullptr) throw std::logic_error("vulkan-1.dll is not loaded");  // caught below: the game's own request goes through
    uint32_t count = 0u;
    loader->enumerate_device_extensions(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    loader->enumerate_device_extensions(physical, nullptr, &count, properties.data());
    std::vector<std::string> offered;
    for (const VkExtensionProperties& property : std::span<const VkExtensionProperties>(properties.data(), count)) {
      offered.emplace_back(property.extensionName);
    }
    bool vulkan12_features = false;
    bool timeline_features = false;
    // Plan 13: what native NR needs of the game's own request (bufferDeviceAddress, and the extended storage formats in either feature form).
    bool buffer_device_address = false;
    bool features2 = false;
    bool buffer_device_address_features = false;  // Plan 19: the game passes a bufferDeviceAddress struct of its own (the core one or the EXT one)
    bool storage_extended_by_game = (info->pEnabledFeatures != nullptr && info->pEnabledFeatures->shaderStorageImageExtendedFormats == VK_TRUE);
    for (const auto* link = static_cast<const VkBaseInStructure*>(info->pNext); link != nullptr; link = link->pNext) {
      vulkan12_features = vulkan12_features || (link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
      timeline_features = timeline_features || (link->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
      switch (link->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
          buffer_device_address = buffer_device_address || (reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(link)->bufferDeviceAddress == VK_TRUE);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
          buffer_device_address_features = true;
          buffer_device_address = buffer_device_address
                                  || (reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(link)->bufferDeviceAddress == VK_TRUE);
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT:
          buffer_device_address_features = true;
          break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
          features2 = true;
          storage_extended_by_game = storage_extended_by_game
                                     || (reinterpret_cast<const VkPhysicalDeviceFeatures2*>(link)->features.shaderStorageImageExtendedFormats == VK_TRUE);
          break;
        default: break;
      }
    }
    // Plan 19 (64-bit only: native NR): NGX's additions whenever the device is adjusted, unless AdjustVulkanDevicesForNgx is 0 (T1b: the marker's
    // first tier sets it) or VulkanNr was not Native at AddonInit (T5: the other routes never need them, and a PS5 emulator crashed inside vkCreateDevice
    // with them). Whether the device supports bufferDeviceAddress, asked only when Uplift may chain the feature, and whether ReShade appends
    // VK_KHR_push_descriptor itself (an instance below 1.4; it does so without looking at the list).
#if defined(_WIN64)
    const bool add_ngx = (config.adjust && config.adjust_ngx && config.native_nr_chosen && !config.native_nr_latched);
    const bool reshade_push = (api_version < RESHADE_VULKAN_1_4
                               && std::ranges::find(offered, std::string_view(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) != offered.end());
#else
    const bool add_ngx = false;
    const bool reshade_push = false;
#endif
    bool address_supported = false;
    if (add_ngx && !vulkan12_features && !buffer_device_address_features && loader->get_physical_device_features2 != nullptr) {
      VkPhysicalDeviceBufferDeviceAddressFeatures query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
      VkPhysicalDeviceFeatures2 query_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &query};
      loader->get_physical_device_features2(physical, &query_features);
      address_supported = (query.bufferDeviceAddress == VK_TRUE);
    }
    const DeviceAdjustment plan = BuildDeviceAdjustment({
        .enabled = std::span<const char* const>(info->ppEnabledExtensionNames, info->enabledExtensionCount),
        .offered = offered,
        .vulkan12_features = vulkan12_features,
        .timeline_features = timeline_features,
        .adjust = config.adjust,
        .reshade_adds_memory_win32 = config.reshade_adds_memory_win32,
        .add_ngx = add_ngx,
        .reshade_adds_push_descriptor = reshade_push,
        .buffer_device_address_features = buffer_device_address_features,
        .buffer_device_address_supported = address_supported,
    });
    names.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
    names.insert(names.end(), plan.added.begin(), plan.added.end());
#if defined(_WIN64)
    const size_t names_without_ngx = names.size();  // Plan 19: the request without NGX's additions lists this prefix
    names.insert(names.end(), plan.ngx_added.begin(), plan.ngx_added.end());
#endif
    for (const char* name : plan.added) {
      added_text += (added_text.empty() ? "" : ", ");
      added_text += name;
    }
    if (plan.chain_timeline_features) {
      timeline.pNext = const_cast<void*>(info->pNext);
      adjusted.pNext = &timeline;
      added_text += (added_text.empty() ? "" : ", ");
      added_text += "timeline semaphores";
    }
#if defined(_WIN64)
    without_ngx.pNext = adjusted.pNext;
    without_ngx.enabledExtensionCount = static_cast<uint32_t>(names_without_ngx);
    without_ngx.ppEnabledExtensionNames = names.data();
    if (plan.chain_buffer_device_address) {
      // In front of the timeline struct (or of the game's chain), so the request without NGX's additions keeps the chain it had.
      address.pNext = const_cast<void*>(adjusted.pNext);
      adjusted.pNext = &address;
    }
    ngx_added = (!plan.ngx_added.empty() || plan.chain_buffer_device_address);
#endif
    adjusted.enabledExtensionCount = static_cast<uint32_t>(names.size());
    adjusted.ppEnabledExtensionNames = names.data();
    record = {
        .physical = physical,
        .memory_win32 = plan.memory_win32,
        .semaphore_win32 = plan.semaphore_win32,
        .timeline = plan.timeline,
        .adjusted = (!plan.added.empty() || plan.chain_timeline_features),
    };
#if defined(_WIN64)
    const bool adjusted_without_ngx = record.adjusted;
    record.adjusted = (adjusted_without_ngx || ngx_added);  // Plan 19: NGX's additions count toward the pending marker too
#endif
    if (!(plan.semaphore_win32 && plan.timeline)) {
      const char* const missing = (!plan.semaphore_win32 ? "VK_KHR_external_semaphore_win32" : "timeline semaphores");
      const char* cause = "this driver does not offer it";
      if (!config.adjust) {
        cause = (config.turned_off_by_marker ? "Uplift turned its adjustment off: the last start with it did not reach its first frame (set AdjustVulkanDevices=1 under [Uplift] to try again)"
                                             : "AdjustVulkanDevices is 0 in ReShade.ini");
      }
      record.not_adjusted = std::format("the game's device was created without {} because {}", missing, cause);
    }
#if defined(_WIN64)
    // Plan 13 (design §3.6): native NR's facts, from the request as it goes to the driver: NGX's extensions and bufferDeviceAddress (the game's own;
    // only a DLSS game, which needs the same, has a DLSS placement; Plan 19: with Uplift's NGX additions, and VK_KHR_push_descriptor when ReShade
    // appends it), and timeline semaphores.
    const std::span<const char* const> game_names(info->ppEnabledExtensionNames, info->enabledExtensionCount);
    std::string ngx_missing = NgxLacks(names, reshade_push, (buffer_device_address || plan.chain_buffer_device_address), record.timeline);
    record.ngx_ready = ngx_missing.empty();
    if (!record.ngx_ready && config.adjust && !config.adjust_ngx) {
      // T1b: why Uplift added none of it (the native route's reason names it).
      ngx_missing += (config.ngx_turned_off_by_marker ? "; Uplift turned NGX's device additions off: the last start with them did not reach its first frame "
                                                        "(set AdjustVulkanDevicesForNgx=1 under [Uplift] to try again)"
                                                      : "; AdjustVulkanDevicesForNgx is 0 in ReShade.ini");
    } else if (!record.ngx_ready && config.adjust && !config.native_nr_chosen) {
      // T5: the panel greys Native with this, and the choice returns after a restart with Native.
      ngx_missing += "; Vulkan NR at Present was not Native when the game started, so Uplift added none of them: Native needs a game restart";
    } else if (!record.ngx_ready && config.adjust && config.native_nr_latched) {
      // T6 fix round (M-2): Uplift's own latch, which a restart alone does not lift.
      ngx_missing += "; native Vulkan NR was off when the game started (VulkanNativeNr = 0), so Uplift added none of them: press Clear latch, or set "
                     "VulkanNativeNr=1 under [Uplift], and restart the game";
    }
    record.ngx_missing = std::move(ngx_missing);
    // What NGX needs that the game's own request lacks, and which of those extensions the driver offers (one line for every device).
    if (const std::string lacks = NgxLacks(game_names, false, buffer_device_address, (vulkan12_features || timeline_features)); lacks.empty()) {
      game_ngx_ready = true;
      ngx_line = "Vulkan: the game's device request has everything NGX needs";
    } else {
      std::string offered_text;
      for (const char* extension : NGX_EXTENSIONS) {
        const bool listed = std::ranges::any_of(game_names, [extension](const char* name) { return std::string_view(name) == extension; });
        if (!listed && std::ranges::find(offered, std::string_view(extension)) != offered.end()) {
          offered_text += std::format("{}{}", (offered_text.empty() ? "" : ", "), extension);
        }
      }
      ngx_line = std::format("Vulkan: for NGX the game's device request lacks {}; the driver offers {}", lacks,
                             (offered_text.empty() ? std::string("none of those extensions") : offered_text));
    }
    {
      const std::shared_lock lock(Shared().mutex);
      const auto found = Shared().instances.find(physical);
      record.instance = (found == Shared().instances.end() ? VK_NULL_HANDLE : found->second);
    }
    // Batch 2 review, minor 2: every native encode shader writes an RG16F storage image, an extended storage format. On a device that can run
    // native NR the feature is turned on here, as the timeline feature is, unless the game passes VkPhysicalDeviceFeatures2 (whose node Uplift
    // cannot copy without rebuilding the game's chain): Vulkan's own SPIR-V rules make StorageImageExtendedFormats a 1.0 capability, so a
    // device whose physical device supports the formats still runs the shaders then.
    VkPhysicalDeviceFeatures supported = {};
    if (loader->get_physical_device_features != nullptr) {
      loader->get_physical_device_features(physical, &supported);
    }
    record.storage_extended_supported = (supported.shaderStorageImageExtendedFormats == VK_TRUE);
    record.storage_extended_enabled = storage_extended_by_game;
    if (config.adjust && record.ngx_ready && record.storage_extended_supported && !storage_extended_by_game && !features2) {
      features = (info->pEnabledFeatures != nullptr ? *info->pEnabledFeatures : VkPhysicalDeviceFeatures{});
      features.shaderStorageImageExtendedFormats = VK_TRUE;
      adjusted.pEnabledFeatures = &features;
      record.storage_extended_enabled = true;
      features_added = true;
      added_text += (added_text.empty() ? "" : ", ");
      added_text += "shaderStorageImageExtendedFormats";
    }
    // Plan 19: the request without NGX's additions (R49's first retry), and what the device has then. The extended storage formats stay in it only
    // when that request alone is NGX-ready (they were added for native NR).
    if (ngx_added) {
      std::string lacks = NgxLacks(std::span<const char* const>(names.data(), names_without_ngx), reshade_push, buffer_device_address, record.timeline);
      without_ngx_features = (features_added && lacks.empty());
      without_ngx.pEnabledFeatures = (without_ngx_features ? adjusted.pEnabledFeatures : info->pEnabledFeatures);
      without_ngx_record = record;
      without_ngx_record.adjusted = adjusted_without_ngx;
      without_ngx_record.ngx_ready = lacks.empty();
      without_ngx_record.ngx_missing = std::move(lacks);
      without_ngx_record.storage_extended_enabled = (storage_extended_by_game || without_ngx_features);
      without_ngx_added_text = added_text;
      if (features_added && !without_ngx_features) {
        without_ngx_added_text.resize(without_ngx_added_text.size() - std::string_view("shaderStorageImageExtendedFormats").size());
        if (without_ngx_added_text.ends_with(", ")) {
          without_ngx_added_text.resize(without_ngx_added_text.size() - 2u);
        }
      }
    }
    // Uplift's NGX additions join the "added" line (after what the request without them has), and the notes say why one was not made.
    for (const char* name : plan.ngx_added) {
      added_text += std::format("{}{}", (added_text.empty() ? "" : ", "), name);
    }
    if (plan.chain_buffer_device_address) {
      added_text += std::format("{}bufferDeviceAddress", (added_text.empty() ? "" : ", "));
    }
    const auto game_lists = [&game_names](std::string_view extension) {
      return std::ranges::any_of(game_names, [extension](const char* name) { return std::string_view(name) == extension; });
    };
    const auto note = [&ngx_notes](std::string_view why) { ngx_notes += std::format("{}{}", (ngx_notes.empty() ? "" : "; "), why); };
    if (!config.adjust) {
      note(config.turned_off_by_marker ? "Uplift turned the device adjustment off (the last start with it did not reach its first frame)"
                                       : "AdjustVulkanDevices is 0 in ReShade.ini");
    } else if (config.adjust_ngx && config.native_nr_chosen && !config.native_nr_latched) {  // otherwise `ngx_missing` names why nothing of NGX's was added
      if (game_lists(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
        note("the game lists VK_EXT_buffer_device_address, which excludes VK_KHR_buffer_device_address and its feature struct");
      } else if ((vulkan12_features || buffer_device_address_features) && !buffer_device_address) {
        note("the game's own feature struct has bufferDeviceAddress off, and Uplift never writes the game's memory");
      } else if (!vulkan12_features && !buffer_device_address_features && !address_supported) {
        note("the driver does not report bufferDeviceAddress");
      }
      if (reshade_push && !game_lists(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        note("ReShade appends VK_KHR_push_descriptor itself");
      }
    }
#endif
    // Final review, minor 1: only a device that can present counts toward the marker (ReShade itself skips a device without VK_KHR_swapchain).
    // A compute or UI device that a D3D game keeps alive, or a game that later crashes for another reason, must not turn the adjustment off.
    if (record.adjusted && plan.presents && !config.pending_marker.empty()) {
      // Before the original runs, so a crash inside it leaves the marker. Once an adjusted device has presented twice nothing is written: the
      // adjustment is proven for this game, and a later device that never presents must not leave a marker behind (I-3).
      const std::unique_lock lock(Shared().mutex);
#if defined(_WIN64)
      reservation = Shared().ledger.Reserve(ngx_added);  // T1b: the file says so once a device with NGX's additions waits on it
#else
      reservation = Shared().ledger.Reserve();
#endif
      if (reservation.counted) {
        g_creations_under_way.fetch_add(1u, std::memory_order_acq_rel);
      }
      if (reservation.write) {
        addon::WriteLatchMarker(config.pending_marker, (reservation.ngx ? MARKER_NGX_TEXT : MARKER_TEXT));
        g_marker_on_disk.store(true, std::memory_order_release);
      }
    }
  } catch (...) {
    // Plain flags only: nothing in a handler, or before the bookkeeping below, may allocate, so nothing escapes into the game's vkCreateDevice
    // (batch 2 review, minor 8).
    adjusted = *info;  // never fail the game's device because of Uplift
    failed = true;
    features_added = false;
    record.adjusted = false;
    record.memory_win32 = false;
    record.semaphore_win32 = false;
    record.timeline = false;
    record.ngx_ready = false;
    record.storage_extended_enabled = false;
    added_text.clear();
#if defined(_WIN64)
    ngx_added = false;
    ngx_line.clear();
    ngx_notes.clear();
#endif
  }

  bool adjusted_device = record.adjusted;
  bool refused = false;
#if defined(_WIN64)
  // Plan 19: a request with Uplift's additions goes to the driver between LogBeforeCreate's line and LogAfterCreate's (the game's own goes without).
  const auto create = [&](const VkDeviceCreateInfo& request, const std::string& additions) {
    if (additions.empty()) return original(physical, &request, allocator, device);
    LogBeforeCreate(request, additions, api_version);
    const VkResult created = original(physical, &request, allocator, device);
    LogAfterCreate(created);
    return created;
  };
  VkResult result = create(adjusted, added_text);
  // Plan 19 (R49): a refusal of the request with NGX's additions tries it without them first, then (below) the game's own request.
  VkResult ngx_refusal = VK_SUCCESS;
  if (result < VK_SUCCESS && ngx_added) {
    ngx_refusal = result;
    result = create(without_ngx, without_ngx_added_text);
    if (result >= VK_SUCCESS) {
      record = std::move(without_ngx_record);
      adjusted_device = record.adjusted;
      features_added = without_ngx_features;
      added_text = std::move(without_ngx_added_text);
    }
  }
#else
  VkResult result = original(physical, &adjusted, allocator, device);
#endif
  if (result < VK_SUCCESS && (adjusted_device || features_added)) {
    result = original(physical, info, allocator, device);  // R49: the driver refused the additions; the game's own request
    adjusted_device = false;
    refused = true;
    record.adjusted = false;
    record.memory_win32 = false;
    record.semaphore_win32 = false;
    record.timeline = false;
    record.ngx_ready = false;  // Plan 13: without the timeline feature it may have added, native NR is not known to work here
    record.ngx_missing = "Uplift's device adjustment (the driver refused it)";
    record.storage_extended_enabled = false;
  }
  if (reservation.counted) {
    g_creations_under_way.fetch_sub(1u, std::memory_order_acq_rel);  // the original (and its retry) returned: no creation is under way for NoteProcessExit
  }
  try {
    if (failed) {
      record.not_adjusted = "Uplift's adjustment failed";
    } else if (refused) {
      record.not_adjusted = "the driver refused Uplift's additions";
      added_text.clear();
    }
    const bool exists = (result >= VK_SUCCESS && device != nullptr);
    if (exists) {
      record.graphics_family = GraphicsFamily(physical, *info);  // Plan 19: the game's queues, which no request of Uplift's changes
    }
    {
      const std::unique_lock lock(Shared().mutex);
      State& state = Shared();
      // The creation failed or ended without the additions (no adjusted device exists to prove or disprove anything): its share is released.
      // Otherwise the device counts from now on.
      if (state.ledger.Created(reservation, (exists && adjusted_device ? static_cast<const void*>(*device) : nullptr))) {
        DeleteMarker(state.config.pending_marker);
      }
      if (exists) {
        state.devices.insert_or_assign(*device, Entry{.record = record});
      }
    }
    if (exists) {
      if (!added_text.empty()) {
        nr::Logf(nr::LogLevel::INFO, "Vulkan: added {} to the game's device", added_text);
      } else if (record.semaphore_win32 && record.timeline) {
        nr::Log(nr::LogLevel::INFO, "Vulkan: the game's device already has the sharing extensions");
      } else {
        nr::Logf(nr::LogLevel::INFO, "Vulkan: device created without Uplift's adjustment ({})", record.not_adjusted);
      }
    }
#if defined(_WIN64)
    if (!ngx_line.empty()) {
      nr::Log(nr::LogLevel::INFO, ngx_line);
    }
    if (exists && !failed) {
      if (ngx_refusal != VK_SUCCESS) {
        nr::Logf(nr::LogLevel::WARN, "Vulkan: the driver refused the device with NGX's additions (VkResult {}); {}", static_cast<int>(ngx_refusal),
                 (refused ? "it refused it without them too, so the device was created as the game asked" : "it was created without them"));
      }
      // What the device has for NGX, unless the game's own request already had everything and the device kept it; the notes say why one of
      // Uplift's additions was not made, when the request went through as planned.
      if (!game_ngx_ready || !record.ngx_ready) {
        const std::string why = ((ngx_notes.empty() || refused || ngx_refusal != VK_SUCCESS) ? std::string() : std::format(" ({})", ngx_notes));
        if (record.ngx_ready) {
          nr::Logf(nr::LogLevel::INFO, "Vulkan: the device has everything NGX needs{}", why);
        } else {
          nr::Logf(nr::LogLevel::INFO, "Vulkan: for NGX the device lacks {}{}", record.ngx_missing, why);
        }
      }
    }
#endif
  } catch (...) {
    // Bookkeeping only: the device exists, and the game gets its result.
  }
  return result;
}

#if defined(_WIN64)
// Plan 13 (design §3.6): each physical device the game is handed, with its instance; the latest enumeration wins, so a probe instance's
// stale entry is overwritten when the real instance enumerates. Never throws.
void RecordInstance(VkInstance instance, std::span<const VkPhysicalDevice> physicals) noexcept {
  try {
    const std::unique_lock lock(Shared().mutex);
    for (const VkPhysicalDevice physical : physicals) {
      Shared().instances.insert_or_assign(physical, instance);
    }
  } catch (...) {
    // No-op: a device made from a physical device missing here reads "Uplift did not see this game's Vulkan instance" (R84).
  }
}

// The loader's exports, which vkGetInstanceProcAddr also returns (design §1.3): no lock across the original, nothing thrown.
VkResult VKAPI_CALL EnumeratePhysicalDevicesDetour(VkInstance instance, uint32_t* count, VkPhysicalDevice* physicals) {
  const PFN_vkEnumeratePhysicalDevices original = g_enumerate_original.load(std::memory_order_acquire);
  if (original == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = original(instance, count, physicals);
  if (result >= VK_SUCCESS && count != nullptr && physicals != nullptr) {
    RecordInstance(instance, std::span<const VkPhysicalDevice>(physicals, *count));
  }
  return result;
}

VkResult VKAPI_CALL EnumeratePhysicalDeviceGroupsDetour(VkInstance instance, uint32_t* count, VkPhysicalDeviceGroupProperties* groups) {
  const PFN_vkEnumeratePhysicalDeviceGroups original = g_enumerate_groups_original.load(std::memory_order_acquire);
  if (original == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = original(instance, count, groups);
  if (result >= VK_SUCCESS && count != nullptr && groups != nullptr) {
    for (const VkPhysicalDeviceGroupProperties& group : std::span<const VkPhysicalDeviceGroupProperties>(groups, *count)) {
      RecordInstance(instance, std::span<const VkPhysicalDevice>(group.physicalDevices, std::min<uint32_t>(group.physicalDeviceCount, VK_MAX_DEVICE_GROUP_SIZE)));
    }
  }
  return result;
}

// Design §3.6: the add-on's teardown runs first, while ReShade's layer still dispatches the device (its own vkDestroyDevice drops the device
// from its map before raising destroy_device). The game has idled the device, as vkDestroyDevice requires. No hook lock is held here.
void VKAPI_CALL DestroyDeviceDetour(VkDevice device, const VkAllocationCallbacks* allocator) {
  const PFN_vkDestroyDevice original = g_destroy_original.load(std::memory_order_acquire);
  if (device != VK_NULL_HANDLE) {
    try {
      if (void (*const callback)(VkDevice) = g_destroy_callback.load(std::memory_order_acquire); callback != nullptr) {
        callback(device);
      }
      nr::Snippet::ForgetVulkanDevice(device);  // a new VkDevice at this address is a new device for the NGX core
    } catch (...) {
      // No-op: the game's device goes whatever Uplift's teardown did; ReShade's destroy_device abandons what is left (R83).
    }
  }
  if (original != nullptr) {
    original(device, allocator);
  }
}
#endif

}  // namespace

std::optional<std::string> DeviceHook::Install() {
  if (g_installed.load(std::memory_order_acquire)) return std::nullopt;  // the common case: every vkCreateInstance raises create_device
  static std::mutex install_mutex;                                       // one installer at a time; the detour never takes it
  const std::scoped_lock lock(install_mutex);
  if (g_installed.load(std::memory_order_acquire)) return std::nullopt;
  const Loader* const loader = Loader::Get();
  if (loader == nullptr) return "vulkan-1.dll is not loaded";
  // Final review I-1: the loader is pinned before anything is patched. DXVK loads vulkan-1.dll with LoadLibrary for each of its instances and
  // frees it with the last one (a probe instance, an adapter check, a game that recreates its IDirect3D9), and the Khronos loader never pins
  // itself: the image, the detour in it and Loader's cached export pointers would go, and a later mapping of the DLL at another base would leave
  // those pointers dangling. Pinned, both last as long as the process.
  HMODULE loader_module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(loader->create_device),
                         &loader_module)
      == FALSE) {
    return std::format("could not pin vulkan-1.dll (error {})", GetLastError());
  }
  // Key decision b: the detour lives in this module, and ReShade unloads its add-ons when the last instance or device goes.
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&CreateDeviceDetour), &self)
      == FALSE) {
    return std::format("could not pin Uplift's module (error {})", GetLastError());
  }
  if (const MH_STATUS status = hooks::EnsureMinHook(); status != MH_OK) return std::format("MinHook failed to start: {}", MH_StatusToString(status));
  void* const target = reinterpret_cast<void*>(loader->create_device);
  PFN_vkCreateDevice original = nullptr;
  if (const MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&CreateDeviceDetour), reinterpret_cast<void**>(&original));
      status != MH_OK) {
    return std::format("MinHook could not hook vkCreateDevice: {}", MH_StatusToString(status));
  }
  g_original.store(original, std::memory_order_release);  // before the hook is live: a device made on another thread must find it
  if (const MH_STATUS status = MH_EnableHook(target); status != MH_OK) {
    MH_RemoveHook(target);
    g_original.store(nullptr, std::memory_order_release);
    return std::format("MinHook could not enable the vkCreateDevice hook: {}", MH_StatusToString(status));
  }
  hooks::HoldMinHook();  // the hook and the pinned module live for the process: nothing may MH_Uninitialize now
#if defined(_WIN64)
  // Plan 13 (design §3.6): the instance for native NR's VULKAN_Init_Ext2, and its teardown before ReShade's own vkDestroyDevice. A failure
  // leaves the Present path as it is and native NR unavailable, with the reason.
  std::string native_error;
  const auto hook_export = [&native_error](void* target, void* detour, const char* name, const auto& publish) {
    if (!native_error.empty()) return;
    if (target == nullptr) {
      native_error = std::format("vulkan-1.dll does not export {}", name);
      return;
    }
    void* original = nullptr;
    if (const MH_STATUS status = MH_CreateHook(target, detour, &original); status != MH_OK) {
      native_error = std::format("MinHook could not hook {}: {}", name, MH_StatusToString(status));
      return;
    }
    publish(original);  // before the hook is live
    if (const MH_STATUS status = MH_EnableHook(target); status != MH_OK) {
      MH_RemoveHook(target);
      publish(nullptr);
      native_error = std::format("MinHook could not enable the {} hook: {}", name, MH_StatusToString(status));
    }
  };
  hook_export(reinterpret_cast<void*>(loader->enumerate_physical_devices), reinterpret_cast<void*>(&EnumeratePhysicalDevicesDetour),
              "vkEnumeratePhysicalDevices", [](void* original) {
                g_enumerate_original.store(reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(original), std::memory_order_release);
              });
  hook_export(reinterpret_cast<void*>(loader->enumerate_physical_device_groups), reinterpret_cast<void*>(&EnumeratePhysicalDeviceGroupsDetour),
              "vkEnumeratePhysicalDeviceGroups", [](void* original) {
                g_enumerate_groups_original.store(reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(original), std::memory_order_release);
              });
  hook_export(reinterpret_cast<void*>(loader->destroy_device), reinterpret_cast<void*>(&DestroyDeviceDetour), "vkDestroyDevice",
              [](void* original) { g_destroy_original.store(reinterpret_cast<PFN_vkDestroyDevice>(original), std::memory_order_release); });
  {
    const std::unique_lock state_lock(Shared().mutex);
    Shared().native_hooks_error = native_error;
  }
  if (!native_error.empty()) {
    nr::Logf(nr::LogLevel::WARN, "Vulkan: NR after DLSS is unavailable in this game: {}", native_error);
  }
#endif
  g_installed.store(true, std::memory_order_release);
  return std::nullopt;
}

bool DeviceHook::Installed() {
  return g_installed.load(std::memory_order_acquire);
}

#if defined(_WIN64)
std::string DeviceHook::NativeHooksError() {
  const std::shared_lock lock(Shared().mutex);
  return Shared().native_hooks_error;
}

void DeviceHook::SetDestroyCallback(void (*callback)(VkDevice device)) {
  g_destroy_callback.store(callback, std::memory_order_release);
}

void DeviceHook::NoteInstanceApiVersion(uint32_t api_version) {
  const std::unique_lock lock(Shared().mutex);
  Shared().instance_api_version = api_version;
}
#endif

void DeviceHook::Configure(bool adjust, bool adjust_ngx, bool native_nr_chosen, bool native_nr_latched, bool reshade_adds_memory_win32,
                           std::filesystem::path pending_marker) {
  const std::unique_lock lock(Shared().mutex);
  Config& config = Shared().config;
  config.adjust = adjust;
  config.adjust_ngx = adjust_ngx;
  config.native_nr_chosen = native_nr_chosen;
  config.native_nr_latched = native_nr_latched;
  config.reshade_adds_memory_win32 = reshade_adds_memory_win32;
  config.pending_marker = std::move(pending_marker);
  const std::wstring& text = config.pending_marker.native();
  if (text.size() < std::size(g_marker_file)) {
    text.copy(g_marker_file, text.size());
    g_marker_file[text.size()] = L'\0';
  }
}

std::optional<DeviceRecord> DeviceHook::Find(VkDevice device) {
  const std::shared_lock lock(Shared().mutex);
  const auto found = Shared().devices.find(device);
  if (found == Shared().devices.end()) return std::nullopt;
  return found->second.record;
}

bool DeviceHook::NotePresent(VkDevice device) {
  const std::unique_lock lock(Shared().mutex);
  State& state = Shared();
  const auto found = state.devices.find(device);
  if (found == state.devices.end() || !found->second.record.adjusted) return true;
  Entry& entry = found->second;
  if (entry.presents >= 2u) return true;
  if (++entry.presents < 2u) return false;
  // ReShade raises `present` before it calls the game's Present, so the second event is the proof that the first real one returned. The
  // adjustment is then proven for this game: every device still counting is released at once and the marker goes, so no other device (one that
  // never presents and is still alive when the game exits, say) can leave it behind, and none created later writes it (batch 2 review I-3).
  if (state.ledger.Proven()) {
    DeleteMarker(state.config.pending_marker);
  }
  return true;
}

void DeviceHook::NoteDestroyed(VkDevice device) {
  const std::unique_lock lock(Shared().mutex);
  State& state = Shared();
  const auto found = state.devices.find(device);
  if (found == state.devices.end()) return;
  // Only a device that still counts takes its share of the marker away, and the marker goes with the last one: one device's end never deletes the
  // marker another live, unproven adjusted device is waiting on (I-3).
  if (state.ledger.Destroyed(device)) {
    DeleteMarker(state.config.pending_marker);
  }
  state.devices.erase(found);
}

void DeviceHook::NoteProcessExit() {
  // A clean exit proves no adjusted device took the process down, so a marker still on disk (a device kept alive without presenting, say) must
  // not turn the adjustment off at the next start. Most crashes never get here (no DLL_PROCESS_DETACH), but a game that ends through ExitProcess
  // from its own crash handler does: while a creation is under way (a thread inside an adjusted vkCreateDevice) the marker therefore stays, and
  // the kill switch still fires. A creation merely slow when the game quits cleanly costs the accepted once-only false positive. No lock (see
  // g_marker_file); DeleteFileW allocates on the process heap, and if a thread ExitProcess killed held the heap's lock, Windows ends the
  // process there and the marker stays, the same false positive.
  if (g_creations_under_way.load(std::memory_order_acquire) != 0u) return;
  if (g_marker_on_disk.exchange(false, std::memory_order_acq_rel) && g_marker_file[0] != L'\0') {
    DeleteFileW(g_marker_file);
  }
}

DeviceHook::MarkerVerdict DeviceHook::CheckMarker(const std::filesystem::path& pending_marker) {
  // Once per process: a marker found later is this start's own (an AddonInit that runs again while a device waits for its first frame).
  static std::atomic<bool> checked{false};
  if (checked.exchange(true)) return MarkerVerdict::NONE;
  if (!addon::LatchMarkerExists(pending_marker)) return MarkerVerdict::NONE;
  // T1b: the first tier names NGX's additions (an unreadable file reads as the second tier, today's).
  std::string text;
  if (std::ifstream file(pending_marker, std::ios::binary); file) {
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  addon::DeleteLatchMarker(pending_marker);
  const bool ngx = (text.find(MARKER_NGX_TEXT) != std::string::npos);
  const std::unique_lock lock(Shared().mutex);
  if (ngx) {
    Shared().config.ngx_turned_off_by_marker = true;
    return MarkerVerdict::NGX_OFF;
  }
  Shared().config.turned_off_by_marker = true;
  return MarkerVerdict::ADJUST_OFF;
}

}  // namespace uplift::vk
