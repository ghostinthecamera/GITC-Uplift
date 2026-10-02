// Fullscreen triangle: vertices (-1, 1), (3, 1), (-1, -3).
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
  const float2 corner = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
  return float4(corner * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
}
