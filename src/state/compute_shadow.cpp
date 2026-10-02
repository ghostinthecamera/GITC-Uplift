#include "state/compute_shadow.hpp"

#include <algorithm>

namespace uplift::state {

std::optional<RootViewType> RootViewTypeOf(uint32_t descriptor_type) {
  switch (descriptor_type) {
    case DESCRIPTOR_BUFFER_SRV:
    case DESCRIPTOR_ACCELERATION_STRUCTURE_6_1:
    case DESCRIPTOR_ACCELERATION_STRUCTURE_6_8: return RootViewType::SRV;
    case DESCRIPTOR_BUFFER_UAV:                 return RootViewType::UAV;
    case DESCRIPTOR_CONSTANT_BUFFER:            return RootViewType::CBV;
    default:                                    return std::nullopt;
  }
}

void ComputeShadow::OnReset(uint64_t epoch) {
  epoch_ = epoch;
  unknown_ = false;
  pipeline_ = 0u;
  root_signature_ = 0u;
  ClearArguments();
}

void ComputeShadow::OnBindPipeline(uint32_t stages, uint64_t pipeline) {
  if ((stages & PIPELINE_STAGE_COMPUTE) != 0u) {
    pipeline_ = pipeline;
  }
}

void ComputeShadow::OnBindDescriptorTables(uint32_t stages, uint64_t layout, uint32_t first, uint32_t count,
                                           const uint64_t* tables) {
  if ((stages & SHADER_STAGE_COMPUTE) == 0u) return;
  if (count == 0u) {
    if (layout != root_signature_) {
      root_signature_ = layout;
      ClearArguments();
    }
    return;
  }
  if (tables == nullptr || first >= MAX_ROOT_PARAMETERS || count > MAX_ROOT_PARAMETERS - first) {
    unknown_ = true;
    return;
  }
  for (uint32_t index = 0u; index < count; ++index) {
    slots_[first + index] = Slot::TABLE;
    values_[first + index] = tables[index];
  }
}

void ComputeShadow::OnPushConstants(uint32_t stages, uint32_t param, uint32_t first, uint32_t count,
                                    const uint32_t* values) {
  if ((stages & SHADER_STAGE_COMPUTE) == 0u || count == 0u) return;
  if (values == nullptr || param >= MAX_ROOT_PARAMETERS || first >= MAX_ROOT_CONSTANTS
      || count > MAX_ROOT_CONSTANTS - first) {
    unknown_ = true;
    return;
  }
  const uint32_t end = first + count;
  const uint32_t size = constant_count_[param];
  if (end > size) {
    // First use of this parameter, or a longer range: move it to the pool's end, keeping its values.
    if (pool_used_ + end > MAX_ROOT_CONSTANTS) {
      unknown_ = true;
      return;
    }
    const uint32_t base = pool_used_;
    std::copy_n(pool_.begin() + constant_base_[param], size, pool_.begin() + base);
    std::fill(pool_.begin() + base + size, pool_.begin() + base + end, 0u);
    constant_base_[param] = static_cast<uint8_t>(base);
    constant_count_[param] = static_cast<uint8_t>(end);
    pool_used_ = static_cast<uint8_t>(base + end);
  }
  std::copy_n(values, count, pool_.begin() + constant_base_[param] + first);
}

void ComputeShadow::OnPushRootView(uint32_t stages, uint32_t param, RootViewType type, uint64_t gpu_address) {
  if ((stages & SHADER_STAGE_COMPUTE) == 0u) return;
  if (param >= MAX_ROOT_PARAMETERS) {
    unknown_ = true;
    return;
  }
  switch (type) {
    case RootViewType::SRV: slots_[param] = Slot::SRV; break;
    case RootViewType::UAV: slots_[param] = Slot::UAV; break;
    case RootViewType::CBV: slots_[param] = Slot::CBV; break;
  }
  values_[param] = gpu_address;
}

void ComputeShadow::OnExecuteBundle() {
  unknown_ = true;
}

bool ComputeShadow::Known(uint64_t epoch) const {
  return epoch != 0u && epoch_ == epoch && !unknown_;
}

void ComputeShadow::Replay(ReplayTarget& target, RestoreMode mode) const {
  target.RebindHeapsAndRootSignature(root_signature_);
  if (mode == RestoreMode::MINIMAL) return;
  if (pipeline_ != 0u) {
    target.BindPipeline(pipeline_);  // D3D12 rejects a null pipeline state
  }
  if (root_signature_ == 0u) return;  // no root signature, no root arguments
  for (uint32_t param = 0u; param < MAX_ROOT_PARAMETERS;) {
    if (slots_[param] != Slot::TABLE) {
      ++param;
      continue;
    }
    uint32_t end = param + 1u;
    while (end < MAX_ROOT_PARAMETERS && slots_[end] == Slot::TABLE) {
      ++end;
    }
    target.BindTables(root_signature_, param, end - param, values_.data() + param);
    param = end;
  }
  for (uint32_t param = 0u; param < MAX_ROOT_PARAMETERS; ++param) {
    if (constant_count_[param] > 0u) {
      target.PushConstants(root_signature_, param, constant_count_[param], pool_.data() + constant_base_[param]);
    }
  }
  for (uint32_t param = 0u; param < MAX_ROOT_PARAMETERS; ++param) {
    switch (slots_[param]) {
      case Slot::SRV: target.SetRootView(param, RootViewType::SRV, values_[param]); break;
      case Slot::UAV: target.SetRootView(param, RootViewType::UAV, values_[param]); break;
      case Slot::CBV: target.SetRootView(param, RootViewType::CBV, values_[param]); break;
      default:        break;
    }
  }
}

void ComputeShadow::ClearArguments() {
  slots_.fill(Slot::NONE);
  constant_count_.fill(0u);
  pool_used_ = 0u;
}

}  // namespace uplift::state
