#include "addon/d3d11_identity.hpp"

#include <wrl/client.h>

namespace uplift::addon {

void MarkD3D11Device(ID3D11Device* device, const void* owner) {
  if (device == nullptr) return;
  if (owner == nullptr) {
    device->SetPrivateData(UPLIFT_RESHADE_DEVICE_GUID, 0u, nullptr);
    return;
  }
  device->SetPrivateData(UPLIFT_RESHADE_DEVICE_GUID, sizeof(owner), &owner);
}

const void* D3D11DeviceMark(ID3D11Device* device) {
  const void* owner = nullptr;
  UINT size = sizeof(owner);
  if (device == nullptr || FAILED(device->GetPrivateData(UPLIFT_RESHADE_DEVICE_GUID, &size, &owner)) || size != sizeof(owner)) return nullptr;
  return owner;
}

const void* D3D11ContextOwner(ID3D11DeviceContext* context, bool* deferred) {
  *deferred = false;
  if (context == nullptr) return nullptr;
  if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
    *deferred = true;  // NGX's Direct3D 11 backend refuses them; the call passes through untouched
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  context->GetDevice(&device);
  return D3D11DeviceMark(device.Get());
}

}  // namespace uplift::addon
