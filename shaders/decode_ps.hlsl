#include "decode_common.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  float4 result = DecodePixel(uint2(position.xy));
  // An sRGB render-target view encodes on write, so hand it linear values.
  if (target_srgb != 0u) {
    result.rgb = SrgbDecode(saturate(result.rgb));
  }
  return result;
}
