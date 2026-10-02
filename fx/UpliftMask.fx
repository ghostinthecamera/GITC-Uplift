// Example NR mask for Uplift (v2 design §3.13). Uplift reads a texture named UPLIFT_MASK from any effect: its red
// channel limits NR per pixel (0 keeps the game's image, 1 is full NR, 0.5 half of NR's change). This example masks
// NR off left of a split line; replace UpliftMaskPS with any mask you need. Uplift uses the previous frame's mask.

uniform float Split <
  ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
  ui_label = "Split"; ui_tooltip = "NR is masked off left of this line.";
> = 0.5;

uniform float Feather <
  ui_type = "slider"; ui_min = 0.001; ui_max = 0.25;
  ui_label = "Feather";
> = 0.02;

texture UPLIFT_MASK { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };

void UpliftMaskVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD) {
  texcoord = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
  position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float UpliftMaskPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target {
  return smoothstep(Split - Feather, Split + Feather, texcoord.x);
}

technique UpliftMaskExample <
  ui_label = "Uplift mask (example)";
  ui_tooltip = "Writes UPLIFT_MASK: NR runs right of the split only.";
>
{
  pass
  {
    VertexShader = UpliftMaskVS;
    PixelShader = UpliftMaskPS;
    RenderTarget = UPLIFT_MASK;
  }
}
