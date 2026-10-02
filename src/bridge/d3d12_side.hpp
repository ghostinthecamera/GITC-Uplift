#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nr/types.hpp"

namespace uplift::bridge {

// Plan 9 (design §2.5): a private D3D12 device on one adapter, its direct queue, a ring of allocators with a busy skip,
// one list, the `progress` fence, and the retire rule for surfaces the queue may still read. From Plan 7's D3D11Bridge,
// which now composes it; the helper's transports sit on it too. ReShade-free; not thread-safe.
class D3D12Side {
 public:
  // nullptr + error when the adapter is not NVIDIA's or a D3D12 object cannot be made. `created` receives the device
  // D3D12CreateDevice returned (ReShade's proxy in a game): release it only outside the add-on's lock. The native
  // device comes from the progress fence's GetDevice (Plan 7 key decision a).
  static std::unique_ptr<D3D12Side> Create(LUID luid, Microsoft::WRL::ComPtr<ID3D12Device>* created, std::string* error);
  ~D3D12Side();  // waits up to 2 s for the last signalled progress value, then releases, the created device last
  D3D12Side(const D3D12Side&) = delete;
  D3D12Side& operator=(const D3D12Side&) = delete;

  [[nodiscard]] ID3D12Device* Device() const { return device_.Get(); }
  [[nodiscard]] ID3D12CommandQueue* Queue() const { return queue_.Get(); }
  // The `progress` fence, for a caller that wants its own completion event (SetEventOnCompletion). Never signal it.
  [[nodiscard]] ID3D12Fence* Progress() const { return progress_.Get(); }
  // A committed 2D texture in COMMON on a D3D12_HEAP_FLAG_SHARED heap. `handle`, when non-null, also receives an NT
  // handle (the caller closes it). Returns the failing call's HRESULT.
  HRESULT CreateShared(nr::Size size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, const wchar_t* name,
                       Microsoft::WRL::ComPtr<ID3D12Resource>* resource, HANDLE* handle);
  HRESULT CreateSharedFence(Microsoft::WRL::ComPtr<ID3D12Fence>* fence, HANDLE* handle);
  [[nodiscard]] bool SlotFree() const;     // the next allocator's last list finished (else: skip this frame)
  ID3D12GraphicsCommandList* BeginList();  // resets the next allocator and the list; null on failure
  bool ExecuteList();                      // closes and executes it; false (nothing submitted) when Close fails
  void CloseList();                        // closes an open list without executing it (a recording that threw)
  // Signals `progress` to `value` on the queue and marks the slot used up to it. False when the queue refused. The slot
  // is marked whenever a list was submitted since the last call, even when the signal failed (Plan 7 final review C-1):
  // its surfaces are then held until `progress` passes `value`, for good if it never does.
  bool SignalProgress(uint64_t value);
  [[nodiscard]] uint64_t Completed() const;
  // The newest value SignalProgress was asked to reach: values must only grow, so a caller that starts afresh on a side that
  // already ran (a second transport after a DETACH) continues above it.
  [[nodiscard]] uint64_t LastSignalled() const { return last_signalled_; }
  // Keeps `resource` and `partner` (an object on another device that must go with it) until progress passes `last_use`;
  // released at once when it already has. True when a resource was released at once (a null one is neither), from the one
  // read of progress that decided it: D3D11Bridge sets `released_` from it, so the D3D10 relay's Flush is never missed.
  bool Retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource, Microsoft::WRL::ComPtr<IUnknown> partner, uint64_t last_use);
  bool FreeFinished();  // releases what progress passed; true when anything went
  // Waits up to 2 s for the last signalled progress value. The destructor does; a composer whose own surfaces the queue
  // may still read calls it first, before releasing them.
  void Drain();

 private:
  D3D12Side() = default;

  struct Slot {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    uint64_t done = 0u;  // the progress value after the recording that last used this allocator
  };
  struct Retired {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<IUnknown> partner;
    uint64_t release_at = 0u;
  };
  // Plan 7 key decision c: the CPU runs up to DXGI's frame latency ahead; a busy ring skips NR for that frame.
  static constexpr size_t RING = 8u;

  // Declared first, so released last: the device as D3D12CreateDevice returned it (ReShade's proxy in a game).
  Microsoft::WRL::ComPtr<ID3D12Device> created_;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;  // native
  // Not shared, and only ever signalled by this side's queue: what the queue really finished. Allocators and retired
  // surfaces go by it (a shared fence can be moved ahead from the CPU by a Stop).
  Microsoft::WRL::ComPtr<ID3D12Fence> progress_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
  std::array<Slot, RING> ring_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
  std::vector<Retired> retired_;
  size_t next_slot_ = 0u;
  uint64_t last_signalled_ = 0u;  // the newest value progress_ was asked to reach
  bool list_open_ = false;        // BeginList ran and the list is neither executed nor closed
  bool executed_ = false;         // a list was submitted since the last SignalProgress
};

}  // namespace uplift::bridge
