#include "color/hud_composer_d3d12.hpp"

#include <climits>
#include <optional>
#include <utility>

#include "color/encoding.hpp"
#include "decode_vs.h"
#include "hud_compose_ps.h"
#include "nr/log.hpp"

namespace uplift::color {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t SHADER_RESOURCES = 4u;  // F, N, H, A
constexpr uint32_t ROOT_CONSTANTS = 2u;    // alpha_channel, clamp_unit
constexpr uint32_t CONSTANTS_PARAMETER = 0u;
constexpr uint32_t TABLE_PARAMETER = 1u;
constexpr uint32_t ALPHA_IN_RED = 0u;
constexpr uint32_t ALPHA_IN_ALPHA = 3u;
constexpr uint32_t TRIANGLE_VERTICES = 3u;
constexpr D3D12_RESOURCE_STATES READ_IN_PASS = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  if (resource == nullptr || before == after) return;
  D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
  barrier.Transition = {.pResource = resource, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = before, .StateAfter = after};
  list->ResourceBarrier(1u, &barrier);
}

bool IsSrgb(DXGI_FORMAT format) {
  return (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}

bool IsTypeless(DXGI_FORMAT format) {
  return (format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_R10G10B10A2_TYPELESS
          || format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
}

// The format an SRV reads a resource's stored values with: its own when typed, its family's UNORM or FLOAT when typeless.
DXGI_FORMAT ReadFormat(DXGI_FORMAT format) {
  if (!IsTypeless(format)) return format;
  const std::optional<FormatInfo> info = DescribeFormat(format);
  return (info ? info->source_view_format : format);
}

// Where the opacity is: .r for single- and three-channel UI alpha, .a for UI colour and alpha, A8 and four-channel images.
uint32_t AlphaChannel(DXGI_FORMAT format, bool ui_colour) {
  if (ui_colour) return ALPHA_IN_ALPHA;
  switch (format) {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return ALPHA_IN_RED;
    default:
      return ALPHA_IN_ALPHA;
  }
}

ComPtr<ID3D12Resource> MakeImage(ID3D12Device* device, const D3D12_RESOURCE_DESC& like, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES state, const wchar_t* name) {
  D3D12_RESOURCE_DESC description = like;
  description.Format = format;
  description.Flags = flags;
  description.MipLevels = 1u;
  description.DepthOrArraySize = 1u;
  description.SampleDesc = {1u, 0u};
  description.Alignment = 0u;
  description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> image;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&image)))) return nullptr;
  image->SetName(name);
  return image;
}

}  // namespace

HudComposerD3D12::~HudComposerD3D12() { Destroy(); }

bool HudComposerD3D12::Initialize(ID3D12Device* native_device, std::string* error) {
  device_ = native_device;
  const D3D12_DESCRIPTOR_RANGE range = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = SHADER_RESOURCES, .BaseShaderRegister = 0u,
                                        .OffsetInDescriptorsFromTableStart = 0u};
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[CONSTANTS_PARAMETER].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[CONSTANTS_PARAMETER].Constants = {.ShaderRegister = 0u, .RegisterSpace = 0u, .Num32BitValues = ROOT_CONSTANTS};
  parameters[CONSTANTS_PARAMETER].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[TABLE_PARAMETER].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[TABLE_PARAMETER].DescriptorTable = {.NumDescriptorRanges = 1u, .pDescriptorRanges = &range};
  parameters[TABLE_PARAMETER].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC signature = {.NumParameters = 2u, .pParameters = parameters};
  ComPtr<ID3DBlob> blob, messages;
  if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages))
      || FAILED(device_->CreateRootSignature(0u, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)))) {
    *error = "the HUD-less composer's root signature could not be made";
    return false;
  }
  const D3D12_DESCRIPTOR_HEAP_DESC srv = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = RING * SHADER_RESOURCES,
                                          .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  const D3D12_DESCRIPTOR_HEAP_DESC rtv = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = RING};
  if (FAILED(device_->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srv_heap_))) || FAILED(device_->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtv_heap_)))
      || FAILED(device_->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    *error = "the HUD-less composer's descriptors or fence could not be made";
    return false;
  }
  srv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  return true;
}

ID3D12PipelineState* HudComposerD3D12::PipelineFor(DXGI_FORMAT target_format) {
  if (const auto found = pipelines_.find(target_format); found != pipelines_.end()) return found->second.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC description = {};
  description.pRootSignature = root_signature_.Get();
  description.VS = {.pShaderBytecode = g_decode_vs, .BytecodeLength = sizeof(g_decode_vs)};
  description.PS = {.pShaderBytecode = g_hud_compose_ps, .BytecodeLength = sizeof(g_hud_compose_ps)};
  description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  description.SampleMask = UINT_MAX;
  description.RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_NONE, .DepthClipEnable = TRUE};
  description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  description.NumRenderTargets = 1u;
  description.RTVFormats[0] = target_format;
  description.SampleDesc = {1u, 0u};
  ComPtr<ID3D12PipelineState> pipeline;
  if (FAILED(device_->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)))) {
    nr::Logf(nr::LogLevel::WARN, "HUD-less composer: no pipeline for back-buffer format {}", static_cast<int>(target_format));
    return nullptr;
  }
  return pipelines_.emplace(target_format, std::move(pipeline)).first->second.Get();
}

bool HudComposerD3D12::EnsureRing(ID3D12Resource* back_buffer, ID3D12Resource* hudless, uint64_t present_number) {
  const D3D12_RESOURCE_DESC wanted = back_buffer->GetDesc();
  const bool hudless_srgb = IsSrgb(hudless->GetDesc().Format);
  const auto width = static_cast<uint32_t>(wanted.Width);
  if (ring_ && ring_->width == width && ring_->height == wanted.Height && ring_->format == wanted.Format && (ring_->hudless_copy != nullptr) == hudless_srgb) {
    return true;
  }
  if (allocation_failed_ && failed_width_ == width && failed_height_ == wanted.Height && failed_format_ == wanted.Format) return false;
  allocation_failed_ = false;
  const std::optional<FormatInfo> info = DescribeFormat(wanted.Format);
  if (!info) return false;
  if (ring_) {
    ring_->retired_at = present_number;
    retiring_.push_back(std::move(ring_));
  }
  auto ring = std::make_unique<Ring>();
  ring->width = width;
  ring->height = wanted.Height;
  ring->format = wanted.Format;
  bool made = true;
  for (Slot& slot : ring->slots) {
    slot.n = std::make_shared<Image>();
    slot.n->resource = MakeImage(device_.Get(), wanted, wanted.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, N_REST_STATE, L"Uplift HUD-less N");
    made = made && slot.n->resource;
  }
  ring->final_copy = MakeImage(device_.Get(), wanted, info->copy_format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"Uplift HUD-less F");
  made = made && ring->final_copy;
  if (hudless_srgb) {
    const std::optional<FormatInfo> hudless_info = DescribeFormat(hudless->GetDesc().Format);
    ring->hudless_copy = (hudless_info ? MakeImage(device_.Get(), wanted, hudless_info->copy_format, D3D12_RESOURCE_FLAG_NONE,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, L"Uplift HUD-less H copy")
                                       : nullptr);
    made = made && ring->hudless_copy;
  }
  if (!made) {
    nr::Logf(nr::LogLevel::WARN, "HUD-less composer: its images ({}x{}) could not be made; the HUD-less image is hidden until the size or format changes",
             width, wanted.Height);
    allocation_failed_ = true;
    failed_width_ = width;
    failed_height_ = wanted.Height;
    failed_format_ = wanted.Format;
    return false;
  }
  ring_ = std::move(ring);
  next_slot_ = 0u;
  return true;
}

ID3D12Resource* HudComposerD3D12::Begin(ID3D12GraphicsCommandList* list, const Inputs& inputs, uint64_t present_number) {
  if (!device_ || begun_ || inputs.back_buffer == nullptr || inputs.hudless == nullptr || inputs.opacity == nullptr) return nullptr;
  if (IsSrgb(inputs.back_buffer->GetDesc().Format)) return nullptr;  // no UNORM render target over an sRGB-typed image
  if (!EnsureRing(inputs.back_buffer, inputs.hudless, present_number)) return nullptr;
  const uint64_t completed = fence_->GetCompletedValue();
  std::optional<uint32_t> free;
  for (uint32_t step = 0u; step < RING && !free; ++step) {
    const uint32_t index = (next_slot_ + step) % RING;
    const Slot& slot = ring_->slots[index];
    if (!slot.pending && completed >= slot.fence_value && slot.n.use_count() == 1) free = index;
  }
  if (!free) return nullptr;
  if (!PipelineFor(DescribeFormat(ring_->format)->source_view_format)) return nullptr;
  slot_ = *free;
  next_slot_ = (slot_ + 1u) % RING;
  Slot& slot = ring_->slots[slot_];
  slot.pending = true;
  frame_ = inputs;
  begun_ = true;
  ended_ = false;
  ID3D12Resource* const n = slot.n->resource.Get();
  Barrier(list, inputs.hudless, inputs.hudless_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(list, n, N_REST_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
  // The first mip only: H may have more (design §3: one mip is used); N and H's copy have one.
  const auto copy_first_mip = [&](ID3D12Resource* to) {
    const D3D12_TEXTURE_COPY_LOCATION destination = {.pResource = to, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
    const D3D12_TEXTURE_COPY_LOCATION source = {.pResource = inputs.hudless, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0u};
    list->CopyTextureRegion(&destination, 0u, 0u, 0u, &source, nullptr);
  };
  copy_first_mip(n);
  if (ring_->hudless_copy) copy_first_mip(ring_->hudless_copy.Get());
  Barrier(list, inputs.hudless, D3D12_RESOURCE_STATE_COPY_SOURCE, inputs.hudless_state);
  Barrier(list, n, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);  // where NR expects a back buffer
  return n;
}

void HudComposerD3D12::End(ID3D12GraphicsCommandList* list) {
  if (!begun_ || ended_) return;
  Ring& ring = *ring_;
  ID3D12Resource* const n = ring.slots[slot_].n->resource.Get();
  ID3D12Resource* const back_buffer = frame_.back_buffer;
  const FormatInfo info = *DescribeFormat(ring.format);
  const DXGI_FORMAT target_format = info.source_view_format;  // UNORM or FLOAT: never an sRGB render target
  ID3D12PipelineState* const pipeline = PipelineFor(target_format);
  // F: the frame as NR left it, raw.
  Barrier(list, back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(ring.final_copy.Get(), back_buffer);
  Barrier(list, back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  Barrier(list, ring.final_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, READ_IN_PASS);
  Barrier(list, n, D3D12_RESOURCE_STATE_PRESENT, READ_IN_PASS);
  ID3D12Resource* const hudless = (ring.hudless_copy ? ring.hudless_copy.Get() : frame_.hudless);
  const D3D12_RESOURCE_STATES hudless_state = (ring.hudless_copy ? D3D12_RESOURCE_STATE_COPY_DEST : frame_.hudless_state);
  Barrier(list, hudless, hudless_state, READ_IN_PASS);
  Barrier(list, frame_.opacity, frame_.opacity_state, READ_IN_PASS);

  const DXGI_FORMAT hudless_format = frame_.hudless->GetDesc().Format;
  const DXGI_FORMAT hudless_read = (ring.hudless_copy ? DescribeFormat(hudless_format)->source_view_format : ReadFormat(hudless_format));
  const DXGI_FORMAT opacity_format = frame_.opacity->GetDesc().Format;
  const std::pair<ID3D12Resource*, DXGI_FORMAT> reads[SHADER_RESOURCES] = {
      {ring.final_copy.Get(), info.source_view_format}, {n, ReadFormat(ring.format)}, {hudless, hudless_read}, {frame_.opacity, ReadFormat(opacity_format)}};
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(slot_) * SHADER_RESOURCES * srv_size_;
  for (const auto& [resource, format] : reads) {
    const D3D12_SHADER_RESOURCE_VIEW_DESC view = {.Format = format, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                                                  .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                                  .Texture2D = {.MostDetailedMip = 0u, .MipLevels = 1u}};
    device_->CreateShaderResourceView(resource, &view, cpu);
    cpu.ptr += srv_size_;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(slot_) * rtv_size_;
  const D3D12_RENDER_TARGET_VIEW_DESC target = {.Format = target_format, .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D};
  device_->CreateRenderTargetView(back_buffer, &target, rtv);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(slot_) * SHADER_RESOURCES * srv_size_;

  const bool unit = (info.source_view_format != DXGI_FORMAT_R16G16B16A16_FLOAT);
  const uint32_t constants[ROOT_CONSTANTS] = {AlphaChannel(opacity_format, frame_.opacity_from_ui_colour), unit ? 1u : 0u};
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
  list->SetDescriptorHeaps(1u, heaps);
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetGraphicsRoot32BitConstants(CONSTANTS_PARAMETER, ROOT_CONSTANTS, constants, 0u);
  list->SetGraphicsRootDescriptorTable(TABLE_PARAMETER, gpu);
  list->SetPipelineState(pipeline);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VIEWPORT viewport = {.Width = static_cast<float>(ring.width), .Height = static_cast<float>(ring.height), .MaxDepth = 1.f};
  const D3D12_RECT scissor = {.right = static_cast<LONG>(ring.width), .bottom = static_cast<LONG>(ring.height)};
  list->RSSetViewports(1u, &viewport);
  list->RSSetScissorRects(1u, &scissor);
  list->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
  list->DrawInstanced(TRIANGLE_VERTICES, 1u, 0u, 0u);

  Barrier(list, back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  Barrier(list, ring.final_copy.Get(), READ_IN_PASS, D3D12_RESOURCE_STATE_COPY_DEST);
  Barrier(list, hudless, READ_IN_PASS, hudless_state);
  Barrier(list, frame_.opacity, READ_IN_PASS, frame_.opacity_state);
  ended_ = true;
}

void HudComposerD3D12::Close(ID3D12GraphicsCommandList* list) {
  if (!begun_) return;
  Barrier(list, ring_->slots[slot_].n->resource.Get(), (ended_ ? READ_IN_PASS : D3D12_RESOURCE_STATE_PRESENT), N_REST_STATE);
  begun_ = false;
}

void HudComposerD3D12::Submitted(ID3D12CommandQueue* queue) {
  if (!ring_ || !ring_->slots[slot_].pending || begun_) return;
  Slot& slot = ring_->slots[slot_];
  if (SUCCEEDED(queue->Signal(fence_.Get(), fence_value_ + 1u))) slot.fence_value = ++fence_value_;
  slot.pending = false;
}

std::shared_ptr<const void> HudComposerD3D12::Owner() const {
  if (!ring_) return nullptr;
  return ring_->slots[slot_].n;
}

ID3D12Resource* HudComposerD3D12::Current() const { return (ring_ ? ring_->slots[slot_].n->resource.Get() : nullptr); }

void HudComposerD3D12::Retire(uint64_t present_number) {
  const uint64_t completed = (fence_ ? fence_->GetCompletedValue() : 0u);
  std::erase_if(retiring_, [&](const std::unique_ptr<Ring>& ring) {
    if (present_number < ring->retired_at + RETIRE_PRESENTS) return false;
    for (const Slot& slot : ring->slots) {
      if (slot.pending || completed < slot.fence_value || slot.n.use_count() != 1) return false;
    }
    return true;
  });
}

void HudComposerD3D12::Release(uint64_t present_number) {
  if (!ring_ || begun_) return;
  ring_->retired_at = present_number;
  retiring_.push_back(std::move(ring_));
}

void HudComposerD3D12::Destroy() {
  ring_.reset();
  retiring_.clear();
  pipelines_.clear();
  srv_heap_.Reset();
  rtv_heap_.Reset();
  root_signature_.Reset();
  fence_.Reset();
  device_.Reset();
  begun_ = false;
}

}  // namespace uplift::color
