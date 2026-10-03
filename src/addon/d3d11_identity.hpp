#pragma once

#include <d3d11.h>
#include <guiddef.h>

namespace uplift::addon {

// Plan 15: private data on a Direct3D 12 device native NR runs on, holding its ReShade device (api::device*). ReShade's proxy forwards Get/SetPrivateData to
// the device itself, so the pointer the game hands NGX (the proxy) and the native one read the same mark. A bridge's private device never carries one.
// Plan 18: and on a game's native Direct3D 11 device, set when its bridge's context is made and removed at destroy_device (ReShade's Direct3D 11 proxy
// forwards Get/SetPrivateData too).
inline constexpr GUID UPLIFT_RESHADE_DEVICE_GUID = {0x6b1d4c2e, 0x8f3a, 0x4e57, {0xa1, 0x9c, 0x5d, 0x02, 0x7e, 0x3b, 0x94, 0xf1}};

// Plan 18 (design §2): marks `device` (a game's Direct3D 11 device, native or ReShade's proxy) as `owner`'s: its ReShade device. A null `owner` removes the mark.
void MarkD3D11Device(ID3D11Device* device, const void* owner);
// The owner marked on `device`, or null (unmarked, or no device).
[[nodiscard]] const void* D3D11DeviceMark(ID3D11Device* device);
// The owner marked on the device `context` belongs to, or null: an unmarked device (another device's DLSS, a mod's own), a null context, or a deferred
// context (`*deferred` says which: NGX's Direct3D 11 backend refuses deferred contexts, so the call passes through untouched). Lock-free.
[[nodiscard]] const void* D3D11ContextOwner(ID3D11DeviceContext* context, bool* deferred);

}  // namespace uplift::addon
