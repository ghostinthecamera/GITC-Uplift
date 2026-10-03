#include "ui/settings_schema.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <type_traits>
#include <utility>
#include <vector>

namespace uplift::ui {
namespace {

constexpr std::array<std::string_view, 3> MODELS = {"Model A", "Model B", "Model C"};
constexpr std::array<std::string_view, 3> SOURCES = {"Auto", "After DLSS", "Present"};
constexpr std::array<std::string_view, 4> MOTION_VECTORS = {"Auto", "DLSS", "Launchpad", "None"};
constexpr std::array<std::string_view, 7> QUALITY_MODES = {"Game", "Performance", "Balanced", "Quality",
                                                           "Ultra Performance", "Ultra Quality", "DLAA"};
constexpr std::array<std::string_view, 5> PRESETS = {"Game", "J", "K", "L", "M"};
constexpr std::array<std::string_view, 3> AUTO_EXPOSURE = {"Game", "Off", "On"};
constexpr std::array<std::string_view, 6> ENCODINGS = {"Auto", "Linear BT.709", "sRGB", "BT.2100 PQ", "scRGB", "scRGB-nl"};
constexpr std::array<std::string_view, 3> DEPTH_DIRECTIONS = {"Game", "Normal", "Inverted"};
constexpr std::array<std::string_view, 2> STATE_RESTORE_MODES = {"Full", "Minimal"};
constexpr std::array<std::string_view, 4> LOG_LEVELS = {"Error", "Warning", "Info", "Debug"};
constexpr std::array<std::string_view, 2> NGX_HOOK_MODES = {"Auto", "Off (safe mode)"};
constexpr std::array<std::string_view, 2> FOREIGN_NR_MODES = {"Yield", "Observe"};
constexpr std::array<std::string_view, 6> RESOLUTION_MODES = {"Full", "Quality (67 %)", "Balanced (58 %)",
                                                              "Performance (50 %)", "Match game", "Custom"};
constexpr std::array<std::string_view, 2> UPSAMPLING_MODES = {"Classic", "Edge-aware"};
constexpr std::array<std::string_view, 2> MASK_MODES = {"Auto (UPLIFT_MASK)", "Off"};
constexpr std::array<std::string_view, 3> UI_CORRECTION_MODES = {"Auto", "Off", "On"};
constexpr std::array<std::string_view, 3> STABILIZE_MODES = {"Off", "Static", "Motion"};
constexpr std::array<std::string_view, 4> INPUT_EXPOSURES = {"Auto", "Game", "Metered", "Manual"};
constexpr std::array<std::string_view, 2> ADAPT_MODES = {"Off", "Smooth"};
constexpr std::array<std::string_view, 4> PRIMARIES = {"Auto", "BT.709", "BT.2020", "AP1 (ACEScg)"};
constexpr std::array<std::string_view, 2> NEURAL_TRANSFERS = {"Bounded ratio", "Consistent"};
constexpr std::array<std::string_view, 3> PEDESTAL_MODES = {"Off", "Auto", "Always"};

using setting_flags::APPLY_ON_RELEASE;
using setting_flags::BOOKKEEPING;
using setting_flags::GREYED;
using setting_flags::NEXT_DLSS_CREATE;
using setting_flags::NEXT_START;
using setting_flags::PASS_SWITCH;
using setting_flags::RESTARTS_HISTORY;

template <class Enum>
double Index(Enum value) {
  return static_cast<double>(static_cast<uint32_t>(value));
}

template <class Enum>
Enum FromIndex(double value) {
  return static_cast<Enum>(static_cast<uint32_t>(value));
}

double Flag(bool value) {
  return (value ? 1.0 : 0.0);
}

constexpr std::string_view PRESET_TOOLTIP =
    "The DLSS render preset for this quality mode (J-M). Applies when the game next creates its DLSS feature.";

const auto BASE_ROWS = std::to_array<SettingDescriptor>({
    {
        .key = ENABLED_KEY,
        .kind = SettingKind::BOOL,
        .section = SettingSection::TOP,
        .label = "Enable",
        .tooltip = "Runs DLSS-NR. Switching it off returns all of NR's video memory after the grace period.",
        .get = [](const Settings& s) { return Flag(s.enabled); },
        .set = [](Settings& s, double v) { s.enabled = (v != 0.0); },
    },
    {
        .key = "EnableKey",
        .kind = SettingKind::KEY,
        .section = SettingSection::HOTKEYS,
        .label = "Enable hotkey",
        .tooltip = "Toggles Enable. Click, then press a key; right-click clears it.",
        .min = 0.0,
        .max = 255.0,
        .get = [](const Settings& s) { return static_cast<double>(s.enable_key); },
        .set = [](Settings& s, double v) { s.enable_key = static_cast<uint32_t>(v); },
    },
    {
        .key = "ResolutionMode",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::PLACEMENT,
        .label = "Resolution",
        .tooltip = "The size NR works at. Lower sizes cost less GPU time and video memory, and NR's change is upsampled. "
                   "Match game follows the game's DLSS render size. NR never works below 1280x720. A change applies "
                   "after half a second and restarts NR's history.",
        .choices = RESOLUTION_MODES,
        .flags = RESTARTS_HISTORY,
        .get = [](const Settings& s) { return Index(s.resolution_mode); },
        .set = [](Settings& s, double v) { s.resolution_mode = FromIndex<ResolutionMode>(v); },
    },
    {
        .key = "Intensity",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LOOK,
        .label = "Intensity",
        .tooltip = "How strongly NR changes the image. Ctrl+click to type a value above 1.",
        .min = 0.0,
        .max = 10.0,
        .ui_max = 1.0,
        .get = [](const Settings& s) { return static_cast<double>(s.intensity); },
        .set = [](Settings& s, double v) { s.intensity = static_cast<float>(v); },
    },
    {
        .key = "Style",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::LOOK,
        .label = "Model",
        .tooltip = "The NR model. A change restarts NR's temporal history.",
        .choices = MODELS,
        .flags = RESTARTS_HISTORY,
        .get = [](const Settings& s) { return static_cast<double>(s.style); },
        .set = [](Settings& s, double v) { s.style = static_cast<uint32_t>(v); },
    },
    {
        .key = "LocalStructure",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LOOK,
        .label = "Structure",
        .tooltip = "Fine structure NR adds. A change restarts NR's temporal history.",
        .min = 0.0,
        .max = 1.0,
        .flags = RESTARTS_HISTORY,
        .get = [](const Settings& s) { return static_cast<double>(s.local_structure); },
        .set = [](Settings& s, double v) { s.local_structure = static_cast<float>(v); },
    },
    {
        .key = "LocalTone",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LOOK,
        .label = "Local tone",
        .tooltip = "Local contrast NR adds. A change restarts NR's temporal history.",
        .min = 0.0,
        .max = 1.0,
        .flags = RESTARTS_HISTORY,
        .get = [](const Settings& s) { return static_cast<double>(s.local_tone); },
        .set = [](Settings& s, double v) { s.local_tone = static_cast<float>(v); },
    },
    {
        .key = "AutoMask",
        .kind = SettingKind::BOOL,
        .section = SettingSection::LOOK,
        .label = "Character mask",
        .tooltip = "Protects characters with NR's own mask. A change restarts NR's temporal history.",
        .flags = RESTARTS_HISTORY,
        .get = [](const Settings& s) { return Flag(s.auto_mask); },
        .set = [](Settings& s, double v) { s.auto_mask = (v != 0.0); },
    },
    {
        .key = "SkinStructure",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LOOK,
        .label = "Skin structure",
        .tooltip = "Structure on skin; -1 follows Structure. A change restarts NR's temporal history.",
        .min = 0.0,
        .max = 1.0,
        .special = static_cast<double>(SKIN_SAME_AS_STRUCTURE),
        .flags = RESTARTS_HISTORY,
        .get = [](const Settings& s) { return static_cast<double>(s.skin_structure); },
        .set = [](Settings& s, double v) { s.skin_structure = static_cast<float>(v); },
    },
    {
        .key = "Mask",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::MASK,
        .label = "NR mask",
        .tooltip = "Auto: an effect that writes a texture named UPLIFT_MASK limits NR by its red channel: 0 keeps the "
                   "game's image, 1 is full NR, 0.5 half of NR's change. It is the previous frame's mask. Uplift ships "
                   "fx/UpliftMask.fx as an example.",
        .choices = MASK_MODES,
        .get = [](const Settings& s) { return Index(s.mask); },
        .set = [](Settings& s, double v) { s.mask = FromIndex<MaskMode>(v); },
    },
    {
        .key = "UICorrection",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::MASK,
        .label = "UI correction",
        .tooltip = "NR's UI correction needs the HUD-less image, which Uplift does not capture: after DLSS it is not "
                   "needed (the HUD is drawn after NR); on the presented image use the NR mask. On forwards the "
                   "runtime's switch only.",
        .choices = UI_CORRECTION_MODES,
        .get = [](const Settings& s) { return Index(s.ui_correction); },
        .set = [](Settings& s, double v) { s.ui_correction = FromIndex<UiCorrection>(v); },
    },
    {
        .key = "ShapeResult",
        .kind = SettingKind::BOOL,
        .section = SettingSection::RESULT,
        .label = "Shape result",
        .tooltip = "Reshapes NR's result live, without restarting its history. Off leaves NR's output exactly as it is.",
        .get = [](const Settings& s) { return Flag(s.look.enabled); },
        .set = [](Settings& s, double v) { s.look.enabled = (v != 0.0); },
    },
    {
        .key = "EditStrength",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Edit strength",
        .tooltip = "0 removes NR's change, 1 keeps it, 2 doubles it, in stops and in colour.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.edit_strength); },
        .set = [](Settings& s, double v) { s.look.shape.edit_strength = static_cast<float>(v); },
    },
    {
        .key = "Brighten",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Brightening",
        .tooltip = "How much of NR's brightening is kept.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.brighten); },
        .set = [](Settings& s, double v) { s.look.shape.brighten = static_cast<float>(v); },
    },
    {
        .key = "Darken",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Darkening",
        .tooltip = "How much of NR's darkening is kept, including the dark halos it can leave around bright objects.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.darken); },
        .set = [](Settings& s, double v) { s.look.shape.darken = static_cast<float>(v); },
    },
    {
        .key = "ResultColor",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Colour",
        .tooltip = "How far NR moves colours toward or away from grey; 0 keeps the game's saturation.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.color); },
        .set = [](Settings& s, double v) { s.look.shape.color = static_cast<float>(v); },
    },
    {
        .key = "HueShift",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Hue shift",
        .tooltip = "How far NR turns hues; 0 keeps the game's hues.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.hue); },
        .set = [](Settings& s, double v) { s.look.shape.hue = static_cast<float>(v); },
    },
    {
        .key = "Shadows",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Shadows",
        .tooltip = "Scales NR's change where the game's image is dark.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.shadows); },
        .set = [](Settings& s, double v) { s.look.shape.shadows = static_cast<float>(v); },
    },
    {
        .key = "Midtones",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Midtones",
        .tooltip = "Scales NR's change in the game's midtones.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.midtones); },
        .set = [](Settings& s, double v) { s.look.shape.midtones = static_cast<float>(v); },
    },
    {
        .key = "Highlights",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::RESULT,
        .label = "Highlights",
        .tooltip = "Scales NR's change where the game's image is bright.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.highlights); },
        .set = [](Settings& s, double v) { s.look.shape.highlights = static_cast<float>(v); },
    },
    {
        .key = "MaxBrighten",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Max brightening (stops)",
        .tooltip = "A soft cap on how far a pixel is brightened; 0 is no cap.",
        .min = 0.0,
        .max = 3.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.max_brighten); },
        .set = [](Settings& s, double v) { s.look.shape.max_brighten = static_cast<float>(v); },
    },
    {
        .key = "MaxDarken",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Max darkening (stops)",
        .tooltip = "A soft cap on how far a pixel is darkened; 0 is no cap.",
        .min = 0.0,
        .max = 3.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.max_darken); },
        .set = [](Settings& s, double v) { s.look.shape.max_darken = static_cast<float>(v); },
    },
    {
        .key = "MaxColorChange",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Max colour change",
        .tooltip = "A soft cap on colour movement, in normalised chroma; 0 is no cap.",
        .min = 0.0,
        .max = 0.5,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.max_color); },
        .set = [](Settings& s, double v) { s.look.shape.max_color = static_cast<float>(v); },
    },
    {
        .key = "LargeScaleTone",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Large-scale tone",
        .tooltip = "Scales NR's broad lighting change.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.tone); },
        .set = [](Settings& s, double v) { s.look.shape.tone = static_cast<float>(v); },
    },
    {
        .key = "FineDetail",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Fine detail",
        .tooltip = "Scales NR's small-scale change.",
        .min = 0.0,
        .max = 2.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.detail); },
        .set = [](Settings& s, double v) { s.look.shape.detail = static_cast<float>(v); },
    },
    {
        .key = "DetailRadius",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Detail radius (% of height)",
        .tooltip = "Where broad change ends and fine detail begins, as a share of the image height; also how far halo "
                   "suppression reaches. A change restarts the stabiliser.",
        .min = 0.25,
        .max = 10.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.detail_radius); },
        .set = [](Settings& s, double v) { s.look.detail_radius = static_cast<float>(v); },
    },
    {
        .key = "HaloSuppression",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Halo suppression",
        .tooltip = "Reduces the dark rings NR leaves next to bright objects. Even darkening across a surface, and "
                   "shadows beside dark objects, are left alone.",
        .min = 0.0,
        .max = 1.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.shape.halo); },
        .set = [](Settings& s, double v) { s.look.shape.halo = static_cast<float>(v); },
    },
    {
        .key = "Stabilize",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::LIMITS,
        .label = "Stabilise",
        .tooltip = "Smooths NR's broad change over time. Static suits still views; Motion follows the motion vectors "
                   "and, without them, drops history where the image changes. Both add a little lag to lighting changes.",
        .choices = STABILIZE_MODES,
        .get = [](const Settings& s) { return Index(s.look.stabilize); },
        .set = [](Settings& s, double v) { s.look.stabilize = FromIndex<look::StabilizeMode>(v); },
    },
    {
        .key = "StabilizeMs",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::LIMITS,
        .label = "Stabilise time (ms)",
        .tooltip = "Longer is steadier but slower to follow lighting.",
        .min = 16.0,
        .max = 2000.0,
        .get = [](const Settings& s) { return static_cast<double>(s.look.stabilize_ms); },
        .set = [](Settings& s, double v) { s.look.stabilize_ms = static_cast<float>(v); },
    },
    {
        .key = "StabilizeDetail",
        .kind = SettingKind::BOOL,
        .section = SettingSection::LIMITS,
        .label = "Stabilise detail",
        .tooltip = "Also smooths NR's small-scale change.",
        .get = [](const Settings& s) { return Flag(s.look.stabilize_detail); },
        .set = [](Settings& s, double v) { s.look.stabilize_detail = (v != 0.0); },
    },
    {
        .key = "Source",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::PLACEMENT,
        .label = "NR stage",
        .tooltip = "Where NR runs. Auto: right after the game's DLSS once the game uses DLSS, else on the presented frame. "
                   "After DLSS needs ReShade 6.1 or newer. A change applies at once.",
        .choices = SOURCES,
        .get = [](const Settings& s) { return Index(s.source); },
        .set = [](Settings& s, double v) { s.source = FromIndex<PlacementSource>(v); },
    },
    {
        .key = "PreUpscale",
        .kind = SettingKind::BOOL,
        .section = SettingSection::PLACEMENT,
        .label = "Before upscaling (pre-SR)",
        .tooltip = "NR enhances the game's render-resolution image, and the game's DLSS-SR upscales the result. Ray "
                   "Reconstruction keeps NR after DLSS. A render below 1280x720 is padded, so NR never runs below that size.",
        .get = [](const Settings& s) { return Flag(s.pre_upscale); },
        .set = [](Settings& s, double v) { s.pre_upscale = (v != 0.0); },
    },
    {
        .key = "ResolutionScale",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::PLACEMENT,
        .label = "Custom scale (%)",
        .tooltip = "The work size for Resolution = Custom, in percent of the output. Applies when the slider is released.",
        .min = 25.0,
        .max = 100.0,
        .get = [](const Settings& s) { return static_cast<double>(s.resolution_scale); },
        .set = [](Settings& s, double v) { s.resolution_scale = static_cast<float>(v); },
    },
    {
        .key = "Upsampling",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::PLACEMENT,
        .label = "Upsampling",
        .tooltip = "How NR's change returns to full size below Full. Edge-aware keeps it on the side of an edge where NR "
                   "made it; Classic is bilinear and slightly cheaper.",
        .choices = UPSAMPLING_MODES,
        .get = [](const Settings& s) { return Index(s.upsampling); },
        .set = [](Settings& s, double v) { s.upsampling = FromIndex<color::Upsampling>(v); },
    },
    {
        .key = "MotionVectors",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::PLACEMENT,
        .label = "Motion vectors",
        .tooltip = "Inside the game's frame (NR stage After DLSS or Before upscaling), Auto and DLSS use the game's own DLSS motion "
                   "vectors, and None uses none. On the presented frame (NR stage Present), Auto and DLSS use copies of them where "
                   "Uplift can make them; otherwise NR runs there without them. Launchpad uses iMMERSE Launchpad's motion on the "
                   "presented image, through Uplift.fx's Uplift technique placed below Launchpad.",
        .choices = MOTION_VECTORS,
        .get = [](const Settings& s) { return Index(s.motion_vectors); },
        .set = [](Settings& s, double v) { s.motion_vectors = FromIndex<MotionVectorSource>(v); },
    },
    {
        .key = "PassCount",
        .kind = SettingKind::UINT,
        .section = SettingSection::LOOK,
        .label = "Pass count",
        .tooltip = "Each extra pass runs NR again on the previous pass's output, at the full cost of one more pass. "
                   "Applies when the slider is released.",
        .min = 1.0,
        .max = 10.0,
        .flags = APPLY_ON_RELEASE,
        .get = [](const Settings& s) { return static_cast<double>(s.pass_count); },
        .set = [](Settings& s, double v) { s.pass_count = static_cast<uint32_t>(v); },
    },
    // User decision (fix round 1): the user does not want to change DLSS modes or presets from
    // Uplift -- it needs a DLSS re-create, and live switching is not practical -- so these 8 rows are
    // hidden. The keys stay readable from the ini and the code stays inert at the GAME defaults.
    {
        .key = "DLSSQualityMode",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "DLSS quality mode",
        .tooltip = "Overrides the quality mode the game creates DLSS-SR with. Applies when the game next creates its DLSS "
                   "feature; a restart or a resolution change may be needed.",
        .choices = QUALITY_MODES,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_quality_mode); },
        .set = [](Settings& s, double v) { s.dlss_quality_mode = FromIndex<DlssQualityOverride>(v); },
    },
    {
        .key = "DLSSPresetDLAA",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "DLAA preset",
        .tooltip = PRESET_TOOLTIP,
        .choices = PRESETS,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_presets[0]); },
        .set = [](Settings& s, double v) { s.dlss_presets[0] = FromIndex<DlssPresetOverride>(v); },
    },
    {
        .key = "DLSSPresetUltraQuality",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "Ultra Quality preset",
        .tooltip = PRESET_TOOLTIP,
        .choices = PRESETS,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_presets[1]); },
        .set = [](Settings& s, double v) { s.dlss_presets[1] = FromIndex<DlssPresetOverride>(v); },
    },
    {
        .key = "DLSSPresetQuality",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "Quality preset",
        .tooltip = PRESET_TOOLTIP,
        .choices = PRESETS,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_presets[2]); },
        .set = [](Settings& s, double v) { s.dlss_presets[2] = FromIndex<DlssPresetOverride>(v); },
    },
    {
        .key = "DLSSPresetBalanced",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "Balanced preset",
        .tooltip = PRESET_TOOLTIP,
        .choices = PRESETS,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_presets[3]); },
        .set = [](Settings& s, double v) { s.dlss_presets[3] = FromIndex<DlssPresetOverride>(v); },
    },
    {
        .key = "DLSSPresetPerformance",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "Performance preset",
        .tooltip = PRESET_TOOLTIP,
        .choices = PRESETS,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_presets[4]); },
        .set = [](Settings& s, double v) { s.dlss_presets[4] = FromIndex<DlssPresetOverride>(v); },
    },
    {
        .key = "DLSSPresetUltraPerformance",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "Ultra Performance preset",
        .tooltip = PRESET_TOOLTIP,
        .choices = PRESETS,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_presets[5]); },
        .set = [](Settings& s, double v) { s.dlss_presets[5] = FromIndex<DlssPresetOverride>(v); },
    },
    {
        .key = "DLSSAutoExposure",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "DLSS auto-exposure",
        .tooltip = "Forces DLSS-SR's auto-exposure on or off. Applies when the game next creates its DLSS feature.",
        .choices = AUTO_EXPOSURE,
        .flags = NEXT_DLSS_CREATE,
        .get = [](const Settings& s) { return Index(s.dlss_auto_exposure); },
        .set = [](Settings& s, double v) { s.dlss_auto_exposure = FromIndex<AutoExposureOverride>(v); },
    },
    {
        .key = "PresentWithFrameGen",
        .kind = SettingKind::BOOL,
        .section = SettingSection::ADVANCED,
        .label = "Present with frame generation",
        .tooltip = "Lets the Present placement run NR while frame generation is on: on every presented frame, generated "
                   "ones included, at twice the cost or more. Its result on generated frames is not guaranteed.",
        .get = [](const Settings& s) { return Flag(s.present_with_frame_gen); },
        .set = [](Settings& s, double v) { s.present_with_frame_gen = (v != 0.0); },
    },
    {
        .key = "Encoding",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::FIXES_COLOR,
        .label = "Encoding",
        .tooltip = "How the image is encoded. Auto follows the swap chain; After DLSS reads DLSS's HDR flag.",
        .choices = ENCODINGS,
        .get = [](const Settings& s) { return Index(s.encoding); },
        .set = [](Settings& s, double v) { s.encoding = FromIndex<color::Encoding>(v); },
    },
    {
        .key = "DiffuseWhiteNits",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_COLOR,
        .label = "Diffuse white (nits)",
        .tooltip = "The brightness NR treats as paper white. Higher gives NR a darker input with more headroom.",
        .min = 48.0,
        .max = 500.0,
        .special = 0.0,
        .get = [](const Settings& s) { return static_cast<double>(s.diffuse_white_nits); },
        .set = [](Settings& s, double v) { s.diffuse_white_nits = static_cast<float>(v); },
    },
    {
        .key = "TransferStrength",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_COLOR,
        .label = "Transfer strength",
        .tooltip = "How much of NR's brightness change reaches the image.",
        .min = 0.0,
        .max = 1.0,
        .get = [](const Settings& s) { return static_cast<double>(s.transfer_strength); },
        .set = [](Settings& s, double v) { s.transfer_strength = static_cast<float>(v); },
    },
    {
        .key = "ColorStrength",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_COLOR,
        .label = "Colour strength",
        .tooltip = "How much of NR's colour change reaches the image.",
        .min = 0.0,
        .max = 1.0,
        .get = [](const Settings& s) { return static_cast<double>(s.color_strength); },
        .set = [](Settings& s, double v) { s.color_strength = static_cast<float>(v); },
    },
    {
        .key = "InputExposure",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::FIXES_COLOR,
        .label = "Input exposure",
        .tooltip = "Where NR's input brightness comes from on scene-linear images. Auto: the game's DLSS exposure while it "
                   "agrees with Uplift's meter, else the meter (Details says which). Game: DLSS's exposure (none on the "
                   "presented image). Metered: Uplift's own meter. Manual: Diffuse white alone. sRGB, scRGB and HDR10 "
                   "images are never metered.",
        .choices = INPUT_EXPOSURES,
        .get = [](const Settings& s) { return Index(s.input_exposure); },
        .set = [](Settings& s, double v) { s.input_exposure = FromIndex<color::InputExposure>(v); },
    },
    {
        .key = "ExposureAdapt",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::FIXES_COLOR,
        .label = "Adaptation",
        .tooltip = "When the meter is used (Metered, or Auto). Smooth adapts at the rates below, with a small dead band; Off follows every frame.",
        .choices = ADAPT_MODES,
        .get = [](const Settings& s) { return Index(s.exposure_adapt); },
        .set = [](Settings& s, double v) { s.exposure_adapt = FromIndex<ExposureAdapt>(v); },
    },
    {
        .key = "AdaptBrighterStops",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_COLOR,
        .label = "Adapt to brighter scenes (stops/s)",
        .tooltip = "When the meter is used (Metered, or Auto).",
        .min = 0.1,
        .max = 20.0,
        .get = [](const Settings& s) { return static_cast<double>(s.adapt_brighter_stops); },
        .set = [](Settings& s, double v) { s.adapt_brighter_stops = static_cast<float>(v); },
    },
    {
        .key = "AdaptDarkerStops",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_COLOR,
        .label = "Adapt to darker scenes (stops/s)",
        .tooltip = "When the meter is used (Metered, or Auto).",
        .min = 0.1,
        .max = 20.0,
        .get = [](const Settings& s) { return static_cast<double>(s.adapt_darker_stops); },
        .set = [](Settings& s, double v) { s.adapt_darker_stops = static_cast<float>(v); },
    },
    {
        .key = "LinearUnitNits",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_COLOR,
        .label = "Linear unit (nits)",
        .tooltip = "Nits per 1.0 of a linear or scRGB image; 0 is automatic (100 for linear BT.709, 80 for scRGB). The "
                   "reset arrow returns to automatic.",
        .min = 1.0,
        .max = 10000.0,
        .special = 0.0,
        .get = [](const Settings& s) { return static_cast<double>(s.linear_unit_nits); },
        .set = [](Settings& s, double v) { s.linear_unit_nits = static_cast<float>(v); },
    },
    {
        .key = "SourcePrimaries",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::FIXES_COLOR,
        .label = "Primaries",
        .tooltip = "The image's colour primaries, converted to BT.709 for NR and back. Auto is BT.709, and BT.2020 for "
                   "HDR10.",
        .choices = PRIMARIES,
        .get = [](const Settings& s) { return Index(s.source_primaries); },
        .set = [](Settings& s, double v) { s.source_primaries = FromIndex<color::Primaries>(v); },
    },
    {
        .key = "NeuralTransfer",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::FIXES_COLOR,
        .label = "Neural transfer",
        .tooltip = "How NR's edit returns to HDR. Consistent undoes the proxy curve's compression in highlights, up to "
                   "4x.",
        .choices = NEURAL_TRANSFERS,
        .get = [](const Settings& s) { return Index(s.neural_transfer); },
        .set = [](Settings& s, double v) { s.neural_transfer = FromIndex<color::NeuralTransfer>(v); },
    },
    {
        .key = "ChromaClamp",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_COLOR,
        .label = "Chroma clamp (stops)",
        .tooltip = "Holds each colour channel's change within this many stops of the brightness change; 0 is off.",
        .min = 0.0,
        .max = 3.0,
        .get = [](const Settings& s) { return static_cast<double>(s.chroma_clamp); },
        .set = [](Settings& s, double v) { s.chroma_clamp = static_cast<float>(v); },
    },
    {
        .key = "NearBlackGuard",
        .kind = SettingKind::BOOL,
        .section = SettingSection::FIXES_COLOR,
        .label = "Near-black colour guard",
        .tooltip = "Fades NR's colour change where the image is near black, so noise does not turn into green or grey "
                   "specks.",
        .get = [](const Settings& s) { return Flag(s.near_black_guard); },
        .set = [](Settings& s, double v) { s.near_black_guard = (v != 0.0); },
    },
    {
        .key = "PedestalRemoval",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "Dark pedestal removal",
        .tooltip = "Not needed so far: Uplift's HDR transfer applies NR's change as a ratio, so black stays black. It "
                   "is checked in game before anything is built.",
        .choices = PEDESTAL_MODES,
        .flags = GREYED,
        .get = [](const Settings& s) { return Index(s.pedestal_removal); },
        .set = [](Settings& s, double v) { s.pedestal_removal = FromIndex<PedestalRemoval>(v); },
    },
    {
        .key = "MotionScaleX",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_GUIDES,
        .label = "Motion scale X",
        .tooltip = "Multiplies the horizontal motion NR is given; a negative value flips the axis.",
        .min = -4.0,
        .max = 4.0,
        .get = [](const Settings& s) { return static_cast<double>(s.motion_scale_x); },
        .set = [](Settings& s, double v) { s.motion_scale_x = static_cast<float>(v); },
    },
    {
        .key = "MotionScaleY",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::FIXES_GUIDES,
        .label = "Motion scale Y",
        .tooltip = "Multiplies the vertical motion NR is given; a negative value flips the axis.",
        .min = -4.0,
        .max = 4.0,
        .get = [](const Settings& s) { return static_cast<double>(s.motion_scale_y); },
        .set = [](Settings& s, double v) { s.motion_scale_y = static_cast<float>(v); },
    },
    {
        .key = "DepthDirection",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::HIDDEN,
        .label = "Depth direction (no effect in NR 310.8)",
        .tooltip = "Forwarded as DLSSNR.DepthInverted. NR 310.8 reads depth but never uses it.",
        .choices = DEPTH_DIRECTIONS,
        .flags = GREYED,
        .get = [](const Settings& s) { return Index(s.depth_direction); },
        .set = [](Settings& s, double v) { s.depth_direction = FromIndex<DepthDirection>(v); },
    },
    {
        .key = "ChainedHistory",
        .kind = SettingKind::BOOL,
        .section = SettingSection::FIXES_GUIDES,
        .label = "Chained history",
        .tooltip = "Off resets the history of passes 2 and later every frame (a diagnostic).",
        .get = [](const Settings& s) { return Flag(s.chained_history); },
        .set = [](Settings& s, double v) { s.chained_history = (v != 0.0); },
    },
    {
        .key = "StateRestore",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::FIXES_GUIDES,
        .label = "Game state after NR",
        .tooltip = "After NR runs inside the game's frame. Full replays the game's compute state; Minimal restores only "
                   "the descriptor heaps and root signature.",
        .choices = STATE_RESTORE_MODES,
        .get = [](const Settings& s) { return Index(s.state_restore); },
        .set = [](Settings& s, double v) { s.state_restore = FromIndex<StateRestore>(v); },
    },
    {
        .key = "GlobalTone",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::HIDDEN,
        .label = "Global Tone Intensity",
        .tooltip = "Stored and forwarded; NR 310.8 never reads it.",
        .min = 0.0,
        .max = 1.0,
        .flags = GREYED,
        .get = [](const Settings& s) { return static_cast<double>(s.global_tone); },
        .set = [](Settings& s, double v) { s.global_tone = static_cast<float>(v); },
    },
    {
        .key = "Preset",
        .kind = SettingKind::UINT,
        .section = SettingSection::HIDDEN,
        .label = "Preset",
        .tooltip = "Stored and forwarded; no effect in NR 310.8.",
        .min = 0.0,
        .max = 15.0,
        .flags = GREYED,
        .get = [](const Settings& s) { return static_cast<double>(s.preset); },
        .set = [](Settings& s, double v) { s.preset = static_cast<uint32_t>(v); },
    },
    {
        .key = "Performance",
        .kind = SettingKind::UINT,
        .section = SettingSection::HIDDEN,
        .label = "Performance",
        .tooltip = "Stored and forwarded; no effect in NR 310.8.",
        .min = 1.0,
        .max = 6.0,
        .flags = GREYED,
        .get = [](const Settings& s) { return static_cast<double>(s.performance); },
        .set = [](Settings& s, double v) { s.performance = static_cast<uint32_t>(v); },
    },
    {
        .key = "GraceSeconds",
        .kind = SettingKind::FLOAT,
        .section = SettingSection::ADVANCED,
        .label = "Grace (seconds)",
        .tooltip = "How long NR keeps its memory after being switched off, so a quick toggle does not reload it.",
        .min = 0.0,
        .max = 60.0,
        .get = [](const Settings& s) { return static_cast<double>(s.grace_seconds); },
        .set = [](Settings& s, double v) { s.grace_seconds = static_cast<float>(v); },
    },
    {
        .key = "BudgetMarginMB",
        .kind = SettingKind::UINT,
        .section = SettingSection::ADVANCED,
        .label = "VRAM margin (MiB, 0 = automatic)",
        .tooltip = "Video memory NR always leaves free for the game.",
        .min = 0.0,
        .max = 65536.0,
        .get = [](const Settings& s) { return static_cast<double>(s.budget_margin_mb); },
        .set = [](Settings& s, double v) { s.budget_margin_mb = static_cast<uint32_t>(v); },
    },
    {
        .key = "AutoResume",
        .kind = SettingKind::BOOL,
        .section = SettingSection::ADVANCED,
        .label = "Resume when VRAM frees up",
        .tooltip = "After NR suspended itself because the game needed video memory.",
        .get = [](const Settings& s) { return Flag(s.auto_resume); },
        .set = [](Settings& s, double v) { s.auto_resume = (v != 0.0); },
    },
    {
        .key = "ShowNvIndicator",
        .kind = SettingKind::BOOL,
        .section = SettingSection::ADVANCED,
        .label = "Show the NVIDIA indicator (from the next load)",
        .tooltip = "NVIDIA's on-screen NR indicator.",
        .get = [](const Settings& s) { return Flag(s.show_nv_indicator); },
        .set = [](Settings& s, double v) { s.show_nv_indicator = (v != 0.0); },
    },
    {
        .key = "LogLevel",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::ADVANCED,
        .label = "Log level",
        .tooltip = "How much Uplift writes to ReShade.log.",
        .choices = LOG_LEVELS,
        .get = [](const Settings& s) { return Index(s.log_level); },
        .set = [](Settings& s, double v) { s.log_level = FromIndex<nr::LogLevel>(v); },
    },
    {
        .key = "SnippetPath",
        .kind = SettingKind::TEXT,
        .section = SettingSection::ADVANCED,
        .label = "Runtime path (from the next game start)",
        .tooltip = "nvngx_dlssnr.dll. Empty looks next to gitc-uplift.addon64, then next to the game.",
        .flags = NEXT_START,
        .get_text = [](const Settings& s) { return std::string_view(s.snippet_path); },
        .set_text = [](Settings& s, std::string_view v) { s.snippet_path = std::string(v); },
    },
    {
        .key = "NgxHooks",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::ADVANCED,
        .label = "NGX hooks (from the next game start)",
        .tooltip = "Off is safe mode: nothing is hooked, and only the Present placement exists.",
        .choices = NGX_HOOK_MODES,
        .flags = NEXT_START,
        .get = [](const Settings& s) { return Index(s.ngx_hooks); },
        .set = [](Settings& s, double v) { s.ngx_hooks = FromIndex<NgxHooksMode>(v); },
    },
    {
        .key = "ForeignNr",
        .kind = SettingKind::CHOICE,
        .section = SettingSection::ADVANCED,
        .label = "When another NR producer runs",
        .tooltip = "Yield stands down while another tool runs NR in this game; Observe keeps running.",
        .choices = FOREIGN_NR_MODES,
        .get = [](const Settings& s) { return Index(s.foreign_nr); },
        .set = [](Settings& s, double v) { s.foreign_nr = FromIndex<ForeignNrMode>(v); },
    },
    {
        .key = "AutoRetry",
        .kind = SettingKind::BOOL,
        .section = SettingSection::ADVANCED,
        .label = "Retry automatically after a failure",
        .tooltip = "After NR fails it retries after 1, 2, 4, 8, 16 and 30 s, then waits for a settings change or Retry now.",
        .get = [](const Settings& s) { return Flag(s.auto_retry); },
        .set = [](Settings& s, double v) { s.auto_retry = (v != 0.0); },
    },
    {
        .key = "UseD3D9Ex",
        .kind = SettingKind::BOOL,
        .section = SettingSection::ADVANCED,
        .label = "Use Direct3D 9Ex (faster; restart the game)",
        .tooltip = "Asks ReShade to create the game's Direct3D 9 device as 9Ex, so frames reach NR on the GPU. Some games "
                   "misbehave on 9Ex: if this one does not start, Uplift turns this off by itself.",
        .flags = NEXT_START,
        .get = [](const Settings& s) { return Flag(s.use_d3d9ex); },
        .set = [](Settings& s, double v) { s.use_d3d9ex = (v != 0.0); },
    },
    {
        .key = "AdjustVulkanDevices",
        .kind = SettingKind::BOOL,
        .section = SettingSection::HIDDEN,
        .label = "Adjust Vulkan devices when the game creates them (restart the game)",
        .tooltip = "1 adds the sharing extensions NR needs to the game's Vulkan device at creation; 0 leaves the device as the game asked "
                   "and NR runs CPU-ordered. Uplift sets 0 itself when the last start with 1 never reached its first frame.",
        .flags = NEXT_START,
        .get = [](const Settings& s) { return Flag(s.adjust_vulkan_devices); },
        .set = [](Settings& s, double v) { s.adjust_vulkan_devices = (v != 0.0); },
    },
    {
        // Plan 17: diagnostic only, never in the panel. It exercises Retry now and the 2-strike rule without a real hang.
        .key = "DiagnosticRemoveDevice",
        .kind = SettingKind::UINT,
        .section = SettingSection::HIDDEN,
        .label = "Diagnostic: remove Uplift's private device after this many frames",
        .tooltip = "Diagnostic only. N > 0 removes Uplift's own private Direct3D 12 device (ID3D12Device5::RemoveDevice; never the game's device, so "
                   "Direct3D 12 games are untouched) once NR has run N frames on it, and presses Retry now itself about a second after each stop "
                   "that may still be retried. 0 (the default) is off.",
        .min = 0.0,
        .max = 1000000.0,
        .get = [](const Settings& s) { return static_cast<double>(s.diagnostic_remove_device); },
        .set = [](Settings& s, double v) { s.diagnostic_remove_device = static_cast<uint32_t>(v); },
    },
    {
        .key = "DlssPlacementBlocked",
        .kind = SettingKind::BOOL,
        .section = SettingSection::HIDDEN,
        .label = "DLSS placement blocked after a device removal",
        .flags = BOOKKEEPING,
        .get = [](const Settings& s) { return Flag(s.dlss_placement_blocked); },
        .set = [](Settings& s, double v) { s.dlss_placement_blocked = (v != 0.0); },
    },
    {
        .key = "ConfigVersion",
        .kind = SettingKind::UINT,
        .section = SettingSection::HIDDEN,
        .label = "Settings version",
        .min = 1.0,
        .max = 65535.0,
        .flags = BOOKKEEPING,
        .get = [](const Settings& /*s*/) { return static_cast<double>(CONFIG_VERSION); },
        .set = [](Settings& s, double v) { s.config_version = static_cast<uint32_t>(v); },
    },
});

// Plan 5 (v2 design §3.10, P4-P13): pass n = Index + 2's own PassSettings field, through Settings::passes[Index].
// `get`/`set` must stay plain function pointers (SettingDescriptor::get/set), so Index and Member are template
// parameters rather than captured state.
template <size_t Index, auto Member>
double PassGet(const Settings& s) {
  using Field = std::remove_cvref_t<decltype(std::declval<PassSettings&>().*Member)>;
  if constexpr (std::is_same_v<Field, bool>) {
    return Flag(s.passes[Index].*Member);
  } else {
    return static_cast<double>(s.passes[Index].*Member);
  }
}

template <size_t Index, auto Member>
void PassSet(Settings& s, double v) {
  using Field = std::remove_cvref_t<decltype(std::declval<PassSettings&>().*Member)>;
  if constexpr (std::is_same_v<Field, bool>) {
    s.passes[Index].*Member = (v != 0.0);
  } else {
    s.passes[Index].*Member = static_cast<Field>(v);
  }
}

constexpr size_t PASS_ROW_COUNT = 9u;
constexpr size_t LATER_PASS_COUNT = MAX_PASSES - 1u;

// The 81 per-pass keys ("Pass2FollowPass1".."Pass10ColorStrength"), built once: every per-pass
// SettingDescriptor::key points into this array, which must outlive them (function-local static).
const std::array<std::string, LATER_PASS_COUNT * PASS_ROW_COUNT>& PassKeyStrings() {
  static const std::array<std::string, LATER_PASS_COUNT * PASS_ROW_COUNT> keys = [] {
    constexpr std::array<std::string_view, PASS_ROW_COUNT> SUFFIXES = {
        "FollowPass1", "Intensity", "Style", "LocalTone", "LocalStructure", "SkinStructure", "AutoMask",
        "TransferStrength", "ColorStrength",
    };
    std::array<std::string, LATER_PASS_COUNT * PASS_ROW_COUNT> result;
    for (size_t index = 0u; index < LATER_PASS_COUNT; ++index) {
      for (size_t field = 0u; field < PASS_ROW_COUNT; ++field) {
        result[index * PASS_ROW_COUNT + field] = std::format("Pass{}{}", index + 2u, SUFFIXES[field]);
      }
    }
    return result;
  }();
  return keys;
}

// Pass n = Index + 2's nine rows, in the order settings.cpp's SaveSettings and the overlay's Pass tree expect.
template <size_t Index>
std::array<SettingDescriptor, PASS_ROW_COUNT> PassRows() {
  constexpr auto PASS = static_cast<uint8_t>(Index + 2u);
  const std::array<std::string, LATER_PASS_COUNT * PASS_ROW_COUNT>& keys = PassKeyStrings();
  const auto key = [&](size_t field) { return std::string_view(keys[Index * PASS_ROW_COUNT + field]); };
  return std::to_array<SettingDescriptor>({
      {
          .key = key(0),
          .kind = SettingKind::BOOL,
          .section = SettingSection::PASSES,
          .label = "Same as pass 1",
          .tooltip = "Off gives this pass its own settings below.",
          .flags = PASS_SWITCH,
          .get = &PassGet<Index, &PassSettings::follow_pass1>,
          .set = &PassSet<Index, &PassSettings::follow_pass1>,
          .pass = PASS,
      },
      {
          .key = key(1),
          .kind = SettingKind::FLOAT,
          .section = SettingSection::PASSES,
          .label = "Intensity",
          .tooltip = "How strongly this pass changes the image. Ctrl+click to type a value above 1.",
          .min = 0.0,
          .max = 10.0,
          .ui_max = 1.0,
          .get = &PassGet<Index, &PassSettings::intensity>,
          .set = &PassSet<Index, &PassSettings::intensity>,
          .pass = PASS,
      },
      {
          .key = key(2),
          .kind = SettingKind::CHOICE,
          .section = SettingSection::PASSES,
          .label = "Model",
          .tooltip = "This pass's NR model. A change restarts its history.",
          .choices = MODELS,
          .flags = RESTARTS_HISTORY,
          .get = &PassGet<Index, &PassSettings::style>,
          .set = &PassSet<Index, &PassSettings::style>,
          .pass = PASS,
      },
      {
          .key = key(3),
          .kind = SettingKind::FLOAT,
          .section = SettingSection::PASSES,
          .label = "Local tone",
          .tooltip = "Local contrast this pass adds. A change restarts its history.",
          .min = 0.0,
          .max = 1.0,
          .flags = RESTARTS_HISTORY,
          .get = &PassGet<Index, &PassSettings::local_tone>,
          .set = &PassSet<Index, &PassSettings::local_tone>,
          .pass = PASS,
      },
      {
          .key = key(4),
          .kind = SettingKind::FLOAT,
          .section = SettingSection::PASSES,
          .label = "Structure",
          .tooltip = "Fine structure this pass adds. A change restarts its history.",
          .min = 0.0,
          .max = 1.0,
          .flags = RESTARTS_HISTORY,
          .get = &PassGet<Index, &PassSettings::local_structure>,
          .set = &PassSet<Index, &PassSettings::local_structure>,
          .pass = PASS,
      },
      {
          .key = key(5),
          .kind = SettingKind::FLOAT,
          .section = SettingSection::PASSES,
          .label = "Skin structure",
          .tooltip = "Structure on skin for this pass; -1 follows its Structure. A change restarts its history.",
          .min = 0.0,
          .max = 1.0,
          .special = static_cast<double>(SKIN_SAME_AS_STRUCTURE),
          .flags = RESTARTS_HISTORY,
          .get = &PassGet<Index, &PassSettings::skin_structure>,
          .set = &PassSet<Index, &PassSettings::skin_structure>,
          .pass = PASS,
      },
      {
          .key = key(6),
          .kind = SettingKind::BOOL,
          .section = SettingSection::PASSES,
          .label = "Character mask",
          .tooltip = "This pass protects characters with NR's own mask. A change restarts its history.",
          .flags = RESTARTS_HISTORY,
          .get = &PassGet<Index, &PassSettings::auto_mask>,
          .set = &PassSet<Index, &PassSettings::auto_mask>,
          .pass = PASS,
      },
      {
          .key = key(7),
          .kind = SettingKind::FLOAT,
          .section = SettingSection::PASSES,
          .label = "Transfer strength",
          .tooltip = "How much of this pass's brightness change it passes on.",
          .min = 0.0,
          .max = 1.0,
          .get = &PassGet<Index, &PassSettings::transfer_strength>,
          .set = &PassSet<Index, &PassSettings::transfer_strength>,
          .pass = PASS,
      },
      {
          .key = key(8),
          .kind = SettingKind::FLOAT,
          .section = SettingSection::PASSES,
          .label = "Colour strength",
          .tooltip = "How much of this pass's colour change it passes on.",
          .min = 0.0,
          .max = 1.0,
          .get = &PassGet<Index, &PassSettings::color_strength>,
          .set = &PassSet<Index, &PassSettings::color_strength>,
          .pass = PASS,
      },
  });
}

template <size_t... Index>
std::vector<SettingDescriptor> AllPassRows(std::index_sequence<Index...>) {
  std::vector<SettingDescriptor> rows;
  rows.reserve(sizeof...(Index) * PASS_ROW_COUNT);
  (
      [&] {
        const std::array<SettingDescriptor, PASS_ROW_COUNT> pass_rows = PassRows<Index>();
        rows.insert(rows.end(), pass_rows.begin(), pass_rows.end());
      }(),
      ...);
  return rows;
}

}  // namespace

std::span<const SettingDescriptor> SettingsSchema() {
  static const std::vector<SettingDescriptor> schema = [] {
    std::vector<SettingDescriptor> rows(BASE_ROWS.begin(), BASE_ROWS.end());
    const std::vector<SettingDescriptor> pass_rows = AllPassRows(std::make_index_sequence<LATER_PASS_COUNT>{});
    rows.insert(rows.end(), pass_rows.begin(), pass_rows.end());
    return rows;
  }();
  return schema;
}

const SettingDescriptor* FindSetting(std::string_view key) {
  const std::span<const SettingDescriptor> schema = SettingsSchema();
  const auto found = std::ranges::find(schema, key, &SettingDescriptor::key);
  return (found == schema.end() ? nullptr : &*found);
}

const Settings& DefaultSettings() {
  static const Settings defaults;
  return defaults;
}

bool IsDefault(const Settings& settings, const SettingDescriptor& descriptor) {
  if (descriptor.kind == SettingKind::TEXT) return descriptor.get_text(settings) == descriptor.get_text(DefaultSettings());
  return descriptor.get(settings) == descriptor.get(DefaultSettings());
}

void ResetSetting(Settings* settings, const SettingDescriptor& descriptor) {
  if (descriptor.kind == SettingKind::TEXT) {
    descriptor.set_text(*settings, descriptor.get_text(DefaultSettings()));
    return;
  }
  descriptor.set(*settings, descriptor.get(DefaultSettings()));
}

bool SectionChanged(const Settings& settings, SettingSection section) {
  return std::ranges::any_of(SettingsSchema(), [&settings, section](const SettingDescriptor& descriptor) {
    return descriptor.section == section && !IsDefault(settings, descriptor);
  });
}

void ResetSection(Settings* settings, SettingSection section) {
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    if (descriptor.section == section) {
      ResetSetting(settings, descriptor);
    }
  }
}

bool InDefaultsView(const SettingDescriptor& descriptor) {
  switch (descriptor.section) {
    case SettingSection::LOOK:
    case SettingSection::RESULT:
    case SettingSection::LIMITS:
    case SettingSection::PASSES: return true;
    default:                     return false;
  }
}

Settings DefaultsView(Settings mine) {
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    if (InDefaultsView(descriptor)) {
      ResetSetting(&mine, descriptor);
    }
  }
  return mine;
}

void RestoreAllDefaults(Settings* settings) {
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    if (descriptor.key != ENABLED_KEY && (descriptor.flags & setting_flags::BOOKKEEPING) == 0u) {
      ResetSetting(settings, descriptor);
    }
  }
}

std::string FormatSettingValue(const SettingDescriptor& descriptor, const Settings& settings) {
  if (descriptor.kind == SettingKind::TEXT) {
    const std::string_view text = descriptor.get_text(settings);
    return (text.empty() ? std::string("empty") : std::string(text));
  }
  const double value = descriptor.get(settings);
  switch (descriptor.kind) {
    case SettingKind::BOOL:   return (value != 0.0 ? "on" : "off");
    case SettingKind::UINT:   return std::format("{}", static_cast<uint32_t>(value));
    case SettingKind::KEY:    return (value == 0.0 ? std::string("none") : std::format("key {:#04x}", static_cast<uint32_t>(value)));
    case SettingKind::CHOICE: {
      const auto index = static_cast<size_t>(value);
      return (index < descriptor.choices.size() ? std::string(descriptor.choices[index]) : std::string("?"));
    }
    case SettingKind::FLOAT:
      if (descriptor.special.has_value() && value == *descriptor.special) {
        return (value < 0.0 ? std::string("same as Structure") : std::string("automatic"));
      }
      return std::format("{:.2f}", value);
    case SettingKind::TEXT: break;
  }
  return {};
}

double DescriptorMax(const SettingDescriptor& descriptor) {
  if (descriptor.kind == SettingKind::CHOICE) return static_cast<double>(descriptor.choices.size()) - 1.0;
  return descriptor.max;
}

}  // namespace uplift::ui
