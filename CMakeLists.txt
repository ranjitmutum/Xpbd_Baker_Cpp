cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/VcpkgBootstrap.cmake")
project(xpbd_baker LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(XPBD_BUILD_APP "Build Nuklear+SDL3 desktop application" OFF)
option(XPBD_BUILD_CLI "Build headless CLI" ON)
option(XPBD_BUILD_TESTS "Build focused viewport/render regression tests" OFF)
option(XPBD_CHECK_SPIRV "Verify embedded Vulkan SPIR-V during the default build" OFF)
option(XPBD_GFX_OPENGL "Enable OpenGL 3.3 backend in app" ON)
option(XPBD_GFX_VULKAN "Enable Vulkan backend in app" ON)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/Streamline.cmake")

set(XPBD_SPIRV_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh_rt.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh_rt.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh_rt.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh_rt.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/path_trace.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rgen"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rmiss"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_shadow.rmiss"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rchit"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rahit"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/pt_composite.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/pt_composite.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/skybox.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/skybox.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_common.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_transmittance.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_direct_irradiance.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_single_scattering.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_scattering_density.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_indirect_irradiance.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_multiple_scattering.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_environment_cache.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bruneton/atmosphere/definitions.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bruneton/atmosphere/functions.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.frag"
)
set(XPBD_SPIRV_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh_rt.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh_rt.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh_rt.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh_rt.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh_rt.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh_rt.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh_rt.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh_rt.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/path_trace.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/path_trace.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rgen.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rmiss.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_shadow.rmiss.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rchit.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rahit.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rgen.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rmiss.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_shadow.rmiss.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rchit.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/rt_debug.rahit.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/pt_composite.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/pt_composite.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/pt_composite.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/pt_composite.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/skybox.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/skybox.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/skybox.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/skybox.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_transmittance.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_transmittance.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_direct_irradiance.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_direct_irradiance.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_single_scattering.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_single_scattering.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_scattering_density.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_scattering_density.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_indirect_irradiance.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_indirect_irradiance.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_multiple_scattering.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_multiple_scattering.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_environment_cache.comp.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/atmosphere_environment_cache.comp.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.frag.spv.inc"
)
set(XPBD_SPIRV_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv_tool.py")
set(XPBD_SPIRV_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv_manifest.json")
find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
find_program(XPBD_GLSLANG_VALIDATOR
    NAMES glslangValidator glslangValidator.exe
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/glslang/bin"
    DOC "glslangValidator used to reproduce checked-in SPIR-V")
find_program(XPBD_SPIRV_VAL
    NAMES spirv-val spirv-val.exe
    HINTS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv-tools/bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/glslang/bin"
    DOC "Optional standalone SPIR-V Tools validator (glslang validation is always enabled)")

if(Python3_Interpreter_FOUND)
    add_custom_target(xpbd_spirv_stale_detection
        COMMAND "${Python3_EXECUTABLE}" "${XPBD_SPIRV_TOOL}" self-test
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Exercise SPIR-V stale source/binary/include detection"
        VERBATIM)
endif()

set(_xpbd_spirv_tools_ready TRUE)
set(_xpbd_spirv_missing_tools)
if(NOT Python3_Interpreter_FOUND)
    set(_xpbd_spirv_tools_ready FALSE)
    list(APPEND _xpbd_spirv_missing_tools
        "Python 3.9+ (set Python3_EXECUTABLE)")
endif()
if(NOT XPBD_GLSLANG_VALIDATOR OR
   NOT EXISTS "${XPBD_GLSLANG_VALIDATOR}")
    set(_xpbd_spirv_tools_ready FALSE)
    list(APPEND _xpbd_spirv_missing_tools
        "glslangValidator (set XPBD_GLSLANG_VALIDATOR or run tools/setup_tools.ps1 on Windows)")
endif()

if(_xpbd_spirv_tools_ready)
    set(_xpbd_spirv_validator_arguments)
    if(XPBD_SPIRV_VAL AND EXISTS "${XPBD_SPIRV_VAL}")
        list(APPEND _xpbd_spirv_validator_arguments
            --spirv-val "${XPBD_SPIRV_VAL}")
        message(STATUS
            "xpbd_baker: SPIR-V validation enabled (${XPBD_SPIRV_VAL})")
    else()
        message(STATUS
            "xpbd_baker: standalone spirv-val not found; glslang built-in SPIRV-Tools validation remains enabled")
    endif()

    add_custom_target(xpbd_rebuild_spirv
        COMMAND "${Python3_EXECUTABLE}" "${XPBD_SPIRV_TOOL}" rebuild
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --manifest "${XPBD_SPIRV_MANIFEST}"
            --glslang "${XPBD_GLSLANG_VALIDATOR}"
            ${_xpbd_spirv_validator_arguments}
        DEPENDS ${XPBD_SPIRV_SOURCES} "${XPBD_SPIRV_TOOL}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Rebuild deterministic embedded Vulkan SPIR-V and hash manifest"
        VERBATIM)
    if(XPBD_CHECK_SPIRV)
        set(_xpbd_spirv_check_all ALL)
    else()
        set(_xpbd_spirv_check_all)
    endif()
    add_custom_target(xpbd_check_spirv ${_xpbd_spirv_check_all}
        COMMAND "${Python3_EXECUTABLE}" "${XPBD_SPIRV_TOOL}" check
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --manifest "${XPBD_SPIRV_MANIFEST}"
            --glslang "${XPBD_GLSLANG_VALIDATOR}"
            ${_xpbd_spirv_validator_arguments}
        DEPENDS
            ${XPBD_SPIRV_SOURCES}
            ${XPBD_SPIRV_INCLUDES}
            "${XPBD_SPIRV_TOOL}"
            "${XPBD_SPIRV_MANIFEST}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Verify GLSL, SPIR-V binaries, embedded words, and SHA-256 manifest"
        VERBATIM)
    add_dependencies(xpbd_check_spirv xpbd_spirv_stale_detection)
elseif(XPBD_CHECK_SPIRV)
    string(JOIN "; " _xpbd_spirv_missing_text
        ${_xpbd_spirv_missing_tools})
    message(FATAL_ERROR
        "XPBD_CHECK_SPIRV is ON but required tools are unavailable: ${_xpbd_spirv_missing_text}")
else()
    string(JOIN "; " _xpbd_spirv_missing_text
        ${_xpbd_spirv_missing_tools})
    message(STATUS
        "xpbd_baker: SPIR-V compile checks unavailable: ${_xpbd_spirv_missing_text}. "
        "Ordinary builds continue because XPBD_CHECK_SPIRV=OFF.")
endif()

include(CheckIPOSupported)
include(CheckCXXSourceCompiles)
check_ipo_supported(RESULT XPBD_IPO_SUPPORTED OUTPUT XPBD_IPO_ERROR)
if(XPBD_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "xpbd_baker: IPO/LTO enabled for Release")
else()
    message(STATUS "xpbd_baker: IPO/LTO not available (${XPBD_IPO_ERROR})")
endif()

if(MSVC)
    add_compile_options(
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob3>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/GL>
    )
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options($<$<CONFIG:Release>:-O3>)
endif()

if(MINGW AND VCPKG_TARGET_TRIPLET MATCHES "windows(-static)?$")
    message(FATAL_ERROR
        "xpbd_baker uses the MSVC vcpkg triplet '${VCPKG_TARGET_TRIPLET}'.\n"
        "CLion is currently using MinGW (g++).\n"
        "Fix: Settings → Build → Toolchains → set a Visual Studio / MSVC toolchain,\n"
        "then Settings → Build → CMake → Profile → select preset "
        "'windows-msvc-debug' (see CMakePresets.json).")
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Bullet CONFIG REQUIRED)

add_library(xpbd_astronomy_engine STATIC
    third_party/astronomy/astronomy.c
)
target_include_directories(xpbd_astronomy_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/astronomy
)
target_compile_definitions(xpbd_astronomy_engine PUBLIC
    ASTRONOMY_ENGINE_NO_CURRENT_TIME=1
)
set_target_properties(xpbd_astronomy_engine PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
if(MSVC)
    target_compile_options(xpbd_astronomy_engine PRIVATE /W4)
else()
    target_compile_options(xpbd_astronomy_engine PRIVATE -Wall -Wextra)
endif()

check_cxx_source_compiles([=[
#if !defined(_M_IX86) && !defined(_M_X64) && !defined(__i386__) && !defined(__x86_64__)
#error This target is not x86
#endif
int main() { return 0; }
]=] XPBD_X86_SIMD_TARGET_V2)

set(XPBD_SIMD_OBJECTS)
set(XPBD_SIMD_CORE_SOURCES)
if(XPBD_X86_SIMD_TARGET_V2)
    add_library(xpbd_simd_sse2 OBJECT src/core/simd_dense_sse2.cpp)
    add_library(xpbd_simd_avx2 OBJECT src/core/simd_dense_avx2.cpp)
    foreach(_xpbd_simd_target IN ITEMS xpbd_simd_sse2 xpbd_simd_avx2)
        target_include_directories(${_xpbd_simd_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        set_target_properties(${_xpbd_simd_target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION FALSE
            INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
    endforeach()
    if(MSVC)
        # /GL is enabled globally for Release; /GL- is the cross-ISA boundary.
        target_compile_options(xpbd_simd_sse2 PRIVATE /GL-)
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            target_compile_options(xpbd_simd_sse2 PRIVATE /arch:SSE2)
        endif()
        target_compile_options(xpbd_simd_avx2 PRIVATE /arch:AVX2 /GL-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(xpbd_simd_sse2 PRIVATE
            -msse2 -mno-avx -mno-avx2 -mno-fma -fno-lto)
        target_compile_options(xpbd_simd_avx2 PRIVATE
            -mavx2 -mno-fma -fno-lto)
    endif()
    set(XPBD_SIMD_OBJECTS
        $<TARGET_OBJECTS:xpbd_simd_sse2>
        $<TARGET_OBJECTS:xpbd_simd_avx2>)
    set(XPBD_SIMD_CORE_SOURCES src/core/simd_kernel_table.cpp)
endif()

set(XPBD_CORE_SOURCES
    src/models/vector3.cpp
    src/models/particle.cpp
    src/core/xpbd_engine.cpp
    src/core/simd_dispatch.cpp
    ${XPBD_SIMD_CORE_SOURCES}
    src/constraints/distance_constraint.cpp
    src/constraints/weld_constraint.cpp
    src/constraints/target_constraint.cpp
    src/constraints/angle_constraint.cpp
    src/constraints/cross_spring_constraint.cpp
    src/constraints/ground_collision_constraint.cpp
    src/baker/rotation_util.cpp
    src/baker/bedrock_transform_resolver.cpp
    src/baker/cube_geometry.cpp
    src/baker/bone_pose_calculator.cpp
    src/baker/final_pose_reconstructor.cpp
    src/baker/bone_mapper.cpp
    src/baker/loop_bake_config.cpp
    src/baker/loop_error_report.cpp
    src/baker/periodic_state_adapter.cpp
    src/baker/xpbd_periodic_state_tracker.cpp
    src/baker/loop_bake_controller.cpp
    src/baker/bake_profiler.cpp
    src/baker/baked_preview_sampler.cpp
    src/baker/inertialized_target.cpp
    src/baker/transition_bake_request.cpp
    src/baker/transition_bake_controller.cpp
    src/baker/loop_seam_corrector.cpp
    src/baker/loop_seam_report.cpp
    src/baker/output_timeline_resampler.cpp
    src/baker/rigid_body_collision_auditor.cpp
    src/baker/rigid_body_joint_auditor.cpp
    src/baker/rigid_body_input_compat.cpp
    src/baker/physics_baker.cpp
    src/baker/body_collider_cache.cpp
    src/loader/bedrock_animation_data.cpp
    src/loader/model_loader.cpp
    src/loader/animation_loader.cpp
    src/loader/molang_keyframe_detector.cpp
    src/export/atomic_file_writer.cpp
    src/export/animation_exporter.cpp
    src/export/velocity_cache_exporter.cpp
    src/constraints/vertex_face_collision_constraint.cpp
    src/rigidbody/bullet_backend.cpp
    src/rigidbody/bedrock_pose_converter.cpp
    src/rigidbody/bedrock_rigid_body_compiler.cpp
    src/rigidbody/rigid_body_bake_session.cpp
    src/render/skeleton_viewport.cpp
    ${XPBD_SIMD_OBJECTS}
)

add_library(xpbd_core STATIC ${XPBD_CORE_SOURCES})
add_library(xpbd::core ALIAS xpbd_core)

if(XPBD_X86_SIMD_TARGET_V2)
    target_compile_definitions(xpbd_core PRIVATE XPBD_HAS_X86_SIMD=1)
endif()

target_include_directories(xpbd_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(xpbd_core
    PUBLIC
        nlohmann_json::nlohmann_json
)

if(TARGET BulletDynamics)
    target_link_libraries(xpbd_core PUBLIC BulletDynamics BulletCollision LinearMath)
elseif(TARGET Bullet::BulletDynamics)
    target_link_libraries(xpbd_core PUBLIC Bullet::BulletDynamics Bullet::BulletCollision Bullet::LinearMath)
elseif(DEFINED BULLET_LIBRARIES)
    target_link_libraries(xpbd_core PUBLIC ${BULLET_LIBRARIES})
else()
    message(FATAL_ERROR "Bullet found but no usable CMake targets (BulletDynamics / BULLET_LIBRARIES)")
endif()
if(DEFINED BULLET_INCLUDE_DIRS)
    target_include_directories(xpbd_core PUBLIC ${BULLET_INCLUDE_DIRS})
endif()

if(MSVC)
    target_compile_options(xpbd_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(xpbd_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(XPBD_BUILD_TESTS)
    enable_testing()
    if(Python3_Interpreter_FOUND)
        add_test(NAME xpbd_spirv_stale_detection
            COMMAND "${Python3_EXECUTABLE}" "${XPBD_SPIRV_TOOL}" self-test)
        set_tests_properties(xpbd_spirv_stale_detection PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if(_xpbd_spirv_tools_ready)
        add_test(NAME xpbd_spirv_consistency
            COMMAND "${Python3_EXECUTABLE}" "${XPBD_SPIRV_TOOL}" check
                --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --manifest "${XPBD_SPIRV_MANIFEST}"
                --glslang "${XPBD_GLSLANG_VALIDATOR}"
                ${_xpbd_spirv_validator_arguments})
        set_tests_properties(xpbd_spirv_consistency PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    add_executable(xpbd_viewport_regression_tests
        src/gfx/viewport_regression_tests_main.cpp
        src/gfx/viewport_mesh.cpp
        src/gfx/texture_image.cpp
        src/gfx/labpbr_material.cpp
        src/gfx/labpbr_import.cpp
        src/gfx/labpbr_authoring.cpp
        src/gfx/labpbr_export.cpp
        src/gfx/rt_scene_records.cpp
        src/gfx/ray_tracing.cpp
        src/gfx/world_environment.cpp
        src/gfx/preview_scene.cpp
        src/gfx/preview_surfaces.cpp
    )
    target_include_directories(xpbd_viewport_regression_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fast_noise_lite
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    )
    target_link_libraries(xpbd_viewport_regression_tests PRIVATE
        xpbd::core
        xpbd_astronomy_engine
    )
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}"
         _xpbd_source_dir_tests)
    target_compile_definitions(xpbd_viewport_regression_tests PRIVATE
        "XPBD_TEST_SOURCE_DIR=\"${_xpbd_source_dir_tests}\"")
    if(MSVC)
        target_compile_options(xpbd_viewport_regression_tests PRIVATE
            /W4 /permissive- /utf-8)
    else()
        target_compile_options(xpbd_viewport_regression_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME xpbd_viewport_regression
             COMMAND xpbd_viewport_regression_tests)

    add_executable(xpbd_nuklear_focus_regression_tests
        src/app/nuklear_focus_regression_tests_main.cpp
        src/app/nuklear_still_settings_panel.cpp
    )
    target_include_directories(xpbd_nuklear_focus_regression_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nuklear
    )
    target_link_libraries(xpbd_nuklear_focus_regression_tests PRIVATE
        xpbd::core
    )
    if(MSVC)
        target_compile_options(xpbd_nuklear_focus_regression_tests PRIVATE
            /utf-8 /W4 /permissive-)
        target_compile_definitions(xpbd_nuklear_focus_regression_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(xpbd_nuklear_focus_regression_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME xpbd_nuklear_focus_regression
             COMMAND xpbd_nuklear_focus_regression_tests)
endif()

if(XPBD_BUILD_CLI)
    add_executable(xpbd_cli
        src/app/cli_main.cpp
        src/app/prefer_high_perf_gpu.cpp
    )
    target_link_libraries(xpbd_cli PRIVATE xpbd::core)
    if(MSVC)
        target_compile_options(xpbd_cli PRIVATE /utf-8)
        target_link_options(xpbd_cli PRIVATE
            "/INCLUDE:NvOptimusEnablement"
            "/INCLUDE:AmdPowerXpressRequestHighPerformance")
    endif()
endif()

if(XPBD_BUILD_APP)
    find_package(SDL3 CONFIG REQUIRED)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/VulkanHints.cmake")
    if(NOT XPBD_GFX_OPENGL AND NOT XPBD_GFX_VULKAN)
        message(FATAL_ERROR
            "xpbd_baker_app requires at least one retained backend: "
            "XPBD_GFX_OPENGL or XPBD_GFX_VULKAN")
    endif()

    set(_xpbd_nuklear_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nuklear")
    if(NOT EXISTS "${_xpbd_nuklear_dir}/nuklear.h")
        message(FATAL_ERROR
            "Nuklear not found at third_party/nuklear/nuklear.h\n"
            "Download: https://github.com/Immediate-Mode-UI/Nuklear (nuklear.h)")
    endif()

    find_package(spdlog CONFIG REQUIRED)

    add_executable(xpbd_baker_app
        src/app/main_sdl3.cpp
        src/app/nuklear_ui.cpp
        src/app/nuklear_still_settings_panel.cpp
        src/app/app_session.cpp
        src/app/native_dialog.cpp
        src/app/prefer_high_perf_gpu.cpp
        src/app/log.cpp
        src/app/i18n.cpp
        src/gfx/backend_select.cpp
        src/gfx/gpu_backend_factory.cpp
        src/gfx/ray_tracing.cpp
        src/gfx/world_environment.cpp
        src/gfx/viewport_mesh.cpp
        src/gfx/preview_scene.cpp
        src/gfx/preview_surfaces.cpp
        src/gfx/still_image_export.cpp
        src/gfx/texture_image.cpp
        src/gfx/labpbr_material.cpp
        src/gfx/labpbr_import.cpp
        src/gfx/labpbr_authoring.cpp
        src/gfx/labpbr_export.cpp
        src/gfx/rt_scene_records.cpp
    )
    target_include_directories(xpbd_baker_app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fast_noise_lite
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        ${_xpbd_nuklear_dir}
    )
    target_link_libraries(xpbd_baker_app PRIVATE
        xpbd::core
        xpbd_astronomy_engine
        SDL3::SDL3
        spdlog::spdlog
    )
    target_compile_definitions(xpbd_baker_app PRIVATE SPDLOG_COMPILED_LIB)
    # GUI subsystem: no console window on Windows (custom WinMain in main_sdl3.cpp).
    if(WIN32)
        set_target_properties(xpbd_baker_app PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_libraries(xpbd_baker_app PRIVATE
            comdlg32
            ole32
            shell32
            uuid
        )
    endif()
    if(XPBD_GFX_OPENGL)
        target_sources(xpbd_baker_app PRIVATE
            src/gfx/gl_loader.cpp
            src/gfx/gl33_backend.cpp
        )
        target_compile_definitions(xpbd_baker_app PRIVATE XPBD_GFX_OPENGL=1)
        if(WIN32)
            target_link_libraries(xpbd_baker_app PRIVATE opengl32)
        elseif(APPLE)
            find_library(XPBD_OPENGL_FRAMEWORK OpenGL REQUIRED)
            target_link_libraries(xpbd_baker_app PRIVATE
                ${XPBD_OPENGL_FRAMEWORK})
        else()
            find_package(OpenGL REQUIRED)
            target_link_libraries(xpbd_baker_app PRIVATE OpenGL::GL)
        endif()
    endif()
    if(XPBD_GFX_VULKAN)
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            target_sources(xpbd_baker_app PRIVATE
                src/gfx/vulkan_backend.cpp
                src/gfx/streamline_vulkan_runtime.cpp
                src/gfx/vulkan_rt_scene.cpp
                src/gfx/vulkan_path_tracer.cpp
                src/gfx/vulkan_rt_pipeline.cpp
            )
            target_compile_definitions(xpbd_baker_app PRIVATE XPBD_GFX_VULKAN=1)
            target_link_libraries(xpbd_baker_app PRIVATE Vulkan::Vulkan)
            if(XPBD_STREAMLINE_FOUND)
                target_compile_definitions(xpbd_baker_app PRIVATE
                    XPBD_WITH_STREAMLINE=1)
                target_include_directories(xpbd_baker_app PRIVATE
                    "${XPBD_STREAMLINE_SDK_ROOT}/include")
                add_custom_command(TARGET xpbd_baker_app POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.interposer.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.common.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.dlss.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.dlss_d.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.dlss_g.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.reflex.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.pcl.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/NvLowLatencyVk.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/nvngx_dlss.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/nvngx_dlssd.dll"
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/nvngx_dlssg.dll"
                        "$<TARGET_FILE_DIR:xpbd_baker_app>"
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:xpbd_baker_app>/notices"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${XPBD_STREAMLINE_SDK_ROOT}/license.txt"
                        "$<TARGET_FILE_DIR:xpbd_baker_app>/notices/STREAMLINE-LICENSE.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${XPBD_STREAMLINE_SDK_ROOT}/3rd-party-licenses.md"
                        "$<TARGET_FILE_DIR:xpbd_baker_app>/notices/STREAMLINE-3RD-PARTY-LICENSES.md"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/nvngx_dlss.license.txt"
                        "$<TARGET_FILE_DIR:xpbd_baker_app>/notices/NVNGX-DLSS-LICENSE.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/reflex.license.txt"
                        "$<TARGET_FILE_DIR:xpbd_baker_app>/notices/NVIDIA-REFLEX-LICENSE.txt"
                    COMMENT "Copy signed NVIDIA Streamline/DLSS runtime and licenses")
            else()
                target_compile_definitions(xpbd_baker_app PRIVATE
                    XPBD_WITH_STREAMLINE=0)
            endif()
            message(STATUS "xpbd_baker: Vulkan package found")
        else()
            message(WARNING
                "xpbd_baker: Vulkan package not found; Vulkan is disabled "
                "and the app will use OpenGL when available")
        endif()
    endif()
    if(MSVC)
        target_compile_options(xpbd_baker_app PRIVATE /utf-8 /W4 /permissive- /wd5054 /wd4805 /wd4305)
        target_compile_definitions(xpbd_baker_app PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_options(xpbd_baker_app PRIVATE
            "/INCLUDE:NvOptimusEnablement"
            "/INCLUDE:AmdPowerXpressRequestHighPerformance")
    else()
        target_compile_options(xpbd_baker_app PRIVATE -Wall -Wextra)
    endif()

    if(XPBD_BUILD_TESTS)
        add_executable(xpbd_app_session_regression_tests
            src/app/app_session_regression_tests_main.cpp
            src/app/app_session.cpp
            src/app/native_dialog.cpp
            src/app/log.cpp
            src/gfx/viewport_mesh.cpp
            src/gfx/texture_image.cpp
            src/gfx/labpbr_material.cpp
            src/gfx/labpbr_import.cpp
            src/gfx/labpbr_authoring.cpp
            src/gfx/labpbr_export.cpp
            src/gfx/ray_tracing.cpp
            src/gfx/still_image_export.cpp
            src/gfx/world_environment.cpp
        )
        target_include_directories(xpbd_app_session_regression_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        )
        target_link_libraries(xpbd_app_session_regression_tests PRIVATE
            xpbd::core
            xpbd_astronomy_engine
            spdlog::spdlog
        )
        target_compile_definitions(xpbd_app_session_regression_tests PRIVATE
            SPDLOG_COMPILED_LIB
        )
        if(WIN32)
            target_link_libraries(xpbd_app_session_regression_tests PRIVATE
                comdlg32
                ole32
                shell32
                uuid
            )
        endif()
        if(MSVC)
            target_compile_options(xpbd_app_session_regression_tests PRIVATE
                /utf-8 /W4 /permissive-)
            target_compile_definitions(xpbd_app_session_regression_tests PRIVATE
                _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        else()
            target_compile_options(xpbd_app_session_regression_tests PRIVATE
                -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME xpbd_app_session_regression
                 COMMAND xpbd_app_session_regression_tests)
    endif()

    # Copy runtime DLLs next to the exe (vcpkg shared triplet).
    if(WIN32)
        add_custom_command(TARGET xpbd_baker_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3>
                $<TARGET_FILE_DIR:xpbd_baker_app>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:spdlog::spdlog>
                $<TARGET_FILE_DIR:xpbd_baker_app>
            COMMENT "Copy SDL3/spdlog DLLs beside xpbd_baker_app"
        )
        # Portable LLVM-MinGW links libc++ and libunwind dynamically even when
        # vcpkg libraries use the static MinGW triplet. Ship those compiler
        # runtimes beside every executable so the app/tests run outside a
        # developer shell without system-error popups.
        if(MINGW)
            get_filename_component(XPBD_MINGW_RUNTIME_DIR
                "${CMAKE_CXX_COMPILER}" DIRECTORY)
            foreach(_xpbd_runtime IN ITEMS libc++.dll libunwind.dll)
                if(EXISTS "${XPBD_MINGW_RUNTIME_DIR}/${_xpbd_runtime}")
                    add_custom_command(TARGET xpbd_baker_app POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${XPBD_MINGW_RUNTIME_DIR}/${_xpbd_runtime}"
                            $<TARGET_FILE_DIR:xpbd_baker_app>
                        COMMENT "Copy ${_xpbd_runtime} beside xpbd_baker_app")
                endif()
            endforeach()
        endif()
        # fmt is a dependency of spdlog (shared)
        if(TARGET fmt::fmt)
            add_custom_command(TARGET xpbd_baker_app POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:fmt::fmt>
                    $<TARGET_FILE_DIR:xpbd_baker_app>
                COMMENT "Copy fmt.dll beside xpbd_baker_app"
            )
        endif()
        # vulkan-1.dll from vcpkg when present
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/vulkan-1.dll")
            add_custom_command(TARGET xpbd_baker_app POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/vulkan-1.dll"
                    $<TARGET_FILE_DIR:xpbd_baker_app>
                COMMENT "Copy vulkan-1.dll beside xpbd_baker_app"
            )
        endif()
        # Third-party notices next to the exe (for packaging / redistribution)
        add_custom_command(TARGET xpbd_baker_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/notices"
                "$<TARGET_FILE_DIR:xpbd_baker_app>/notices"
            COMMENT "Copy notices/ beside xpbd_baker_app"
        )
        # Fixed offline CC0 preview-scene HDR assets.
        add_custom_command(TARGET xpbd_baker_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/preview_scenes"
                "$<TARGET_FILE_DIR:xpbd_baker_app>/assets/preview_scenes"
            COMMENT "Copy CC0 preview-scene assets beside xpbd_baker_app"
        )
        # i18n JSON catalogs next to the exe
        add_custom_command(TARGET xpbd_baker_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
                "$<TARGET_FILE_DIR:xpbd_baker_app>/i18n"
            COMMENT "Copy i18n/ beside xpbd_baker_app"
        )
    endif()
endif()

if(MINGW)
    get_filename_component(XPBD_MINGW_RUNTIME_DIR
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_xpbd_target IN ITEMS xpbd_cli)
        if(TARGET ${_xpbd_target})
            foreach(_xpbd_runtime IN ITEMS libc++.dll libunwind.dll)
                if(EXISTS "${XPBD_MINGW_RUNTIME_DIR}/${_xpbd_runtime}")
                    add_custom_command(TARGET ${_xpbd_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${XPBD_MINGW_RUNTIME_DIR}/${_xpbd_runtime}"
                            $<TARGET_FILE_DIR:${_xpbd_target}>
                        COMMENT "Copy ${_xpbd_runtime} beside ${_xpbd_target}")
                endif()
            endforeach()
        endif()
    endforeach()
endif()

message(STATUS
    "xpbd_baker: cli=${XPBD_BUILD_CLI} app=${XPBD_BUILD_APP}")
