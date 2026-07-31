# Optional NVIDIA RTX Path Tracing (RTXPT) discovery.
# https://github.com/NVIDIA-RTX/RTXPT
#
# RTXPT is a Donut/NVRHI sample stack, not a small linkable SDK. This module only
# locates a local tree and exposes XPBD_WITH_RTXPT / XPBD_RTXPT_ROOT for the app
# bridge. Full path-tracer linkage is staged (see docs/rtxpt_integration.md).

option(XPBD_WITH_RTXPT "Enable NVIDIA RTXPT integration bridge (requires fetched tree)" OFF)
set(XPBD_RTXPT_ROOT "" CACHE PATH "Path to cloned NVIDIA-RTX/RTXPT repository")

set(XPBD_RTXPT_FOUND FALSE)
set(XPBD_RTXPT_VERSION "unknown")

if(XPBD_WITH_RTXPT)
    if(NOT XPBD_RTXPT_ROOT OR XPBD_RTXPT_ROOT STREQUAL "")
        set(_xpbd_rtxpt_default "${CMAKE_CURRENT_SOURCE_DIR}/third_party/RTXPT")
        if(EXISTS "${_xpbd_rtxpt_default}/CMakeLists.txt")
            set(XPBD_RTXPT_ROOT "${_xpbd_rtxpt_default}" CACHE PATH
                "Path to cloned NVIDIA-RTX/RTXPT repository" FORCE)
        endif()
    endif()

    if(XPBD_RTXPT_ROOT AND EXISTS "${XPBD_RTXPT_ROOT}/CMakeLists.txt"
       AND EXISTS "${XPBD_RTXPT_ROOT}/Rtxpt")
        set(XPBD_RTXPT_FOUND TRUE)
        # Best-effort version from README title line.
        if(EXISTS "${XPBD_RTXPT_ROOT}/README.md")
            file(STRINGS "${XPBD_RTXPT_ROOT}/README.md" _xpbd_rtxpt_readme LIMIT_COUNT 5)
            foreach(_line IN LISTS _xpbd_rtxpt_readme)
                if(_line MATCHES "RTX Path Tracing v([0-9.]+)")
                    set(XPBD_RTXPT_VERSION "${CMAKE_MATCH_1}")
                    break()
                endif()
            endforeach()
        endif()
        message(STATUS "xpbd_baker: RTXPT found at ${XPBD_RTXPT_ROOT} (v${XPBD_RTXPT_VERSION})")
        message(STATUS "xpbd_baker: RTXPT full viewport path-trace is staged; see docs/rtxpt_integration.md")
    else()
        message(WARNING
            "XPBD_WITH_RTXPT=ON but sample tree not found.\n"
            "  Developers: powershell -File tools/vendor_rtxpt.ps1\n"
            "  Or set XPBD_RTXPT_ROOT to a local clone of NVIDIA-RTX/RTXPT")
    endif()
endif()
