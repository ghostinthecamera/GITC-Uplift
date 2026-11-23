#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace uplift::color {

// HUD-less (design §4): the Direct3D 12 composer. Begin copies the game's HUD-less image H into N, which NR then records into as
// if it were the back buffer; End puts the HUD back: back buffer = F + (1 - A) * (N - H), F the frame NR left untouched; Close always
// follows and leaves N where DLSS-G reads it (N_REST_STATE, measured 23 November). Every game resource goes back to the state its
// tag gave. Descriptors, images and pipelines come from the native device. Not thread-safe: the caller's lock.
class HudComposerD3D12 {
 public:
  static constexpr uint32_t RING = 4u;  // HUD_RING 3 plus one: frame generation keeps up to three frames in flight
  static constexpr uint64_t RETIRE_PRESENTS = 16u;  // HUD_RETIRE_PRESENTS: presents a retired ring waits before it is freed
  static constexpr D3D12_RESOURCE_STATES N_REST_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  HudComposerD3D12() = default;
  HudComposerD3D12(const HudComposerD3D12&) = delete;
  HudComposerD3D12& operator=(const HudComposerD3D12&) = delete;
  ~HudComposerD3D12();

  bool Initialize(ID3D12Device* native_device, std::string* error);

  struct Inputs {
    ID3D12Resource* back_buffer = nullptr;
    ID3D12Resource* hudless = nullptr;
    D3D12_RESOURCE_STATES hudless_state = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* opacity = nullptr;
    D3D12_RESOURCE_STATES opacity_state = D3D12_RESOURCE_STATE_COMMON;
    bool opacity_from_ui_colour = false;
  };
  // H -> N, N left in PRESENT for NR. Null, recording nothing, when the back buffer's format has no pass, no slot is free (its N
  // is still held, or its last submission has not finished) or an image could not be made: the frame is hidden.
  ID3D12Resource* Begin(ID3D12GraphicsCommandList* list, const Inputs& inputs, uint64_t present_number);
  // After NR applied into N: F = raw copy of the back buffer, then the pass draws into the back buffer (left in PRESENT).
  void End(ID3D12GraphicsCommandList* list);
  // Always after Begin: N to N_REST_STATE.
  void Close(ID3D12GraphicsCommandList* list);
  // After the list with this frame's commands was executed on `queue`: the slot is busy until this signal.
  void Submitted(ID3D12CommandQueue* queue);
  // Keeps the current slot's N alive (FgInput's entry and the evaluate hold it).
  [[nodiscard]] std::shared_ptr<const void> Owner() const;
  [[nodiscard]] ID3D12Resource* Current() const;
  // Frees retired rings whose presents have passed, whose work finished and whose N nobody holds.
  void Retire(uint64_t present_number);
  // No longer wanted (UICorrection left On, or DLSS-G stopped reading Uplift's output): the ring retires as on a size change.
  void Release(uint64_t present_number);
  [[nodiscard]] size_t RetiringCount() const { return retiring_.size(); }
  void Destroy();

 private:
  struct Image {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  };
  struct Slot {
    std::shared_ptr<Image> n;
    uint64_t fence_value = 0u;  // signalled when the slot's last frame finished
    bool pending = false;       // begun, not yet submitted
  };
  struct Ring {
    uint32_t width = 0u, height = 0u;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::array<Slot, RING> slots;
    Microsoft::WRL::ComPtr<ID3D12Resource> final_copy;    // F, typeless
    Microsoft::WRL::ComPtr<ID3D12Resource> hudless_copy;  // H's raw copy when H is sRGB-typed, typeless
    uint64_t retired_at = 0u;
  };
  bool EnsureRing(ID3D12Resource* back_buffer, ID3D12Resource* hudless, uint64_t present_number);
  ID3D12PipelineState* PipelineFor(DXGI_FORMAT target_format);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  std::unordered_map<DXGI_FORMAT, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;  // shader-visible, four per slot
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;  // one per slot
  uint32_t srv_size_ = 0u, rtv_size_ = 0u;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  uint64_t fence_value_ = 0u;
  std::unique_ptr<Ring> ring_;
  std::vector<std::unique_ptr<Ring>> retiring_;
  bool allocation_failed_ = false;  // until the size or format changes
  uint32_t failed_width_ = 0u, failed_height_ = 0u;
  DXGI_FORMAT failed_format_ = DXGI_FORMAT_UNKNOWN;
  // The frame in progress.
  Inputs frame_;
  uint32_t slot_ = 0u;
  bool begun_ = false;
  bool ended_ = false;
  uint32_t next_slot_ = 0u;
};

}  // namespace uplift::color
