cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/VcpkgBootstrap.cmake")
project(xpbd_baker LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(XPBD_BUILD_APP "Build Nuklear+SDL3 desktop application" OFF)
option(XPBD_BUILD_CLI "Build headless CLI" ON)
option(XPBD_BUILD_BENCHMARKS "Build repeatable performance benchmark tool" ON)
option(XPBD_CHECK_SPIRV "Verify embedded Vulkan SPIR-V during the default build" OFF)
option(XPBD_GFX_OPENGL "Enable OpenGL 3.3 backend in app" ON)
option(XPBD_GFX_VULKAN "Enable Vulkan backend in app (stub until full impl)" ON)

set(XPBD_SPIRV_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.frag"
)
set(XPBD_SPIRV_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/mesh.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/static_mesh.frag.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.vert.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.frag.spv"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.vert.spv.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/spirv/ui.frag.spv.inc"
)
if(WIN32)
    find_program(XPBD_POWERSHELL_EXECUTABLE NAMES powershell pwsh)
    set(XPBD_GLSLANG_VALIDATOR
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/glslang/bin/glslangValidator.exe")
    if(XPBD_POWERSHELL_EXECUTABLE AND EXISTS "${XPBD_GLSLANG_VALIDATOR}")
        add_custom_target(xpbd_rebuild_spirv
            COMMAND "${XPBD_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_spirv.ps1"
            DEPENDS ${XPBD_SPIRV_SOURCES}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Rebuild deterministic embedded Vulkan SPIR-V"
            VERBATIM)
        if(XPBD_CHECK_SPIRV)
            set(_xpbd_spirv_check_all ALL)
        else()
            set(_xpbd_spirv_check_all)
        endif()
        add_custom_target(xpbd_check_spirv ${_xpbd_spirv_check_all}
            COMMAND "${XPBD_POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_spirv.ps1" -Check
            DEPENDS ${XPBD_SPIRV_SOURCES} ${XPBD_SPIRV_INCLUDES}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Verify embedded Vulkan SPIR-V is current"
            VERBATIM)
    elseif(XPBD_CHECK_SPIRV)
        message(FATAL_ERROR
            "XPBD_CHECK_SPIRV requires PowerShell and tools/glslang/bin/glslangValidator.exe")
    else()
        message(STATUS
            "xpbd_baker: SPIR-V targets unavailable (run tools/setup_tools.ps1 on Windows)")
    endif()
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

if(XPBD_BUILD_BENCHMARKS)
    add_executable(xpbd_perf_benchmark
        tools/perf_benchmark_main.cpp
        src/gfx/texture_image.cpp
        src/gfx/viewport_mesh.cpp
    )
    target_link_libraries(xpbd_perf_benchmark PRIVATE xpbd::core)
    target_include_directories(xpbd_perf_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    )
    if(MSVC)
        target_compile_options(xpbd_perf_benchmark PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(xpbd_perf_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(XPBD_BUILD_APP)
    find_package(SDL3 CONFIG REQUIRED)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/VulkanHints.cmake")

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
        src/app/app_session.cpp
        src/app/prefer_high_perf_gpu.cpp
        src/app/log.cpp
        src/app/i18n.cpp
        src/gfx/gl_loader.cpp
        src/gfx/gl33_backend.cpp
        src/gfx/vulkan_backend.cpp
        src/gfx/dx11_backend.cpp
        src/gfx/metal_backend.cpp
        src/gfx/backend_select.cpp
        src/gfx/gpu_backend_factory.cpp
        src/gfx/viewport_mesh.cpp
        src/gfx/texture_image.cpp
    )
    target_include_directories(xpbd_baker_app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        ${_xpbd_nuklear_dir}
    )
    target_link_libraries(xpbd_baker_app PRIVATE
        xpbd::core
        SDL3::SDL3
        spdlog::spdlog
    )
    target_compile_definitions(xpbd_baker_app PRIVATE SPDLOG_COMPILED_LIB)
    # GUI subsystem: no console window on Windows (custom WinMain in main_sdl3.cpp).
    if(WIN32)
        set_target_properties(xpbd_baker_app PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    if(XPBD_GFX_OPENGL)
        target_compile_definitions(xpbd_baker_app PRIVATE XPBD_GFX_OPENGL=1)
        if(WIN32)
            target_link_libraries(xpbd_baker_app PRIVATE opengl32)
        elseif(APPLE)
            find_library(XPBD_OPENGL_FRAMEWORK OpenGL REQUIRED)
            target_link_libraries(xpbd_baker_app PRIVATE ${XPBD_OPENGL_FRAMEWORK})
        else()
            find_package(OpenGL REQUIRED)
            target_link_libraries(xpbd_baker_app PRIVATE OpenGL::GL)
        endif()
    endif()
    if(XPBD_GFX_VULKAN)
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            target_compile_definitions(xpbd_baker_app PRIVATE XPBD_GFX_VULKAN=1)
            target_link_libraries(xpbd_baker_app PRIVATE Vulkan::Vulkan)
            message(STATUS "xpbd_baker: Vulkan package found (backend may still be stub)")
        else()
            message(STATUS "xpbd_baker: Vulkan package not found; OpenGL-only app build")
        endif()
    endif()
    if(WIN32)
        target_link_libraries(xpbd_baker_app PRIVATE comdlg32 d3d11 dxgi d3dcompiler)
        target_compile_definitions(xpbd_baker_app PRIVATE XPBD_GFX_DX11=1)
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
    foreach(_xpbd_target IN ITEMS xpbd_cli xpbd_perf_benchmark)
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
    "xpbd_baker: cli=${XPBD_BUILD_CLI} "
    "benchmarks=${XPBD_BUILD_BENCHMARKS} app=${XPBD_BUILD_APP}")
