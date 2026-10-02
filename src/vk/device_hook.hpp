#pragma once

#include <vulkan/vulkan.h>

#include <filesystem>
#include <optional>
#include <string>

namespace uplift::vk {

// What the detour learned about one device it saw being created (Vulkan design §2.2).
struct DeviceRecord {
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  bool memory_win32 = false;     // VK_KHR_external_memory_win32 is on the device, whoever enabled it
  bool semaphore_win32 = false;  // VK_KHR_external_semaphore_win32
  bool timeline = false;         // timeline semaphores
  bool adjusted = false;         // Uplift added something to this device
  // Why NR cannot order on the GPU, when it cannot: the design §8 reason, the whole sentence ("the game's device was created without
  // VK_KHR_external_semaphore_win32 because ..."). Empty for a device with all three.
  std::string not_adjusted;
  // Plan 13 (design §3.6), recorded by the 64-bit half only. The instance the physical device was enumerated from (the latest
  // vkEnumeratePhysicalDevices[Groups] wins; null: Uplift did not see it, R84). `ngx_ready`: the game listed NGX's four extensions,
  // enabled bufferDeviceAddress, and the device has timeline semaphores; `ngx_missing` names what it lacks otherwise.
  VkInstance instance = VK_NULL_HANDLE;
  bool ngx_ready = false;
  std::string ngx_missing;
  // shaderStorageImageExtendedFormats (batch 2 review, minor 2): supported by the physical device, and on the device (the game enabled
  // it, or Uplift's adjustment did). Native NR's encode shaders write an RG16F storage image, one of the extended formats.
  bool storage_extended_supported = false;
  bool storage_extended_enabled = false;
};

// Plan 11 (Vulkan design §2): a MinHook detour on vulkan-1.dll!vkCreateDevice. ReShade's layer runs the add-on's AddonInit inside the game's
// vkCreateInstance, before any device exists, so a detour installed there is in time for every device. The detour adds
// VK_KHR_external_semaphore_win32, VK_KHR_external_memory_win32 and timeline semaphores when the physical device offers them and the game did
// not list them, to a copy of the create info (the game's memory is never written).
//
// Self-contained (key decision b): static state behind its own shared mutex, never the add-on's lock; it throws nothing; no lock is held across
// the original call; it raises no ReShade event. Install pins vulkan-1.dll (final review I-1: DXVK frees the loader with its last instance, and
// the detour and Loader's cached export pointers would go with it) and the add-on module, and is idempotent (the add-on pins ReShade's module next
// to it, batch 2 review I-4). ReShade-free, in both halves.
//
// The pending marker is one file per process, counting every adjusted device that lists VK_KHR_swapchain (a device that can never present cannot
// prove anything): it is written before the first one is created and deleted when the last one that has neither presented twice nor been destroyed
// is gone, or at the first proof (an adjusted device's second present), after which nothing is written any more (batch 2 review I-3).
class DeviceHook {
 public:
  // create_device(vulkan) (ReShade 6.8 raises it in every vkCreateInstance, before any device exists), and AddonInit of a ReShade older than 6.8 when
  // vulkan-1.dll is loaded. Returns the reason on failure, nullopt once installed (also when it already was).
  static std::optional<std::string> Install();
  [[nodiscard]] static bool Installed();
  // `adjust`: AdjustVulkanDevices, and not turned off by the pending marker. `reshade_adds_memory_win32`: ReShade >= 6.8 adds
  // VK_KHR_external_memory_win32 to every device itself. `pending_marker`: the file written before an adjusted device is created.
  static void Configure(bool adjust, bool reshade_adds_memory_win32, std::filesystem::path pending_marker);
  // A copy of what the detour recorded for `device`; nullopt for a device it never saw.
  [[nodiscard]] static std::optional<DeviceRecord> Find(VkDevice device);
  // Every present event of `device`: the second one of an adjusted device proves the first real present returned, so the adjustment works in
  // this game: the pending marker goes, every other device still counting is released from it, and no later device writes it. True once
  // counting is over (nothing recorded, nothing adjusted, or the proof came), so the caller stops calling.
  static bool NotePresent(VkDevice device);
  // destroy_device: the record goes. A device still counting toward the marker takes its share away, and the marker goes with the last one;
  // another live, unproven adjusted device keeps it. Takes no ReShade call.
  static void NoteDestroyed(VkDevice device);
  // The add-on's DllMain at process exit (DLL_PROCESS_DETACH, process ending): a marker still on disk is deleted, because a clean exit shows that
  // no adjusted device took the process down, whatever device was still alive then, unless a creation is under way (a thread inside an adjusted
  // vkCreateDevice: a game's crash handler that calls ExitProcess ends up here too). No lock.
  static void NoteProcessExit();
  // AddonInit (the first call of the process only; later ones say true): a marker left by the last start means an adjusted device never presented
  // and never went. True when there is none. When one was left, it is deleted, later devices get the "Uplift turned its adjustment off" reason,
  // and the result is false: the caller turns AdjustVulkanDevices off, saves, and says so.
  [[nodiscard]] static bool CheckMarker(const std::filesystem::path& pending_marker);
#if defined(_WIN64)
  // Plan 13 (design §3.6, 64-bit only): Install also detours the loader's vkEnumeratePhysicalDevices, vkEnumeratePhysicalDeviceGroups (the
  // instance native NR's VULKAN_Init_Ext2 needs) and vkDestroyDevice. Empty once all three are in; else why not (native NR is then unavailable).
  [[nodiscard]] static std::string NativeHooksError();
  // Called in the vkDestroyDevice detour BEFORE the original, while ReShade's layer still knows the device, on the game's thread, with no hook
  // lock held and inside a try: the add-on tears the device's native NR down there (design §3.6). Then the detour forgets the device's NGX
  // core initialisation (nr::Snippet::ForgetVulkanDevice) and calls the original. Set once, from AddonInit.
  static void SetDestroyCallback(void (*callback)(VkDevice device));
#endif
};

}  // namespace uplift::vk
