// Uplift marker technique. Put this file in one of ReShade's effect search paths,
// enable "Uplift" and drag it to where DLSS-NR should run: techniques above it see the game's
// image, techniques below it see NR's output. Without it, Uplift runs NR before every effect.
//
// The two motion checkboxes permit the add-on to use a provider when its technique is enabled above Uplift.
// Setup chooses the motion source. The add-on compiles the matching pass so unused provider textures and Launchpad's IPC request can be released.

#ifndef UPLIFT_USE_LAUNCHPAD
  #define UPLIFT_USE_LAUNCHPAD 1
#endif
#ifndef UPLIFT_USE_LUMENITE
  #define UPLIFT_USE_LUMENITE 1
  #define UPLIFT_DETECT_KERNEL
#endif

#if __RENDERER__ >= 0x9000

uniform bool UpliftAllowLaunchpad <
  ui_type = "checkbox";
  ui_label = "Allow Launchpad motion";
  ui_tooltip = "Use iMMERSE Launchpad when selected in Uplift Setup. Enable its technique above Uplift.";
> = true;
#if __RENDERER__ >= 0xa000
uniform bool UpliftAllowLumenite <
  ui_type = "checkbox";
  ui_label = "Allow Lumenite motion";
  ui_tooltip = "Use Lumenite Kernel when selected in Uplift Setup. Enable its technique above Uplift.";
> = true;
#endif

#define UPLIFT_LAUNCHPAD 0
#if __RENDERER__ >= 0x9000
  #if UPLIFT_USE_LAUNCHPAD && __RESHADE__ >= 60000
    #if exists "MartysMods/mmx_deferred.fxh"
      #include "MartysMods/mmx_deferred.fxh"
      #undef UPLIFT_LAUNCHPAD
      #define UPLIFT_LAUNCHPAD 1
    #elif exists "iMMERSE/MartysMods/mmx_deferred.fxh"
      #include "iMMERSE/MartysMods/mmx_deferred.fxh"
      #undef UPLIFT_LAUNCHPAD
      #define UPLIFT_LAUNCHPAD 1
    #endif
  #endif
#endif

// Lumenite: only without LaunchPad's, from Direct3D 10 on (the nine-cell upsample below is not checked on shader model 3).
#define UPLIFT_LUMENITE 0
#if !UPLIFT_LAUNCHPAD && UPLIFT_USE_LUMENITE && __RENDERER__ >= 0xa000
  // A renamed Kernel is also supported: the add-on recognizes its technique and explicitly links this pass.
  #if !defined(UPLIFT_DETECT_KERNEL)
    #undef UPLIFT_LUMENITE
    #define UPLIFT_LUMENITE 1
  #elif __RESHADE__ >= 60000
    #if exists "lumenite_Kernel.fx"
      #undef UPLIFT_LUMENITE
      #define UPLIFT_LUMENITE 1
    #endif
  #endif
#endif

#if UPLIFT_LAUNCHPAD || UPLIFT_LUMENITE
texture UPLIFT_MV { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };

void UpliftMotionVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD) {
  texcoord = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
  position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#endif

#if UPLIFT_LAUNCHPAD
// LaunchPad's motion is a UV offset from this frame to the previous one; NR and Uplift's stabiliser take pixels.
float2 UpliftMotionPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target {
  if (!UpliftAllowLaunchpad) return 0.0;
  return Deferred::get_motion(texcoord) * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
}
#endif

#if UPLIFT_LUMENITE
// Kernel's two shared textures, declared exactly as Lumenite's own effects import them, so ReShade gives this file the same ones. Kernel writes them; this
// file only reads them.
namespace Kernel {
  texture2D tFlow { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = RG16F; };
  texture2D tConfidence { Width = BUFFER_WIDTH/8; Height = BUFFER_HEIGHT/8; Format = R16F; };
}
sampler2D UpliftKernelFlow { Texture = Kernel::tFlow; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D UpliftKernelConfidence { Texture = Kernel::tConfidence; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
texture UpliftBackBufferTex : COLOR;
sampler UpliftBackBuffer { Texture = UpliftBackBufferTex; AddressU = CLAMP; AddressV = CLAMP; };

// Kernel's grid: one cell per 8x8 pixels (its textures' own size).
static const float2 UPLIFT_FLOW_CELLS = float2(BUFFER_WIDTH / 8, BUFFER_HEIGHT / 8);
// Confidence: a cell at or below LOW contributes no motion, at or above HIGH all of it, smoothly between. A cell Kernel is unsure of is taken as still:
// NR then keeps that pixel's history where it is and rejects it on its own where the image changed, which wrong motion would not let it do.
#define UPLIFT_LUMENITE_CONFIDENCE_LOW 0.25
#define UPLIFT_LUMENITE_CONFIDENCE_HIGH 0.6
// Edge-aware weighting: a cell counts less the more its luma (at its centre) differs from this pixel's, relative to the brighter of the two, so a bright
// edge and an HDR image weigh alike. At EDGE the weight is exp(-0.5); SPREAD is the spatial reach, in cells.
#define UPLIFT_LUMENITE_EDGE 0.12
#define UPLIFT_LUMENITE_SPREAD 0.75
// 1.2.1: a thin feature (a 1-3 px outline, hair or wire over a brighter area) differs from all nine cell centres, so every edge weight is near zero and the
// edge-aware result would be near zero motion: it smeared under NR in a pan. When the cells alike to this pixel carry less than TRUST of the distance
// weight, the result blends toward the distance-only one (the surroundings' motion), linearly down to it at zero. A pixel with any cell alike to it keeps
// the edge-aware result untouched, so edges stay sharp. A floor on each edge weight was the alternative; it leaks the far side's motion into a pixel whose
// only alike cell is a distant one, wherever the floor outweighs that cell, so the blend's switch on how much of the neighbourhood agrees is the safer one.
#define UPLIFT_LUMENITE_EDGE_TRUST 0.05
// 1.2.1: luma is clamped both ways, so an infinite scRGB pixel cannot turn the weights, and so the vector, into NaN (Inf - Inf).
#define UPLIFT_LUMENITE_LUMA_MAX 1e4

float UpliftLuma(float3 color) { return clamp(dot(color, float3(0.2126, 0.7152, 0.0722)), -UPLIFT_LUMENITE_LUMA_MAX, UPLIFT_LUMENITE_LUMA_MAX); }

// Kernel's flow is a UV offset from this frame to the previous one, as LaunchPad's (Lumenite's own effects fetch the previous frame at uv + flow). It
// comes to full size through a joint bilateral upsample over the 3x3 cells around this pixel: each cell's flow, weighted by its distance and by how alike
// its colour is to this pixel's, so a pixel takes the motion of the side of an edge it is on instead of an 8-pixel block, and (1.2.1) a pixel alike to none
// of them takes its surroundings' motion (EDGE_TRUST). Each cell's confidence scales its flow but not its weight, so unsure cells pull the result toward
// zero motion. Then to pixels, as LaunchPad's.
float2 UpliftLumeniteMotionPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target {
  if (!UpliftAllowLumenite) return 0.0;
  float luma = UpliftLuma(tex2Dlod(UpliftBackBuffer, float4(texcoord, 0.0, 0.0)).rgb);
  float2 cell = texcoord * UPLIFT_FLOW_CELLS - 0.5;  // this pixel in cell-centre coordinates
  float2 nearest = floor(cell + 0.5);
  float2 flow = 0.0;      // edge-aware: distance x likeness
  float weights = 0.0;
  float2 near_flow = 0.0;  // distance only (1.2.1: the thin-feature fallback)
  float near_weights = 0.0;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      float2 index = clamp(nearest + float2(x, y), 0.0, UPLIFT_FLOW_CELLS - 1.0);
      float2 uv = (index + 0.5) / UPLIFT_FLOW_CELLS;
      float2 delta = cell - index;
      float spatial = exp(-dot(delta, delta) / (2.0 * UPLIFT_LUMENITE_SPREAD * UPLIFT_LUMENITE_SPREAD));
      float guide = UpliftLuma(tex2Dlod(UpliftBackBuffer, float4(uv, 0.0, 0.0)).rgb);
      float difference = (luma - guide) / (max(max(luma, guide), 0.0) + 0.05);
      float range = exp(-(difference * difference) / (2.0 * UPLIFT_LUMENITE_EDGE * UPLIFT_LUMENITE_EDGE));
      float weight = spatial * range;
      float confidence = smoothstep(UPLIFT_LUMENITE_CONFIDENCE_LOW, UPLIFT_LUMENITE_CONFIDENCE_HIGH, tex2Dlod(UpliftKernelConfidence, float4(uv, 0.0, 0.0)).x);
      float2 cell_flow = confidence * tex2Dlod(UpliftKernelFlow, float4(uv, 0.0, 0.0)).xy;
      flow += weight * cell_flow;
      weights += weight;
      near_flow += spatial * cell_flow;
      near_weights += spatial;
    }
  }
  float trust = saturate(weights / (UPLIFT_LUMENITE_EDGE_TRUST * near_weights));
  float2 edge_aware = flow / max(weights, 1e-30);  // bounded: a weighted mean of the cells' flows (0 when every weight underflowed, where trust is 0)
  return lerp(near_flow / near_weights, edge_aware, trust) * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
}
#endif

#if __RENDERER__ >= 0xb000
void UpliftMarkerCS(uint3 id : SV_DispatchThreadID) {}
#else
texture UpliftMarkerTarget { Width = 1; Height = 1; Format = R8; };
float4 UpliftMarkerVS(uint id : SV_VertexID) : SV_Position { return float4(0.0, 0.0, 0.0, 1.0); }
float UpliftMarkerPS(float4 position : SV_Position) : SV_Target { return 0.0; }
#endif

technique Uplift <
  ui_label = "Uplift (DLSS-NR position)";
  ui_tooltip = "Place this technique where Present passes should run among your effects. Enable a motion provider above it and choose the source in Uplift Setup.";
#if UPLIFT_LAUNCHPAD
  uplift_motion_source = 1;
#elif UPLIFT_LUMENITE
  uplift_motion_source = 2;
#else
  uplift_motion_source = 0;
#endif
>
{
#if UPLIFT_LAUNCHPAD
  // Newer LaunchPads compute optical flow only when an effect asks for it; older ones always do and have no such request.
  #ifdef IPC_REQUEST_FEATURE
  IPC_REQUEST_FEATURE(MARTYSMODS_IPC_FEATURE_OPTICALFLOW)
  #endif
  pass Motion
  {
    VertexShader = UpliftMotionVS;
    PixelShader = UpliftMotionPS;
    RenderTarget = UPLIFT_MV;
  }
#endif
#if UPLIFT_LUMENITE
  pass Motion
  {
    VertexShader = UpliftMotionVS;
    PixelShader = UpliftLumeniteMotionPS;
    RenderTarget = UPLIFT_MV;
  }
#endif
  pass Marker
  {
#if __RENDERER__ >= 0xb000
    ComputeShader = UpliftMarkerCS<1, 1>;
    DispatchSizeX = 1;
    DispatchSizeY = 1;
#else
    PrimitiveTopology = POINTLIST;
    VertexCount = 1;
    VertexShader = UpliftMarkerVS;
    PixelShader = UpliftMarkerPS;
    RenderTarget = UpliftMarkerTarget;
#endif
  }
}

#endif
