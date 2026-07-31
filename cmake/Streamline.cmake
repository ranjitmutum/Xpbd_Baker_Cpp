# Optional NVIDIA Streamline SDK.
#
# The SDK is never downloaded implicitly. Point XPBD_STREAMLINE_SDK_ROOT at an
# official Streamline release tree containing include/, bin/x64/ and license/.
# Only signed production binaries from bin/x64 are copied into the application.

set(XPBD_STREAMLINE_SDK_ROOT "" CACHE PATH
    "Official NVIDIA Streamline SDK 2.12.0 release root")

if(NOT XPBD_STREAMLINE_SDK_ROOT)
    set(_xpbd_streamline_local_root
        "${CMAKE_CURRENT_LIST_DIR}/../.tmp/streamline-sdk")
    if(EXISTS "${_xpbd_streamline_local_root}/include/sl.h")
        get_filename_component(XPBD_STREAMLINE_SDK_ROOT
            "${_xpbd_streamline_local_root}" ABSOLUTE)
    endif()
endif()

set(XPBD_STREAMLINE_FOUND FALSE)
if(XPBD_STREAMLINE_SDK_ROOT)
    set(_xpbd_streamline_required
        "${XPBD_STREAMLINE_SDK_ROOT}/include/sl.h"
        "${XPBD_STREAMLINE_SDK_ROOT}/include/sl_security.h"
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
        "${XPBD_STREAMLINE_SDK_ROOT}/license.txt"
        "${XPBD_STREAMLINE_SDK_ROOT}/3rd-party-licenses.md"
        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/reflex.license.txt"
        "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/nvngx_dlss.license.txt")
    set(_xpbd_streamline_missing "")
    foreach(_xpbd_streamline_path IN LISTS _xpbd_streamline_required)
        if(NOT EXISTS "${_xpbd_streamline_path}")
            list(APPEND _xpbd_streamline_missing
                "${_xpbd_streamline_path}")
        endif()
    endforeach()
    if(_xpbd_streamline_missing)
        message(WARNING
            "xpbd_baker: Streamline SDK root is incomplete; DLSS disabled. "
            "Missing: ${_xpbd_streamline_missing}")
    else()
        set(XPBD_STREAMLINE_FOUND TRUE)
        message(STATUS
            "xpbd_baker: NVIDIA Streamline SDK found at "
            "${XPBD_STREAMLINE_SDK_ROOT}")
    endif()
endif()
