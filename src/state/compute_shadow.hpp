#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace uplift::state {

// ReShade's enum values, compared as numbers: Uplift's pinned v6.0.0 headers predate some names.
// Checked in d3d12_command_list.cpp of ReShade 6.1.0 to 6.8.0 (this plan's Key decisions).
inline constexpr uint32_t SHADER_STAGE_COMPUTE = 0x20u;                 // api::shader_stage::compute
inline constexpr uint32_t PIPELINE_STAGE_COMPUTE = 0x800u;              // api::pipeline_stage::compute_shader
inline constexpr uint32_t DESCRIPTOR_BUFFER_SRV = 4u;                   // a root SRV: a raw GPU address (6.1+)
inline constexpr uint32_t DESCRIPTOR_BUFFER_UAV = 5u;                   // a root UAV: a raw GPU address (6.1+)
inline constexpr uint32_t DESCRIPTOR_CONSTANT_BUFFER = 6u;              // a root CBV: a resolved buffer_range
inline constexpr uint32_t DESCRIPTOR_ACCELERATION_STRUCTURE_6_1 = 8u;   // a root SRV holding one (6.1-6.7)
inline constexpr uint32_t DESCRIPTOR_ACCELERATION_STRUCTURE_6_8 = 10u;  // the same from 6.8

// A D3D12 root signature holds at most 64 DWORDs, so no parameter index or constant offset reaches 64.
inline constexpr uint32_t MAX_ROOT_PARAMETERS = 64u;
inline constexpr uint32_t MAX_ROOT_CONSTANTS = 64u;

enum class RootViewType : uint8_t {
  SRV,
  UAV,
  CBV,
};

// The root-view type a push_descriptors descriptor type stands for; nullopt for anything else.
[[nodiscard]] std::optional<RootViewType> RootViewTypeOf(uint32_t descriptor_type);

enum class RestoreMode : uint8_t {
  FULL,     // v2 design §3.2 steps 1-5
  MINIMAL,  // step 1 only
};

// Where replay goes. The add-on's target (Task 11) calls ReShade's command_list API for steps 1-4
// and the native list for step 5; unit tests record the calls.
class ReplayTarget {
 public:
  virtual ~ReplayTarget() = default;
  // Step 1: bind_descriptor_tables(compute, layout, 0, 0, nullptr). With a count of 0, ReShade
  // 6.1+ re-issues its cached descriptor heaps and this root signature.
  virtual void RebindHeapsAndRootSignature(uint64_t layout) = 0;
  // Step 2: bind_pipeline(all, pipeline).
  virtual void BindPipeline(uint64_t pipeline) = 0;
  // Step 3: bind_descriptor_tables(compute, layout, first, count, tables).
  virtual void BindTables(uint64_t layout, uint32_t first, uint32_t count, const uint64_t* tables) = 0;
  // Step 4: push_constants(compute, layout, param, 0, count, values).
  virtual void PushConstants(uint64_t layout, uint32_t param, uint32_t count, const uint32_t* values) = 0;
  // Step 5, on the native list: SetComputeRoot{ShaderResource,UnorderedAccess,ConstantBuffer}View.
  virtual void SetRootView(uint32_t param, RootViewType type, uint64_t gpu_address) = 0;
};

// One command list's compute state, from ReShade's events (v2 design §3.2). Under 1 KiB. Not
// thread-safe: one thread records a command list at a time, and its shadow with it.
class ComputeShadow {
 public:
  // reset_command_list or init_command_list: the list starts empty and known in `epoch` (never 0).
  void OnReset(uint64_t epoch);
  // bind_pipeline; `stages` is api::pipeline_stage as a number.
  void OnBindPipeline(uint32_t stages, uint64_t pipeline);
  // bind_descriptor_tables; `stages` is api::shader_stage as a number. A count of 0 sets the root
  // signature: a different one clears every root argument, the same one keeps them (D3D12 rules).
  void OnBindDescriptorTables(uint32_t stages, uint64_t layout, uint32_t first, uint32_t count, const uint64_t* tables);
  void OnPushConstants(uint32_t stages, uint32_t param, uint32_t first, uint32_t count, const uint32_t* values);
  // push_descriptors for a root view, with its GPU address (Task 11 turns a CBV's buffer_range into one).
  void OnPushRootView(uint32_t stages, uint32_t param, RootViewType type, uint64_t gpu_address);
  // execute_secondary_command_list: a bundle may change anything; unknown until the next Reset.
  void OnExecuteBundle();

  // True when every compute-state change since a Reset in `epoch` was seen and held.
  [[nodiscard]] bool Known(uint64_t epoch) const;
  [[nodiscard]] uint64_t RootSignature() const { return root_signature_; }
  // Re-binds the recorded state after Uplift's own recording, in the order of v2 design §3.2.
  void Replay(ReplayTarget& target, RestoreMode mode) const;

 private:
  enum class Slot : uint8_t {
    NONE,
    TABLE,
    SRV,
    UAV,
    CBV,
  };

  void ClearArguments();

  uint64_t epoch_ = 0u;  // 0: never reset while tracking
  bool unknown_ = false;
  uint64_t pipeline_ = 0u;
  uint64_t root_signature_ = 0u;
  std::array<uint64_t, MAX_ROOT_PARAMETERS> values_ = {};  // table handles and view addresses
  std::array<Slot, MAX_ROOT_PARAMETERS> slots_ = {};
  std::array<uint8_t, MAX_ROOT_PARAMETERS> constant_base_ = {};   // into pool_, valid while count > 0
  std::array<uint8_t, MAX_ROOT_PARAMETERS> constant_count_ = {};  // 0: no constants on this parameter
  std::array<uint32_t, MAX_ROOT_CONSTANTS> pool_ = {};
  uint8_t pool_used_ = 0u;
};

}  // namespace uplift::state
