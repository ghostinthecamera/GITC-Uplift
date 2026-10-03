#pragma once

#include <cstdint>
#include <optional>

#include "nr/ngx.hpp"

namespace uplift::ngx_hooks {

// Typed reads of a game's NGX parameter block. Each tries the type NGX's helper macros set the key
// with first (unsigned int for sizes and subrect bases, int for flags, quality and Reset, float for
// scales, D3D12 resources), then the other numeric types, because engines differ. Never throws and
// never dereferences a resource.
[[nodiscard]] std::optional<uint32_t> ReadUint(const NVSDK_NGX_Parameter& parameters, const char* key);
[[nodiscard]] std::optional<int32_t> ReadInt(const NVSDK_NGX_Parameter& parameters, const char* key);
[[nodiscard]] std::optional<float> ReadFloat(const NVSDK_NGX_Parameter& parameters, const char* key);
[[nodiscard]] ID3D12Resource* ReadResource(const NVSDK_NGX_Parameter& parameters, const char* key);
// Plan 18 (design §2): ReadResource for a Direct3D 11 block, whose resources NGX_D3D11_EVALUATE_DLSS_EXT sets as ID3D11Resource*: the ID3D11Resource type
// first, then a void* (some engines set that), else null. Never dereferences the pointer.
[[nodiscard]] ID3D11Resource* ReadD3D11Resource(const NVSDK_NGX_Parameter& parameters, const char* key);

}  // namespace uplift::ngx_hooks
