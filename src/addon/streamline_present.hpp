#pragma once

#include <Windows.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace uplift::addon::streamline {

// sl::DLSSGMode values the game passes to slDLSSGSetOptions (Streamline's public sl_dlss_g.h).
inline constexpr uint32_t DLSSG_MODE_OFF = 0u;      // eOff
inline constexpr uint32_t DLSSG_MODE_ON = 1u;       // eOn
inline constexpr uint32_t DLSSG_MODE_AUTO = 2u;     // eAuto
inline constexpr uint32_t DLSSG_MODE_DYNAMIC = 3u;  // eDynamic
// sl::Result values (sl_result.h) after which Streamline has applied the options: eOk, and eWarnOutOfVRAM, its only warning,
// which NBA 2K27 gets on every call while frame generation runs (issue 16).
inline constexpr int SL_RESULT_OK = 0;
inline constexpr int SL_RESULT_WARN_OUT_OF_VRAM = 39;

// Borrowed pointers to VkForward are valid only while its forwarding scope is alive.
// An unclaimed scope must complete release_tick, which takes the add-on lock, before it may destroy this object.
// Takers dereference only under that same lock and disarm release_tick after their final read.
struct VkForward {
  VkPresentInfoKHR info;
  VkResult failure = VK_SUCCESS;
  bool native_tick = false;
  using ReleaseTick = void (*)(const VkForward*) noexcept;
  std::atomic<ReleaseTick> release_tick{};
  bool TakeNativeTick() noexcept {
    const bool ticked = native_tick;
    release_tick.store(nullptr, std::memory_order_release);  // Last access: the outer scope may now end on another thread.
    return ticked;
  }
  // A forwarded call that produces no accepted inner present must not leave its tick for a later frame.
  void ReleaseNativeTick() noexcept {
    if (const auto release = release_tick.load(std::memory_order_acquire)) release(this);
  }
};

// The callbacks run before forwarding the application's call, outside the hook installation lock.
struct Observer {
  void (*dxgi_created)(IDXGISwapChain* outer, IUnknown* creation_device, void* reshade_chain);
  void (*dxgi_present)(IDXGISwapChain* outer, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);
  void (*vk_present)(VkQueue queue, const VkPresentInfoKHR* application, VkForward* forwarded);
  void (*dxgi_resize)(IDXGISwapChain*, UINT count, IUnknown* const* queues, void* inner) = nullptr;
  void (*vk_created)(VkDevice, VkSwapchainKHR, const VkSwapchainCreateInfoKHR&, void* inner) = nullptr;
  void (*vk_images)(VkDevice, VkSwapchainKHR, uint32_t, const VkImage*) = nullptr;
  void (*vk_destroyed)(VkDevice, VkSwapchainKHR) = nullptr;
  void (*vk_acquired)(VkDevice, VkSwapchainKHR, uint32_t) = nullptr;
  VkImageUsageFlags (*vk_usage)(VkDevice, const VkSwapchainCreateInfoKHR&) = nullptr;
  void (*dxgi_retry)(IDXGISwapChain*) = nullptr;
  // HUD-less (design §2): Streamline's tags, reported after the call was forwarded unchanged. `viewport` is an sl::ViewportHandle*,
  // `tags` an sl::ResourceTag array; `frame` is the sl::FrameToken's address, never null. v2 interposers only. They must not throw.
  void (*hud_tags)(const void* viewport, const void* tags, uint32_t count) = nullptr;
  void (*hud_tags_for_frame)(const void* frame, const void* viewport, const void* tags, uint32_t count) = nullptr;
  // Before Streamline's slShutdown is forwarded.
  void (*sl_shutdown)() = nullptr;
  bool diagnostic_dxgi = false;
};

bool Start(HMODULE self, const Observer& observer);
void Poll();
void NoteReshadeSwapchain(void* chain);
bool WatchGameFacing(IDXGISwapChain* chain, bool trace_method = false);
bool InsideOuterPresent();
bool Installed();
// The game's Streamline interposer (sl.interposer.dll) is in the process, so frame generation can exist here; false until Poll or the game's first
// resolved Streamline export finds it, and for good in a game without Streamline (the overlay greys Before FG with why).
bool StreamlineLoaded();
enum class FgMode { UNKNOWN, OFF, ON, AUTO };
FgMode Mode();
// The mode's name for the DiagnosticBeforeFG trace.
std::string_view ModeName(FgMode mode);

// The Streamline mode of a raw sl::DLSSGMode; a value past the known modes is UNKNOWN.
[[nodiscard]] constexpr FgMode ModeFromRaw(uint32_t raw) {
  switch (raw) {
    case DLSSG_MODE_OFF: return FgMode::OFF;
    case DLSSG_MODE_ON: return FgMode::ON;
    case DLSSG_MODE_AUTO:
    case DLSSG_MODE_DYNAMIC: return FgMode::AUTO;
    default: return FgMode::UNKNOWN;
  }
}

// The mode Mode() keeps after a slDLSSGSetOptions call. A stored OFF vetoes all evidence of frame generation (FrameGenerationActive), so it
// must be true: the game's request counts once Streamline applied it, and after any other result nothing is known and UNKNOWN lets the
// evidence decide. Never a stale OFF while frame generation runs, nor an ON that nothing applied.
[[nodiscard]] constexpr FgMode StoredMode(FgMode requested, int result) {
  return (result == SL_RESULT_OK || result == SL_RESULT_WARN_OUT_OF_VRAM) ? requested : FgMode::UNKNOWN;
}

[[nodiscard]] constexpr bool FrameGenerationActive(FgMode mode, bool detected) {
  return mode != FgMode::OFF && (mode == FgMode::ON || mode == FgMode::AUTO || detected);
}

// Whether the overlay shows the game's frame generation as running: the stage diagram's FG box and the Before FG choice read this one
// fact. `detected`: the device's frame generation detector or a running Before FG pass; `owns_frame`: Before FG's outer Present owns the frame.
[[nodiscard]] constexpr bool FrameGenerationShown(FgMode mode, bool detected, bool owns_frame) {
  return owns_frame || FrameGenerationActive(mode, detected);
}

}  // namespace uplift::addon::streamline
