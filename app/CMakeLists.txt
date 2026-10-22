# Metal Gear Rising: Revengeance (Xbox 360): static recompilation host application, stage 1 (Windows PC).
#
# Run tools/codegen.ps1 first: it writes generated/ (including generated/rexglue.cmake) from your own default.xex.
# Build (from PowerShell, through tools\win\run.ps1):
#   cmake --preset win-pc -S app && cmake --build out/pc
#
# The SA2 port's full CMake file (native renderer, Switch build, PGO, ...) is kept as CMakeLists.sa2.txt: the Switch
# and renderer parts come back from there, one at a time, after the first boot.

cmake_minimum_required(VERSION 3.25)
project(sa2 LANGUAGES CXX)   # "sa2" is the prefix of the generated files; renamed to "mgr" in one step later

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR "app/generated/rexglue.cmake is missing: run tools/codegen.ps1 first")
endif()
include(generated/rexglue.cmake)

option(MGR_RENUT_RENDERER "Build the Direct3D-level reNut renderer (deko3d on Switch)" ${REXGLUE_PLATFORM_SWITCH})
if(REXGLUE_PLATFORM_SWITCH AND NOT MGR_RENUT_RENDERER)
    message(FATAL_ERROR "This Switch product requires MGR_RENUT_RENDERER=ON")
endif()
if(REXGLUE_PLATFORM_SWITCH AND NOT REXGLUE_SWITCH_NO_NVK)
    message(FATAL_ERROR "This deko3d product requires REXGLUE_SWITCH_NO_NVK=ON")
endif()

# The build re-runs the generator when an input in codegen.d changes; the flavour's post-codegen steps follow it.
if(TARGET sa2_codegen)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp" APPEND
        COMMAND "${CMAKE_COMMAND}" -E env "REXSDK_DIR=${REXSDK_DIR}"
                "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/post_codegen.py")
endif()

set(MGR_SOURCES
    mgr_src/mgr_main.cpp
    # Stage 4, milestone 2: the shader container dump (--mgr_dump_shaders=true) on CreateVertexShader / CreatePixelShader.
    mgr_src/mgr_shader_dump.cpp
)
# Diagnostic: counting hooks on every function of the Direct3D range (tools/mgr_d3d_census.py writes the source).
option(MGR_D3D_CENSUS "Count the calls of every Direct3D-range function per frame (diagnostic build)" OFF)
if(MGR_D3D_CENSUS)
    list(APPEND MGR_SOURCES mgr_src/mgr_d3d_census.cpp)
endif()

if(WIN32)
    add_executable(sa2 WIN32 ${MGR_SOURCES})
else()
    add_executable(sa2 ${MGR_SOURCES})
endif()

# Stage 1 draws with ReXGlue's Xenos GPU emulation.
if(REXGLUE_PLATFORM_SWITCH)
    # The native renderer owns Vulkan on Horizon; the emulated GPU is only a PC reference.
    rexglue_setup_target(sa2)
else()
    rexglue_setup_target(sa2 GPU_PLUGINS xenos)
endif()

# The app includes xxhash.h (and later the renderer renderdoc_app.h) directly; when the SDK is built from source they
# are private to its libraries (the same block as CMakeLists.sa2.txt).
foreach(_dep xxhash renderdoc)
    if(TARGET ${_dep})
        target_include_directories(sa2 PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# Stage 4, milestone 2: the native Vulkan renderer of the SA2 port (src/me, from masseffect-nx through banjo-nx), with
# MGR's own hooks (mgr_src/mgr_me_hooks.cpp). Left out on purpose: SA2's hooks and everything that names SA2's guest
# functions (src/me/sa2_me_hooks.cpp, sa2_d3d_api.cpp, sa2_api_*.cpp: the native Direct3D layer; src/cpu/sa2_hot_guest.cpp,
# sa2_gpu_wait.cpp; src/sa2_d3d_trace.cpp, sa2_debug_hooks.cpp). What the kept code still needs from them is stubbed in
# mgr_src/mgr_sa2_stubs.cpp. The renderer runs only with sa2_renderer_native = true; the emulated GPU stays the default.
set(_mgr_native_default ON)
if(MGR_RENUT_RENDERER)
    set(_mgr_native_default OFF)
endif()
option(MGR_NATIVE_RENDERER "Build the PM4 Vulkan reference renderer (src/me)" ${_mgr_native_default})
if(MGR_NATIVE_RENDERER)
    target_sources(sa2 PRIVATE
        mgr_src/mgr_me_hooks.cpp
        mgr_src/mgr_sa2_stubs.cpp
        src/sa2_capture.cpp
        src/profiler/sa2_thread_cycles_pc.cpp
        src/me/me_native_system.cpp
        src/me/me_ring_wait.cpp
        src/me/me_renderdoc.cpp
        src/me/native/sa2_native_draws.cpp
        src/me/native/sa2_deferred_recording.cpp
        src/me/native/sa2_native_targets.cpp
        src/me/native/sa2_native_shaders.cpp
        src/me/native/sa2_native_textures_pool.cpp
        src/me/native/sa2_shader_library.cpp
        src/me/native/sa2_native_glue.cpp
        src/me/sa2_render_mode.cpp
        src/cpu/sa2_enhancements.cpp
        src/cpu/sa2_codegen_checks.cpp
        src/cpu/sa2_forms_selfcheck.cpp)
    if(NOT REXGLUE_PLATFORM_SWITCH)
        target_sources(sa2 PRIVATE src/me/me_pc_stubs.cpp)
    endif()
    target_compile_definitions(sa2 PRIVATE MGR_NATIVE_RENDERER=1)
    set(_mgr_codegen_label "classic codegen")
    set(_mgr_codegen_label_file "${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen_flavor.txt")
    if(EXISTS "${_mgr_codegen_label_file}")
        file(READ "${_mgr_codegen_label_file}" _mgr_codegen_label)
        string(STRIP "${_mgr_codegen_label}" _mgr_codegen_label)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mgr_codegen_label_file}")
    endif()
    set_source_files_properties(src/cpu/sa2_enhancements.cpp PROPERTIES
        COMPILE_DEFINITIONS "SA2_BUILD_FLAVOR=\"${_mgr_codegen_label}\"")

# SPIR-V initializers of the native renderer's EDRAM transfer / utility shaders (src/me/native/shaders), compiled
# on the host with glslangValidator (tools/win/build_glslang.ps1 builds it from the SDK's glslang). The Switch build
# uses the same host tool: SPIR-V is platform independent.
set(_MGR_HOST_EXE_SUFFIX "")
if(CMAKE_HOST_WIN32)
    set(_MGR_HOST_EXE_SUFFIX ".exe")
endif()
set(SA2_GLSLANG_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/../out/glslang/StandAlone/glslangValidator${_MGR_HOST_EXE_SUFFIX}"
    CACHE FILEPATH "Host glslangValidator")
set(SA2_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
set(SA2_EDRAM_SHADER_INCLUDES)
foreach(_shader sa2_edram_7e3_to_rgba8 sa2_edram_rgba8_to_7e3 sa2_edram_16f_to_16f me_resolve_exp_bias me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_import me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2 me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_depth_to_raw64 me_edram_raw64_to_depth me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2 me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_edram_color_to_color me_edram_r64_to_r64 sa2_resolve_copy sa2_depth_resolve_ms)
    set(_extension comp)
    if(_shader STREQUAL "me_edram_import")
        set(_extension vert)
    elseif(_shader MATCHES "^me_edram_(color_to_depth|depth_to_depth|depth_to_stencil|color_to_stencil|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x|raw64_to_depth|color_to_color|r64_to_r64)$" OR _shader STREQUAL "sa2_resolve_copy" OR
           _shader STREQUAL "sa2_depth_resolve_ms")
        set(_extension frag)
    endif()
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/src/me/native/shaders/${_shader}.${_extension}")
    set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}.inc")
    set(_target_flags)
    if(_shader MATCHES "^me_edram_(stencil_to_buffer|color_stencil_to_buffer|stencil_msaa2_to_buffer|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x)$" OR _shader STREQUAL "me_depth_resolve_guestspace_msaa2")
        set(_target_flags --target-env vulkan1.2)
        set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}_generated.inc")
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SA2_GENERATED_SHADER_DIR}"
        COMMAND "${SA2_GLSLANG_VALIDATOR}" -V ${_target_flags} -x -o "${_output}" "${_source}"
        DEPENDS "${_source}" VERBATIM)
    list(APPEND SA2_EDRAM_SHADER_INCLUDES "${_output}")
endforeach()
add_custom_target(sa2_edram_shaders DEPENDS ${SA2_EDRAM_SHADER_INCLUDES})
add_dependencies(sa2 sa2_edram_shaders)
target_include_directories(sa2 PRIVATE
    "${SA2_GENERATED_SHADER_DIR}"
    src
    src/me
    src/me/native
    src/me/native/shaders
    "${REXSDK_DIR}/src/core"
    # Precompiled SPIR-V of the SDK presenter (vulkan_spirv/guest_output_*): the renderer draws into its output.
    "${REXSDK_DIR}/src/ui/shaders")

# CPU-only SDK sources the native renderer reuses (draw-extent estimation, shader microcode interpretation): the
# GPU plugin's private symbols cannot satisfy application-side use.
target_sources(sa2 PRIVATE
    "${REXSDK_DIR}/src/graphics/register_file.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")

endif()

if(MGR_RENUT_RENDERER)
    if(NOT MGR_NATIVE_RENDERER)
        target_sources(sa2 PRIVATE
            mgr_src/mgr_me_hooks.cpp
            mgr_src/mgr_sa2_stubs.cpp
            src/sa2_capture.cpp
            src/profiler/sa2_thread_cycles_pc.cpp
            src/me/sa2_render_mode.cpp
            src/cpu/sa2_enhancements.cpp
            src/cpu/sa2_codegen_checks.cpp
            src/cpu/sa2_forms_selfcheck.cpp)
    endif()
    target_include_directories(sa2 PRIVATE src src/me src/me/native "${REXSDK_DIR}/src/core")
    include(cmake/mgr_renut_gpu.cmake)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # Full codegen makes unhooked calls direct. Optimize those calls across the
    # generated translation units, preserving guest floating-point semantics.
    option(MGR_SWITCH_LTO "LTO for MGR game code and native renderer" ON)
    set(MGR_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel Switch LTO link jobs")
    target_compile_definitions(sa2 PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
    if(MGR_SWITCH_LTO)
        set(_mgr_lto_targets sa2_recomp sa2)
        if(TARGET mgr_renut_gpu)
            list(APPEND _mgr_lto_targets mgr_renut_gpu)
        endif()
        foreach(_mgr_lto_target IN LISTS _mgr_lto_targets)
            target_compile_options(${_mgr_lto_target} PRIVATE -flto -fno-fat-lto-objects -fipa-pta)
        endforeach()
        target_link_options(sa2 PRIVATE -flto=${MGR_SWITCH_LTO_JOBS} -flto-partition=balanced
            -fipa-pta -ffp-contract=off -fno-strict-aliasing)
    endif()
    option(MGR_SWITCH_HOT_ORDER "Group MGR hot functions from console samples" ON)
    if(MGR_SWITCH_HOT_ORDER)
        set(_mgr_hot_order "${CMAKE_CURRENT_SOURCE_DIR}/mgr_hot_functions.ld")
        if(NOT EXISTS "${_mgr_hot_order}")
            message(FATAL_ERROR "mgr_hot_functions.ld is missing; regenerate it from MGR console samples")
        endif()
        target_link_options(sa2 PRIVATE "-Wl,--section-ordering-file=${_mgr_hot_order}")
        set_property(TARGET sa2 APPEND PROPERTY LINK_DEPENDS "${_mgr_hot_order}")
    endif()
    target_compile_options(sa2_recomp PRIVATE -ffp-contract=off -fno-strict-aliasing)
    # One startup-only function registers ~85k guest functions. GCC's -O3
    # optimization of that single basic block consumed >7 GB on the host.
    # Keep gameplay code optimized and compile only the registration list at O0.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sa2_register.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -O0)
    set_source_files_properties(generated/default/sa2_register.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    rexglue_switch_add_nro(sa2 NAME "Metal Gear Rising deko3d" AUTHOR "MGR-NX" VERSION "0.2.0")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sa2 PRIVATE -ffp-contract=off -fno-strict-aliasing)
endif()
