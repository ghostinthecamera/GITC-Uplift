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
  // vkEnumeratePhysicalDevices[Groups] wins; null: Uplift did not see it, R84). `ngx_ready`: the device was created with NGX's four extensions
  // and bufferDeviceAddress (the game's own, or Uplift's additions, Plan 19) and has timeline semaphores; `ngx_missing` names what it lacks otherwise.
  VkInstance instance = VK_NULL_HANDLE;
  bool ngx_ready = false;
  std::string ngx_missing;
  // shaderStorageImageExtendedFormats (batch 2 review, minor 2): supported by the physical device, and on the device (the game enabled
  // it, or Uplift's adjustment did). Native NR's encode shaders write an RG16F storage image, one of the extended formats.
  bool storage_extended_supported = false;
  bool storage_extended_enabled = false;
  // Plan 19: the first queue family the game created a queue in that supports graphics, the family of ReShade's effect queue (its primary graphics
  // queue, vulkan_hooks_device.cpp:138-152, 762-767); UINT32_MAX when there is none or the loader could not tell.
  uint32_t graphics_family = UINT32_MAX;
};

// Plan 11 (Vulkan design §2): a MinHook detour on vulkan-1.dll!vkCreateDevice. ReShade's layer runs the add-on's AddonInit inside the game's
// vkCreateInstance, before any device exists, so a detour installed there is in time for every device. The detour adds
// VK_KHR_external_semaphore_win32, VK_KHR_external_memory_win32 and timeline semaphores when the physical device offers them and the game did
// not list them, to a copy of the create info (the game's memory is never written). The 64-bit half adds what NGX needs for native NR too
// (Plan 19): its four extensions and bufferDeviceAddress, on the same terms (BuildDeviceAdjustment's `add_ngx`). A refusal retries without NGX's
// additions first, then with the game's own request (R49).
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
  // VK_KHR_external_memory_win32 to every device itself. `pending_marker`: the file written before an adjusted device is created. `adjust_ngx`:
  // AdjustVulkanDevicesForNgx (Plan 19 T1b; the 64-bit half only adds NGX's extensions and bufferDeviceAddress, and only with `adjust` too).
  // `native_nr_chosen` (T5): VulkanNr is Native as read at AddonInit. Only the native route needs NGX's additions (Direct3D 12 and the helper never do),
  // so they are made only then too; the sharing extensions do not depend on it. A device made without them says so in `ngx_missing` (Native then needs
  // a game restart). `native_nr_latched` (Plan 19 T6 fix round, M-2): VulkanNativeNr is 0 (Uplift's latch): no NGX additions either, and `ngx_missing`
  // names the key and Clear latch instead.
  static void Configure(bool adjust, bool adjust_ngx, bool native_nr_chosen, bool native_nr_latched, bool reshade_adds_memory_win32,
                        std::filesystem::path pending_marker);
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
  // What a marker left by the last start asks for (Plan 19 T1b: two tiers).
  enum class MarkerVerdict {
    NONE,        // no marker (or not the process's first call)
    NGX_OFF,     // first tier: a device with NGX's additions waited on it; the caller turns AdjustVulkanDevicesForNgx off, saves, and says so
    ADJUST_OFF,  // second tier: a device without them did; the caller turns AdjustVulkanDevices off, saves, and says so
  };
  // AddonInit (the first call of the process only; later ones say NONE): a marker left by the last start means an adjusted device never presented
  // and never went. When one was left, it is deleted and later devices get the matching "Uplift turned ... off" reason.
  [[nodiscard]] static MarkerVerdict CheckMarker(const std::filesystem::path& pending_marker);
#if defined(_WIN64)
  // Plan 13 (design §3.6, 64-bit only): Install also detours the loader's vkEnumeratePhysicalDevices, vkEnumeratePhysicalDeviceGroups (the
  // instance native NR's VULKAN_Init_Ext2 needs) and vkDestroyDevice. Empty once all three are in; else why not (native NR is then unavailable).
  [[nodiscard]] static std::string NativeHooksError();
  // Called in the vkDestroyDevice detour BEFORE the original, while ReShade's layer still knows the device, on the game's thread, with no hook
  // lock held and inside a try: the add-on tears the device's native NR down there (design §3.6). Then the detour forgets the device's NGX
  // core initialisation (nr::Snippet::ForgetVulkanDevice) and calls the original. Set once, from AddonInit.
  static void SetDestroyCallback(void (*callback)(VkDevice device));
  // Plan 19: create_device(vulkan), in each vkCreateInstance, with the app's API version as ReShade codes it (major << 12 | minor << 8). ReShade appends
  // VK_KHR_push_descriptor to a device of an instance below 1.4; the latest instance's version decides (0, none seen yet, reads below 1.4).
  static void NoteInstanceApiVersion(uint32_t api_version);
#endif
};

}  // namespace uplift::vk
