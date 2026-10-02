#pragma once

namespace uplift::nr::keys {

struct Subrect {
  const char* base_x;
  const char* base_y;
  const char* width;
  const char* height;
};

struct Resource {
  const char* resource;
  Subrect subrect;
};

inline constexpr char CREATION_NODE_MASK[] = "CreationNodeMask";
inline constexpr char VISIBILITY_NODE_MASK[] = "VisibilityNodeMask";
inline constexpr char WIDTH[] = "DLSSNR.Width";
inline constexpr char HEIGHT[] = "DLSSNR.Height";
inline constexpr char PRESET[] = "DLSSNR.Hint.Render.Preset";
inline constexpr char SCALING_RATIO[] = "DLSSNR.ScalingRatio";
inline constexpr char PERF_QUALITY_VALUE[] = "PerfQualityValue";

inline constexpr Resource COLOR = {"DLSSNR.Color", {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight"}};
inline constexpr Resource OUTPUT = {"DLSSNR.Output", {"DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY", "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight"}};
inline constexpr Resource MVEC = {"DLSSNR.MVec", {"DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight"}};
inline constexpr Resource CONTROL_MASK = {"DLSSNR.ControlMask", {"DLSSNR.ControlMaskSubrectBaseX", "DLSSNR.ControlMaskSubrectBaseY", "DLSSNR.ControlMaskSubrectWidth", "DLSSNR.ControlMaskSubrectHeight"}};
inline constexpr Resource BACKBUFFER = {"DLSSNR.Backbuffer", {"DLSSNR.BackbufferSubrectBaseX", "DLSSNR.BackbufferSubrectBaseY", "DLSSNR.BackbufferSubrectWidth", "DLSSNR.BackbufferSubrectHeight"}};
inline constexpr Resource UI = {"DLSSNR.UI", {"DLSSNR.UISubrectBaseX", "DLSSNR.UISubrectBaseY", "DLSSNR.UISubrectWidth", "DLSSNR.UISubrectHeight"}};
inline constexpr Resource UI_ALPHA = {"DLSSNR.UIAlpha", {"DLSSNR.UIAlphaSubrectBaseX", "DLSSNR.UIAlphaSubrectBaseY", "DLSSNR.UIAlphaSubrectWidth", "DLSSNR.UIAlphaSubrectHeight"}};

inline constexpr char MVEC_SCALE_X[] = "DLSSNR.MVecScaleX";
inline constexpr char MVEC_SCALE_Y[] = "DLSSNR.MVecScaleY";
inline constexpr char INTENSITY[] = "DLSSNR.Intensity";
inline constexpr char LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
inline constexpr char LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
inline constexpr char GLOBAL_TONE[] = "DLSSNR.GlobalToneStrength";
inline constexpr char USE_AUTO_MASK[] = "DLSSNR.UseAutoMask";
inline constexpr char SKIN_STRUCTURE[] = "DLSSNR.SkinStructureStrength";
inline constexpr char STYLE[] = "DLSSNR.Style";
inline constexpr char RESET[] = "DLSSNR.Reset";
inline constexpr char ENABLED[] = "DLSSNR.Enabled";
inline constexpr char UI_CORRECTION[] = "DLSSNR.UICorrection";
inline constexpr char DEPTH_INVERTED[] = "DLSSNR.DepthInverted";

}  // namespace uplift::nr::keys
