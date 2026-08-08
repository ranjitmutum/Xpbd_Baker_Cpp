include_guard(GLOBAL)

# The bridge includes the complete public 2.12 feature ABI from one official
# SDK include tree. Runtime binaries remain feature-scoped below so a missing
# optional plugin never disables an otherwise complete independent feature.
set(_XPBD_STREAMLINE_CORE_REQUIRED
    include/sl.h
    include/sl_security.h
    include/sl_dlss.h
    include/sl_dlss_d.h
    include/sl_dlss_g.h
    include/sl_helpers.h
    include/sl_matrix_helpers.h
    include/sl_pcl.h
    include/sl_reflex.h
    bin/x64/sl.interposer.dll
    bin/x64/sl.common.dll
    license.txt
    3rd-party-licenses.md)

set(_XPBD_STREAMLINE_SR_REQUIRED
    bin/x64/sl.dlss.dll
    bin/x64/nvngx_dlss.dll
    bin/x64/nvngx_dlss.license.txt)

set(_XPBD_STREAMLINE_RR_REQUIRED
    bin/x64/sl.dlss_d.dll
    bin/x64/nvngx_dlssd.dll
    bin/x64/nvngx_dlss.license.txt)

set(_XPBD_STREAMLINE_FG_REQUIRED
    bin/x64/sl.dlss_g.dll
    bin/x64/nvngx_dlssg.dll
    bin/x64/nvngx_dlss.license.txt)

set(_XPBD_STREAMLINE_REFLEX_REQUIRED
    bin/x64/sl.reflex.dll
    bin/x64/NvLowLatencyVk.dll
    bin/x64/reflex.license.txt)

set(_XPBD_STREAMLINE_PCL_REQUIRED
    bin/x64/sl.pcl.dll)

function(xpbd_streamline_known_manifest output_variable)
    set(_xpbd_manifest
        ${_XPBD_STREAMLINE_CORE_REQUIRED}
        ${_XPBD_STREAMLINE_SR_REQUIRED}
        ${_XPBD_STREAMLINE_RR_REQUIRED}
        ${_XPBD_STREAMLINE_FG_REQUIRED}
        ${_XPBD_STREAMLINE_REFLEX_REQUIRED}
        ${_XPBD_STREAMLINE_PCL_REQUIRED})
    list(REMOVE_DUPLICATES _xpbd_manifest)
    set(${output_variable} "${_xpbd_manifest}" PARENT_SCOPE)
endfunction()

function(xpbd_streamline_resolve_manifest manifest)
    set(_xpbd_manifest "${manifest}")
    foreach(_xpbd_group IN ITEMS CORE SR RR FG REFLEX PCL)
        set(_xpbd_found TRUE)
        set(_xpbd_missing "")
        set(_xpbd_required_variable
            "_XPBD_STREAMLINE_${_xpbd_group}_REQUIRED")
        foreach(_xpbd_path IN LISTS ${_xpbd_required_variable})
            if(NOT _xpbd_path IN_LIST _xpbd_manifest)
                set(_xpbd_found FALSE)
                list(APPEND _xpbd_missing "${_xpbd_path}")
            endif()
        endforeach()
        set(_xpbd_${_xpbd_group}_FILES_FOUND ${_xpbd_found})
        set(XPBD_STREAMLINE_${_xpbd_group}_MISSING
            "${_xpbd_missing}" PARENT_SCOPE)
        set(XPBD_STREAMLINE_${_xpbd_group}_FILES_FOUND
            ${_xpbd_found} PARENT_SCOPE)
    endforeach()

    set(_xpbd_core ${_xpbd_CORE_FILES_FOUND})
    set(_xpbd_sr FALSE)
    set(_xpbd_rr FALSE)
    set(_xpbd_fg FALSE)
    set(_xpbd_reflex FALSE)
    set(_xpbd_pcl FALSE)
    if(_xpbd_core AND _xpbd_SR_FILES_FOUND)
        set(_xpbd_sr TRUE)
    endif()
    # XPBD's RR integration calls the companion SR options API as required by
    # the bundled DLSSD contract, so RR additionally depends on a valid SR set.
    if(_xpbd_sr AND _xpbd_RR_FILES_FOUND)
        set(_xpbd_rr TRUE)
    endif()
    if(_xpbd_core AND _xpbd_REFLEX_FILES_FOUND)
        set(_xpbd_reflex TRUE)
    endif()
    if(_xpbd_core AND _xpbd_PCL_FILES_FOUND)
        set(_xpbd_pcl TRUE)
    endif()
    # DLSS-G requires Reflex at runtime. Keep PCL independently diagnosable.
    if(_xpbd_core AND _xpbd_FG_FILES_FOUND AND _xpbd_reflex)
        set(_xpbd_fg TRUE)
    endif()

    set(_xpbd_any_feature FALSE)
    if(_xpbd_sr OR _xpbd_rr OR _xpbd_fg OR _xpbd_reflex OR _xpbd_pcl)
        set(_xpbd_any_feature TRUE)
    endif()
    set(_xpbd_found FALSE)
    if(_xpbd_core AND _xpbd_any_feature)
        set(_xpbd_found TRUE)
    endif()
    set(_xpbd_complete FALSE)
    if(_xpbd_core AND _xpbd_sr AND _xpbd_rr AND _xpbd_fg AND
       _xpbd_reflex AND _xpbd_pcl)
        set(_xpbd_complete TRUE)
    endif()

    set(XPBD_STREAMLINE_CORE_FOUND ${_xpbd_core} PARENT_SCOPE)
    set(XPBD_STREAMLINE_SR_FOUND ${_xpbd_sr} PARENT_SCOPE)
    set(XPBD_STREAMLINE_RR_FOUND ${_xpbd_rr} PARENT_SCOPE)
    set(XPBD_STREAMLINE_FG_FOUND ${_xpbd_fg} PARENT_SCOPE)
    set(XPBD_STREAMLINE_REFLEX_FOUND ${_xpbd_reflex} PARENT_SCOPE)
    set(XPBD_STREAMLINE_PCL_FOUND ${_xpbd_pcl} PARENT_SCOPE)
    set(XPBD_STREAMLINE_FOUND ${_xpbd_found} PARENT_SCOPE)
    set(XPBD_STREAMLINE_COMPLETE_FOUND ${_xpbd_complete} PARENT_SCOPE)
endfunction()
