#include "addon/before_fg_dx12.hpp"

#include <optional>
#include <algorithm>
#include "text/before_fg_messages.hpp"

namespace uplift::addon {
namespace {
using Microsoft::WRL::ComPtr;
// Identifies the BeforeFgDx12 route stored on the game-facing swapchain by Attach() and read by Find().
constexpr GUID ROUTE_ID = {0xfe799951,0xa813,0x4df1,{0xa9,0x0f,0xab,0x91,0xf2,0x11,0xcc,0x7a}};
// Requests the underlying COM object from a ReShade wrapper. If unsupported, Native() silently queries the original
// object for the requested interface instead, retaining the proxy when one was supplied.
constexpr GUID RESHADE_UNWRAP = {0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
// The one colour space ReShade names that NR cannot decode at Before FG. UNKNOWN passes: ReShade has not seen the game
// set a space, as when a game never calls SetColorSpace1.
constexpr color::ColorSpace UNSUPPORTED_OUTER_SPACE = color::ColorSpace::HDR10_HLG;

template<class T>
ComPtr<T> Native(IUnknown* object) {
  ComPtr<IUnknown> original;
  ComPtr<T> result;
  if (object && SUCCEEDED(object->QueryInterface(RESHADE_UNWRAP, reinterpret_cast<void**>(original.GetAddressOf())))) object = original.Get();
  if (object) object->QueryInterface(IID_PPV_ARGS(&result));
  return result;
}

class GameList final : public FrameHost {
 public:
  GameList(ID3D12GraphicsCommandList* list, ID3D12Resource* target) : list_(list), target_(target) {}
  void FlushPending() override {}
  ID3D12GraphicsCommandList* NativeList() override { return list_; }
  void TargetBarrier(D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) override {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {target_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list_->ResourceBarrier(1u, &barrier);
    touched = true;
  }
  bool touched = false;
 private:
  ID3D12GraphicsCommandList* list_;
  ID3D12Resource* target_;
};
}  // namespace

ULONG BeforeFgDx12::AddRef() { return ++refs_; }
ULONG BeforeFgDx12::Release() {
  const ULONG refs = --refs_;
  if (!refs) delete this;
  return refs;
}
HRESULT BeforeFgDx12::QueryInterface(REFIID iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
  *out = static_cast<IUnknown*>(this);
  AddRef();
  return S_OK;
}

std::string_view BeforeFgDx12::Attach(IDXGISwapChain* chain, IUnknown* queue, void* inner) {
  auto native = Native<ID3D12CommandQueue>(queue);
  if (!native) return "cannot unwrap D3D12 creation queue";
  if (native->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return "creation queue is not direct";
  if (!inner) return "no ReShade swapchain observed during creation";
  ComPtr<BeforeFgDx12> route;
  route.Attach(new BeforeFgDx12());
  route->creation_queue_ = std::move(native);
  route->inner_.store(inner);
  route->identity_ = chain;
  route->queues_.push_back({route->creation_queue_});
  if (FAILED(chain->SetPrivateDataInterface(ROUTE_ID, route.Get()))) return "could not store route on swapchain";
  return {};
}

ComPtr<BeforeFgDx12> BeforeFgDx12::Find(IDXGISwapChain* chain) {
  ComPtr<BeforeFgDx12> result;
  UINT size = sizeof(void*);
  if (FAILED(chain->GetPrivateData(ROUTE_ID, &size, result.GetAddressOf())) || !result || result->identity_ != chain) result.Reset();
  return result;
}

void BeforeFgDx12::Resize(UINT count, IUnknown* const* queues, void* inner) {
  std::lock_guard lock(mutex_);
  retry_.store(false);
  if (inner) inner_.store(inner);
  if (queues) {
    queues_.clear();
    for (UINT i = 0; i < count; ++i) queues_.push_back({Native<ID3D12CommandQueue>(queues[i])});
  } else if (queues_.size() == 1u) {
    queues_[0].ring.reset();
  } else if (count != 0u && count != queues_.size()) {
    queues_.clear();
  }
}

// Closes the ring's list when the recording scope ends, as the route always has.
struct CloseGuard {
  bridge::D3D12Side& ring;
  explicit CloseGuard(bridge::D3D12Side& r) : ring(r) {}
  ~CloseGuard() { ring.CloseList(); }
};

BeforeFgDx12::Result BeforeFgDx12::Record(IDXGISwapChain* chain, ID3D12Device* device, DeviceContext& context, const FrameSettings& settings,
                                           const FrameFacts& facts, color::ColorSpace space, std::chrono::steady_clock::time_point now,
                                           const HudPresentD3D12* hud) {
  std::lock_guard lock(mutex_);
  if (retry_.exchange(false)) return {.reason = uplift::text::WAIT_SUCCESSFUL_PRESENT_LINE};
  if (space == UNSUPPORTED_OUTER_SPACE && settings.encoding == color::Encoding::AUTO) return {.reason = uplift::text::OUTER_COLOR_SPACE_LINE};
  ComPtr<IDXGISwapChain3> chain3;
  if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) return {.reason = uplift::text::REAL_BACKBUFFER_UNKNOWN_LINE};
  const UINT index = chain3->GetCurrentBackBufferIndex();
  const size_t slot = queues_.size() == 1u ? 0u : index;
  if (slot >= queues_.size() || !queues_[slot].native) return {.reason = uplift::text::REAL_BACKBUFFER_QUEUE_LINE};
  auto& queue = queues_[slot];
  ComPtr<ID3D12Device> queue_device;
  if (FAILED(queue.native->GetDevice(IID_PPV_ARGS(&queue_device))) || Native<ID3D12Device>(queue_device.Get()).Get() != device) return {.reason = uplift::text::OUTER_QUEUE_DEVICE_LINE};
  ComPtr<ID3D12Resource> backbuffer;
  if (FAILED(chain3->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) return {.reason = uplift::text::REAL_BACKBUFFER_UNAVAILABLE_LINE};
  ComPtr<ID3D12Resource> native = Native<ID3D12Resource>(backbuffer.Get());
  ComPtr<ID3D12Device> buffer_device;
  if (!native || FAILED(native->GetDevice(IID_PPV_ARGS(&buffer_device))) || Native<ID3D12Device>(buffer_device.Get()).Get() != device) return {.reason = uplift::text::OUTER_IMAGE_DEVICE_LINE};
  if (!queue.ring) queue.ring = bridge::D3D12Side::OnGameQueue(queue.native.Get());
  // HUD-less (design §5): with use wanted the list is begun first, so the composer copies the game's HUD-less image into N before
  // BeginFrame is given N. This Present's tags are taken either way, so none outlive it.
  ID3D12GraphicsCommandList* list = nullptr;
  HudBegun hud_begun;
  if (hud && hud->use_wanted) {
    list = (queue.ring ? queue.ring->BeginList() : nullptr);
    if (list) {
      hud_begun = BeginHudD3D12(*hud, list, native.Get(), hud->input->Presents(), now);
    } else if (hud->tags) {
      static_cast<void>(hud->tags->Take());
      hud_begun.reason = HudReason::NO_FREE_IMAGE;
    }
  }
  ID3D12Resource* const record_target = (hud_begun.n ? hud_begun.n : native.Get());
  // The composer's End (when NR applied) and Close, then the list is executed: N's copy already touched it.
  const auto finish_hud = [&](bool applied) {
    if (!hud_begun.n) return;
    if (applied) hud->composer->End(list);
    hud->composer->Close(list);
  };
  const auto submit = [&]() {
    if (!queue.ring->ExecuteList() || !queue.ring->SignalProgress(queue.ring->LastSignalled() + 1u)) return false;
    if (hud_begun.n) hud->composer->Submitted(queue.native.Get());
    return true;
  };
  std::optional<CloseGuard> close;
  if (list) close.emplace(*queue.ring);
  const auto trigger = context.BeginFrame(queue.native.Get(), settings, facts, {.resource = record_target, .color_space = space}, false, now);
  // HUD-less (design §5): every return after BeginFrame cancels the frame it armed, so no later point records into this target.
  if (trigger == TriggerPoint::NONE || context.NrState() != nr::SessionState::ACTIVE) {
    context.CancelFrame();
    finish_hud(false);
    if (hud_begun.n && !submit()) return {.reason = uplift::text::REAL_FRAME_SUBMISSION_LINE, .hud_reason = hud_begun.reason};
    return {.hud_reason = hud_begun.reason};
  }
  if (!queue.ring) {
    context.CancelFrame();
    return {.reason = uplift::text::REAL_FRAME_COMMAND_LIST_LINE, .hud_reason = hud_begun.reason};
  }
  if (!list) {
    list = queue.ring->BeginList();
    if (!list) {
      context.CancelFrame();
      return {.reason = uplift::text::REAL_FRAME_RECORDING_BUSY_LINE, .hud_reason = hud_begun.reason};
    }
    close.emplace(*queue.ring);
  }
  GameList host(list, record_target);
  const auto result = context.Run(&host, D3D12_RESOURCE_STATE_PRESENT, trigger);
  const uint32_t passes = (result.nr_applied ? result.passes_run : 0u);
  finish_hud(passes != 0u);
  if ((host.touched || hud_begun.n) && !submit()) return {.reason = uplift::text::REAL_FRAME_SUBMISSION_LINE, .hud_reason = hud_begun.reason};
  Result done = {.passes = passes, .reason = nr::SkipReasonText(result.reason), .image = reinterpret_cast<uint64_t>(native.Get()),
                 .hud_reason = hud_begun.reason};
  if (hud_begun.n && passes != 0u) {
    done.replacement = {.owner = hud->composer->Owner(), .native = hud_begun.n};
    done.hud_inputs = hud_begun.inputs;
  }
  return done;
}

uint64_t BeforeFgDx12::CurrentBackBuffer(IDXGISwapChain* chain) {
  ComPtr<IDXGISwapChain3> chain3;
  if (chain == nullptr || FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) return 0u;
  ComPtr<ID3D12Resource> backbuffer;
  if (FAILED(chain3->GetBuffer(chain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) return 0u;
  return reinterpret_cast<uint64_t>(Native<ID3D12Resource>(backbuffer.Get()).Get());
}

}  // namespace uplift::addon
