#include "color/color_pipeline.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <format>

#include "decode_cs.h"
#include "decode_ps.h"
#include "decode_vs.h"
#include "encode_cs.h"
#include "faces_cs.h"
#include "look_pyramid_cs.h"
#include "look_shape_cs.h"
#include "look_stabilize_cs.h"
#include "meter_cs.h"
#include "motion_cs.h"
#include "nr/log.hpp"
#include "resolve_cs.h"

namespace uplift::color {
namespace {

constexpr uint32_t ROOT_CONSTANTS = 32u;     // the root signature's; each pass sets only what its shader reads
constexpr uint32_t PASS_CONSTANTS = 16u;     // Plans 2-4's passes
constexpr uint32_t SHADER_RESOURCES = 5u;    // t0-t4
constexpr uint32_t UNORDERED_ACCESSES = 2u;  // u0, u1
constexpr uint32_t TABLE_SIZE = SHADER_RESOURCES + UNORDERED_ACCESSES;
// One table per pass kind in each slot, so the passes of one recording never overwrite each other's descriptors.
constexpr uint32_t ENCODE_TABLE = 0u * TABLE_SIZE;
constexpr uint32_t CHANGE_TABLE = 1u * TABLE_SIZE;
constexpr uint32_t DECODE_TABLE = 2u * TABLE_SIZE;
constexpr uint32_t MOTION_TABLE = 3u * TABLE_SIZE;
constexpr uint32_t METER_TABLE = 4u * TABLE_SIZE;
constexpr uint32_t PYRAMID_TABLE = 5u * TABLE_SIZE;
constexpr uint32_t STABILIZE_TABLE = 6u * TABLE_SIZE;
constexpr uint32_t DETAIL_TABLE = 7u * TABLE_SIZE;
constexpr uint32_t SHAPE_TABLE = 8u * TABLE_SIZE;
constexpr uint32_t RESOLVE_TABLE = 9u * TABLE_SIZE;
constexpr uint32_t RESOLVE_SWAPPED_TABLE = 10u * TABLE_SIZE;
// Keep faces: pass 1's pyramid and combine, a later pass's (A -> B, or swapped B -> A), and Show the face mask.
constexpr uint32_t FACES_LEVEL_FIRST_TABLE = 11u * TABLE_SIZE;
constexpr uint32_t FACES_LEVEL_TABLE = 12u * TABLE_SIZE;
constexpr uint32_t FACES_LEVEL_SWAPPED_TABLE = 13u * TABLE_SIZE;
constexpr uint32_t FACES_COMBINE_FIRST_TABLE = 14u * TABLE_SIZE;
constexpr uint32_t FACES_COMBINE_TABLE = 15u * TABLE_SIZE;
constexpr uint32_t FACES_COMBINE_SWAPPED_TABLE = 16u * TABLE_SIZE;
constexpr uint32_t FACES_SHOW_TABLE = 17u * TABLE_SIZE;
// Fix round 4: the despike of pass 1's difference, and of a later pass's (A -> B, or swapped B -> A).
constexpr uint32_t FACES_DESPIKE_FIRST_TABLE = 18u * TABLE_SIZE;
constexpr uint32_t FACES_DESPIKE_TABLE = 19u * TABLE_SIZE;
constexpr uint32_t FACES_DESPIKE_SWAPPED_TABLE = 20u * TABLE_SIZE;
constexpr uint32_t DESCRIPTORS_PER_SLOT = 21u * TABLE_SIZE;
constexpr uint32_t STABILIZE_CUT = 0u;  // look_stabilize_cs.hlsl's modes and flags
constexpr uint32_t STABILIZE_LEVELS = 1u;
constexpr uint32_t STABILIZE_DETAIL = 2u;
constexpr uint32_t STABILIZE_FLAG_RESET = 1u;
constexpr uint32_t STABILIZE_FLAG_MOTION = 2u;
constexpr uint32_t STABILIZE_FLAG_VECTORS = 4u;
constexpr uint32_t FACES_MODE_LEVEL = 0u;  // faces_cs.hlsl's modes
constexpr uint32_t FACES_MODE_FIRST = 1u;
constexpr uint32_t FACES_MODE_LATER = 2u;
constexpr uint32_t FACES_MODE_SHOW = 3u;
constexpr uint32_t FACES_MODE_DESPIKE = 4u;
constexpr uint32_t FACES_FLAG_FIRST = 1u;  // faces_cs.hlsl's flags
constexpr uint32_t FACES_FLAG_FILL = 2u;

uint32_t Bits(float value) {
  return std::bit_cast<uint32_t>(value);
}

// faces_cs.hlsl's constants. A level dispatch's `first` matters at level 1 only.
std::array<uint32_t, 16> FacesConstants(nr::Size size, uint32_t levels, uint32_t mode, uint32_t level, uint32_t flags,
                                        const look::FacesParameters& parameters) {
  return {size.width, size.height, levels, mode, level, flags, Bits(parameters.low_level), Bits(parameters.mask_level),
          Bits(parameters.edge_level), Bits(parameters.dead_zone), Bits(parameters.full_weight), Bits(parameters.coverage_gain), Bits(parameters.density),
          parameters.speck_radius, Bits(parameters.fill_level), Bits(parameters.fill_tolerance)};
}

// shaders/encode_cs.hlsl's constants; `change_mode` selects its change-field pass, `second_output` its u1 write.
std::array<uint32_t, PASS_CONSTANTS> EncodeConstants(const EncodePass& pass, uint32_t change_mode, bool second_output) {
  const bool whole = (pass.region.width == 0u || pass.region.height == 0u);
  const bool unpadded = pass.canvas.Empty();
  return {
      static_cast<uint32_t>(pass.encoding), pass.width, pass.height, Bits(pass.input_scale),
      (second_output ? 1u : 0u), (pass.exposure != nullptr ? 1u : 0u), Bits(pass.exposure_factor),
      (whole ? 0u : pass.region.x), (whole ? 0u : pass.region.y), (whole ? pass.width : pass.region.width),
      (whole ? pass.height : pass.region.height), (unpadded ? pass.width : pass.canvas.width),
      (unpadded ? pass.height : pass.canvas.height), change_mode, pass.options, 0u,
  };
}

// look_stabilize_cs.hlsl's constants for one mode.
std::array<uint32_t, PASS_CONSTANTS> StabilizeConstants(uint32_t mode, const StabilizePass& pass) {
  const StabilizeMotion& motion = pass.motion;
  const bool vectors = (motion.vectors.resource != nullptr);
  const uint32_t flags = (pass.reset ? STABILIZE_FLAG_RESET : 0u) | (pass.motion_mode ? STABILIZE_FLAG_MOTION : 0u)
                         | (vectors ? STABILIZE_FLAG_VECTORS : 0u);
  return {
      pass.atlas.image.width, pass.atlas.image.height, pass.atlas.levels, mode, pass.bands.stable_level, flags,
      Bits(pass.rate), Bits(pass.bands.low_level), Bits(motion.scale_x), Bits(motion.scale_y), motion.vectors.rect.x,
      motion.vectors.rect.y, (vectors ? motion.vectors.rect.width : 1u), (vectors ? motion.vectors.rect.height : 1u), 0u, 0u,
  };
}

// The stabiliser's motion SRV: a null view still needs a real format.
DXGI_FORMAT MotionViewOf(const StabilizeMotion& motion) {
  return (motion.vectors.resource != nullptr ? motion.view_format : DXGI_FORMAT_R16G16_FLOAT);
}

void UavBarriers(ID3D12GraphicsCommandList* list, std::initializer_list<ID3D12Resource*> resources) {
  std::array<D3D12_RESOURCE_BARRIER, 2> barriers = {};
  uint32_t count = 0u;
  for (ID3D12Resource* const resource : resources) {
    barriers[count] = {.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE};
    barriers[count].UAV = {.pResource = resource};
    ++count;
  }
  list->ResourceBarrier(count, barriers.data());
}

}  // namespace

bool ColorPipeline::Initialize(ID3D12Device* device, std::string* error) {
  device_ = device;
  const D3D12_DESCRIPTOR_RANGE ranges[] = {
      {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = SHADER_RESOURCES, .BaseShaderRegister = 0u,
       .RegisterSpace = 0u, .OffsetInDescriptorsFromTableStart = 0u},
      {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, .NumDescriptors = UNORDERED_ACCESSES, .BaseShaderRegister = 0u,
       .RegisterSpace = 0u, .OffsetInDescriptorsFromTableStart = SHADER_RESOURCES},
  };
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {.ShaderRegister = 0u, .RegisterSpace = 0u, .Num32BitValues = ROOT_CONSTANTS};
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {.NumDescriptorRanges = 2u, .pDescriptorRanges = ranges};
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  const D3D12_ROOT_SIGNATURE_DESC signature = {.NumParameters = 2u, .pParameters = parameters, .NumStaticSamplers = 0u,
                                               .pStaticSamplers = nullptr, .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE};
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> messages;
  if (const HRESULT result = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages);
      FAILED(result)) {
    *error = "could not serialize the colour root signature";
    nr::Logf(nr::LogLevel::ERR, "D3D12SerializeRootSignature failed: {:#010x}{}", static_cast<uint32_t>(result),
             (messages ? std::string(": ") + static_cast<const char*>(messages->GetBufferPointer()) : std::string()));
    return false;
  }
  if (const HRESULT result =
          device->CreateRootSignature(0u, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_));
      FAILED(result)) {
    *error = "could not create the colour root signature";
    nr::Logf(nr::LogLevel::ERR, "CreateRootSignature failed: {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  const D3D12_COMPUTE_PIPELINE_STATE_DESC encode = {
      .pRootSignature = root_signature_.Get(),
      .CS = {.pShaderBytecode = g_encode_cs, .BytecodeLength = sizeof(g_encode_cs)},
  };
  if (const HRESULT result = device->CreateComputePipelineState(&encode, IID_PPV_ARGS(&encode_pipeline_));
      FAILED(result)) {
    *error = "could not create the encode pipeline";
    nr::Logf(nr::LogLevel::ERR, "CreateComputePipelineState (encode) failed: {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  const D3D12_COMPUTE_PIPELINE_STATE_DESC compute_decode = {
      .pRootSignature = root_signature_.Get(),
      .CS = {.pShaderBytecode = g_decode_cs, .BytecodeLength = sizeof(g_decode_cs)},
  };
  if (const HRESULT result = device->CreateComputePipelineState(&compute_decode, IID_PPV_ARGS(&compute_decode_pipeline_));
      FAILED(result)) {
    *error = "could not create the compute decode pipeline";
    nr::Logf(nr::LogLevel::ERR, "CreateComputePipelineState (decode) failed: {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  const D3D12_COMPUTE_PIPELINE_STATE_DESC motion = {
      .pRootSignature = root_signature_.Get(),
      .CS = {.pShaderBytecode = g_motion_cs, .BytecodeLength = sizeof(g_motion_cs)},
  };
  if (const HRESULT result = device->CreateComputePipelineState(&motion, IID_PPV_ARGS(&motion_pipeline_)); FAILED(result)) {
    *error = "could not create the motion-vector copy pipeline";
    nr::Logf(nr::LogLevel::ERR, "CreateComputePipelineState (motion) failed: {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  // Plan 5: the look, meter and resolve pipelines.
  const auto build_compute = [&](const void* bytecode, size_t length, Microsoft::WRL::ComPtr<ID3D12PipelineState>* pipeline,
                                 const char* name) {
    const D3D12_COMPUTE_PIPELINE_STATE_DESC description = {
        .pRootSignature = root_signature_.Get(),
        .CS = {.pShaderBytecode = bytecode, .BytecodeLength = length},
    };
    const HRESULT result = device->CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline->GetAddressOf()));
    if (FAILED(result)) {
      *error = std::format("could not create the {} pipeline", name);
      nr::Logf(nr::LogLevel::ERR, "CreateComputePipelineState ({}) failed: {:#010x}", name, static_cast<uint32_t>(result));
    }
    return SUCCEEDED(result);
  };
  if (!build_compute(g_meter_cs, sizeof(g_meter_cs), &meter_pipeline_, "meter")
      || !build_compute(g_look_pyramid_cs, sizeof(g_look_pyramid_cs), &pyramid_pipeline_, "look pyramid")
      || !build_compute(g_look_stabilize_cs, sizeof(g_look_stabilize_cs), &stabilize_pipeline_, "look stabiliser")
      || !build_compute(g_look_shape_cs, sizeof(g_look_shape_cs), &shape_pipeline_, "look shape")
      || !build_compute(g_resolve_cs, sizeof(g_resolve_cs), &resolve_pipeline_, "resolve")) {
    return false;
  }
  // Keep faces fix round 1 (C3): optional. Without it NR runs as before and Keep faces is unavailable (FacesReady).
  const D3D12_COMPUTE_PIPELINE_STATE_DESC faces = {.pRootSignature = root_signature_.Get(), .CS = {.pShaderBytecode = g_faces_cs, .BytecodeLength = sizeof(g_faces_cs)}};
  if (const HRESULT result = device->CreateComputePipelineState(&faces, IID_PPV_ARGS(&faces_pipeline_)); FAILED(result)) {
    faces_pipeline_.Reset();
    nr::Logf(nr::LogLevel::WARN, "Keep faces is unavailable on this device: its pipeline could not be built ({:#010x})", static_cast<uint32_t>(result));
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  uav_loads_ = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
               && options.TypedUAVLoadAdditionalFormats != FALSE;
  if (!uav_loads_) {
    nr::Log(nr::LogLevel::WARN, "this GPU has no typed UAV loads: Shape result, Metered exposure, pass strengths and Keep faces are unavailable");
  }
  const D3D12_DESCRIPTOR_HEAP_DESC shader_heap = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                  .NumDescriptors = RING_SLOTS * DESCRIPTORS_PER_SLOT,
                                                  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, .NodeMask = 0u};
  const D3D12_DESCRIPTOR_HEAP_DESC target_heap = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1u,
                                                  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE, .NodeMask = 0u};
  if (const HRESULT result = device->CreateDescriptorHeap(&shader_heap, IID_PPV_ARGS(&shader_heap_)); FAILED(result)) {
    *error = "could not create the colour shader descriptor heap";
    nr::Logf(nr::LogLevel::ERR, "CreateDescriptorHeap (shader) failed: {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  shader_heap_->SetName(L"Uplift colour descriptors");  // Plan 17: DRED names
  if (const HRESULT result = device->CreateDescriptorHeap(&target_heap, IID_PPV_ARGS(&target_heap_)); FAILED(result)) {
    *error = "could not create the colour target descriptor heap";
    nr::Logf(nr::LogLevel::ERR, "CreateDescriptorHeap (target) failed: {:#010x}", static_cast<uint32_t>(result));
    return false;
  }
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  // Every target format up front: no driver compile in the present path, and nothing allocated after the
  // end-to-end test's VRAM baseline except NR itself.
  const auto build_decode = [this](DXGI_FORMAT format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description = {};
    description.pRootSignature = root_signature_.Get();
    description.VS = {.pShaderBytecode = g_decode_vs, .BytecodeLength = sizeof(g_decode_vs)};
    description.PS = {.pShaderBytecode = g_decode_ps, .BytecodeLength = sizeof(g_decode_ps)};
    description.BlendState.RenderTarget[0] = {
        .BlendEnable = FALSE, .LogicOpEnable = FALSE,
        .SrcBlend = D3D12_BLEND_ONE, .DestBlend = D3D12_BLEND_ZERO, .BlendOp = D3D12_BLEND_OP_ADD,
        .SrcBlendAlpha = D3D12_BLEND_ONE, .DestBlendAlpha = D3D12_BLEND_ZERO, .BlendOpAlpha = D3D12_BLEND_OP_ADD,
        .LogicOp = D3D12_LOGIC_OP_NOOP, .RenderTargetWriteMask = static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL)};
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1u;
    description.RTVFormats[0] = format;
    description.SampleDesc = {.Count = 1u, .Quality = 0u};
    auto& pipeline = decode_pipelines_[format];
    if (const HRESULT result = device_->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)); FAILED(result)) {
      nr::Logf(nr::LogLevel::ERR, "could not create the decode pipeline for DXGI format {}: {:#010x}",
               static_cast<int>(format), static_cast<uint32_t>(result));
    }
  };
  for (const DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                                   DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R10G10B10A2_UNORM,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT}) {
    build_decode(format);
  }
  return true;
}

ColorPipeline::View ColorPipeline::ExposureView(ID3D12Resource* exposure) {
  if (exposure == nullptr) return {nullptr, DXGI_FORMAT_R32_FLOAT};
  const DXGI_FORMAT native = exposure->GetDesc().Format;
  switch (native) {
    case DXGI_FORMAT_R32_TYPELESS:       return {exposure, DXGI_FORMAT_R32_FLOAT};
    case DXGI_FORMAT_R16_TYPELESS:       return {exposure, DXGI_FORMAT_R16_FLOAT};
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {exposure, native};
    default:                             return {nullptr, DXGI_FORMAT_R32_FLOAT};  // a null view reads 0, which GameExposure replaces with 1
  }
}

void ColorPipeline::WriteTable(uint32_t base, std::initializer_list<View> shader_resources, std::initializer_list<View> unordered) {
  std::array<View, SHADER_RESOURCES> resources = {};
  std::ranges::copy(shader_resources, resources.begin());
  for (uint32_t index = 0u; index < SHADER_RESOURCES; ++index) {
    WriteShaderResource(base + index, resources[index].resource, resources[index].format);
  }
  std::array<View, UNORDERED_ACCESSES> accesses = {};
  std::ranges::copy(unordered, accesses.begin());
  for (uint32_t index = 0u; index < UNORDERED_ACCESSES; ++index) {
    WriteUnorderedAccess(base + SHADER_RESOURCES + index, accesses[index].resource, accesses[index].format);
  }
}

bool ColorPipeline::SlotInRange(uint32_t slot, const char* pass) const {
  if (slot < RING_SLOTS) return true;
  nr::Logf(nr::LogLevel::ERR, "{}: slot {} is out of range (RING_SLOTS = {})", pass, slot, RING_SLOTS);
  return false;
}

void ColorPipeline::DispatchCompute(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline,
                                    std::span<const uint32_t> constants, uint32_t base, uint32_t width, uint32_t height) {
  ID3D12DescriptorHeap* const heaps[] = {shader_heap_.Get()};
  list->SetDescriptorHeaps(1u, heaps);
  list->SetComputeRootSignature(root_signature_.Get());
  list->SetComputeRoot32BitConstants(0u, static_cast<UINT>(constants.size()), constants.data(), 0u);
  list->SetComputeRootDescriptorTable(1u, GpuDescriptor(base));
  list->SetPipelineState(pipeline);
  list->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);
}

bool ColorPipeline::RecordEncode(ID3D12GraphicsCommandList* list, uint32_t slot, const EncodePass& pass) {
  if (!SlotInRange(slot, "RecordEncode")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + ENCODE_TABLE;
  WriteTable(base, {{pass.source, pass.source_view_format}, {}, ExposureView(pass.exposure)},
             {{pass.model}, {pass.motion, DXGI_FORMAT_R16G16_FLOAT}});
  DispatchCompute(list, encode_pipeline_.Get(), EncodeConstants(pass, 0u, pass.motion != nullptr), base,
                  (pass.canvas.Empty() ? pass.width : pass.canvas.width), (pass.canvas.Empty() ? pass.height : pass.canvas.height));
  return true;
}

bool ColorPipeline::RecordChange(ID3D12GraphicsCommandList* list, uint32_t slot, const ChangePass& pass) {
  if (!SlotInRange(slot, "RecordChange")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + CHANGE_TABLE;
  WriteTable(base, {{pass.encode.source, pass.encode.source_view_format}, {pass.nr_output}, ExposureView(pass.encode.exposure)},
             {{pass.change}, {pass.basis, DXGI_FORMAT_R16G16_FLOAT}});
  DispatchCompute(list, encode_pipeline_.Get(), EncodeConstants(pass.encode, 1u, pass.basis != nullptr), base, pass.encode.width,
                  pass.encode.height);
  return true;
}

bool ColorPipeline::RecordMotion(ID3D12GraphicsCommandList* list, uint32_t slot, const MotionPass& pass) {
  if (!SlotInRange(slot, "RecordMotion")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + MOTION_TABLE;
  WriteTable(base, {{}, {pass.source, pass.source_view_format}, ExposureView(nullptr)}, {{}, {pass.target, DXGI_FORMAT_R16G16_FLOAT}});
  const std::array<uint32_t, PASS_CONSTANTS> constants = {
      pass.region.x, pass.region.y, pass.region.width, pass.region.height, pass.image.width, pass.image.height,
      pass.canvas.width, pass.canvas.height, Bits(pass.scale_x), Bits(pass.scale_y),
      (pass.flip_y ? 1u : 0u), 0u, 0u, 0u, 0u, 0u,
  };
  DispatchCompute(list, motion_pipeline_.Get(), constants, base, pass.canvas.width, pass.canvas.height);
  return true;
}

bool ColorPipeline::RecordDecode(ID3D12GraphicsCommandList* list, uint32_t slot, const DecodePass& pass) {
  if (!SlotInRange(slot, "RecordDecode")) return false;
  const auto found = decode_pipelines_.find(pass.target_view_format);
  if (found == decode_pipelines_.end() || !found->second) return false;
  ID3D12PipelineState* const pipeline = found->second.Get();

  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + DECODE_TABLE;
  WriteTable(base, {{pass.source, pass.source_view_format}, {pass.nr_output}, ExposureView(pass.exposure),
                    {pass.mask, pass.mask_view_format}},
             {});
  D3D12_RENDER_TARGET_VIEW_DESC target_view = {};
  target_view.Format = pass.target_view_format;
  target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  // RTV descriptors are read when OMSetRenderTargets records, so one slot serves every frame.
  const D3D12_CPU_DESCRIPTOR_HANDLE target_handle = target_heap_->GetCPUDescriptorHandleForHeapStart();
  device_->CreateRenderTargetView(pass.target, &target_view, target_handle);
  const std::array<uint32_t, PASS_CONSTANTS> constants = {
      static_cast<uint32_t>(pass.encoding), (pass.target_view_srgb ? 1u : 0u), Bits(pass.input_scale),
      Bits(pass.transfer_strength), Bits(pass.color_strength), (pass.exposure != nullptr ? 1u : 0u),
      Bits(pass.exposure_factor), 0u, 0u, pass.width, pass.height,
      (pass.upsampling ? static_cast<uint32_t>(*pass.upsampling) + 1u : 0u), 0u, 0u,
      pass.options | (pass.mask != nullptr ? shader_options::MASK : 0u), Bits(pass.chroma_clamp),
  };
  const D3D12_VIEWPORT viewport = {.TopLeftX = 0.f, .TopLeftY = 0.f, .Width = static_cast<float>(pass.width),
                                   .Height = static_cast<float>(pass.height), .MinDepth = 0.f, .MaxDepth = 1.f};
  const D3D12_RECT scissor = {.left = 0, .top = 0, .right = static_cast<LONG>(pass.width), .bottom = static_cast<LONG>(pass.height)};
  ID3D12DescriptorHeap* const heaps[] = {shader_heap_.Get()};
  list->SetDescriptorHeaps(1u, heaps);
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetGraphicsRoot32BitConstants(0u, static_cast<UINT>(constants.size()), constants.data(), 0u);
  list->SetGraphicsRootDescriptorTable(1u, GpuDescriptor(base));
  list->SetPipelineState(pipeline);
  list->OMSetRenderTargets(1u, &target_handle, FALSE, nullptr);
  list->RSSetViewports(1u, &viewport);
  list->RSSetScissorRects(1u, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3u, 1u, 0u, 0u);
  return true;
}

bool ColorPipeline::RecordComputeDecode(ID3D12GraphicsCommandList* list, uint32_t slot, const ComputeDecodePass& pass) {
  if (!SlotInRange(slot, "RecordComputeDecode")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + DECODE_TABLE;
  WriteTable(base, {{pass.source, pass.source_view_format}, {pass.nr_output}, ExposureView(pass.exposure),
                    {pass.mask, pass.mask_view_format}},
             {{pass.target, pass.target_uav_format}});
  const std::array<uint32_t, PASS_CONSTANTS> constants = {
      static_cast<uint32_t>(pass.encoding), 0u, Bits(pass.input_scale), Bits(pass.transfer_strength),
      Bits(pass.color_strength), (pass.exposure != nullptr ? 1u : 0u), Bits(pass.exposure_factor), pass.origin_x,
      pass.origin_y, pass.width, pass.height, (pass.upsampling ? static_cast<uint32_t>(*pass.upsampling) + 1u : 0u),
      pass.source_x, pass.source_y, pass.options | (pass.mask != nullptr ? shader_options::MASK : 0u), Bits(pass.chroma_clamp),
  };
  DispatchCompute(list, compute_decode_pipeline_.Get(), constants, base, pass.width, pass.height);
  return true;
}

bool ColorPipeline::RecordMeter(ID3D12GraphicsCommandList* list, uint32_t slot, const MeterPass& pass) {
  if (!SlotInRange(slot, "RecordMeter")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + METER_TABLE;
  // Plan 17: t2 is the game's exposure, as the encode binds it (a null view reads 0, which GameExposure replaces with 1).
  WriteTable(base, {{pass.source, pass.source_view_format}, {}, ExposureView(pass.probe ? pass.game_exposure : nullptr)},
             {{pass.state, DXGI_FORMAT_R32G32B32A32_FLOAT}});
  const std::array<uint32_t, PASS_CONSTANTS> constants = {
      static_cast<uint32_t>(pass.encoding), pass.primaries, Bits(pass.input_scale), pass.region.x, pass.region.y,
      pass.region.width, pass.region.height, (pass.snap ? 1u : 0u), (pass.smooth ? 1u : 0u), Bits(pass.brighter_rate),
      Bits(pass.darker_rate), Bits(pass.frame_seconds), (pass.probe ? 1u : 0u), (pass.probe && pass.game_exposure != nullptr ? 1u : 0u),
      Bits(pass.game_exposure_factor), 0u,
  };
  DispatchCompute(list, meter_pipeline_.Get(), constants, base, 1u, 1u);  // one group of 256 threads
  return true;
}

bool ColorPipeline::RecordPyramid(ID3D12GraphicsCommandList* list, uint32_t slot, const PyramidPass& pass) {
  if (!SlotInRange(slot, "RecordPyramid")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + PYRAMID_TABLE;
  WriteTable(base, {{pass.change}}, {{pass.gauss}, {pass.peaks, DXGI_FORMAT_R16_FLOAT}});
  for (uint32_t level = 1u; level <= pass.atlas.levels; ++level) {
    if (level > 1u) {
      UavBarriers(list, {pass.gauss, pass.peaks});  // level reads level - 1
    }
    const nr::Size size = look::LevelSize(pass.atlas.image, level);
    const std::array<uint32_t, 4> constants = {pass.atlas.image.width, pass.atlas.image.height, level, 0u};
    DispatchCompute(list, pyramid_pipeline_.Get(), constants, base, size.width, size.height);
  }
  return true;
}

bool ColorPipeline::RecordStabilize(ID3D12GraphicsCommandList* list, uint32_t slot, const StabilizePass& pass) {
  if (!SlotInRange(slot, "RecordStabilize")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + STABILIZE_TABLE;
  WriteTable(base, {{pass.gauss}, {pass.previous}, {pass.motion.vectors.resource, MotionViewOf(pass.motion)}},
             {{pass.current}, {pass.state, DXGI_FORMAT_R32G32B32A32_FLOAT}});
  DispatchCompute(list, stabilize_pipeline_.Get(), StabilizeConstants(STABILIZE_CUT, pass), base, 1u, 1u);
  UavBarriers(list, {pass.state});
  const nr::Size size = look::LevelSize(pass.atlas.image, pass.bands.stable_level);
  DispatchCompute(list, stabilize_pipeline_.Get(), StabilizeConstants(STABILIZE_LEVELS, pass), base, size.width, size.height);
  return true;
}

bool ColorPipeline::RecordDetail(ID3D12GraphicsCommandList* list, uint32_t slot, const StabilizePass& pass) {
  if (!SlotInRange(slot, "RecordDetail")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + DETAIL_TABLE;
  WriteTable(base, {{pass.gauss}, {pass.previous}, {pass.motion.vectors.resource, MotionViewOf(pass.motion)}, {pass.change}},
             {{pass.current}, {pass.state, DXGI_FORMAT_R32G32B32A32_FLOAT}});
  DispatchCompute(list, stabilize_pipeline_.Get(), StabilizeConstants(STABILIZE_DETAIL, pass), base, pass.atlas.image.width,
                  pass.atlas.image.height);
  return true;
}

bool ColorPipeline::RecordShape(ID3D12GraphicsCommandList* list, uint32_t slot, const ShapePass& pass) {
  if (!SlotInRange(slot, "RecordShape")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + SHAPE_TABLE;
  WriteTable(base, {{pass.gauss}, {pass.peaks, DXGI_FORMAT_R16_FLOAT}, {pass.history}, {pass.basis, DXGI_FORMAT_R16G16_FLOAT},
                    {pass.detail}},
             {{pass.change}});
  const look::ShapeSettings& shape = pass.settings;
  const uint32_t flags = (pass.sdr ? 1u : 0u) | (pass.history != nullptr ? 2u : 0u) | (pass.detail != nullptr ? 4u : 0u);
  const std::array<uint32_t, 20> constants = {
      pass.atlas.image.width, pass.atlas.image.height, pass.atlas.levels, flags,
      Bits(pass.bands.low_level), pass.bands.peak_level, Bits(shape.tone), Bits(shape.detail),
      Bits(shape.halo), Bits(shape.brighten), Bits(shape.darken), Bits(shape.color),
      Bits(shape.hue), Bits(shape.shadows), Bits(shape.midtones), Bits(shape.highlights),
      Bits(shape.edit_strength), Bits(shape.max_brighten), Bits(shape.max_darken), Bits(shape.max_color),
  };
  DispatchCompute(list, shape_pipeline_.Get(), constants, base, pass.atlas.image.width, pass.atlas.image.height);
  return true;
}

bool ColorPipeline::RecordResolve(ID3D12GraphicsCommandList* list, uint32_t slot, const ResolveStrengthsPass& pass) {
  if (!SlotInRange(slot, "RecordResolve")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + (pass.swapped ? RESOLVE_SWAPPED_TABLE : RESOLVE_TABLE);
  WriteTable(base, {{pass.given}}, {{pass.returned}});
  const std::array<uint32_t, 4> constants = {pass.size.width, pass.size.height, Bits(pass.transfer_strength),
                                             Bits(pass.color_strength)};
  DispatchCompute(list, resolve_pipeline_.Get(), constants, base, pass.size.width, pass.size.height);
  return true;
}

bool ColorPipeline::RecordFacesPyramid(ID3D12GraphicsCommandList* list, uint32_t slot, const FacesPass& pass) {
  if (!SlotInRange(slot, "RecordFacesPyramid")) return false;
  const bool first = (pass.mask == nullptr);
  uint32_t table = FACES_LEVEL_TABLE;
  if (first) {
    table = FACES_LEVEL_FIRST_TABLE;
  } else if (pass.swapped) {
    table = FACES_LEVEL_SWAPPED_TABLE;
  }
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + table;
  // Round 5: pass 1 with the fill writes the fill's pyramid too (u1).
  const bool fill = (first && pass.parameters.fill);
  WriteTable(base, {{pass.changed}, {pass.reference}, {pass.mask}}, {{pass.atlas}, {(fill ? pass.fill : nullptr)}});
  const uint32_t flags = (first ? FACES_FLAG_FIRST : 0u) | (fill ? FACES_FLAG_FILL : 0u);
  for (uint32_t level = 1u; level <= pass.layout.levels; ++level) {
    if (level > 1u) {
      // Level reads level - 1.
      if (fill) {
        UavBarriers(list, {pass.atlas, pass.fill});
      } else {
        UavBarriers(list, {pass.atlas});
      }
    }
    const nr::Size size = look::LevelSize(pass.layout.image, level);
    DispatchCompute(list, faces_pipeline_.Get(), FacesConstants(pass.layout.image, pass.layout.levels, FACES_MODE_LEVEL, level, flags, pass.parameters), base,
                    size.width, size.height);
  }
  return true;
}

bool ColorPipeline::RecordFacesCombine(ID3D12GraphicsCommandList* list, uint32_t slot, const FacesPass& pass) {
  if (!SlotInRange(slot, "RecordFacesCombine")) return false;
  const bool first = (pass.mask == nullptr);
  uint32_t table = FACES_COMBINE_TABLE;
  if (first) {
    table = FACES_COMBINE_FIRST_TABLE;
  } else if (pass.swapped) {
    table = FACES_COMBINE_SWAPPED_TABLE;
  }
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + table;
  if (first) {
    WriteTable(base, {{pass.atlas}, {}, {}, {pass.detail}, {pass.fill}}, {{pass.changed}, {pass.reference}});
  } else {
    WriteTable(base, {{pass.atlas}, {pass.mask}, {pass.reference}, {pass.detail}}, {{pass.changed}});
  }
  DispatchCompute(list, faces_pipeline_.Get(),
                  FacesConstants(pass.layout.image, pass.layout.levels, (first ? FACES_MODE_FIRST : FACES_MODE_LATER), 0u,
                                 ((first && pass.parameters.fill) ? FACES_FLAG_FILL : 0u), pass.parameters),
                  base,
                  pass.layout.image.width, pass.layout.image.height);
  return true;
}

bool ColorPipeline::RecordFacesDespike(ID3D12GraphicsCommandList* list, uint32_t slot, const FacesPass& pass) {
  if (!SlotInRange(slot, "RecordFacesDespike")) return false;
  uint32_t table = FACES_DESPIKE_TABLE;
  if (pass.mask == nullptr) {
    table = FACES_DESPIKE_FIRST_TABLE;
  } else if (pass.swapped) {
    table = FACES_DESPIKE_SWAPPED_TABLE;
  }
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + table;
  WriteTable(base, {{pass.changed}, {pass.reference}}, {{pass.detail}});
  DispatchCompute(list, faces_pipeline_.Get(), FacesConstants(pass.layout.image, pass.layout.levels, FACES_MODE_DESPIKE, 0u, 0u, pass.parameters), base,
                  pass.layout.image.width, pass.layout.image.height);
  return true;
}

bool ColorPipeline::RecordFacesShow(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* target, ID3D12Resource* mask, nr::Size size) {
  if (!SlotInRange(slot, "RecordFacesShow")) return false;
  const uint32_t base = slot * DESCRIPTORS_PER_SLOT + FACES_SHOW_TABLE;
  WriteTable(base, {{}, {mask}}, {{target}});
  DispatchCompute(list, faces_pipeline_.Get(), FacesConstants(size, 1u, FACES_MODE_SHOW, 0u, 0u, {}), base, size.width, size.height);
  return true;
}

bool ColorPipeline::HasDecodePipeline(DXGI_FORMAT target_view_format) const {
  const auto found = decode_pipelines_.find(target_view_format);
  return found != decode_pipelines_.end() && found->second;
}

bool ColorPipeline::SupportsTypedUavStore(DXGI_FORMAT format) const {
  if (const auto found = typed_uav_store_.find(format); found != typed_uav_store_.end()) return found->second;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {.Format = format};
  const bool supported = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
                         && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
  typed_uav_store_.emplace(format, supported);
  return supported;
}

D3D12_CPU_DESCRIPTOR_HANDLE ColorPipeline::CpuDescriptor(uint32_t index) const {
  D3D12_CPU_DESCRIPTOR_HANDLE handle = shader_heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(index) * descriptor_size_;
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE ColorPipeline::GpuDescriptor(uint32_t index) const {
  D3D12_GPU_DESCRIPTOR_HANDLE handle = shader_heap_->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<UINT64>(index) * descriptor_size_;
  return handle;
}

void ColorPipeline::WriteShaderResource(uint32_t index, ID3D12Resource* resource, DXGI_FORMAT format) {
  D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
  view.Format = format;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = 1u;
  device_->CreateShaderResourceView(resource, &view, CpuDescriptor(index));
}

void ColorPipeline::WriteUnorderedAccess(uint32_t index, ID3D12Resource* resource, DXGI_FORMAT format) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
  view.Format = format;
  view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(resource, nullptr, &view, CpuDescriptor(index));
}

}  // namespace uplift::color
