# Optional NVIDIA Streamline SDK.
#
# The SDK is never downloaded implicitly. Point XPBD_STREAMLINE_SDK_ROOT at an
# official Streamline 2.12.0 release tree. Runtime capabilities are resolved
# per feature; only the strict formal gate requires the complete target set.

include("${CMAKE_CURRENT_LIST_DIR}/StreamlineManifest.cmake")

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

xpbd_streamline_known_manifest(_xpbd_streamline_known_manifest)
set(_xpbd_streamline_present_manifest "")
if(XPBD_STREAMLINE_SDK_ROOT)
    foreach(_xpbd_streamline_relative IN LISTS
            _xpbd_streamline_known_manifest)
        if(EXISTS
           "${XPBD_STREAMLINE_SDK_ROOT}/${_xpbd_streamline_relative}")
            list(APPEND _xpbd_streamline_present_manifest
                "${_xpbd_streamline_relative}")
        endif()
    endforeach()
endif()
xpbd_streamline_resolve_manifest(
    "${_xpbd_streamline_present_manifest}")

set(XPBD_STREAMLINE_RUNTIME_FILES "")
if(XPBD_STREAMLINE_FOUND)
    foreach(_xpbd_streamline_relative IN ITEMS
            bin/x64/sl.interposer.dll
            bin/x64/sl.common.dll)
        list(APPEND XPBD_STREAMLINE_RUNTIME_FILES
            "${XPBD_STREAMLINE_SDK_ROOT}/${_xpbd_streamline_relative}")
    endforeach()
    if(XPBD_STREAMLINE_SR_FOUND)
        foreach(_xpbd_streamline_relative IN ITEMS
                bin/x64/sl.dlss.dll
                bin/x64/nvngx_dlss.dll)
            list(APPEND XPBD_STREAMLINE_RUNTIME_FILES
                "${XPBD_STREAMLINE_SDK_ROOT}/${_xpbd_streamline_relative}")
        endforeach()
    endif()
    if(XPBD_STREAMLINE_RR_FOUND)
        foreach(_xpbd_streamline_relative IN ITEMS
                bin/x64/sl.dlss_d.dll
                bin/x64/nvngx_dlssd.dll)
            list(APPEND XPBD_STREAMLINE_RUNTIME_FILES
                "${XPBD_STREAMLINE_SDK_ROOT}/${_xpbd_streamline_relative}")
        endforeach()
    endif()
    if(XPBD_STREAMLINE_FG_FOUND)
        foreach(_xpbd_streamline_relative IN ITEMS
                bin/x64/sl.dlss_g.dll
                bin/x64/nvngx_dlssg.dll)
            list(APPEND XPBD_STREAMLINE_RUNTIME_FILES
                "${XPBD_STREAMLINE_SDK_ROOT}/${_xpbd_streamline_relative}")
        endforeach()
    endif()
    if(XPBD_STREAMLINE_REFLEX_FOUND)
        foreach(_xpbd_streamline_relative IN ITEMS
                bin/x64/sl.reflex.dll
                bin/x64/NvLowLatencyVk.dll)
            list(APPEND XPBD_STREAMLINE_RUNTIME_FILES
                "${XPBD_STREAMLINE_SDK_ROOT}/${_xpbd_streamline_relative}")
        endforeach()
    endif()
    if(XPBD_STREAMLINE_PCL_FOUND)
        list(APPEND XPBD_STREAMLINE_RUNTIME_FILES
            "${XPBD_STREAMLINE_SDK_ROOT}/bin/x64/sl.pcl.dll")
    endif()
    message(STATUS
        "xpbd_baker: Streamline 2.12 capabilities at "
        "${XPBD_STREAMLINE_SDK_ROOT}: "
        "SR=${XPBD_STREAMLINE_SR_FOUND} "
        "RR=${XPBD_STREAMLINE_RR_FOUND} "
        "FG=${XPBD_STREAMLINE_FG_FOUND} "
        "Reflex=${XPBD_STREAMLINE_REFLEX_FOUND} "
        "PCL=${XPBD_STREAMLINE_PCL_FOUND}")
elseif(XPBD_STREAMLINE_SDK_ROOT)
    message(WARNING
        "xpbd_baker: Streamline core or all independent features are "
        "unavailable; bridge disabled. Core missing: "
        "${XPBD_STREAMLINE_CORE_MISSING}")
endif()
