// Uplift marker technique (spec §8.1). Put this file in one of ReShade's effect search paths,
// enable "Uplift" and drag it to where DLSS-NR should run: techniques above it see the game's
// image, techniques below it see NR's output. Without it, Uplift runs NR before every effect.
//
// With iMMERSE LaunchPad installed (Plan 6, v2 design §3.20), this technique can also ask LaunchPad for its optical
// flow and write it to UPLIFT_MV (back-buffer pixels, current -> previous) right before Uplift runs NR here. Put
// Uplift below LaunchPad. It includes LaunchPad's own header from your install; nothing of LaunchPad's ships with Uplift.
//
// Automatic: Uplift sets UPLIFT_USE_LAUNCHPAD itself, through the add-on API, whenever Motion vectors is LaunchPad
// or Auto and MartysMods_Launchpad is enabled; no preprocessor edit is needed. It stays 0 otherwise, because
// including LaunchPad's header creates LaunchPad's textures (about 200 MB of VRAM at 4K) and requests optical flow
// every frame, whether NR is on or not. Without Uplift's add-on loaded, UPLIFT_USE_LAUNCHPAD can still be set by
// hand in this effect's preprocessor definitions.
//
// Plan 8: Direct3D 10 games too. ReShade's D3D10 backend has no compute shaders, so there the marker pass draws one
// point into a 1x1 texture (as LaunchPad's own IPC passes do); D3D11 and D3D12 keep the compute marker.
//
// Plan 9: 32-bit Direct3D 9 games too, with the same one-point marker.
//
// Plan 10: LaunchPad's optical flow on Direct3D 9 too (shader model 3, through ReShade's emulation); the gate below is __RENDERER__ >= 0x9000.
// LaunchPad was checked to compile on ReShade's Direct3D 9 backend, so the gate stays. Only if it fails on some setup, set it back to
// 0xa000 together with LAUNCHPAD_ON_D3D9 in src/addon/launchpad_link.hpp.
//
// Plan 11: Vulkan too, with the compute marker (ReShade's SPIR-V backend reports __RENDERER__ 0x20000, so the compute pass below applies, and so does
// the LaunchPad block). There is no shader change for Vulkan.
//
// Plan 12: OpenGL too, with the compute marker (ReShade's GLSL backend reports __RENDERER__ 0x14xxx for GL 4.6, so the compute pass below applies). There is no shader change for OpenGL.

#ifndef UPLIFT_USE_LAUNCHPAD
  #define UPLIFT_USE_LAUNCHPAD 0
#endif

#if __RENDERER__ >= 0x9000

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

#if UPLIFT_LAUNCHPAD
texture UPLIFT_MV { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };

void UpliftMotionVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD) {
  texcoord = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
  position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// LaunchPad's motion is a UV offset from this frame to the previous one; NR and Uplift's stabiliser take pixels.
float2 UpliftMotionPS(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target {
  return Deferred::get_motion(texcoord) * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
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
  ui_tooltip = "Marks where Uplift runs DLSS-NR among your effects. With iMMERSE LaunchPad above it and Motion "
               "vectors set to LaunchPad or Auto, Uplift links itself to LaunchPad automatically and hands its "
               "motion vectors here; no preprocessor edit needed.";
>
{
#if UPLIFT_LAUNCHPAD
  IPC_REQUEST_FEATURE(MARTYSMODS_IPC_FEATURE_OPTICALFLOW)
  pass Motion
  {
    VertexShader = UpliftMotionVS;
    PixelShader = UpliftMotionPS;
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
