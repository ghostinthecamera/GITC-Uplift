#pragma once

#include "addon/reshade_api.hpp"

#include <d3d12.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "addon/vk_dlss_context.hpp"
#include "sources/after_dlss_source.hpp"
#include "state/compute_shadow.hpp"

namespace uplift::addon {

// One game command list as Uplift sees it (v2 design §3.2). It lives in ReShade's private data, and
// its address in the native list's private data, so the NGX hook finds it from the list NGX was given.
struct __declspec(uuid("3f2a9c64-8b0e-4f1d-9e57-c1a2b3d4e5f6")) ListState {
  reshade::api::command_list* list = nullptr;
  reshade::api::device* device = nullptr;
  state::ComputeShadow shadow;
  std::vector<uint64_t> tokens;  // completion tokens recorded on this list and not yet submitted
  uint64_t first_token = 0u;     // N12: the first Uplift token since the last Reset; 0 = none
  bool executed = false;         // N12: submitted at least once since the last Reset
};

// init_command_list: creates the list's state and stamps the native list with its address.
void CreateListState(reshade::api::command_list* list);
// destroy_command_list: removes both, and returns the tokens that were never submitted.
std::vector<uint64_t> DestroyListState(reshade::api::command_list* list);
[[nodiscard]] ListState* FindListState(reshade::api::command_list* list);
// From the list NGX was handed. ReShade's proxy forwards GetPrivateData to the native list.
[[nodiscard]] ListState* FindListState(ID3D12GraphicsCommandList* list);

// Plan 13 (design §3.4): the VkCommandBuffer -> VkListState map, filled at init_command_list for every Vulkan list (its own shared mutex, never
// the add-on's lock). ReShade's private data is not used: a Vulkan list's state is only ever found from its handle, by the NGX detour and the
// command-list events, and no Direct3D 12 lookup can see it.
struct StaleVkList {
  reshade::api::device* device = nullptr;
  std::vector<uint64_t> tokens;  // in recording order
};
// init_command_list of a Vulkan list. A state still held for the same handle is stale (its pool was destroyed, which raises no
// destroy_command_list): it is replaced, and returned when it holds tokens, for NrCompletion::Recycle (batch 1 review, minor 3).
[[nodiscard]] std::optional<StaleVkList> CreateVkListState(reshade::api::command_list* list);
// destroy_command_list of a Vulkan list: removes its state and returns it.
[[nodiscard]] std::optional<VkListState> DestroyVkListState(reshade::api::command_list* list);
// The state of `buffer`, or null for a buffer allocated before Uplift registered its events. Stable until the buffer is freed.
[[nodiscard]] VkListState* FindVkListState(VkCommandBuffer buffer);
// Batch 3 review, minor 3: destroy_device of a Vulkan device. vkDestroyDevice frees its pools without destroy_command_list, so every state of `device`
// goes now, its tokens with it (they belonged to the device's own completion), and a later device never finds them under a reused handle.
void ForgetVkListStates(const void* device);

// The tracking events are registered once, at AddonInit (amendment 2), and gated here. Turning
// tracking on starts a new epoch: a list is known again only after its next Reset.
void SetTracking(bool on);
[[nodiscard]] uint64_t TrackingEpoch();  // 0 while tracking is off
// bind_pipeline, bind_descriptor_tables, push_constants, push_descriptors, execute_secondary_command_list.
void RegisterTrackingEvents();

// One game list for an After-DLSS recording: native recording, ReShade replay, tokens on the list.
class ReshadeDlssFrameHost final : public sources::DlssFrameHost {
 public:
  explicit ReshadeDlssFrameHost(ListState* state) : state_(state) {}
  ID3D12GraphicsCommandList* NativeList() override;
  bool StateKnown() override;
  void ReplayState(state::RestoreMode mode) override;
  void AttachToken(uint64_t token) override;

 private:
  ListState* state_;
};

}  // namespace uplift::addon
