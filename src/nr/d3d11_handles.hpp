#pragma once
// Plan 18 (design §2): the Direct3D 11 twin of nr/vk_handles.hpp. The NGX hooks, the registry and DlssFrame are typed for Direct3D 12; on Direct3D 11 they
// carry the evaluate's ID3D11DeviceContext as the command list and the game's ID3D11Resource pointers as the resources, untouched. These casts are the only
// place that pun is written; only the add-on's Direct3D 11 routing, the summary and the Direct3D 11 bridge turn a pointer back.
#include <d3d11.h>
#include <d3d12.h>

namespace uplift::nr {

[[nodiscard]] inline ID3D12GraphicsCommandList* AsList(ID3D11DeviceContext* context) { return reinterpret_cast<ID3D12GraphicsCommandList*>(context); }
[[nodiscard]] inline ID3D11DeviceContext* D3D11ContextOf(ID3D12GraphicsCommandList* list) { return reinterpret_cast<ID3D11DeviceContext*>(list); }
[[nodiscard]] inline ID3D12Resource* AsResource(ID3D11Resource* resource) { return reinterpret_cast<ID3D12Resource*>(resource); }
[[nodiscard]] inline ID3D11Resource* D3D11ResourceOf(ID3D12Resource* resource) { return reinterpret_cast<ID3D11Resource*>(resource); }

}  // namespace uplift::nr
