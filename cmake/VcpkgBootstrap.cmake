# Shared vcpkg wiring for CLI (build.bat), CLion, and other IDEs.
# Include BEFORE project() so the toolchain can run on the first configure.

# ---------------------------------------------------------------------------
# Toolchain: auto-select vcpkg when the IDE omits CMAKE_TOOLCHAIN_FILE
# ---------------------------------------------------------------------------
if(NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT}
       AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "vcpkg CMake toolchain file" FORCE)
        message(STATUS "xpbd_baker: using vcpkg toolchain from VCPKG_ROOT")
    elseif(DEFINED ENV{VCPKG_ROOT})
        message(WARNING
            "VCPKG_ROOT is set but toolchain file is missing: "
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    else()
        message(WARNING
            "VCPKG_ROOT is not set and CMAKE_TOOLCHAIN_FILE is empty. "
            "find_package(nlohmann_json) will fail. "
            "Set VCPKG_ROOT or pass -DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake")
    endif()
endif()

# ---------------------------------------------------------------------------
# One installed tree for ALL binary dirs (build/, cmake-build-debug/, out/...).
# Without this, each -B directory re-runs "vcpkg install" into its own
# <build>/vcpkg_installed and duplicates multi-hundred-MB artifacts.
# ---------------------------------------------------------------------------
if(NOT VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../vcpkg_installed"
        CACHE PATH "Shared vcpkg install prefix (outside any binary dir)")
endif()
# Normalize to absolute path for multi-config / nested generators.
get_filename_component(_xpbd_vcpkg_installed "${VCPKG_INSTALLED_DIR}" ABSOLUTE)
set(VCPKG_INSTALLED_DIR "${_xpbd_vcpkg_installed}"
    CACHE PATH "Shared vcpkg install prefix (outside any binary dir)" FORCE)
unset(_xpbd_vcpkg_installed)

# Default triplet matches MSVC + build.bat (not MinGW).
if(NOT VCPKG_TARGET_TRIPLET AND WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg target triplet")
endif()

# Manifest mode only — never require classic `vcpkg install` into VCPKG_ROOT.
set(VCPKG_MANIFEST_MODE ON CACHE BOOL "Use vcpkg.json next to this project" FORCE)

message(STATUS "xpbd_baker: VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}")
message(STATUS "xpbd_baker: VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}")
