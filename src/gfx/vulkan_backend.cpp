#include "vulkan/vulkan_backend_internal.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/preview_scene.hpp"
#include "xpbd/gfx/ray_tracing.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"
#include "xpbd/gfx/streamline_vulkan_runtime.hpp"
#include "xpbd/gfx/vulkan_path_tracer.hpp"
#include "xpbd/gfx/vulkan_queue_selection.hpp"
#include "xpbd/gfx/vulkan_rt_scene.hpp"
#include "xpbd/log.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_BOOL
#include "nuklear.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace xpbd::gfx {
namespace {

#define VK_CHECK(x)                                                            \
  do {                                                                         \
    VkResult _r = (x);                                                         \
    if (_r != VK_SUCCESS) {                                                    \
      SDL_Log("Vulkan error %d at %s:%d", (int)_r, __FILE__, __LINE__);        \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define VK_CHECK_VOID(x)                                                       \
  do {                                                                         \
    VkResult _r = (x);                                                         \
    if (_r != VK_SUCCESS) {                                                    \
      SDL_Log("Vulkan error %d at %s:%d", (int)_r, __FILE__, __LINE__);        \
    }                                                                          \
  } while (0)



constexpr std::uint64_t kDiagnosticWaitSliceNs = 250'000'000ull;
constexpr auto kSwapchainRecreateRetryDelay = std::chrono::milliseconds(100);

bool environmentFlagEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr && value[0] != '\0' &&
         std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "FALSE") != 0;
}

constexpr bool frameGenerationColorFormatSupported(
    VkFormat format) noexcept {
  // The current FG UI guide uses the swapchain format. Keep this list to
  // 8-bit RGBA/BGRA formats so UI alpha has enough precision and so the
  // unsupported FP16/scRGB path can never be advertised as available.
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
    return true;
  default:
    return false;
  }
}









std::uint64_t diagnosticTimestampUs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::uint64_t diagnosticThreadId() {
  return static_cast<std::uint64_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

template <typename Handle> std::uint64_t diagnosticHandle(Handle handle) {
  if constexpr (std::is_pointer_v<Handle>) {
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(handle));
  } else {
    return static_cast<std::uint64_t>(handle);
  }
}



VKAPI_ATTR VkBool32 VKAPI_CALL
vulkanValidationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                         VkDebugUtilsMessageTypeFlagsEXT,
                         const VkDebugUtilsMessengerCallbackDataEXT *data,
                         void *) {
  const char *message_id =
      data != nullptr && data->pMessageIdName != nullptr
          ? data->pMessageIdName
          : "<no-id>";
  const char *message =
      data != nullptr && data->pMessage != nullptr ? data->pMessage
                                                  : "<no-message>";
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
    xpbd::log::errorf("Vulkan validation [%s]: %s", message_id, message);
  } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) !=
             0) {
    xpbd::log::warnf("Vulkan validation [%s]: %s", message_id, message);
  } else {
    xpbd::log::infof("Vulkan validation [%s]: %s", message_id, message);
  }
  return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT validationMessengerCreateInfo() {
  VkDebugUtilsMessengerCreateInfoEXT info{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  info.pfnUserCallback = vulkanValidationCallback;
  return info;
}





















































// Unit cube (triangle list, 36 verts) for skybox sampling.


// Unit cube (triangle list, 36 verts) for skybox sampling.








}

namespace detail {


const char *VulkanBackend::vkResultName(VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return "VK_SUCCESS";
  case VK_NOT_READY:
    return "VK_NOT_READY";
  case VK_TIMEOUT:
    return "VK_TIMEOUT";
  case VK_EVENT_SET:
    return "VK_EVENT_SET";
  case VK_EVENT_RESET:
    return "VK_EVENT_RESET";
  case VK_INCOMPLETE:
    return "VK_INCOMPLETE";
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED:
    return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST:
    return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_MEMORY_MAP_FAILED:
    return "VK_ERROR_MEMORY_MAP_FAILED";
  case VK_ERROR_LAYER_NOT_PRESENT:
    return "VK_ERROR_LAYER_NOT_PRESENT";
  case VK_ERROR_EXTENSION_NOT_PRESENT:
    return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT:
    return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER:
    return "VK_ERROR_INCOMPATIBLE_DRIVER";
  case VK_ERROR_TOO_MANY_OBJECTS:
    return "VK_ERROR_TOO_MANY_OBJECTS";
  case VK_ERROR_FORMAT_NOT_SUPPORTED:
    return "VK_ERROR_FORMAT_NOT_SUPPORTED";
  case VK_ERROR_FRAGMENTED_POOL:
    return "VK_ERROR_FRAGMENTED_POOL";
  case VK_ERROR_SURFACE_LOST_KHR:
    return "VK_ERROR_SURFACE_LOST_KHR";
  case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
    return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
  case VK_SUBOPTIMAL_KHR:
    return "VK_SUBOPTIMAL_KHR";
  case VK_ERROR_OUT_OF_DATE_KHR:
    return "VK_ERROR_OUT_OF_DATE_KHR";
  case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
    return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
  case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT:
    return "VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT";
  default:
    return "VK_RESULT_UNKNOWN";
  }
}

void VulkanBackend::writeLog(const char *msg) { xpbd::log::info(msg); }

void VulkanBackend::appendPathTraceHistoryBytes(
    std::uint64_t &hash, const void *data, std::size_t byte_count) {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  for (std::size_t index = 0; index < byte_count; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
}





bool VulkanBackend::init(SDL_Window *window) {
    window_ = window;
    diagnostics_enabled_ =
        environmentFlagEnabled("XPBD_VULKAN_DIAGNOSTICS");
    perf_diagnostics_enabled_ =
        diagnostics_enabled_ ||
        environmentFlagEnabled("XPBD_PERF_DIAGNOSTICS");
    validation_requested_ =
        environmentFlagEnabled("XPBD_VULKAN_VALIDATION");
    validation_enabled_ = false;
    writeLog("VulkanBackend::init");
    (void)streamline_vulkan_runtime_.initializeBeforeVulkan();

    uint32_t ext_count = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts || ext_count == 0) {
      writeLog("No SDL Vulkan instance extensions");
      return false;
    }
    std::vector<const char *> instance_exts(exts, exts + ext_count);

    std::uint32_t available_extension_count = 0;
    VkResult available_extension_result =
        vkEnumerateInstanceExtensionProperties(
            nullptr, &available_extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions;
    if (available_extension_result == VK_SUCCESS &&
        available_extension_count > 0) {
      available_extensions.resize(available_extension_count);
      available_extension_result = vkEnumerateInstanceExtensionProperties(
          nullptr, &available_extension_count, available_extensions.data());
      if (available_extension_result == VK_SUCCESS) {
        available_extensions.resize(available_extension_count);
      } else {
        available_extensions.clear();
      }
    }
    const auto extension_available =
        [&](const char *extension_name) {
          return std::any_of(
              available_extensions.begin(), available_extensions.end(),
              [extension_name](const VkExtensionProperties &property) {
                return std::strcmp(property.extensionName, extension_name) ==
                       0;
              });
        };
    const auto append_instance_extension =
        [&](const char *extension_name) {
          if (!extension_available(extension_name)) {
            return false;
          }
          const bool already_enabled =
              std::any_of(instance_exts.begin(), instance_exts.end(),
                          [extension_name](const char *enabled_name) {
                            return std::strcmp(enabled_name, extension_name) ==
                                   0;
                          });
          if (!already_enabled) {
            instance_exts.push_back(extension_name);
          }
          return true;
        };
    if (append_instance_extension(
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
      surface_maintenance1_khr_enabled_ = append_instance_extension(
          VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
      surface_maintenance1_ext_enabled_ = append_instance_extension(
          VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }

    std::uint32_t layer_count = 0;
    VkResult layer_result =
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers;
    if (layer_result == VK_SUCCESS && layer_count > 0) {
      available_layers.resize(layer_count);
      layer_result =
          vkEnumerateInstanceLayerProperties(&layer_count,
                                             available_layers.data());
      if (layer_result == VK_SUCCESS) {
        available_layers.resize(layer_count);
      } else {
        available_layers.clear();
      }
    }

    constexpr const char *kValidationLayer =
        "VK_LAYER_KHRONOS_validation";
    const bool validation_layer_available =
        layer_result == VK_SUCCESS &&
        std::any_of(available_layers.begin(), available_layers.end(),
                    [](const VkLayerProperties &layer) {
                      return std::strcmp(layer.layerName,
                                         kValidationLayer) == 0;
                    });
    const bool debug_utils_available =
        extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation_requested_) {
      if (validation_layer_available && debug_utils_available) {
        validation_enabled_ =
            append_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        xpbd::log::infof(
            "Vulkan validation requested: enabled layer=%s debug_utils=%d",
            kValidationLayer, validation_enabled_ ? 1 : 0);
      } else {
        xpbd::log::warnf(
            "Vulkan validation requested but unavailable: layer=%d "
            "debug_utils=%d; continuing without validation",
            validation_layer_available ? 1 : 0,
            debug_utils_available ? 1 : 0);
      }
    }

    if (diagnostics_enabled_) {
      xpbd::log::infof(
          "VKDIAG config ts_us=%llu thread=%llu enabled=1 "
          "validation_requested=%d application_enabled_layers=%u "
          "instance_extensions=%u wait_slice_ms=250",
          static_cast<unsigned long long>(diagnosticTimestampUs()),
          static_cast<unsigned long long>(diagnosticThreadId()),
          validation_requested_ ? 1 : 0, validation_enabled_ ? 1u : 0u,
          static_cast<unsigned>(instance_exts.size()));
      for (const char *extension : instance_exts) {
        xpbd::log::infof("VKDIAG instance_extension name=%s",
                         extension != nullptr ? extension : "<null>");
      }
      xpbd::log::infof(
          "VKDIAG available_instance_layers result=%s(%d) count=%u",
          vkResultName(layer_result), static_cast<int>(layer_result),
          layer_result == VK_SUCCESS ? layer_count : 0u);
      if (layer_result == VK_SUCCESS) {
        for (const auto &layer : available_layers) {
          xpbd::log::infof(
              "VKDIAG available_instance_layer name=%s spec=%u impl=%u",
              layer.layerName, layer.specVersion, layer.implementationVersion);
        }
      }
      xpbd::log::flush();
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "XPBD Bone Baker";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(instance_exts.size());
    ici.ppEnabledExtensionNames = instance_exts.data();
    const std::array<const char *, 1> enabled_layers = {kValidationLayer};
    VkDebugUtilsMessengerCreateInfoEXT validation_info{};
    if (validation_enabled_) {
      ici.enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size());
      ici.ppEnabledLayerNames = enabled_layers.data();
      validation_info = validationMessengerCreateInfo();
      ici.pNext = &validation_info;
    }
    VK_CHECK(streamline_vulkan_runtime_.createInstance(
        &ici, nullptr, &instance_));
    enabled_instance_extensions_.clear();
    enabled_instance_extensions_.reserve(instance_exts.size());
    for (const char *extension : instance_exts) {
      if (extension != nullptr) {
        enabled_instance_extensions_.emplace_back(extension);
      }
    }

    if (validation_enabled_) {
      const auto create_debug_messenger =
          reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
              vkGetInstanceProcAddr(instance_,
                                    "vkCreateDebugUtilsMessengerEXT"));
      if (create_debug_messenger == nullptr) {
        xpbd::log::warnf(
            "Vulkan validation layer enabled but debug messenger entry point "
            "is unavailable");
      } else {
        const VkResult messenger_result = create_debug_messenger(
            instance_, &validation_info, nullptr, &debug_messenger_);
        if (messenger_result != VK_SUCCESS) {
          xpbd::log::warnf(
              "Vulkan debug messenger creation failed: %s(%d)",
              vkResultName(messenger_result),
              static_cast<int>(messenger_result));
        }
      }
    }

    bool surface_created = false;
#if defined(_WIN32)
    void *native_window = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window_),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (streamline_vulkan_runtime_.initialized()) {
      const VkResult surface_result =
          streamline_vulkan_runtime_.createWin32Surface(
              instance_, native_window, nullptr, &surface_);
      surface_created = surface_result == VK_SUCCESS;
      if (!surface_created) {
        xpbd::log::warnf(
            "Streamline Win32 surface hook failed: %s(%d); "
            "disabling Streamline",
            vkResultName(surface_result),
            static_cast<int>(surface_result));
        streamline_vulkan_runtime_.shutdownBeforeVulkan();
      }
    }
#endif
    if (!surface_created) {
      surface_created =
          SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_);
    }
    if (!surface_created) {
      writeLog(SDL_GetError());
      return false;
    }

    if (!pickDevice()) {
      return false;
    }
    probeRayTracingCapability();
    if (!createDevice()) {
      return false;
    }
    streamline_vulkan_runtime_.inspectPhysicalDevice(phys_);
    if (rt_capability_.supported && rt_capability_.device_extensions_enabled) {
      bool rt_scenes_ready = true;
      for (auto &scene : rt_scenes_) {
        if (!scene.init(phys_, device_, graphics_family_, graphics_queue_)) {
          rt_scenes_ready = false;
          break;
        }
      }
      if (!rt_scenes_ready) {
        for (auto &scene : rt_scenes_) {
          scene.shutdown();
        }
        xpbd::log::warn(
            "Vulkan RT: scene helper init failed; RT shadows disabled");
        rt_capability_.supported = false;
        rt_capability_.device_extensions_enabled = false;
        rt_capability_.unsupported_reason =
            "Failed to initialize ray-tracing scene resources";
      } else {
        xpbd::log::infof(
            "Vulkan RT: NVIDIA RT ready on '%s' (max recursion %u). "
            "Preferred path: built-in Vulkan RT Pipeline",
            rt_capability_.device_name.c_str(),
            rt_capability_.max_ray_recursion_depth);
      }
    } else {
      xpbd::log::infof("Vulkan RT: unavailable — %s",
                       rt_capability_.unsupported_reason.empty()
                           ? "not supported"
                           : rt_capability_.unsupported_reason.c_str());
    }
    if (!createSwapchain()) {
      return false;
    }
    if (!createRenderPass()) {
      return false;
    }
    if (!createFramebuffers()) {
      return false;
    }
    if (!streamline_vulkan_runtime_.completeFrameGenerationSwapchainTransition(
            streamline_vulkan_runtime_.swapchainOwnership(),
            streamline_vulkan_runtime_.swapchainOwnership() ==
                    SwapchainOwnership::StreamlineFrameGenerationProxy &&
                fg_swapchain_resources_ready_,
            static_cast<std::uint64_t>(frame_index_),
            "initial native swapchain")) {
      return false;
    }
    if (!createDescriptors()) {
      return false;
    }
    if (!createPipelines()) {
      return false;
    }
    // The unified compute path is the preferred interactive RT renderer.
    // Pipeline creation failure retains raster materials + ray-query shadows.
    if (rt_capability_.device_extensions_enabled && render_pass_) {
      bool path_tracers_ready = true;
      for (auto &path_tracer : path_tracers_) {
        if (!path_tracer.init(
                phys_, device_, render_pass_, true,
                descriptor_binding_partially_bound_enabled_)) {
          path_tracers_ready = false;
          break;
        }
      }
      if (!path_tracers_ready) {
        for (auto &path_tracer : path_tracers_) {
          path_tracer.shutdown();
        }
        xpbd::log::warn(
            "Vulkan path tracer init failed; hybrid RT shadows still available");
      } else if (!still_path_tracer_.init(
                     phys_, device_, render_pass_, false,
                     descriptor_binding_partially_bound_enabled_)) {
        xpbd::log::warn(
            "Vulkan still-render path tracer init failed; viewport path "
            "tracing remains available");
      }
    }
    const bool path_tracer_ready =
        std::all_of(path_tracers_.begin(), path_tracers_.end(),
                    [](const VulkanPathTracer &path_tracer) {
                      return path_tracer.ready();
                    });
    const bool rt_pipeline_ready =
        path_tracer_ready &&
        std::all_of(path_tracers_.begin(), path_tracers_.end(),
                    [](const VulkanPathTracer &path_tracer) {
                      return path_tracer.rtPipelineReady();
                    });
    setVulkanPathTraceAvailability(path_tracer_ready, rt_pipeline_ready);
    if (!createBuffers()) {
      return false;
    }
    if (!createCommandPool()) {
      return false;
    }
    if (!createSync()) {
      return false;
    }
    createTimestampQueryPools();

    writeLog("VulkanBackend init OK");
    return true;
  }

void VulkanBackend::shutdown() {
    if (device_) {
      // Turn interpolation off before draining or destroying any presentation
      // object. The feature itself is unloaded after the swapchain is gone.
      const std::uint64_t shutdown_frame =
          static_cast<std::uint64_t>(frame_index_);
      const bool frame_generation_disable_started =
          streamline_vulkan_runtime_.beginFrameGenerationShutdown(
              shutdown_frame);
      bool frame_generation_disable_ready = false;
      if (!frame_generation_disable_started) {
        fg_recovery_reason_ =
            streamline_vulkan_runtime_.frameGenerationDiagnostic().status;
      }
      swapchain_recreate_target_ = SwapchainOwnership::Native;
      fg_force_native_recovery_ = true;
      for (std::uint32_t drain_cycle = 0u;
           !frame_generation_disable_ready &&
           drain_cycle <= kFrameGenerationDisableMaxAttempts;
           ++drain_cycle) {
        const FrameSync &sync = frames_[frame_index_];
        const auto idle_start = Clock::now();
        logDiagnosticApi("vkDeviceWaitIdle.shutdown", "before", std::nullopt,
                         0.0, UINT32_MAX, sync.fence, VK_NULL_HANDLE, sync.cmd,
                         true, true);
        const VkResult idle_result =
            streamline_vulkan_runtime_.deviceWaitIdle(device_);
        logDiagnosticApi(
            "vkDeviceWaitIdle.shutdown", "after", idle_result,
            std::chrono::duration<double, std::milli>(Clock::now() -
                                                      idle_start)
                .count(),
            UINT32_MAX, sync.fence, VK_NULL_HANDLE, sync.cmd, true, false);
        if (idle_result != VK_SUCCESS) {
          SDL_Log("Vulkan device idle wait during shutdown failed: %d",
                  static_cast<int>(idle_result));
          if (idle_result == VK_ERROR_DEVICE_LOST) {
            fatal_error_ = true;
            fatal_error_detail_ =
                "VK_ERROR_DEVICE_LOST while draining DLSS-G shutdown";
          }
          xpbd::log::error(
              "shutdown blocked by FG disable failure");
          return;
        }
        if (!waitForPendingPresentFences("shutdown")) {
          SDL_Log("Vulkan present completion wait during shutdown failed");
          xpbd::log::error(
              "shutdown blocked by FG disable failure");
          return;
        }
        frame_generation_disable_ready =
            streamline_vulkan_runtime_
                .retryFrameGenerationDisableAfterDrain(
                    shutdown_frame, "shutdown drain completed");
        if (!frame_generation_disable_ready &&
            streamline_vulkan_runtime_.frameGenerationDiagnostic()
                .disable_exhausted) {
          break;
        }
      }
      if (!frame_generation_disable_ready) {
        const FrameGenerationDiagnostic diagnostic =
            streamline_vulkan_runtime_.frameGenerationDiagnostic();
        fg_recovery_reason_ = diagnostic.status;
        xpbd::log::errorf(
            "shutdown blocked by FG disable failure: %s",
            diagnostic.status.c_str());
        return;
      }
    }
    if (!destroySwapchainObjects()) {
      xpbd::log::error("shutdown blocked by FG disable failure");
      return;
    }
    if (!streamline_vulkan_runtime_.unloadFrameGenerationForShutdown()) {
      xpbd::log::error(
          "shutdown blocked by FG disable failure: DLSS-G plugin remained "
          "loaded");
      return;
    }
    if (font_view_) {
      vkDestroyImageView(device_, font_view_, nullptr);
    }
    if (font_image_) {
      vkDestroyImage(device_, font_image_, nullptr);
    }
    if (font_mem_) {
      vkFreeMemory(device_, font_mem_, nullptr);
    }
    if (font_sampler_) {
      vkDestroySampler(device_, font_sampler_, nullptr);
    }
    destroyStaticModelResources();
    destroySkyboxGpu();
    for (auto &path_tracer : path_tracers_) {
      path_tracer.shutdown();
    }
    still_path_tracer_.shutdown();
    still_active_job_id_ = 0;
    still_path_trace_frame_index_ = 0;
    still_waiting_job_id_ = 0;
    still_wait_started_ = {};
    still_progress_job_id_ = 0;
    still_last_logged_samples_ = 0;
    still_last_progress_time_ = {};
    destroyProceduralAtmosphereGpu();
    destroyWorldEnvironmentGpu();
    setVulkanPathTraceAvailability(false, false);
    for (auto &scene : rt_scenes_) {
      scene.shutdown();
    }
    rt_scene_built_.fill(false);
    last_rt_scene_hash_.fill(0);
    rt_fallback_generation_serial_ = 0;
    last_mesh_rt_descriptor_sets_ = {};
    last_static_rt_descriptor_sets_ = {};
    last_mesh_rt_tlas_ = {};
    last_static_rt_tlas_ = {};
    destroyStaticMaterialSamplers();
    if (static_desc_pool_) {
      vkDestroyDescriptorPool(device_, static_desc_pool_, nullptr);
      static_desc_pool_ = VK_NULL_HANDLE;
    }
    if (static_desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, static_desc_layout_, nullptr);
      static_desc_layout_ = VK_NULL_HANDLE;
    }
    if (static_rt_desc_pool_) {
      vkDestroyDescriptorPool(device_, static_rt_desc_pool_, nullptr);
      static_rt_desc_pool_ = VK_NULL_HANDLE;
      static_rt_descriptor_sets_ = {};
    }
    if (static_rt_desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, static_rt_desc_layout_, nullptr);
      static_rt_desc_layout_ = VK_NULL_HANDLE;
    }
    if (mesh_rt_desc_pool_) {
      vkDestroyDescriptorPool(device_, mesh_rt_desc_pool_, nullptr);
      mesh_rt_desc_pool_ = VK_NULL_HANDLE;
      mesh_rt_desc_sets_ = {};
    }
    if (mesh_rt_desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, mesh_rt_desc_layout_, nullptr);
      mesh_rt_desc_layout_ = VK_NULL_HANDLE;
    }
    if (desc_pool_) {
      vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    }
    if (desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, desc_layout_, nullptr);
    }
    destroyGraphicsPipelines();
    if (fg_overlay_render_pass_) {
      vkDestroyRenderPass(device_, fg_overlay_render_pass_, nullptr);
      fg_overlay_render_pass_ = VK_NULL_HANDLE;
    }
    if (fg_ui_render_pass_) {
      vkDestroyRenderPass(device_, fg_ui_render_pass_, nullptr);
      fg_ui_render_pass_ = VK_NULL_HANDLE;
    }
    if (render_pass_) {
      vkDestroyRenderPass(device_, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
      render_pass_format_ = VK_FORMAT_UNDEFINED;
    }
    destroyBuffer(uniform_buf_);
    for (auto &s : frames_) {
      destroyBuffer(s.ui_vbo);
      destroyBuffer(s.ui_ibo);
      destroyBuffer(s.mesh_vbo);
      destroyBuffer(s.bone_ssbo);
      if (s.timestamp_pool) {
        vkDestroyQueryPool(device_, s.timestamp_pool, nullptr);
        s.timestamp_pool = VK_NULL_HANDLE;
      }
      if (s.fence) {
        vkDestroyFence(device_, s.fence, nullptr);
      }
      if (s.image_available) {
        vkDestroySemaphore(device_, s.image_available, nullptr);
      }
    }
    if (cmd_pool_) {
      vkDestroyCommandPool(device_, cmd_pool_, nullptr);
    }
    if (surface_) {
      streamline_vulkan_runtime_.destroySurface(
          instance_, surface_, nullptr);
      surface_ = VK_NULL_HANDLE;
    }
    streamline_vulkan_runtime_.shutdownBeforeVulkan();
    if (device_) {
      vkDestroyDevice(device_, nullptr);
    }
    if (debug_messenger_ && instance_) {
      const auto destroy_debug_messenger =
          reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
              vkGetInstanceProcAddr(instance_,
                                    "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy_debug_messenger != nullptr) {
        destroy_debug_messenger(instance_, debug_messenger_, nullptr);
      }
      debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_) {
      vkDestroyInstance(instance_, nullptr);
    }
    streamline_vulkan_runtime_.releaseAfterVulkan();
    instance_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    descriptor_binding_partially_bound_enabled_ = false;
  }

void VulkanBackend::resize(int, int) {
    // A resize is a mandatory Native transition.  Keep the user's FG request
    // intact so the next stable frame can opt back in through a fresh proxy
    // swapchain.
    recreate_swapchain_ = true;
    swapchain_recreate_target_ = SwapchainOwnership::Native;
    fg_force_native_recovery_ = true;
    // A window minimize/restore or framebuffer resize can skip presentation
    // frames entirely, so invalidate temporal reconstruction at the event
    // boundary rather than waiting for a drawable viewport.
    streamline_vulkan_runtime_.invalidateDlssHistory();
    streamline_temporal_history_valid_ = false;
  }

bool VulkanBackend::uploadFontAtlas(const void *pixels, int width, int height,
                       bool is_rgba) {
    if (!pixels || width <= 0 || height <= 0 || !device_) {
      return false;
    }
    font_w_ = width;
    font_h_ = height;

    std::vector<uint8_t> rgba;
    const uint8_t *src = static_cast<const uint8_t *>(pixels);
    VkDeviceSize size = 0;
    if (is_rgba) {
      size = static_cast<VkDeviceSize>(width) * height * 4;
    } else {
      rgba.resize(static_cast<size_t>(width) * height * 4);
      for (int i = 0, n = width * height; i < n; ++i) {
        rgba[static_cast<size_t>(i) * 4 + 0] = 255;
        rgba[static_cast<size_t>(i) * 4 + 1] = 255;
        rgba[static_cast<size_t>(i) * 4 + 2] = 255;
        rgba[static_cast<size_t>(i) * 4 + 3] = src[i];
      }
      src = rgba.data();
      size = static_cast<VkDeviceSize>(rgba.size());
    }
    Buffer staging{};
    if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging)) {
      return false;
    }
    if (!staging.mapped) {
      destroyBuffer(staging);
      return false;
    }
    std::memcpy(staging.mapped, src, static_cast<size_t>(size));

    if (font_image_) {
      vkDestroyImageView(device_, font_view_, nullptr);
      vkDestroyImage(device_, font_image_, nullptr);
      vkFreeMemory(device_, font_mem_, nullptr);
      font_view_ = VK_NULL_HANDLE;
      font_image_ = VK_NULL_HANDLE;
      font_mem_ = VK_NULL_HANDLE;
      font_ready_ = false;
    }

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ii, nullptr, &font_image_));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, font_image_, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    const auto memory_type = findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      writeLog("Vulkan font image has no compatible memory type");
      vkDestroyImage(device_, font_image_, nullptr);
      font_image_ = VK_NULL_HANDLE;
      destroyBuffer(staging);
      return false;
    }
    ai.memoryTypeIndex = *memory_type;
    const VkResult allocation_result =
        vkAllocateMemory(device_, &ai, nullptr, &font_mem_);
    if (allocation_result != VK_SUCCESS) {
      SDL_Log("Vulkan font image allocation failed: %d",
              static_cast<int>(allocation_result));
      vkDestroyImage(device_, font_image_, nullptr);
      font_image_ = VK_NULL_HANDLE;
      destroyBuffer(staging);
      return false;
    }
    const VkResult bind_result =
        vkBindImageMemory(device_, font_image_, font_mem_, 0);
    if (bind_result != VK_SUCCESS) {
      SDL_Log("Vulkan font image memory bind failed: %d",
              static_cast<int>(bind_result));
      vkDestroyImage(device_, font_image_, nullptr);
      vkFreeMemory(device_, font_mem_, nullptr);
      font_image_ = VK_NULL_HANDLE;
      font_mem_ = VK_NULL_HANDLE;
      destroyBuffer(staging);
      return false;
    }


    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult begin_result = vkBeginCommandBuffer(cmd, &bi);
    if (begin_result != VK_SUCCESS) {
      SDL_Log("Vulkan font upload command begin failed: %d",
              static_cast<int>(begin_result));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      return false;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = font_image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, font_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    const VkResult end_result = vkEndCommandBuffer(cmd);
    if (end_result != VK_SUCCESS) {
      SDL_Log("Vulkan font upload command end failed: %d",
              static_cast<int>(end_result));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      return false;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const FrameSync &sync = frames_[frame_index_];
    const auto submit_start = Clock::now();
    logDiagnosticApi("vkQueueSubmit.font_upload", "before", std::nullopt, 0.0,
                     UINT32_MAX, sync.fence, VK_NULL_HANDLE, cmd, true, true);
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    logDiagnosticApi(
        "vkQueueSubmit.font_upload", "after", submit_result,
        std::chrono::duration<double, std::milli>(Clock::now() - submit_start)
            .count(),
        UINT32_MAX, sync.fence, VK_NULL_HANDLE, cmd, true, false);
    if (submit_result != VK_SUCCESS) {
      SDL_Log("Vulkan font upload submit failed: %d",
              static_cast<int>(submit_result));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      return false;
    }
    const auto idle_start = Clock::now();
    logDiagnosticApi("vkQueueWaitIdle.font_upload", "before", std::nullopt,
                     0.0, UINT32_MAX, sync.fence, VK_NULL_HANDLE, cmd, true,
                     true);
    const VkResult idle_result = vkQueueWaitIdle(graphics_queue_);
    logDiagnosticApi(
        "vkQueueWaitIdle.font_upload", "after", idle_result,
        std::chrono::duration<double, std::milli>(Clock::now() - idle_start)
            .count(),
        UINT32_MAX, sync.fence, VK_NULL_HANDLE, cmd, true, false);
    if (idle_result != VK_SUCCESS) {
      SDL_Log("Vulkan font upload queue wait failed: %d",
              static_cast<int>(idle_result));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      return false;
    }
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    destroyBuffer(staging);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = font_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &font_view_));

    if (!font_sampler_) {
      VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sci.magFilter = VK_FILTER_LINEAR;
      sci.minFilter = VK_FILTER_LINEAR;
      sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &font_sampler_));
    }

    VkDescriptorImageInfo di{};
    di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    di.imageView = font_view_;
    di.sampler = font_sampler_;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = desc_set_;
    w.dstBinding = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo = &di;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    font_ready_ = true;
    writeLog("Vulkan font atlas uploaded");
    return true;
  }

unsigned int VulkanBackend::fontTextureId() const {

    return 1;
  }

bool VulkanBackend::supportsStaticModel() const { return true; }

void VulkanBackend::beginLatencyFrame(
      std::uint32_t frame_index, PathTraceReflexMode mode,
      bool frame_generation_requested) {
    streamline_vulkan_runtime_.beginLatencyFrame(
        frame_index, mode, frame_generation_requested);
    stats_.dlss_frame_generation_supported =
        frameGenerationPlatformSupported();
    stats_.dlss_frame_generation_requested =
        frame_generation_requested;
    stats_.reflex_supported =
        streamline_vulkan_runtime_.reflexSupported();
  }

void VulkanBackend::endLatencySimulation() {
    streamline_vulkan_runtime_.endLatencySimulation();
  }

std::uint32_t VulkanBackend::latencyPingMessage() const {
    return streamline_vulkan_runtime_.pclLatencyPingMessage();
  }

void VulkanBackend::markLatencyPing() {
    streamline_vulkan_runtime_.markPclLatencyPing();
  }

void VulkanBackend::render(const FrameInput &frame) {
#include "vulkan_render/vulkan_backend_render.inc"
  }

void VulkanBackend::setVSync(bool enabled) {
    if (vsync_ == enabled) {
      return;
    }
    vsync_ = enabled;
    recreate_swapchain_ = true;
    swapchain_recreate_target_ = SwapchainOwnership::Native;
    fg_force_native_recovery_ = true;
  }

bool VulkanBackend::vsyncEnabled() const { return vsync_; }

BackendKind VulkanBackend::kind() const { return BackendKind::Vulkan; }

const char *VulkanBackend::name() const { return "Vulkan"; }

const char *VulkanBackend::deviceName() const { return device_name_.c_str(); }

FrameStats VulkanBackend::stats() const { return stats_; }

PathTracePostProcessCapabilities
  VulkanBackend::pathTracePostProcessCapabilities() const {
    PathTracePostProcessCapabilities capabilities{};
    capabilities.dlss_super_resolution =
        streamline_vulkan_runtime_.dlssSupported();
    capabilities.dlaa = streamline_vulkan_runtime_.dlssSupported();
    capabilities.dlss_ray_reconstruction =
        streamline_vulkan_runtime_.dlssRayReconstructionSupported();
    capabilities.dlss_frame_generation =
        frameGenerationPlatformSupported();
    capabilities.reflex =
        streamline_vulkan_runtime_.reflexSupported();
    return capabilities;
  }

std::string_view VulkanBackend::pathTracePostProcessStatus() const {
    post_process_status_cache_ = streamline_vulkan_runtime_.status();
    const std::string fg_status =
        streamline_vulkan_runtime_.frameGenerationStatus();
    const FrameGenerationDiagnostic fg_diagnostic =
        streamline_vulkan_runtime_.frameGenerationDiagnostic();
    if (!fg_status.empty()) {
      if (!post_process_status_cache_.empty()) {
        post_process_status_cache_ += " | ";
      }
      post_process_status_cache_ += fg_status;
    }
    post_process_status_cache_ +=
        " | FG Requested=" +
        std::string(fg_diagnostic.requested ? "On" : "Off") +
        " Active=" + (fg_diagnostic.state ==
                               FrameGenerationRuntimeState::Active
                           ? "On"
                           : "Off") +
        " Swapchain=" +
        swapchainOwnershipName(fg_diagnostic.ownership) +
        " Frames=" +
        std::to_string(fg_diagnostic.frames_actually_presented);
    if (streamline_vulkan_runtime_.frameGenerationSupported() &&
        !fg_swapchain_color_format_supported_) {
      post_process_status_cache_ +=
          " | DLSS Frame Generation requires an 8-bit RGBA/BGRA swapchain";
    } else if (streamline_vulkan_runtime_.frameGenerationSupported() &&
               graphics_family_ != present_family_) {
      post_process_status_cache_ +=
          " | DLSS Frame Generation requires a shared graphics/present queue";
    } else if (streamline_vulkan_runtime_.frameGenerationSupported() &&
               !vsync_ &&
               swap_present_mode_ != VK_PRESENT_MODE_IMMEDIATE_KHR) {
      post_process_status_cache_ +=
          " | DLSS Frame Generation requires "
          "VK_PRESENT_MODE_IMMEDIATE_KHR";
    } else if (streamline_vulkan_runtime_.frameGenerationSupported() &&
               !fg_swapchain_transfer_src_supported_) {
      post_process_status_cache_ +=
          " | DLSS Frame Generation requires swapchain transfer-source usage";
    } else if (streamline_vulkan_runtime_.frameGenerationSupported() &&
               !fg_swapchain_resources_ready_) {
      post_process_status_cache_ +=
          " | DLSS Frame Generation guide images are pending swapchain "
          "creation";
    }
    return post_process_status_cache_;
  }

RayTracingCapability VulkanBackend::rayTracingCapability() const {
    return rt_capability_;
  }

bool VulkanBackend::supportsRayTracing() const {
    return rt_capability_.supported && rt_capability_.device_extensions_enabled;
  }

RenderPath VulkanBackend::activeRenderPath() const { return active_render_path_; }

void VulkanBackend::prepareForSystemDialog() {
    swapchain_recreate_target_ = SwapchainOwnership::Native;
    fg_force_native_recovery_ = true;
    recreate_swapchain_ = true;
    // If a proxy is currently armed, complete the Native transition while
    // the window is still drawable.  If the shell has already hidden it,
    // resumeAfterSystemDialog() retries the same transaction.
    if (device_ && swapchain_ &&
        streamline_vulkan_runtime_.swapchainOwnership() ==
            SwapchainOwnership::StreamlineFrameGenerationProxy) {
      (void)recreateSwapchain();
    }
    presentation_suspended_ = true;
    streamline_temporal_history_valid_ = false;
    if (device_) {
      // Drain all queues so the modal shell dialog cannot race RT builds.
      (void)streamline_vulkan_runtime_.deviceWaitIdle(device_);
    }
  }

void VulkanBackend::resumeAfterSystemDialog() {
    presentation_suspended_ = false;
    // Dialog may have resized/minimized the window or stolen the GPU.
    recreate_swapchain_ = true;
    swapchain_recreate_target_ = SwapchainOwnership::Native;
    fg_force_native_recovery_ = true;
    rt_scene_built_.fill(false);
    last_rt_scene_hash_.fill(0);
    streamline_temporal_history_valid_ = false;
  }

bool VulkanBackend::presentationSuspended() const {
    return presentation_suspended_;
  }

void VulkanBackend::failActiveStillRender(const FrameInput &frame,
                             const std::string &detail) {
    if (frame.still_render == nullptr ||
        frame.still_render->status == nullptr ||
        frame.still_render->job_id == 0u) {
      return;
    }
    StillRenderStatus &status = *frame.still_render->status;
    if (status.state == StillRenderJobState::Completed ||
        status.state == StillRenderJobState::Failed ||
        status.state == StillRenderJobState::Cancelled) {
      return;
    }
    status.job_id = frame.still_render->job_id;
    status.state = StillRenderJobState::Failed;
    status.error = detail;
    still_active_job_id_ = 0u;
    xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                      static_cast<unsigned long long>(status.job_id),
                      status.error.c_str());
  }

void VulkanBackend::enterFatalVulkanError(const FrameInput &frame, const char *api,
                             VkResult result) {
    recordFatalVulkanError(api, result);
    failActiveStillRender(frame, fatal_error_detail_);
  }

void VulkanBackend::recordFatalVulkanError(const char *api, VkResult result) {
    fatal_error_ = true;
    fatal_error_detail_ = std::string(api) + " failed: " +
                          vkResultName(result) + " (" +
                          std::to_string(static_cast<int>(result)) + ")";
    xpbd::log::error(fatal_error_detail_);
  }

[[nodiscard]] bool VulkanBackend::presentFenceLifecycleEnabled() const noexcept {
    // DLSS-G owns an asynchronous proxy present. Its documented Vulkan
    // contract consumes the Present semaphore and later releases the acquired
    // image; it does not signal an application-provided maintenance1 Present
    // fence. Native swapchains retain the stronger fence lifecycle.
    return swapchain_maintenance1_enabled_ &&
           streamline_vulkan_runtime_.swapchainOwnership() ==
               SwapchainOwnership::Native;
  }

[[nodiscard]] bool
  VulkanBackend::frameGenerationPlatformSupported() const noexcept {
    return streamline_vulkan_runtime_.frameGenerationSupported() &&
           fg_swapchain_transfer_src_supported_ &&
           fg_swapchain_color_format_supported_ &&
           graphics_family_ == present_family_;
  }

[[nodiscard]] bool
  VulkanBackend::frameGenerationSwapchainReady() const noexcept {
    return frameGenerationPlatformSupported() && !vsync_ &&
           swap_present_mode_ == VK_PRESENT_MODE_IMMEDIATE_KHR &&
           streamline_vulkan_runtime_.swapchainOwnership() ==
               SwapchainOwnership::StreamlineFrameGenerationProxy &&
           fg_swapchain_resources_ready_;
  }

void VulkanBackend::logDiagnosticApi(const char *api, const char *edge,
                        std::optional<VkResult> result, double elapsed_ms,
                        std::uint32_t image_index, VkFence frame_fence,
                        VkFence image_fence, VkCommandBuffer command,
                        bool force, bool flush_after) const {
    if (!diagnostics_enabled_) {
      return;
    }
    const bool failed = result.has_value() && *result != VK_SUCCESS;
    if (!diagnostic_trace_frame_ && !force && !failed) {
      return;
    }
    const char *result_name =
        result.has_value() ? vkResultName(*result) : "PENDING";
    const int result_code =
        result.has_value() ? static_cast<int>(*result) : 0;
    xpbd::log::infof(
        "VKDIAG api ts_us=%llu thread=%llu frame=%llu commit=%llu "
        "slot=%zu image=%u stage=%s edge=%s result=%s(%d) elapsed_ms=%.3f "
        "frame_fence=0x%llx image_fence=0x%llx cmd=0x%llx",
        static_cast<unsigned long long>(diagnosticTimestampUs()),
        static_cast<unsigned long long>(diagnosticThreadId()),
        static_cast<unsigned long long>(diagnostic_context_.render_frame),
        static_cast<unsigned long long>(
            diagnostic_context_.result_commit_frame),
        frame_index_, image_index, api, edge, result_name, result_code,
        elapsed_ms,
        static_cast<unsigned long long>(diagnosticHandle(frame_fence)),
        static_cast<unsigned long long>(diagnosticHandle(image_fence)),
        static_cast<unsigned long long>(diagnosticHandle(command)));
    if (flush_after || failed) {
      xpbd::log::flush();
    }
  }

void VulkanBackend::logDiagnosticFrame(const FrameInput &frame,
                          const FrameSync &sync) const {
    if (!diagnostics_enabled_ || !diagnostic_trace_frame_) {
      return;
    }
    const std::size_t bone_count =
        frame.static_model_frame ? frame.static_model_frame->bones.size() : 0;
    const std::uint32_t cube_count =
        frame.static_model_frame ? frame.static_model_frame->cube_count : 0;
    const std::size_t vertex_count =
        frame.static_model ? frame.static_model->vertices.size() : 0;
    const std::size_t index_count =
        frame.static_model ? frame.static_model->indices.size() : 0;
    const std::size_t solid_count =
        frame.scene ? frame.scene->solid.size() : 0;
    const std::size_t transparent_count =
        frame.scene ? frame.scene->transparent.size() : 0;
    const std::size_t line_count =
        frame.scene ? frame.scene->lines.size() : 0;
    const auto &d = frame.diagnostics;
    xpbd::log::infof(
        "VKDIAG frame ts_us=%llu thread=%llu frame=%llu commit=%llu "
        "remaining=%u worker=%d presentation=%d playback=%d "
        "preview_time=%.9g preview_index=%d bake=%d/%d "
        "gen_model=%llu gen_animation=%llu gen_physics=%llu gen_texture=%llu "
        "static_gen_model=%llu static_gen_texture=%llu bones=%zu cubes=%u "
        "vertices=%zu indices=%zu overlay=%zu/%zu/%zu "
        "capacity_bone=%llu capacity_mesh=%llu capacity_ui_v=%llu "
        "capacity_ui_i=%llu",
        static_cast<unsigned long long>(diagnosticTimestampUs()),
        static_cast<unsigned long long>(diagnosticThreadId()),
        static_cast<unsigned long long>(d.render_frame),
        static_cast<unsigned long long>(d.result_commit_frame),
        d.frames_remaining, d.worker_phase, d.presentation_mode,
        d.playback_state, d.preview_time, d.preview_frame_index, d.bake_current,
        d.bake_total,
        static_cast<unsigned long long>(d.model_generation),
        static_cast<unsigned long long>(d.animation_generation),
        static_cast<unsigned long long>(d.physics_generation),
        static_cast<unsigned long long>(d.texture_generation),
        static_cast<unsigned long long>(frame.static_model_generation),
        static_cast<unsigned long long>(frame.static_texture_generation),
        bone_count, cube_count, vertex_count, index_count, solid_count,
        transparent_count, line_count,
        static_cast<unsigned long long>(sync.bone_ssbo.capacity),
        static_cast<unsigned long long>(sync.mesh_vbo.capacity),
        static_cast<unsigned long long>(sync.ui_vbo.capacity),
        static_cast<unsigned long long>(sync.ui_ibo.capacity));
    xpbd::log::flush();
  }

void VulkanBackend::logDiagnosticPerf(const FrameSync &sync) const {
    if (!perf_diagnostics_enabled_ || !sync.perf_pending) {
      return;
    }
    const FrameStats &recorded = sync.perf_snapshot;
    // CPU values and RT counters were captured when this slot was submitted;
    // GPU values have just been read from this same slot's completed query
    // pool. Keep those sources separate to avoid cross-slot frame mixing.
    xpbd::log::infof(
        "VKDIAG rt_perf frame=%llu slot=%zu cpu_backend_ms=%.4f "
        "cpu_scene_assembly_ms=%.4f cpu_scene_hash_ms=%.4f "
        "cpu_emitter_distribution_ms=%.4f cpu_descriptor_update_ms=%.4f "
        "gpu_total_ms=%.4f gpu_as_build_ms=%.4f gpu_path_trace_ms=%.4f "
        "gpu_timestamp_valid=%d rt_upload_bytes=%llu "
        "rt_emitter_distribution_rebuilds=%llu "
        "rt_descriptor_write_calls=%llu rt_descriptor_entries_written=%llu "
        "rt_descriptor_cache_hits=%llu rt_blas_full_builds=%llu "
        "rt_blas_refits=%llu rt_tlas_full_builds=%llu rt_tlas_updates=%llu "
        "rt_allocated_bytes=%llu rt_aov_write_mask=0x%08x "
        "last_build_reason=%s last_tlas_reason=%s",
        static_cast<unsigned long long>(sync.perf_render_frame), frame_index_,
        recorded.backend_cpu_ms, recorded.cpu_scene_assembly_ms,
        recorded.cpu_scene_hash_ms, recorded.cpu_emitter_distribution_ms,
        recorded.cpu_descriptor_update_ms, stats_.gpu_timestamp_total_ms,
        stats_.gpu_as_build_ms, stats_.gpu_path_trace_ms,
        stats_.gpu_timestamp_valid ? 1 : 0,
        static_cast<unsigned long long>(recorded.rt_upload_bytes),
        static_cast<unsigned long long>(
            recorded.rt_emitter_distribution_rebuilds),
        static_cast<unsigned long long>(recorded.rt_descriptor_write_calls),
        static_cast<unsigned long long>(
            recorded.rt_descriptor_entries_written),
        static_cast<unsigned long long>(recorded.rt_descriptor_cache_hits),
        static_cast<unsigned long long>(recorded.rt_full_builds),
        static_cast<unsigned long long>(recorded.rt_refits),
        static_cast<unsigned long long>(recorded.rt_tlas_full_builds),
        static_cast<unsigned long long>(recorded.rt_tlas_updates),
        static_cast<unsigned long long>(recorded.rt_allocated_bytes),
        static_cast<unsigned>(recorded.rt_aov_write_mask),
        rtAccelerationBuildReasonName(recorded.rt_last_build_reason),
        rtAccelerationBuildReasonName(recorded.rt_last_tlas_reason));
  }

void VulkanBackend::logDiagnosticResources(const FrameInput &frame,
                              const FrameSync &sync,
                              VkDeviceSize requested_bone_bytes,
                              VkDeviceSize requested_mesh_bytes,
                              VkDeviceSize requested_ui_vertex_bytes,
                              VkDeviceSize requested_ui_index_bytes,
                               bool force) const {
    if (!diagnostics_enabled_ || (!diagnostic_trace_frame_ && !force)) {
      return;
    }
    xpbd::log::infof(
        "VKDIAG resources ts_us=%llu thread=%llu frame=%llu slot=%zu "
        "requested_bone=%llu requested_mesh=%llu requested_ui_v=%llu "
        "requested_ui_i=%llu capacity_bone=%llu capacity_mesh=%llu "
        "capacity_ui_v=%llu capacity_ui_i=%llu upload=%llu "
        "static_upload=%llu realloc_frame=%d realloc_total=%llu "
        "static_rebuilds=%llu",
        static_cast<unsigned long long>(diagnosticTimestampUs()),
        static_cast<unsigned long long>(diagnosticThreadId()),
        static_cast<unsigned long long>(frame.diagnostics.render_frame),
        frame_index_,
        static_cast<unsigned long long>(requested_bone_bytes),
        static_cast<unsigned long long>(requested_mesh_bytes),
        static_cast<unsigned long long>(requested_ui_vertex_bytes),
        static_cast<unsigned long long>(requested_ui_index_bytes),
        static_cast<unsigned long long>(sync.bone_ssbo.capacity),
        static_cast<unsigned long long>(sync.mesh_vbo.capacity),
        static_cast<unsigned long long>(sync.ui_vbo.capacity),
        static_cast<unsigned long long>(sync.ui_ibo.capacity),
        static_cast<unsigned long long>(stats_.upload_bytes),
        static_cast<unsigned long long>(stats_.static_resource_upload_bytes),
        stats_.buffer_reallocations,
        static_cast<unsigned long long>(total_buffer_reallocations_),
        static_cast<unsigned long long>(static_resource_rebuilds_));
  }

void VulkanBackend::mulMat(const float *a, const float *b, float *o) {
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                       a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
  }

void VulkanBackend::glMvpToVulkan(const float *m, float *o) {
    for (int c = 0; c < 4; ++c) {
      const float x = m[c * 4 + 0];
      const float y = m[c * 4 + 1];
      const float z = m[c * 4 + 2];
      const float w = m[c * 4 + 3];
      o[c * 4 + 0] = x;
      o[c * 4 + 1] = -y;
      o[c * 4 + 2] = 0.5f * z + 0.5f * w;
      o[c * 4 + 3] = w;
    }
  }

void VulkanBackend::destroyGraphicsPipelines() {
    rt_pipelines_ready_ = false;
    if (ui_pipeline_) {
      vkDestroyPipeline(device_, ui_pipeline_, nullptr);
      ui_pipeline_ = VK_NULL_HANDLE;
    }
    if (mesh_pipeline_trans_) {
      vkDestroyPipeline(device_, mesh_pipeline_trans_, nullptr);
      mesh_pipeline_trans_ = VK_NULL_HANDLE;
    }
    if (mesh_pipeline_lines_) {
      vkDestroyPipeline(device_, mesh_pipeline_lines_, nullptr);
      mesh_pipeline_lines_ = VK_NULL_HANDLE;
    }
    if (mesh_pipeline_overlay_lines_) {
      vkDestroyPipeline(device_, mesh_pipeline_overlay_lines_, nullptr);
      mesh_pipeline_overlay_lines_ = VK_NULL_HANDLE;
    }
    if (mesh_pipeline_temporal_hud_lines_) {
      vkDestroyPipeline(device_, mesh_pipeline_temporal_hud_lines_, nullptr);
      mesh_pipeline_temporal_hud_lines_ = VK_NULL_HANDLE;
    }
    if (mesh_pipeline_) {
      vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
      mesh_pipeline_ = VK_NULL_HANDLE;
    }
    if (static_mesh_pipeline_blend_) {
      vkDestroyPipeline(device_, static_mesh_pipeline_blend_, nullptr);
      static_mesh_pipeline_blend_ = VK_NULL_HANDLE;
    }
    if (static_mesh_pipeline_) {
      vkDestroyPipeline(device_, static_mesh_pipeline_, nullptr);
      static_mesh_pipeline_ = VK_NULL_HANDLE;
    }
    if (mesh_rt_pipeline_trans_) {
      vkDestroyPipeline(device_, mesh_rt_pipeline_trans_, nullptr);
      mesh_rt_pipeline_trans_ = VK_NULL_HANDLE;
    }
    if (mesh_rt_pipeline_) {
      vkDestroyPipeline(device_, mesh_rt_pipeline_, nullptr);
      mesh_rt_pipeline_ = VK_NULL_HANDLE;
    }
    if (static_rt_pipeline_blend_) {
      vkDestroyPipeline(device_, static_rt_pipeline_blend_, nullptr);
      static_rt_pipeline_blend_ = VK_NULL_HANDLE;
    }
    if (static_rt_pipeline_) {
      vkDestroyPipeline(device_, static_rt_pipeline_, nullptr);
      static_rt_pipeline_ = VK_NULL_HANDLE;
    }
    if (skybox_pipeline_) {
      vkDestroyPipeline(device_, skybox_pipeline_, nullptr);
      skybox_pipeline_ = VK_NULL_HANDLE;
    }
    if (ui_layout_) {
      vkDestroyPipelineLayout(device_, ui_layout_, nullptr);
      ui_layout_ = VK_NULL_HANDLE;
    }
    if (mesh_layout_) {
      vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
      mesh_layout_ = VK_NULL_HANDLE;
    }
    if (mesh_rt_layout_) {
      vkDestroyPipelineLayout(device_, mesh_rt_layout_, nullptr);
      mesh_rt_layout_ = VK_NULL_HANDLE;
    }
    if (static_rt_layout_) {
      vkDestroyPipelineLayout(device_, static_rt_layout_, nullptr);
      static_rt_layout_ = VK_NULL_HANDLE;
    }
    if (static_mesh_layout_) {
      vkDestroyPipelineLayout(device_, static_mesh_layout_, nullptr);
      static_mesh_layout_ = VK_NULL_HANDLE;
    }
    if (skybox_layout_) {
      vkDestroyPipelineLayout(device_, skybox_layout_, nullptr);
      skybox_layout_ = VK_NULL_HANDLE;
    }
  }

[[nodiscard]] bool VulkanBackend::graphicsPipelinesReady() const {
    const bool base = ui_layout_ && mesh_layout_ && static_mesh_layout_ &&
                      skybox_layout_ && ui_pipeline_ && mesh_pipeline_ &&
                      mesh_pipeline_trans_ && mesh_pipeline_lines_ &&
                      mesh_pipeline_overlay_lines_ &&
                      mesh_pipeline_temporal_hud_lines_ &&
                      static_mesh_pipeline_ && static_mesh_pipeline_blend_ &&
                      skybox_pipeline_;
    if (!base) {
      return false;
    }
    if (!rt_capability_.device_extensions_enabled) {
      return true;
    }
    return rt_pipelines_ready_;
  }

std::optional<uint32_t>
  VulkanBackend::findMemoryType(uint32_t bits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & props) == props) {
        return i;
      }
    }
    return std::nullopt;
  }

[[nodiscard]] VulkanBackend::MemoryHeapDiagnostic
VulkanBackend::memoryHeapDiagnostic(std::uint32_t memory_type) const {
    MemoryHeapDiagnostic diagnostic;
    diagnostic.memory_type = memory_type;
    if (phys_ == VK_NULL_HANDLE ||
        memory_type == (std::numeric_limits<std::uint32_t>::max)()) {
      return diagnostic;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = memory_budget_supported_ ? &budget : nullptr;
    vkGetPhysicalDeviceMemoryProperties2(phys_, &properties);
    if (memory_type >= properties.memoryProperties.memoryTypeCount) {
      return diagnostic;
    }
    diagnostic.heap =
        properties.memoryProperties.memoryTypes[memory_type].heapIndex;
    if (memory_budget_supported_ &&
        diagnostic.heap < properties.memoryProperties.memoryHeapCount) {
      diagnostic.budget = budget.heapBudget[diagnostic.heap];
      diagnostic.usage = budget.heapUsage[diagnostic.heap];
    }
    return diagnostic;
  }

void VulkanBackend::logBufferResourceError(const char *api, VkResult result,
                              const char *resource_name, VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              std::uint32_t memory_type) {
    const MemoryHeapDiagnostic heap = memoryHeapDiagnostic(memory_type);
    xpbd::log::errorf(
        "Vulkan resource failure: API=%s VkResult=%s(%d) resource=%s "
        "size=%llu usage=0x%x memory_type=%u heap=%u heap_budget=%llu "
        "heap_usage=%llu frame_slot=%u still_job_id=%llu",
        api, vkResultName(result), static_cast<int>(result), resource_name,
        static_cast<unsigned long long>(size), static_cast<unsigned int>(usage),
        heap.memory_type, heap.heap,
        static_cast<unsigned long long>(heap.budget),
        static_cast<unsigned long long>(heap.usage), frame_index_,
        static_cast<unsigned long long>(still_active_job_id_));
    if (result == VK_ERROR_DEVICE_LOST) {
      recordFatalVulkanError(api, result);
    }
  }

void VulkanBackend::logImageResourceError(const char *api, VkResult result,
                             const char *resource_name, VkFormat format,
                             std::uint32_t width, std::uint32_t height,
                             std::uint32_t depth, VkImageUsageFlags usage,
                             VkDeviceSize allocation_size,
                             std::uint32_t memory_type) {
    const MemoryHeapDiagnostic heap = memoryHeapDiagnostic(memory_type);
    xpbd::log::errorf(
        "Vulkan resource failure: API=%s VkResult=%s(%d) resource=%s "
        "format=%d extent=%ux%ux%u usage=0x%x allocation_size=%llu "
        "memory_type=%u heap=%u heap_budget=%llu heap_usage=%llu "
        "frame_slot=%u still_job_id=%llu",
        api, vkResultName(result), static_cast<int>(result), resource_name,
        static_cast<int>(format), width, height, depth,
        static_cast<unsigned int>(usage),
        static_cast<unsigned long long>(allocation_size), heap.memory_type,
        heap.heap, static_cast<unsigned long long>(heap.budget),
        static_cast<unsigned long long>(heap.usage), frame_index_,
        static_cast<unsigned long long>(still_active_job_id_));
    if (result == VK_ERROR_DEVICE_LOST) {
      recordFatalVulkanError(api, result);
    }
  }

void VulkanBackend::destroyBuffer(Buffer &b) {
    if (b.mapped && b.memory) {
      vkUnmapMemory(device_, b.memory);
      b.mapped = nullptr;
    }
    if (b.buffer) {
      vkDestroyBuffer(device_, b.buffer, nullptr);
    }
    if (b.memory) {
      vkFreeMemory(device_, b.memory, nullptr);
    }
    b = {};
  }

bool VulkanBackend::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, Buffer &out,
                     const char *resource_name) {
    destroyBuffer(out);

    VkDeviceSize alloc = size;
    if (alloc < 64 * 1024) {
      alloc = 64 * 1024;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = alloc;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const VkResult create_result =
        vkCreateBuffer(device_, &bi, nullptr, &out.buffer);
    if (create_result != VK_SUCCESS) {
      logBufferResourceError(
          "vkCreateBuffer", create_result, resource_name, alloc, usage,
          (std::numeric_limits<std::uint32_t>::max)());
      return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    const auto memory_type = findMemoryType(req.memoryTypeBits, props);
    if (!memory_type) {
      logBufferResourceError(
          "findMemoryType", VK_ERROR_FEATURE_NOT_PRESENT, resource_name,
          req.size, usage, (std::numeric_limits<std::uint32_t>::max)());
      destroyBuffer(out);
      return false;
    }
    ai.memoryTypeIndex = *memory_type;
    const VkResult allocation_result =
        vkAllocateMemory(device_, &ai, nullptr, &out.memory);
    if (allocation_result != VK_SUCCESS) {
      logBufferResourceError("vkAllocateMemory", allocation_result,
                             resource_name, req.size, usage, *memory_type);
      destroyBuffer(out);
      return false;
    }
    const VkResult bind_result =
        vkBindBufferMemory(device_, out.buffer, out.memory, 0);
    if (bind_result != VK_SUCCESS) {
      logBufferResourceError("vkBindBufferMemory", bind_result,
                             resource_name, req.size, usage, *memory_type);
      destroyBuffer(out);
      return false;
    }
    out.capacity = alloc;
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      const VkResult map_result =
          vkMapMemory(device_, out.memory, 0, alloc, 0, &out.mapped);
      if (map_result != VK_SUCCESS) {
        logBufferResourceError("vkMapMemory", map_result, resource_name,
                               alloc, usage, *memory_type);
        destroyBuffer(out);
        return false;
      }
    }
    return true;
  }

bool VulkanBackend::ensureBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage,
                    bool *reallocated) {
    if (reallocated) {
      *reallocated = false;
    }
    if (b.buffer && b.capacity >= size) {
      return true;
    }

    VkDeviceSize next = size + size / 2;
    if (next < size + 64 * 1024) {
      next = size + 64 * 1024;
    }
    if (next > size + 8 * 1024 * 1024) {
      next = size + 8 * 1024 * 1024;
    }
    const bool created = createBuffer(next, usage,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      b);
    if (created && reallocated) {
      *reallocated = true;
    }
    return created;
  }

bool VulkanBackend::uploadBuffer(Buffer &b, VkDeviceSize offset, VkDeviceSize bytes,
                    const void *src) {
    if (bytes == 0) {
      return true;
    }
    if (!b.mapped || !src || offset > b.capacity ||
        bytes > b.capacity - offset) {
      return false;
    }
    std::memcpy(static_cast<std::byte *>(b.mapped) + offset, src,
                static_cast<size_t>(bytes));
    return true;
  }

void VulkanBackend::destroyImage(ImageResource &image) {
    if (image.view) {
      vkDestroyImageView(device_, image.view, nullptr);
    }
    if (image.image) {
      vkDestroyImage(device_, image.image, nullptr);
    }
    if (image.memory) {
      vkFreeMemory(device_, image.memory, nullptr);
    }
    image = {};
  }

bool VulkanBackend::createFrameGenerationImage(std::uint32_t width,
                                  std::uint32_t height,
                                  VkImageUsageFlags usage,
                                  ImageResource &out) {
    if (width == 0u || height == 0u) {
      return false;
    }
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = swap_format_;
    image_info.extent = {width, height, 1u};
    image_info.mipLevels = 1u;
    image_info.arrayLayers = 1u;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &out.image) !=
        VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, out.image, &requirements);
    const auto memory_type =
        findMemoryType(requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      destroyImage(out);
      return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *memory_type;
    if (vkAllocateMemory(device_, &allocation, nullptr, &out.memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(device_, out.image, out.memory, 0) != VK_SUCCESS) {
      destroyImage(out);
      return false;
    }
    VkImageViewCreateInfo view_info{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = swap_format_;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u,
                                  1u};
    if (vkCreateImageView(device_, &view_info, nullptr, &out.view) !=
        VK_SUCCESS) {
      destroyImage(out);
      return false;
    }
    out.width = width;
    out.height = height;
    return true;
  }

void VulkanBackend::readCompletedTimestamps(FrameSync &frame) {
    stats_.gpu_timestamp_valid = false;
    stats_.gpu_timestamp_total_ms = 0.0f;
    stats_.gpu_timestamp_ui_ms = 0.0f;
    stats_.gpu_timestamp_opaque_ms = 0.0f;
    stats_.gpu_timestamp_transparent_ms = 0.0f;
    stats_.gpu_timestamp_lines_ms = 0.0f;
    stats_.gpu_as_build_ms = 0.0f;
    stats_.gpu_path_trace_ms = 0.0f;
    stats_.gpu_timestamp_valid_bits = timestamp_valid_bits_;
    stats_.gpu_timestamp_period_ns = static_cast<float>(timestamp_period_ns_);
    stats_.gpu_ms = 0.0f;

    if (!timestamp_queries_enabled_ || !frame.timestamp_pool ||
        !frame.timestamps_pending) {
      return;
    }

    std::array<std::uint64_t, kGpuTimestampQueryCount> ticks{};
    const VkResult result = vkGetQueryPoolResults(
        device_, frame.timestamp_pool, 0, kGpuTimestampQueryCount,
        sizeof(ticks), ticks.data(), sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT);
    frame.timestamps_pending = false;
    if (result != VK_SUCCESS) {
      if (!timestamp_read_error_logged_) {
        SDL_Log("Vulkan timestamp query read failed: %d",
                static_cast<int>(result));
        timestamp_read_error_logged_ = true;
      }
      return;
    }

    auto milliseconds = [&](GpuTimestampQuery begin, GpuTimestampQuery end) {
      return static_cast<float>(gpuTimestampDeltaMilliseconds(
          ticks[queryIndex(begin)], ticks[queryIndex(end)],
          timestamp_valid_bits_, timestamp_period_ns_));
    };
    stats_.gpu_timestamp_total_ms = milliseconds(GpuTimestampQuery::FrameBegin,
                                                 GpuTimestampQuery::FrameEnd);
    stats_.gpu_timestamp_ui_ms =
        milliseconds(GpuTimestampQuery::FrameBegin, GpuTimestampQuery::UiEnd);
    stats_.gpu_timestamp_opaque_ms =
        milliseconds(GpuTimestampQuery::UiEnd, GpuTimestampQuery::OpaqueEnd);
    stats_.gpu_timestamp_transparent_ms = milliseconds(
        GpuTimestampQuery::OpaqueEnd, GpuTimestampQuery::TransparentEnd);
    stats_.gpu_timestamp_lines_ms = milliseconds(
        GpuTimestampQuery::TransparentEnd, GpuTimestampQuery::LinesEnd);
    stats_.gpu_as_build_ms =
        milliseconds(GpuTimestampQuery::AsBegin, GpuTimestampQuery::AsEnd);
    stats_.gpu_path_trace_ms = milliseconds(GpuTimestampQuery::PathTraceBegin,
                                            GpuTimestampQuery::PathTraceEnd);
    stats_.gpu_timestamp_valid = true;
    stats_.gpu_ms = stats_.gpu_timestamp_total_ms;
  }

bool VulkanBackend::pickDevice() {
    uint32_t count = 0;
    const VkResult count_result =
        streamline_vulkan_runtime_.enumeratePhysicalDevices(
            instance_, &count, nullptr);
    if (count_result != VK_SUCCESS || count == 0) {
      writeLog("No Vulkan devices");
      return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    const VkResult enumerate_result =
        streamline_vulkan_runtime_.enumeratePhysicalDevices(
            instance_, &count, devs.data());
    if (enumerate_result != VK_SUCCESS) {
      SDL_Log("Vulkan physical-device enumeration failed: %d",
              static_cast<int>(enumerate_result));
      return false;
    }
    for (auto d : devs) {
      if (!supportsRequiredDeviceExtensions(d)) {
        continue;
      }
      uint32_t qcount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, nullptr);
      if (qcount == 0) {
        continue;
      }
      std::vector<VkQueueFamilyProperties> qs(qcount);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, qs.data());
      std::vector<VulkanQueueFamilySupport> queue_support(qcount);
      for (uint32_t i = 0; i < qcount; ++i) {
        queue_support[i].graphics =
            (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        VkBool32 support = VK_FALSE;
        const VkResult support_result =
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &support);
        if (support_result != VK_SUCCESS) {
          support = VK_FALSE;
        }
        queue_support[i].present = support == VK_TRUE;
      }
      // Prefer one queue family that can both render and present. Apart from
      // avoiding needless ownership transfers, Streamline's conservative
      // Vulkan DLSS-G mode blocks the presenting client queue while it
      // consumes resources produced by that queue.
      const VulkanQueueFamilySelection queue_selection =
          selectVulkanQueueFamilies(queue_support);
      if (queue_selection.valid()) {
        SwapchainSupport swapchain_support;
        if (!querySupport(d, swapchain_support)) {
          continue;
        }
        phys_ = d;
        graphics_family_ = queue_selection.graphics_family;
        present_family_ = queue_selection.present_family;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        device_name_ = props.deviceName;
        timestamp_valid_bits_ =
            qs[queue_selection.graphics_family].timestampValidBits;
        timestamp_period_ns_ = props.limits.timestampPeriod;
        xpbd::log::infof(
            "Vulkan queues selected: graphics_family=%u present_family=%u "
            "shared=%d",
            graphics_family_, present_family_,
            graphics_family_ == present_family_ ? 1 : 0);
        return true;
      }
    }
    writeLog("No suitable Vulkan device");
    return false;
  }

bool VulkanBackend::supportsDeviceExtension(VkPhysicalDevice device,
                               const char *extension_name) const {
    uint32_t count = 0;
    VkResult result =
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
      return false;
    }
    std::vector<VkExtensionProperties> properties(count);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                                  properties.data());
    if (result != VK_SUCCESS) {
      return false;
    }
    return std::any_of(
        properties.begin(), properties.end(),
        [extension_name](const auto &property) {
          return std::strcmp(property.extensionName, extension_name) == 0;
        });
  }

bool VulkanBackend::supportsRequiredDeviceExtensions(VkPhysicalDevice device) const {
    return supportsDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

[[nodiscard]] bool
  VulkanBackend::deviceHasExtension(const std::vector<VkExtensionProperties> &props,
                     const char *name) {
    return std::any_of(props.begin(), props.end(), [&](const auto &p) {
      return std::strcmp(p.extensionName, name) == 0;
    });
  }

void VulkanBackend::probeRayTracingCapability() {
    rt_capability_ = {};
    if (phys_ == VK_NULL_HANDLE) {
      rt_capability_.unsupported_reason = "No Vulkan physical device";
      return;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_, &props);
    device_name_ = props.deviceName;

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> ext_props(ext_count);
    if (ext_count > 0) {
      vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count,
                                           ext_props.data());
    }

    const bool has_as = deviceHasExtension(
        ext_props, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool has_rtp = deviceHasExtension(
        ext_props, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    const bool has_rq =
        deviceHasExtension(ext_props, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool has_dho = deviceHasExtension(
        ext_props, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    // buffer_device_address is core in Vulkan 1.2; accept extension or 1.2+.
    const bool has_bda_ext = deviceHasExtension(
        ext_props, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    const bool has_bda_core =
        props.apiVersion >= VK_API_VERSION_1_2 || has_bda_ext;
    // Common dependencies for acceleration structures on 1.1 drivers.
    const bool has_desc_indexing = deviceHasExtension(
                                       ext_props,
                                       VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) ||
                                   props.apiVersion >= VK_API_VERSION_1_2;
    const bool has_spirv14 =
        deviceHasExtension(ext_props, VK_KHR_SPIRV_1_4_EXTENSION_NAME) ||
        props.apiVersion >= VK_API_VERSION_1_2;
    const bool has_float_controls =
        deviceHasExtension(ext_props,
                           VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME) ||
        props.apiVersion >= VK_API_VERSION_1_2;

    // Hybrid RT shadows need AS + ray query. Pipeline extension kept for
    // future Kickstart-style effect passes.
    const bool has_exts = has_as && has_rq && has_dho && has_bda_core &&
                          has_desc_indexing && has_spirv14 && has_float_controls &&
                          (has_rtp || has_rq);

    bool has_features = false;
    std::uint32_t max_recursion = 0;
    if (has_exts) {
      VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
      VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp_features{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
      VkPhysicalDeviceRayQueryFeaturesKHR rq_features{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
      VkPhysicalDeviceBufferDeviceAddressFeatures bda_features{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
      as_features.pNext = &rtp_features;
      rtp_features.pNext = &rq_features;
      bda_features.pNext = &as_features;
      VkPhysicalDeviceFeatures2 features2{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features2.pNext = &bda_features;
      vkGetPhysicalDeviceFeatures2(phys_, &features2);
      has_features = as_features.accelerationStructure == VK_TRUE &&
                     rq_features.rayQuery == VK_TRUE &&
                     bda_features.bufferDeviceAddress == VK_TRUE &&
                     (rtp_features.rayTracingPipeline == VK_TRUE ||
                      rq_features.rayQuery == VK_TRUE);

      if (has_rtp) {
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp_props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &rtp_props;
        vkGetPhysicalDeviceProperties2(phys_, &props2);
        max_recursion = rtp_props.maxRayRecursionDepth;
      } else {
        max_recursion = 1;
      }
    }

    rt_capability_ = evaluateRayTracingCapability(
        props.vendorID, props.deviceID, props.deviceName, has_exts, has_features,
        max_recursion, props.apiVersion, props.driverVersion);
  }

bool VulkanBackend::createDevice() {
    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    std::vector<uint32_t> families = {graphics_family_};
    if (present_family_ != graphics_family_) {
      families.push_back(present_family_);
    }
    for (uint32_t f : families) {
      VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      q.queueFamilyIndex = f;
      q.queueCount = 1;
      q.pQueuePriorities = &prio;
      qcis.push_back(q);
    }

    std::vector<const char *> device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    memory_budget_supported_ = supportsDeviceExtension(
        phys_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memory_budget_supported_) {
      device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    const char *maintenance_extension = nullptr;
    if (surface_maintenance1_khr_enabled_ &&
        supportsDeviceExtension(
            phys_, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
      maintenance_extension =
          VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    } else if (surface_maintenance1_ext_enabled_ &&
               supportsDeviceExtension(
                   phys_, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
      maintenance_extension =
          VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    }

    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
    if (maintenance_extension != nullptr) {
      VkPhysicalDeviceFeatures2 features{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features.pNext = &maintenance_features;
      vkGetPhysicalDeviceFeatures2(phys_, &features);
      if (maintenance_features.swapchainMaintenance1 == VK_TRUE) {
        device_extensions.push_back(maintenance_extension);
        swapchain_maintenance1_enabled_ = true;
        swapchain_maintenance1_extension_ = maintenance_extension;
      }
    }
    maintenance_features.swapchainMaintenance1 =
        swapchain_maintenance1_enabled_ ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    bool synchronization2_enabled = false;
    if (supportsDeviceExtension(
            phys_, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
      VkPhysicalDeviceFeatures2 features{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features.pNext = &synchronization2_features;
      vkGetPhysicalDeviceFeatures2(phys_, &features);
      if (synchronization2_features.synchronization2 == VK_TRUE) {
        device_extensions.push_back(
            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        synchronization2_enabled = true;
      }
    }
    synchronization2_features.synchronization2 =
        synchronization2_enabled ? VK_TRUE : VK_FALSE;

    // Always need swapchain. Optionally enable NVIDIA RT extension stack when
    // the selected device is NVIDIA and supports hardware ray tracing.
    VkPhysicalDeviceBufferDeviceAddressFeatures bda_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rq_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceFeatures supported_core_features{};
    vkGetPhysicalDeviceFeatures(phys_, &supported_core_features);
    VkPhysicalDeviceDescriptorIndexingFeatures
        supported_descriptor_indexing_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceFeatures2 descriptor_indexing_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    descriptor_indexing_query.pNext =
        &supported_descriptor_indexing_features;
    vkGetPhysicalDeviceFeatures2(phys_, &descriptor_indexing_query);
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceFeatures enabled_core_features{};
    enabled_core_features.shaderStorageImageExtendedFormats =
        supported_core_features.shaderStorageImageExtendedFormats;
    storage_image_extended_formats_enabled_ =
        enabled_core_features.shaderStorageImageExtendedFormats == VK_TRUE;
    void *features_chain = nullptr;
    void *optional_feature_chain = nullptr;
    if (synchronization2_enabled) {
      synchronization2_features.pNext = optional_feature_chain;
      optional_feature_chain = &synchronization2_features;
    }
    if (swapchain_maintenance1_enabled_) {
      maintenance_features.pNext = optional_feature_chain;
      optional_feature_chain = &maintenance_features;
    }
    const bool enable_rt = rt_capability_.supported;
    descriptor_binding_partially_bound_enabled_ =
        enable_rt &&
        supported_descriptor_indexing_features
                .descriptorBindingPartiallyBound == VK_TRUE;
    if (descriptor_binding_partially_bound_enabled_) {
      descriptor_indexing_features.descriptorBindingPartiallyBound = VK_TRUE;
      descriptor_indexing_features.pNext = optional_feature_chain;
      optional_feature_chain = &descriptor_indexing_features;
    }
    bool has_rtp_ext = false;
    if (enable_rt) {
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(phys_, &props);

      device_extensions.push_back(
          VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
      device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      device_extensions.push_back(
          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
      // Pipeline extension optional but preferred for future RT passes.
      uint32_t ext_count = 0;
      vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count, nullptr);
      std::vector<VkExtensionProperties> ext_props(ext_count);
      if (ext_count > 0) {
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ext_count,
                                             ext_props.data());
      }
      has_rtp_ext = deviceHasExtension(
          ext_props, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
      if (has_rtp_ext) {
        device_extensions.push_back(
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
      }
      if (props.apiVersion < VK_API_VERSION_1_2) {
        device_extensions.push_back(
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        device_extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
        device_extensions.push_back(
            VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
      }

      bda_features.bufferDeviceAddress = VK_TRUE;
      as_features.accelerationStructure = VK_TRUE;
      rq_features.rayQuery = VK_TRUE;
      rtp_features.rayTracingPipeline = has_rtp_ext ? VK_TRUE : VK_FALSE;
      as_features.pNext = has_rtp_ext ? static_cast<void *>(&rtp_features)
                                     : static_cast<void *>(&rq_features);
      if (has_rtp_ext) {
        rtp_features.pNext = &rq_features;
      }
      rq_features.pNext = optional_feature_chain;
      bda_features.pNext = &as_features;
      features_chain = &bda_features;
    } else {
      features_chain = optional_feature_chain;
    }

    auto try_create = [&](const std::vector<const char *> &exts,
                          void *pnext) -> bool {
      VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      di.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
      di.pQueueCreateInfos = qcis.data();
      di.enabledExtensionCount = static_cast<uint32_t>(exts.size());
      di.ppEnabledExtensionNames = exts.data();
      di.pEnabledFeatures = &enabled_core_features;
      di.pNext = pnext;
      const VkResult r = streamline_vulkan_runtime_.createDevice(
          instance_, phys_, &di, nullptr, &device_);
      if (r != VK_SUCCESS) {
        device_ = VK_NULL_HANDLE;
        return false;
      }
      enabled_device_extensions_.clear();
      enabled_device_extensions_.reserve(exts.size());
      for (const char *extension : exts) {
        if (extension != nullptr) {
          enabled_device_extensions_.emplace_back(extension);
        }
      }
      return true;
    };

    bool created = try_create(device_extensions, features_chain);
    bool rt_armed = enable_rt && created;
    if (!created && enable_rt) {
      // RT feature/extension combination rejected — fall back to raster-only.
      writeLog("Vulkan RT device create failed; falling back to rasterization");
      rt_capability_.supported = false;
      rt_capability_.device_extensions_enabled = false;
      rt_capability_.unsupported_reason =
          "Driver rejected Vulkan RT feature chain; using rasterization";
      descriptor_binding_partially_bound_enabled_ = false;
      std::vector<const char *> raster_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
      if (memory_budget_supported_) {
        raster_exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
      }
      void *raster_features = nullptr;
      if (synchronization2_enabled) {
        raster_exts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        synchronization2_features.pNext = nullptr;
        raster_features = &synchronization2_features;
      }
      if (swapchain_maintenance1_enabled_) {
        raster_exts.push_back(maintenance_extension);
        maintenance_features.pNext = raster_features;
        raster_features = &maintenance_features;
      }
      created = try_create(raster_exts, raster_features);
      rt_armed = false;
    }
    if (!created && swapchain_maintenance1_enabled_) {
      writeLog("Vulkan swapchain maintenance1 device create failed; retrying "
               "with the core swapchain extension");
      swapchain_maintenance1_enabled_ = false;
      swapchain_maintenance1_extension_.clear();
      std::vector<const char *> core_exts = {
          VK_KHR_SWAPCHAIN_EXTENSION_NAME};
      if (memory_budget_supported_) {
        core_exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
      }
      void *core_features = nullptr;
      if (synchronization2_enabled) {
        core_exts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        synchronization2_features.pNext = nullptr;
        core_features = &synchronization2_features;
      }
      created = try_create(core_exts, core_features);
      rt_armed = false;
    }
    if (!created) {
      writeLog("vkCreateDevice failed");
      return false;
    }

    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);
    if (swapchain_maintenance1_enabled_) {
      xpbd::log::infof("Vulkan present-fence lifecycle enabled via %s",
                       swapchain_maintenance1_extension_.c_str());
    } else {
      xpbd::log::warn(
          "Vulkan swapchain maintenance1 unavailable; shutdown/recreate "
          "uses the unextended WaitIdle best-effort fallback");
    }

    rt_capability_.device_extensions_enabled = rt_armed;
    if (rt_armed) {
      writeLog("Vulkan device created with NVIDIA RT extensions enabled");
    }
    return true;
  }

bool VulkanBackend::querySupport(VkPhysicalDevice device, SwapchainSupport &s) const {
    s = {};
    VkResult result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &s.caps);
    if (result != VK_SUCCESS) {
      return false;
    }
    uint32_t fc = 0, mc = 0;
    result =
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &fc, nullptr);
    if (result != VK_SUCCESS || fc == 0) {
      return false;
    }
    s.formats.resize(fc);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &fc,
                                                  s.formats.data());
    if (result != VK_SUCCESS || fc == 0) {
      return false;
    }
    s.formats.resize(fc);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &mc,
                                                       nullptr);
    if (result != VK_SUCCESS || mc == 0) {
      return false;
    }
    s.modes.resize(mc);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        device, surface_, &mc, s.modes.data());
    if (result != VK_SUCCESS || mc == 0) {
      return false;
    }
    s.modes.resize(mc);
    return !s.formats.empty() && !s.modes.empty();
  }

bool VulkanBackend::createSwapchain(
      VkSwapchainKHR old_swapchain,
      SwapchainOwnership target_ownership) {
    SwapchainSupport support;
    if (!querySupport(phys_, support)) {
      writeLog("Vulkan swapchain surface has no usable formats/present modes");
      return false;
    }
    VkSurfaceFormatKHR format = support.formats[0];
    bool preferred_format_found = false;
    if (support.formats.size() == 1 &&
        support.formats[0].format == VK_FORMAT_UNDEFINED) {
      format.format = VK_FORMAT_B8G8R8A8_UNORM;
      format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      preferred_format_found = true;
    }
    for (const auto &f : support.formats) {
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
        format = f;
        preferred_format_found = true;
        break;
      }
    }
    if (!preferred_format_found) {
      for (const auto &f : support.formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB) {
          format = f;
          break;
        }
      }
    }
    swap_format_ = format.format;
    fg_swapchain_color_format_supported_ =
        frameGenerationColorFormatSupported(swap_format_);



    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (vsync_) {
      bool has_fifo = false;
      for (auto m : support.modes) {
        if (m == VK_PRESENT_MODE_FIFO_KHR) {
          has_fifo = true;
          break;
        }
      }
      if (!has_fifo && !support.modes.empty()) {
        mode = support.modes[0];
      }
    } else {
      bool found = false;
      for (auto prefer :
           {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_FIFO_RELAXED_KHR}) {
        for (auto m : support.modes) {
          if (m == prefer) {
            mode = m;
            found = true;
            break;
          }
        }
        if (found) {
          break;
        }
      }
      if (!found) {
        xpbd::log::warn("Vulkan: no non-VSync present mode "
                        "(IMMEDIATE/MAILBOX); display may stay "
                        "locked");
      } else {
        xpbd::log::infof("Vulkan: VSync off, present mode=%u",
                         static_cast<unsigned>(mode));
      }
    }
    swap_present_mode_ = mode;
    if (target_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy &&
        mode != VK_PRESENT_MODE_IMMEDIATE_KHR) {
      xpbd::log::warn(
          "DLSS Frame Generation requires VK_PRESENT_MODE_IMMEDIATE_KHR; "
          "keeping FG disabled");
      return false;
    }
    if (support.caps.currentExtent.width != UINT32_MAX) {
      swap_extent_ = support.caps.currentExtent;
    } else {
      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(window_, &w, &h);
      if (w <= 0 || h <= 0) {
        return false;
      }
      swap_extent_.width = std::clamp(static_cast<uint32_t>(w),
                                      support.caps.minImageExtent.width,
                                      support.caps.maxImageExtent.width);
      swap_extent_.height = std::clamp(static_cast<uint32_t>(h),
                                       support.caps.minImageExtent.height,
                                       support.caps.maxImageExtent.height);
    }
    if (swap_extent_.width == 0 || swap_extent_.height == 0) {
      return false;
    }
    fg_swapchain_transfer_src_supported_ =
        (support.caps.supportedUsageFlags &
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (target_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy &&
        (!fg_swapchain_transfer_src_supported_ ||
         !fg_swapchain_color_format_supported_ ||
         graphics_family_ != present_family_)) {
      xpbd::log::warn(
          "DLSS Frame Generation proxy requirements are unavailable for the "
          "selected swapchain format/queue; keeping FG disabled");
      return false;
    }
    fg_swapchain_resources_ready_ = false;
    bool allocate_fg_resources = false;


    uint32_t images = support.caps.minImageCount;
    if (images < 2) {
      images = 2;
    }
    if (support.caps.maxImageCount > 0 && images > support.caps.maxImageCount) {
      images = support.caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = images;
    ci.imageFormat = swap_format_;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = swap_extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fg_swapchain_transfer_src_supported_) {
      ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    uint32_t qf[] = {graphics_family_, present_family_};
    if (graphics_family_ != present_family_) {
      ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      ci.queueFamilyIndexCount = 2;
      ci.pQueueFamilyIndices = qf;
    } else {
      ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    ci.preTransform = support.caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((support.caps.supportedCompositeAlpha & ci.compositeAlpha) == 0) {
      for (const VkCompositeAlphaFlagBitsKHR candidate :
           {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if ((support.caps.supportedCompositeAlpha & candidate) != 0) {
          ci.compositeAlpha = candidate;
          break;
        }
      }
    }
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old_swapchain;
    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    SwapchainOwnership actual_ownership = SwapchainOwnership::Native;
    const VkResult create_swapchain =
        streamline_vulkan_runtime_.createSwapchain(
            device_, &ci, nullptr, &new_swapchain, &actual_ownership);
    if (create_swapchain != VK_SUCCESS) {
      SDL_Log("Vulkan swapchain creation failed: %d",
              static_cast<int>(create_swapchain));
      return false;
    }
    swapchain_ = new_swapchain;
    if (target_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy &&
        actual_ownership !=
            SwapchainOwnership::StreamlineFrameGenerationProxy) {
      xpbd::log::warn(
          "DLSS Frame Generation proxy was not armed; keeping a Native "
          "swapchain");
    }
    allocate_fg_resources =
        actual_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy;
    if (actual_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy &&
        swapchain_maintenance1_enabled_) {
      xpbd::log::info(
          "Vulkan present-fence lifecycle disabled for the asynchronous "
          "DLSS-G swapchain proxy");
    }

    uint32_t ic = 0;
    VkResult images_result =
        streamline_vulkan_runtime_.getSwapchainImages(
            device_, swapchain_, &ic, nullptr);
    if (images_result != VK_SUCCESS || ic == 0) {
      SDL_Log("Vulkan swapchain image-count query failed: %d",
              static_cast<int>(images_result));
      if (!destroySwapchainObjects()) {
        return false;
      }
      return false;
    }
    swap_images_.resize(ic);
    images_result =
        streamline_vulkan_runtime_.getSwapchainImages(
            device_, swapchain_, &ic, swap_images_.data());
    if (images_result != VK_SUCCESS || ic == 0) {
      SDL_Log("Vulkan swapchain image query failed: %d",
              static_cast<int>(images_result));
      if (!destroySwapchainObjects()) {
        return false;
      }
      return false;
    }
    swap_images_.resize(ic);
    swap_views_.resize(ic);
    swap_image_resources_.resize(ic);
    VkSemaphoreCreateInfo semaphore_info{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo present_fence_info{
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (uint32_t i = 0; i < ic; ++i) {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = swap_images_[i];
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = swap_format_;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkResult result =
          vkCreateImageView(device_, &vi, nullptr, &swap_views_[i]);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan swapchain image-view creation failed: %d",
                static_cast<int>(result));
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }

      SwapchainImageResource &resource = swap_image_resources_[i];
      result = vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                 &resource.render_finished);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan present semaphore creation failed: %d",
                static_cast<int>(result));
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }
      if (presentFenceLifecycleEnabled()) {
        result = vkCreateFence(device_, &present_fence_info, nullptr,
                               &resource.present_fence);
        if (result != VK_SUCCESS) {
          SDL_Log("Vulkan present fence creation failed: %d",
                  static_cast<int>(result));
          if (!destroySwapchainObjects()) {
            return false;
          }
          return false;
        }
      }

      VkImageCreateInfo depth_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      depth_info.imageType = VK_IMAGE_TYPE_2D;
      depth_info.format = VK_FORMAT_D32_SFLOAT;
      depth_info.extent = {swap_extent_.width, swap_extent_.height, 1};
      depth_info.mipLevels = 1;
      depth_info.arrayLayers = 1;
      depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
      depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      result = vkCreateImage(device_, &depth_info, nullptr,
                             &resource.depth_image);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan depth image creation failed: %d",
                static_cast<int>(result));
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(device_, resource.depth_image,
                                   &requirements);
      const auto memory_type = findMemoryType(
          requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!memory_type) {
        writeLog("Vulkan depth image has no compatible memory type");
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }
      VkMemoryAllocateInfo allocation{
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize = requirements.size;
      allocation.memoryTypeIndex = *memory_type;
      result = vkAllocateMemory(device_, &allocation, nullptr,
                                &resource.depth_memory);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan depth memory allocation failed: %d",
                static_cast<int>(result));
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }
      result = vkBindImageMemory(device_, resource.depth_image,
                                 resource.depth_memory, 0);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan depth memory bind failed: %d",
                static_cast<int>(result));
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }
      VkImageViewCreateInfo depth_view_info{
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      depth_view_info.image = resource.depth_image;
      depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      depth_view_info.format = VK_FORMAT_D32_SFLOAT;
      depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0,
                                          1};
      result = vkCreateImageView(device_, &depth_view_info, nullptr,
                                 &resource.depth_view);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan depth image-view creation failed: %d",
                static_cast<int>(result));
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }
      if (allocate_fg_resources) {
        const bool hudless_ok = createFrameGenerationImage(
            swap_extent_.width, swap_extent_.height,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            resource.fg_hudless);
        const bool ui_ok = hudless_ok && createFrameGenerationImage(
                                             swap_extent_.width,
                                             swap_extent_.height,
                                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                                             resource.fg_ui);
        if (!ui_ok) {
          destroyImage(resource.fg_hudless);
          destroyImage(resource.fg_ui);
          allocate_fg_resources = false;
          xpbd::log::warn(
              "Vulkan DLSS Frame Generation guide images could not be "
              "allocated; keeping FG disabled");
        }
      }
    }
    if (!allocate_fg_resources) {
      for (auto &resource : swap_image_resources_) {
        destroyImage(resource.fg_hudless);
        destroyImage(resource.fg_ui);
      }
    } else {
      fg_swapchain_resources_ready_ = true;
    }
    xpbd::log::infof(
        "Vulkan DLSS-G capability: runtime=%d transfer_src=%d color_format=%d "
        "shared_queue=%d guide_images=%d vsync=%d present_mode=%u",
        streamline_vulkan_runtime_.frameGenerationSupported() ? 1 : 0,
        fg_swapchain_transfer_src_supported_ ? 1 : 0,
        fg_swapchain_color_format_supported_ ? 1 : 0,
        graphics_family_ == present_family_ ? 1 : 0,
        fg_swapchain_resources_ready_ ? 1 : 0, vsync_ ? 1 : 0,
        static_cast<unsigned>(swap_present_mode_));
    return true;
  }

bool VulkanBackend::waitForPendingPresentFences(const char *reason) {
    if (!presentFenceLifecycleEnabled()) {
      return true;
    }
    const std::string stage =
        std::string("vkWaitForFences.present_") + reason;
    for (std::uint32_t image_index = 0;
         image_index < swap_image_resources_.size(); ++image_index) {
      SwapchainImageResource &resource =
          swap_image_resources_[image_index];
      if (!resource.present_pending) {
        continue;
      }
      if (!resource.present_fence) {
        writeLog("Vulkan pending present has no completion fence");
        return false;
      }

      const auto wait_start = Clock::now();
      logDiagnosticApi(stage.c_str(), "before", std::nullopt, 0.0,
                       image_index, resource.present_fence,
                       resource.last_in_flight, VK_NULL_HANDLE, true, true);
      VkResult result = VK_SUCCESS;
      if (!diagnostics_enabled_) {
        result = vkWaitForFences(device_, 1, &resource.present_fence, VK_TRUE,
                                 UINT64_MAX);
      } else {
        do {
          result =
              vkWaitForFences(device_, 1, &resource.present_fence, VK_TRUE,
                              kDiagnosticWaitSliceNs);
          if (result == VK_TIMEOUT) {
            logDiagnosticApi(
                stage.c_str(), "timeout", result,
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          wait_start)
                    .count(),
                image_index, resource.present_fence,
                resource.last_in_flight, VK_NULL_HANDLE, true, true);
          }
        } while (result == VK_TIMEOUT);
      }
      logDiagnosticApi(
          stage.c_str(), "after", result,
          std::chrono::duration<double, std::milli>(Clock::now() - wait_start)
              .count(),
          image_index, resource.present_fence, resource.last_in_flight,
          VK_NULL_HANDLE, true, false);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan present fence wait during %s failed: %d", reason,
                static_cast<int>(result));
        return false;
      }
      resource.present_pending = false;
    }
    return true;
  }

void VulkanBackend::destroySwapchainImageObjects() {
    for (auto fb : framebuffers_) {
      vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();
    for (auto v : swap_views_) {
      vkDestroyImageView(device_, v, nullptr);
    }
    swap_views_.clear();
    for (auto &resource : swap_image_resources_) {
      if (resource.render_finished) {
        vkDestroySemaphore(device_, resource.render_finished, nullptr);
      }
      if (resource.present_fence) {
        vkDestroyFence(device_, resource.present_fence, nullptr);
      }
      if (resource.fg_ui_framebuffer) {
        vkDestroyFramebuffer(device_, resource.fg_ui_framebuffer, nullptr);
      }
      if (resource.fg_overlay_framebuffer) {
        vkDestroyFramebuffer(device_, resource.fg_overlay_framebuffer,
                             nullptr);
      }
      if (resource.depth_view) {
        vkDestroyImageView(device_, resource.depth_view, nullptr);
      }
      destroyImage(resource.fg_hudless);
      destroyImage(resource.fg_ui);
      resource.fg_hudless_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      resource.fg_ui_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (resource.depth_image) {
        vkDestroyImage(device_, resource.depth_image, nullptr);
      }
      if (resource.depth_memory) {
        vkFreeMemory(device_, resource.depth_memory, nullptr);
      }
    }
    swap_image_resources_.clear();
    swap_images_.clear();
    fg_swapchain_resources_ready_ = false;
  }

bool VulkanBackend::destroySwapchainObjects() {
    if (swapchain_ != VK_NULL_HANDLE ||
        streamline_vulkan_runtime_.swapchainOwnership() ==
            SwapchainOwnership::StreamlineFrameGenerationProxy) {
      if (!streamline_vulkan_runtime_.notifyFrameGenerationSwapchainDestroyed(
              static_cast<std::uint64_t>(frame_index_),
              "swapchain destroyed")) {
        xpbd::log::error(
            "DLSS Frame Generation blocked unsafe swapchain destruction");
        return false;
      }
    }
    destroySwapchainImageObjects();
    if (swapchain_) {
      streamline_vulkan_runtime_.destroySwapchain(
          device_, swapchain_, nullptr);
      swapchain_ = VK_NULL_HANDLE;
    }
    swapchain_recreate_target_ = SwapchainOwnership::Native;
    return true;
  }

bool VulkanBackend::recreateSwapchain() {
    if (!window_) {
      return false;
    }
    SwapchainOwnership target_ownership = swapchain_recreate_target_;
    if (target_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy &&
        (!streamline_vulkan_runtime_.frameGenerationActivationAllowed() ||
         !frameGenerationPlatformSupported() || vsync_)) {
      target_ownership = SwapchainOwnership::Native;
    }
    const SDL_WindowFlags window_flags = SDL_GetWindowFlags(window_);
    if ((window_flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
      return false;
    }
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_GetWindowSizeInPixels(window_, &drawable_width, &drawable_height);
    if (drawable_width <= 0 || drawable_height <= 0) {
      return false;
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    const VkResult capabilities_result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_,
                                                  &capabilities);
    if (capabilities_result != VK_SUCCESS) {
      SDL_Log("Vulkan surface-capabilities query before swapchain recreate "
              "failed: %d",
              static_cast<int>(capabilities_result));
      if (capabilities_result == VK_ERROR_DEVICE_LOST) {
        fatal_error_ = true;
      }
      return false;
    }
    if (capabilities.currentExtent.width != UINT32_MAX &&
        (capabilities.currentExtent.width == 0 ||
         capabilities.currentExtent.height == 0)) {
      return false;
    }

    // Do not mutate Streamline's lifecycle while the window has no drawable
    // surface.  A minimized/hidden retry must leave the current swapchain and
    // its FG state intact until a real transaction can drain and rebuild it.
    const std::uint64_t transition_frame =
        static_cast<std::uint64_t>(frame_index_);
    const char *transition_reason =
        fg_recovery_reason_.empty()
            ? "swapchain recreate"
            : fg_recovery_reason_.c_str();
    const bool frame_generation_disable_started =
        streamline_vulkan_runtime_.beginFrameGenerationSwapchainTransition(
            target_ownership, transition_frame, transition_reason);
    if (!frame_generation_disable_started) {
      fg_force_native_recovery_ = true;
      target_ownership = SwapchainOwnership::Native;
      swapchain_recreate_target_ = SwapchainOwnership::Native;
      fg_recovery_reason_ =
          streamline_vulkan_runtime_.frameGenerationDiagnostic().status;
    }

    bool frame_generation_disable_ready = false;
    for (std::uint32_t drain_cycle = 0u;
         !frame_generation_disable_ready &&
         drain_cycle <= kFrameGenerationDisableMaxAttempts;
         ++drain_cycle) {
      const FrameSync &sync = frames_[frame_index_];
      const auto idle_start = Clock::now();
      logDiagnosticApi("vkDeviceWaitIdle.swapchain_recreate", "before",
                       std::nullopt, 0.0, UINT32_MAX, sync.fence,
                       VK_NULL_HANDLE, sync.cmd, true, true);
      const VkResult idle_result =
          streamline_vulkan_runtime_.deviceWaitIdle(device_);
      logDiagnosticApi(
          "vkDeviceWaitIdle.swapchain_recreate", "after", idle_result,
          std::chrono::duration<double, std::milli>(Clock::now() - idle_start)
              .count(),
          UINT32_MAX, sync.fence, VK_NULL_HANDLE, sync.cmd, true, false);
      if (idle_result != VK_SUCCESS) {
        SDL_Log("Vulkan device idle wait before swapchain recreate failed: %d",
                static_cast<int>(idle_result));
        if (idle_result == VK_ERROR_DEVICE_LOST) {
          fatal_error_ = true;
          fatal_error_detail_ =
              "VK_ERROR_DEVICE_LOST while draining DLSS-G transition";
        }
        return false;
      }
      // Proxy presents do not expose an application maintenance1 fence.
      // Native fences are still drained before their swapchain is destroyed.
      if (!waitForPendingPresentFences("swapchain_recreate")) {
        return false;
      }
      frame_generation_disable_ready =
          streamline_vulkan_runtime_
              .retryFrameGenerationDisableAfterDrain(
                  transition_frame, "swapchain recreate drain completed");
      if (!frame_generation_disable_ready &&
          streamline_vulkan_runtime_.frameGenerationDiagnostic()
              .disable_exhausted) {
        break;
      }
    }
    if (!frame_generation_disable_ready) {
      const FrameGenerationDiagnostic diagnostic =
          streamline_vulkan_runtime_.frameGenerationDiagnostic();
      fg_force_native_recovery_ = true;
      swapchain_recreate_target_ = SwapchainOwnership::Native;
      fg_recovery_reason_ = diagnostic.status;
      xpbd::log::errorf(
          "DLSS Frame Generation swapchain transition blocked: %s",
          diagnostic.status.c_str());
      return false;
    }

    VkSwapchainKHR old_swapchain = swapchain_;
    if (!streamline_vulkan_runtime_.notifyFrameGenerationSwapchainDestroyed(
            transition_frame, "old swapchain destroyed")) {
      return false;
    }
    swapchain_ = VK_NULL_HANDLE;
    destroySwapchainImageObjects();
    if (old_swapchain) {
      streamline_vulkan_runtime_.destroySwapchain(
          device_, old_swapchain, nullptr);
    }

    // NVIDIA's documented transition is strict: destroy the old swapchain,
    // load/unload the DLSS-G feature, then create a new swapchain. Passing a
    // swapchain created under the opposite hook state as oldSwapchain is not
    // valid and can deadlock the first intercepted Present.
    bool effective_proxy_target =
        target_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy;
    if (!streamline_vulkan_runtime_.configureFrameGenerationFeature(
            effective_proxy_target
                ? SwapchainOwnership::StreamlineFrameGenerationProxy
                : SwapchainOwnership::Native,
            transition_frame, transition_reason)) {
      if (effective_proxy_target) {
        effective_proxy_target = false;
        target_ownership = SwapchainOwnership::Native;
        fg_force_native_recovery_ = true;
        fg_recovery_reason_ =
            "DLSS Frame Generation plugin load failed; using Native";
        xpbd::log::warn(fg_recovery_reason_);
        if (!streamline_vulkan_runtime_.configureFrameGenerationFeature(
                SwapchainOwnership::Native, transition_frame,
                fg_recovery_reason_.c_str())) {
          xpbd::log::error(
              "DLSS Frame Generation could not reach a safe Native state");
          return false;
        }
      } else {
        xpbd::log::error(
            "DLSS Frame Generation could not reach a safe Native state");
        return false;
      }
    }

    SwapchainOwnership actual_ownership = SwapchainOwnership::Native;
    bool created = createSwapchain(
        VK_NULL_HANDLE,
        effective_proxy_target
            ? SwapchainOwnership::StreamlineFrameGenerationProxy
            : SwapchainOwnership::Native);
    if (!created) {
      if (!effective_proxy_target) {
        return false;
      }
      // A proxy creation failure must not take down SR/RR/native rendering.
      // Tear down any partial objects, unload DLSS-G, and retry once as
      // Native in the same transaction.
      if (!destroySwapchainObjects()) {
        return false;
      }
      effective_proxy_target = false;
      target_ownership = SwapchainOwnership::Native;
      fg_force_native_recovery_ = true;
      fg_recovery_reason_ =
          "DLSS Frame Generation proxy creation failed; using Native";
      if (!streamline_vulkan_runtime_.configureFrameGenerationFeature(
              SwapchainOwnership::Native, transition_frame,
              fg_recovery_reason_.c_str()) ||
          !createSwapchain(VK_NULL_HANDLE, SwapchainOwnership::Native)) {
        return false;
      }
    }
    actual_ownership = streamline_vulkan_runtime_.swapchainOwnership();
    if (actual_ownership ==
            SwapchainOwnership::StreamlineFrameGenerationProxy &&
        !fg_swapchain_resources_ready_) {
      xpbd::log::warn(
          "DLSS Frame Generation guide resources unavailable; retrying "
          "Native swapchain");
      if (!destroySwapchainObjects()) {
        return false;
      }
      effective_proxy_target = false;
      target_ownership = SwapchainOwnership::Native;
      fg_force_native_recovery_ = true;
      fg_recovery_reason_ =
          "DLSS Frame Generation guide resources unavailable; using Native";
      if (!streamline_vulkan_runtime_.configureFrameGenerationFeature(
              SwapchainOwnership::Native, transition_frame,
              fg_recovery_reason_.c_str()) ||
          !createSwapchain(VK_NULL_HANDLE, SwapchainOwnership::Native)) {
        return false;
      }
      actual_ownership = streamline_vulkan_runtime_.swapchainOwnership();
    }
    const bool rebuild_graphics =
        !render_pass_ || render_pass_format_ != swap_format_ ||
        !graphicsPipelinesReady();
    if (rebuild_graphics) {
      for (auto &path_tracer : path_tracers_) {
        path_tracer.shutdown();
      }
      still_path_tracer_.shutdown();
      still_active_job_id_ = 0;
      still_path_trace_frame_index_ = 0;
      still_waiting_job_id_ = 0;
      still_wait_started_ = {};
      still_progress_job_id_ = 0;
      still_last_logged_samples_ = 0;
      still_last_progress_time_ = {};
      setVulkanPathTraceAvailability(false, false);
      destroyGraphicsPipelines();
      if (fg_overlay_render_pass_) {
        vkDestroyRenderPass(device_, fg_overlay_render_pass_, nullptr);
        fg_overlay_render_pass_ = VK_NULL_HANDLE;
      }
      if (fg_ui_render_pass_) {
        vkDestroyRenderPass(device_, fg_ui_render_pass_, nullptr);
        fg_ui_render_pass_ = VK_NULL_HANDLE;
      }
      if (render_pass_) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
        render_pass_format_ = VK_FORMAT_UNDEFINED;
      }
      if (!createRenderPass() || !createPipelines()) {
        destroyGraphicsPipelines();
        if (fg_overlay_render_pass_) {
          vkDestroyRenderPass(device_, fg_overlay_render_pass_, nullptr);
          fg_overlay_render_pass_ = VK_NULL_HANDLE;
        }
        if (fg_ui_render_pass_) {
          vkDestroyRenderPass(device_, fg_ui_render_pass_, nullptr);
          fg_ui_render_pass_ = VK_NULL_HANDLE;
        }
        if (render_pass_) {
          vkDestroyRenderPass(device_, render_pass_, nullptr);
          render_pass_ = VK_NULL_HANDLE;
          render_pass_format_ = VK_FORMAT_UNDEFINED;
        }
        if (!destroySwapchainObjects()) {
          return false;
        }
        return false;
      }
      if (rt_capability_.device_extensions_enabled && render_pass_) {
        bool path_tracers_ready = true;
        for (auto &path_tracer : path_tracers_) {
          if (!path_tracer.init(
                  phys_, device_, render_pass_, true,
                  descriptor_binding_partially_bound_enabled_)) {
            path_tracers_ready = false;
            break;
          }
        }
        if (!path_tracers_ready) {
          for (auto &path_tracer : path_tracers_) {
            path_tracer.shutdown();
          }
        } else {
          (void)still_path_tracer_.init(
              phys_, device_, render_pass_, false,
              descriptor_binding_partially_bound_enabled_);
        }
      }
      const bool path_tracer_ready =
          std::all_of(path_tracers_.begin(), path_tracers_.end(),
                      [](const VulkanPathTracer &path_tracer) {
                        return path_tracer.ready();
                      });
      const bool rt_pipeline_ready =
          path_tracer_ready &&
          std::all_of(path_tracers_.begin(), path_tracers_.end(),
                      [](const VulkanPathTracer &path_tracer) {
                        return path_tracer.rtPipelineReady();
                      });
      setVulkanPathTraceAvailability(path_tracer_ready, rt_pipeline_ready);
    }
    if (!createFramebuffers()) {
      if (!destroySwapchainObjects()) {
        return false;
      }
      return false;
    }
    actual_ownership = streamline_vulkan_runtime_.swapchainOwnership();
    if (!streamline_vulkan_runtime_.completeFrameGenerationSwapchainTransition(
            actual_ownership,
            actual_ownership ==
                    SwapchainOwnership::StreamlineFrameGenerationProxy &&
                fg_swapchain_resources_ready_,
            transition_frame, transition_reason)) {
      return false;
    }
    if (actual_ownership == SwapchainOwnership::Native) {
      // A forced recovery is consumed only after a complete Native rebuild.
      fg_force_native_recovery_ = false;
      fg_recovery_reason_.clear();
    }
    return true;
  }

bool VulkanBackend::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = swap_format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{1,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cr;
    sub.pDepthStencilAttachment = &dr;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = dep.srcStageMask;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> atts = {color, depth};
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = 2;
    ri.pAttachments = atts.data();
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 1;
    ri.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(device_, &ri, nullptr, &render_pass_));

    // FG guide pass: clear a full-size transparent UI image and leave it in
    // shader-read layout for Streamline's UI recomposition.
    VkAttachmentDescription fg_ui_color = color;
    fg_ui_color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    fg_ui_color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    fg_ui_color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentDescription fg_ui_depth = depth;
    fg_ui_depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    fg_ui_depth.initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    fg_ui_depth.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    std::array<VkAttachmentDescription, 2> fg_ui_atts{
        fg_ui_color, fg_ui_depth};
    VkRenderPassCreateInfo fg_ui_ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    fg_ui_ri.attachmentCount = 2;
    fg_ui_ri.pAttachments = fg_ui_atts.data();
    fg_ui_ri.subpassCount = 1;
    fg_ui_ri.pSubpasses = &sub;
    fg_ui_ri.dependencyCount = 1;
    fg_ui_ri.pDependencies = &dep;
    if (vkCreateRenderPass(device_, &fg_ui_ri, nullptr,
                           &fg_ui_render_pass_) != VK_SUCCESS) {
      vkDestroyRenderPass(device_, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
      return false;
    }

    // Final overlay pass: load the intercepted swapchain image after the
    // scene/HUD-less copy and draw only the native UI overlay.
    VkAttachmentDescription fg_overlay_color = color;
    fg_overlay_color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    fg_overlay_color.initialLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    fg_overlay_color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentDescription fg_overlay_depth = depth;
    fg_overlay_depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    fg_overlay_depth.initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    fg_overlay_depth.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    std::array<VkAttachmentDescription, 2> fg_overlay_atts{
        fg_overlay_color, fg_overlay_depth};
    VkRenderPassCreateInfo fg_overlay_ri{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    fg_overlay_ri.attachmentCount = 2;
    fg_overlay_ri.pAttachments = fg_overlay_atts.data();
    fg_overlay_ri.subpassCount = 1;
    fg_overlay_ri.pSubpasses = &sub;
    fg_overlay_ri.dependencyCount = 1;
    fg_overlay_ri.pDependencies = &dep;
    if (vkCreateRenderPass(device_, &fg_overlay_ri, nullptr,
                           &fg_overlay_render_pass_) != VK_SUCCESS) {
      vkDestroyRenderPass(device_, fg_ui_render_pass_, nullptr);
      fg_ui_render_pass_ = VK_NULL_HANDLE;
      vkDestroyRenderPass(device_, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
      return false;
    }
    render_pass_format_ = swap_format_;
    return true;
  }

bool VulkanBackend::createFramebuffers() {
    if (swap_views_.size() != swap_image_resources_.size() ||
        swap_views_.empty()) {
      writeLog("Vulkan framebuffer resources do not match swapchain images");
      return false;
    }
    framebuffers_.resize(swap_views_.size());
    for (size_t i = 0; i < swap_views_.size(); ++i) {
      std::array<VkImageView, 2> atts = {
          swap_views_[i], swap_image_resources_[i].depth_view};
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fi.renderPass = render_pass_;
      fi.attachmentCount = 2;
      fi.pAttachments = atts.data();
      fi.width = swap_extent_.width;
      fi.height = swap_extent_.height;
      fi.layers = 1;
      VK_CHECK(vkCreateFramebuffer(device_, &fi, nullptr, &framebuffers_[i]));
      if (fg_swapchain_resources_ready_) {
        std::array<VkImageView, 2> fg_ui_atts{
            swap_image_resources_[i].fg_ui.view,
            swap_image_resources_[i].depth_view};
        fi.renderPass = fg_ui_render_pass_;
        fi.pAttachments = fg_ui_atts.data();
        VK_CHECK(vkCreateFramebuffer(
            device_, &fi, nullptr,
            &swap_image_resources_[i].fg_ui_framebuffer));

        std::array<VkImageView, 2> fg_overlay_atts{
            swap_views_[i], swap_image_resources_[i].depth_view};
        fi.renderPass = fg_overlay_render_pass_;
        fi.pAttachments = fg_overlay_atts.data();
        VK_CHECK(vkCreateFramebuffer(
            device_, &fi, nullptr,
            &swap_image_resources_[i].fg_overlay_framebuffer));
      }
    }
    return true;
  }

bool VulkanBackend::createDescriptors() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr, &desc_layout_));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    pi.maxSets = 1;
    VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_));
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &desc_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &desc_set_));

    std::array<VkDescriptorSetLayoutBinding, 4> static_bindings{};
    static_bindings[0].binding = 0;
    static_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    static_bindings[0].descriptorCount = 1;
    static_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    static_bindings[1].binding = 1;
    static_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    static_bindings[1].descriptorCount = 1;
    static_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (std::uint32_t binding = 2; binding <= 3; ++binding) {
      static_bindings[binding].binding = binding;
      static_bindings[binding].descriptorType =
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      static_bindings[binding].descriptorCount = 1;
      static_bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = static_cast<std::uint32_t>(static_bindings.size());
    li.pBindings = static_bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr,
                                         &static_desc_layout_));

    std::array<VkDescriptorPoolSize, 2> static_pool_sizes{};
    static_pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            static_cast<std::uint32_t>(frames_.size())};
    static_pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                            static_cast<std::uint32_t>(frames_.size() * 3u)};
    pi.poolSizeCount = static_cast<std::uint32_t>(static_pool_sizes.size());
    pi.pPoolSizes = static_pool_sizes.data();
    pi.maxSets = static_cast<std::uint32_t>(frames_.size());
    VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &static_desc_pool_));

    std::array<VkDescriptorSetLayout, 2> static_layouts = {static_desc_layout_,
                                                           static_desc_layout_};
    std::array<VkDescriptorSet, 2> static_sets{};
    ai.descriptorPool = static_desc_pool_;
    ai.descriptorSetCount = static_cast<std::uint32_t>(static_sets.size());
    ai.pSetLayouts = static_layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, static_sets.data()));
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      frames_[i].static_descriptor_set = static_sets[i];
    }

    VkSamplerCreateInfo static_sampler_info{
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    static_sampler_info.magFilter = VK_FILTER_NEAREST;
    static_sampler_info.minFilter = VK_FILTER_NEAREST;
    // Preserve nearest texel lookup inside each atlas mip while blending
    // continuously between adjacent ray-cone LODs.
    static_sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    static_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    static_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    static_sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    static_sampler_info.minLod = 0.0f;
    static_sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_, &static_sampler_info, nullptr,
                        &static_albedo_sampler_) != VK_SUCCESS) {
      return false;
    }
    if (vkCreateSampler(device_, &static_sampler_info, nullptr,
                        &static_normal_sampler_) != VK_SUCCESS) {
      destroyStaticMaterialSamplers();
      return false;
    }
    if (vkCreateSampler(device_, &static_sampler_info, nullptr,
                        &static_specular_sampler_) != VK_SUCCESS) {
      destroyStaticMaterialSamplers();
      return false;
    }

    // Hybrid RT descriptors (ray-query acceleration structure bindings).
    if (rt_capability_.device_extensions_enabled) {
      // mesh_rt: set0 binding0 = TLAS
      VkDescriptorSetLayoutBinding mesh_rt_b{};
      mesh_rt_b.binding = 0;
      mesh_rt_b.descriptorType =
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      mesh_rt_b.descriptorCount = 1;
      mesh_rt_b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      li.bindingCount = 1;
      li.pBindings = &mesh_rt_b;
      VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr,
                                            &mesh_rt_desc_layout_));
      VkDescriptorPoolSize mesh_rt_ps{
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
          static_cast<std::uint32_t>(frames_.size())};
      pi.poolSizeCount = 1;
      pi.pPoolSizes = &mesh_rt_ps;
      pi.maxSets = static_cast<std::uint32_t>(frames_.size());
      VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &mesh_rt_desc_pool_));
      std::array<VkDescriptorSetLayout, 2> mesh_rt_layouts = {
          mesh_rt_desc_layout_, mesh_rt_desc_layout_};
      ai.descriptorPool = mesh_rt_desc_pool_;
      ai.descriptorSetCount =
          static_cast<std::uint32_t>(mesh_rt_desc_sets_.size());
      ai.pSetLayouts = mesh_rt_layouts.data();
      VK_CHECK(vkAllocateDescriptorSets(device_, &ai,
                                        mesh_rt_desc_sets_.data()));

      // static_rt: bone SSBO + base texture + TLAS + normal/specular sidecars
      std::array<VkDescriptorSetLayoutBinding, 5> static_rt_bindings{};
      static_rt_bindings[0].binding = 0;
      static_rt_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      static_rt_bindings[0].descriptorCount = 1;
      static_rt_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
      static_rt_bindings[1].binding = 1;
      static_rt_bindings[1].descriptorType =
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      static_rt_bindings[1].descriptorCount = 1;
      static_rt_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      static_rt_bindings[2].binding = 2;
      static_rt_bindings[2].descriptorType =
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      static_rt_bindings[2].descriptorCount = 1;
      static_rt_bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      for (std::uint32_t binding = 3; binding <= 4; ++binding) {
        static_rt_bindings[binding].binding = binding;
        static_rt_bindings[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        static_rt_bindings[binding].descriptorCount = 1;
        static_rt_bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      li.bindingCount = static_cast<std::uint32_t>(static_rt_bindings.size());
      li.pBindings = static_rt_bindings.data();
      VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr,
                                           &static_rt_desc_layout_));
      std::array<VkDescriptorPoolSize, 3> static_rt_pool_sizes{};
      static_rt_pool_sizes[0] = {
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          static_cast<std::uint32_t>(frames_.size())};
      static_rt_pool_sizes[1] = {
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          static_cast<std::uint32_t>(frames_.size() * 3u)};
      static_rt_pool_sizes[2] = {
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
          static_cast<std::uint32_t>(frames_.size())};
      pi.poolSizeCount = static_cast<std::uint32_t>(static_rt_pool_sizes.size());
      pi.pPoolSizes = static_rt_pool_sizes.data();
      pi.maxSets = static_cast<std::uint32_t>(frames_.size());
      VK_CHECK(
          vkCreateDescriptorPool(device_, &pi, nullptr, &static_rt_desc_pool_));
      std::array<VkDescriptorSetLayout, 2> static_rt_layouts = {
          static_rt_desc_layout_, static_rt_desc_layout_};
      ai.descriptorPool = static_rt_desc_pool_;
      ai.descriptorSetCount =
          static_cast<std::uint32_t>(static_rt_descriptor_sets_.size());
      ai.pSetLayouts = static_rt_layouts.data();
      VK_CHECK(vkAllocateDescriptorSets(device_, &ai,
                                        static_rt_descriptor_sets_.data()));
    }

    // Skybox cubemap sampler + descriptor (one set, updated when scene changes).
    VkDescriptorSetLayoutBinding sky_b{};
    sky_b.binding = 0;
    sky_b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sky_b.descriptorCount = 1;
    sky_b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.bindingCount = 1;
    li.pBindings = &sky_b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr,
                                         &skybox_desc_layout_));
    VkDescriptorPoolSize sky_ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &sky_ps;
    pi.maxSets = 1;
    VK_CHECK(
        vkCreateDescriptorPool(device_, &pi, nullptr, &skybox_desc_pool_));
    ai.descriptorPool = skybox_desc_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &skybox_desc_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &skybox_desc_set_));
    VkSamplerCreateInfo sky_sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sky_sampler.magFilter = VK_FILTER_LINEAR;
    sky_sampler.minFilter = VK_FILTER_LINEAR;
    sky_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sky_sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sky_sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sky_sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sky_sampler.minLod = 0.0f;
    sky_sampler.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(device_, &sky_sampler, nullptr, &skybox_sampler_));
    return true;
  }

VkShaderModule VulkanBackend::makeModule(const uint32_t *words, size_t word_count) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = word_count * 4;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return m;
  }

bool VulkanBackend::createBuffers() {

    for (std::size_t fi = 0; fi < frames_.size(); ++fi) {
      auto &frame = frames_[fi];
      if (!createBuffer(256 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        frame.ui_vbo) ||
          !createBuffer(128 * 1024, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        frame.ui_ibo) ||
          !createBuffer(512 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        frame.mesh_vbo) ||
          !createBuffer(64 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        frame.bone_ssbo)) {
        return false;
      }

      VkDescriptorBufferInfo bone_info{};
      bone_info.buffer = frame.bone_ssbo.buffer;
      bone_info.offset = 0;
      bone_info.range = frame.bone_ssbo.capacity;
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = frame.static_descriptor_set;
      write.dstBinding = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.descriptorCount = 1;
      write.pBufferInfo = &bone_info;
      vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
      if (static_rt_descriptor_sets_[fi]) {
        write.dstSet = static_rt_descriptor_sets_[fi];
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
      }
    }
    return true;
  }

bool VulkanBackend::createCommandPool() {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = graphics_family_;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(device_, &pi, nullptr, &cmd_pool_));
    return true;
  }

bool VulkanBackend::createSync() {
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(frames_.size());
    std::array<VkCommandBuffer, 2> cmds{};
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, cmds.data()));
    for (size_t i = 0; i < frames_.size(); ++i) {
      frames_[i].cmd = cmds[i];
      VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      VK_CHECK(vkCreateSemaphore(device_, &si, nullptr,
                                 &frames_[i].image_available));
      VK_CHECK(vkCreateFence(device_, &fi, nullptr, &frames_[i].fence));
    }
    return true;
  }

void VulkanBackend::createTimestampQueryPools() {
    timestamp_queries_enabled_ =
        gpuTimestampSupported(timestamp_valid_bits_, timestamp_period_ns_);
    if (!timestamp_queries_enabled_) {
      return;
    }

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kGpuTimestampQueryCount;
    for (auto &frame : frames_) {
      const VkResult result =
          vkCreateQueryPool(device_, &info, nullptr, &frame.timestamp_pool);
      if (result == VK_SUCCESS) {
        continue;
      }

      SDL_Log("Vulkan timestamp query pool creation failed: %d; disabling GPU "
              "timestamps",
              static_cast<int>(result));
      for (auto &created : frames_) {
        if (created.timestamp_pool) {
          vkDestroyQueryPool(device_, created.timestamp_pool, nullptr);
          created.timestamp_pool = VK_NULL_HANDLE;
        }
      }
      timestamp_queries_enabled_ = false;
      return;
    }
  }


int VulkanBackend::drawUi(FrameSync &frame, const UiDrawData &ui,
                          bool overlay_only) {
  if (!ui.ctx || !ui.cmds || !ui.vertices || !ui.indices) {
    return 0;
  }
  const nk_size vsize = ui.vertices->allocated > 0
                            ? ui.vertices->allocated
                            : nk_buffer_total(ui.vertices);
  const nk_size esize = ui.indices->allocated > 0 ? ui.indices->allocated
                                                  : nk_buffer_total(ui.indices);
  if (vsize == 0 || esize == 0 || !frame.ui_vbo.buffer ||
      !frame.ui_ibo.buffer) {
    return 0;
  }
  const VkCommandBuffer cmd = frame.cmd;

  const int lw = (std::max)(1, ui.logical_w);
  const int lh = (std::max)(1, ui.logical_h);



  float pc[4] = {2.0f / static_cast<float>(lw), 2.0f / static_cast<float>(lh),
                 -1.0f, -1.0f};

  VkViewport vp{};
  vp.x = 0;
  vp.y = 0;
  vp.width = static_cast<float>(swap_extent_.width);
  vp.height = static_cast<float>(swap_extent_.height);
  vp.minDepth = 0;
  vp.maxDepth = 1;
  VkRect2D full{{0, 0}, swap_extent_};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &full);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_layout_, 0,
                          1, &desc_set_, 0, nullptr);
  vkCmdPushConstants(cmd, ui_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc),
                     pc);
  VkDeviceSize off = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &frame.ui_vbo.buffer, &off);
  vkCmdBindIndexBuffer(cmd, frame.ui_ibo.buffer, 0, VK_INDEX_TYPE_UINT16);

  const float sx =
      static_cast<float>(swap_extent_.width) / static_cast<float>(lw);
  const float sy =
      static_cast<float>(swap_extent_.height) / static_cast<float>(lh);

  const nk_draw_command *dcmd = nullptr;
  uint32_t index_offset = 0;
  int draw_commands = 0;
  nk_draw_foreach(dcmd, ui.ctx, ui.cmds) {
    if (!dcmd || dcmd->elem_count == 0) {
      continue;
    }
    int32_t x1 = static_cast<int32_t>(dcmd->clip_rect.x * sx);
    int32_t y1 = static_cast<int32_t>(dcmd->clip_rect.y * sy);
    int32_t x2 = static_cast<int32_t>(
        (dcmd->clip_rect.x + dcmd->clip_rect.w) * sx);
    int32_t y2 = static_cast<int32_t>(
        (dcmd->clip_rect.y + dcmd->clip_rect.h) * sy);
    if (overlay_only) {
      x1 = (std::max)(x1, static_cast<int32_t>(ui.overlay_x * sx));
      y1 = (std::max)(y1, static_cast<int32_t>(ui.overlay_y * sy));
      x2 = (std::min)(
          x2, static_cast<int32_t>((ui.overlay_x + ui.overlay_w) * sx));
      y2 = (std::min)(
          y2, static_cast<int32_t>((ui.overlay_y + ui.overlay_h) * sy));
    }
    x1 = (std::max)(x1, 0);
    y1 = (std::max)(y1, 0);
    x2 = (std::min)(x2, static_cast<int32_t>(swap_extent_.width));
    y2 = (std::min)(y2, static_cast<int32_t>(swap_extent_.height));
    if (x2 <= x1 || y2 <= y1) {
      index_offset += dcmd->elem_count;
      continue;
    }
    VkRect2D sc{};
    sc.offset.x = x1;
    sc.offset.y = y1;
    sc.extent.width = static_cast<uint32_t>(x2 - x1);
    sc.extent.height = static_cast<uint32_t>(y2 - y1);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDrawIndexed(cmd, dcmd->elem_count, 1, index_offset, 0, 0);
    index_offset += dcmd->elem_count;
    ++draw_commands;
  }
  return draw_commands;
}

}

std::unique_ptr<IGpuBackend> createVulkanBackend() {
  return std::make_unique<detail::VulkanBackend>();
}

}
