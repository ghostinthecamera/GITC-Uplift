#include "vk/frame.hpp"

#include <atomic>
#include <format>

#include "color/encoding.hpp"
#include "vk/format.hpp"

namespace uplift::vk {
namespace {

// Batch 2 review I-1. The accesses a game's (or ReShade's) writes to the back buffer can have, and every access ReShade's effects, its overlay or
// a screenshot may start with. The global barriers name them so the dependency is a real one: ReShade's own barrier from the present state
// (TOP_OF_PIPE source, access 0) orders nothing (vulkan_impl_type_convert.cpp:736-739, 808-814).
constexpr Usage FRAME_WRITES = Usage::RENDER_TARGET | Usage::UNORDERED_ACCESS | Usage::COPY_DEST;
constexpr Usage FRAME_ACCESSES =
    Usage::RENDER_TARGET | Usage::SHADER_RESOURCE | Usage::UNORDERED_ACCESS | Usage::COPY_SOURCE | Usage::COPY_DEST;

std::atomic<bool> g_exclusive_fullscreen_swapchain{false};

}  // namespace

void NoteExclusiveFullscreenSwapchain() {
  g_exclusive_fullscreen_swapchain.store(true, std::memory_order_relaxed);
}

bool ExclusiveFullscreenSwapchainSeen() {
  return g_exclusive_fullscreen_swapchain.load(std::memory_order_relaxed);
}

std::string BackBufferProblem(const ImageInfo& back_buffer, bool memory_win32) {
  if (!back_buffer.copy_dest) return (ExclusiveFullscreenSwapchainSeen() ? EXCLUSIVE_FULLSCREEN_PROBLEM : COPY_ACCESS_PROBLEM);
  if (back_buffer.samples != 1u) return "Multisampled back buffers are not supported on Vulkan";
  if (!color::DescribeFormat(back_buffer.format) || VkFormatOf(SharedFormatOf(back_buffer.format)) == VK_FORMAT_UNDEFINED) {
    return std::format("Unsupported back-buffer format (DXGI_FORMAT {})", static_cast<int>(back_buffer.format));
  }
  if (!memory_win32) return "This Vulkan device cannot import Uplift's textures (VK_KHR_external_memory_win32 is not enabled)";
  return {};
}

void RecordOpeningBarrier(GameHost& host) {
  host.Barrier(VK_NULL_HANDLE, FRAME_WRITES, FRAME_ACCESSES);
}

void RecordCopyIn(GameHost& host, VkImage back_buffer, Usage entry_state, VkImage shared_color, VkImage motion, VkImage shared_motion) {
  // Design §3.4 case 1: a real execution and memory dependency on everything the game submitted on this queue, before the copy reads the frame.
  // The per-image transition alone carries only a TOP_OF_PIPE source stage and no source access for the present state (ReShade's mapping), which
  // orders nothing by itself; a game's last writes to its back buffer are colour attachment, storage image or transfer ones. So the global barrier
  // and the back buffer's transition are one two-entry call (batch 2 review, minor 3): the transition takes the global barrier's ALL_COMMANDS
  // source scope. The opening barrier already covers the game's frame; this one also covers what the effects wrote since.
  host.BarrierWithGlobal(back_buffer, entry_state, Usage::COPY_SOURCE, FRAME_WRITES, Usage::COPY_SOURCE);
  host.Barrier(shared_color, Usage::UNDEFINED, Usage::COPY_DEST);
  host.Copy(back_buffer, shared_color);
  host.Barrier(shared_color, Usage::COPY_DEST, Usage::GENERAL);
  host.Barrier(back_buffer, Usage::COPY_SOURCE, entry_state);
  if (motion != VK_NULL_HANDLE) {
    // UPLIFT_MV: a texture ReShade keeps in shader_resource between techniques.
    host.Barrier(motion, Usage::SHADER_RESOURCE, Usage::COPY_SOURCE);
    host.Barrier(shared_motion, Usage::UNDEFINED, Usage::COPY_DEST);
    host.Copy(motion, shared_motion);
    host.Barrier(shared_motion, Usage::COPY_DEST, Usage::GENERAL);
    host.Barrier(motion, Usage::COPY_SOURCE, Usage::SHADER_RESOURCE);
  }
}

void RecordCopyOut(GameHost& host, VkImage back_buffer, Usage entry_state, VkImage shared_color) {
  host.Barrier(shared_color, Usage::GENERAL, Usage::COPY_SOURCE);
  host.Barrier(back_buffer, entry_state, Usage::COPY_DEST);
  host.Copy(shared_color, back_buffer);
  host.Barrier(shared_color, Usage::COPY_SOURCE, Usage::GENERAL);
  // Batch 2 review I-1 (b): the closing global barrier, chained with the back buffer's return to `entry_state`. What ReShade records next (its
  // effects after the Present point, its overlay, a screenshot) starts from the present state, which orders nothing after NR's transfer write;
  // this barrier does, and makes the write visible to every access those can start with.
  host.BarrierWithGlobal(back_buffer, Usage::COPY_DEST, entry_state, Usage::COPY_DEST, FRAME_ACCESSES);
}

void RecordMaskCopy(GameHost& host, VkImage mask, VkImage shared_mask) {
  // ReShade keeps an effect texture in shader_resource between techniques. The next Run's Signal orders this copy before the next recording.
  host.Barrier(mask, Usage::SHADER_RESOURCE, Usage::COPY_SOURCE);
  host.Barrier(shared_mask, Usage::UNDEFINED, Usage::COPY_DEST);
  host.Copy(mask, shared_mask);
  host.Barrier(shared_mask, Usage::COPY_DEST, Usage::GENERAL);
  host.Barrier(mask, Usage::COPY_SOURCE, Usage::SHADER_RESOURCE);
}

}  // namespace uplift::vk
