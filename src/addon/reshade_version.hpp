#pragma once

#include <Windows.h>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>

namespace uplift::addon {

struct ModuleVersion {
  uint32_t major = 0u;
  uint32_t minor = 0u;
  uint32_t patch = 0u;
  uint32_t build = 0u;

  friend constexpr auto operator<=>(const ModuleVersion&, const ModuleVersion&) = default;
};

// v2 design §3.2: the count-0 bind_descriptor_tables that replay step 1 relies on is in every tag
// from 6.0.1; the design gates on 6.1 to stay conservative.
inline constexpr ModuleVersion MIN_DLSS_PLACEMENT_RESHADE = {6u, 1u, 0u, 0u};

// ReShade 6.8 is the first whose Vulkan layer raises create_device(vulkan) in every vkCreateInstance and create_swapchain (event 97, with the longer
// swapchain_desc that has fullscreen_state), and that adds VK_KHR_external_memory_win32 to every device (Vulkan design §2, §3.5).
inline constexpr ModuleVersion MIN_RESHADE_VULKAN_EVENTS = {6u, 8u, 0u, 0u};

// The file version from `module`'s version resource; nullopt when it has none or cannot be read.
[[nodiscard]] std::optional<ModuleVersion> ReadModuleVersion(HMODULE module);
[[nodiscard]] bool SupportsDlssPlacement(const ModuleVersion& version);
// "6.1.0.1718"
[[nodiscard]] std::string FormatModuleVersion(const ModuleVersion& version);

}  // namespace uplift::addon
