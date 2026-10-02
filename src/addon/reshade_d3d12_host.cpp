#include "addon/reshade_d3d12_host.hpp"

#include <cstdint>

namespace uplift::addon {
namespace {

namespace api = reshade::api;

// client::D3D12Usage is ReShade's api::resource_usage, numbered the same (reshade_api_resource.hpp).
static_assert(static_cast<uint32_t>(client::D3D12Usage::RENDER_TARGET) == static_cast<uint32_t>(api::resource_usage::render_target));
static_assert(static_cast<uint32_t>(client::D3D12Usage::SHADER_RESOURCE) == static_cast<uint32_t>(api::resource_usage::shader_resource));
static_assert(static_cast<uint32_t>(client::D3D12Usage::COPY_DEST) == static_cast<uint32_t>(api::resource_usage::copy_dest));
static_assert(static_cast<uint32_t>(client::D3D12Usage::COPY_SOURCE) == static_cast<uint32_t>(api::resource_usage::copy_source));
static_assert(static_cast<uint32_t>(client::D3D12Usage::GENERAL) == static_cast<uint32_t>(api::resource_usage::general));
static_assert(static_cast<uint32_t>(client::D3D12Usage::PRESENT) == static_cast<uint32_t>(api::resource_usage::present));

api::resource_usage UsageOf(client::D3D12Usage usage) {
  return static_cast<api::resource_usage>(static_cast<uint32_t>(usage));
}

// A ReShade handle is 64 bits wide in every build, the object it names a pointer of the game's width.
api::resource ResourceOf(ID3D12Resource* resource) {
  return api::resource{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resource))};
}
api::fence FenceOf(ID3D12Fence* fence) {
  return api::fence{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence))};
}

}  // namespace

void ReshadeD3D12Host::Barrier(ID3D12Resource* resource, client::D3D12Usage before, client::D3D12Usage after) {
  if (list_ == nullptr) return;
  list_->barrier(ResourceOf(resource), UsageOf(before), UsageOf(after));
}

void ReshadeD3D12Host::Copy(ID3D12Resource* source, ID3D12Resource* destination) {
  if (list_ == nullptr) return;
  list_->copy_resource(ResourceOf(source), ResourceOf(destination));
}

bool ReshadeD3D12Host::Signal(ID3D12Fence* fence, uint64_t value) {
  return queue_ != nullptr && queue_->signal(FenceOf(fence), value);
}

bool ReshadeD3D12Host::Wait(ID3D12Fence* fence, uint64_t value) {
  return queue_ != nullptr && queue_->wait(FenceOf(fence), value);
}

}  // namespace uplift::addon
