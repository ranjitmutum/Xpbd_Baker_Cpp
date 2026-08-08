cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/../StreamlineManifest.cmake")

function(xpbd_expect_capabilities name manifest core sr rr fg reflex pcl
         found complete)
    xpbd_streamline_resolve_manifest("${manifest}")
    foreach(_xpbd_field IN ITEMS CORE SR RR FG REFLEX PCL FOUND
                                  COMPLETE_FOUND)
        if(_xpbd_field STREQUAL "FOUND")
            set(_xpbd_actual "${XPBD_STREAMLINE_FOUND}")
            set(_xpbd_expected "${found}")
        elseif(_xpbd_field STREQUAL "COMPLETE_FOUND")
            set(_xpbd_actual "${XPBD_STREAMLINE_COMPLETE_FOUND}")
            set(_xpbd_expected "${complete}")
        else()
            string(TOLOWER "${_xpbd_field}" _xpbd_expected_name)
            set(_xpbd_actual "${XPBD_STREAMLINE_${_xpbd_field}_FOUND}")
            set(_xpbd_expected "${${_xpbd_expected_name}}")
        endif()
        if(NOT "${_xpbd_actual}" STREQUAL "${_xpbd_expected}")
            message(FATAL_ERROR
                "${name}: ${_xpbd_field} expected ${_xpbd_expected}, "
                "got ${_xpbd_actual}")
        endif()
    endforeach()
endfunction()

xpbd_streamline_known_manifest(_xpbd_full)
xpbd_expect_capabilities(full "${_xpbd_full}"
    TRUE TRUE TRUE TRUE TRUE TRUE TRUE TRUE)

set(_xpbd_no_rr "${_xpbd_full}")
list(REMOVE_ITEM _xpbd_no_rr
    bin/x64/sl.dlss_d.dll bin/x64/nvngx_dlssd.dll)
xpbd_expect_capabilities(no_rr "${_xpbd_no_rr}"
    TRUE TRUE FALSE TRUE TRUE TRUE TRUE FALSE)

set(_xpbd_no_fg "${_xpbd_full}")
list(REMOVE_ITEM _xpbd_no_fg
    bin/x64/sl.dlss_g.dll bin/x64/nvngx_dlssg.dll)
xpbd_expect_capabilities(no_fg "${_xpbd_no_fg}"
    TRUE TRUE TRUE FALSE TRUE TRUE TRUE FALSE)

set(_xpbd_no_reflex "${_xpbd_full}")
list(REMOVE_ITEM _xpbd_no_reflex
    bin/x64/sl.reflex.dll bin/x64/NvLowLatencyVk.dll)
xpbd_expect_capabilities(no_reflex "${_xpbd_no_reflex}"
    TRUE TRUE TRUE FALSE FALSE TRUE TRUE FALSE)

set(_xpbd_no_pcl "${_xpbd_full}")
list(REMOVE_ITEM _xpbd_no_pcl bin/x64/sl.pcl.dll)
xpbd_expect_capabilities(no_pcl "${_xpbd_no_pcl}"
    TRUE TRUE TRUE TRUE TRUE FALSE TRUE FALSE)

set(_xpbd_no_sr "${_xpbd_full}")
list(REMOVE_ITEM _xpbd_no_sr
    bin/x64/sl.dlss.dll bin/x64/nvngx_dlss.dll)
xpbd_expect_capabilities(no_sr "${_xpbd_no_sr}"
    TRUE FALSE FALSE TRUE TRUE TRUE TRUE FALSE)

set(_xpbd_no_core "${_xpbd_full}")
list(REMOVE_ITEM _xpbd_no_core bin/x64/sl.common.dll)
xpbd_expect_capabilities(no_core "${_xpbd_no_core}"
    FALSE FALSE FALSE FALSE FALSE FALSE FALSE FALSE)

message(STATUS "Streamline constructed-manifest capability tests passed")
