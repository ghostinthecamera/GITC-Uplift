#include "addon/hud_frame.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <span>

#include "addon/sl_tag_layout.hpp"
#include "nr/log.hpp"
#include "text/hudless_messages.hpp"

namespace uplift::addon {
namespace {

// Format families the composer reads in stored values (design §3, condition 6). A HUD-less image matches the
// backbuffer when both are in one family: its sRGB, UNORM or typeless sibling.
enum class Family : uint8_t { NONE, RGBA8, BGRA8, RGB10A2, RGBA16F };

struct FormatFamily {
  uint32_t format;
  Family family;
};

constexpr auto DXGI_FAMILIES = std::to_array<FormatFamily>({
    {27u, Family::RGBA8}, {28u, Family::RGBA8}, {29u, Family::RGBA8},        // R8G8B8A8 TYPELESS, UNORM, UNORM_SRGB
    {87u, Family::BGRA8}, {90u, Family::BGRA8}, {91u, Family::BGRA8},        // B8G8R8A8 UNORM, TYPELESS, UNORM_SRGB
    {23u, Family::RGB10A2}, {24u, Family::RGB10A2},                          // R10G10B10A2 TYPELESS, UNORM
    {9u, Family::RGBA16F}, {10u, Family::RGBA16F},                           // R16G16B16A16 TYPELESS, FLOAT
});
constexpr auto VK_FAMILIES = std::to_array<FormatFamily>({
    {37u, Family::RGBA8}, {43u, Family::RGBA8},  // R8G8B8A8 UNORM, SRGB
    {44u, Family::BGRA8}, {50u, Family::BGRA8},  // B8G8R8A8 UNORM, SRGB
    {64u, Family::RGB10A2},                      // A2B10G10R10_UNORM_PACK32
    {97u, Family::RGBA16F},                      // R16G16B16A16_SFLOAT
});
// Single-channel and packed formats an opacity image may also have, besides the families above.
constexpr auto DXGI_OPACITY_EXTRA = std::to_array<uint32_t>({61u, 65u, 54u, 56u, 41u, 26u});  // R8_UNORM, A8_UNORM, R16_FLOAT, R16_UNORM, R32_FLOAT, R11G11B10_FLOAT
constexpr auto VK_OPACITY_EXTRA = std::to_array<uint32_t>({9u, 70u, 76u, 100u, 122u});       // R8_UNORM, R16_UNORM, R16_SFLOAT, R32_SFLOAT, B10G11R11_UFLOAT_PACK32

constexpr uint32_t VK_USAGE_TRANSFER_SRC = 0x1u;
constexpr uint32_t VK_USAGE_SAMPLED = 0x4u;
constexpr uint32_t SINGLE = 1u;
// Where the HUD-less and opacity formats sit in the key of the last set of formats logged.
constexpr unsigned HUDLESS_FORMAT_SHIFT = 21u;
constexpr unsigned OPACITY_FORMAT_SHIFT = 42u;
constexpr uint64_t FORMATS_LOGGED_NONE = ~uint64_t{0u};

std::atomic<uint64_t> g_formats_logged{FORMATS_LOGGED_NONE};

Family FamilyOf(TagApi api, uint32_t format) {
  const auto& table = (api == TagApi::VULKAN ? std::span<const FormatFamily>(VK_FAMILIES) : std::span<const FormatFamily>(DXGI_FAMILIES));
  const auto found = std::ranges::find(table, format, &FormatFamily::format);
  return (found == table.end() ? Family::NONE : found->family);
}

bool OpacityReadable(TagApi api, uint32_t format) {
  if (FamilyOf(api, format) != Family::NONE) return true;
  const auto extra = (api == TagApi::VULKAN ? std::span<const uint32_t>(VK_OPACITY_EXTRA) : std::span<const uint32_t>(DXGI_OPACITY_EXTRA));
  return std::ranges::find(extra, format) != extra.end();
}

bool WholeExtent(const TaggedImage& image, const HudFrameFacts& back_buffer) {
  if (image.extent_w == 0u || image.extent_h == 0u) return true;
  return (image.extent_x == 0u && image.extent_y == 0u && image.extent_w == back_buffer.width && image.extent_h == back_buffer.height);
}

bool BackBufferSize(const TaggedImage& image, const HudFrameFacts& back_buffer) {
  return (image.described && image.width == back_buffer.width && image.height == back_buffer.height);
}

bool SingleSampledLayer(const TaggedImage& image) {
  return (std::max(image.samples, SINGLE) == SINGLE && std::max(image.layers, SINGLE) == SINGLE);
}

}  // namespace

HudReason DecideHudFrame(bool use_wanted, bool frame_generation_reads, const TakeResult& taken, const HudFrameFacts& back_buffer) {
  if (!use_wanted) return HudReason::CORRECTION_OFF;
  if (!frame_generation_reads) return HudReason::NOT_READ;
  if (!taken.frame) return (taken.reason == HudReason::USABLE ? HudReason::NOT_TAGGED : taken.reason);
  const TaggedImage& hudless = taken.frame->hudless;
  const TaggedImage& opacity = taken.frame->opacity;
  if (opacity.native == 0u) return HudReason::NO_OPACITY;
  if (hudless.lifecycle != sl_layout::VALID_UNTIL_PRESENT || opacity.lifecycle != sl_layout::VALID_UNTIL_PRESENT) return HudReason::LIFECYCLE;
  if (!WholeExtent(hudless, back_buffer) || !WholeExtent(opacity, back_buffer)) return HudReason::EXTENT;
  if (!BackBufferSize(hudless, back_buffer) || !BackBufferSize(opacity, back_buffer)) return HudReason::SIZE;
  if (!SingleSampledLayer(hudless) || !SingleSampledLayer(opacity)) return HudReason::SAMPLES;
  const Family family = FamilyOf(back_buffer.api, back_buffer.format);
  if (family == Family::NONE || FamilyOf(back_buffer.api, hudless.format) != family || !OpacityReadable(back_buffer.api, opacity.format)) {
    return HudReason::FORMAT;
  }
  if (hudless.state == sl_layout::UNKNOWN_STATE || opacity.state == sl_layout::UNKNOWN_STATE) return HudReason::STATE;
  if (back_buffer.api == TagApi::VULKAN) {
    const bool sampled = ((hudless.usage & VK_USAGE_SAMPLED) != 0u && (opacity.usage & VK_USAGE_SAMPLED) != 0u);
    if (!sampled || (hudless.usage & VK_USAGE_TRANSFER_SRC) == 0u) return HudReason::USAGE;
  } else if (hudless.denies_shader_reads || opacity.denies_shader_reads) {
    return HudReason::USAGE;
  }
  if (back_buffer.early && (back_buffer.effects_on || back_buffer.technique_expected)) return HudReason::EFFECTS_ON;
  return HudReason::USABLE;
}

void NoteHudFormats(HudReason reason, const TakeResult& taken, const HudFrameFacts& back_buffer) {
  if (reason != HudReason::FORMAT || !taken.frame) return;
  const uint32_t hudless = taken.frame->hudless.format;
  const uint32_t opacity = taken.frame->opacity.format;
  const uint64_t formats =
      uint64_t{back_buffer.format} ^ (uint64_t{hudless} << HUDLESS_FORMAT_SHIFT) ^ (uint64_t{opacity} << OPACITY_FORMAT_SHIFT);
  if (g_formats_logged.exchange(formats, std::memory_order_relaxed) == formats) return;
  const Family family = FamilyOf(back_buffer.api, back_buffer.format);
  nr::Logf(nr::LogLevel::INFO,
           "HUD-less image cannot be used, formats ({}): the frame {} ({}), the HUD-less image {} ({}), the HUD's opacity {} ({})",
           (back_buffer.api == TagApi::VULKAN ? "VkFormat" : "DXGI_FORMAT"), back_buffer.format,
           (family == Family::NONE ? "not one NR uses here" : "usable"), hudless,
           (family != Family::NONE && FamilyOf(back_buffer.api, hudless) == family ? "matches the frame" : "does not match the frame"), opacity,
           (OpacityReadable(back_buffer.api, opacity) ? "readable" : "not readable"));
}

std::string_view HudReasonText(HudReason reason) {
  const auto index = static_cast<size_t>(reason);
  return (index < text::HUD_REASON_TEXTS.size() ? text::HUD_REASON_TEXTS[index] : std::string_view{});
}

bool HudSettle::Admit(HudReason reason) noexcept {
  if (reason != HudReason::USABLE) {
    usable_ = 0u;
    return false;
  }
  usable_ = std::min(usable_ + 1u, HUD_SETTLE_PRESENTS);
  return usable_ >= HUD_SETTLE_PRESENTS;
}

}  // namespace uplift::addon
