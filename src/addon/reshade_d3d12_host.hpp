#pragma once

#include "addon/reshade_api.hpp"
#include "client/d3d12_client.hpp"

namespace uplift::addon {

// Plan 12 (design §6): client::D3D12Host on ReShade's API, on the present queue's immediate command list (on Direct3D 12 the effect runtime's queue is the game's
// own, so the present event's and the effect events' queues are the same). Barriers and copies are command_list::barrier and copy_resource, which record
// into ReShade's native immediate list and raise no add-on event; Signal is command_queue::signal, which flushes that list and then signals the fence natively;
// Wait is command_queue::wait, which does not flush. Resource states are ReShade's api::resource_usage values (general is COMMON; undefined is never used: it
// asserts on Direct3D 12). With a null queue nothing is valid (destroy_device: ReShade may already have released it).
class ReshadeD3D12Host final : public client::D3D12Host {
 public:
  explicit ReshadeD3D12Host(reshade::api::command_queue* queue)
      : queue_(queue), list_(queue != nullptr ? queue->get_immediate_command_list() : nullptr) {}

  [[nodiscard]] bool Valid() const override { return list_ != nullptr; }
  void Barrier(ID3D12Resource* resource, client::D3D12Usage before, client::D3D12Usage after) override;
  void Copy(ID3D12Resource* source, ID3D12Resource* destination) override;
  bool Signal(ID3D12Fence* fence, uint64_t value) override;
  bool Wait(ID3D12Fence* fence, uint64_t value) override;

 private:
  reshade::api::command_queue* queue_;
  reshade::api::command_list* list_;
};

}  // namespace uplift::addon
