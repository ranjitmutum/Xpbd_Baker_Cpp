# Hint CMake's FindVulkan toward VULKAN_SDK, common install trees, or vcpkg.
# Safe to include multiple times; never forces Vulkan on if not present.

if(DEFINED XPBD_VULKAN_HINTS_INCLUDED)
    return()
endif()
set(XPBD_VULKAN_HINTS_INCLUDED TRUE)

function(xpbd_probe_vulkan_sdk)
    if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}")
        set(XPBD_VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}" PARENT_SCOPE)
        return()
    endif()
    if(DEFINED ENV{VK_SDK_PATH} AND EXISTS "$ENV{VK_SDK_PATH}")
        set(XPBD_VULKAN_SDK_ROOT "$ENV{VK_SDK_PATH}" PARENT_SCOPE)
        return()
    endif()

    set(_roots
        "C:/VulkanSDK"
        "D:/VulkanSDK"
        "C:/Libs/VulkanSDK"
        "D:/Libs/VulkanSDK"
        "$ENV{ProgramFiles}/VulkanSDK"
        "$ENV{ProgramFiles\(x86\)}/VulkanSDK"
    )
    foreach(_root IN LISTS _roots)
        if(NOT EXISTS "${_root}")
            continue()
        endif()
        file(GLOB _versions LIST_DIRECTORIES true "${_root}/*")
        list(SORT _versions ORDER DESCENDING)
        foreach(_ver IN LISTS _versions)
            if(EXISTS "${_ver}/Include/vulkan/vulkan.h" OR EXISTS "${_ver}/include/vulkan/vulkan.h")
                set(XPBD_VULKAN_SDK_ROOT "${_ver}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
endfunction()

xpbd_probe_vulkan_sdk()

if(DEFINED XPBD_VULKAN_SDK_ROOT)
    set(ENV{VULKAN_SDK} "${XPBD_VULKAN_SDK_ROOT}")
    list(PREPEND CMAKE_PREFIX_PATH "${XPBD_VULKAN_SDK_ROOT}")
    if(EXISTS "${XPBD_VULKAN_SDK_ROOT}/Include")
        set(Vulkan_INCLUDE_DIR "${XPBD_VULKAN_SDK_ROOT}/Include" CACHE PATH "Vulkan include" FORCE)
    elseif(EXISTS "${XPBD_VULKAN_SDK_ROOT}/include")
        set(Vulkan_INCLUDE_DIR "${XPBD_VULKAN_SDK_ROOT}/include" CACHE PATH "Vulkan include" FORCE)
    endif()
    message(STATUS "xpbd_baker: VULKAN_SDK hinted at ${XPBD_VULKAN_SDK_ROOT}")
else()
    message(STATUS "xpbd_baker: system Vulkan SDK not found; relying on vcpkg vulkan-headers/loader if installed")
endif()
