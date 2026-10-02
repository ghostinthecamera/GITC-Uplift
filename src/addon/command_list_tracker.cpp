#include "addon/command_list_tracker.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

#include "addon/uplift_catch.hpp"

namespace uplift::addon {
namespace {

namespace api = reshade::api;

// {6c1a4f2e-93b1-4d8e-a50c-712e449bd318}: the native list's private data holds its ListState*.
constexpr GUID LIST_STATE_GUID = {0x6c1a4f2e, 0x93b1, 0x4d8e, {0xa5, 0x0c, 0x71, 0x2e, 0x44, 0x9b, 0xd3, 0x18}};

std::atomic<bool> g_tracking{false};
std::atomic<uint64_t> g_epoch{0u};

// Plan 13: every Vulkan list's state, by handle. Never freed: the events and the NGX detour can run until the process ends.
struct VkLists {
  std::shared_mutex mutex;
  std::unordered_map<VkCommandBuffer, std::unique_ptr<VkListState>> states;
};
VkLists& SharedVkLists() {
  static VkLists* const lists = new VkLists();
  return *lists;
}

VkCommandBuffer VkBufferOf(api::command_list* list) {
  return reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(list->get_native()));
}

ID3D12GraphicsCommandList* NativeOf(api::command_list* list) {
  return reinterpret_cast<ID3D12GraphicsCommandList*>(list->get_native());
}

// The tracking handlers (v2 design §3.2): one relaxed load while tracking is off, no lock ever.
void OnBindPipeline(api::command_list* cmd_list, api::pipeline_stage stages, api::pipeline pipeline) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::PIPELINE_STAGE_COMPUTE) == 0u) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnBindPipeline(static_cast<uint32_t>(stages), pipeline.handle);
    }
  }
  UPLIFT_CATCH("bind_pipeline", )
}

void OnBindDescriptorTables(api::command_list* cmd_list, api::shader_stage stages, api::pipeline_layout layout,
                            uint32_t first, uint32_t count, const api::descriptor_table* tables) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::SHADER_STAGE_COMPUTE) == 0u) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnBindDescriptorTables(static_cast<uint32_t>(stages), layout.handle, first, count,
                                           reinterpret_cast<const uint64_t*>(tables));
    }
  }
  UPLIFT_CATCH("bind_descriptor_tables", )
}

void OnPushConstants(api::command_list* cmd_list, api::shader_stage stages, api::pipeline_layout /*layout*/,
                     uint32_t layout_param, uint32_t first, uint32_t count, const void* values) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::SHADER_STAGE_COMPUTE) == 0u) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnPushConstants(static_cast<uint32_t>(stages), layout_param, first, count,
                                    static_cast<const uint32_t*>(values));
    }
  }
  UPLIFT_CATCH("push_constants", )
}

void OnPushDescriptors(api::command_list* cmd_list, api::shader_stage stages, api::pipeline_layout /*layout*/,
                       uint32_t layout_param, const api::descriptor_table_update& update) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  if ((static_cast<uint32_t>(stages) & state::SHADER_STAGE_COMPUTE) == 0u) return;
  try {
    ListState* const state = FindListState(cmd_list);
    const std::optional<state::RootViewType> type = state::RootViewTypeOf(static_cast<uint32_t>(update.type));
    if (state == nullptr || !type || update.count != 1u || update.descriptors == nullptr) return;
    uint64_t address = 0u;
    if (*type == state::RootViewType::CBV) {
      // ReShade resolves a root CBV into a buffer range; the replay needs its address back.
      const auto& range = *static_cast<const api::buffer_range*>(update.descriptors);
      auto* const buffer = reinterpret_cast<ID3D12Resource*>(range.buffer.handle);
      if (buffer == nullptr) return;
      address = buffer->GetGPUVirtualAddress() + range.offset;
    } else {
      address = *static_cast<const uint64_t*>(update.descriptors);  // 6.1+: the raw GPU address
    }
    state->shadow.OnPushRootView(static_cast<uint32_t>(stages), layout_param, *type, address);
  }
  UPLIFT_CATCH("push_descriptors", )
}

void OnExecuteSecondaryCommandList(api::command_list* cmd_list, api::command_list* /*secondary*/) {
  if (!g_tracking.load(std::memory_order_relaxed)) return;
  try {
    if (ListState* const state = FindListState(cmd_list)) {
      state->shadow.OnExecuteBundle();
    }
  }
  UPLIFT_CATCH("execute_secondary_command_list", )
}

// v2 design §3.2's replay: steps 1-4 through ReShade's API (step 1's count of 0 re-issues ReShade's
// cached heaps and the root signature), step 5 natively.
class ReshadeReplayTarget final : public state::ReplayTarget {
 public:
  explicit ReshadeReplayTarget(api::command_list* list) : list_(list), native_(NativeOf(list)) {}
  void RebindHeapsAndRootSignature(uint64_t layout) override {
    // A count of 0 re-issues ReShade's cached heaps for any stage mask; the compute bit also re-sets the root
    // signature, so pass it only when the list had one (never a null compute root signature).
    list_->bind_descriptor_tables(layout != 0u ? api::shader_stage::all_compute : static_cast<api::shader_stage>(0),
                                  api::pipeline_layout{layout}, 0u, 0u, nullptr);
  }
  void BindPipeline(uint64_t pipeline) override { list_->bind_pipeline(api::pipeline_stage::all, api::pipeline{pipeline}); }
  void BindTables(uint64_t layout, uint32_t first, uint32_t count, const uint64_t* tables) override {
    list_->bind_descriptor_tables(api::shader_stage::all_compute, api::pipeline_layout{layout}, first, count,
                                  reinterpret_cast<const api::descriptor_table*>(tables));
  }
  void PushConstants(uint64_t layout, uint32_t param, uint32_t count, const uint32_t* values) override {
    list_->push_constants(api::shader_stage::all_compute, api::pipeline_layout{layout}, param, 0u, count, values);
  }
  void SetRootView(uint32_t param, state::RootViewType type, uint64_t gpu_address) override {
    switch (type) {
      case state::RootViewType::SRV: native_->SetComputeRootShaderResourceView(param, gpu_address); break;
      case state::RootViewType::UAV: native_->SetComputeRootUnorderedAccessView(param, gpu_address); break;
      case state::RootViewType::CBV: native_->SetComputeRootConstantBufferView(param, gpu_address); break;
    }
  }

 private:
  api::command_list* list_;
  ID3D12GraphicsCommandList* native_;
};

}  // namespace

void CreateListState(api::command_list* list) {
  ListState& state = list->create_private_data<ListState>();
  state.list = list;
  state.device = list->get_device();
  state.shadow.OnReset(TrackingEpoch());  // a new list is open and empty
  ListState* const address = &state;
  NativeOf(list)->SetPrivateData(LIST_STATE_GUID, sizeof(address), &address);
}

std::vector<uint64_t> DestroyListState(api::command_list* list) {
  ListState* const state = FindListState(list);
  if (state == nullptr) return {};
  std::vector<uint64_t> tokens = std::move(state->tokens);
  NativeOf(list)->SetPrivateData(LIST_STATE_GUID, 0u, nullptr);
  list->destroy_private_data<ListState>();
  return tokens;
}

ListState* FindListState(api::command_list* list) {
  uint64_t value = 0u;
  list->get_private_data(reinterpret_cast<const uint8_t*>(&__uuidof(ListState)), &value);
  return reinterpret_cast<ListState*>(static_cast<uintptr_t>(value));
}

ListState* FindListState(ID3D12GraphicsCommandList* list) {
  if (list == nullptr) return nullptr;
  ListState* state = nullptr;
  UINT size = sizeof(state);
  if (FAILED(list->GetPrivateData(LIST_STATE_GUID, &size, &state)) || size != sizeof(state)) return nullptr;
  return state;
}

std::optional<StaleVkList> CreateVkListState(api::command_list* list) {
  auto state = std::make_unique<VkListState>();
  state->device = list->get_device();
  std::unique_ptr<VkListState> previous;
  {
    VkLists& lists = SharedVkLists();
    const std::unique_lock lock(lists.mutex);
    previous = std::exchange(lists.states[VkBufferOf(list)], std::move(state));
  }
  if (previous == nullptr || previous->tokens.empty()) return std::nullopt;
  return StaleVkList{.device = static_cast<api::device*>(const_cast<void*>(previous->device)), .tokens = std::move(previous->tokens)};
}

std::optional<VkListState> DestroyVkListState(api::command_list* list) {
  std::unique_ptr<VkListState> state;
  {
    VkLists& lists = SharedVkLists();
    const std::unique_lock lock(lists.mutex);
    const auto found = lists.states.find(VkBufferOf(list));
    if (found == lists.states.end()) return std::nullopt;
    state = std::move(found->second);
    lists.states.erase(found);
  }
  return std::move(*state);
}

VkListState* FindVkListState(VkCommandBuffer buffer) {
  VkLists& lists = SharedVkLists();
  const std::shared_lock lock(lists.mutex);
  const auto found = lists.states.find(buffer);
  return (found == lists.states.end() ? nullptr : found->second.get());
}

void ForgetVkListStates(const void* device) {
  VkLists& lists = SharedVkLists();
  const std::unique_lock lock(lists.mutex);
  std::erase_if(lists.states, [device](const auto& entry) { return entry.second->device == device; });
}

void SetTracking(bool on) {
  if (!on) {
    g_tracking.store(false, std::memory_order_release);
    return;
  }
  if (!g_tracking.load(std::memory_order_relaxed)) {
    g_epoch.fetch_add(1u, std::memory_order_relaxed);
    g_tracking.store(true, std::memory_order_release);
  }
}

uint64_t TrackingEpoch() {
  return (g_tracking.load(std::memory_order_acquire) ? g_epoch.load(std::memory_order_relaxed) : 0u);
}

void RegisterTrackingEvents() {
  reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
  reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OnBindDescriptorTables);
  reshade::register_event<reshade::addon_event::push_constants>(OnPushConstants);
  reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
  reshade::register_event<reshade::addon_event::execute_secondary_command_list>(OnExecuteSecondaryCommandList);
}

ID3D12GraphicsCommandList* ReshadeDlssFrameHost::NativeList() {
  return (state_ == nullptr ? nullptr : NativeOf(state_->list));
}

bool ReshadeDlssFrameHost::StateKnown() {
  return state_ != nullptr && state_->shadow.Known(TrackingEpoch());
}

void ReshadeDlssFrameHost::ReplayState(state::RestoreMode mode) {
  ReshadeReplayTarget target(state_->list);
  state_->shadow.Replay(target, mode);
}

void ReshadeDlssFrameHost::AttachToken(uint64_t token) {
  if (state_->first_token == 0u) {
    state_->first_token = token;
  }
  state_->tokens.push_back(token);
}

}  // namespace uplift::addon
