#pragma once

#include <array>
#include <string_view>

// HUD-less (design §9): what Details and the log say about DLSS-G's HUD-less image. English here; the Chinese is in
// ui/localization.cpp, matched by these exact strings (the formatted ones by their templates).
namespace uplift::text {

inline constexpr std::string_view HUDLESS_DETAILS_LABEL = "HUD-less image";
inline constexpr std::string_view HUDLESS_USED = "used: NR ran on it too";
inline constexpr char HUDLESS_HIDDEN[] = "hidden from frame generation ({})";
inline constexpr char HUDLESS_LEFT[] = "left as the game made it ({})";
inline constexpr std::string_view HUDLESS_HOOKS_OFF = "cannot be changed: NGX hooks are off";
// The notes under UI correction while frame generation reads Uplift's output (Before FG): NR runs on the game's HUD-less image, the setting is
// not On, or it is On and the game's image cannot be used (with the Present's reason).
inline constexpr std::string_view HUDLESS_UI_CORRECTION_USED = "On: NR runs on the game's HUD-less image; the HUD is not changed";
inline constexpr std::string_view HUDLESS_UI_CORRECTION_NOT_ON =
    "Auto and Off: NR runs on the frame with its HUD. On uses the game's HUD-less image instead";
inline constexpr char HUDLESS_UI_CORRECTION_UNUSABLE[] = "On, but the game's HUD-less image cannot be used: {}. NR runs on the frame with its HUD";

// The evaluate's reasons (addon::FgInput::Decide).
inline constexpr std::string_view HUDLESS_NO_IMAGE = "no HUD-less image";
inline constexpr std::string_view HUDLESS_NOT_CHANGED = "NR did not change this image";
inline constexpr std::string_view HUDLESS_OTHER_FRAME = "another frame's HUD-less image";
inline constexpr std::string_view HUDLESS_NOT_USED = "NR ran on the frame with its HUD";
inline constexpr std::string_view HUDLESS_HIDE_OFF = "hiding is off";
inline constexpr auto HUDLESS_EVALUATE_REASONS =
    std::to_array<std::string_view>({HUDLESS_NO_IMAGE, HUDLESS_NOT_CHANGED, HUDLESS_OTHER_FRAME, HUDLESS_NOT_USED, HUDLESS_HIDE_OFF});

// Why a Present's HUD-less image can or cannot be used, indexed by addon::HudReason.
inline constexpr auto HUD_REASON_TEXTS = std::to_array<std::string_view>({
    "usable",                                          // USABLE
    "UI correction is not On",                         // CORRECTION_OFF
    "frame generation does not read Uplift's output",  // NOT_READ
    "waiting for steady tags",                         // SETTLING
    "no tags seen",                                    // NOT_TAGGED
    "more than one frame tagged",                      // MORE_THAN_ONE_FRAME
    "no HUD opacity tagged",                           // NO_OPACITY
    "tagged as valid only when made",                  // LIFECYCLE
    "tagged for part of the frame",                    // EXTENT
    "not the frame's size",                            // SIZE
    "multisampled or layered",                         // SAMPLES
    "a format NR cannot use",                          // FORMAT
    "an unknown resource state",                       // STATE
    "cannot be read by a shader",                      // USAGE
    "ReShade's effects run before frame generation",   // EFFECTS_ON
    "no free HUD-less image of Uplift's",             // NO_FREE_IMAGE
});

}  // namespace uplift::text
