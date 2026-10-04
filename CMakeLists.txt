cmake_minimum_required(VERSION 3.23)
project(uplift VERSION 1.1.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Plan 9: one tree, two halves. x64 is everything Uplift was before, plus the 64-bit NR helper; x86 is the 32-bit add-on's
# own code (no NGX, no D3D12 pipeline, no shaders; MinHook only for the Vulkan device hook, Plan 11). build.cmd picks the tools by preset
# (vcvars64 or vcvars32).
set(UPLIFT_ARCH "x64" CACHE STRING "x64 or x86: which half of Uplift this build makes")
if(UPLIFT_ARCH STREQUAL "x86" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "UPLIFT_ARCH=x86 needs the x86 tools: configure release-x86 from build.cmd (vcvars32)")
elseif(UPLIFT_ARCH STREQUAL "x64" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "UPLIFT_ARCH=x64 needs the x64 tools (vcvars64)")
endif()

# The tests, their helpers (src/testing) and the experiment tools are built only where they exist: the public source tree is the
# add-on alone, built by this same file.
set(UPLIFT_WITH_TESTS OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/src/testing" AND EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
  set(UPLIFT_WITH_TESTS ON)
endif()

set(UPLIFT_RESHADE_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/_deps/reshade/include" CACHE PATH "ReShade add-on API headers (pinned tag)")
set(UPLIFT_IMGUI_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/_deps/imgui" CACHE PATH "Dear ImGui headers matching the pinned ReShade tag")
if(NOT EXISTS "${UPLIFT_RESHADE_INCLUDE}/reshade.hpp" OR NOT EXISTS "${UPLIFT_IMGUI_INCLUDE}/imgui.h")
  message(FATAL_ERROR "ReShade/ImGui headers not found; run tools\\fetch_deps.cmd")
endif()

# Plan 11: the Vulkan SDK's headers, for both halves. Nothing links a Vulkan library (vulkan-1.dll is loaded at run time) and nothing from
# the SDK is copied into the repository.
find_path(UPLIFT_VULKAN_INCLUDE vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" DOC "the Vulkan SDK's Include folder")
if(NOT UPLIFT_VULKAN_INCLUDE)
  message(FATAL_ERROR "Uplift needs the Vulkan SDK's headers (install the Vulkan SDK; VULKAN_SDK)")
endif()

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
# The C++ warning and conformance set; MinHook's C sources below keep their own warning level.
add_compile_options(/utf-8 "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/Zc:__cplusplus;/EHsc;/external:W0>")

# Plan 9 (design §2.2): UPLIFT_BUILD_ID = "<version>+<git short hash>[-dirty]", written at every build (not at configure) by
# cmake/build_id.cmake, so gitc-uplift.addon32 and gitc-uplift-helper64.exe built from one tree agree and halves from different
# trees are refused. Targets that include "build_id.hpp" depend on uplift_build_id and add UPLIFT_GENERATED.
set(UPLIFT_GENERATED "${CMAKE_BINARY_DIR}/generated")
add_custom_target(uplift_build_id ALL
  COMMAND "${CMAKE_COMMAND}" "-DUPLIFT_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DUPLIFT_VERSION=${PROJECT_VERSION}"
          "-DUPLIFT_OUTPUT=${UPLIFT_GENERATED}/build_id.hpp" -P "${CMAKE_SOURCE_DIR}/cmake/build_id.cmake"
  BYPRODUCTS "${UPLIFT_GENERATED}/build_id.hpp"
  COMMENT "uplift build id"
  VERBATIM)

# MinHook, in both halves since Plan 11: the x86 add-on detours vulkan-1.dll's vkCreateDevice too (the x64 half also hooks NGX with it).
set(UPLIFT_MINHOOK_DIR "${CMAKE_SOURCE_DIR}/third_party/_deps/minhook" CACHE PATH "MinHook at the commit the pinned ReShade tag records")
if(NOT EXISTS "${UPLIFT_MINHOOK_DIR}/include/MinHook.h")
  message(FATAL_ERROR "MinHook not found in ${UPLIFT_MINHOOK_DIR}; run tools\\fetch_deps.cmd")
endif()

add_library(uplift_minhook STATIC
  "${UPLIFT_MINHOOK_DIR}/src/buffer.c"
  "${UPLIFT_MINHOOK_DIR}/src/hook.c"
  "${UPLIFT_MINHOOK_DIR}/src/trampoline.c"
  "${UPLIFT_MINHOOK_DIR}/src/hde/hde32.c"
  "${UPLIFT_MINHOOK_DIR}/src/hde/hde64.c")
target_include_directories(uplift_minhook SYSTEM PUBLIC "${UPLIFT_MINHOOK_DIR}/include")
target_compile_options(uplift_minhook PRIVATE /W1)

# Plan 11 (key decision c): MinHook is initialised once per process (MH_ERROR_ALREADY_INITIALIZED counts as started), shared by the NGX hooks
# (x64) and the Vulkan device hook (both halves).
add_library(uplift_minhook_once STATIC src/ngx_hooks/minhook_once.cpp)
target_include_directories(uplift_minhook_once PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(uplift_minhook_once PUBLIC uplift_minhook)

# Plan 11: the Vulkan game side, ReShade-free and D3D12-free, in both halves: the pure device-adjustment decision, the vkCreateDevice hook, the
# loader's functions, the imports and the formats. It logs and writes its pending marker through the half's base library (linked below).
add_library(uplift_vk STATIC
  src/vk/device_adjustment.cpp
  src/vk/loader.cpp
  src/vk/device_hook.cpp
  src/vk/marker_ledger.cpp
  src/vk/interop.cpp
  src/vk/format.cpp
  src/vk/frame.cpp)
target_include_directories(uplift_vk PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(uplift_vk SYSTEM PUBLIC "${UPLIFT_VULKAN_INCLUDE}")
target_compile_definitions(uplift_vk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(uplift_vk PUBLIC uplift_minhook_once)

# Plan 12: the OpenGL game side, ReShade-free and D3D12-free, in both halves: the native functions (resolved at run time from the system opengl32.dll; nothing
# links opengl32.lib, and gl.h only supplies GL 1.1's types), the formats by family, the imports and semaphores, the capped GLsync wait and the default
# framebuffer's state guard. It logs nothing.
add_library(uplift_gl STATIC
  src/gl/functions.cpp
  src/gl/format.cpp
  src/gl/state_guard.cpp
  src/gl/interop.cpp)
target_include_directories(uplift_gl PUBLIC "${CMAKE_SOURCE_DIR}/src")

if(UPLIFT_ARCH STREQUAL "x64")

set(UPLIFT_NGX_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/_deps/dlss/include" CACHE PATH "NGX SDK include directory")
if(NOT EXISTS "${UPLIFT_NGX_INCLUDE}/nvsdk_ngx.h")
  message(FATAL_ERROR "NGX SDK headers not found in ${UPLIFT_NGX_INCLUDE}; run tools\\fetch_deps.cmd")
endif()

add_library(uplift_nr STATIC
  src/nr/log.cpp
  src/nr/pe_imports.cpp
  src/nr/driver_store.cpp
  src/nr/snippet.cpp
  src/nr/feature.cpp
  src/nr/timeline.cpp
  src/nr/budget.cpp
  src/nr/real_host.cpp
  src/nr/vk_feature.cpp
  src/nr/vk_host.cpp
  src/nr/session.cpp)
target_include_directories(uplift_nr PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(uplift_nr SYSTEM PUBLIC "${UPLIFT_NGX_INCLUDE}")
# Plan 13: nr::Snippet's Vulkan binding and VkHost/VkFeature name Vulkan's types. Headers only: nothing links a Vulkan library.
target_include_directories(uplift_nr SYSTEM PUBLIC "${UPLIFT_VULKAN_INCLUDE}")
target_compile_definitions(uplift_nr PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(uplift_nr PUBLIC d3d12 dxgi version)

add_library(uplift_ngx_hooks STATIC
  src/ngx_hooks/hook_installer.cpp
  src/ngx_hooks/ngx_parameters.cpp
  src/ngx_hooks/feature_registry.cpp
  src/ngx_hooks/dlss_capture.cpp
  src/ngx_hooks/dlss_overrides.cpp
  src/ngx_hooks/frame_generation.cpp)
target_link_libraries(uplift_ngx_hooks PUBLIC uplift_nr PRIVATE uplift_minhook uplift_minhook_once)

if(UPLIFT_WITH_TESTS)
add_library(uplift_testing STATIC
  src/testing/gpu_environment.cpp
  src/testing/d3d12_util.cpp
  src/testing/d3d9_util.cpp
  src/testing/d3d10_util.cpp
  src/testing/d3d11_util.cpp
  src/testing/d3d11_bindings.cpp
  src/testing/png.cpp
  src/testing/scenario_rig.cpp
  src/testing/color_images.cpp
  src/testing/identity_host.cpp
  src/testing/vk_identity_host.cpp
  src/testing/dlss_host.cpp
  src/testing/dlss_host11.cpp)  # Plan 18: the Direct3D 11 DLSS backends' keys and the fake core's (NVIDIA's is ngx_real_backend11.cpp, test executables only)
target_link_libraries(uplift_testing PUBLIC uplift_nr uplift_color windowscodecs ole32 d3dcompiler advapi32 d3d9 d3d10_1 d3d11 dxgi)
target_compile_definitions(uplift_testing PUBLIC UPLIFT_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}")
endif()

file(TO_CMAKE_PATH "$ENV{WindowsSdkVerBinPath}" UPLIFT_SDK_BIN)
find_program(UPLIFT_DXC NAMES dxc
  HINTS "${UPLIFT_SDK_BIN}x64" "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
if(NOT UPLIFT_DXC)
  message(FATAL_ERROR
    "dxc.exe not found. Uplift compiles its shaders with the Windows SDK's dxc.exe, normally at "
    "Windows Kits\\10\\bin\\<version>\\x64\\dxc.exe. Run this configure from a Visual Studio "
    "Developer/x64 Native Tools prompt (which sets WindowsSdkVerBinPath), install the Windows SDK "
    "if it is missing, or pass -DUPLIFT_DXC=<full path to dxc.exe>.")
endif()
set(UPLIFT_SHADER_OUTPUT "${CMAKE_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${UPLIFT_SHADER_OUTPUT}")
set(UPLIFT_SHADER_HEADERS)
foreach(shader IN ITEMS "encode_cs|cs_6_0" "decode_vs|vs_6_0" "decode_ps|ps_6_0" "decode_cs|cs_6_0" "motion_cs|cs_6_0"
                        "look_pyramid_cs|cs_6_0" "look_stabilize_cs|cs_6_0" "look_shape_cs|cs_6_0" "resolve_cs|cs_6_0"
                        "meter_cs|cs_6_0")
  string(REPLACE "|" ";" shader_parts "${shader}")
  list(GET shader_parts 0 shader_name)
  list(GET shader_parts 1 shader_profile)
  set(shader_header "${UPLIFT_SHADER_OUTPUT}/${shader_name}.h")
  add_custom_command(
    OUTPUT "${shader_header}"
    COMMAND "${UPLIFT_DXC}" -nologo -HV 2021 -T ${shader_profile} -E main -O3 -WX
            -Fh "${shader_header}" -Vn g_${shader_name} "${CMAKE_SOURCE_DIR}/shaders/${shader_name}.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/${shader_name}.hlsl" "${CMAKE_SOURCE_DIR}/shaders/color_common.hlsli"
            "${CMAKE_SOURCE_DIR}/shaders/change_common.hlsli" "${CMAKE_SOURCE_DIR}/shaders/decode_common.hlsli"
            "${CMAKE_SOURCE_DIR}/shaders/look_common.hlsli" "${CMAKE_SOURCE_DIR}/shaders/vk_common.hlsli"
    COMMENT "dxc ${shader_name}.hlsl (${shader_profile})"
    VERBATIM)
  list(APPEND UPLIFT_SHADER_HEADERS "${shader_header}")
endforeach()

# Plan 13 (design §1.5, §4.2): SPIR-V for the Vulkan DLSS placements, from the same HLSL, with the Vulkan SDK's DXC (the Windows SDK's has no SPIR-V
# back end). The SDK is required since Plan 11, so this adds no dependency. The DXIL above is built exactly as before: vk_common.hlsli's macros are empty
# there (no __spirv__), so the generated DXIL headers stay byte-identical.
find_program(UPLIFT_DXC_SPIRV NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH)
if(NOT UPLIFT_DXC_SPIRV)
  message(FATAL_ERROR "the Vulkan SDK's dxc.exe (VULKAN_SDK/Bin) was not found: it compiles Uplift's SPIR-V shaders")
endif()
set(UPLIFT_SPIRV_FLAGS -nologo -spirv -HV 2021 -T cs_6_0 -E main -O3 -WX -fspv-target-env=vulkan1.1
    -fvk-t-shift 0 0 -fvk-u-shift 16 0 -fvk-b-shift 32 0)
# One SPIR-V header: g_<name> from shaders/<source>.hlsl with the given -D options (a storage image's format is UPLIFT_VK_TARGET_FORMAT, a string).
function(uplift_add_spirv name source)
  set(header "${UPLIFT_SHADER_OUTPUT}/${name}.h")
  add_custom_command(
    OUTPUT "${header}"
    COMMAND "${UPLIFT_DXC_SPIRV}" ${UPLIFT_SPIRV_FLAGS} ${ARGN} -Fh "${header}" -Vn g_${name} "${CMAKE_SOURCE_DIR}/shaders/${source}.hlsl"
    DEPENDS "${CMAKE_SOURCE_DIR}/shaders/${source}.hlsl" "${CMAKE_SOURCE_DIR}/shaders/vk_common.hlsli"
            "${CMAKE_SOURCE_DIR}/shaders/color_common.hlsli" "${CMAKE_SOURCE_DIR}/shaders/change_common.hlsli"
            "${CMAKE_SOURCE_DIR}/shaders/decode_common.hlsli" "${CMAKE_SOURCE_DIR}/shaders/look_common.hlsli"
    COMMENT "dxc (SPIR-V) ${name}"
    VERBATIM)
  set(UPLIFT_SHADER_HEADERS ${UPLIFT_SHADER_HEADERS} "${header}" PARENT_SCOPE)
endfunction()
uplift_add_spirv(encode_vk_sampled encode_cs)
uplift_add_spirv(decode_vk_presr decode_cs "-DUPLIFT_VK_TARGET_FORMAT=\"rgba16f\"")
uplift_add_spirv(motion_vk motion_cs)
# Plan 14 (design §3.2): the look stage's three passes, for the Vulkan DLSS placements.
uplift_add_spirv(look_pyramid_vk look_pyramid_cs)
uplift_add_spirv(look_stabilize_vk look_stabilize_cs)
uplift_add_spirv(look_shape_vk look_shape_cs)
# Plan 14 Task 10: each later pass's own Transfer/Colour strength, and the exposure meter (sampled for Before upscaling, one per output format for After DLSS).
uplift_add_spirv(resolve_vk resolve_cs)
uplift_add_spirv(meter_vk_sampled meter_cs)
foreach(format IN ITEMS rgba16f r11g11b10f rgba8 rgb10a2 rgba32f)
  uplift_add_spirv(encode_vk_${format} encode_cs -DUPLIFT_VK_SOURCE_STORAGE=1 "-DUPLIFT_VK_TARGET_FORMAT=\"${format}\"")
  uplift_add_spirv(decode_vk_${format} decode_cs -DUPLIFT_VK_SOURCE_STORAGE=1 "-DUPLIFT_VK_TARGET_FORMAT=\"${format}\"")
  uplift_add_spirv(meter_vk_${format} meter_cs -DUPLIFT_VK_SOURCE_STORAGE=1 "-DUPLIFT_VK_TARGET_FORMAT=\"${format}\"")
endforeach()

add_library(uplift_color STATIC
  src/color/encoding.cpp
  src/color/reference.cpp
  src/color/color_pipeline.cpp
  src/color/vk_color_pipeline.cpp
  src/look/look_math.cpp
  ${UPLIFT_SHADER_HEADERS})
target_include_directories(uplift_color PRIVATE "${UPLIFT_SHADER_OUTPUT}")
target_link_libraries(uplift_color PUBLIC uplift_nr)

add_library(uplift_sources STATIC
  src/sources/work_size.cpp
  src/sources/nr_pipeline.cpp
  src/sources/present_source.cpp
  src/sources/after_dlss_source.cpp
  src/sources/pre_sr_source.cpp
  src/sources/vk_nr_pipeline.cpp
  src/sources/look_plan.cpp
  src/sources/auto_exposure.cpp)
target_link_libraries(uplift_sources PUBLIC uplift_color uplift_ngx_hooks uplift_state)

add_library(uplift_state STATIC
  src/state/compute_shadow.cpp)
target_include_directories(uplift_state PUBLIC "${CMAKE_SOURCE_DIR}/src")

add_library(uplift_ui STATIC
  src/ui/settings.cpp
  src/ui/settings_schema.cpp
  src/ui/controls_coalescer.cpp
  src/ui/status_text.cpp)
target_link_libraries(uplift_ui PUBLIC uplift_color)

add_library(uplift_addon_core STATIC
  src/addon/frame_trigger.cpp
  src/addon/device_context.cpp
  src/addon/frame_config.cpp
  src/addon/placement.cpp
  src/addon/nr_claim.cpp
  src/addon/environment.cpp
  src/addon/swapchain_selector.cpp
  src/addon/dlss_settings.cpp
  src/addon/reshade_version.cpp
  src/addon/ngx_bridge.cpp
  src/addon/d3d11_identity.cpp
  src/addon/dlss_summary.cpp
  src/addon/removal_latch.cpp
  src/addon/launchpad_link.cpp
  src/addon/nr_heartbeat.cpp
  src/addon/dred.cpp)
target_link_libraries(uplift_addon_core PUBLIC uplift_sources uplift_ui uplift_ngx_hooks uplift_state bcrypt)
# Plan 11: the Vulkan side's log and pending marker (nr::Log, addon::WriteLatchMarker) come from here in the 64-bit half.
target_link_libraries(uplift_vk PUBLIC uplift_addon_core)
# Plan 13: native Vulkan NR's device functions and completion (on nr::Timeline), the 64-bit half only. Batch 3: the native context of the DLSS
# placements (addon::VkDlssContext, ReShade-free), which sits on both and on the add-on core.
target_sources(uplift_vk PRIVATE src/vk/nr_functions.cpp src/vk/nr_timeline.cpp src/addon/vk_dlss_context.cpp)

# Plan 7: the D3D11 bridge (design 2026-09-28-uplift-d3d11-design-draft.md). ReShade-free.
# Plan 8: the D3D10 bridge on it (design 2026-09-29-uplift-d3d10-design.md); no new link library.
# Plan 9: D3D12Side (its D3D12 half, which the 32-bit helper's transports sit on too) and RecordNr (the recorder both
# bridges and the helper run).
# Plan 10: KeyedRelay (the D3D10 bridge's relay and keyed-mutex hand-offs, which the 32-bit D3D10 client shares).
# Plan 11: VkBridge (a Vulkan game device's NR, in-process), on src/vk/.
# Plan 12: GlBridge (an OpenGL context's NR, in-process), on src/gl/.
# Plan 18: the Direct3D 11 bridge's mid-frame DLSS hand-off and the Present ring (d3d11_dlss.cpp).
add_library(uplift_bridge STATIC
  src/bridge/bridge_sequence.cpp
  src/bridge/d3d12_side.cpp
  src/bridge/record_nr.cpp
  src/bridge/keyed_relay.cpp
  src/bridge/d3d11_bridge.cpp
  src/bridge/d3d11_dlss.cpp
  src/bridge/d3d10_bridge.cpp
  src/bridge/vk_bridge.cpp
  src/bridge/gl_bridge.cpp)
target_link_libraries(uplift_bridge PUBLIC uplift_addon_core uplift_vk uplift_gl d3d11 d3d12 dxgi)

# Plan 10 (design §2.1): the 32-bit add-on's clients in the 64-bit half too. gitc-uplift.addon64 runs a Direct3D 9 device's NR in
# gitc-uplift-helper64.exe through the same D3D9Client and RemoteNr (the D3D10 and D3D11 clients are compiled here and never routed).
# Plan 12: the OpenGL client (src/client/gl_client.cpp, on src/gl/) and the Direct3D 12 client, in both halves like the others; gitc-uplift.addon64 routes neither.
add_library(uplift_client STATIC
  src/client/remote_nr.cpp
  src/client/d3d9_client.cpp
  src/client/d3d10_client.cpp
  src/client/d3d11_client.cpp
  src/client/d3d12_client.cpp
  src/client/vk_client.cpp
  src/client/gl_client.cpp)
target_link_libraries(uplift_client PUBLIC uplift_ipc uplift_bridge uplift_vk uplift_gl d3d9 d3d11 dxgi)

add_library(uplift_addon SHARED
  src/addon.cpp
  src/addon/helper_front.cpp
  src/addon/live_facts.cpp
  src/addon/log_bridge.cpp
  src/addon/reshade_config_store.cpp
  src/addon/command_list_tracker.cpp
  src/addon/swapchain_usage.cpp
  src/addon/reshade_vk_host.cpp
  src/addon/reshade_gl_host.cpp
  src/addon/reshade_d3d12_host.cpp
  src/ui/overlay.cpp)
set_target_properties(uplift_addon PROPERTIES OUTPUT_NAME "gitc-uplift" SUFFIX ".addon64")
target_include_directories(uplift_addon SYSTEM PRIVATE "${UPLIFT_RESHADE_INCLUDE}" "${UPLIFT_IMGUI_INCLUDE}")
target_include_directories(uplift_addon PRIVATE "${UPLIFT_GENERATED}")
target_compile_definitions(uplift_addon PRIVATE ImTextureID=ImU64 IMGUI_DISABLE_OBSOLETE_FUNCTIONS UPLIFT_VERSION_TEXT="${PROJECT_VERSION}")
target_link_libraries(uplift_addon PRIVATE uplift_addon_core uplift_bridge uplift_client uplift_vk uplift_gl d3d9)
add_dependencies(uplift_addon uplift_build_id)

else()

# Plan 9: the 32-bit add-on's own code. No NGX include path, no shaders, no D3D12 pipeline.
add_library(uplift_ui32 STATIC
  src/nr/log.cpp src/color/encoding.cpp src/look/look_math.cpp
  src/ui/settings.cpp src/ui/settings_schema.cpp src/ui/controls_coalescer.cpp src/ui/status_text.cpp
  src/addon/frame_trigger.cpp src/addon/swapchain_selector.cpp src/addon/launchpad_link.cpp
  src/addon/nr_claim.cpp src/addon/reshade_version.cpp src/addon/environment.cpp src/addon/removal_latch.cpp)
target_include_directories(uplift_ui32 PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(uplift_ui32 PUBLIC dxgi bcrypt version)
# Plan 11: the Vulkan side's log and pending marker come from here in the 32-bit half.
target_link_libraries(uplift_vk PUBLIC uplift_ui32)
add_library(uplift_bridge_sequence STATIC src/bridge/bridge_sequence.cpp)
target_include_directories(uplift_bridge_sequence PUBLIC "${CMAKE_SOURCE_DIR}/src")

endif()

# Plan 9 (design §2.2): the control block, the settings text and the launcher, in both halves. The 32-bit add-on and the
# 32-bit tests start the helper with the launcher; the helper maps the block and reads the text.
add_library(uplift_ipc STATIC
  src/ipc/control_block.cpp
  src/ipc/settings_text.cpp
  src/ipc/helper_launcher.cpp)
target_include_directories(uplift_ipc PUBLIC "${CMAKE_SOURCE_DIR}/src")
if(UPLIFT_ARCH STREQUAL "x64")
  target_link_libraries(uplift_ipc PUBLIC uplift_ui)
else()
  target_link_libraries(uplift_ipc PUBLIC uplift_ui32)
endif()

if(UPLIFT_ARCH STREQUAL "x86")

# Plan 9 (design §2.5): the 32-bit add-on's link to the helper and its D3D9 and D3D11 clients. ReShade-free (native
# pointers only), so the x86 GPU tests drive them exactly as gitc-uplift.addon32 does. The x64 half builds them too (above).
# Plan 10 (design §3): the Direct3D 10 client, on Plan 8's relay (KeyedRelay, ReShade-free too).
# Plan 12: gl_client.cpp (on src/gl/) and d3d12_client.cpp. Nothing here links d3d12.lib or opengl32.lib: the Direct3D 12 client makes only COM calls on the game's
# own device, and every GL function is resolved at run time.
add_library(uplift_client STATIC
  src/client/remote_nr.cpp
  src/client/d3d9_client.cpp
  src/client/d3d10_client.cpp
  src/client/d3d11_client.cpp
  src/client/d3d12_client.cpp
  src/client/vk_client.cpp
  src/client/gl_client.cpp
  src/bridge/keyed_relay.cpp)
target_link_libraries(uplift_client PUBLIC uplift_ipc uplift_bridge_sequence uplift_ui32 uplift_vk uplift_gl d3d9 d3d10_1 d3d11 dxgi)

# Plan 9 (design §2.6): gitc-uplift.addon32, the 32-bit add-on: ReShade's events, the settings and the panel, and the clients (Plan 10:
# through addon::HelperFront, which gitc-uplift.addon64 hosts for its Direct3D 9 devices too). It contains no NGX code; NR runs in
# gitc-uplift-helper64.exe, which lies next to it.
add_library(uplift_addon32 SHARED
  src/addon32.cpp
  src/addon/helper_front.cpp
  src/addon/live_facts.cpp
  src/addon/log_bridge.cpp
  src/addon/reshade_config_store.cpp
  src/addon/swapchain_usage.cpp
  src/addon/reshade_vk_host.cpp
  src/addon/reshade_gl_host.cpp
  src/addon/reshade_d3d12_host.cpp
  src/ui/overlay.cpp)
set_target_properties(uplift_addon32 PROPERTIES OUTPUT_NAME "gitc-uplift" SUFFIX ".addon32")
target_include_directories(uplift_addon32 SYSTEM PRIVATE "${UPLIFT_RESHADE_INCLUDE}" "${UPLIFT_IMGUI_INCLUDE}")
target_include_directories(uplift_addon32 PRIVATE "${UPLIFT_GENERATED}")
target_compile_definitions(uplift_addon32 PRIVATE ImTextureID=ImU64 IMGUI_DISABLE_OBSOLETE_FUNCTIONS UPLIFT_VERSION_TEXT="${PROJECT_VERSION}")
target_link_libraries(uplift_addon32 PRIVATE uplift_client uplift_vk uplift_gl delayimp)
# Plan 10 (final review M-3): D3D11CreateDevice, the only d3d11.dll import, runs for a Direct3D 10 game's relay alone. Delay-loaded, so a
# Direct3D 9 game never maps d3d11.dll for Uplift's sake.
target_link_options(uplift_addon32 PRIVATE /DELAYLOAD:d3d11.dll)
add_dependencies(uplift_addon32 uplift_build_id)

# Plan 9 Task 7: the 32-bit GPU smokes (tests/x86), against the x64 half's helper test build.
if(UPLIFT_WITH_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/tests/x86/CMakeLists.txt")
  enable_testing()
  add_subdirectory(tests/x86)
endif()

endif()

if(UPLIFT_ARCH STREQUAL "x64")

# Plan 9 (design §2.1): the 64-bit NR helper, DeviceContext on D3D12Side; no UI, no window (a WIN32 executable). The
# test build swaps NVIDIA's runtime for testing::IdentityHost (UPLIFT_HELPER_IDENTITY_NR); it never ships.
set(UPLIFT_HELPER_SOURCES src/helper/main.cpp src/helper/helper.cpp src/helper/transports.cpp)
add_executable(uplift_helper64 WIN32 ${UPLIFT_HELPER_SOURCES})
set_target_properties(uplift_helper64 PROPERTIES OUTPUT_NAME "gitc-uplift-helper64")
target_include_directories(uplift_helper64 PRIVATE "${UPLIFT_GENERATED}")
target_link_libraries(uplift_helper64 PRIVATE uplift_bridge uplift_ipc)
add_dependencies(uplift_helper64 uplift_build_id)

if(UPLIFT_WITH_TESTS)
add_executable(uplift_helper64_test WIN32 ${UPLIFT_HELPER_SOURCES})
set_target_properties(uplift_helper64_test PROPERTIES OUTPUT_NAME "uplift-helper64-test")
target_include_directories(uplift_helper64_test PRIVATE "${UPLIFT_GENERATED}")
target_compile_definitions(uplift_helper64_test PRIVATE UPLIFT_HELPER_IDENTITY_NR)
target_link_libraries(uplift_helper64_test PRIVATE uplift_bridge uplift_ipc uplift_testing)
add_dependencies(uplift_helper64_test uplift_build_id)

enable_testing()
add_subdirectory(tests)
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt")
  add_subdirectory(tools)
endif()
endif()

endif()
