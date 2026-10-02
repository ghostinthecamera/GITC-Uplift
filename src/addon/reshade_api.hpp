#pragma once

// Every add-on translation unit includes ReShade through this header, so imgui.h always
// precedes reshade.hpp. reshade.hpp then defines each ImGui function as an inline call through
// the function table ReShade hands out, and reshade::register_addon asks ReShade for the table
// that matches IMGUI_VERSION_NUM. Uplift compiles no ImGui source. CMake sets ImTextureID=ImU64
// and IMGUI_DISABLE_OBSOLETE_FUNCTIONS to match ReShade's own ImGui build.
#include <imgui.h>
#include <reshade.hpp>
