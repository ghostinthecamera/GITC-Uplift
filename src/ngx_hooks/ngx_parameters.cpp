#include "ngx_hooks/ngx_parameters.hpp"

#include <cstdint>
#include <limits>

namespace uplift::ngx_hooks {

std::optional<uint32_t> ReadUint(const NVSDK_NGX_Parameter& parameters, const char* key) {
  unsigned int value = 0u;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &value))) return value;
  int signed_value = 0;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &signed_value))) {
    if (signed_value < 0) return std::nullopt;
    return static_cast<uint32_t>(signed_value);
  }
  unsigned long long wide_value = 0u;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &wide_value)) && wide_value <= std::numeric_limits<uint32_t>::max()) {
    return static_cast<uint32_t>(wide_value);
  }
  return std::nullopt;
}

std::optional<int32_t> ReadInt(const NVSDK_NGX_Parameter& parameters, const char* key) {
  int value = 0;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &value))) return value;
  unsigned int unsigned_value = 0u;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &unsigned_value))
      && unsigned_value <= static_cast<unsigned int>(std::numeric_limits<int32_t>::max())) {
    return static_cast<int32_t>(unsigned_value);
  }
  return std::nullopt;
}

std::optional<float> ReadFloat(const NVSDK_NGX_Parameter& parameters, const char* key) {
  float value = 0.f;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &value))) return value;
  double wide_value = 0.0;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &wide_value))) return static_cast<float>(wide_value);
  return std::nullopt;
}

ID3D12Resource* ReadResource(const NVSDK_NGX_Parameter& parameters, const char* key) {
  ID3D12Resource* resource = nullptr;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &resource))) return resource;
  void* pointer = nullptr;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &pointer))) return static_cast<ID3D12Resource*>(pointer);
  return nullptr;
}

ID3D11Resource* ReadD3D11Resource(const NVSDK_NGX_Parameter& parameters, const char* key) {
  ID3D11Resource* resource = nullptr;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &resource))) return resource;
  void* pointer = nullptr;
  if (NVSDK_NGX_SUCCEED(parameters.Get(key, &pointer))) return static_cast<ID3D11Resource*>(pointer);
  return nullptr;
}

}  // namespace uplift::ngx_hooks
