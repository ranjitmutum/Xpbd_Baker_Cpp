


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

using Clock = std::chrono::steady_clock;

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

struct NkVertex {
  float pos[2];
  float uv[2];
  uint8_t col[4];
};

enum class GpuTimestampQuery : std::uint32_t {
  FrameBegin,
  AsBegin,
  AsEnd,
  PathTraceBegin,
  PathTraceEnd,
  UiEnd,
  OpaqueEnd,
  TransparentEnd,
  LinesEnd,
  FrameEnd,
  Count,
};

constexpr std::uint32_t queryIndex(GpuTimestampQuery query) {
  return static_cast<std::uint32_t>(query);
}

constexpr std::uint32_t kGpuTimestampQueryCount =
    queryIndex(GpuTimestampQuery::Count);

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

float halfToFloat(std::uint16_t value) noexcept {
  const bool negative = (value & 0x8000u) != 0u;
  const std::uint32_t exponent = (value >> 10u) & 0x1fu;
  const std::uint32_t mantissa = value & 0x03ffu;
  double decoded = 0.0;
  if (exponent == 0u) {
    decoded = std::ldexp(static_cast<double>(mantissa), -24);
  } else if (exponent == 0x1fu) {
    decoded = mantissa == 0u
                  ? std::numeric_limits<double>::infinity()
                  : std::numeric_limits<double>::quiet_NaN();
  } else {
    decoded = std::ldexp(static_cast<double>(1024u + mantissa),
                         static_cast<int>(exponent) - 25);
  }
  return static_cast<float>(negative ? -decoded : decoded);
}

std::array<float, 3> colorTemperatureRgb(float kelvin) noexcept {
  const double temperature =
      std::clamp(static_cast<double>(kelvin), 1000.0, 40000.0) / 100.0;
  double red = 255.0;
  double green = 255.0;
  double blue = 255.0;
  if (temperature <= 66.0) {
    green = 99.4708025861 * std::log(temperature) - 161.1195681661;
    blue = temperature <= 19.0
               ? 0.0
               : 138.5177312231 * std::log(temperature - 10.0) -
                     305.0447927307;
  } else {
    red = 329.698727446 * std::pow(temperature - 60.0, -0.1332047592);
    green = 288.1221695283 *
            std::pow(temperature - 60.0, -0.0755148492);
  }
  const auto normalized = [](double channel) {
    return static_cast<float>(std::clamp(channel, 0.0, 255.0) / 255.0);
  };
  return {normalized(red), normalized(green), normalized(blue)};
}

void appendPathTraceHistoryBytes(std::uint64_t &hash, const void *data,
                                 std::size_t byte_count) {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  for (std::size_t index = 0; index < byte_count; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
}

template <typename T>
void appendPathTraceHistoryValue(std::uint64_t &hash,
                                 const T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  appendPathTraceHistoryBytes(hash, &value, sizeof(value));
}

std::uint64_t diagnosticTimestampUs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          Clock::now().time_since_epoch())
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

const char *vkResultName(VkResult result) {
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





























static void writeLog(const char *msg) { xpbd::log::info(msg); }



static const uint32_t kSpvUiVert[] = {
#include "spirv/ui.vert.spv.inc"
};
static const uint32_t kSpvUiFrag[] = {
#include "spirv/ui.frag.spv.inc"
};
static const uint32_t kSpvMeshVert[] = {
#include "spirv/mesh.vert.spv.inc"
};
static const uint32_t kSpvMeshFrag[] = {
#include "spirv/mesh.frag.spv.inc"
};
static const uint32_t kSpvStaticMeshVert[] = {
#include "spirv/static_mesh.vert.spv.inc"
};
static const uint32_t kSpvStaticMeshFrag[] = {
#include "spirv/static_mesh.frag.spv.inc"
};
static const uint32_t kSpvSkyboxVert[] = {
#include "spirv/skybox.vert.spv.inc"
};
static const uint32_t kSpvSkyboxFrag[] = {
#include "spirv/skybox.frag.spv.inc"
};
static const uint32_t kSpvAtmosphereTransmittanceComp[] = {
#include "spirv/atmosphere_transmittance.comp.spv.inc"
};
static const uint32_t kSpvAtmosphereDirectIrradianceComp[] = {
#include "spirv/atmosphere_direct_irradiance.comp.spv.inc"
};
static const uint32_t kSpvAtmosphereSingleScatteringComp[] = {
#include "spirv/atmosphere_single_scattering.comp.spv.inc"
};
static const uint32_t kSpvAtmosphereScatteringDensityComp[] = {
#include "spirv/atmosphere_scattering_density.comp.spv.inc"
};
static const uint32_t kSpvAtmosphereIndirectIrradianceComp[] = {
#include "spirv/atmosphere_indirect_irradiance.comp.spv.inc"
};
static const uint32_t kSpvAtmosphereMultipleScatteringComp[] = {
#include "spirv/atmosphere_multiple_scattering.comp.spv.inc"
};
static const uint32_t kSpvAtmosphereEnvironmentCacheComp[] = {
#include "spirv/atmosphere_environment_cache.comp.spv.inc"
};
static const uint32_t kSpvMeshRtVert[] = {
#include "spirv/mesh_rt.vert.spv.inc"
};
static const uint32_t kSpvMeshRtFrag[] = {
#include "spirv/mesh_rt.frag.spv.inc"
};
static const uint32_t kSpvStaticMeshRtVert[] = {
#include "spirv/static_mesh_rt.vert.spv.inc"
};
static const uint32_t kSpvStaticMeshRtFrag[] = {
#include "spirv/static_mesh_rt.frag.spv.inc"
};

// Unit cube (triangle list, 36 verts) for skybox sampling.
static const float kSkyboxCubePositions[] = {
    // -Z
    -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1,
    // +Z
    -1, -1, 1, -1, 1, 1, 1, 1, 1, -1, -1, 1, 1, 1, 1, 1, -1, 1,
    // -X
    -1, -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, -1, -1, 1, 1, -1, -1, 1,
    // +X
    1, -1, -1, 1, -1, 1, 1, 1, 1, 1, -1, -1, 1, 1, 1, 1, 1, -1,
    // -Y
    -1, -1, -1, -1, -1, 1, 1, -1, 1, -1, -1, -1, 1, -1, 1, 1, -1, -1,
    // +Y
    -1, 1, -1, 1, 1, -1, 1, 1, 1, -1, 1, -1, 1, 1, 1, -1, 1, 1,
};





struct SwapchainSupport {
  VkSurfaceCapabilitiesKHR caps{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> modes;
};

class VulkanBackend final : public IGpuBackend {
public:
  bool init(SDL_Window *window) override {
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
    streamline_vulkan_runtime_.completeFrameGenerationSwapchainTransition(
        streamline_vulkan_runtime_.swapchainOwnership(),
        streamline_vulkan_runtime_.swapchainOwnership() ==
                SwapchainOwnership::StreamlineFrameGenerationProxy &&
            fg_swapchain_resources_ready_,
        static_cast<std::uint64_t>(frame_index_),
        "initial native swapchain");
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

  void shutdown() override {
    if (device_) {
      // Turn interpolation off before draining or destroying any presentation
      // object. The feature itself is unloaded after the swapchain is gone.
      streamline_vulkan_runtime_.beginFrameGenerationShutdown(
          static_cast<std::uint64_t>(frame_index_));
      swapchain_recreate_target_ = SwapchainOwnership::Native;
      fg_force_native_recovery_ = true;
      const FrameSync &sync = frames_[frame_index_];
      const auto idle_start = Clock::now();
      logDiagnosticApi("vkDeviceWaitIdle.shutdown", "before", std::nullopt,
                       0.0, UINT32_MAX, sync.fence, VK_NULL_HANDLE, sync.cmd,
                       true, true);
      const VkResult idle_result =
          streamline_vulkan_runtime_.deviceWaitIdle(device_);
      logDiagnosticApi(
          "vkDeviceWaitIdle.shutdown", "after", idle_result,
          std::chrono::duration<double, std::milli>(Clock::now() - idle_start)
              .count(),
          UINT32_MAX, sync.fence, VK_NULL_HANDLE, sync.cmd, true, false);
      if (idle_result != VK_SUCCESS) {
        SDL_Log("Vulkan device idle wait during shutdown failed: %d",
                static_cast<int>(idle_result));
      } else if (!waitForPendingPresentFences("shutdown")) {
        SDL_Log("Vulkan present completion wait during shutdown failed");
      }
    }
    destroySwapchainObjects();
    if (!streamline_vulkan_runtime_.unloadFrameGenerationForShutdown()) {
      xpbd::log::warn(
          "DLSS Frame Generation plugin did not unload cleanly during "
          "shutdown");
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

  void resize(int, int) override {
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

  bool uploadFontAtlas(const void *pixels, int width, int height,
                       bool is_rgba) override {
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

  unsigned int fontTextureId() const override {

    return 1;
  }

  bool supportsStaticModel() const override { return true; }

  void beginLatencyFrame(
      std::uint32_t frame_index, PathTraceReflexMode mode,
      bool frame_generation_requested) override {
    streamline_vulkan_runtime_.beginLatencyFrame(
        frame_index, mode, frame_generation_requested);
    stats_.dlss_frame_generation_supported =
        frameGenerationPlatformSupported();
    stats_.dlss_frame_generation_requested =
        frame_generation_requested;
    stats_.reflex_supported =
        streamline_vulkan_runtime_.reflexSupported();
  }

  void endLatencySimulation() override {
    streamline_vulkan_runtime_.endLatencySimulation();
  }
  std::uint32_t latencyPingMessage() const override {
    return streamline_vulkan_runtime_.pclLatencyPingMessage();
  }
  void markLatencyPing() override {
    streamline_vulkan_runtime_.markPclLatencyPing();
  }

  void render(const FrameInput &frame) override {
    const auto t0 = Clock::now();
    stats_.present_succeeded = false;
    diagnostic_context_ = frame.diagnostics;
    diagnostic_trace_frame_ =
        perf_diagnostics_enabled_ && frame.diagnostics.active;
    if (fatal_error_ || !device_) {
      failActiveStillRender(
          frame, fatal_error_detail_.empty()
                     ? "Vulkan backend is unavailable after a fatal error"
                     : fatal_error_detail_);
      return;
    }
    // While a native file dialog is open, do not submit GPU work (avoids hang).
    if (presentation_suspended_) {
      return;
    }
    rt_descriptor_write_calls_frame_ = 0;
    rt_descriptor_cache_hits_frame_ = 0;
    rt_descriptor_entries_written_frame_ = 0;

    const PathTraceSettings normalized_frame_settings =
        normalizePathTraceSettings(frame.path_trace_settings);
    const bool requested_frame_generation =
        normalized_frame_settings.requested_frame_generation ==
        PathTraceFrameGeneration::On;
    const bool interactive_preview_resize =
        frame.interactive_viewport_resize;
    // Keep the runtime's atomic request authoritative even when a caller
    // reaches the backend without the normal latency-frame prelude.
    streamline_vulkan_runtime_.requestFrameGeneration(
        requested_frame_generation);
    const bool want_rt =
        frame.prefer_ray_tracing && !presentation_suspended_;
    active_render_path_ = resolveRenderPath(want_rt, rt_capability_);
    const bool frame_generation_supported =
        frameGenerationPlatformSupported();
    const bool frame_generation_available =
        frameGenerationSwapchainReady();
    const FrameGenerationDiagnostic fg_diagnostic =
        streamline_vulkan_runtime_.frameGenerationDiagnostic();
    // Capability and current proxy readiness are deliberately separate.  A
    // requested FG transition must be allowed to create the proxy resources;
    // requiring resources here would make activation impossible from a native
    // swapchain.  The next transaction still enforces Vulkan Immediate mode,
    // shared queues, transfer-source usage, and guide-image allocation.
    const bool desired_frame_generation =
        requested_frame_generation && frame_generation_supported &&
        !vsync_ && !fg_force_native_recovery_ &&
        !fg_diagnostic.recovery_required &&
        active_render_path_ == RenderPath::RayTracing &&
        streamline_vulkan_runtime_.frameGenerationActivationAllowed();
    stats_.dlss_frame_generation_supported =
        frame_generation_supported;
    stats_.dlss_frame_generation_requested =
        requested_frame_generation;
    stats_.reflex_supported =
        streamline_vulkan_runtime_.reflexSupported();
    stats_.dlss_frame_generation_active =
        streamline_vulkan_runtime_.frameGenerationActive();
    stats_.dlss_frames_actually_presented =
        streamline_vulkan_runtime_.framesActuallyPresented();

    const SwapchainOwnership desired_ownership =
        desired_frame_generation
            ? SwapchainOwnership::StreamlineFrameGenerationProxy
            : SwapchainOwnership::Native;
    // Streamline requires a fresh swapchain for every FG on/off transition.
    // Recovery/resize/dialog gates force one Native transaction first.
    if (!recreate_swapchain_ &&
        streamline_vulkan_runtime_.swapchainOwnership() !=
            desired_ownership) {
      swapchain_recreate_target_ = desired_ownership;
      recreate_swapchain_ = true;
    }
    if (recreate_swapchain_ || !swapchain_) {
      swapchain_recreate_target_ =
          fg_force_native_recovery_ ? SwapchainOwnership::Native
                                    : desired_ownership;
    }

    // Path selection: honor user RT preference only when NVIDIA RT is armed.
    // Unsupported / failed RT always falls back to the forward raster path.
    // Never run RT while a system dialog owns the UI thread.
    if (want_rt &&
        active_render_path_ != RenderPath::RayTracing && !rt_fallback_logged_) {
      xpbd::log::warnf(
          "Ray tracing requested but falling back to rasterization: %s",
          rt_capability_.unsupported_reason.empty()
              ? "RT not available on this device"
              : rt_capability_.unsupported_reason.c_str());
      rt_fallback_logged_ = true;
    }
    bool rt_shadows_active = false;

    FrameSync &fs = frames_[frame_index_];
    VulkanRtScene &rt_scene = rt_scenes_[frame_index_];
    VulkanPathTracer &path_tracer = path_tracers_[frame_index_];
    bool &rt_scene_built = rt_scene_built_[frame_index_];
    std::uint64_t &last_rt_scene_hash = last_rt_scene_hash_[frame_index_];
    const VkDescriptorSet mesh_rt_desc_set =
        mesh_rt_desc_sets_[frame_index_];
    const bool static_input =
        frame.static_model != nullptr && frame.static_model_frame != nullptr;
    const bool static_refresh_pending =
        static_input &&
        static_generations_.needsRefresh(frame.static_model_generation,
                                         frame.static_texture_generation);
    logDiagnosticFrame(frame, fs);

    if (recreate_swapchain_ || !swapchain_) {
      if (Clock::now() < next_swapchain_recreate_attempt_) {
        SDL_Delay(16);
        return;
      }
      if (!recreateSwapchain()) {
        recreate_swapchain_ = true;
        if (!fatal_error_) {
          next_swapchain_recreate_attempt_ =
              Clock::now() + kSwapchainRecreateRetryDelay;
          SDL_Delay(16);
        }
        return;
      }
      recreate_swapchain_ = false;
      next_swapchain_recreate_attempt_ = {};
    }

    auto wait_for_fence = [&](VkFence fence, const char *stage,
                              std::uint32_t image_index,
                              VkFence image_fence) {
      const auto wait_start = Clock::now();
      logDiagnosticApi(stage, "before", std::nullopt, 0.0, image_index,
                       fence, image_fence, fs.cmd, false, true);
      VkResult result = VK_SUCCESS;
      bool timed_out = false;
      if (!diagnostics_enabled_) {
        result =
            vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
      } else {
        do {
          result = vkWaitForFences(device_, 1, &fence, VK_TRUE,
                                   kDiagnosticWaitSliceNs);
          if (result == VK_TIMEOUT) {
            timed_out = true;
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          wait_start)
                    .count();
            logDiagnosticApi(stage, "timeout", result, elapsed_ms,
                             image_index, fence, image_fence, fs.cmd, true,
                             true);
          }
        } while (result == VK_TIMEOUT);
      }
      const double elapsed_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - wait_start)
              .count();
      logDiagnosticApi(stage, "after", result, elapsed_ms, image_index,
                       fence, image_fence, fs.cmd, timed_out, false);
      return result;
    };

    const VkResult wait_result =
        wait_for_fence(fs.fence, "vkWaitForFences.frame", UINT32_MAX,
                       VK_NULL_HANDLE);
    if (wait_result != VK_SUCCESS) {
      SDL_Log("Vulkan fence wait failed: %d", static_cast<int>(wait_result));
      enterFatalVulkanError(frame, "vkWaitForFences(frame)", wait_result);
      return;
    }

    readCompletedTimestamps(fs);
    logDiagnosticPerf(fs);
    fs.perf_pending = false;
    stats_.backend_cpu_ms = 0.0f;
    stats_.cpu_scene_assembly_ms = 0.0f;
    stats_.cpu_scene_hash_ms = 0.0f;
    stats_.cpu_emitter_distribution_ms = 0.0f;
    stats_.cpu_descriptor_update_ms = 0.0f;
    stats_.gpu_as_build_ms = 0.0f;
    stats_.gpu_path_trace_ms = 0.0f;
    stats_.gpu_rr_ms = 0.0f;
    stats_.gpu_sr_ms = 0.0f;
    stats_.gpu_fg_present_ms = 0.0f;
    stats_.rt_aov_write_mask = 0;
    ResolvedWorldEnvironment resolved_world_environment =
        frame.world_environment != nullptr
            ? resolveWorldEnvironment(*frame.world_environment)
            : ResolvedWorldEnvironment{};
    const bool procedural_atmosphere_ready =
        ensureProceduralAtmosphereResources(resolved_world_environment);
    const bool hdr_environment_ready =
        rt_capability_.device_extensions_enabled &&
        ensureWorldEnvironmentResources(resolved_world_environment);
    if (fatal_error_) {
      failActiveStillRender(
          frame, fatal_error_detail_.empty()
                     ? "Vulkan device was lost while updating environment resources"
                     : fatal_error_detail_);
      return;
    }
    if (resolved_world_environment.sky_rendering ==
            SkyRendering::ProceduralDayNight &&
        !procedural_atmosphere_ready) {
      resolved_world_environment.sky_rendering = SkyRendering::Off;
      resolved_world_environment.background_visible = false;
      resolved_world_environment.environment_lighting = false;
      resolved_world_environment.environment_strength = 0.0f;
      resolved_world_environment.celestial = nullptr;
      resolved_world_environment.atmosphere = nullptr;
      resolved_world_environment.clouds = nullptr;
    }
    if (resolved_world_environment.sky_rendering ==
            SkyRendering::UserHdri &&
        !hdr_environment_ready) {
      resolved_world_environment.sky_rendering = SkyRendering::Off;
      resolved_world_environment.background_visible = false;
      resolved_world_environment.environment_lighting = false;
      resolved_world_environment.environment_strength = 0.0f;
      resolved_world_environment.hdr = nullptr;
    }
    const bool procedural_environment_ready =
        resolved_world_environment.sky_rendering ==
            SkyRendering::ProceduralDayNight &&
        procedural_atmosphere_ready && atmosphere_environment_ready_;
    const bool importance_environment_ready =
        hdr_environment_ready || procedural_environment_ready;

    const auto rt_scene_cpu_begin = Clock::now();
    // RT acceleration: CPU-skin when poses change, then record BLAS build/refit
    // into the frame command buffer (no per-frame QueueWaitIdle — that froze
    // the app when playing animations with RT enabled).
    bool path_trace_active = false;
    bool rt_as_needs_record = false;
    std::uint64_t rt_upload_bytes_frame = 0;
    float rt_emitter_distribution_frame_ms = 0.0f;
    std::uint64_t rt_streamline_topology_hash =
        14695981039346656037ull;
    std::uint64_t rt_streamline_material_hash =
        14695981039346656037ull;
    if (active_render_path_ == RenderPath::RayTracing &&
        rt_capability_.device_extensions_enabled && !presentation_suspended_) {
      std::vector<float> packed_bones;
      std::vector<float> packed_tints;
      if (static_input && frame.static_model_frame != nullptr &&
          !frame.static_model_frame->bones.empty()) {
        packed_bones.resize(frame.static_model_frame->bones.size() * 16u);
        packed_tints.resize(frame.static_model_frame->bones.size() * 4u);
        for (std::size_t i = 0; i < frame.static_model_frame->bones.size();
             ++i) {
          std::memcpy(packed_bones.data() + i * 16u,
                      frame.static_model_frame->bones[i].transform.data(),
                      16u * sizeof(float));
          std::memcpy(packed_tints.data() + i * 4u,
                      frame.static_model_frame->bones[i].tint.data(),
                      4u * sizeof(float));
        }
      }

      std::array<RtColoredGeometryView, 4> geometry_views{};
      std::size_t geometry_view_count = 0;
      auto append_geometry = [&](const std::vector<MeshVertex> &vertices,
                                 bool alpha_blended,
                                 RtGeometryKind kind,
                                 RtBlasPolicy blas_policy,
                                 std::uint64_t content_generation,
                                 std::uint64_t topology_generation,
                                 std::uint64_t material_generation,
                                 std::uint64_t emission_generation) {
        if (vertices.empty() ||
            geometry_view_count >= geometry_views.size()) {
          return;
        }
        geometry_views[geometry_view_count] = {
            vertices.data(), vertices.size(), alpha_blended, kind,
            blas_policy, content_generation, topology_generation,
            material_generation, emission_generation};
        ++geometry_view_count;
      };
      if (frame.scene != nullptr) {
        append_geometry(frame.scene->solid, false,
                        RtGeometryKind::SkinnedModel,
                        RtBlasPolicy::DynamicRefit,
                        frame.rt_scene_generations.positions,
                        frame.rt_scene_generations.topology,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
        append_geometry(frame.scene->transparent, true,
                        RtGeometryKind::SkinnedModel,
                        RtBlasPolicy::DynamicRefit,
                        frame.rt_scene_generations.positions,
                        frame.rt_scene_generations.topology,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
      }
      if (frame.raster_scene != nullptr) {
        const bool ocean =
            frame.raster_scene->id == PreviewSceneId::Ocean;
        const RtGeometryKind environment_kind =
            ocean ? RtGeometryKind::Ocean : RtGeometryKind::StaticScene;
        const std::uint64_t raster_static_generation =
            ocean ? frame.raster_scene->topology_generation
                  : frame.raster_scene->geometry_generation;
        append_geometry(frame.raster_scene->environment.solid, false,
                        environment_kind,
                        RtBlasPolicy::StaticBuildCompact,
                        raster_static_generation,
                        frame.raster_scene->topology_generation,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
        append_geometry(frame.raster_scene->environment.transparent, true,
                        environment_kind,
                         ocean && frame.raster_scene->surface_dynamic_baked
                             ? RtBlasPolicy::DynamicRefit
                             : RtBlasPolicy::StaticBuildCompact,
                         frame.raster_scene->geometry_generation,
                         frame.raster_scene->topology_generation,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
      }

      const auto scene_hash_begin = Clock::now();
      RtSceneGenerations effective_generations = frame.rt_scene_generations;
      if (!frame.rt_scene_generations_valid) {
        // Compatibility callers may not know the generation ABI.  A
        // conservative serial preserves correctness without scanning vertex
        // arrays; the normal application path always supplies generations.
        effective_generations = {};
        effective_generations.topology = ++rt_fallback_generation_serial_;
        effective_generations.positions = rt_fallback_generation_serial_;
        effective_generations.transforms = rt_fallback_generation_serial_;
        effective_generations.materials = rt_fallback_generation_serial_;
        effective_generations.emission = rt_fallback_generation_serial_;
        effective_generations.visibility = rt_fallback_generation_serial_;
      }
      for (std::size_t i = 0; i < geometry_view_count; ++i) {
        RtColoredGeometryView &view = geometry_views[i];
        if (view.content_generation == 0u) {
          view.content_generation = effective_generations.positions;
        }
        if (view.topology_generation == 0u) {
          view.topology_generation = effective_generations.topology;
        }
        if (view.material_generation == 0u) {
          view.material_generation = effective_generations.materials;
        }
        if (view.emission_generation == 0u) {
          view.emission_generation = effective_generations.emission;
        }
      }
      std::uint64_t scene_hash = rtSceneGenerationKey(effective_generations);
      std::uint64_t topology_hash =
          mixRtGeneration(0xcbf29ce484222325ull,
                          effective_generations.topology);
      topology_hash = mixRtGeneration(topology_hash,
                                      effective_generations.visibility);
      for (std::size_t i = 0; i < geometry_view_count; ++i) {
        const RtColoredGeometryView &view = geometry_views[i];
        const std::uint64_t range_tag =
            static_cast<std::uint64_t>(view.vertex_count) ^
            (view.alpha_blended ? (std::uint64_t{1} << 63u) : 0u) ^
            (static_cast<std::uint64_t>(view.kind) << 48u) ^
            (static_cast<std::uint64_t>(view.blas_policy) << 40u);
        scene_hash = mixRtGeneration(scene_hash, range_tag);
        scene_hash = mixRtGeneration(scene_hash, view.content_generation);
        scene_hash = mixRtGeneration(scene_hash, view.material_generation);
        scene_hash = mixRtGeneration(scene_hash, view.emission_generation);
        topology_hash = mixRtGeneration(topology_hash, range_tag);
        topology_hash = mixRtGeneration(topology_hash,
                                        view.topology_generation);
      }
      scene_hash = mixRtGeneration(scene_hash, geometry_view_count);
      topology_hash = mixRtGeneration(topology_hash, geometry_view_count);
      rt_streamline_topology_hash = topology_hash;
      rt_streamline_material_hash = mixRtGeneration(
          rt_streamline_material_hash, effective_generations.materials);
      rt_streamline_material_hash = mixRtGeneration(
          rt_streamline_material_hash, effective_generations.emission);
      stats_.cpu_scene_hash_ms =
          std::chrono::duration<float, std::milli>(Clock::now() -
                                                   scene_hash_begin)
              .count();

      const bool have_rt_geometry = static_input || geometry_view_count != 0u;
      bool as_ok = !static_refresh_pending && rt_scene_built &&
                   rt_scene.ready() && scene_hash == last_rt_scene_hash;
      if (as_ok) {
        rt_scene.markMotionStable();
      }
      // Empty-scene transition frames are valid: there is no BLAS/TLAS input
      // to build yet, so keep the raster path quiet until geometry arrives.
      if (!as_ok && !static_refresh_pending && have_rt_geometry) {
        // CPU skin + append all scene triangles, then mark BLAS build/refit.
        const bool explicit_motion_history_valid =
            rt_motion_history_valid_ &&
            topology_hash == rt_motion_topology_hash_;
        const RtSceneStats before_scene_update = rt_scene.stats();
        const bool scene_updated = rt_scene.updateGeometry(
            packed_bones.empty() ? nullptr : packed_bones.data(),
            packed_bones.size() / 16u,
            packed_tints.empty() ? nullptr : packed_tints.data(),
            packed_tints.size() / 4u,
            std::span<const RtColoredGeometryView>(
                geometry_views.data(), geometry_view_count),
            static_input,
            explicit_motion_history_valid
                ? std::span<const float>(rt_motion_previous_positions_)
                : std::span<const float>{},
            explicit_motion_history_valid &&
                    !rt_motion_previous_bones_.empty()
                ? rt_motion_previous_bones_.data()
                : nullptr,
            explicit_motion_history_valid
                ? rt_motion_previous_bones_.size() / 16u
                : 0u,
            explicit_motion_history_valid, effective_generations,
            frame.rt_scene_generations_valid);
        const RtSceneStats after_scene_update = rt_scene.stats();
        if (after_scene_update.upload_bytes >=
            before_scene_update.upload_bytes) {
          rt_upload_bytes_frame += after_scene_update.upload_bytes -
                                   before_scene_update.upload_bytes;
        }
        rt_emitter_distribution_frame_ms +=
            after_scene_update.emitter_distribution_ms;
        if (scene_updated) {
          rt_scene_built = true;
          last_rt_scene_hash = scene_hash;
          as_ok = true;
          rt_as_needs_record = true;
          const std::span<const float> current_positions =
              rt_scene.packedPositionSnapshot();
          rt_motion_previous_positions_.assign(
              current_positions.begin(), current_positions.end());
          rt_motion_previous_bones_ = packed_bones;
          rt_motion_topology_hash_ = topology_hash;
          rt_motion_history_valid_ = true;
        } else if (!rt_fallback_logged_) {
          xpbd::log::warnf(
              "Vulkan RT: acceleration rebuild failed; using raster path (%s)",
              rt_scene.lastUpdateFailureReason());
          rt_fallback_logged_ = true;
        }
      }
      if (!have_rt_geometry) {
        rt_motion_previous_positions_.clear();
        rt_motion_previous_bones_.clear();
        rt_motion_topology_hash_ = 0u;
        rt_motion_history_valid_ = false;
      }
      if (have_rt_geometry && as_ok) {
        // Allow a later, genuine rebuild failure to be reported once without
        // reintroducing per-frame warning spam.
        rt_fallback_logged_ = false;
      }
      if (as_ok) {
        rt_shadows_active = rt_scene.ready() && rt_pipelines_ready_;
        if (rt_shadows_active) {
          const VkAccelerationStructureKHR active_tlas = rt_scene.tlas();
          const auto descriptor_update_begin = Clock::now();
          if (mesh_rt_desc_set) {
            if (last_mesh_rt_descriptor_sets_[frame_index_] !=
                    mesh_rt_desc_set ||
                last_mesh_rt_tlas_[frame_index_] != active_tlas) {
              rt_scene.writeTlasDescriptor(mesh_rt_desc_set, 0);
              last_mesh_rt_descriptor_sets_[frame_index_] = mesh_rt_desc_set;
              last_mesh_rt_tlas_[frame_index_] = active_tlas;
              ++rt_descriptor_write_calls_frame_;
              ++rt_descriptor_entries_written_frame_;
            } else {
              ++rt_descriptor_cache_hits_frame_;
            }
          } else {
            // A descriptor pool/layout refresh can temporarily remove the
            // slot.  Do not let a later reuse of the same handle look like a
            // valid cache hit after the set has been reset.
            last_mesh_rt_descriptor_sets_[frame_index_] = VK_NULL_HANDLE;
            last_mesh_rt_tlas_[frame_index_] = VK_NULL_HANDLE;
          }
          const VkDescriptorSet static_rt_set =
              static_rt_descriptor_sets_[frame_index_];
          if (static_rt_set) {
            if (last_static_rt_descriptor_sets_[frame_index_] !=
                    static_rt_set ||
                last_static_rt_tlas_[frame_index_] != active_tlas) {
              rt_scene.writeTlasDescriptor(static_rt_set, 2);
              last_static_rt_descriptor_sets_[frame_index_] = static_rt_set;
              last_static_rt_tlas_[frame_index_] = active_tlas;
              ++rt_descriptor_write_calls_frame_;
              ++rt_descriptor_entries_written_frame_;
            } else {
              ++rt_descriptor_cache_hits_frame_;
            }
          } else {
            last_static_rt_descriptor_sets_[frame_index_] = VK_NULL_HANDLE;
            last_static_rt_tlas_[frame_index_] = VK_NULL_HANDLE;
          }
          stats_.cpu_descriptor_update_ms =
              std::chrono::duration<float, std::milli>(Clock::now() -
                                                       descriptor_update_begin)
                  .count();
        }
        path_trace_active = rt_scene.ready() && path_tracer.ready();
        if (path_trace_active && !unified_rt_logged_) {
          const RtSceneStats rt_stats = rt_scene.stats();
          xpbd::log::infof(
              "Vulkan RT: unified alpha-aware primary path active "
              "(vertices=%u triangles=%u BLAS=%u TLAS=%u instances=%u)",
              rt_scene.pathTraceVertexCount(),
              rt_scene.pathTraceIndexCount() / 3u, rt_stats.blas_count,
              rt_stats.tlas_count, rt_stats.instance_count);
          unified_rt_logged_ = true;
        }
      }
    }
    if (active_render_path_ == RenderPath::RayTracing &&
        rt_capability_.device_extensions_enabled && !presentation_suspended_) {
      stats_.cpu_scene_assembly_ms =
          std::chrono::duration<float, std::milli>(Clock::now() -
                                                   rt_scene_cpu_begin)
              .count();
    }

    stats_.upload_ms = 0.0f;
    stats_.upload_bytes = 0;
    stats_.ui_upload_bytes = 0;
    stats_.mesh_upload_bytes = 0;
    stats_.static_bone_upload_bytes = 0;
    stats_.static_resource_upload_bytes = 0;
    stats_.mesh_solid_offset_bytes = 0;
    stats_.mesh_transparent_offset_bytes = 0;
    stats_.mesh_line_offset_bytes = 0;
    stats_.buffer_reallocations = 0;
    stats_.draw_calls = 0;
    stats_.ui_commands = 0;
    stats_.active_render_path = static_cast<int>(active_render_path_);
    stats_.ray_tracing_supported = supportsRayTracing();
    stats_.ray_tracing_requested = frame.prefer_ray_tracing;
    stats_.static_resource_rebuilds = static_resource_rebuilds_;
    stats_.static_model_vertex_bytes = static_vertex_bytes_;
    stats_.static_model_index_bytes = static_index_bytes_;
    stats_.static_opaque_index_count = static_draw_plan_.opaque.index_count;
    stats_.static_cutout_index_count = static_draw_plan_.cutout.index_count;
    stats_.static_blend_index_count = static_draw_plan_.blend.index_count;

    const int framebuffer_width = static_cast<int>(std::min<std::uint32_t>(
        swap_extent_.width,
        static_cast<std::uint32_t>((std::numeric_limits<int>::max)())));
    const int framebuffer_height = static_cast<int>(std::min<std::uint32_t>(
        swap_extent_.height,
        static_cast<std::uint32_t>((std::numeric_limits<int>::max)())));
    const auto clamped_edge = [](int origin, int size, int limit) {
      const std::int64_t edge = static_cast<std::int64_t>(origin) +
                                static_cast<std::int64_t>((std::max)(size, 0));
      return static_cast<int>((std::clamp)(
          edge, std::int64_t{0}, static_cast<std::int64_t>(limit)));
    };
    ViewportRect safe_viewport{};
    safe_viewport.x =
        (std::clamp)(frame.viewport.x, 0, framebuffer_width);
    safe_viewport.y =
        (std::clamp)(frame.viewport.y, 0, framebuffer_height);
    safe_viewport.w =
        (std::max)(0, clamped_edge(frame.viewport.x, frame.viewport.w,
                                   framebuffer_width) -
                          safe_viewport.x);
    safe_viewport.h =
        (std::max)(0, clamped_edge(frame.viewport.y, frame.viewport.h,
                                   framebuffer_height) -
                          safe_viewport.y);
    const bool valid_viewport =
        safe_viewport.w > 1 && safe_viewport.h > 1 && frame.view_matrix &&
        frame.proj_matrix;
    const bool draw_mesh =
        valid_viewport && frame.scene &&
        (!frame.scene->solid.empty() || !frame.scene->transparent.empty() ||
         !frame.scene->lines.empty());
    const ViewportRasterScene *raster = frame.raster_scene;
    const bool draw_raster_env =
        valid_viewport && raster != nullptr && raster->show_environment &&
        !raster->environment.solid.empty();
    const bool draw_raster_env_trans =
        valid_viewport && raster != nullptr && raster->show_environment &&
        !raster->environment.transparent.empty();
    const bool draw_raster_grid =
        valid_viewport && raster != nullptr && !raster->grid.lines.empty();
    const bool draw_skybox =
        valid_viewport && raster != nullptr && skybox_pipeline_ &&
        skybox_layout_ && skybox_desc_set_ && raster->skybox.valid();
    const DynamicMeshUploadLayout mesh_upload =
        draw_mesh ? makeDynamicMeshUploadLayout(frame.scene->solid.size(),
                                                frame.scene->transparent.size(),
                                                frame.scene->lines.size())
                  : DynamicMeshUploadLayout{};
    // Append preview-scene environment (solid + transparent water) + grid.
    const std::size_t raster_env_offset = mesh_upload.total_bytes;
    const std::size_t raster_env_bytes =
        draw_raster_env
            ? raster->environment.solid.size() * sizeof(MeshVertex)
            : 0;
    const std::size_t raster_env_trans_offset =
        raster_env_offset + raster_env_bytes;
    const std::size_t raster_env_trans_bytes =
        draw_raster_env_trans
            ? raster->environment.transparent.size() * sizeof(MeshVertex)
            : 0;
    const std::size_t raster_grid_offset =
        raster_env_trans_offset + raster_env_trans_bytes;
    const std::size_t raster_grid_bytes =
        draw_raster_grid ? raster->grid.lines.size() * sizeof(MeshVertex) : 0;
    const std::size_t mesh_arena_bytes =
        raster_grid_offset + raster_grid_bytes;
    const bool upload_dynamic_mesh = draw_mesh || draw_raster_env ||
                                    draw_raster_env_trans || draw_raster_grid;

    VkDeviceSize ui_vertex_bytes = 0;
    VkDeviceSize ui_index_bytes = 0;
    bool draw_ui = frame.ui && font_ready_ && frame.ui->ctx && frame.ui->cmds &&
                   frame.ui->vertices && frame.ui->indices;
    if (draw_ui) {
      ui_vertex_bytes = frame.ui->vertices->allocated > 0
                            ? frame.ui->vertices->allocated
                            : nk_buffer_total(frame.ui->vertices);
      ui_index_bytes = frame.ui->indices->allocated > 0
                           ? frame.ui->indices->allocated
                           : nk_buffer_total(frame.ui->indices);
      draw_ui = ui_vertex_bytes > 0 && ui_index_bytes > 0;
    }




    const auto upload_start = Clock::now();
    auto ensure_owned_buffer = [&](Buffer &buffer, VkDeviceSize bytes,
                                   VkBufferUsageFlags usage) {
      bool reallocated = false;
      if (!ensureBuffer(buffer, bytes, usage, &reallocated)) {
        return false;
      }
      if (reallocated) {
        ++stats_.buffer_reallocations;
        ++total_buffer_reallocations_;
      }
      return true;
    };

    if (static_refresh_pending) {
      // rebuildStaticModelResources submits its upload after all previously
      // queued graphics work and waits only for that submission's fence. Once
      // it signals, the old static resources are no longer referenced and can
      // be replaced without a device-wide idle.
      std::uint64_t resource_upload_bytes = 0;
      if (!rebuildStaticModelResources(
              *frame.static_model, frame.static_model_texture,
              frame.static_model_material,
              frame.static_model_generation, frame.static_texture_generation,
              resource_upload_bytes)) {
        writeLog("Vulkan static model resource rebuild failed");
        return;
      }
      stats_.static_resource_upload_bytes = resource_upload_bytes;
      stats_.static_resource_rebuilds = static_resource_rebuilds_;
      stats_.static_model_vertex_bytes = static_vertex_bytes_;
      stats_.static_model_index_bytes = static_index_bytes_;
      stats_.static_opaque_index_count = static_draw_plan_.opaque.index_count;
      stats_.static_cutout_index_count = static_draw_plan_.cutout.index_count;
      stats_.static_blend_index_count = static_draw_plan_.blend.index_count;
    }

    bool draw_static =
        valid_viewport && static_input && static_model_ready_ &&
        !static_draw_plan_.indices.empty() &&
        staticModelFrameMatchesMesh(*frame.static_model,
                                    *frame.static_model_frame) &&
        frame.static_model_frame->bones.size() == static_bone_count_;
    if (static_input && !static_draw_plan_.indices.empty() && !draw_static &&
        !static_mismatch_logged_) {
      SDL_Log("Vulkan static model/frame mismatch: mesh bones=%zu, frame "
              "bones=%zu; skipping static draw",
              static_bone_count_, frame.static_model_frame->bones.size());
      static_mismatch_logged_ = true;
    }

    VkDeviceSize requested_bone_bytes = 0;
    if (draw_static) {
      const VkDeviceSize bone_bytes =
          static_cast<VkDeviceSize>(frame.static_model_frame->bones.size() *
                                    sizeof(StaticModelBoneState));
      requested_bone_bytes = bone_bytes;
      if (!ensure_owned_buffer(fs.bone_ssbo, bone_bytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
          !uploadBuffer(fs.bone_ssbo, 0, bone_bytes,
                        frame.static_model_frame->bones.data())) {
        writeLog("Vulkan static bone upload failed");
        return;
      }
      updateStaticBoneDescriptor(fs);
      stats_.static_bone_upload_bytes = bone_bytes;
      stats_.mesh_upload_bytes += bone_bytes;
    }
    if (draw_skybox && !uploadSkyboxCubemap(raster->skybox)) {
      writeLog("Vulkan skybox cubemap upload failed");
    }
    const bool skybox_draw = draw_skybox && skybox_ready_;

    const bool draw_viewport =
        valid_viewport &&
        (path_trace_active || draw_mesh || static_input || draw_raster_env ||
         draw_raster_env_trans || draw_raster_grid || skybox_draw);
    if (!path_trace_active || !draw_viewport) {
      // A folded, minimized, or hidden preview has no valid temporal
      // continuity. Force the next DLSS frame to start cleanly.
      streamline_vulkan_runtime_.invalidateDlssHistory();
      streamline_temporal_history_valid_ = false;
    }

    if (draw_ui) {
      if (!ensure_owned_buffer(fs.ui_vbo, ui_vertex_bytes,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
          !ensure_owned_buffer(fs.ui_ibo, ui_index_bytes,
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
          !uploadBuffer(fs.ui_vbo, 0, ui_vertex_bytes,
                        nk_buffer_memory_const(frame.ui->vertices)) ||
          !uploadBuffer(fs.ui_ibo, 0, ui_index_bytes,
                        nk_buffer_memory_const(frame.ui->indices))) {
        writeLog("Vulkan UI dynamic upload failed");
        return;
      }
      stats_.ui_upload_bytes =
          static_cast<std::uint64_t>(ui_vertex_bytes + ui_index_bytes);
    }

    if (upload_dynamic_mesh) {
      if (!ensure_owned_buffer(fs.mesh_vbo,
                               static_cast<VkDeviceSize>(mesh_arena_bytes),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        writeLog("Vulkan mesh arena allocate failed");
        return;
      }
      if (draw_mesh) {
        if (!uploadBuffer(
                fs.mesh_vbo,
                static_cast<VkDeviceSize>(mesh_upload.solid.offset_bytes),
                static_cast<VkDeviceSize>(mesh_upload.solid.size_bytes),
                frame.scene->solid.data()) ||
            !uploadBuffer(
                fs.mesh_vbo,
                static_cast<VkDeviceSize>(mesh_upload.transparent.offset_bytes),
                static_cast<VkDeviceSize>(mesh_upload.transparent.size_bytes),
                frame.scene->transparent.data()) ||
            !uploadBuffer(
                fs.mesh_vbo,
                static_cast<VkDeviceSize>(mesh_upload.lines.offset_bytes),
                static_cast<VkDeviceSize>(mesh_upload.lines.size_bytes),
                frame.scene->lines.data())) {
          writeLog("Vulkan mesh dynamic upload failed");
          return;
        }
      }
      if (draw_raster_env &&
          !uploadBuffer(fs.mesh_vbo,
                        static_cast<VkDeviceSize>(raster_env_offset),
                        static_cast<VkDeviceSize>(raster_env_bytes),
                        raster->environment.solid.data())) {
        writeLog("Vulkan raster environment upload failed");
        return;
      }
      if (draw_raster_env_trans &&
          !uploadBuffer(fs.mesh_vbo,
                        static_cast<VkDeviceSize>(raster_env_trans_offset),
                        static_cast<VkDeviceSize>(raster_env_trans_bytes),
                        raster->environment.transparent.data())) {
        writeLog("Vulkan raster water upload failed");
        return;
      }
      if (draw_raster_grid &&
          !uploadBuffer(fs.mesh_vbo,
                        static_cast<VkDeviceSize>(raster_grid_offset),
                        static_cast<VkDeviceSize>(raster_grid_bytes),
                        raster->grid.lines.data())) {
        writeLog("Vulkan raster grid upload failed");
        return;
      }
      stats_.mesh_upload_bytes += mesh_arena_bytes;
      stats_.mesh_solid_offset_bytes = mesh_upload.solid.offset_bytes;
      stats_.mesh_transparent_offset_bytes =
          mesh_upload.transparent.offset_bytes;
      stats_.mesh_line_offset_bytes = mesh_upload.lines.offset_bytes;
    }

    stats_.mesh_arena_capacity_bytes = fs.mesh_vbo.capacity;
    stats_.ui_vertex_capacity_bytes = fs.ui_vbo.capacity;
    stats_.ui_index_capacity_bytes = fs.ui_ibo.capacity;
    stats_.upload_bytes = stats_.ui_upload_bytes + stats_.mesh_upload_bytes +
                          stats_.static_resource_upload_bytes;
    stats_.total_buffer_reallocations = total_buffer_reallocations_;
    stats_.upload_ms =
        std::chrono::duration<float, std::milli>(Clock::now() - upload_start)
            .count();
    logDiagnosticResources(
        frame, fs, requested_bone_bytes,
        static_cast<VkDeviceSize>(mesh_upload.total_bytes), ui_vertex_bytes,
        ui_index_bytes,
        stats_.buffer_reallocations > 0 ||
            stats_.static_resource_upload_bytes > 0);

    uint32_t image_index = 0;
    const auto acquire_start = Clock::now();
    logDiagnosticApi("vkAcquireNextImageKHR", "before", std::nullopt, 0.0,
                     UINT32_MAX, fs.fence, VK_NULL_HANDLE, fs.cmd, false, true);
    VkResult acq = VK_SUCCESS;
    bool acquire_timed_out = false;
    if (!diagnostics_enabled_) {
      acq = streamline_vulkan_runtime_.acquireNextImage(
          device_, swapchain_, UINT64_MAX, fs.image_available,
          VK_NULL_HANDLE, &image_index);
    } else {
      do {
        acq = streamline_vulkan_runtime_.acquireNextImage(
            device_, swapchain_, kDiagnosticWaitSliceNs,
            fs.image_available, VK_NULL_HANDLE, &image_index);
        if (acq == VK_TIMEOUT || acq == VK_NOT_READY) {
          acquire_timed_out = true;
          logDiagnosticApi(
              "vkAcquireNextImageKHR", "timeout", acq,
              std::chrono::duration<double, std::milli>(Clock::now() -
                                                        acquire_start)
                  .count(),
              UINT32_MAX, fs.fence, VK_NULL_HANDLE, fs.cmd, true, true);
        }
      } while (acq == VK_TIMEOUT || acq == VK_NOT_READY);
    }
    logDiagnosticApi(
        "vkAcquireNextImageKHR", "after", acq,
        std::chrono::duration<double, std::milli>(Clock::now() - acquire_start)
            .count(),
        acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR ? image_index
                                                       : UINT32_MAX,
        fs.fence, VK_NULL_HANDLE, fs.cmd, acquire_timed_out, false);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
      recreate_swapchain_ = true;
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        fg_force_native_recovery_ = true;
        swapchain_recreate_target_ = SwapchainOwnership::Native;
      }
      return;
    }
    if (acq == VK_SUBOPTIMAL_KHR) {
      recreate_swapchain_ = true;
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        fg_force_native_recovery_ = true;
        swapchain_recreate_target_ = SwapchainOwnership::Native;
      }
    } else if (acq != VK_SUCCESS) {
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        const FrameGenerationTransitionResult transition =
            streamline_vulkan_runtime_.classifyFrameGenerationVkResult(
                acq, "Acquire");
        if (transition == FrameGenerationTransitionResult::FatalDeviceLost) {
          enterFatalVulkanError(frame, "vkAcquireNextImageKHR", acq);
        } else {
          fg_force_native_recovery_ = true;
          swapchain_recreate_target_ = SwapchainOwnership::Native;
          recreate_swapchain_ = true;
        }
      } else {
        SDL_Log("Vulkan acquire failed: %d", static_cast<int>(acq));
        enterFatalVulkanError(frame, "vkAcquireNextImageKHR", acq);
      }
      if (fatal_error_ || recreate_swapchain_) {
        return;
      }
    }
    if (image_index >= framebuffers_.size() ||
        image_index >= swap_image_resources_.size()) {
      writeLog("Vulkan acquire returned an invalid swapchain image index");
      fatal_error_ = true;
      return;
    }
    SwapchainImageResource &image_resource =
        swap_image_resources_[image_index];
    if (!image_resource.render_finished) {
      writeLog("Vulkan swapchain image has no present semaphore");
      fatal_error_ = true;
      return;
    }
    if (presentFenceLifecycleEnabled() &&
        !image_resource.present_fence) {
      writeLog("Vulkan swapchain image has no present completion fence");
      fatal_error_ = true;
      return;
    }
    if (image_resource.last_in_flight &&
        image_resource.last_in_flight != fs.fence) {
      const VkResult image_wait = wait_for_fence(
          image_resource.last_in_flight, "vkWaitForFences.image", image_index,
          image_resource.last_in_flight);
      if (image_wait != VK_SUCCESS) {
        SDL_Log("Vulkan swapchain image fence wait failed: %d",
                static_cast<int>(image_wait));
        enterFatalVulkanError(frame, "vkWaitForFences(image)", image_wait);
        return;
      }
    }

    auto call_with_diagnostics = [&](const char *stage, auto &&call) {
      const auto call_start = Clock::now();
      logDiagnosticApi(stage, "before", std::nullopt, 0.0, image_index,
                       fs.fence, image_resource.last_in_flight, fs.cmd, false,
                       true);
      const VkResult result = call();
      logDiagnosticApi(
          stage, "after", result,
          std::chrono::duration<double, std::milli>(Clock::now() - call_start)
              .count(),
          image_index, fs.fence, image_resource.last_in_flight, fs.cmd, false,
          false);
      return result;
    };

    if (presentFenceLifecycleEnabled() &&
        image_resource.present_pending) {
      const VkResult present_wait =
          wait_for_fence(image_resource.present_fence,
                         "vkWaitForFences.present_reuse", image_index,
                         image_resource.last_in_flight);
      if (present_wait != VK_SUCCESS) {
        SDL_Log("Vulkan present fence wait before reuse failed: %d",
                static_cast<int>(present_wait));
        enterFatalVulkanError(frame, "vkWaitForFences(present)",
                              present_wait);
        return;
      }
      image_resource.present_pending = false;

      const auto reset_present_start = Clock::now();
      logDiagnosticApi("vkResetFences.present", "before", std::nullopt, 0.0,
                       image_index, image_resource.present_fence,
                       image_resource.last_in_flight, fs.cmd, false, true);
      const VkResult reset_present = vkResetFences(
          device_, 1, &image_resource.present_fence);
      logDiagnosticApi(
          "vkResetFences.present", "after", reset_present,
          std::chrono::duration<double, std::milli>(Clock::now() -
                                                    reset_present_start)
              .count(),
          image_index, image_resource.present_fence,
          image_resource.last_in_flight, fs.cmd, false, false);
      if (reset_present != VK_SUCCESS) {
        SDL_Log("Vulkan present fence reset failed: %d",
                static_cast<int>(reset_present));
        enterFatalVulkanError(frame, "vkResetFences(present)",
                              reset_present);
        return;
      }
    }

    const VkResult reset_fence = call_with_diagnostics(
        "vkResetFences", [&] { return vkResetFences(device_, 1, &fs.fence); });
    if (reset_fence != VK_SUCCESS) {
      SDL_Log("Vulkan fence reset failed: %d",
              static_cast<int>(reset_fence));
      enterFatalVulkanError(frame, "vkResetFences(frame)", reset_fence);
      return;
    }
    const VkResult reset_command = call_with_diagnostics(
        "vkResetCommandBuffer",
        [&] { return vkResetCommandBuffer(fs.cmd, 0); });
    if (reset_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer reset failed: %d",
              static_cast<int>(reset_command));
      enterFatalVulkanError(frame, "vkResetCommandBuffer", reset_command);
      return;
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    const VkResult begin_command = call_with_diagnostics(
        "vkBeginCommandBuffer",
        [&] { return vkBeginCommandBuffer(fs.cmd, &bi); });
    if (begin_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer begin failed: %d",
              static_cast<int>(begin_command));
      enterFatalVulkanError(frame, "vkBeginCommandBuffer", begin_command);
      return;
    }

    auto write_timestamp = [&](GpuTimestampQuery query,
                               VkPipelineStageFlagBits stage) {
      if (timestamp_queries_enabled_ && fs.timestamp_pool) {
        vkCmdWriteTimestamp(fs.cmd, stage, fs.timestamp_pool,
                            queryIndex(query));
      }
    };
    if (timestamp_queries_enabled_ && fs.timestamp_pool) {
      vkCmdResetQueryPool(fs.cmd, fs.timestamp_pool, 0,
                          kGpuTimestampQueryCount);
      write_timestamp(GpuTimestampQuery::FrameBegin,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    }

    // Record pending BLAS full-build / animation refit before any ray queries.
    write_timestamp(GpuTimestampQuery::AsBegin,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    if (rt_as_needs_record) {
      const RtSceneStats before_build = rt_scene.stats();
      rt_scene.recordBuilds(fs.cmd);
      const RtSceneStats after_build = rt_scene.stats();
      if (diagnostics_enabled_ &&
          (diagnostic_rt_as_events_logged_ < 64u ||
           after_build.full_builds != before_build.full_builds ||
           after_build.refits != before_build.refits)) {
        xpbd::log::infof(
            "VKDIAG rt_as frame=%llu slot=%zu event=%llu "
            "reason=%s blas_full_delta=%llu blas_refit_delta=%llu "
            "full_total=%llu refit_total=%llu blas=%u instances=%u tlas=1",
            static_cast<unsigned long long>(
                frame.diagnostics.render_frame),
            frame_index_,
            static_cast<unsigned long long>(diagnostic_rt_as_events_logged_),
            rtAccelerationBuildReasonName(after_build.last_build_reason),
            static_cast<unsigned long long>(
                after_build.full_builds - before_build.full_builds),
            static_cast<unsigned long long>(
                after_build.refits - before_build.refits),
            static_cast<unsigned long long>(after_build.full_builds),
            static_cast<unsigned long long>(after_build.refits),
            static_cast<unsigned>(after_build.blas_count),
            static_cast<unsigned>(after_build.instance_count));
      }
      ++diagnostic_rt_as_events_logged_;
    }
    write_timestamp(GpuTimestampQuery::AsEnd,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    const RtSceneStats active_rt_stats = rt_scene.stats();
    stats_.rt_blas_count = 0;
    stats_.rt_tlas_count = 0;
    stats_.rt_instance_count = 0;
    stats_.rt_visible_instance_mask_count = 0;
    stats_.rt_hidden_instance_mask_count = 0;
    stats_.rt_positive_emitter_count = 0;
    stats_.rt_hidden_source_emitter_triangle_count = 0;
    stats_.rt_hidden_positive_weight_triangle_count = 0;
    stats_.rt_as_storage_bytes = 0;
    stats_.rt_scratch_bytes = 0;
    stats_.rt_attribute_bytes = 0;
    stats_.rt_allocated_bytes = 0;
    stats_.rt_full_builds = 0;
    stats_.rt_refits = 0;
    stats_.rt_tlas_full_builds = 0;
    stats_.rt_tlas_updates = 0;
    stats_.rt_upload_bytes = rt_upload_bytes_frame;
    stats_.cpu_emitter_distribution_ms = rt_emitter_distribution_frame_ms;
    stats_.rt_emitter_distribution_rebuilds = 0;
    stats_.rt_descriptor_write_calls = 0;
    stats_.rt_descriptor_cache_hits = 0;
    stats_.rt_descriptor_entries_written = 0;
    for (const auto &scene : rt_scenes_) {
      const RtSceneStats scene_stats = scene.stats();
      stats_.rt_blas_count += scene_stats.blas_count;
      stats_.rt_tlas_count += scene_stats.tlas_count;
      stats_.rt_instance_count += scene_stats.instance_count;
      stats_.rt_as_storage_bytes += scene_stats.as_storage_bytes;
      stats_.rt_scratch_bytes += scene_stats.scratch_bytes;
      stats_.rt_attribute_bytes += scene_stats.attribute_bytes;
      stats_.rt_allocated_bytes += scene_stats.allocated_bytes;
      stats_.rt_full_builds += scene_stats.full_builds;
      stats_.rt_refits += scene_stats.refits;
      stats_.rt_tlas_full_builds += scene_stats.tlas_full_builds;
      stats_.rt_tlas_updates += scene_stats.tlas_updates;
      stats_.rt_emitter_distribution_rebuilds +=
          scene_stats.emitter_distribution_rebuilds;
      stats_.rt_descriptor_write_calls += scene_stats.descriptor_write_count;
      stats_.rt_descriptor_cache_hits += scene_stats.descriptor_cache_hits;
    }
    stats_.rt_primitive_count = active_rt_stats.primitive_count;
    stats_.rt_visible_instance_mask_count =
        active_rt_stats.visible_instance_mask_count;
    stats_.rt_hidden_instance_mask_count =
        active_rt_stats.hidden_instance_mask_count;
    stats_.rt_positive_emitter_count =
        active_rt_stats.positive_emitter_count;
    stats_.rt_hidden_source_emitter_triangle_count =
        active_rt_stats.hidden_source_emitter_triangle_count;
    stats_.rt_hidden_positive_weight_triangle_count =
        active_rt_stats.hidden_positive_weight_triangle_count;
    stats_.rt_last_build_reason = active_rt_stats.last_build_reason;
    stats_.rt_last_tlas_reason = active_rt_stats.last_tlas_reason;
    stats_.rt_descriptor_write_calls = rt_descriptor_write_calls_frame_;
    stats_.rt_descriptor_cache_hits = rt_descriptor_cache_hits_frame_;
    stats_.rt_descriptor_entries_written =
        rt_descriptor_entries_written_frame_;

    // Built-in path tracer dispatch (RT Pipeline preferred, before the
    // graphics pass).
    PathTraceFrameParams pt_params{};
    bool pt_dlss_active = false;
    bool pt_target_exact_for_frame = false;
    bool pt_motion_guide_ready_for_frame = false;
    bool pt_dispatch_recorded_for_frame = false;
    bool pt_temporal_reconstruction_requested_for_frame = false;
    bool pt_history_reset = true;
    bool pt_streamline_history_reset = true;
    std::uint64_t pt_streamline_history_key = 0u;
    PathTracePostProcessCapabilities pt_post_capabilities{};
    pt_post_capabilities.dlss_ray_reconstruction =
        streamline_vulkan_runtime_.dlssRayReconstructionSupported();
    pt_post_capabilities.dlss_super_resolution =
        streamline_vulkan_runtime_.dlssSupported();
    pt_post_capabilities.dlaa =
        streamline_vulkan_runtime_.dlssSupported();
    pt_post_capabilities.dlss_frame_generation =
        frame_generation_supported;
    pt_post_capabilities.reflex =
        streamline_vulkan_runtime_.reflexSupported();
    PathTracePostProcessState pt_post{};
    if (path_trace_active && draw_viewport) {
      PathTraceSettings path_settings =
          normalizePathTraceSettings(frame.path_trace_settings);
      if (interactive_preview_resize) {
        // Splitter motion produces a stream of temporary output extents. Keep
        // the current PT images alive, render one responsive raw sample, and
        // restart temporal reconstruction once the final extent is known.
        path_settings.samples_per_frame = 1u;
        streamline_vulkan_runtime_.invalidateDlssHistory();
        streamline_temporal_history_valid_ = false;
      }
      pt_post = resolvePathTracePostProcess(
          path_settings, pt_post_capabilities);
      const std::uint32_t preview_output_width =
          static_cast<std::uint32_t>((std::max)(1, safe_viewport.w));
      const std::uint32_t preview_output_height =
          static_cast<std::uint32_t>((std::max)(1, safe_viewport.h));
      StreamlineDlssOptimalSettings dlss_settings{};
      bool pt_rr_requested =
          pt_post.active_denoiser ==
          PathTraceDenoiser::DlssRayReconstruction;
      pt_temporal_reconstruction_requested_for_frame =
          pt_rr_requested ||
          pt_post.active_upscale != PathTraceUpscale::Off;
      const std::uint32_t supported_target_outputs =
          path_tracer.supportedTargetOutputMask();
      if (pt_rr_requested &&
          (supported_target_outputs & kPathTraceAllRrGuideOutputMask) !=
              kPathTraceAllRrGuideOutputMask) {
        if (!streamline_rr_target_format_failure_logged_) {
          xpbd::log::warnf(
              "DLSS Ray Reconstruction disabled: required target formats "
              "unsupported (required=0x%04x supported=0x%04x)",
              static_cast<unsigned>(kPathTraceAllRrGuideOutputMask),
              static_cast<unsigned>(supported_target_outputs));
          streamline_rr_target_format_failure_logged_ = true;
        }
        pt_rr_requested = false;
        pt_post.active_denoiser = PathTraceDenoiser::Raw;
        pt_post.reconstruction_mode = PathTraceUpscale::Off;
        streamline_vulkan_runtime_.invalidateDlssHistory();
      } else {
        streamline_rr_target_format_failure_logged_ = false;
      }
      if (!interactive_preview_resize && pt_rr_requested) {
        dlss_settings =
            streamline_vulkan_runtime_
                .queryDlssRayReconstructionOptimalSettings(
                    pt_post.reconstruction_mode,
                    preview_output_width, preview_output_height);
        if (!dlss_settings.valid) {
          if (!streamline_dlss_failure_logged_) {
            xpbd::log::warnf(
                "DLSS Ray Reconstruction mode unavailable: %s",
                streamline_vulkan_runtime_.status().c_str());
            streamline_dlss_failure_logged_ = true;
          }
          pt_post.active_denoiser = PathTraceDenoiser::Raw;
          pt_post.reconstruction_mode = PathTraceUpscale::Off;
          streamline_vulkan_runtime_.invalidateDlssHistory();
        }
      } else if (!interactive_preview_resize &&
                 pt_post.active_upscale != PathTraceUpscale::Off) {
        dlss_settings =
            streamline_vulkan_runtime_.queryDlssOptimalSettings(
                pt_post.active_upscale, preview_output_width,
                preview_output_height);
        if (!dlss_settings.valid) {
          if (!streamline_dlss_failure_logged_) {
            xpbd::log::warnf(
                "DLSS Super Resolution mode unavailable: %s",
                streamline_vulkan_runtime_.status().c_str());
            streamline_dlss_failure_logged_ = true;
          }
          pt_post.active_upscale = PathTraceUpscale::Off;
          streamline_vulkan_runtime_.invalidateDlssHistory();
        }
      }
      const float preview_scale =
          path_settings.preview_resolution_scale;
      const std::uint32_t requested_ptw =
          dlss_settings.valid
              ? dlss_settings.render_width
              : static_cast<std::uint32_t>((std::max)(
                    1, static_cast<int>(std::lround(
                           static_cast<float>(safe_viewport.w) *
                           preview_scale))));
      const std::uint32_t requested_pth =
          dlss_settings.valid
              ? dlss_settings.render_height
              : static_cast<std::uint32_t>((std::max)(
                    1, static_cast<int>(std::lround(
                           static_cast<float>(safe_viewport.h) *
                           preview_scale))));
      const PathTraceTargetExtent target_extent =
          choosePathTraceTargetExtent(
              requested_ptw, requested_pth, path_tracer.targetWidth(),
              path_tracer.targetHeight(), interactive_preview_resize);
      std::uint32_t requested_output_mask = 0u;
      if (dlss_settings.valid ||
          (desired_frame_generation && !interactive_preview_resize)) {
        requested_output_mask |= kPathTraceRrMotionOutputMask;
      }
      if (dlss_settings.valid && pt_rr_requested) {
        requested_output_mask |= kPathTraceAllRrGuideOutputMask;
      }
      const PathTraceTargetResult target_result = path_tracer.ensureTarget(
          target_extent.width, target_extent.height,
          PathTraceTargetRequirements{requested_output_mask});
      if (target_result.failure.vk_result == VK_ERROR_DEVICE_LOST) {
        enterFatalVulkanError(frame, "VulkanPathTracer::ensureTarget",
                              VK_ERROR_DEVICE_LOST);
        return;
      }
      const bool target_has_requested_outputs =
          (target_result.allocated_output_mask & requested_output_mask) ==
          requested_output_mask;
      if (target_result.hasActiveTarget()) {
        const std::uint32_t ptw = target_result.active_width;
        const std::uint32_t pth = target_result.active_height;
        if (!target_result.exact() || !target_has_requested_outputs) {
          // Candidate failure keeps the last good color/depth alive. Avoid
          // feeding mismatched extents or dummy guides to Streamline while the
          // same-key retry backoff is active; composite the usable raw target.
          dlss_settings = {};
          pt_post.active_denoiser = PathTraceDenoiser::Raw;
          pt_post.active_upscale = PathTraceUpscale::Off;
          requested_output_mask = 0u;
          streamline_vulkan_runtime_.invalidateDlssHistory();
          streamline_temporal_history_valid_ = false;
        }
        pt_target_exact_for_frame = target_result.exact();
        pt_motion_guide_ready_for_frame =
            target_result.exact() &&
            (requested_output_mask & kPathTraceRrMotionOutputMask) != 0u &&
            (target_result.allocated_output_mask &
             kPathTraceRrMotionOutputMask) != 0u;
        pt_params.view = frame.view_matrix;
        pt_params.proj = frame.proj_matrix;
        pt_params.previous_view =
            rt_motion_camera_history_valid_
                ? rt_motion_previous_view_.data()
                : nullptr;
        pt_params.previous_proj =
            rt_motion_camera_history_valid_
                ? rt_motion_previous_proj_.data()
                : nullptr;
        pt_params.motion_history_valid =
            rt_motion_camera_history_valid_;
        pt_params.temporal_reconstruction_input = dlss_settings.valid;
        pt_params.ray_reconstruction_guides =
            dlss_settings.valid && pt_rr_requested;
        pt_params.output_write_mask = requested_output_mask;
        if (frame.raster_scene) {
          pt_params.light_dir[0] = frame.raster_scene->lighting.direction[0];
          pt_params.light_dir[1] = frame.raster_scene->lighting.direction[1];
          pt_params.light_dir[2] = frame.raster_scene->lighting.direction[2];
          pt_params.ambient = frame.raster_scene->lighting.ambient;
          pt_params.light_color[0] = frame.raster_scene->lighting.color[0];
          pt_params.light_color[1] = frame.raster_scene->lighting.color[1];
          pt_params.light_color[2] = frame.raster_scene->lighting.color[2];
          pt_params.intensity = frame.raster_scene->lighting.intensity;
        }
        pt_params.clear_r = frame.clear_r;
        pt_params.clear_g = frame.clear_g;
        pt_params.clear_b = frame.clear_b;
        pt_params.width = ptw;
        pt_params.height = pth;
        pt_params.frame_index = path_trace_frame_index_++;
        if (pt_params.temporal_reconstruction_input) {
          const auto jitter = pathTraceTemporalJitter(
              pt_params.frame_index, ptw, preview_output_width);
          pt_params.camera_jitter[0] = jitter[0];
          pt_params.camera_jitter[1] = jitter[1];
        }
        pt_params.frame_slot =
            static_cast<std::uint32_t>(frame_index_);
        pt_params.settings = path_settings;
        pt_params.output_requires_srgb_encoding =
            swap_format_ == VK_FORMAT_B8G8R8A8_UNORM ||
            swap_format_ == VK_FORMAT_R8G8B8A8_UNORM;
        pt_params.exposure =
            std::exp2(pt_params.settings.display_exposure_ev);
        if (importance_environment_ready) {
          pt_params.settings.analytic_environment_strength = 0.0f;
        }
        pt_params.material_debug_view = frame.material_debug_view;
        pt_params.rt_debug_view = frame.rt_debug_view;
        pt_params.material_feature_flags =
            labPbrFeatureFlags(frame.static_model_material);
        if (procedural_environment_ready) {
          // The finite solar disk is already part of the importance-sampled
          // dynamic cache; avoid double-counting a legacy directional light.
          pt_params.ambient = 0.0f;
          pt_params.intensity = 0.0f;
        }
        pt_params.hdr_environment = importance_environment_ready;
        pt_params.environment_view =
            hdr_environment_ready
                ? world_environment_texture_.view
                : (procedural_environment_ready
                       ? atmosphere_environment_cache_.view
                       : VK_NULL_HANDLE);
        pt_params.environment_sampler =
            importance_environment_ready ? world_environment_sampler_
                                         : VK_NULL_HANDLE;
        pt_params.environment_distribution =
            hdr_environment_ready
                ? world_environment_distribution_.buffer
                : (procedural_environment_ready
                       ? atmosphere_environment_distribution_.buffer
                       : VK_NULL_HANDLE);
        pt_params.environment_distribution_bytes =
            hdr_environment_ready
                ? world_environment_distribution_bytes_
                : (procedural_environment_ready
                       ? atmosphere_environment_distribution_bytes_
                       : 0u);
        pt_params.viewport_x = safe_viewport.x;
        pt_params.viewport_y = safe_viewport.y;
        pt_params.viewport_w = safe_viewport.w;
        pt_params.viewport_h = safe_viewport.h;
        // Raw and debug views are written directly to color/depth. Only
        // temporal reconstruction asks for motion/RR guides; AOV/statistics
        // stay lazy until an explicit export/inspection consumer requests
        // them.

        // The key contains only radiance/visibility-affecting state. SPP and
        // maximum_samples intentionally remain compatible with the current
        // average and are handled by the slot-local accumulation transition.
        std::uint64_t history_key = 14695981039346656037ull;
        appendPathTraceHistoryValue(history_key,
                                    frame.static_model_generation);
        appendPathTraceHistoryValue(history_key,
                                    frame.static_texture_generation);
        appendPathTraceHistoryValue(history_key, safe_viewport.x);
        appendPathTraceHistoryValue(history_key, safe_viewport.y);
        appendPathTraceHistoryValue(history_key, ptw);
        appendPathTraceHistoryValue(history_key, pth);
        appendPathTraceHistoryBytes(history_key, pt_params.light_dir,
                                    sizeof(pt_params.light_dir));
        appendPathTraceHistoryValue(history_key, pt_params.ambient);
        appendPathTraceHistoryBytes(history_key, pt_params.light_color,
                                    sizeof(pt_params.light_color));
        appendPathTraceHistoryValue(history_key, pt_params.intensity);
        appendPathTraceHistoryValue(history_key, pt_params.clear_r);
        appendPathTraceHistoryValue(history_key, pt_params.clear_g);
        appendPathTraceHistoryValue(history_key, pt_params.clear_b);
        appendPathTraceHistoryValue(
            history_key,
            static_cast<std::uint32_t>(pt_params.material_debug_view));
        appendPathTraceHistoryValue(
            history_key,
            static_cast<std::uint32_t>(pt_params.rt_debug_view));
        appendPathTraceHistoryValue(
            history_key, pt_params.material_feature_flags);
        appendPathTraceHistoryValue(
            history_key, pt_params.output_write_mask);
        appendPathTraceHistoryValue(
            history_key, resolvedPathTraceSeed(pt_params.settings));
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_diffuse_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_glossy_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_transmission_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_transparent_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.russian_roulette_start);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.russian_roulette);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.analytic_lights);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.emissive_surfaces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.next_event_estimation);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.multiple_importance_sampling);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.environment_importance_sampling);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.emissive_mesh_sampling);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.emissive_multiplier);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.light_samples_per_path);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.direct_clamp);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.indirect_clamp);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.nvidia_rt_core_acceleration);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.force_software_fallback);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.reset_generation);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.target_generation);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.analytic_environment_strength);
        appendPathTraceHistoryValue(
            history_key,
            static_cast<std::uint32_t>(
                resolved_world_environment.sky_rendering));
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.lighting_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.celestial_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.cloud_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.target_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.environment_lighting);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.sun_moon_lighting);
        appendPathTraceHistoryValue(
            history_key,
            resolved_world_environment.global_lighting_strength);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.environment_strength);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.rotation_radians);
        if (resolved_world_environment.hdr != nullptr) {
          appendPathTraceHistoryBytes(
              history_key,
              resolved_world_environment.hdr->checksum.data(),
              resolved_world_environment.hdr->checksum.size());
        }
        if (procedural_environment_ready) {
          appendPathTraceHistoryBytes(
              history_key, atmosphere_environment_key_.data(),
              atmosphere_environment_key_.size());
        }

        // Streamline temporal compatibility intentionally excludes current
        // camera matrices and deforming vertex positions. Dense motion vectors
        // describe ordinary camera, skeletal, and preview-surface motion. Only
        // topology/material/radiance changes, a missing motion history, or a
        // post-process/output reconfiguration starts a new network history.
        std::uint64_t streamline_history_key = history_key;
        appendPathTraceHistoryValue(
            streamline_history_key, rt_streamline_topology_hash);
        appendPathTraceHistoryValue(
            streamline_history_key, rt_streamline_material_hash);
        if (frame.raster_scene != nullptr) {
          appendPathTraceHistoryValue(
              streamline_history_key,
              static_cast<std::uint32_t>(frame.raster_scene->id));
        }
        appendPathTraceHistoryValue(
            streamline_history_key,
            pt_params.settings.post_process_generation);
        appendPathTraceHistoryValue(
            streamline_history_key, pt_rr_requested);
        appendPathTraceHistoryValue(
            streamline_history_key,
            static_cast<std::uint32_t>(
                pt_rr_requested ? pt_post.reconstruction_mode
                                : pt_post.active_upscale));
        appendPathTraceHistoryValue(
            streamline_history_key, preview_output_width);
        appendPathTraceHistoryValue(
            streamline_history_key, preview_output_height);
        pt_streamline_history_key = streamline_history_key;
        pt_streamline_history_reset =
            shouldResetTemporalReconstructionHistory(
                streamline_temporal_history_valid_,
                streamline_temporal_history_key_,
                pt_streamline_history_key,
                pt_params.motion_history_valid);

        // Raw PT accumulation has stricter compatibility: any camera or
        // deforming-scene change invalidates the per-pixel sample average.
        appendPathTraceHistoryValue(history_key, last_rt_scene_hash);
        if (frame.raster_scene != nullptr) {
          appendPathTraceHistoryValue(
              history_key, frame.raster_scene->geometry_generation);
        }
        if (frame.view_matrix != nullptr) {
          appendPathTraceHistoryBytes(history_key, frame.view_matrix,
                                      sizeof(float) * 16u);
        }
        if (frame.proj_matrix != nullptr) {
          appendPathTraceHistoryBytes(history_key, frame.proj_matrix,
                                      sizeof(float) * 16u);
        }
        pt_params.history_key = history_key;

        // Performance controls affect only the dispatch schedule, never the
        // compatibility key. Use the previous completed GPU timing to reduce
        // work toward the target frame time, and use the selected interactive
        // tier for the first sample after camera/scene motion.
        if (stats_.gpu_timestamp_valid &&
            stats_.gpu_timestamp_total_ms >
                pt_params.settings.target_frame_time_ms * 1.05f) {
          const float ratio =
              pt_params.settings.target_frame_time_ms /
              (std::max)(stats_.gpu_timestamp_total_ms, 0.001f);
          pt_params.settings.samples_per_frame =
              (std::max)(1u, static_cast<std::uint32_t>(
                                std::floor(
                                    static_cast<float>(
                                        pt_params.settings
                                            .samples_per_frame) *
                                    std::clamp(ratio, 0.05f, 1.0f))));
        }
        if (path_tracer.historyKey() != history_key) {
          switch (pt_params.settings.interactive_quality) {
          case PathTraceInteractiveQuality::Fast:
            pt_params.settings.samples_per_frame = 1u;
            break;
          case PathTraceInteractiveQuality::Balanced:
            pt_params.settings.samples_per_frame =
                (std::min)(pt_params.settings.samples_per_frame, 2u);
            break;
          case PathTraceInteractiveQuality::Full:
            break;
          }
        }
        if (interactive_preview_resize) {
          pt_params.settings.samples_per_frame = 1u;
        }
        pt_history_reset =
            path_tracer.historyKey() != history_key ||
            !pt_params.motion_history_valid;
        if (pt_history_reset && diagnostics_enabled_ &&
            diagnostic_pt_history_resets_logged_ < 128u) {
          std::uint64_t view_hash = 14695981039346656037ull;
          std::uint64_t projection_hash = 14695981039346656037ull;
          std::uint64_t light_hash = 14695981039346656037ull;
          if (frame.view_matrix != nullptr) {
            appendPathTraceHistoryBytes(
                view_hash, frame.view_matrix, sizeof(float) * 16u);
          }
          if (frame.proj_matrix != nullptr) {
            appendPathTraceHistoryBytes(
                projection_hash, frame.proj_matrix, sizeof(float) * 16u);
          }
          appendPathTraceHistoryBytes(
              light_hash, pt_params.light_dir,
              sizeof(pt_params.light_dir));
          appendPathTraceHistoryValue(light_hash, pt_params.ambient);
          appendPathTraceHistoryBytes(
              light_hash, pt_params.light_color,
              sizeof(pt_params.light_color));
          appendPathTraceHistoryValue(light_hash, pt_params.intensity);
          const std::uint64_t raster_generation =
              frame.raster_scene != nullptr
                  ? frame.raster_scene->geometry_generation
                  : 0u;
          xpbd::log::infof(
              "VKDIAG pt_history_components frame=%u slot=%u "
              "key=%016llx rt=%016llx view=%016llx proj=%016llx "
              "static=%llu/%llu raster=%llu settings=%llu/%llu "
              "world=%llu/%llu/%llu/%llu/%llu light=%016llx "
              "motion=%d sl_key=%016llx sl_reset=%d sl_valid=%d",
              pt_params.frame_index,
              static_cast<unsigned>(frame_index_),
              static_cast<unsigned long long>(history_key),
              static_cast<unsigned long long>(last_rt_scene_hash),
              static_cast<unsigned long long>(view_hash),
              static_cast<unsigned long long>(projection_hash),
              static_cast<unsigned long long>(
                  frame.static_model_generation),
              static_cast<unsigned long long>(
                  frame.static_texture_generation),
              static_cast<unsigned long long>(raster_generation),
              static_cast<unsigned long long>(
                  pt_params.settings.reset_generation),
              static_cast<unsigned long long>(
                  pt_params.settings.target_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.lighting_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.celestial_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.cloud_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.target_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.generation),
              static_cast<unsigned long long>(light_hash),
              pt_params.motion_history_valid ? 1 : 0,
              static_cast<unsigned long long>(
                  pt_streamline_history_key),
              pt_streamline_history_reset ? 1 : 0,
              streamline_temporal_history_valid_ ? 1 : 0);
          ++diagnostic_pt_history_resets_logged_;
        }

        // Sample the static model atlas so path-traced models keep textures
        // (avoids white clay look). Every pixel-art atlas channel uses nearest
        // base-level sampling so normal and LabPBR texels stay aligned with
        // albedo and alpha coverage.
        const VkImageView albedo_view =
            static_texture_.view != VK_NULL_HANDLE ? static_texture_.view
                                                   : VK_NULL_HANDLE;
        const VkImageView normal_view =
            static_normal_texture_.view != VK_NULL_HANDLE
                ? static_normal_texture_.view
                : VK_NULL_HANDLE;
        const VkImageView specular_view =
            static_specular_texture_.view != VK_NULL_HANDLE
                ? static_specular_texture_.view
                : VK_NULL_HANDLE;
        write_timestamp(GpuTimestampQuery::PathTraceBegin,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (target_result.exact()) {
          path_tracer.recordDispatch(fs.cmd, rt_scene, pt_params, albedo_view,
                                     normal_view, specular_view,
                                     static_albedo_sampler_,
                                     static_normal_sampler_,
                                     static_specular_sampler_);
          pt_dispatch_recorded_for_frame =
              path_tracer.lastDispatchRecorded();
          stats_.rt_aov_write_mask = path_tracer.lastOutputWriteMask();
        } else {
          stats_.rt_aov_write_mask = 0u;
        }
        const VulkanPathTracer::DescriptorStats path_descriptor_stats =
            path_tracer.descriptorStats();
        stats_.rt_descriptor_write_calls += path_descriptor_stats.write_calls;
        stats_.rt_descriptor_cache_hits += path_descriptor_stats.cache_hits;
        stats_.rt_descriptor_entries_written +=
            path_descriptor_stats.entries_written;
        stats_.cpu_descriptor_update_ms += path_descriptor_stats.update_ms;
        write_timestamp(GpuTimestampQuery::PathTraceEnd,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (dlss_settings.valid && pt_dispatch_recorded_for_frame) {
          auto fill_dlss_frame =
              [&](StreamlineDlssFrame &dlss_frame,
                  PathTraceUpscale mode) {
            dlss_frame.command_buffer = fs.cmd;
            dlss_frame.color_image = path_tracer.colorImage();
            dlss_frame.color_memory = path_tracer.colorMemory();
            dlss_frame.color_view = path_tracer.colorView();
            dlss_frame.depth_image = path_tracer.depthImage();
            dlss_frame.depth_memory = path_tracer.depthMemory();
            dlss_frame.depth_view = path_tracer.depthView();
            // DLSS SR and RR both require an exact RG16F/RG32F dense-motion
            // input. Reuse the dedicated RG32F guide instead of the wider
            // diagnostic AOV layer.
            dlss_frame.motion_image = path_tracer.rrMotionImage();
            dlss_frame.motion_memory = path_tracer.rrMotionMemory();
            dlss_frame.motion_view = path_tracer.rrMotionView();
            dlss_frame.view = pt_params.view;
            dlss_frame.projection = pt_params.proj;
            dlss_frame.previous_view = pt_params.previous_view;
            dlss_frame.previous_projection =
                pt_params.previous_proj;
            dlss_frame.render_width = ptw;
            dlss_frame.render_height = pth;
            dlss_frame.output_width = preview_output_width;
            dlss_frame.output_height = preview_output_height;
            // Streamline, Reflex, SR/RR, FG, and present must share the same
            // application-frame token. The path-trace frame index remains a
            // separate temporal sampling sequence.
            dlss_frame.frame_index = static_cast<std::uint32_t>(
                frame.diagnostics.render_frame);
            dlss_frame.frame_slot = pt_params.frame_slot;
            dlss_frame.jitter_x = pt_params.camera_jitter[0];
            dlss_frame.jitter_y = pt_params.camera_jitter[1];
            dlss_frame.mode = mode;
            dlss_frame.reset_history =
                pt_streamline_history_reset;
          };
          if (pt_rr_requested) {
            StreamlineDlssRayReconstructionFrame rr_frame{};
            fill_dlss_frame(rr_frame, pt_post.reconstruction_mode);
            rr_frame.diffuse_albedo_image =
                path_tracer.rrDiffuseAlbedoImage();
            rr_frame.diffuse_albedo_memory =
                path_tracer.rrDiffuseAlbedoMemory();
            rr_frame.diffuse_albedo_view =
                path_tracer.rrDiffuseAlbedoView();
            rr_frame.specular_albedo_image =
                path_tracer.rrSpecularAlbedoImage();
            rr_frame.specular_albedo_memory =
                path_tracer.rrSpecularAlbedoMemory();
            rr_frame.specular_albedo_view =
                path_tracer.rrSpecularAlbedoView();
            rr_frame.normal_roughness_image =
                path_tracer.rrNormalRoughnessImage();
            rr_frame.normal_roughness_memory =
                path_tracer.rrNormalRoughnessMemory();
            rr_frame.normal_roughness_view =
                path_tracer.rrNormalRoughnessView();
            rr_frame.specular_hit_distance_image =
                path_tracer.rrSpecularHitDistanceImage();
            rr_frame.specular_hit_distance_memory =
                path_tracer.rrSpecularHitDistanceMemory();
            rr_frame.specular_hit_distance_view =
                path_tracer.rrSpecularHitDistanceView();
            pt_dlss_active =
                streamline_vulkan_runtime_
                    .recordDlssRayReconstruction(rr_frame);
          } else {
            StreamlineDlssFrame dlss_frame{};
            fill_dlss_frame(dlss_frame, pt_post.active_upscale);
            pt_dlss_active =
                streamline_vulkan_runtime_.recordDlss(dlss_frame);
          }
          if (pt_dlss_active) {
            streamline_temporal_history_key_ =
                pt_streamline_history_key;
            streamline_temporal_history_valid_ = true;
            if (pt_rr_requested &&
                !streamline_rr_active_logged_) {
              xpbd::log::infof(
                  "Path-trace post-process active: PT noisy HDR -> "
                  "DLSS Ray Reconstruction (%ux%u -> %ux%u mode=%u)",
                  ptw, pth, preview_output_width,
                  preview_output_height,
                  static_cast<unsigned>(
                      pt_post.reconstruction_mode));
              streamline_rr_active_logged_ = true;
            }
            if (!pt_rr_requested) {
              streamline_rr_active_logged_ = false;
            }
            if (streamline_dlss_failure_logged_) {
              xpbd::log::infof(
                  "DLSS reconstruction recovered: input=%ux%u "
                  "output=%ux%u",
                  ptw, pth, preview_output_width,
                  preview_output_height);
            }
            streamline_dlss_failure_logged_ = false;
          } else {
            streamline_temporal_history_valid_ = false;
            if (!streamline_dlss_failure_logged_) {
              xpbd::log::warnf(
                  "DLSS reconstruction dispatch disabled: %s",
                  streamline_vulkan_runtime_.status().c_str());
              streamline_dlss_failure_logged_ = true;
            }
          }
        } else {
          if (dlss_settings.valid) {
            // Target exactness alone is insufficient: pause/max-sample and
            // late dispatch prerequisites can leave last frame's guides in
            // place. Never advance Streamline history with stale inputs.
            streamline_vulkan_runtime_.invalidateDlssHistory();
          }
          streamline_temporal_history_valid_ = false;
        }
        if (pt_dispatch_recorded_for_frame && frame.view_matrix != nullptr &&
            frame.proj_matrix != nullptr) {
          std::copy_n(frame.view_matrix, 16u,
                      rt_motion_previous_view_.begin());
          std::copy_n(frame.proj_matrix, 16u,
                      rt_motion_previous_proj_.begin());
          rt_motion_camera_history_valid_ = true;
        } else if (pt_dispatch_recorded_for_frame) {
          rt_motion_camera_history_valid_ = false;
        }
      } else {
        path_trace_active = false;
      }
    }
    if (!(path_trace_active && draw_viewport)) {
      // Keep the optional timestamp pair valid on raster/fallback frames so a
      // previous slot's query result can never be mistaken for this frame.
      write_timestamp(GpuTimestampQuery::PathTraceBegin,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      write_timestamp(GpuTimestampQuery::PathTraceEnd,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    // Independent still-render accumulator. It intentionally records only on
    // frame slot zero, so the mapped readback is consumed after the same
    // slot's fence and never races the interactive accumulators.
    if (frame.still_render != nullptr &&
        frame.still_render->status != nullptr &&
        frame.still_render->job_id != 0u) {
      const StillRenderFrameRequest &request = *frame.still_render;
      StillRenderStatus &status = *request.status;
      status.job_id = request.job_id;
      status.target_samples = request.target_samples;
      status.output_path = request.output_path;

      bool terminal =
          status.state == StillRenderJobState::Completed ||
          status.state == StillRenderJobState::Failed ||
          status.state == StillRenderJobState::Cancelled;
      if (!terminal && frame_index_ == 0u &&
          still_active_job_id_ == request.job_id &&
          still_progress_job_id_ == request.job_id &&
          Clock::now() - still_last_progress_time_ >
              std::chrono::seconds(30)) {
        still_path_tracer_.cancelStillCapture();
        still_active_job_id_ = 0u;
        status.state = StillRenderJobState::Failed;
        status.error =
            "Still render made no progress for 30 seconds";
        terminal = true;
        xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                          static_cast<unsigned long long>(request.job_id),
                          status.error.c_str());
      }
      if (request.cancel_requested && !terminal) {
        if (frame_index_ == 0u) {
          still_path_tracer_.cancelStillCapture();
          still_active_job_id_ = 0u;
          status.state = StillRenderJobState::Cancelled;
          status.error.clear();
          xpbd::log::infof("STILL_JOB cancelled job_id=%llu",
                           static_cast<unsigned long long>(request.job_id));
        }
      } else if (!terminal &&
                 (active_render_path_ != RenderPath::RayTracing ||
                  !rt_capability_.device_extensions_enabled ||
                  !still_path_tracer_.ready() ||
                  !still_path_tracer_.rtPipelineReady() ||
                  !request.path_trace_settings
                       .nvidia_rt_core_acceleration ||
                   request.path_trace_settings.force_software_fallback)) {
        still_path_tracer_.cancelStillCapture();
        status.state = StillRenderJobState::Failed;
        status.error =
            "Still rendering requires an active NVIDIA Vulkan RT pipeline";
        still_active_job_id_ = 0u;
        xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                          static_cast<unsigned long long>(request.job_id),
                          status.error.c_str());
      } else if (!terminal && frame_index_ == 0u && !rt_scene.ready()) {
        if (still_waiting_job_id_ != request.job_id) {
          still_waiting_job_id_ = request.job_id;
          still_wait_started_ = Clock::now();
          xpbd::log::infof(
              "STILL_JOB begin_wait job_id=%llu reason=rt_scene_not_ready",
              static_cast<unsigned long long>(request.job_id));
        } else if (Clock::now() - still_wait_started_ >
                   std::chrono::seconds(30)) {
          status.state = StillRenderJobState::Failed;
          status.error =
              "Still render RT scene did not become ready within 30 seconds";
          still_active_job_id_ = 0u;
          xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                            static_cast<unsigned long long>(request.job_id),
                            status.error.c_str());
        }
      } else if (!terminal && rt_scene.ready() && frame_index_ == 0u) {
        still_waiting_job_id_ = 0u;
        const bool new_job = still_active_job_id_ != request.job_id;
        if (new_job) {
          still_path_tracer_.cancelStillCapture();
          still_active_job_id_ = request.job_id;
          still_path_trace_frame_index_ = 0u;
          status.accumulated_samples = 0u;
          status.error.clear();
          PathTraceStillBackgroundInput preview_background{};
          const bool use_preview_background =
              request.preview_skybox != nullptr &&
              request.preview_skybox->valid() &&
              !resolved_world_environment.background_visible;
          if (use_preview_background) {
            preview_background.face_size = static_cast<std::uint32_t>(
                request.preview_skybox->face_size);
            preview_background.rgba8 = request.preview_skybox->rgba.data();
            preview_background.rgba8_size =
                request.preview_skybox->rgba.size();
            preview_background.view = request.view_matrix;
            preview_background.proj = request.proj_matrix;
          }
          if (request.output_path.empty() ||
              !still_path_tracer_.requestStillCapture(
                  std::filesystem::path(request.output_path), request.format,
                  request.transparent_background, request.job_id,
                  use_preview_background ? &preview_background : nullptr)) {
            status.state = StillRenderJobState::Failed;
            status.error = request.output_path.empty()
                               ? "Still render output path is empty"
                               : "Still render capture request was rejected";
            still_active_job_id_ = 0u;
            xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                              static_cast<unsigned long long>(request.job_id),
                              status.error.c_str());
          } else {
            status.state = StillRenderJobState::Rendering;
            still_progress_job_id_ = request.job_id;
            still_last_logged_samples_ = 0u;
            still_last_progress_time_ = Clock::now();
            xpbd::log::infof(
                "STILL_JOB begin job_id=%llu width=%u height=%u "
                "target_samples=%u preview_background=%d",
                static_cast<unsigned long long>(request.job_id), request.width,
                request.height, request.target_samples,
                use_preview_background ? 1 : 0);
          }
        }

        if (still_active_job_id_ == request.job_id &&
            status.state != StillRenderJobState::Failed) {
          const PathTraceTargetResult still_target =
              still_path_tracer_.ensureTarget(
                  request.width, request.height,
                  PathTraceTargetRequirements{0u});
          if (still_target.failure.vk_result == VK_ERROR_DEVICE_LOST) {
            enterFatalVulkanError(frame,
                                  "VulkanPathTracer::ensureTarget(still)",
                                  VK_ERROR_DEVICE_LOST);
            return;
          }
          if (!still_target.exact()) {
            still_path_tracer_.cancelStillCapture();
            still_active_job_id_ = 0u;
            status.state = StillRenderJobState::Failed;
            const PathTraceTargetFailure &failure = still_target.failure;
            status.error =
                "Still render target unavailable: VkResult=" +
                std::string(vkResultName(failure.vk_result)) + "(" +
                std::to_string(static_cast<int>(failure.vk_result)) +
                ") resource=" +
                (failure.resource.empty() ? std::string("target-bundle")
                                          : failure.resource) +
                " format=" +
                std::to_string(static_cast<int>(failure.format)) +
                " extent=" + std::to_string(request.width) + "x" +
                std::to_string(request.height) + " estimated_bytes=" +
                std::to_string(static_cast<unsigned long long>(
                    still_target.estimated_bytes)) +
                " memory_type=" + std::to_string(failure.memory_type) +
                " heap=" + std::to_string(failure.heap) +
                " heap_budget=" +
                std::to_string(static_cast<unsigned long long>(
                    failure.heap_budget)) +
                " heap_usage=" +
                std::to_string(static_cast<unsigned long long>(
                    failure.heap_usage)) +
                " frame_slot=0 still_job_id=" +
                std::to_string(static_cast<unsigned long long>(
                    request.job_id));
            if (still_target.status ==
                PathTraceTargetStatus::RetryDeferred) {
              status.error += " retry_after_ms=" +
                              std::to_string(
                                  still_target.retry_after_ms);
            }
            xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                              static_cast<unsigned long long>(request.job_id),
                              status.error.c_str());
          } else {
            PathTraceFrameParams still_params{};
            still_params.view = request.view_matrix;
            still_params.proj = request.proj_matrix;
            still_params.motion_history_valid = false;
            if (frame.raster_scene != nullptr) {
              still_params.light_dir[0] =
                  frame.raster_scene->lighting.direction[0];
              still_params.light_dir[1] =
                  frame.raster_scene->lighting.direction[1];
              still_params.light_dir[2] =
                  frame.raster_scene->lighting.direction[2];
              still_params.ambient = frame.raster_scene->lighting.ambient;
              still_params.light_color[0] =
                  frame.raster_scene->lighting.color[0];
              still_params.light_color[1] =
                  frame.raster_scene->lighting.color[1];
              still_params.light_color[2] =
                  frame.raster_scene->lighting.color[2];
              still_params.intensity =
                  frame.raster_scene->lighting.intensity;
            }
            still_params.clear_r = frame.clear_r;
            still_params.clear_g = frame.clear_g;
            still_params.clear_b = frame.clear_b;
            still_params.width = request.width;
            still_params.height = request.height;
            still_params.frame_index = still_path_trace_frame_index_++;
            still_params.frame_slot = 0u;
            still_params.settings =
                normalizePathTraceSettings(request.path_trace_settings);
            still_params.settings.samples_per_frame =
                (std::max)(1u, request.samples_per_submit);
            still_params.settings.maximum_samples =
                (std::max)(1u, request.target_samples);
            still_params.settings.pause_accumulation = false;
            still_params.settings.transparent_background =
                request.transparent_background;
            still_params.exposure =
                std::exp2(still_params.settings.display_exposure_ev);
            if (importance_environment_ready) {
              still_params.settings.analytic_environment_strength = 0.0f;
            }
            still_params.material_debug_view =
                request.material_debug_view;
            still_params.rt_debug_view = request.rt_debug_view;
            still_params.material_feature_flags =
                labPbrFeatureFlags(frame.static_model_material);
            if (procedural_environment_ready) {
              still_params.ambient = 0.0f;
              still_params.intensity = 0.0f;
            }
            still_params.hdr_environment = importance_environment_ready;
            still_params.environment_view =
                hdr_environment_ready
                    ? world_environment_texture_.view
                    : (procedural_environment_ready
                           ? atmosphere_environment_cache_.view
                           : VK_NULL_HANDLE);
            still_params.environment_sampler =
                importance_environment_ready ? world_environment_sampler_
                                             : VK_NULL_HANDLE;
            still_params.environment_distribution =
                hdr_environment_ready
                    ? world_environment_distribution_.buffer
                    : (procedural_environment_ready
                           ? atmosphere_environment_distribution_.buffer
                           : VK_NULL_HANDLE);
            still_params.environment_distribution_bytes =
                hdr_environment_ready
                    ? world_environment_distribution_bytes_
                    : (procedural_environment_ready
                           ? atmosphere_environment_distribution_bytes_
                           : 0u);

            std::uint64_t history_key = 14695981039346656037ull;
            appendPathTraceHistoryValue(history_key, request.job_id);
            appendPathTraceHistoryValue(history_key, last_rt_scene_hash);
            appendPathTraceHistoryValue(history_key,
                                        frame.static_model_generation);
            appendPathTraceHistoryValue(history_key,
                                        frame.static_texture_generation);
            appendPathTraceHistoryValue(
                history_key, resolved_world_environment.generation);
            appendPathTraceHistoryValue(
                history_key,
                resolved_world_environment.lighting_generation);
            appendPathTraceHistoryValue(
                history_key,
                resolved_world_environment.celestial_generation);
            appendPathTraceHistoryValue(
                history_key, resolved_world_environment.cloud_generation);
            if (request.view_matrix != nullptr) {
              appendPathTraceHistoryBytes(history_key, request.view_matrix,
                                          sizeof(float) * 16u);
            }
            if (request.proj_matrix != nullptr) {
              appendPathTraceHistoryBytes(history_key, request.proj_matrix,
                                          sizeof(float) * 16u);
            }
            still_params.history_key = history_key;

            const VkImageView albedo_view =
                static_texture_.view != VK_NULL_HANDLE
                    ? static_texture_.view
                    : VK_NULL_HANDLE;
            const VkImageView normal_view =
                static_normal_texture_.view != VK_NULL_HANDLE
                    ? static_normal_texture_.view
                    : VK_NULL_HANDLE;
            const VkImageView specular_view =
                static_specular_texture_.view != VK_NULL_HANDLE
                    ? static_specular_texture_.view
                    : VK_NULL_HANDLE;
            still_path_tracer_.recordDispatch(
                fs.cmd, rt_scene, still_params, albedo_view, normal_view,
                specular_view, static_albedo_sampler_,
                static_normal_sampler_, static_specular_sampler_);
            status.accumulated_samples =
                still_path_tracer_.accumulatedSamples();
            if (still_progress_job_id_ != request.job_id) {
              still_progress_job_id_ = request.job_id;
              still_last_logged_samples_ = 0u;
            }
            if (status.accumulated_samples != still_last_logged_samples_) {
              still_last_logged_samples_ = status.accumulated_samples;
              still_last_progress_time_ = Clock::now();
              xpbd::log::infof(
                  "STILL_JOB progress job_id=%llu samples=%u target=%u",
                  static_cast<unsigned long long>(request.job_id),
                  status.accumulated_samples, status.target_samples);
            }
            switch (still_path_tracer_.captureState()) {
            case PathTraceCaptureState::Completed:
              status.state = StillRenderJobState::Completed;
              status.error.clear();
              still_active_job_id_ = 0u;
              xpbd::log::infof(
                  "STILL_JOB complete job_id=%llu samples=%u path=%s",
                  static_cast<unsigned long long>(request.job_id),
                  status.accumulated_samples, status.output_path.c_str());
              break;
            case PathTraceCaptureState::Failed:
              status.state = StillRenderJobState::Failed;
              status.error = still_path_tracer_.captureError();
              still_active_job_id_ = 0u;
              xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                                static_cast<unsigned long long>(request.job_id),
                                status.error.c_str());
              break;
            case PathTraceCaptureState::Cancelled:
              status.state = StillRenderJobState::Cancelled;
              status.error.clear();
              still_active_job_id_ = 0u;
              xpbd::log::infof("STILL_JOB cancelled job_id=%llu",
                               static_cast<unsigned long long>(request.job_id));
              break;
            case PathTraceCaptureState::PendingGpuReadback:
              if (status.state != StillRenderJobState::Saving) {
                still_last_progress_time_ = Clock::now();
                xpbd::log::infof(
                    "STILL_JOB readback job_id=%llu samples=%u",
                    static_cast<unsigned long long>(request.job_id),
                    status.accumulated_samples);
              }
              status.state = StillRenderJobState::Saving;
              break;
            case PathTraceCaptureState::Requested:
            case PathTraceCaptureState::Idle:
              status.state = StillRenderJobState::Rendering;
              break;
            }
          }
        }
      }
    }

    const bool fg_frame_candidate =
        streamline_vulkan_runtime_.swapchainOwnership() ==
            SwapchainOwnership::StreamlineFrameGenerationProxy &&
        frame_generation_available &&
        !interactive_preview_resize &&
        frameGenerationTemporalInputIsReady(
            pt_temporal_reconstruction_requested_for_frame,
            pt_dlss_active) &&
        pt_target_exact_for_frame && pt_motion_guide_ready_for_frame &&
        pt_dispatch_recorded_for_frame &&
        path_trace_active && draw_viewport &&
        image_index < swap_images_.size() &&
        image_resource.fg_hudless.image != VK_NULL_HANDLE &&
        image_resource.fg_hudless.memory != VK_NULL_HANDLE &&
        image_resource.fg_hudless.view != VK_NULL_HANDLE &&
        image_resource.fg_ui.image != VK_NULL_HANDLE &&
        image_resource.fg_ui.memory != VK_NULL_HANDLE &&
        image_resource.fg_ui.view != VK_NULL_HANDLE &&
        image_resource.fg_ui_framebuffer != VK_NULL_HANDLE &&
        image_resource.fg_overlay_framebuffer != VK_NULL_HANDLE &&
        path_tracer.depthImage() != VK_NULL_HANDLE &&
        path_tracer.depthMemory() != VK_NULL_HANDLE &&
        path_tracer.depthView() != VK_NULL_HANDLE &&
        path_tracer.rrMotionImage() != VK_NULL_HANDLE &&
        path_tracer.rrMotionMemory() != VK_NULL_HANDLE &&
        path_tracer.rrMotionView() != VK_NULL_HANDLE;
    const bool temporal_selection_active =
        pt_dlss_active || fg_frame_candidate;

    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{frame.clear_r, frame.clear_g, frame.clear_b, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image_index];
    rp.renderArea.extent = swap_extent_;
    rp.clearValueCount = 2;
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(fs.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);


    int draws = 0;
    VkViewport viewport{};
    VkRect2D viewport_scissor{};
    MeshScenePushConstants mesh_pc{};
    constexpr VkShaderStageFlags kMeshPcStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (draw_viewport) {
      viewport.x = static_cast<float>(safe_viewport.x);
      viewport.y = static_cast<float>(safe_viewport.y);
      viewport.width = static_cast<float>(safe_viewport.w);
      viewport.height = static_cast<float>(safe_viewport.h);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      viewport_scissor = {
          {safe_viewport.x, safe_viewport.y},
          {static_cast<uint32_t>(safe_viewport.w),
           static_cast<uint32_t>(safe_viewport.h)}};

      float mvp_gl[16];
      mulMat(frame.proj_matrix, frame.view_matrix, mvp_gl);
      glMvpToVulkan(mvp_gl, mesh_pc.mvp);
      if (raster != nullptr) {
        mesh_pc.light_dir[0] = raster->lighting.direction[0];
        mesh_pc.light_dir[1] = raster->lighting.direction[1];
        mesh_pc.light_dir[2] = raster->lighting.direction[2];
        mesh_pc.ambient = raster->lighting.ambient;
        mesh_pc.light_color[0] = raster->lighting.color[0];
        mesh_pc.light_color[1] = raster->lighting.color[1];
        mesh_pc.light_color[2] = raster->lighting.color[2];
        mesh_pc.intensity = raster->lighting.intensity;
      } else {
        mesh_pc.light_dir[0] = 0.35f;
        mesh_pc.light_dir[1] = 0.85f;
        mesh_pc.light_dir[2] = 0.40f;
        mesh_pc.ambient = 0.38f;
        mesh_pc.light_color[0] = 1.0f;
        mesh_pc.light_color[1] = 1.0f;
        mesh_pc.light_color[2] = 1.0f;
        mesh_pc.intensity = 0.85f;
      }
      mesh_pc.material_debug[0] =
          static_cast<std::uint32_t>(frame.material_debug_view);
      mesh_pc.material_debug[1] =
          labPbrFeatureFlags(frame.static_model_material);
    }
    const auto draw_selection_lines = [&](VkPipeline pipeline) {
      if (!draw_mesh || frame.scene == nullptr || frame.scene->lines.empty() ||
          pipeline == VK_NULL_HANDLE) {
        return;
      }
      vkCmdSetViewport(fs.cmd, 0, 1, &viewport);
      vkCmdSetScissor(fs.cmd, 0, 1, &viewport_scissor);
      vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdPushConstants(fs.cmd, mesh_layout_, kMeshPcStages, 0,
                         sizeof(mesh_pc), &mesh_pc);
      const VkDeviceSize off = mesh_upload.lines.offset_bytes;
      vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
      vkCmdDraw(fs.cmd, static_cast<uint32_t>(frame.scene->lines.size()), 1, 0,
                0);
      ++draws;
    };
    if (draw_ui) {
      stats_.ui_commands = drawUi(fs, *frame.ui, false);
      draws += stats_.ui_commands;
    }
    write_timestamp(GpuTimestampQuery::UiEnd,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);


    if (draw_viewport) {
      vkCmdSetViewport(fs.cmd, 0, 1, &viewport);
      vkCmdSetScissor(fs.cmd, 0, 1, &viewport_scissor);

      // Always clear the viewport. Full RT keeps the raster sky, then
      // composites unified model/environment visibility with explicit depth.
      {
        const float cr = raster ? raster->lighting.clear_r : (30.0f / 255.0f);
        const float cg = raster ? raster->lighting.clear_g : (30.0f / 255.0f);
        const float cb = raster ? raster->lighting.clear_b : (40.0f / 255.0f);
        VkClearAttachment cas[2]{};
        cas[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cas[0].colorAttachment = 0;
        cas[0].clearValue.color = {{cr, cg, cb, 1.0f}};
        cas[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        cas[1].clearValue.depthStencil = {1.0f, 0};
        VkClearRect crct{};
        crct.rect = viewport_scissor;
        crct.baseArrayLayer = 0;
        crct.layerCount = 1;
        vkCmdClearAttachments(fs.cmd, 2, cas, 1, &crct);
      }

      // --- Forward rasterization / unified RT composite ---
      // Raster: sky, environment, opaque model, transparent ranges, lines.
      // RT: sky only, then a unified model+environment BVH composite. Lines
      // remain raster and depth-test against the opaque depth emitted by RT.

      if (skybox_draw) {
        float view_rot[16];
        for (int i = 0; i < 16; ++i) {
          view_rot[i] = frame.view_matrix[i];
        }
        view_rot[12] = 0.0f;
        view_rot[13] = 0.0f;
        view_rot[14] = 0.0f;
        float sky_mvp_gl[16];
        mulMat(frame.proj_matrix, view_rot, sky_mvp_gl);
        SkyboxPushConstants sky_pc{};
        glMvpToVulkan(sky_mvp_gl, sky_pc.mvp);
        sky_pc.flags[0] =
            swap_format_ == VK_FORMAT_B8G8R8A8_SRGB ||
                    swap_format_ == VK_FORMAT_R8G8B8A8_SRGB
                ? 1u
                : 0u;
        constexpr VkShaderStageFlags kSkyPcStages =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          skybox_pipeline_);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                skybox_layout_, 0, 1, &skybox_desc_set_, 0,
                                nullptr);
        vkCmdPushConstants(fs.cmd, skybox_layout_, kSkyPcStages, 0,
                           sizeof(sky_pc), &sky_pc);
        const VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &skybox_vbo_.buffer, &off);
        vkCmdDraw(fs.cmd, 36, 1, 0, 0);
        ++draws;
      }

      const bool use_rt = rt_shadows_active && mesh_rt_pipeline_ &&
                          static_rt_pipeline_ && mesh_rt_layout_ &&
                          static_rt_layout_;
      VkPipeline mesh_pipe = use_rt ? mesh_rt_pipeline_ : mesh_pipeline_;
      VkPipeline mesh_pipe_trans =
          use_rt && mesh_rt_pipeline_trans_ ? mesh_rt_pipeline_trans_
                                            : mesh_pipeline_trans_;
      VkPipelineLayout mesh_lay = use_rt ? mesh_rt_layout_ : mesh_layout_;
      VkPipeline static_pipe =
          use_rt ? static_rt_pipeline_ : static_mesh_pipeline_;
      VkPipeline static_pipe_blend =
          use_rt && static_rt_pipeline_blend_ ? static_rt_pipeline_blend_
                                              : static_mesh_pipeline_blend_;
      VkPipelineLayout static_lay =
          use_rt ? static_rt_layout_ : static_mesh_layout_;
      VkDescriptorSet static_set =
          use_rt ? static_rt_descriptor_sets_[frame_index_]
                 : fs.static_descriptor_set;

      // In full RT, environment/base geometry is already in the unified BLAS.
      if (!path_trace_active && draw_raster_env) {
        const bool env_use_rt = use_rt && !path_trace_active;
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          env_use_rt ? mesh_pipe : mesh_pipeline_);
        if (env_use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, env_use_rt ? mesh_lay : mesh_layout_,
                           kMeshPcStages, 0, sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize off =
            static_cast<VkDeviceSize>(raster_env_offset);
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd,
                  static_cast<uint32_t>(raster->environment.solid.size()), 1,
                  0, 0);
        ++draws;
      }

      // Unified path-traced model + scene over the raster sky.
      if (path_trace_active) {
        path_tracer.recordComposite(
            fs.cmd, pt_params,
            pt_dlss_active
                ? streamline_vulkan_runtime_.dlssOutputView()
                : VK_NULL_HANDLE);
        ++draws;
      }

      if (!path_trace_active && draw_static &&
          (static_draw_plan_.opaque.index_count > 0 ||
           static_draw_plan_.cutout.index_count > 0)) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, static_pipe);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                static_lay, 0, 1, &static_set, 0, nullptr);
        vkCmdPushConstants(fs.cmd, static_lay, kMeshPcStages, 0,
                           sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &static_model_vbo_.buffer,
                               &offset);
        vkCmdBindIndexBuffer(fs.cmd, static_model_ibo_.buffer, 0,
                             VK_INDEX_TYPE_UINT32);
        auto draw_static_range = [&](const StaticModelIndexRange &range) {
          if (range.index_count == 0) {
            return;
          }
          vkCmdDrawIndexed(fs.cmd, range.index_count, 1, range.first_index, 0,
                           0);
          ++draws;
        };
        draw_static_range(static_draw_plan_.opaque);
        draw_static_range(static_draw_plan_.cutout);
      }

      if (!path_trace_active && draw_mesh && !frame.scene->solid.empty()) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipe);
        if (use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, mesh_lay, kMeshPcStages, 0, sizeof(mesh_pc),
                           &mesh_pc);
        const VkDeviceSize off = mesh_upload.solid.offset_bytes;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd, static_cast<uint32_t>(frame.scene->solid.size()), 1,
                  0, 0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::OpaqueEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      // Environment translucency before model alpha so water never covers
      // model semi-transparent materials (glass / stained glass / etc.).
      if (!path_trace_active && draw_raster_env_trans &&
          mesh_pipeline_trans_) {
        const bool env_use_rt = use_rt && !path_trace_active;
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          env_use_rt && mesh_pipe_trans ? mesh_pipe_trans
                                                        : mesh_pipeline_trans_);
        if (env_use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, env_use_rt ? mesh_lay : mesh_layout_,
                           kMeshPcStages, 0, sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize off =
            static_cast<VkDeviceSize>(raster_env_trans_offset);
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(
            fs.cmd,
            static_cast<uint32_t>(raster->environment.transparent.size()), 1,
            0, 0);
        ++draws;
      }
      if (!path_trace_active && draw_static &&
          static_draw_plan_.blend.index_count > 0) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          static_pipe_blend);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                static_lay, 0, 1, &static_set, 0, nullptr);
        vkCmdPushConstants(fs.cmd, static_lay, kMeshPcStages, 0,
                           sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &static_model_vbo_.buffer,
                               &offset);
        vkCmdBindIndexBuffer(fs.cmd, static_model_ibo_.buffer, 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(fs.cmd, static_draw_plan_.blend.index_count, 1,
                         static_draw_plan_.blend.first_index, 0, 0);
        ++draws;
      }
      if (!path_trace_active && draw_mesh &&
          !frame.scene->transparent.empty() && mesh_pipe_trans) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipe_trans);
        if (use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, mesh_lay, kMeshPcStages, 0, sizeof(mesh_pc),
                           &mesh_pc);
        const VkDeviceSize off = mesh_upload.transparent.offset_bytes;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd,
                  static_cast<uint32_t>(frame.scene->transparent.size()), 1, 0,
                  0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::TransparentEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      if (!fg_frame_candidate) {
        draw_selection_lines(
            temporal_selection_active ? mesh_pipeline_temporal_hud_lines_
                                      : mesh_pipeline_overlay_lines_);
      }
      if (draw_raster_grid) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipeline_lines_);
        vkCmdPushConstants(fs.cmd, mesh_layout_, kMeshPcStages, 0,
                           sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize off =
            static_cast<VkDeviceSize>(raster_grid_offset);
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd, static_cast<uint32_t>(raster->grid.lines.size()), 1,
                  0, 0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::LinesEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
      write_timestamp(GpuTimestampQuery::OpaqueEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      write_timestamp(GpuTimestampQuery::TransparentEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      write_timestamp(GpuTimestampQuery::LinesEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    if (!fg_frame_candidate && draw_ui && draw_viewport &&
        frame.ui->overlay_visible) {
      const int overlay_commands = drawUi(fs, *frame.ui, true);
      stats_.ui_commands += overlay_commands;
      draws += overlay_commands;
    }

    vkCmdEndRenderPass(fs.cmd);

    bool fg_inputs_recorded = false;
    if (fg_frame_candidate) {
      auto image_barrier =
          [&](VkImage image, VkImageLayout old_layout,
              VkImageLayout new_layout, VkAccessFlags src_access,
              VkAccessFlags dst_access,
              VkPipelineStageFlags src_stage,
              VkPipelineStageFlags dst_stage) {
            VkImageMemoryBarrier barrier{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = src_access;
            barrier.dstAccessMask = dst_access;
            barrier.oldLayout = old_layout;
            barrier.newLayout = new_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(fs.cmd, src_stage, dst_stage, 0, 0, nullptr,
                                 0, nullptr, 1, &barrier);
          };

      // Selection/hover guides are deliberately deferred, so HUDLess contains
      // only scene color described by the tagged depth and motion vectors.
      image_barrier(
          swap_images_[image_index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      const VkImageLayout previous_hudless_layout =
          image_resource.fg_hudless_layout;
      image_barrier(
          image_resource.fg_hudless.image, previous_hudless_layout,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          previous_hudless_layout == VK_IMAGE_LAYOUT_UNDEFINED
              ? 0u
              : VK_ACCESS_SHADER_READ_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          previous_hudless_layout == VK_IMAGE_LAYOUT_UNDEFINED
              ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
              : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);

      VkImageCopy copy{};
      copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.srcSubresource.layerCount = 1;
      copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.dstSubresource.layerCount = 1;
      copy.extent = {swap_extent_.width, swap_extent_.height, 1u};
      vkCmdCopyImage(
          fs.cmd, swap_images_[image_index],
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          image_resource.fg_hudless.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      image_barrier(
          image_resource.fg_hudless.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      image_resource.fg_hudless_layout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      // Alpha starts at zero. The regular UI blend state writes
      // premultiplied RGB and source alpha, exactly matching Streamline's
      // UIColorAndAlpha contract. Only UI inside the generated preview
      // subrect needs recomposition; surrounding native UI is outside the
      // tagged Backbuffer extent.
      std::array<VkClearValue, 2> fg_ui_clears{};
      fg_ui_clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
      fg_ui_clears[1].depthStencil = {1.0f, 0};
      VkRenderPassBeginInfo fg_ui_rp{
          VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      fg_ui_rp.renderPass = fg_ui_render_pass_;
      fg_ui_rp.framebuffer = image_resource.fg_ui_framebuffer;
      fg_ui_rp.renderArea.extent = swap_extent_;
      fg_ui_rp.clearValueCount =
          static_cast<std::uint32_t>(fg_ui_clears.size());
      fg_ui_rp.pClearValues = fg_ui_clears.data();
      vkCmdBeginRenderPass(fs.cmd, &fg_ui_rp,
                           VK_SUBPASS_CONTENTS_INLINE);
      draw_selection_lines(mesh_pipeline_temporal_hud_lines_);
      if (draw_ui && frame.ui->overlay_visible) {
        (void)drawUi(fs, *frame.ui, true);
      }
      vkCmdEndRenderPass(fs.cmd);
      image_resource.fg_ui_layout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      image_barrier(
          swap_images_[image_index],
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

      VkRenderPassBeginInfo fg_overlay_rp{
          VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      fg_overlay_rp.renderPass = fg_overlay_render_pass_;
      fg_overlay_rp.framebuffer =
          image_resource.fg_overlay_framebuffer;
      fg_overlay_rp.renderArea.extent = swap_extent_;
      fg_overlay_rp.clearValueCount = 0;
      fg_overlay_rp.pClearValues = nullptr;
      vkCmdBeginRenderPass(fs.cmd, &fg_overlay_rp,
                           VK_SUBPASS_CONTENTS_INLINE);
      draw_selection_lines(mesh_pipeline_temporal_hud_lines_);
      if (draw_ui && frame.ui->overlay_visible) {
        const int overlay_commands = drawUi(fs, *frame.ui, true);
        stats_.ui_commands += overlay_commands;
        draws += overlay_commands;
      }
      vkCmdEndRenderPass(fs.cmd);

      StreamlineFrameGenerationFrame fg_frame{};
      fg_frame.command_buffer = fs.cmd;
      fg_frame.depth_image = path_tracer.depthImage();
      fg_frame.depth_memory = path_tracer.depthMemory();
      fg_frame.depth_view = path_tracer.depthView();
      fg_frame.motion_image = path_tracer.rrMotionImage();
      fg_frame.motion_memory = path_tracer.rrMotionMemory();
      fg_frame.motion_view = path_tracer.rrMotionView();
      fg_frame.hudless_image = image_resource.fg_hudless.image;
      fg_frame.hudless_memory = image_resource.fg_hudless.memory;
      fg_frame.hudless_view = image_resource.fg_hudless.view;
      fg_frame.ui_image = image_resource.fg_ui.image;
      fg_frame.ui_memory = image_resource.fg_ui.memory;
      fg_frame.ui_view = image_resource.fg_ui.view;
      fg_frame.view = pt_params.view;
      fg_frame.projection = pt_params.proj;
      fg_frame.previous_view = pt_params.previous_view;
      fg_frame.previous_projection = pt_params.previous_proj;
      fg_frame.color_format = swap_format_;
      fg_frame.render_width = pt_params.width;
      fg_frame.render_height = pt_params.height;
      fg_frame.output_width = swap_extent_.width;
      fg_frame.output_height = swap_extent_.height;
      fg_frame.viewport_x =
          static_cast<std::uint32_t>(safe_viewport.x);
      fg_frame.viewport_y =
          static_cast<std::uint32_t>(safe_viewport.y);
      fg_frame.viewport_width =
          static_cast<std::uint32_t>(safe_viewport.w);
      fg_frame.viewport_height =
          static_cast<std::uint32_t>(safe_viewport.h);
      fg_frame.swapchain_image_count =
          static_cast<std::uint32_t>(swap_images_.size());
      fg_frame.frame_index = static_cast<std::uint32_t>(
          frame.diagnostics.render_frame);
      fg_frame.jitter_x = pt_params.camera_jitter[0];
      fg_frame.jitter_y = pt_params.camera_jitter[1];
      fg_frame.reset_history = pt_streamline_history_reset;
      fg_inputs_recorded =
          streamline_vulkan_runtime_.recordFrameGenerationInputs(
              fg_frame);
      if (!fg_inputs_recorded) {
        streamline_vulkan_runtime_.clearFrameGenerationInputs(
            fs.cmd, fg_frame.frame_index, fg_frame.output_width,
            fg_frame.output_height, fg_frame.viewport_x,
            fg_frame.viewport_y, fg_frame.viewport_width,
            fg_frame.viewport_height);
      }
    } else if (streamline_vulkan_runtime_.swapchainOwnership() ==
               SwapchainOwnership::StreamlineFrameGenerationProxy) {
      // NVIDIA's checklist requires explicit null tags whenever loading,
      // raster fallback, or a collapsed preview makes depth/motion invalid.
      streamline_vulkan_runtime_.clearFrameGenerationInputs(
          fs.cmd, static_cast<std::uint32_t>(
                      frame.diagnostics.render_frame),
          swap_extent_.width, swap_extent_.height,
          static_cast<std::uint32_t>(safe_viewport.x),
          static_cast<std::uint32_t>(safe_viewport.y),
          static_cast<std::uint32_t>(safe_viewport.w),
          static_cast<std::uint32_t>(safe_viewport.h));
    }
    stats_.draw_calls = draws;
    if (diagnostics_enabled_ && diagnostic_trace_frame_) {
      xpbd::log::infof(
          "VKDIAG command ts_us=%llu thread=%llu frame=%llu slot=%zu "
          "image=%u draw_calls=%d upload=%llu static_bone_upload=%llu "
          "static_resource_upload=%llu fg_recorded=%d "
          "interactive_resize=%d pt_target=%ux%u",
          static_cast<unsigned long long>(diagnosticTimestampUs()),
          static_cast<unsigned long long>(diagnosticThreadId()),
          static_cast<unsigned long long>(frame.diagnostics.render_frame),
          frame_index_, image_index, draws,
          static_cast<unsigned long long>(stats_.upload_bytes),
          static_cast<unsigned long long>(stats_.static_bone_upload_bytes),
          static_cast<unsigned long long>(
              stats_.static_resource_upload_bytes),
          fg_inputs_recorded ? 1 : 0,
          interactive_preview_resize ? 1 : 0,
          path_tracer.targetWidth(), path_tracer.targetHeight());
    }

    write_timestamp(GpuTimestampQuery::FrameEnd,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    const VkResult end_command = call_with_diagnostics(
        "vkEndCommandBuffer",
        [&] { return vkEndCommandBuffer(fs.cmd); });
    if (end_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer end failed: %d",
              static_cast<int>(end_command));
      enterFatalVulkanError(frame, "vkEndCommandBuffer", end_command);
      return;
    }

    const bool fg_acquire_handoff =
        frameGenerationAcquireMustPrecedeFrame(
            streamline_vulkan_runtime_.swapchainOwnership());
    // NVIDIA's Vulkan DLSS-G contract requires waiting on the intercepted
    // acquire semaphore before starting any work for the new frame. Native
    // presentation can retain the narrower first-swapchain-use dependency.
    VkPipelineStageFlags wait_stage =
        fg_acquire_handoff
            ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
            : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &fs.image_available;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fs.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &image_resource.render_finished;
    streamline_vulkan_runtime_.markRenderSubmitStart();
    const VkResult submit_result = call_with_diagnostics(
        "vkQueueSubmit",
        [&] { return vkQueueSubmit(graphics_queue_, 1, &si, fs.fence); });
    streamline_vulkan_runtime_.markRenderSubmitEnd();
    if (submit_result != VK_SUCCESS) {
      SDL_Log("Vulkan queue submit failed: %d",
              static_cast<int>(submit_result));
      enterFatalVulkanError(frame, "vkQueueSubmit(frame)", submit_result);
      return;
    }
    image_resource.last_in_flight = fs.fence;
    fs.timestamps_pending = timestamp_queries_enabled_ && fs.timestamp_pool;



    stats_.backend_cpu_ms =
        std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
    // GPU timestamps become readable only when this frame slot's fence is
    // waited on during a later render. Preserve the CPU-side measurements and
    // RT counters in the slot so the eventual diagnostic row cannot mix them
    // with a newer frame recorded through another slot.
    fs.perf_snapshot = stats_;
    fs.perf_render_frame = frame.diagnostics.render_frame;
    fs.perf_pending = diagnostic_trace_frame_;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &image_resource.render_finished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;
    VkSwapchainPresentFenceInfoKHR present_fence_info{
        VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR};
    if (presentFenceLifecycleEnabled()) {
      present_fence_info.swapchainCount = 1;
      present_fence_info.pFences = &image_resource.present_fence;
      pi.pNext = &present_fence_info;
    }
#ifndef NDEBUG
    if (streamline_vulkan_runtime_.swapchainOwnership() ==
        SwapchainOwnership::StreamlineFrameGenerationProxy) {
      assert(pi.pNext == nullptr &&
             "DLSS-G proxy Present must not carry a maintenance1 fence");
      assert(image_resource.present_fence == VK_NULL_HANDLE &&
             "DLSS-G proxy images must not own application present fences");
    }
#endif
    const auto present_start = Clock::now();
    logDiagnosticApi("vkQueuePresentKHR", "before", std::nullopt, 0.0,
                     image_index, fs.fence, image_resource.last_in_flight,
                     fs.cmd, false, true);
    streamline_vulkan_runtime_.markPresentStart();
    VkResult pr =
        streamline_vulkan_runtime_.queuePresent(present_queue_, &pi);
    streamline_vulkan_runtime_.markPresentEnd();
    stats_.present_succeeded =
        pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR;
    if (stats_.present_succeeded) {
      ++stats_.present_success_count;
    }
    const FrameGenerationTransitionResult fg_present_transition =
        streamline_vulkan_runtime_.updateFrameGenerationStateAfterPresent(pr);
    stats_.dlss_frame_generation_active =
        streamline_vulkan_runtime_.frameGenerationActive();
    stats_.dlss_frames_actually_presented =
        streamline_vulkan_runtime_.framesActuallyPresented();
    if (fg_present_transition == FrameGenerationTransitionResult::RecoverNative) {
      const FrameGenerationDiagnostic diagnostic =
          streamline_vulkan_runtime_.frameGenerationDiagnostic();
      fg_force_native_recovery_ = true;
      fg_recovery_reason_ = diagnostic.status;
      swapchain_recreate_target_ = SwapchainOwnership::Native;
      recreate_swapchain_ = true;
    } else if (fg_present_transition ==
               FrameGenerationTransitionResult::FatalDeviceLost) {
      enterFatalVulkanError(frame, "vkQueuePresentKHR", pr);
    }
    if (presentFenceLifecycleEnabled()) {
      image_resource.present_pending =
          pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR ||
          pr == VK_ERROR_OUT_OF_DATE_KHR ||
          pr == VK_ERROR_SURFACE_LOST_KHR ||
          pr == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT ||
          pr == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT;
    }
    logDiagnosticApi(
        "vkQueuePresentKHR", "after", pr,
        std::chrono::duration<double, std::milli>(Clock::now() - present_start)
            .count(),
        image_index, fs.fence, image_resource.last_in_flight, fs.cmd, false,
        false);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
      recreate_swapchain_ = true;
    } else if (pr != VK_SUCCESS) {
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        // The runtime has already classified the intercepted proxy result;
        // non-device-lost errors recover through a Native transaction.
        if (fg_present_transition ==
            FrameGenerationTransitionResult::FatalDeviceLost) {
          if (!fatal_error_) {
            enterFatalVulkanError(frame, "vkQueuePresentKHR", pr);
          }
        } else {
          fg_force_native_recovery_ = true;
          swapchain_recreate_target_ = SwapchainOwnership::Native;
          recreate_swapchain_ = true;
        }
      } else {
        SDL_Log("Vulkan present failed: %d", static_cast<int>(pr));
        enterFatalVulkanError(frame, "vkQueuePresentKHR", pr);
      }
    }

    frame_index_ = (frame_index_ + 1) % frames_.size();
    if (static_input) {
      stats_.cube_count =
          static_cast<int>(frame.static_model_frame->cube_count);
      stats_.line_count = frame.scene ? frame.scene->line_segment_count : 0;
    } else if (frame.scene) {
      stats_.cube_count = frame.scene->cube_count;
      stats_.line_count = frame.scene->line_segment_count;
    }
  }

  void setVSync(bool enabled) override {
    if (vsync_ == enabled) {
      return;
    }
    vsync_ = enabled;
    recreate_swapchain_ = true;
    swapchain_recreate_target_ = SwapchainOwnership::Native;
    fg_force_native_recovery_ = true;
  }
  bool vsyncEnabled() const override { return vsync_; }
  BackendKind kind() const override { return BackendKind::Vulkan; }
  const char *name() const override { return "Vulkan"; }
  const char *deviceName() const override { return device_name_.c_str(); }
  FrameStats stats() const override { return stats_; }
  PathTracePostProcessCapabilities
  pathTracePostProcessCapabilities() const override {
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
  std::string_view pathTracePostProcessStatus() const override {
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
  RayTracingCapability rayTracingCapability() const override {
    return rt_capability_;
  }
  bool supportsRayTracing() const override {
    return rt_capability_.supported && rt_capability_.device_extensions_enabled;
  }
  RenderPath activeRenderPath() const override { return active_render_path_; }

  void prepareForSystemDialog() override {
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
  void resumeAfterSystemDialog() override {
    presentation_suspended_ = false;
    // Dialog may have resized/minimized the window or stolen the GPU.
    recreate_swapchain_ = true;
    swapchain_recreate_target_ = SwapchainOwnership::Native;
    fg_force_native_recovery_ = true;
    rt_scene_built_.fill(false);
    last_rt_scene_hash_.fill(0);
    streamline_temporal_history_valid_ = false;
  }
  bool presentationSuspended() const override {
    return presentation_suspended_;
  }

private:
  void failActiveStillRender(const FrameInput &frame,
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

  void enterFatalVulkanError(const FrameInput &frame, const char *api,
                             VkResult result) {
    recordFatalVulkanError(api, result);
    failActiveStillRender(frame, fatal_error_detail_);
  }

  void recordFatalVulkanError(const char *api, VkResult result) {
    fatal_error_ = true;
    fatal_error_detail_ = std::string(api) + " failed: " +
                          vkResultName(result) + " (" +
                          std::to_string(static_cast<int>(result)) + ")";
    xpbd::log::error(fatal_error_detail_);
  }

  [[nodiscard]] bool presentFenceLifecycleEnabled() const noexcept {
    // DLSS-G owns an asynchronous proxy present. Its documented Vulkan
    // contract consumes the Present semaphore and later releases the acquired
    // image; it does not signal an application-provided maintenance1 Present
    // fence. Native swapchains retain the stronger fence lifecycle.
    return swapchain_maintenance1_enabled_ &&
           streamline_vulkan_runtime_.swapchainOwnership() ==
               SwapchainOwnership::Native;
  }

  [[nodiscard]] bool
  frameGenerationPlatformSupported() const noexcept {
    return streamline_vulkan_runtime_.frameGenerationSupported() &&
           fg_swapchain_transfer_src_supported_ &&
           fg_swapchain_color_format_supported_ &&
           graphics_family_ == present_family_;
  }

  [[nodiscard]] bool
  frameGenerationSwapchainReady() const noexcept {
    return frameGenerationPlatformSupported() && !vsync_ &&
           swap_present_mode_ == VK_PRESENT_MODE_IMMEDIATE_KHR &&
           streamline_vulkan_runtime_.swapchainOwnership() ==
               SwapchainOwnership::StreamlineFrameGenerationProxy &&
           fg_swapchain_resources_ready_;
  }

  struct StaticGpuVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
    std::uint32_t bone_index = 0;
    std::uint32_t flags = 0;
    float tx = 1.0f, ty = 0.0f, tz = 0.0f;
    float tangent_handedness = 1.0f;
  };
  static_assert(sizeof(StaticGpuVertex) == 56);
  static_assert(offsetof(StaticGpuVertex, px) == 0);
  static_assert(offsetof(StaticGpuVertex, nx) == 12);
  static_assert(offsetof(StaticGpuVertex, u) == 24);
  static_assert(offsetof(StaticGpuVertex, bone_index) == 32);
  static_assert(offsetof(StaticGpuVertex, flags) == 36);
  static_assert(offsetof(StaticGpuVertex, tx) == 40);
  static_assert(offsetof(StaticGpuVertex, tangent_handedness) == 52);
  static_assert(sizeof(StaticModelBoneState) == 80);
  static_assert(offsetof(StaticModelBoneState, transform) == 0);
  static_assert(offsetof(StaticModelBoneState, tint) == 64);

  struct alignas(16) WorldEnvironmentGpuHeader {
    std::uint32_t flags = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t entry_count = 0;
    float lighting_strength = 0.0f;
    float background_multiplier = 0.0f;
    float rotation_radians = 0.0f;
    float padding = 0.0f;
    std::array<float, 4> sun_direction_moon_mean{};
    std::array<float, 4> moon_direction_angular_radius{};
    std::array<float, 4> moon_phase_libration{};
    std::array<float, 4> sun_color_strength{};
  };
  static_assert(sizeof(WorldEnvironmentGpuHeader) == 96u);

  struct alignas(16) WorldEnvironmentGpuAlias {
    float acceptance = 0.0f;
    std::uint32_t alias_index = 0;
    float probability = 0.0f;
    float padding = 0.0f;
  };
  static_assert(sizeof(WorldEnvironmentGpuAlias) == 16u);

  struct alignas(16) AtmosphereEnvironmentPush {
    std::array<float, 4> sun_direction_observer_height{};
    std::array<float, 4> moon_direction_angular_radius{};
    std::array<float, 4> moon_phase_libration{};
    std::array<float, 4> observer_sidereal_twilight{};
    std::array<float, 4> night_parameters{};
    std::array<float, 4> cloud_layer{};
    std::array<float, 4> cloud_weather{};
    std::array<std::uint32_t, 4> cloud_quality{};
    std::array<float, 4> sky_energy{};
    std::array<std::uint32_t, 4> sky_flags{};
    std::array<float, 4> cloud_optics{};
    std::array<float, 4> cloud_lighting{};
    std::array<float, 4> cloud_post{};
    // Current-to-previous weather offset, history-valid flag, shadow grid.
    std::array<float, 4> cloud_history{};
  };
  static_assert(sizeof(AtmosphereEnvironmentPush) == 224u);

  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
    void *mapped = nullptr;
  };
  struct ImageResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
  };
  struct DynamicSkyCpuInput {
    const std::uint16_t *readback = nullptr;
    void *distribution_mapped = nullptr;
    VkDeviceSize distribution_capacity = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CelestialState celestial{};
    SunControls sun{};
    MoonControls moon{};
    bool background_visible = false;
    bool environment_lighting = false;
    bool sun_moon_lighting = false;
    float environment_strength = 0.0f;
    float background_multiplier = 0.0f;
    float rotation_radians = 0.0f;
    float moon_fraction = 0.0f;
    float moon_phase_radians = 0.0f;
  };
  struct DynamicSkyCpuResult {
    bool success = false;
    VkResult fence_result = VK_SUCCESS;
    std::string error;
    double cache_compute_ms = 0.0;
    double readback_ms = 0.0;
    double distribution_build_ms = 0.0;
    std::uint64_t positive_rgb = 0u;
    float brightest_luminance = 0.0f;
    std::uint32_t brightest_x = 0u;
    std::uint32_t brightest_y = 0u;
    double moon_probability = 0.0;
    float moon_peak_luminance = 0.0f;
  };
  struct DynamicSkyPending {
    ImageResource cache{};
    ImageResource cloud_history{};
    Buffer readback{};
    Buffer distribution{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::future<DynamicSkyCpuResult> completion{};
    std::string environment_key;
    std::string cloud_compatibility_key;
    std::array<float, 2> weather_offset{};
    std::array<float, 4> cloud_history_parameters{};
    std::uint32_t cloud_frame = 0u;
    std::uint32_t previous_cloud_frame = 0u;
    float cloud_history_weight = 0.0f;
    std::uint32_t cloud_shadow_resolution = 0u;
    bool cloud_enabled = false;
    bool cloud_history_valid = false;
    VkDeviceSize distribution_bytes = 0u;
    Clock::time_point submitted_at{};

    [[nodiscard]] bool active() const noexcept {
      return fence != VK_NULL_HANDLE;
    }
  };
  struct SwapchainImageResource {
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    ImageResource fg_hudless{};
    ImageResource fg_ui{};
    VkImageLayout fg_hudless_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout fg_ui_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFramebuffer fg_ui_framebuffer = VK_NULL_HANDLE;
    VkFramebuffer fg_overlay_framebuffer = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence present_fence = VK_NULL_HANDLE;
    bool present_pending = false;
    VkFence last_in_flight = VK_NULL_HANDLE;
  };
  struct FrameSync {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer ui_vbo;
    Buffer ui_ibo;
    Buffer mesh_vbo;
    Buffer bone_ssbo;
    VkDescriptorSet static_descriptor_set = VK_NULL_HANDLE;
    VkQueryPool timestamp_pool = VK_NULL_HANDLE;
    bool timestamps_pending = false;
    FrameStats perf_snapshot{};
    std::uint64_t perf_render_frame = 0;
    bool perf_pending = false;
  };

  SDL_Window *window_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  VkQueue present_queue_ = VK_NULL_HANDLE;
  uint32_t graphics_family_ = 0;
  uint32_t present_family_ = 0;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swap_format_ = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D swap_extent_{};
  VkPresentModeKHR swap_present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  std::vector<VkImage> swap_images_;
  std::vector<VkImageView> swap_views_;
  std::vector<SwapchainImageResource> swap_image_resources_;
  std::vector<VkFramebuffer> framebuffers_;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkRenderPass fg_ui_render_pass_ = VK_NULL_HANDLE;
  VkRenderPass fg_overlay_render_pass_ = VK_NULL_HANDLE;
  VkFormat render_pass_format_ = VK_FORMAT_UNDEFINED;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  std::array<FrameSync, 2> frames_{};
  size_t frame_index_ = 0;
  bool recreate_swapchain_ = false;
  bool fg_swapchain_transfer_src_supported_ = false;
  bool fg_swapchain_color_format_supported_ = false;
  bool fg_swapchain_resources_ready_ = false;
  // The Streamline runtime owns the current swapchain mode.  The backend
  // stores only the target for the next atomic destroy -> feature transition
  // -> create transaction and a one-shot Native recovery gate.
  SwapchainOwnership swapchain_recreate_target_ =
      SwapchainOwnership::Native;
  bool fg_force_native_recovery_ = false;
  std::string fg_recovery_reason_;
  Clock::time_point next_swapchain_recreate_attempt_{};
  bool fatal_error_ = false;
  std::string fatal_error_detail_;
  bool surface_maintenance1_khr_enabled_ = false;
  bool surface_maintenance1_ext_enabled_ = false;
  bool swapchain_maintenance1_enabled_ = false;
  std::string swapchain_maintenance1_extension_;
  bool diagnostics_enabled_ = false;
  bool perf_diagnostics_enabled_ = false;
  bool validation_requested_ = false;
  bool validation_enabled_ = false;
  bool storage_image_extended_formats_enabled_ = false;
  bool memory_budget_supported_ = false;
  bool descriptor_binding_partially_bound_enabled_ = false;
  bool diagnostic_trace_frame_ = false;
  FrameDiagnosticContext diagnostic_context_{};

  VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
  VkPipelineLayout ui_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;
  VkPipeline ui_pipeline_ = VK_NULL_HANDLE;
  VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
  VkPipeline mesh_pipeline_trans_ = VK_NULL_HANDLE;
  VkPipeline mesh_pipeline_lines_ = VK_NULL_HANDLE;
  VkPipeline mesh_pipeline_overlay_lines_ = VK_NULL_HANDLE;
  VkPipeline mesh_pipeline_temporal_hud_lines_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout static_desc_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool static_desc_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout static_mesh_layout_ = VK_NULL_HANDLE;
  VkPipeline static_mesh_pipeline_ = VK_NULL_HANDLE;
  VkPipeline static_mesh_pipeline_blend_ = VK_NULL_HANDLE;
  // Pixel-art atlas channels all stay nearest-filtered so base color, normal,
  // and LabPBR parameter texels remain aligned. The sidecars are still linear
  // UNORM data; color-space interpretation is independent of filtering.
  VkSampler static_albedo_sampler_ = VK_NULL_HANDLE;
  VkSampler static_normal_sampler_ = VK_NULL_HANDLE;
  VkSampler static_specular_sampler_ = VK_NULL_HANDLE;

  // Preview-scene skybox cubemap (Vulkan raster path).
  VkDescriptorSetLayout skybox_desc_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool skybox_desc_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet skybox_desc_set_ = VK_NULL_HANDLE;
  VkPipelineLayout skybox_layout_ = VK_NULL_HANDLE;
  VkPipeline skybox_pipeline_ = VK_NULL_HANDLE;
  VkSampler skybox_sampler_ = VK_NULL_HANDLE;
  Buffer skybox_vbo_{};
  ImageResource skybox_cubemap_{};
  std::uint32_t skybox_face_size_ = 0;
  std::uint64_t skybox_generation_ = 0;
  bool skybox_ready_ = false;

  // Shared linear-float User HDRI + alias/PDF table for both RT frame slots.
  VkSampler world_environment_sampler_ = VK_NULL_HANDLE;
  ImageResource world_environment_texture_{};
  Buffer world_environment_distribution_{};
  VkDeviceSize world_environment_distribution_bytes_ = 0;
  std::uint64_t world_environment_resource_key_ = 0;
  std::uint64_t world_environment_failed_key_ = 0;
  bool world_environment_ready_ = false;

  // Shared, lazy Bruneton procedural-atmosphere precomputation.
  VkDescriptorSetLayout atmosphere_desc_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool atmosphere_desc_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet atmosphere_desc_set_ = VK_NULL_HANDLE;
  VkPipelineLayout atmosphere_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline atmosphere_transmittance_pipeline_ = VK_NULL_HANDLE;
  VkPipeline atmosphere_direct_irradiance_pipeline_ = VK_NULL_HANDLE;
  VkPipeline atmosphere_single_scattering_pipeline_ = VK_NULL_HANDLE;
  VkPipeline atmosphere_scattering_density_pipeline_ = VK_NULL_HANDLE;
  VkPipeline atmosphere_indirect_irradiance_pipeline_ = VK_NULL_HANDLE;
  VkPipeline atmosphere_multiple_scattering_pipeline_ = VK_NULL_HANDLE;
  VkPipeline atmosphere_environment_cache_pipeline_ = VK_NULL_HANDLE;
  VkSampler atmosphere_sampler_ = VK_NULL_HANDLE;
  ImageResource atmosphere_transmittance_{};
  ImageResource atmosphere_scattering_{};
  ImageResource atmosphere_irradiance_{};
  ImageResource atmosphere_environment_cache_{};
  ImageResource atmosphere_cloud_history_{};
  ImageResource atmosphere_environment_spare_cache_{};
  ImageResource atmosphere_cloud_history_spare_{};
  Buffer atmosphere_environment_readback_{};
  Buffer atmosphere_environment_distribution_{};
  Buffer atmosphere_environment_distribution_spare_{};
  DynamicSkyPending atmosphere_environment_pending_{};
  VkFence atmosphere_environment_spare_retirement_fence_ = VK_NULL_HANDLE;
  VkDeviceSize atmosphere_environment_distribution_bytes_ = 0;
  std::string atmosphere_resource_key_;
  std::string atmosphere_failed_key_;
  std::string atmosphere_environment_key_;
  std::string atmosphere_environment_failed_key_;
  std::string atmosphere_cloud_history_compatibility_key_;
  std::array<float, 2> atmosphere_cloud_history_weather_offset_{};
  std::uint32_t atmosphere_cloud_history_frame_ = 0u;
  Clock::time_point atmosphere_environment_last_update_{};
  std::uint64_t atmosphere_environment_cache_reallocations_ = 0u;
  bool atmosphere_ready_ = false;
  bool atmosphere_environment_ready_ = false;

  bool vsync_ = true;

  // NVIDIA RT (Vulkan ray tracing extensions) capability + active path.
  RayTracingCapability rt_capability_{};
  RenderPath active_render_path_ = RenderPath::Raster;
  bool rt_fallback_logged_ = false;
  bool unified_rt_logged_ = false;
  std::array<VulkanRtScene, 2> rt_scenes_{};
  std::array<VulkanPathTracer, 2> path_tracers_{};
  VulkanPathTracer still_path_tracer_{};
  StreamlineVulkanRuntime streamline_vulkan_runtime_{};
  bool streamline_dlss_failure_logged_ = false;
  bool streamline_rr_active_logged_ = false;
  bool streamline_rr_target_format_failure_logged_ = false;
  mutable std::string post_process_status_cache_{};
  std::vector<std::string> enabled_instance_extensions_;
  std::vector<std::string> enabled_device_extensions_;
  std::uint64_t still_active_job_id_ = 0;
  std::uint32_t still_path_trace_frame_index_ = 0;
  std::uint64_t still_waiting_job_id_ = 0;
  Clock::time_point still_wait_started_{};
  std::uint64_t still_progress_job_id_ = 0;
  std::uint32_t still_last_logged_samples_ = 0;
  Clock::time_point still_last_progress_time_{};
  bool rt_pipelines_ready_ = false;
  std::array<bool, 2> rt_scene_built_{};
  bool presentation_suspended_ = false;
  std::uint32_t path_trace_frame_index_ = 0;
  std::array<std::uint64_t, 2> last_rt_scene_hash_{};
  std::uint64_t rt_fallback_generation_serial_ = 0;
  std::array<VkDescriptorSet, 2> last_mesh_rt_descriptor_sets_{};
  std::array<VkDescriptorSet, 2> last_static_rt_descriptor_sets_{};
  std::array<VkAccelerationStructureKHR, 2> last_mesh_rt_tlas_{};
  std::array<VkAccelerationStructureKHR, 2> last_static_rt_tlas_{};
  std::uint64_t rt_descriptor_write_calls_frame_ = 0;
  std::uint64_t rt_descriptor_cache_hits_frame_ = 0;
  std::uint64_t rt_descriptor_entries_written_frame_ = 0;
  // One rendered-frame CPU snapshot shared by both in-flight scene slots.
  // Per-slot previous state would otherwise describe a two-frame delta.
  std::vector<float> rt_motion_previous_positions_;
  std::vector<float> rt_motion_previous_bones_;
  std::uint64_t rt_motion_topology_hash_ = 0u;
  bool rt_motion_history_valid_ = false;
  std::array<float, 16> rt_motion_previous_view_{};
  std::array<float, 16> rt_motion_previous_proj_{};
  bool rt_motion_camera_history_valid_ = false;
  std::uint64_t streamline_temporal_history_key_ = 0u;
  bool streamline_temporal_history_valid_ = false;
  std::uint64_t diagnostic_rt_as_events_logged_ = 0;
  std::uint64_t diagnostic_pt_history_resets_logged_ = 0;

  // Optional RT descriptor layouts / pipelines (ray-query hybrid shadows).
  VkDescriptorSetLayout mesh_rt_desc_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool mesh_rt_desc_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 2> mesh_rt_desc_sets_{};
  VkPipelineLayout mesh_rt_layout_ = VK_NULL_HANDLE;
  VkPipeline mesh_rt_pipeline_ = VK_NULL_HANDLE;
  VkPipeline mesh_rt_pipeline_trans_ = VK_NULL_HANDLE;

  VkDescriptorSetLayout static_rt_desc_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool static_rt_desc_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 2> static_rt_descriptor_sets_{};
  VkPipelineLayout static_rt_layout_ = VK_NULL_HANDLE;
  VkPipeline static_rt_pipeline_ = VK_NULL_HANDLE;
  VkPipeline static_rt_pipeline_blend_ = VK_NULL_HANDLE;

  Buffer uniform_buf_{};
  VkImage font_image_ = VK_NULL_HANDLE;
  VkDeviceMemory font_mem_ = VK_NULL_HANDLE;
  VkImageView font_view_ = VK_NULL_HANDLE;
  VkSampler font_sampler_ = VK_NULL_HANDLE;
  int font_w_ = 0, font_h_ = 0;
  bool font_ready_ = false;

  Buffer static_model_vbo_{};
  Buffer static_model_ibo_{};
  ImageResource static_texture_{};
  ImageResource static_normal_texture_{};
  ImageResource static_specular_texture_{};
  StaticModelDrawPlan static_draw_plan_{};
  StaticModelGenerationCache static_generations_{};
  std::size_t static_bone_count_ = 0;
  std::uint64_t static_resource_rebuilds_ = 0;
  std::uint64_t static_vertex_bytes_ = 0;
  std::uint64_t static_index_bytes_ = 0;
  bool static_model_ready_ = false;
  bool static_mismatch_logged_ = false;

  std::string device_name_ = "Vulkan";
  FrameStats stats_{};
  std::uint64_t total_buffer_reallocations_ = 0;
  std::uint32_t timestamp_valid_bits_ = 0;
  double timestamp_period_ns_ = 0.0;
  bool timestamp_queries_enabled_ = false;
  bool timestamp_read_error_logged_ = false;

  void logDiagnosticApi(const char *api, const char *edge,
                        std::optional<VkResult> result, double elapsed_ms,
                        std::uint32_t image_index, VkFence frame_fence,
                        VkFence image_fence, VkCommandBuffer command,
                        bool force = false, bool flush_after = false) const {
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

  void logDiagnosticFrame(const FrameInput &frame,
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

  void logDiagnosticPerf(const FrameSync &sync) const {
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

  void logDiagnosticResources(const FrameInput &frame,
                              const FrameSync &sync,
                              VkDeviceSize requested_bone_bytes,
                              VkDeviceSize requested_mesh_bytes,
                              VkDeviceSize requested_ui_vertex_bytes,
                              VkDeviceSize requested_ui_index_bytes,
                              bool force = false) const {
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

  static void mulMat(const float *a, const float *b, float *o) {
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                       a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
  }


  static void glMvpToVulkan(const float *m, float *o) {
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

  void destroySkyboxGpu() {
    destroyImage(skybox_cubemap_);
    destroyBuffer(skybox_vbo_);
    if (skybox_pipeline_) {
      vkDestroyPipeline(device_, skybox_pipeline_, nullptr);
      skybox_pipeline_ = VK_NULL_HANDLE;
    }
    if (skybox_layout_) {
      vkDestroyPipelineLayout(device_, skybox_layout_, nullptr);
      skybox_layout_ = VK_NULL_HANDLE;
    }
    if (skybox_desc_pool_) {
      vkDestroyDescriptorPool(device_, skybox_desc_pool_, nullptr);
      skybox_desc_pool_ = VK_NULL_HANDLE;
      skybox_desc_set_ = VK_NULL_HANDLE;
    }
    if (skybox_desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, skybox_desc_layout_, nullptr);
      skybox_desc_layout_ = VK_NULL_HANDLE;
    }
    if (skybox_sampler_) {
      vkDestroySampler(device_, skybox_sampler_, nullptr);
      skybox_sampler_ = VK_NULL_HANDLE;
    }
    skybox_face_size_ = 0;
    skybox_generation_ = 0;
    skybox_ready_ = false;
  }

  void destroyGraphicsPipelines() {
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

  [[nodiscard]] bool graphicsPipelinesReady() const {
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
  findMemoryType(uint32_t bits, VkMemoryPropertyFlags props) const {
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

  struct MemoryHeapDiagnostic {
    std::uint32_t memory_type = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t heap = (std::numeric_limits<std::uint32_t>::max)();
    VkDeviceSize budget = 0;
    VkDeviceSize usage = 0;
  };

  [[nodiscard]] MemoryHeapDiagnostic
  memoryHeapDiagnostic(std::uint32_t memory_type) const {
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

  void logBufferResourceError(const char *api, VkResult result,
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

  void logImageResourceError(const char *api, VkResult result,
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

  void destroyBuffer(Buffer &b) {
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

  bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, Buffer &out,
                    const char *resource_name = "buffer") {
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

  bool ensureBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage,
                    bool *reallocated = nullptr) {
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

  bool uploadBuffer(Buffer &b, VkDeviceSize offset, VkDeviceSize bytes,
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

  void destroyImage(ImageResource &image) {
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

  [[nodiscard]] static DynamicSkyCpuResult buildDynamicSkyDistribution(
      const DynamicSkyCpuInput &input, VkDevice device, VkFence fence,
      Clock::time_point submitted_at) {
    DynamicSkyCpuResult result;
    result.fence_result =
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    result.cache_compute_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - submitted_at)
            .count();
    if (result.fence_result != VK_SUCCESS) {
      result.error = "dynamic-sky GPU fence wait failed";
      return result;
    }
    if (input.readback == nullptr || input.distribution_mapped == nullptr ||
        input.width == 0u || input.height == 0u) {
      result.error = "dynamic-sky readback/output mapping is unavailable";
      return result;
    }

    try {
      const auto readback_begin = Clock::now();
      const std::uint64_t pixel_count =
          static_cast<std::uint64_t>(input.width) * input.height;
      const VkDeviceSize distribution_bytes =
          sizeof(WorldEnvironmentGpuHeader) +
          static_cast<VkDeviceSize>(pixel_count) *
              sizeof(WorldEnvironmentGpuAlias);
      if (distribution_bytes > input.distribution_capacity) {
        result.error = "dynamic-sky distribution buffer is undersized";
        return result;
      }

      FloatEnvironmentImage radiance;
      radiance.width = input.width;
      radiance.height = input.height;
      radiance.rgba.resize(static_cast<std::size_t>(pixel_count) * 4u);
      std::uint64_t positive_rgb = 0u;
      float brightest_luminance = 0.0f;
      std::uint32_t brightest_x = 0u;
      std::uint32_t brightest_y = 0u;
      double moon_integrated_luminance = 0.0;
      bool valid_radiance = true;
      constexpr double kPi = 3.14159265358979323846;
      constexpr double kTwoPi = 2.0 * kPi;
      for (std::uint64_t pixel = 0;
           valid_radiance && pixel < pixel_count; ++pixel) {
        for (std::uint32_t channel = 0; channel < 3u; ++channel) {
          const float value =
              halfToFloat(input.readback[pixel * 4u + channel]);
          if (!std::isfinite(value) || value < 0.0f) {
            valid_radiance = false;
            break;
          }
          radiance.rgba[pixel * 4u + channel] = value;
          positive_rgb += value > 0.0f ? 1u : 0u;
        }
        const float moon_luminance =
            halfToFloat(input.readback[pixel * 4u + 3u]);
        radiance.rgba[pixel * 4u + 3u] = moon_luminance;
        valid_radiance = valid_radiance && std::isfinite(moon_luminance) &&
                         moon_luminance >= 0.0f;
        if (!valid_radiance) {
          break;
        }
        const float luminance =
            0.2126f * radiance.rgba[pixel * 4u] +
            0.7152f * radiance.rgba[pixel * 4u + 1u] +
            0.0722f * radiance.rgba[pixel * 4u + 2u];
        const std::uint32_t x =
            static_cast<std::uint32_t>(pixel % input.width);
        const std::uint32_t y =
            static_cast<std::uint32_t>(pixel / input.width);
        const double theta0 =
            kPi * static_cast<double>(y) / static_cast<double>(input.height);
        const double theta1 = kPi * static_cast<double>(y + 1u) /
                              static_cast<double>(input.height);
        const double solid_angle =
            (kTwoPi / static_cast<double>(input.width)) *
            (std::cos(theta0) - std::cos(theta1));
        moon_integrated_luminance +=
            static_cast<double>(moon_luminance) * solid_angle;
        if (luminance > brightest_luminance) {
          brightest_luminance = luminance;
          brightest_x = x;
          brightest_y = y;
        }
      }
      if (!valid_radiance || positive_rgb == 0u) {
        result.error = "dynamic-sky readback contains invalid radiance";
        return result;
      }

      const double sun_angular_radius =
          std::clamp(static_cast<double>(
                         input.sun.angular_diameter_degrees),
                     0.05, 5.0) *
          0.5 * (kPi / 180.0);
      const double sun_cos_radius = std::cos(sun_angular_radius);
      const double cos_rotation = std::cos(input.rotation_radians);
      const double sin_rotation = std::sin(input.rotation_radians);
      const std::array<double, 3> sun_cache_direction = {
          input.celestial.sun.direction[0] * cos_rotation -
              input.celestial.sun.direction[2] * sin_rotation,
          input.celestial.sun.direction[1],
          input.celestial.sun.direction[2] * cos_rotation +
              input.celestial.sun.direction[0] * sin_rotation};
      const std::array<float, 3> sun_color =
          colorTemperatureRgb(input.sun.color_temperature_kelvin);
      const float sun_importance_strength =
          input.sun_moon_lighting && input.sun.enabled
              ? std::clamp(input.sun.strength, 0.0f, 32.0f)
              : 0.0f;
      const std::array<float, 3> analytic_sun_radiance = {
          700.0f * sun_color[0] * sun_importance_strength,
          700.0f * sun_color[1] * sun_importance_strength,
          700.0f * sun_color[2] * sun_importance_strength};
      for (std::uint64_t pixel = 0; pixel < pixel_count; ++pixel) {
        const std::uint32_t x =
            static_cast<std::uint32_t>(pixel % input.width);
        const std::uint32_t y =
            static_cast<std::uint32_t>(pixel / input.width);
        const double theta =
            kPi * (static_cast<double>(y) + 0.5) / input.height;
        const double phi =
            kTwoPi * (static_cast<double>(x) + 0.5) / input.width;
        const double sin_theta = std::sin(theta);
        const std::array<double, 3> direction = {
            sin_theta * std::sin(phi), std::cos(theta),
            sin_theta * std::cos(phi)};
        const double cosine = direction[0] * sun_cache_direction[0] +
                              direction[1] * sun_cache_direction[1] +
                              direction[2] * sun_cache_direction[2];
        double sun_coverage = 0.0;
        if (cosine >= sun_cos_radius) {
          const double angular_distance =
              std::acos(std::clamp(cosine, -1.0, 1.0));
          const double radial = angular_distance / sun_angular_radius;
          const double edge =
              std::clamp((radial - 0.94) / 0.06, 0.0, 1.0);
          sun_coverage = 1.0 - edge * edge * (3.0 - 2.0 * edge);
        }
        const float moon_luminance = radiance.rgba[pixel * 4u + 3u];
        const float lighting_moon_luminance =
            input.sun_moon_lighting && input.moon.enabled
                ? moon_luminance
                : 0.0f;
        for (std::uint32_t channel = 0; channel < 3u; ++channel) {
          radiance.rgba[pixel * 4u + channel] +=
              lighting_moon_luminance +
              static_cast<float>(sun_coverage) *
                  analytic_sun_radiance[channel];
        }
        radiance.rgba[pixel * 4u + 3u] = 1.0f;
      }
      result.readback_ms =
          std::chrono::duration<double, std::milli>(Clock::now() -
                                                    readback_begin)
              .count();

      EnvironmentDistribution distribution;
      const auto distribution_begin = Clock::now();
      if (!distribution.build(radiance)) {
        result.error = "dynamic-sky importance distribution build failed";
        return result;
      }
      result.distribution_build_ms =
          std::chrono::duration<double, std::milli>(Clock::now() -
                                                    distribution_begin)
              .count();

      const double moon_u_unwrapped =
          std::atan2(input.celestial.moon.direction[0],
                     input.celestial.moon.direction[2]) /
          kTwoPi;
      const double moon_u =
          moon_u_unwrapped < 0.0 ? moon_u_unwrapped + 1.0
                                 : moon_u_unwrapped;
      const double moon_v =
          std::acos(std::clamp(input.celestial.moon.direction[1], -1.0,
                               1.0)) /
          kPi;
      const std::int32_t moon_center_x = static_cast<std::int32_t>(
          std::clamp(moon_u * input.width, 0.0,
                     static_cast<double>(input.width - 1u)));
      const std::int32_t moon_center_y = static_cast<std::int32_t>(
          std::clamp(moon_v * input.height, 0.0,
                     static_cast<double>(input.height - 1u)));
      const double moon_angular_radius =
          std::clamp(static_cast<double>(
                         input.moon.angular_diameter_degrees),
                     0.05, 5.0) *
          0.5 * (kPi / 180.0);
      const double moon_solid_angle =
          kTwoPi * (1.0 - std::cos(moon_angular_radius));
      const float moon_mean_luminance =
          moon_solid_angle > 0.0
              ? static_cast<float>(moon_integrated_luminance /
                                   moon_solid_angle)
              : 0.0f;
      double moon_probability = 0.0;
      float moon_peak_luminance = 0.0f;
      for (std::int32_t offset_y = -1; offset_y <= 1; ++offset_y) {
        const std::uint32_t y = static_cast<std::uint32_t>(std::clamp(
            moon_center_y + offset_y, 0,
            static_cast<std::int32_t>(input.height - 1u)));
        for (std::int32_t offset_x = -1; offset_x <= 1; ++offset_x) {
          const std::int32_t wrapped_x =
              (moon_center_x + offset_x +
               static_cast<std::int32_t>(input.width)) %
              static_cast<std::int32_t>(input.width);
          const std::uint32_t x = static_cast<std::uint32_t>(wrapped_x);
          moon_probability += distribution.texelProbability(x, y);
          moon_peak_luminance = (std::max)(
              moon_peak_luminance,
              halfToFloat(input.readback[
                  (static_cast<std::size_t>(y) * input.width + x) * 4u +
                  3u]));
        }
      }

      constexpr std::uint32_t kValidEnvironment = 1u << 0u;
      constexpr std::uint32_t kBackgroundVisible = 1u << 1u;
      constexpr std::uint32_t kLightingEnabled = 1u << 2u;
      constexpr std::uint32_t kProceduralFiniteMoon = 1u << 3u;
      constexpr std::uint32_t kSunBackgroundVisible = 1u << 4u;
      constexpr std::uint32_t kMoonBackgroundVisible = 1u << 5u;
      constexpr std::uint32_t kSunLightingEnabled = 1u << 6u;
      constexpr std::uint32_t kMoonLightingEnabled = 1u << 7u;
      constexpr std::uint32_t kSunCastsShadows = 1u << 8u;
      constexpr std::uint32_t kMoonCastsShadows = 1u << 9u;
      constexpr std::uint32_t kMoonSurfaceDetail = 1u << 10u;
      constexpr std::uint32_t kMoonManualPhase = 1u << 11u;
      WorldEnvironmentGpuHeader header;
      header.flags =
          kValidEnvironment |
          (input.background_visible ? kBackgroundVisible : 0u) |
          (input.environment_lighting ? kLightingEnabled : 0u) |
          (input.moon.enabled ? kProceduralFiniteMoon : 0u) |
          (input.sun.enabled && input.sun.disk_visible
               ? kSunBackgroundVisible
               : 0u) |
          (input.moon.enabled && input.moon.disk_visible
               ? kMoonBackgroundVisible
               : 0u) |
          (input.sun_moon_lighting && input.sun.enabled
               ? kSunLightingEnabled
               : 0u) |
          (input.sun_moon_lighting && input.moon.enabled
               ? kMoonLightingEnabled
               : 0u) |
          (input.sun.cast_shadows ? kSunCastsShadows : 0u) |
          (input.moon.cast_shadows ? kMoonCastsShadows : 0u) |
          (input.moon.surface_detail > 0.0f ? kMoonSurfaceDetail : 0u) |
          (input.moon.phase_mode == MoonPhaseMode::Manual
               ? kMoonManualPhase
               : 0u);
      header.width = input.width;
      header.height = input.height;
      header.entry_count = static_cast<std::uint32_t>(pixel_count);
      header.lighting_strength = input.environment_strength;
      header.background_multiplier = input.background_multiplier;
      header.rotation_radians = input.rotation_radians;
      header.padding = static_cast<float>(sun_angular_radius);
      header.sun_direction_moon_mean = {
          static_cast<float>(input.celestial.sun.direction[0]),
          static_cast<float>(input.celestial.sun.direction[1]),
          static_cast<float>(input.celestial.sun.direction[2]),
          moon_mean_luminance};
      header.moon_direction_angular_radius = {
          static_cast<float>(input.celestial.moon.direction[0]),
          static_cast<float>(input.celestial.moon.direction[1]),
          static_cast<float>(input.celestial.moon.direction[2]),
          static_cast<float>(moon_angular_radius)};
      header.moon_phase_libration = {
          input.moon_fraction, input.moon_phase_radians,
          static_cast<float>(
              input.celestial.moon_libration_latitude_degrees *
              (kPi / 180.0)),
          static_cast<float>(
              input.celestial.moon_libration_longitude_degrees *
              (kPi / 180.0))};
      header.sun_color_strength = {
          sun_color[0], sun_color[1], sun_color[2],
          std::clamp(input.sun.strength, 0.0f, 32.0f)};
      std::memcpy(input.distribution_mapped, &header, sizeof(header));
      auto *gpu_alias = reinterpret_cast<WorldEnvironmentGpuAlias *>(
          static_cast<std::byte *>(input.distribution_mapped) +
          sizeof(WorldEnvironmentGpuHeader));
      for (std::uint32_t y = 0; y < input.height; ++y) {
        for (std::uint32_t x = 0; x < input.width; ++x) {
          const std::uint32_t index = y * input.width + x;
          gpu_alias[index].acceptance = static_cast<float>(
              std::clamp(distribution.aliasAcceptance(x, y), 0.0, 1.0));
          gpu_alias[index].alias_index = distribution.aliasIndex(x, y);
          gpu_alias[index].probability = static_cast<float>(
              (std::max)(distribution.texelProbability(x, y), 0.0));
        }
      }
      result.success = true;
      result.positive_rgb = positive_rgb;
      result.brightest_luminance = brightest_luminance;
      result.brightest_x = brightest_x;
      result.brightest_y = brightest_y;
      result.moon_probability = moon_probability;
      result.moon_peak_luminance = moon_peak_luminance;
    } catch (const std::exception &exception) {
      result.error = std::string("dynamic-sky CPU build exception: ") +
                     exception.what();
    } catch (...) {
      result.error = "dynamic-sky CPU build failed with unknown exception";
    }
    return result;
  }

  bool pollDynamicSkyEnvironmentCache(bool wait_for_completion = false) {
    DynamicSkyPending &pending = atmosphere_environment_pending_;
    if (!pending.active()) {
      return false;
    }
    if (!pending.completion.valid()) {
      if (!wait_for_completion) {
        return false;
      }
      DynamicSkyCpuResult fallback;
      fallback.fence_result =
          vkWaitForFences(device_, 1, &pending.fence, VK_TRUE, UINT64_MAX);
      fallback.error = "dynamic-sky worker launch failed";
      std::promise<DynamicSkyCpuResult> promise;
      pending.completion = promise.get_future();
      promise.set_value(std::move(fallback));
    } else if (!wait_for_completion &&
               pending.completion.wait_for(std::chrono::milliseconds(0)) !=
                   std::future_status::ready) {
      return false;
    }

    DynamicSkyCpuResult cpu_result;
    try {
      cpu_result = pending.completion.get();
    } catch (const std::exception &exception) {
      // A broken future does not prove that the submitted command buffer has
      // completed. Establish completion independently before reclaiming any
      // Vulkan object owned by the pending transaction.
      cpu_result.fence_result =
          vkWaitForFences(device_, 1, &pending.fence, VK_TRUE, UINT64_MAX);
      cpu_result.error = std::string("dynamic-sky worker completion failed: ") +
                         exception.what();
    } catch (...) {
      cpu_result.fence_result =
          vkWaitForFences(device_, 1, &pending.fence, VK_TRUE, UINT64_MAX);
      cpu_result.error =
          "dynamic-sky worker completion failed with unknown exception";
    }
    if (cpu_result.fence_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Dynamic sky completion could not be established: "
          "API=vkWaitForFences VkResult=%s(%d) frame_slot=%u "
          "still_job_id=%llu; quarantining submitted resources until device "
          "teardown",
          vkResultName(cpu_result.fence_result),
          static_cast<int>(cpu_result.fence_result), frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      recordFatalVulkanError("vkWaitForFences(dynamic_sky)",
                             cpu_result.fence_result);
      // Do not destroy the fence, free the command buffer, or release any
      // image/buffer here: a non-successful wait does not prove completion.
      return true;
    }
    if (pending.fence != VK_NULL_HANDLE) {
      vkDestroyFence(device_, pending.fence, nullptr);
      pending.fence = VK_NULL_HANDLE;
    }
    if (pending.command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &pending.command);
      pending.command = VK_NULL_HANDLE;
    }
    if (!cpu_result.success) {
      xpbd::log::warnf(
          "Dynamic sky asynchronous update failed: VkResult=%s(%d) %s; "
          "retaining previous radiance/PDF pair",
          vkResultName(cpu_result.fence_result),
          static_cast<int>(cpu_result.fence_result),
          cpu_result.error.c_str());
      atmosphere_environment_failed_key_ = pending.environment_key;
      destroyImage(atmosphere_environment_spare_cache_);
      atmosphere_environment_spare_cache_ = pending.cache;
      pending.cache = {};
      destroyImage(atmosphere_cloud_history_spare_);
      atmosphere_cloud_history_spare_ = pending.cloud_history;
      pending.cloud_history = {};
      destroyBuffer(atmosphere_environment_distribution_spare_);
      atmosphere_environment_distribution_spare_ = pending.distribution;
      pending.distribution = {};
      destroyBuffer(atmosphere_environment_readback_);
      atmosphere_environment_readback_ = pending.readback;
      pending.readback = {};
      destroyImage(pending.cache);
      destroyImage(pending.cloud_history);
      destroyBuffer(pending.distribution);
      destroyBuffer(pending.readback);
      atmosphere_environment_pending_ = {};
      atmosphere_environment_last_update_ = Clock::now();
      return true;
    }

    const bool retiring_shared_front =
        atmosphere_environment_cache_.image != VK_NULL_HANDLE ||
        atmosphere_environment_distribution_.buffer != VK_NULL_HANDLE;
    if (retiring_shared_front) {
      const std::size_t other_slot =
          (frame_index_ + 1u) % frames_.size();
      atmosphere_environment_spare_retirement_fence_ =
          frames_[other_slot].fence;
    } else {
      atmosphere_environment_spare_retirement_fence_ = VK_NULL_HANDLE;
    }
    destroyImage(atmosphere_environment_spare_cache_);
    atmosphere_environment_spare_cache_ = atmosphere_environment_cache_;
    atmosphere_environment_cache_ = pending.cache;
    pending.cache = {};
    destroyImage(atmosphere_cloud_history_spare_);
    atmosphere_cloud_history_spare_ = atmosphere_cloud_history_;
    atmosphere_cloud_history_ = pending.cloud_history;
    pending.cloud_history = {};
    destroyBuffer(atmosphere_environment_distribution_spare_);
    atmosphere_environment_distribution_spare_ =
        atmosphere_environment_distribution_;
    atmosphere_environment_distribution_ = pending.distribution;
    pending.distribution = {};
    destroyBuffer(atmosphere_environment_readback_);
    atmosphere_environment_readback_ = pending.readback;
    pending.readback = {};
    atmosphere_environment_distribution_bytes_ = pending.distribution_bytes;
    atmosphere_environment_key_ = pending.environment_key;
    atmosphere_cloud_history_compatibility_key_ =
        pending.cloud_compatibility_key;
    atmosphere_cloud_history_weather_offset_ = pending.weather_offset;
    atmosphere_cloud_history_frame_ = pending.cloud_frame;
    atmosphere_environment_failed_key_.clear();
    atmosphere_environment_ready_ = true;
    atmosphere_environment_last_update_ = Clock::now();
    if (pending.cloud_enabled) {
      xpbd::log::infof(
          "Dynamic cloud temporal cache: %ux%u history=%s frame=%u "
          "previous_frame=%u weather_delta=%.6f/%.6f weight=%.3f "
          "shadow_resolution=%u",
          atmosphere_environment_cache_.width,
          atmosphere_environment_cache_.height,
          pending.cloud_history_valid ? "reprojected" : "reset",
          pending.cloud_frame, pending.previous_cloud_frame,
          pending.cloud_history_parameters[0],
          pending.cloud_history_parameters[1],
          pending.cloud_history_weight, pending.cloud_shadow_resolution);
    }
    xpbd::log::infof(
        "Dynamic sky asynchronous cache ready: %ux%u positive=%llu "
        "table=%llu brightest=%.7g@%u,%u moon_pmf=%.9g "
        "moon_peak=%.7g",
        atmosphere_environment_cache_.width,
        atmosphere_environment_cache_.height,
        static_cast<unsigned long long>(cpu_result.positive_rgb),
        static_cast<unsigned long long>(pending.distribution_bytes),
        cpu_result.brightest_luminance, cpu_result.brightest_x,
        cpu_result.brightest_y, cpu_result.moon_probability,
        cpu_result.moon_peak_luminance);
    xpbd::log::infof(
        "Dynamic sky update perf: cache_compute_ms=%.3f readback_ms=%.3f "
        "distribution_build_ms=%.3f queue_idle_count=0 "
        "cache_realloc_count=%llu",
        cpu_result.cache_compute_ms, cpu_result.readback_ms,
        cpu_result.distribution_build_ms,
        static_cast<unsigned long long>(
            atmosphere_environment_cache_reallocations_));
    atmosphere_environment_pending_ = {};
    return true;
  }

  void discardDynamicSkyPending() {
    if (!atmosphere_environment_pending_.active()) {
      atmosphere_environment_pending_ = {};
      return;
    }
    (void)pollDynamicSkyEnvironmentCache(true);
    if (!atmosphere_environment_pending_.active()) {
      return;
    }
    // poll(true) only leaves the transaction active when its fence completion
    // could not be established. Intentionally lose the child handles here;
    // vkDestroyDevice owns the final reclamation, while freeing them directly
    // could race work that is still in flight.
    xpbd::log::error(
        "Dynamic sky pending resources quarantined for device teardown");
    atmosphere_environment_pending_ = {};
  }

  void clearDynamicSkyEnvironmentCache() {
    discardDynamicSkyPending();
    destroyImage(atmosphere_environment_cache_);
    destroyImage(atmosphere_cloud_history_);
    destroyImage(atmosphere_environment_spare_cache_);
    destroyImage(atmosphere_cloud_history_spare_);
    destroyBuffer(atmosphere_environment_readback_);
    destroyBuffer(atmosphere_environment_distribution_);
    destroyBuffer(atmosphere_environment_distribution_spare_);
    atmosphere_environment_distribution_bytes_ = 0;
    atmosphere_environment_key_.clear();
    atmosphere_cloud_history_compatibility_key_.clear();
    atmosphere_cloud_history_weather_offset_ = {};
    atmosphere_cloud_history_frame_ = 0u;
    atmosphere_environment_last_update_ = {};
    atmosphere_environment_spare_retirement_fence_ = VK_NULL_HANDLE;
    atmosphere_environment_ready_ = false;
  }

  void clearProceduralAtmosphereImage() {
    clearDynamicSkyEnvironmentCache();
    destroyImage(atmosphere_transmittance_);
    destroyImage(atmosphere_scattering_);
    destroyImage(atmosphere_irradiance_);
    atmosphere_resource_key_.clear();
    atmosphere_ready_ = false;
  }

  void destroyProceduralAtmosphereGpu() {
    clearProceduralAtmosphereImage();
    if (atmosphere_environment_cache_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_environment_cache_pipeline_,
                        nullptr);
      atmosphere_environment_cache_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_multiple_scattering_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_multiple_scattering_pipeline_,
                        nullptr);
      atmosphere_multiple_scattering_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_indirect_irradiance_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_indirect_irradiance_pipeline_,
                        nullptr);
      atmosphere_indirect_irradiance_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_scattering_density_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_scattering_density_pipeline_,
                        nullptr);
      atmosphere_scattering_density_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_single_scattering_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_single_scattering_pipeline_,
                        nullptr);
      atmosphere_single_scattering_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_direct_irradiance_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_direct_irradiance_pipeline_,
                        nullptr);
      atmosphere_direct_irradiance_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_transmittance_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_transmittance_pipeline_, nullptr);
      atmosphere_transmittance_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, atmosphere_pipeline_layout_, nullptr);
      atmosphere_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (atmosphere_desc_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, atmosphere_desc_pool_, nullptr);
      atmosphere_desc_pool_ = VK_NULL_HANDLE;
      atmosphere_desc_set_ = VK_NULL_HANDLE;
    }
    if (atmosphere_desc_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, atmosphere_desc_layout_, nullptr);
      atmosphere_desc_layout_ = VK_NULL_HANDLE;
    }
    if (atmosphere_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, atmosphere_sampler_, nullptr);
      atmosphere_sampler_ = VK_NULL_HANDLE;
    }
    atmosphere_failed_key_.clear();
    atmosphere_environment_failed_key_.clear();
  }

  bool ensureProceduralAtmospherePipeline() {
    if (atmosphere_transmittance_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_direct_irradiance_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_single_scattering_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_scattering_density_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_indirect_irradiance_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_multiple_scattering_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_environment_cache_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_pipeline_layout_ != VK_NULL_HANDLE &&
        atmosphere_desc_set_ != VK_NULL_HANDLE &&
        atmosphere_sampler_ != VK_NULL_HANDLE) {
      return true;
    }
    destroyProceduralAtmosphereGpu();

    constexpr std::array<VkDescriptorType, 18> descriptor_types{
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    };
    std::array<VkDescriptorSetLayoutBinding, descriptor_types.size()>
        bindings{};
    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
      bindings[index].binding = index;
      bindings[index].descriptorType = descriptor_types[index];
      bindings[index].descriptorCount = 1;
      bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor_layout_info.bindingCount =
        static_cast<std::uint32_t>(bindings.size());
    descriptor_layout_info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &descriptor_layout_info, nullptr,
                                    &atmosphere_desc_layout_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(AtmosphereEnvironmentPush);
    VkPipelineLayoutCreateInfo pipeline_layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &atmosphere_desc_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                               &atmosphere_pipeline_layout_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }

    auto create_pipeline = [&](const std::uint32_t *words,
                               std::size_t word_count,
                               VkPipeline &pipeline) {
      VkShaderModule module = makeModule(words, word_count);
      if (module == VK_NULL_HANDLE) {
        return false;
      }
      VkPipelineShaderStageCreateInfo shader_stage{
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      shader_stage.module = module;
      shader_stage.pName = "main";
      VkComputePipelineCreateInfo pipeline_info{
          VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipeline_info.stage = shader_stage;
      pipeline_info.layout = atmosphere_pipeline_layout_;
      const VkResult result =
          vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &pipeline);
      vkDestroyShaderModule(device_, module, nullptr);
      return result == VK_SUCCESS;
    };
    if (!create_pipeline(
            kSpvAtmosphereTransmittanceComp,
            sizeof(kSpvAtmosphereTransmittanceComp) / sizeof(std::uint32_t),
            atmosphere_transmittance_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereDirectIrradianceComp,
            sizeof(kSpvAtmosphereDirectIrradianceComp) /
                sizeof(std::uint32_t),
            atmosphere_direct_irradiance_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereSingleScatteringComp,
            sizeof(kSpvAtmosphereSingleScatteringComp) /
                sizeof(std::uint32_t),
            atmosphere_single_scattering_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereScatteringDensityComp,
            sizeof(kSpvAtmosphereScatteringDensityComp) /
                sizeof(std::uint32_t),
            atmosphere_scattering_density_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereIndirectIrradianceComp,
            sizeof(kSpvAtmosphereIndirectIrradianceComp) /
                sizeof(std::uint32_t),
            atmosphere_indirect_irradiance_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereMultipleScatteringComp,
            sizeof(kSpvAtmosphereMultipleScatteringComp) /
                sizeof(std::uint32_t),
            atmosphere_multiple_scattering_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereEnvironmentCacheComp,
            sizeof(kSpvAtmosphereEnvironmentCacheComp) /
                sizeof(std::uint32_t),
            atmosphere_environment_cache_pipeline_)) {
      destroyProceduralAtmosphereGpu();
      return false;
    }

    std::array<VkDescriptorPoolSize, 2> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
    }};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount =
        static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr,
                               &atmosphere_desc_pool_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }
    VkDescriptorSetAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_info.descriptorPool = atmosphere_desc_pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &atmosphere_desc_layout_;
    if (vkAllocateDescriptorSets(device_, &allocate_info,
                                 &atmosphere_desc_set_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (vkCreateSampler(device_, &sampler_info, nullptr,
                        &atmosphere_sampler_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }
    return true;
  }

  bool createAtmosphereImage(std::uint32_t width, std::uint32_t height,
                             std::uint32_t depth, ImageResource &out) {
    if (!storage_image_extended_formats_enabled_) {
      xpbd::log::warn(
          "Procedural atmosphere requires "
          "shaderStorageImageExtendedFormats");
      return false;
    }
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R16G16B16A16_SFLOAT,
                                        &format_properties);
    constexpr VkFormatFeatureFlags required_features =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    constexpr VkImageUsageFlags kUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkDeviceSize estimated_bytes =
        static_cast<VkDeviceSize>(width) * height * depth * 8u;
    if ((format_properties.optimalTilingFeatures & required_features) !=
        required_features) {
      logImageResourceError(
          "vkGetPhysicalDeviceFormatProperties",
          VK_ERROR_FORMAT_NOT_SUPPORTED, "procedural-atmosphere-rgba16f",
          VK_FORMAT_R16G16B16A16_SFLOAT, width, height, depth, kUsage,
          estimated_bytes, (std::numeric_limits<std::uint32_t>::max)());
      return false;
    }

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType =
        depth > 1u ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image_info.extent = {width, height, depth};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = kUsage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkResult create_result =
        vkCreateImage(device_, &image_info, nullptr, &out.image);
    if (create_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImage", create_result, "procedural-atmosphere-rgba16f",
          image_info.format, width, height, depth, image_info.usage,
          estimated_bytes, (std::numeric_limits<std::uint32_t>::max)());
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, out.image, &requirements);
    const auto memory_type = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      logImageResourceError(
          "findMemoryType", VK_ERROR_FEATURE_NOT_PRESENT,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size,
          (std::numeric_limits<std::uint32_t>::max)());
      destroyImage(out);
      return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *memory_type;
    const VkResult allocation_result =
        vkAllocateMemory(device_, &allocation, nullptr, &out.memory);
    if (allocation_result != VK_SUCCESS) {
      logImageResourceError(
          "vkAllocateMemory", allocation_result,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size, *memory_type);
      destroyImage(out);
      return false;
    }
    const VkResult bind_result =
        vkBindImageMemory(device_, out.image, out.memory, 0);
    if (bind_result != VK_SUCCESS) {
      logImageResourceError(
          "vkBindImageMemory", bind_result,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size, *memory_type);
      destroyImage(out);
      return false;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = out.image;
    view_info.viewType =
        depth > 1u ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkResult view_result =
        vkCreateImageView(device_, &view_info, nullptr, &out.view);
    if (view_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImageView", view_result,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size, *memory_type);
      destroyImage(out);
      return false;
    }
    out.width = width;
    out.height = height;
    out.depth = depth;
    return true;
  }

  bool buildProceduralAtmosphereLuts(
      const ResolvedWorldEnvironment &resolved,
      const std::string &resource_key) {
    if (resolved.sky_rendering != SkyRendering::ProceduralDayNight ||
        resolved.atmosphere == nullptr ||
        !resolved.atmosphere->valid() || resource_key.empty()) {
      return false;
    }
    const BrunetonAtmosphereConfig &config = *resolved.atmosphere;
    const AtmosphereLutDimensions &dimensions = config.dimensions;
    const AtmosphereLutDimensions frozen_dimensions{};
    if (config.format != AtmosphereLutFormat::Rgba16Float ||
        dimensions.transmittance_width !=
            frozen_dimensions.transmittance_width ||
        dimensions.transmittance_height !=
            frozen_dimensions.transmittance_height ||
        dimensions.scattering_radial !=
            frozen_dimensions.scattering_radial ||
        dimensions.scattering_view_cosine !=
            frozen_dimensions.scattering_view_cosine ||
        dimensions.scattering_sun_cosine !=
            frozen_dimensions.scattering_sun_cosine ||
        dimensions.scattering_relative_azimuth !=
            frozen_dimensions.scattering_relative_azimuth ||
        dimensions.irradiance_width != frozen_dimensions.irradiance_width ||
        dimensions.irradiance_height != frozen_dimensions.irradiance_height) {
      xpbd::log::warn(
          "Procedural atmosphere rejected: shader/LUT dimensions differ");
      return false;
    }
    if (!ensureProceduralAtmospherePipeline()) {
      xpbd::log::warn("Procedural atmosphere compute pipeline creation failed");
      return false;
    }
    if ((atmosphere_transmittance_.image != VK_NULL_HANDLE ||
         atmosphere_scattering_.image != VK_NULL_HANDLE ||
         atmosphere_irradiance_.image != VK_NULL_HANDLE) &&
        vkQueueWaitIdle(graphics_queue_) != VK_SUCCESS) {
      return false;
    }

    const std::uint32_t scattering_width = dimensions.scatteringWidth();
    const std::uint64_t transmittance_pixels =
        static_cast<std::uint64_t>(dimensions.transmittance_width) *
        dimensions.transmittance_height;
    const std::uint64_t irradiance_pixels =
        static_cast<std::uint64_t>(dimensions.irradiance_width) *
        dimensions.irradiance_height;
    const std::uint64_t scattering_pixels =
        static_cast<std::uint64_t>(scattering_width) *
        dimensions.scattering_view_cosine *
        dimensions.scattering_radial;
    const VkDeviceSize transmittance_bytes =
        static_cast<VkDeviceSize>(transmittance_pixels) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize irradiance_bytes =
        static_cast<VkDeviceSize>(irradiance_pixels) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize scattering_bytes =
        static_cast<VkDeviceSize>(scattering_pixels) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize transmittance_offset = 0;
    const VkDeviceSize irradiance_offset = transmittance_bytes;
    const VkDeviceSize scattering_offset =
        irradiance_offset + irradiance_bytes;
    const VkDeviceSize readback_bytes = scattering_offset + scattering_bytes;
    constexpr VkDeviceSize kMaximumAtmosphereBytes =
        VkDeviceSize{128} * 1024u * 1024u;
    if (readback_bytes == 0u || readback_bytes > kMaximumAtmosphereBytes) {
      xpbd::log::warn("Procedural atmosphere LUT budget exceeded");
      return false;
    }

    ImageResource candidate_transmittance{};
    ImageResource candidate_irradiance{};
    ImageResource candidate_scattering{};
    ImageResource delta_irradiance{};
    ImageResource delta_rayleigh{};
    ImageResource delta_mie{};
    ImageResource scattering_density{};
    ImageResource delta_multiple{};
    Buffer readback{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      destroyBuffer(readback);
      destroyImage(candidate_transmittance);
      destroyImage(candidate_irradiance);
      destroyImage(candidate_scattering);
      destroyImage(delta_irradiance);
      destroyImage(delta_rayleigh);
      destroyImage(delta_mie);
      destroyImage(scattering_density);
      destroyImage(delta_multiple);
    };
    if (!createAtmosphereImage(dimensions.transmittance_width,
                               dimensions.transmittance_height, 1u,
                               candidate_transmittance) ||
        !createAtmosphereImage(dimensions.irradiance_width,
                               dimensions.irradiance_height, 1u,
                               candidate_irradiance) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               candidate_scattering) ||
        !createAtmosphereImage(dimensions.irradiance_width,
                               dimensions.irradiance_height, 1u,
                               delta_irradiance) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               delta_rayleigh) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial, delta_mie) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               scattering_density) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               delta_multiple) ||
        !createBuffer(readback_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      readback)) {
      cleanup();
      return false;
    }

    const std::array<VkImageView, 14> descriptor_views{
        candidate_transmittance.view,
        candidate_transmittance.view,
        delta_irradiance.view,
        candidate_irradiance.view,
        delta_rayleigh.view,
        delta_mie.view,
        candidate_scattering.view,
        delta_rayleigh.view,
        delta_mie.view,
        delta_multiple.view,
        delta_irradiance.view,
        scattering_density.view,
        scattering_density.view,
        delta_multiple.view,
    };
    const auto is_sampled_binding = [](std::uint32_t binding) {
      return binding == 1u || binding == 7u || binding == 8u ||
             binding == 9u || binding == 10u || binding == 12u;
    };
    std::array<VkDescriptorImageInfo, descriptor_views.size()>
        descriptor_images{};
    std::array<VkWriteDescriptorSet, descriptor_views.size()>
        descriptor_writes{};
    for (std::uint32_t binding = 0; binding < descriptor_views.size();
         ++binding) {
      descriptor_images[binding].sampler =
          is_sampled_binding(binding) ? atmosphere_sampler_ : VK_NULL_HANDLE;
      descriptor_images[binding].imageView = descriptor_views[binding];
      descriptor_images[binding].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      descriptor_writes[binding].sType =
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptor_writes[binding].dstSet = atmosphere_desc_set_;
      descriptor_writes[binding].dstBinding = binding;
      descriptor_writes[binding].descriptorType =
          is_sampled_binding(binding)
              ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      descriptor_writes[binding].descriptorCount = 1;
      descriptor_writes[binding].pImageInfo =
          &descriptor_images[binding];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(descriptor_writes.size()),
        descriptor_writes.data(), 0, nullptr);

    VkCommandBufferAllocateInfo command_allocate{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate.commandPool = cmd_pool_;
    command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &command_allocate, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }

    const std::array<ImageResource *, 8> all_images{
        &candidate_transmittance, &candidate_irradiance,
        &candidate_scattering,   &delta_irradiance,
        &delta_rayleigh,         &delta_mie,
        &scattering_density,     &delta_multiple,
    };
    std::array<VkImageMemoryBarrier, all_images.size()> initial_barriers{};
    for (std::size_t index = 0; index < all_images.size(); ++index) {
      auto &barrier = initial_barriers[index];
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = all_images[index]->image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.dstAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr,
                         static_cast<std::uint32_t>(initial_barriers.size()),
                         initial_barriers.data());
    VkMemoryBarrier compute_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    compute_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    compute_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    auto separate_compute_passes = [&] {
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                           &compute_barrier, 0, nullptr, 0, nullptr);
    };
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            atmosphere_pipeline_layout_, 0, 1,
                            &atmosphere_desc_set_, 0, nullptr);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_transmittance_pipeline_);
    vkCmdDispatch(command, (dimensions.transmittance_width + 7u) / 8u,
                  (dimensions.transmittance_height + 7u) / 8u, 1u);
    separate_compute_passes();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_direct_irradiance_pipeline_);
    vkCmdDispatch(command, (dimensions.irradiance_width + 7u) / 8u,
                  (dimensions.irradiance_height + 7u) / 8u, 1u);
    separate_compute_passes();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_single_scattering_pipeline_);
    vkCmdDispatch(command, (scattering_width + 3u) / 4u,
                  (dimensions.scattering_view_cosine + 3u) / 4u,
                  (dimensions.scattering_radial + 3u) / 4u);
    separate_compute_passes();
    for (std::uint32_t order = 2u; order <= config.scattering_orders;
         ++order) {
      const std::int32_t density_order = static_cast<std::int32_t>(order);
      vkCmdPushConstants(command, atmosphere_pipeline_layout_,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(density_order), &density_order);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        atmosphere_scattering_density_pipeline_);
      vkCmdDispatch(command, (scattering_width + 3u) / 4u,
                    (dimensions.scattering_view_cosine + 3u) / 4u,
                    (dimensions.scattering_radial + 3u) / 4u);
      separate_compute_passes();

      const std::int32_t irradiance_order =
          static_cast<std::int32_t>(order - 1u);
      vkCmdPushConstants(command, atmosphere_pipeline_layout_,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(irradiance_order), &irradiance_order);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        atmosphere_indirect_irradiance_pipeline_);
      vkCmdDispatch(command, (dimensions.irradiance_width + 7u) / 8u,
                    (dimensions.irradiance_height + 7u) / 8u, 1u);
      separate_compute_passes();

      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        atmosphere_multiple_scattering_pipeline_);
      vkCmdDispatch(command, (scattering_width + 3u) / 4u,
                    (dimensions.scattering_view_cosine + 3u) / 4u,
                    (dimensions.scattering_radial + 3u) / 4u);
      separate_compute_passes();
    }

    const std::array<ImageResource *, 3> persistent_images{
        &candidate_transmittance, &candidate_irradiance,
        &candidate_scattering};
    std::array<VkImageMemoryBarrier, persistent_images.size()>
        transfer_barriers{};
    for (std::size_t index = 0; index < persistent_images.size(); ++index) {
      auto &barrier = transfer_barriers[index];
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = persistent_images[index]->image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(transfer_barriers.size()),
        transfer_barriers.data());
    std::array<VkBufferImageCopy, 3> copies{};
    copies[0].bufferOffset = transmittance_offset;
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[0].imageExtent = {dimensions.transmittance_width,
                             dimensions.transmittance_height, 1u};
    copies[1].bufferOffset = irradiance_offset;
    copies[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[1].imageExtent = {dimensions.irradiance_width,
                             dimensions.irradiance_height, 1u};
    copies[2].bufferOffset = scattering_offset;
    copies[2].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[2].imageExtent = {scattering_width,
                             dimensions.scattering_view_cosine,
                             dimensions.scattering_radial};
    for (std::size_t index = 0; index < persistent_images.size(); ++index) {
      vkCmdCopyImageToBuffer(command, persistent_images[index]->image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &copies[index]);
      transfer_barriers[index].oldLayout =
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      transfer_barriers[index].newLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      transfer_barriers[index].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      transfer_barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(transfer_barriers.size()),
        transfer_barriers.data());
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
    if (submit_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Procedural atmosphere LUT submit failed: API=vkQueueSubmit "
          "VkResult=%s(%d) resource=atmosphere-luts frame_slot=%u "
          "still_job_id=%llu",
          vkResultName(submit_result), static_cast<int>(submit_result),
          frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueSubmit(atmosphere_luts)",
                               submit_result);
      }
      cleanup();
      return false;
    }
    const VkResult wait_result = vkQueueWaitIdle(graphics_queue_);
    if (wait_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Procedural atmosphere LUT wait failed: API=vkQueueWaitIdle "
          "VkResult=%s(%d) resource=atmosphere-luts frame_slot=%u "
          "still_job_id=%llu",
          vkResultName(wait_result), static_cast<int>(wait_result),
          frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueWaitIdle(atmosphere_luts)",
                               wait_result);
      }
      cleanup();
      return false;
    }
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
    command = VK_NULL_HANDLE;

    struct HalfValidation {
      bool valid = false;
      std::uint64_t finite_rgb = 0;
      std::uint64_t positive_rgb = 0;
      std::uint64_t intermediate_rgb = 0;
    };
    const auto *half =
        static_cast<const std::uint16_t *>(readback.mapped);
    auto validate_half_image =
        [&](VkDeviceSize byte_offset, std::uint64_t pixel_count,
            bool unit_bounded, bool opaque_alpha) {
          HalfValidation validation;
          if (half == nullptr || (byte_offset % sizeof(std::uint16_t)) != 0u) {
            return validation;
          }
          const std::uint64_t half_offset =
              byte_offset / sizeof(std::uint16_t);
          validation.valid = true;
          for (std::uint64_t pixel = 0;
               validation.valid && pixel < pixel_count; ++pixel) {
            for (std::uint32_t channel = 0; channel < 3u; ++channel) {
              const std::uint16_t value =
                  half[half_offset + pixel * 4u + channel];
              const std::uint16_t magnitude = value & 0x7fffu;
              const bool finite = (magnitude & 0x7c00u) != 0x7c00u;
              const bool nonnegative =
                  (value & 0x8000u) == 0u || magnitude == 0u;
              const bool bounded = !unit_bounded || magnitude <= 0x3c00u;
              if (!finite || !nonnegative || !bounded) {
                validation.valid = false;
                break;
              }
              ++validation.finite_rgb;
              if (magnitude > 0u) {
                ++validation.positive_rgb;
              }
              if (magnitude > 0u && magnitude < 0x3c00u) {
                ++validation.intermediate_rgb;
              }
            }
            const std::uint16_t alpha =
                half[half_offset + pixel * 4u + 3u];
            const std::uint16_t alpha_magnitude = alpha & 0x7fffu;
            const bool alpha_finite =
                (alpha_magnitude & 0x7c00u) != 0x7c00u;
            const bool alpha_nonnegative =
                (alpha & 0x8000u) == 0u || alpha_magnitude == 0u;
            if (!alpha_finite || !alpha_nonnegative ||
                (opaque_alpha && alpha != 0x3c00u)) {
              validation.valid = false;
            }
          }
          validation.valid =
              validation.valid &&
              validation.finite_rgb == pixel_count * 3u &&
              validation.positive_rgb > 0u;
          return validation;
        };
    const HalfValidation transmittance_validation =
        validate_half_image(transmittance_offset, transmittance_pixels,
                            true, true);
    const HalfValidation irradiance_validation =
        validate_half_image(irradiance_offset, irradiance_pixels,
                            false, true);
    const HalfValidation scattering_validation =
        validate_half_image(scattering_offset, scattering_pixels,
                            false, false);
    const bool valid_output =
        transmittance_validation.valid &&
        transmittance_validation.intermediate_rgb > 0u &&
        irradiance_validation.valid && scattering_validation.valid;
    if (!valid_output) {
      xpbd::log::warn(
          "Procedural atmosphere LUT readback validation failed");
      cleanup();
      return false;
    }

    clearProceduralAtmosphereImage();
    atmosphere_transmittance_ = candidate_transmittance;
    candidate_transmittance = {};
    atmosphere_irradiance_ = candidate_irradiance;
    candidate_irradiance = {};
    atmosphere_scattering_ = candidate_scattering;
    candidate_scattering = {};
    atmosphere_resource_key_ = resource_key;
    atmosphere_failed_key_.clear();
    atmosphere_ready_ = true;
    destroyBuffer(readback);
    destroyImage(delta_irradiance);
    destroyImage(delta_rayleigh);
    destroyImage(delta_mie);
    destroyImage(scattering_density);
    destroyImage(delta_multiple);
    xpbd::log::infof(
        "Procedural atmosphere LUTs ready: transmittance=%ux%u "
        "scattering=%ux%ux%u irradiance=%ux%u orders=%u "
        "positive=%llu/%llu/%llu",
        dimensions.transmittance_width, dimensions.transmittance_height,
        scattering_width, dimensions.scattering_view_cosine,
        dimensions.scattering_radial, dimensions.irradiance_width,
        dimensions.irradiance_height, config.scattering_orders,
        static_cast<unsigned long long>(
            transmittance_validation.positive_rgb),
        static_cast<unsigned long long>(scattering_validation.positive_rgb),
        static_cast<unsigned long long>(irradiance_validation.positive_rgb));
    return true;
  }

  std::string proceduralEnvironmentResourceKey(
      const ResolvedWorldEnvironment &resolved) const {
    if (!atmosphere_ready_ || atmosphere_resource_key_.empty() ||
        resolved.sky_rendering != SkyRendering::ProceduralDayNight ||
        resolved.celestial == nullptr || !resolved.celestial->valid) {
      return {};
    }
    std::uint64_t hash = 14695981039346656037ull;
    appendPathTraceHistoryBytes(hash, atmosphere_resource_key_.data(),
                                atmosphere_resource_key_.size());
    appendPathTraceHistoryValue(hash, resolved.rotation_radians);
    appendPathTraceHistoryValue(hash, resolved.background_visible);
    appendPathTraceHistoryValue(hash, resolved.environment_lighting);
    appendPathTraceHistoryValue(hash, resolved.environment_strength);
    appendPathTraceHistoryValue(hash, resolved.background_multiplier);
    appendPathTraceHistoryValue(hash, resolved.sun_moon_lighting);
    appendPathTraceHistoryValue(hash, resolved.celestial->sun.direction);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->sun.angular_diameter_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->sun.geometric_altitude_degrees);
    appendPathTraceHistoryValue(hash, resolved.celestial->moon.direction);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon.angular_diameter_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_phase_angle_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_illuminated_fraction);
    appendPathTraceHistoryValue(hash, resolved.celestial->moon_magnitude);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_libration_latitude_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_libration_longitude_degrees);
    appendPathTraceHistoryValue(hash, resolved.celestial->sidereal_time_hours);
    appendPathTraceHistoryValue(
        hash, static_cast<std::uint8_t>(resolved.celestial->twilight));
    appendPathTraceHistoryValue(
        hash, resolved.celestial->observer.latitude_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->observer.elevation_meters);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->observer.north_offset_degrees);
    if (resolved.sun == nullptr || resolved.moon == nullptr ||
        resolved.atmosphere_controls == nullptr ||
        resolved.night == nullptr) {
      return {};
    }
    appendPathTraceHistoryValue(hash, resolved.sun->enabled);
    appendPathTraceHistoryValue(hash, resolved.sun->strength);
    appendPathTraceHistoryValue(hash, resolved.sun->direction_mode);
    appendPathTraceHistoryValue(hash,
                                resolved.sun->color_temperature_kelvin);
    appendPathTraceHistoryValue(hash,
                                resolved.sun->angular_diameter_degrees);
    appendPathTraceHistoryValue(hash, resolved.sun->disk_visible);
    appendPathTraceHistoryValue(hash, resolved.sun->cast_shadows);
    appendPathTraceHistoryValue(hash, resolved.moon->enabled);
    appendPathTraceHistoryValue(hash, resolved.moon->strength);
    appendPathTraceHistoryValue(hash, resolved.moon->phase_mode);
    appendPathTraceHistoryValue(
        hash, resolved.moon->manual_illuminated_fraction);
    appendPathTraceHistoryValue(hash, resolved.moon->direction_mode);
    appendPathTraceHistoryValue(hash,
                                resolved.moon->angular_diameter_degrees);
    appendPathTraceHistoryValue(hash, resolved.moon->surface_detail);
    appendPathTraceHistoryValue(hash, resolved.moon->disk_visible);
    appendPathTraceHistoryValue(hash, resolved.moon->cast_shadows);
    appendPathTraceHistoryValue(
        hash, resolved.atmosphere_controls->sky_relative_strength);
    appendPathTraceHistoryValue(hash,
                                resolved.atmosphere_controls->turbidity);
    appendPathTraceHistoryValue(hash, resolved.atmosphere_controls->ozone);
    appendPathTraceHistoryValue(
        hash, resolved.atmosphere_controls->lut_quality);
    appendPathTraceHistoryValue(hash, resolved.night->stars_enabled);
    appendPathTraceHistoryValue(hash, resolved.night->star_intensity);
    appendPathTraceHistoryValue(hash,
                                resolved.night->milky_way_enabled);
    appendPathTraceHistoryValue(hash,
                                resolved.night->milky_way_intensity);
    appendPathTraceHistoryValue(hash, resolved.night->light_pollution);
    appendPathTraceHistoryValue(hash,
                                resolved.night->star_rotation_degrees);
    appendPathTraceHistoryValue(hash, resolved.night->night_fill);
    const VolumetricCloudState disabled_clouds;
    const std::string cloud_key = volumetricCloudCacheKey(
        resolved.clouds != nullptr ? *resolved.clouds : disabled_clouds);
    if (cloud_key.empty()) {
      return {};
    }
    appendPathTraceHistoryBytes(hash, cloud_key.data(), cloud_key.size());
    return std::to_string(hash);
  }

  bool buildDynamicSkyEnvironmentCache(
      const ResolvedWorldEnvironment &resolved,
      const std::string &environment_key) {
    const float render_ratio =
        resolved.clouds != nullptr
            ? std::clamp(resolved.clouds->render_ratio, 0.25f, 1.0f)
            : 1.0f;
    const std::uint32_t kCacheWidth = static_cast<std::uint32_t>(
        std::lround(2048.0f * render_ratio));
    const std::uint32_t kCacheHeight = static_cast<std::uint32_t>(
        std::lround(1024.0f * render_ratio));
    const std::uint64_t kPixelCount =
        static_cast<std::uint64_t>(kCacheWidth) * kCacheHeight;
    const VkDeviceSize kReadbackBytes =
        static_cast<VkDeviceSize>(kPixelCount) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize kDistributionBytes =
        sizeof(WorldEnvironmentGpuHeader) +
        static_cast<VkDeviceSize>(kPixelCount) *
            sizeof(WorldEnvironmentGpuAlias);
    if (!atmosphere_ready_ ||
        atmosphere_environment_cache_pipeline_ == VK_NULL_HANDLE ||
        atmosphere_transmittance_.image == VK_NULL_HANDLE ||
        atmosphere_scattering_.image == VK_NULL_HANDLE ||
        resolved.celestial == nullptr || !resolved.celestial->valid ||
        environment_key.empty() || !ensureWorldEnvironmentSampler()) {
      return false;
    }
    if (atmosphere_environment_pending_.active()) {
      return true;
    }
    if (atmosphere_environment_spare_retirement_fence_ != VK_NULL_HANDLE) {
      const VkResult retirement_result = vkGetFenceStatus(
          device_, atmosphere_environment_spare_retirement_fence_);
      if (retirement_result == VK_NOT_READY) {
        // The old front bundle is still referenced by the other frame slot.
        // Defer without blocking; that slot is waited at the start of its next
        // frame before this function is called again.
        return true;
      }
      if (retirement_result != VK_SUCCESS) {
        xpbd::log::errorf(
            "Dynamic sky retirement fence failed: API=vkGetFenceStatus "
            "VkResult=%s(%d) frame_slot=%u still_job_id=%llu",
            vkResultName(retirement_result),
            static_cast<int>(retirement_result), frame_index_,
            static_cast<unsigned long long>(still_active_job_id_));
        recordFatalVulkanError("vkGetFenceStatus(dynamic_sky_retirement)",
                               retirement_result);
        return false;
      }
      atmosphere_environment_spare_retirement_fence_ = VK_NULL_HANDLE;
    }

    ImageResource candidate_cache{};
    ImageResource candidate_cloud_history{};
    Buffer readback{};
    Buffer pending_distribution{};
    bool candidate_cache_reused = false;
    bool candidate_cloud_history_reused = false;
    bool readback_reused = false;
    bool pending_distribution_reused = false;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence update_fence = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (update_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, update_fence, nullptr);
        update_fence = VK_NULL_HANDLE;
      }
      if (command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      if (readback_reused &&
          atmosphere_environment_readback_.buffer == VK_NULL_HANDLE) {
        atmosphere_environment_readback_ = readback;
        readback = {};
      } else {
        destroyBuffer(readback);
      }
      if (candidate_cache_reused &&
          atmosphere_environment_spare_cache_.image == VK_NULL_HANDLE) {
        atmosphere_environment_spare_cache_ = candidate_cache;
        candidate_cache = {};
      } else {
        destroyImage(candidate_cache);
      }
      if (candidate_cloud_history_reused &&
          atmosphere_cloud_history_spare_.image == VK_NULL_HANDLE) {
        atmosphere_cloud_history_spare_ = candidate_cloud_history;
        candidate_cloud_history = {};
      } else {
        destroyImage(candidate_cloud_history);
      }
      if (pending_distribution_reused &&
          atmosphere_environment_distribution_spare_.buffer ==
              VK_NULL_HANDLE) {
        atmosphere_environment_distribution_spare_ = pending_distribution;
        pending_distribution = {};
      } else {
        destroyBuffer(pending_distribution);
      }
    };
    const auto acquire_image = [&](ImageResource &spare,
                                   ImageResource &candidate,
                                   bool &reused) {
      if (spare.image != VK_NULL_HANDLE && spare.width == kCacheWidth &&
          spare.height == kCacheHeight && spare.depth == 1u) {
        candidate = spare;
        spare = {};
        reused = true;
        return true;
      }
      destroyImage(spare);
      if (!createAtmosphereImage(kCacheWidth, kCacheHeight, 1u, candidate)) {
        return false;
      }
      ++atmosphere_environment_cache_reallocations_;
      return true;
    };
    if (!acquire_image(atmosphere_environment_spare_cache_, candidate_cache,
                       candidate_cache_reused) ||
        !acquire_image(atmosphere_cloud_history_spare_,
                       candidate_cloud_history,
                       candidate_cloud_history_reused)) {
      cleanup();
      return false;
    }
    if (atmosphere_environment_readback_.buffer != VK_NULL_HANDLE &&
        atmosphere_environment_readback_.capacity >= kReadbackBytes) {
      readback = atmosphere_environment_readback_;
      atmosphere_environment_readback_ = {};
      readback_reused = true;
    } else {
      destroyBuffer(atmosphere_environment_readback_);
      if (!createBuffer(kReadbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        readback, "dynamic-sky-readback")) {
        cleanup();
        return false;
      }
      ++atmosphere_environment_cache_reallocations_;
    }
    if (atmosphere_environment_distribution_spare_.buffer !=
            VK_NULL_HANDLE &&
        atmosphere_environment_distribution_spare_.capacity >=
            kDistributionBytes) {
      pending_distribution = atmosphere_environment_distribution_spare_;
      atmosphere_environment_distribution_spare_ = {};
      pending_distribution_reused = true;
    } else {
      destroyBuffer(atmosphere_environment_distribution_spare_);
      if (!createBuffer(kDistributionBytes,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        pending_distribution,
                        "dynamic-sky-environment-distribution")) {
        cleanup();
        return false;
      }
      ++atmosphere_environment_cache_reallocations_;
    }

    VolumetricCloudState cloud_compatibility;
    std::string cloud_compatibility_key;
    std::array<float, 2> current_weather_offset{};
    const std::uint32_t previous_cloud_frame =
        atmosphere_cloud_history_frame_;
    bool cloud_history_valid = false;
    if (resolved.clouds != nullptr) {
      cloud_compatibility = *resolved.clouds;
      cloud_compatibility.time_seconds = 0.0f;
      cloud_compatibility.temporal_frame = 0u;
      cloud_compatibility.generation = 0u;
      cloud_compatibility_key =
          volumetricCloudCacheKey(cloud_compatibility);
      const float advection_hours =
          resolved.clouds->time_seconds / 3600.0f;
      current_weather_offset = {
          resolved.clouds->weather_offset_km[0] +
              resolved.clouds->wind_direction[0] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours,
          resolved.clouds->weather_offset_km[1] +
              resolved.clouds->wind_direction[1] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours};
      cloud_history_valid =
          resolved.clouds->reprojection &&
          atmosphere_cloud_history_.view != VK_NULL_HANDLE &&
          atmosphere_cloud_history_.width == kCacheWidth &&
          atmosphere_cloud_history_.height == kCacheHeight &&
          !cloud_compatibility_key.empty() &&
          cloud_compatibility_key ==
              atmosphere_cloud_history_compatibility_key_;
    }

    std::array<VkDescriptorImageInfo, 5> descriptor_images{};
    descriptor_images[0].sampler = atmosphere_sampler_;
    descriptor_images[0].imageView = atmosphere_transmittance_.view;
    descriptor_images[0].imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_images[1].sampler = atmosphere_sampler_;
    descriptor_images[1].imageView = atmosphere_scattering_.view;
    descriptor_images[1].imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_images[2].imageView = candidate_cache.view;
    descriptor_images[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    descriptor_images[3].sampler = atmosphere_sampler_;
    descriptor_images[3].imageView =
        cloud_history_valid ? atmosphere_cloud_history_.view
                            : atmosphere_transmittance_.view;
    descriptor_images[3].imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_images[4].imageView = candidate_cloud_history.view;
    descriptor_images[4].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    constexpr std::array<std::uint32_t, 5> kBindings{
        1u, 14u, 15u, 16u, 17u};
    constexpr std::array<VkDescriptorType, 5> kTypes{
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (std::size_t index = 0; index < writes.size(); ++index) {
      writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[index].dstSet = atmosphere_desc_set_;
      writes[index].dstBinding = kBindings[index];
      writes[index].descriptorType = kTypes[index];
      writes[index].descriptorCount = 1;
      writes[index].pImageInfo = &descriptor_images[index];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    VkCommandBufferAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = cmd_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate_info, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = candidate_cache_reused
                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = candidate_cache.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = candidate_cache_reused
                                ? VK_ACCESS_SHADER_READ_BIT
                                : 0u;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    VkImageMemoryBarrier cloud_history_barrier = barrier;
    cloud_history_barrier.image = candidate_cloud_history.image;
    cloud_history_barrier.oldLayout =
        candidate_cloud_history_reused
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
    cloud_history_barrier.srcAccessMask =
        candidate_cloud_history_reused ? VK_ACCESS_SHADER_READ_BIT : 0u;
    const std::array<VkImageMemoryBarrier, 2> output_barriers{
        barrier, cloud_history_barrier};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr,
                         static_cast<std::uint32_t>(output_barriers.size()),
                         output_barriers.data());
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_environment_cache_pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            atmosphere_pipeline_layout_, 0, 1,
                            &atmosphere_desc_set_, 0, nullptr);
    constexpr double kRadiansPerDegree =
        3.14159265358979323846 / 180.0;
    if (resolved.sun == nullptr || resolved.moon == nullptr ||
        resolved.atmosphere_controls == nullptr ||
        resolved.night == nullptr) {
      cleanup();
      return false;
    }
    const SunControls &sun_controls = *resolved.sun;
    const MoonControls &moon_controls = *resolved.moon;
    const AtmosphereControls &atmosphere_controls =
        *resolved.atmosphere_controls;
    const NightSkyControls &night_controls = *resolved.night;
    AtmosphereEnvironmentPush environment_push;
    environment_push.sun_direction_observer_height = {
        static_cast<float>(resolved.celestial->sun.direction[0]),
        static_cast<float>(resolved.celestial->sun.direction[1]),
        static_cast<float>(resolved.celestial->sun.direction[2]),
        static_cast<float>((std::max)(
            resolved.celestial->observer.elevation_meters / 1000.0, 0.001))};
    environment_push.moon_direction_angular_radius = {
        static_cast<float>(resolved.celestial->moon.direction[0]),
        static_cast<float>(resolved.celestial->moon.direction[1]),
        static_cast<float>(resolved.celestial->moon.direction[2]),
        std::clamp(moon_controls.angular_diameter_degrees, 0.05f, 5.0f) *
            0.5f * static_cast<float>(kRadiansPerDegree)};
    const float moon_fraction =
        moon_controls.phase_mode == MoonPhaseMode::Manual
            ? std::clamp(moon_controls.manual_illuminated_fraction,
                         0.0f, 1.0f)
            : static_cast<float>(
                  resolved.celestial->moon_illuminated_fraction);
    const float moon_phase_radians =
        moon_controls.phase_mode == MoonPhaseMode::Manual
            ? std::acos(std::clamp(2.0f * moon_fraction - 1.0f,
                                   -1.0f, 1.0f))
            : static_cast<float>(
                  resolved.celestial->moon_phase_angle_degrees *
                  kRadiansPerDegree);
    environment_push.moon_phase_libration = {
        moon_fraction, moon_phase_radians,
        static_cast<float>(
            resolved.celestial->moon_libration_latitude_degrees *
            kRadiansPerDegree),
        static_cast<float>(
            resolved.celestial->moon_libration_longitude_degrees *
            kRadiansPerDegree)};
    environment_push.observer_sidereal_twilight = {
        static_cast<float>(resolved.celestial->observer.latitude_degrees *
                           kRadiansPerDegree),
        static_cast<float>(resolved.celestial->observer.north_offset_degrees *
                           kRadiansPerDegree),
        static_cast<float>(
            (resolved.celestial->sidereal_time_hours * 15.0 +
             night_controls.star_rotation_degrees) *
            kRadiansPerDegree),
        static_cast<float>(
            resolved.celestial->sun.geometric_altitude_degrees)};
    const float light_pollution_attenuation =
        std::exp2(-std::clamp(night_controls.light_pollution, 0.0f, 16.0f));
    environment_push.night_parameters = {
        std::clamp(sun_controls.angular_diameter_degrees, 0.05f, 5.0f) *
            0.5f * static_cast<float>(kRadiansPerDegree),
        static_cast<float>(resolved.celestial->moon_magnitude), 1.0f, 1.0f};
    environment_push.night_parameters[2] =
        night_controls.stars_enabled
            ? std::clamp(night_controls.star_intensity, 0.0f, 32.0f) *
                  light_pollution_attenuation
            : 0.0f;
    environment_push.night_parameters[3] =
        night_controls.milky_way_enabled
            ? std::clamp(night_controls.milky_way_intensity, 0.0f, 32.0f) *
                  light_pollution_attenuation
            : 0.0f;
    environment_push.sky_energy = {
        std::clamp(atmosphere_controls.sky_relative_strength, 0.0f, 8.0f),
        std::clamp(sun_controls.strength, 0.0f, 32.0f),
        std::clamp(moon_controls.strength, 0.0f, 32.0f),
        std::clamp(night_controls.night_fill, 0.0f, 4.0f)};
    std::uint32_t sky_flags = 0u;
    sky_flags |= sun_controls.enabled ? 1u : 0u;
    sky_flags |= moon_controls.enabled ? 2u : 0u;
    sky_flags |=
        moon_controls.phase_mode == MoonPhaseMode::Manual ? 4u : 0u;
    environment_push.sky_flags = {
        sky_flags,
        std::bit_cast<std::uint32_t>(
            std::clamp(moon_controls.surface_detail, 0.0f, 1.0f)),
        static_cast<std::uint32_t>(resolved.debug_view), 0u};
    if (resolved.clouds != nullptr) {
      const float advection_hours = resolved.clouds->time_seconds / 3600.0f;
      environment_push.cloud_layer = {
          1.0f, resolved.clouds->coverage, resolved.clouds->density,
          resolved.clouds->base_altitude_km};
      environment_push.cloud_weather = {
          resolved.clouds->thickness_km,
          resolved.clouds->weather_offset_km[0] +
              resolved.clouds->wind_direction[0] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours,
          resolved.clouds->weather_offset_km[1] +
              resolved.clouds->wind_direction[1] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours,
          resolved.clouds->time_seconds};
      environment_push.cloud_quality = {
          resolved.clouds->seed, resolved.clouds->ray_steps,
          resolved.clouds->light_steps, resolved.clouds->temporal_frame};
      environment_push.cloud_optics = {
          resolved.clouds->weather_scale,
          resolved.clouds->base_shape_scale,
          resolved.clouds->detail_scale, resolved.clouds->erosion};
      environment_push.cloud_lighting = {
          resolved.clouds->forward_scattering,
          resolved.clouds->silver_lining, resolved.clouds->absorption,
          resolved.clouds->multiple_scattering};
      environment_push.cloud_post = {
          resolved.clouds->shadow_strength,
          resolved.clouds->lighting_strength,
          resolved.clouds->render_ratio,
          resolved.clouds->history_weight};
      environment_push.cloud_history = {
          current_weather_offset[0] -
              atmosphere_cloud_history_weather_offset_[0],
          current_weather_offset[1] -
              atmosphere_cloud_history_weather_offset_[1],
          cloud_history_valid ? 1.0f : 0.0f,
          static_cast<float>(resolved.clouds->shadow_resolution)};
    }
    vkCmdPushConstants(command, atmosphere_pipeline_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(environment_push), &environment_push);
    vkCmdDispatch(command, (kCacheWidth + 7u) / 8u,
                  (kCacheHeight + 7u) / 8u, 1u);

    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    cloud_history_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    cloud_history_barrier.newLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cloud_history_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cloud_history_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const std::array<VkImageMemoryBarrier, 2> post_compute_barriers{
        barrier, cloud_history_barrier};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(
                             post_compute_barriers.size()),
                         post_compute_barriers.data());
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kCacheWidth, kCacheHeight, 1u};
    vkCmdCopyImageToBuffer(command, candidate_cache.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);
    VkBufferMemoryBarrier readback_barrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    readback_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readback_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.buffer = readback.buffer;
    readback_barrier.offset = 0u;
    readback_barrier.size = kReadbackBytes;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 1, &readback_barrier, 1, &barrier);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fence_info, nullptr, &update_fence) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    const auto cache_compute_begin = Clock::now();
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit, update_fence);
    if (submit_result != VK_SUCCESS) {
      xpbd::log::warnf(
          "Dynamic sky cache submit failed: API=vkQueueSubmit "
          "VkResult=%s(%d) resource=dynamic-sky-cache extent=%ux%u "
          "frame_slot=%u still_job_id=%llu",
          vkResultName(submit_result), static_cast<int>(submit_result),
          kCacheWidth, kCacheHeight, frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueSubmit(dynamic_sky)", submit_result);
      }
      cleanup();
      return false;
    }

    DynamicSkyCpuInput cpu_input;
    cpu_input.readback =
        static_cast<const std::uint16_t *>(readback.mapped);
    cpu_input.distribution_mapped = pending_distribution.mapped;
    cpu_input.distribution_capacity = pending_distribution.capacity;
    cpu_input.width = kCacheWidth;
    cpu_input.height = kCacheHeight;
    cpu_input.celestial = *resolved.celestial;
    cpu_input.sun = sun_controls;
    cpu_input.moon = moon_controls;
    cpu_input.background_visible = resolved.background_visible;
    cpu_input.environment_lighting = resolved.environment_lighting;
    cpu_input.sun_moon_lighting = resolved.sun_moon_lighting;
    cpu_input.environment_strength = resolved.environment_strength;
    cpu_input.background_multiplier = resolved.background_multiplier;
    cpu_input.rotation_radians = resolved.rotation_radians;
    cpu_input.moon_fraction = moon_fraction;
    cpu_input.moon_phase_radians = moon_phase_radians;

    DynamicSkyPending pending;
    pending.cache = candidate_cache;
    candidate_cache = {};
    pending.cloud_history = candidate_cloud_history;
    candidate_cloud_history = {};
    pending.readback = readback;
    readback = {};
    pending.distribution = pending_distribution;
    pending_distribution = {};
    pending.command = command;
    command = VK_NULL_HANDLE;
    pending.fence = update_fence;
    update_fence = VK_NULL_HANDLE;
    pending.environment_key = environment_key;
    pending.cloud_compatibility_key = std::move(cloud_compatibility_key);
    pending.weather_offset = current_weather_offset;
    pending.cloud_history_parameters = environment_push.cloud_history;
    pending.cloud_frame =
        resolved.clouds != nullptr ? resolved.clouds->temporal_frame : 0u;
    pending.previous_cloud_frame = previous_cloud_frame;
    pending.cloud_history_weight =
        resolved.clouds != nullptr ? resolved.clouds->history_weight : 0.0f;
    pending.cloud_shadow_resolution =
        resolved.clouds != nullptr ? resolved.clouds->shadow_resolution : 0u;
    pending.cloud_enabled = resolved.clouds != nullptr;
    pending.cloud_history_valid = cloud_history_valid;
    pending.distribution_bytes = kDistributionBytes;
    pending.submitted_at = cache_compute_begin;
    atmosphere_environment_pending_ = std::move(pending);
    try {
      const VkDevice worker_device = device_;
      const VkFence worker_fence = atmosphere_environment_pending_.fence;
      atmosphere_environment_pending_.completion = std::async(
          std::launch::async,
          [cpu_input, worker_device, worker_fence,
           cache_compute_begin]() mutable {
            return buildDynamicSkyDistribution(
                cpu_input, worker_device, worker_fence,
                cache_compute_begin);
          });
    } catch (const std::exception &exception) {
      xpbd::log::errorf(
          "Dynamic sky worker launch failed: %s; waiting only to reclaim "
          "submitted resources safely",
          exception.what());
      (void)pollDynamicSkyEnvironmentCache(true);
      return false;
    }
    atmosphere_environment_last_update_ = cache_compute_begin;
    xpbd::log::infof(
        "Dynamic sky asynchronous update queued: %ux%u "
        "queue_idle_count=0 cache_realloc_count=%llu",
        kCacheWidth, kCacheHeight,
        static_cast<unsigned long long>(
            atmosphere_environment_cache_reallocations_));
    return true;
  }

  bool ensureProceduralAtmosphereResources(
      const ResolvedWorldEnvironment &resolved) {
    if (resolved.sky_rendering != SkyRendering::ProceduralDayNight ||
        resolved.atmosphere == nullptr) {
      if (atmosphere_transmittance_.image != VK_NULL_HANDLE ||
          atmosphere_transmittance_pipeline_ != VK_NULL_HANDLE) {
        if (vkQueueWaitIdle(graphics_queue_) == VK_SUCCESS) {
          destroyProceduralAtmosphereGpu();
        }
      }
      atmosphere_failed_key_.clear();
      return false;
    }
    (void)pollDynamicSkyEnvironmentCache(false);
    if (fatal_error_) {
      return false;
    }
    const std::string resource_key =
        brunetonAtmosphereCacheKey(*resolved.atmosphere);
    if (resource_key.empty()) {
      return false;
    }
    if (atmosphere_ready_ && atmosphere_resource_key_ == resource_key) {
      // Reuse the static physical LUTs and update only the dynamic sky cache.
    } else {
      // Static LUT rebuild updates the shared atmosphere descriptor set. It is
      // rare and must not race an in-flight dynamic-cache dispatch.
      if (atmosphere_environment_pending_.active()) {
        (void)pollDynamicSkyEnvironmentCache(true);
        if (fatal_error_ || atmosphere_environment_pending_.active()) {
          return false;
        }
      }
      if (atmosphere_failed_key_ == resource_key) {
        return false;
      }
      if (!buildProceduralAtmosphereLuts(resolved, resource_key)) {
        atmosphere_failed_key_ = resource_key;
        xpbd::log::warn(
            "Procedural atmosphere GPU precomputation failed; resolving Off");
        return false;
      }
    }
    const std::string environment_key =
        proceduralEnvironmentResourceKey(resolved);
    if (environment_key.empty()) {
      return false;
    }
    if (atmosphere_environment_ready_ &&
        atmosphere_environment_key_ == environment_key) {
      return true;
    }
    if (atmosphere_environment_pending_.active()) {
      return atmosphere_environment_ready_;
    }
    const float requested_render_ratio =
        resolved.clouds != nullptr
            ? std::clamp(resolved.clouds->render_ratio, 0.25f, 1.0f)
            : 1.0f;
    const std::uint32_t requested_width = static_cast<std::uint32_t>(
        std::lround(2048.0f * requested_render_ratio));
    const std::uint32_t requested_height = static_cast<std::uint32_t>(
        std::lround(1024.0f * requested_render_ratio));
    constexpr auto kMinimumDynamicSkyUpdateInterval =
        std::chrono::milliseconds(100);
    if (atmosphere_environment_ready_ &&
        atmosphere_environment_cache_.width == requested_width &&
        atmosphere_environment_cache_.height == requested_height &&
        atmosphere_environment_last_update_ != Clock::time_point{} &&
        Clock::now() - atmosphere_environment_last_update_ <
            kMinimumDynamicSkyUpdateInterval) {
      return true;
    }
    if (atmosphere_environment_failed_key_ == environment_key) {
      return atmosphere_environment_ready_;
    }
    if (!buildDynamicSkyEnvironmentCache(resolved, environment_key)) {
      if (fatal_error_) {
        return false;
      }
      atmosphere_environment_failed_key_ = environment_key;
      xpbd::log::warn(
          "Dynamic sky environment cache failed; retaining previous cache");
      return atmosphere_environment_ready_;
    }
    return atmosphere_environment_ready_;
  }

  void clearWorldEnvironmentResources() {
    destroyImage(world_environment_texture_);
    destroyBuffer(world_environment_distribution_);
    world_environment_distribution_bytes_ = 0;
    world_environment_resource_key_ = 0;
    world_environment_ready_ = false;
  }

  void destroyWorldEnvironmentGpu() {
    clearWorldEnvironmentResources();
    if (world_environment_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, world_environment_sampler_, nullptr);
      world_environment_sampler_ = VK_NULL_HANDLE;
    }
    world_environment_failed_key_ = 0;
  }

  std::uint64_t worldEnvironmentResourceKey(
      const ResolvedWorldEnvironment &resolved) const {
    std::uint64_t key = 14695981039346656037ull;
    appendPathTraceHistoryValue(
        key, static_cast<std::uint32_t>(resolved.sky_rendering));
    appendPathTraceHistoryValue(key, resolved.generation);
    appendPathTraceHistoryValue(key, resolved.background_visible);
    appendPathTraceHistoryValue(key, resolved.environment_lighting);
    appendPathTraceHistoryValue(key, resolved.environment_strength);
    appendPathTraceHistoryValue(key, resolved.background_exposure);
    appendPathTraceHistoryValue(key, resolved.rotation_radians);
    if (resolved.hdr != nullptr) {
      appendPathTraceHistoryBytes(key, resolved.hdr->checksum.data(),
                                  resolved.hdr->checksum.size());
    }
    return key;
  }

  bool ensureWorldEnvironmentSampler() {
    if (world_environment_sampler_ != VK_NULL_HANDLE) {
      return true;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    return vkCreateSampler(device_, &sampler_info, nullptr,
                           &world_environment_sampler_) == VK_SUCCESS;
  }

  bool uploadWorldEnvironment(
      const ResolvedWorldEnvironment &resolved) {
    constexpr VkDeviceSize kMaximumGpuBytes =
        VkDeviceSize{512} * 1024u * 1024u;
    if (resolved.sky_rendering != SkyRendering::UserHdri ||
        resolved.hdr == nullptr || !resolved.hdr->valid()) {
      return false;
    }
    const FloatEnvironmentImage &radiance = resolved.hdr->radiance;
    const EnvironmentDistribution &distribution =
        resolved.hdr->distribution;
    if (!radiance.valid() || !distribution.valid() ||
        radiance.width != distribution.width() ||
        radiance.height != distribution.height()) {
      return false;
    }
    const std::uint64_t entry_count64 =
        static_cast<std::uint64_t>(radiance.width) * radiance.height;
    if (entry_count64 == 0u ||
        entry_count64 >
            (std::numeric_limits<std::uint32_t>::max)()) {
      return false;
    }
    const VkDeviceSize entry_count =
        static_cast<VkDeviceSize>(entry_count64);
    const VkDeviceSize image_bytes =
        entry_count * VkDeviceSize{4u * sizeof(float)};
    const VkDeviceSize table_bytes =
        sizeof(WorldEnvironmentGpuHeader) +
        entry_count * sizeof(WorldEnvironmentGpuAlias);
    if (image_bytes > kMaximumGpuBytes ||
        table_bytes > kMaximumGpuBytes ||
        image_bytes > kMaximumGpuBytes - table_bytes) {
      xpbd::log::warnf(
          "World HDRI GPU upload rejected: image=%llu table=%llu "
          "combined limit=%llu",
          static_cast<unsigned long long>(image_bytes),
          static_cast<unsigned long long>(table_bytes),
          static_cast<unsigned long long>(kMaximumGpuBytes));
      return false;
    }

    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(
        phys_, VK_FORMAT_R32G32B32A32_SFLOAT, &format_properties);
    if ((format_properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0u) {
      xpbd::log::warn(
          "World HDRI GPU upload rejected: RGBA32F sampling unsupported");
      return false;
    }
    if (!ensureWorldEnvironmentSampler()) {
      xpbd::log::warn("World HDRI sampler creation failed");
      return false;
    }

    Buffer staging{};
    Buffer new_distribution{};
    ImageResource new_texture{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      destroyBuffer(staging);
      destroyBuffer(new_distribution);
      destroyImage(new_texture);
    };
    if (!createBuffer(image_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging) ||
        !createBuffer(table_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      new_distribution) ||
        !createStaticTexture(radiance.width, radiance.height,
                             VK_FORMAT_R32G32B32A32_SFLOAT,
                             new_texture)) {
      cleanup();
      return false;
    }
    std::memcpy(staging.mapped, radiance.rgba.data(),
                static_cast<std::size_t>(image_bytes));

    constexpr std::uint32_t kValidHdr = 1u << 0u;
    constexpr std::uint32_t kBackgroundVisible = 1u << 1u;
    constexpr std::uint32_t kLightingEnabled = 1u << 2u;
    WorldEnvironmentGpuHeader header;
    header.flags = kValidHdr |
                   (resolved.background_visible ? kBackgroundVisible : 0u) |
                   (resolved.environment_lighting ? kLightingEnabled : 0u);
    header.width = radiance.width;
    header.height = radiance.height;
    header.entry_count = static_cast<std::uint32_t>(entry_count64);
    header.lighting_strength = resolved.environment_strength;
    header.background_multiplier = resolved.background_multiplier;
    header.rotation_radians = resolved.rotation_radians;
    std::memcpy(new_distribution.mapped, &header, sizeof(header));
    auto *gpu_alias = reinterpret_cast<WorldEnvironmentGpuAlias *>(
        static_cast<std::byte *>(new_distribution.mapped) +
        sizeof(WorldEnvironmentGpuHeader));
    for (std::uint32_t y = 0; y < radiance.height; ++y) {
      for (std::uint32_t x = 0; x < radiance.width; ++x) {
        const std::uint32_t index = y * radiance.width + x;
        gpu_alias[index].acceptance = static_cast<float>(
            std::clamp(distribution.aliasAcceptance(x, y), 0.0, 1.0));
        gpu_alias[index].alias_index =
            distribution.aliasIndex(x, y);
        gpu_alias[index].probability = static_cast<float>(
            std::max(distribution.texelProbability(x, y), 0.0));
      }
    }

    VkCommandBufferAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = cmd_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate_info, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = new_texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {radiance.width, radiance.height, 1};
    vkCmdCopyBufferToImage(command, staging.buffer, new_texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
    if (submit_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "HDR environment upload submit failed: API=vkQueueSubmit "
          "VkResult=%s(%d) resource=world-environment extent=%ux%u "
          "frame_slot=%u still_job_id=%llu",
          vkResultName(submit_result), static_cast<int>(submit_result),
          radiance.width, radiance.height, frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueSubmit(world_environment)",
                               submit_result);
      }
      cleanup();
      return false;
    }
    const VkResult wait_result = vkQueueWaitIdle(graphics_queue_);
    if (wait_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "HDR environment upload wait failed: API=vkQueueWaitIdle "
          "VkResult=%s(%d) resource=world-environment extent=%ux%u "
          "frame_slot=%u still_job_id=%llu",
          vkResultName(wait_result), static_cast<int>(wait_result),
          radiance.width, radiance.height, frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueWaitIdle(world_environment)",
                               wait_result);
      }
      cleanup();
      return false;
    }
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
    command = VK_NULL_HANDLE;
    destroyBuffer(staging);

    clearWorldEnvironmentResources();
    world_environment_texture_ = new_texture;
    new_texture = {};
    world_environment_distribution_ = new_distribution;
    new_distribution = {};
    world_environment_distribution_bytes_ = table_bytes;
    world_environment_resource_key_ =
        worldEnvironmentResourceKey(resolved);
    world_environment_failed_key_ = 0;
    world_environment_ready_ = true;
    xpbd::log::infof(
        "World HDRI GPU ready: %ux%u image=%llu table=%llu "
        "generation=%llu",
        radiance.width, radiance.height,
        static_cast<unsigned long long>(image_bytes),
        static_cast<unsigned long long>(table_bytes),
        static_cast<unsigned long long>(resolved.generation));
    return true;
  }

  bool ensureWorldEnvironmentResources(
      const ResolvedWorldEnvironment &resolved) {
    if (resolved.sky_rendering != SkyRendering::UserHdri ||
        resolved.hdr == nullptr) {
      if (world_environment_texture_.image != VK_NULL_HANDLE ||
          world_environment_distribution_.buffer != VK_NULL_HANDLE) {
        if (vkQueueWaitIdle(graphics_queue_) == VK_SUCCESS) {
          clearWorldEnvironmentResources();
        }
      }
      world_environment_failed_key_ = 0;
      return false;
    }
    const std::uint64_t resource_key =
        worldEnvironmentResourceKey(resolved);
    if (world_environment_ready_ &&
        world_environment_resource_key_ == resource_key) {
      return true;
    }
    if (world_environment_failed_key_ == resource_key) {
      return false;
    }
    if (!uploadWorldEnvironment(resolved)) {
      world_environment_failed_key_ = resource_key;
      xpbd::log::warnf(
          "World HDRI GPU upload failed for generation %llu; resolving Off",
          static_cast<unsigned long long>(resolved.generation));
      return false;
    }
    return true;
  }

  void destroyStaticModelResources() {
    destroyBuffer(static_model_vbo_);
    destroyBuffer(static_model_ibo_);
    destroyImage(static_texture_);
    destroyImage(static_normal_texture_);
    destroyImage(static_specular_texture_);
    static_draw_plan_ = {};
    static_generations_ = {};
    static_bone_count_ = 0;
    static_vertex_bytes_ = 0;
    static_index_bytes_ = 0;
    static_model_ready_ = false;
  }

  bool createStaticTexture(std::uint32_t width, std::uint32_t height,
                           VkFormat format, ImageResource &out) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &out.image) !=
        VK_SUCCESS) {
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, out.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    const auto memory_type =
        findMemoryType(requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      destroyImage(out);
      return false;
    }
    allocation.memoryTypeIndex = *memory_type;
    if (vkAllocateMemory(device_, &allocation, nullptr, &out.memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(device_, out.image, out.memory, 0) != VK_SUCCESS) {
      destroyImage(out);
      return false;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view_info, nullptr, &out.view) !=
        VK_SUCCESS) {
      destroyImage(out);
      return false;
    }
    out.width = width;
    out.height = height;
    return true;
  }

  bool createFrameGenerationImage(std::uint32_t width,
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

  void destroyStaticMaterialSamplers() {
    if (static_specular_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, static_specular_sampler_, nullptr);
      static_specular_sampler_ = VK_NULL_HANDLE;
    }
    if (static_normal_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, static_normal_sampler_, nullptr);
      static_normal_sampler_ = VK_NULL_HANDLE;
    }
    if (static_albedo_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, static_albedo_sampler_, nullptr);
      static_albedo_sampler_ = VK_NULL_HANDLE;
    }
  }

  void updateStaticTextureDescriptors() {
    std::array<VkDescriptorImageInfo, 3> image_infos{};
    const std::array<VkImageView, 3> views{
        static_texture_.view, static_normal_texture_.view,
        static_specular_texture_.view};
    const std::array<VkSampler, 3> samplers{
        static_albedo_sampler_, static_normal_sampler_,
        static_specular_sampler_};
    for (std::size_t i = 0; i < image_infos.size(); ++i) {
      image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      image_infos[i].imageView = views[i];
      image_infos[i].sampler = samplers[i];
    }
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      for (std::size_t image = 0; image < image_infos.size(); ++image) {
        auto &write = writes[i * image_infos.size() + image];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frames_[i].static_descriptor_set;
        write.dstBinding = static_cast<std::uint32_t>(1u + image);
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_infos[image];
      }
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    if (static_rt_desc_layout_ && static_rt_descriptor_sets_[0]) {
      std::array<VkWriteDescriptorSet, 6> rt_writes{};
      constexpr std::array<std::uint32_t, 3> kRtBindings{1u, 3u, 4u};
      for (std::size_t i = 0; i < frames_.size(); ++i) {
        for (std::size_t image = 0; image < image_infos.size(); ++image) {
          auto &write = rt_writes[i * image_infos.size() + image];
          write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write.dstSet = static_rt_descriptor_sets_[i];
          write.dstBinding = kRtBindings[image];
          write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          write.descriptorCount = 1;
          write.pImageInfo = &image_infos[image];
        }
      }
      vkUpdateDescriptorSets(device_,
                             static_cast<std::uint32_t>(rt_writes.size()),
                             rt_writes.data(), 0, nullptr);
    }
  }

  void updateStaticBoneDescriptor(FrameSync &frame) {
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = frame.bone_ssbo.buffer;
    buffer_info.offset = 0;
    buffer_info.range = frame.bone_ssbo.capacity;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = frame.static_descriptor_set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    // Match frame index for RT descriptor set.
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      if (&frames_[i] != &frame) {
        continue;
      }
      if (static_rt_descriptor_sets_[i]) {
        write.dstSet = static_rt_descriptor_sets_[i];
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
      }
      break;
    }
  }

  bool rebuildStaticModelResources(const StaticIndexedModelMesh &mesh,
                                   const TextureImage *texture,
                                   const ResolvedMaterialTable *material,
                                   std::uint64_t model_generation,
                                   std::uint64_t texture_generation,
                                   std::uint64_t &uploaded_bytes) {
    uploaded_bytes = 0;
    StaticModelDrawPlan new_plan = makeStaticModelDrawPlan(mesh, texture);

    std::vector<StaticGpuVertex> gpu_vertices;
    if (!new_plan.indices.empty()) {
      std::vector<float> normal_sign(mesh.vertices.size(), 1.0f);
      if (texture == nullptr || !texture->valid()) {




        for (const auto &face : mesh.faces) {
          if (!face.textured || !detail::validFace(mesh, face) ||
              (face.direction != StaticModelFaceDirection::Down &&
               face.direction != StaticModelFaceDirection::Up)) {
            continue;
          }
          const std::size_t end =
              static_cast<std::size_t>(face.first_vertex) + face.vertex_count;
          std::fill(normal_sign.begin() + face.first_vertex,
                    normal_sign.begin() + end, -1.0f);
        }
      }
      gpu_vertices.resize(mesh.vertices.size());
      for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto &source = mesh.vertices[i];
        auto &destination = gpu_vertices[i];
        destination.px = source.px;
        destination.py = source.py;
        destination.pz = source.pz;
        destination.nx = source.nx * normal_sign[i];
        destination.ny = source.ny * normal_sign[i];
        destination.nz = source.nz * normal_sign[i];
        destination.u = source.u;
        destination.v = source.v;
        destination.bone_index = source.bone_index;
        destination.flags = new_plan.textured_vertices[i] != 0u ? 1u : 0u;
        destination.tx = source.tx;
        destination.ty = source.ty;
        destination.tz = source.tz;
        destination.tangent_handedness = source.tangent_handedness;
      }
    }

    const VkDeviceSize vertex_bytes = static_cast<VkDeviceSize>(
        gpu_vertices.size() * sizeof(StaticGpuVertex));
    const VkDeviceSize index_bytes = static_cast<VkDeviceSize>(
        new_plan.indices.size() * sizeof(std::uint32_t));
    constexpr std::array<std::uint8_t, 4> kWhitePixel = {255, 255, 255, 255};
    constexpr std::array<std::uint8_t, 4> kFlatNormalPixel = {
        128, 128, 255, 255};
    constexpr std::array<std::uint8_t, 4> kFallbackSpecularPixel = {
        0, 10, 0, 255};
    const bool has_texture = texture != nullptr && texture->valid();
    const std::uint8_t *texture_pixels =
        has_texture ? texture->rgba.data() : kWhitePixel.data();
    const std::uint32_t texture_width =
        has_texture ? static_cast<std::uint32_t>(texture->width) : 1u;
    const std::uint32_t texture_height =
        has_texture ? static_cast<std::uint32_t>(texture->height) : 1u;
    const VkDeviceSize texture_bytes =
        static_cast<VkDeviceSize>(texture_width) * texture_height * 4u;
    const bool has_normal =
        material != nullptr && material->normal_map_active &&
        material->normal_image.valid();
    const bool has_specular =
        material != nullptr && material->specular_map_active &&
        material->specular_image.valid();
    xpbd::log::infof(
        "VKDIAG LabPBR GPU material normal=%d specular=%d flags=%u "
        "base=%ux%u normal=%ux%u specular=%ux%u",
        has_normal ? 1 : 0, has_specular ? 1 : 0,
        labPbrFeatureFlags(material), texture_width, texture_height,
        has_normal ? static_cast<std::uint32_t>(material->normal_image.width)
                   : 1u,
        has_normal ? static_cast<std::uint32_t>(material->normal_image.height)
                   : 1u,
        has_specular
            ? static_cast<std::uint32_t>(material->specular_image.width)
            : 1u,
        has_specular
            ? static_cast<std::uint32_t>(material->specular_image.height)
            : 1u);
    const std::uint8_t *normal_pixels =
        has_normal ? material->normal_image.rgba.data()
                   : kFlatNormalPixel.data();
    const std::uint8_t *specular_pixels =
        has_specular ? material->specular_image.rgba.data()
                     : kFallbackSpecularPixel.data();
    // Sidecars are required to match the base atlas during LabPBR resolve,
    // but use their own dimensions here so a malformed/legacy resource can
    // never make the staging copy read past the sidecar allocation.
    const std::uint32_t normal_width =
        has_normal ? static_cast<std::uint32_t>(material->normal_image.width)
                   : 1u;
    const std::uint32_t normal_height =
        has_normal ? static_cast<std::uint32_t>(material->normal_image.height)
                   : 1u;
    const std::uint32_t specular_width =
        has_specular
            ? static_cast<std::uint32_t>(material->specular_image.width)
            : 1u;
    const std::uint32_t specular_height =
        has_specular
            ? static_cast<std::uint32_t>(material->specular_image.height)
            : 1u;
    const VkDeviceSize normal_bytes =
        static_cast<VkDeviceSize>(normal_width) * normal_height * 4u;
    const VkDeviceSize specular_bytes =
        static_cast<VkDeviceSize>(specular_width) * specular_height * 4u;
    if (vertex_bytes >
            (std::numeric_limits<VkDeviceSize>::max)() - index_bytes ||
        vertex_bytes + index_bytes >
            (std::numeric_limits<VkDeviceSize>::max)() - texture_bytes ||
        vertex_bytes + index_bytes + texture_bytes >
            (std::numeric_limits<VkDeviceSize>::max)() - normal_bytes ||
        vertex_bytes + index_bytes + texture_bytes + normal_bytes >
            (std::numeric_limits<VkDeviceSize>::max)() - specular_bytes) {
      writeLog("Vulkan static resource size overflow");
      return false;
    }

    Buffer staging{};
    Buffer new_vertex_buffer{};
    Buffer new_index_buffer{};
    ImageResource new_texture{};
    ImageResource new_normal_texture{};
    ImageResource new_specular_texture{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (upload_fence) {
        vkDestroyFence(device_, upload_fence, nullptr);
        upload_fence = VK_NULL_HANDLE;
      }
      if (command) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      destroyBuffer(staging);
      destroyBuffer(new_vertex_buffer);
      destroyBuffer(new_index_buffer);
      destroyImage(new_texture);
      destroyImage(new_normal_texture);
      destroyImage(new_specular_texture);
    };

    const VkDeviceSize staging_bytes =
        vertex_bytes + index_bytes + texture_bytes + normal_bytes +
        specular_bytes;
    if (!createBuffer(staging_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging) ||
        (vertex_bytes > 0 &&
         !createBuffer(vertex_bytes,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       new_vertex_buffer)) ||
        (index_bytes > 0 && !createBuffer(index_bytes,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          new_index_buffer)) ||
        !createStaticTexture(texture_width, texture_height,
                             VK_FORMAT_R8G8B8A8_UNORM, new_texture) ||
        !createStaticTexture(normal_width, normal_height,
                             VK_FORMAT_R8G8B8A8_UNORM,
                             new_normal_texture) ||
        !createStaticTexture(specular_width, specular_height,
                             VK_FORMAT_R8G8B8A8_UNORM,
                             new_specular_texture)) {
      cleanup();
      writeLog("Vulkan static device resource allocation failed");
      return false;
    }

    if (vertex_bytes > 0) {
      std::memcpy(staging.mapped, gpu_vertices.data(),
                  static_cast<std::size_t>(vertex_bytes));
    }
    if (index_bytes > 0) {
      std::memcpy(static_cast<std::byte *>(staging.mapped) + vertex_bytes,
                  new_plan.indices.data(),
                  static_cast<std::size_t>(index_bytes));
    }
    std::memcpy(static_cast<std::byte *>(staging.mapped) + vertex_bytes +
                    index_bytes,
                texture_pixels, static_cast<std::size_t>(texture_bytes));
    const VkDeviceSize normal_offset =
        vertex_bytes + index_bytes + texture_bytes;
    const VkDeviceSize specular_offset = normal_offset + normal_bytes;
    std::memcpy(static_cast<std::byte *>(staging.mapped) + normal_offset,
                normal_pixels, static_cast<std::size_t>(normal_bytes));
    std::memcpy(static_cast<std::byte *>(staging.mapped) + specular_offset,
                specular_pixels, static_cast<std::size_t>(specular_bytes));

    VkCommandBufferAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = cmd_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate_info, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }

    if (vertex_bytes > 0) {
      VkBufferCopy copy{0, 0, vertex_bytes};
      vkCmdCopyBuffer(command, staging.buffer, new_vertex_buffer.buffer, 1,
                      &copy);
    }
    if (index_bytes > 0) {
      VkBufferCopy copy{vertex_bytes, 0, index_bytes};
      vkCmdCopyBuffer(command, staging.buffer, new_index_buffer.buffer, 1,
                      &copy);
    }

    std::array<VkBufferMemoryBarrier, 2> geometry_barriers{};
    std::uint32_t geometry_barrier_count = 0u;
    const auto append_geometry_barrier =
        [&](VkBuffer buffer, VkAccessFlags consumer_access) {
          auto &barrier = geometry_barriers[geometry_barrier_count++];
          barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
          barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.dstAccessMask = consumer_access |
                                  VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.buffer = buffer;
          barrier.offset = 0u;
          barrier.size = VK_WHOLE_SIZE;
        };
    if (vertex_bytes > 0) {
      append_geometry_barrier(new_vertex_buffer.buffer,
                              VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
    }
    if (index_bytes > 0) {
      append_geometry_barrier(new_index_buffer.buffer,
                              VK_ACCESS_INDEX_READ_BIT);
    }
    if (geometry_barrier_count > 0u) {
      constexpr VkPipelineStageFlags kGeometryConsumerStages =
          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
          VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           kGeometryConsumerStages, 0, 0, nullptr,
                           geometry_barrier_count, geometry_barriers.data(),
                           0, nullptr);
    }

    const auto upload_image =
        [&](const ImageResource &image, std::uint32_t width,
            std::uint32_t height, VkDeviceSize offset) {
          VkImageMemoryBarrier barrier{
              VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = image.image;
          barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          barrier.srcAccessMask = 0;
          barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &barrier);
          VkBufferImageCopy image_copy{};
          image_copy.bufferOffset = offset;
          image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          image_copy.imageExtent = {width, height, 1};
          vkCmdCopyBufferToImage(command, staging.buffer, image.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &image_copy);
          barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          constexpr VkPipelineStageFlags kTextureConsumerStages =
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
              VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
          vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               kTextureConsumerStages, 0, 0, nullptr, 0,
                               nullptr, 1, &barrier);
        };
    upload_image(new_texture, texture_width, texture_height,
                 vertex_bytes + index_bytes);
    upload_image(new_normal_texture, normal_width, normal_height,
                 normal_offset);
    upload_image(new_specular_texture, specular_width, specular_height,
                 specular_offset);

    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fence_info, nullptr, &upload_fence) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    const auto submit_start = Clock::now();
    logDiagnosticApi("vkQueueSubmit.static_upload", "before", std::nullopt,
                     0.0, UINT32_MAX, upload_fence, VK_NULL_HANDLE, command,
                     true, true);
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit, upload_fence);
    logDiagnosticApi(
        "vkQueueSubmit.static_upload", "after", submit_result,
        std::chrono::duration<double, std::milli>(Clock::now() - submit_start)
            .count(),
        UINT32_MAX, upload_fence, VK_NULL_HANDLE, command, true, false);
    if (submit_result != VK_SUCCESS) {
      cleanup();
      return false;
    }
    const auto wait_start = Clock::now();
    logDiagnosticApi("vkWaitForFences.static_upload", "before", std::nullopt,
                     0.0, UINT32_MAX, upload_fence, VK_NULL_HANDLE, command,
                     true, true);
    const VkResult wait_result =
        vkWaitForFences(device_, 1, &upload_fence, VK_TRUE, UINT64_MAX);
    logDiagnosticApi(
        "vkWaitForFences.static_upload", "after", wait_result,
        std::chrono::duration<double, std::milli>(Clock::now() - wait_start)
            .count(),
        UINT32_MAX, upload_fence, VK_NULL_HANDLE, command, true, false);
    if (wait_result != VK_SUCCESS) {
      cleanup();
      return false;
    }
    vkDestroyFence(device_, upload_fence, nullptr);
    upload_fence = VK_NULL_HANDLE;
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
    command = VK_NULL_HANDLE;
    destroyBuffer(staging);

    destroyBuffer(static_model_vbo_);
    destroyBuffer(static_model_ibo_);
    destroyImage(static_texture_);
    destroyImage(static_normal_texture_);
    destroyImage(static_specular_texture_);
    static_model_vbo_ = new_vertex_buffer;
    static_model_ibo_ = new_index_buffer;
    static_texture_ = new_texture;
    static_normal_texture_ = new_normal_texture;
    static_specular_texture_ = new_specular_texture;
    new_vertex_buffer = {};
    new_index_buffer = {};
    new_texture = {};
    new_normal_texture = {};
    new_specular_texture = {};
    static_draw_plan_ = std::move(new_plan);
    static_bone_count_ = mesh.bone_names.size();
    static_vertex_bytes_ = vertex_bytes;
    static_index_bytes_ = index_bytes;
    static_model_ready_ = true;
    static_mismatch_logged_ = false;
    static_generations_.accept(model_generation, texture_generation);
    ++static_resource_rebuilds_;
    uploaded_bytes = static_cast<std::uint64_t>(staging_bytes);
    updateStaticTextureDescriptors();

    // Feed rest-pose triangles and exact per-primitive material metadata into
    // the unified RT scene (positions, normals, UVs, bones, alpha mode).
    if (rt_capability_.device_extensions_enabled) {
      RtRestGeometry rest;
      rest.positions.resize(mesh.vertices.size() * 3);
      rest.normals.resize(mesh.vertices.size() * 3);
      rest.uvs.resize(mesh.vertices.size() * 2);
      rest.tangents.resize(mesh.vertices.size() * 4);
      rest.bone_indices.resize(mesh.vertices.size());
      for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        rest.positions[i * 3 + 0] = mesh.vertices[i].px;
        rest.positions[i * 3 + 1] = mesh.vertices[i].py;
        rest.positions[i * 3 + 2] = mesh.vertices[i].pz;
        rest.normals[i * 3 + 0] = mesh.vertices[i].nx;
        rest.normals[i * 3 + 1] = mesh.vertices[i].ny;
        rest.normals[i * 3 + 2] = mesh.vertices[i].nz;
        rest.uvs[i * 2 + 0] = mesh.vertices[i].u;
        rest.uvs[i * 2 + 1] = mesh.vertices[i].v;
        rest.tangents[i * 4 + 0] = mesh.vertices[i].tx;
        rest.tangents[i * 4 + 1] = mesh.vertices[i].ty;
        rest.tangents[i * 4 + 2] = mesh.vertices[i].tz;
        rest.tangents[i * 4 + 3] = mesh.vertices[i].tangent_handedness;
        rest.bone_indices[i] = mesh.vertices[i].bone_index;
      }
      const RtSceneRecords scene_records = buildRigidModelRtSceneRecords(
          mesh, static_draw_plan_, material);
      if (!scene_records.valid()) {
        writeLog("Vulkan RT scene-record construction failed");
        return false;
      }
      const RtPackedPrimitiveLayout packed_layout =
          buildRtPackedPrimitiveLayout(static_draw_plan_, scene_records);
      if (!packed_layout.valid()) {
        writeLog("Vulkan RT primitive packing failed");
        return false;
      }
      const auto primitive_flags =
          [&](std::size_t primitive) {
        std::uint32_t flags = 0u;
        if (primitive < static_draw_plan_.primitive_materials.size()) {
          const StaticModelPrimitiveMaterial &primitive_material =
              static_draw_plan_.primitive_materials[primitive];
          if (primitive_material.textured) {
            flags |= kRtPrimitiveTextured;
          }
          switch (primitive_material.material) {
          case StaticModelMaterialClass::Cutout:
            flags |= kRtPrimitiveCutout;
            break;
          case StaticModelMaterialClass::Blend:
            flags |= kRtPrimitiveBlend;
            break;
          case StaticModelMaterialClass::Opaque:
          default:
            break;
          }
        }
        return flags;
      };
      rest.indices = packed_layout.indices;
      rest.geometry_ranges = packed_layout.geometry_ranges;
      rest.primitive_flags.reserve(
          packed_layout.source_primitive_indices.size());
      rest.primitive_metadata.reserve(
          packed_layout.source_primitive_indices.size());
      rest.primitive_emission.reserve(
          packed_layout.source_primitive_indices.size());
      for (std::size_t packed_primitive = 0;
           packed_primitive < packed_layout.source_primitive_indices.size();
           ++packed_primitive) {
        const std::uint32_t primitive =
            packed_layout.source_primitive_indices[packed_primitive];
        rest.primitive_flags.push_back(primitive_flags(primitive));
        if (primitive >= scene_records.primitives.size()) {
          writeLog("Vulkan RT primitive metadata packing failed");
          return false;
        }
        const RtPrimitiveRecord &record =
            scene_records.primitives[primitive];
        rest.primitive_metadata.push_back(
            {record.cube_index,
             static_cast<std::uint32_t>(record.face_direction),
             record.material_index, record.primitive_index});
        std::array<float, 3> average_emission{};
        if (material != nullptr && material->valid() &&
            material->specular_map_active &&
            packed_primitive * 3u + 2u < rest.indices.size()) {
          const std::uint32_t i0 = rest.indices[packed_primitive * 3u + 0u];
          const std::uint32_t i1 = rest.indices[packed_primitive * 3u + 1u];
          const std::uint32_t i2 = rest.indices[packed_primitive * 3u + 2u];
          if (i0 < mesh.vertices.size() && i1 < mesh.vertices.size() &&
              i2 < mesh.vertices.size()) {
            const std::array<std::array<float, 2>, 4> uv_samples{{
                {mesh.vertices[i0].u, mesh.vertices[i0].v},
                {mesh.vertices[i1].u, mesh.vertices[i1].v},
                {mesh.vertices[i2].u, mesh.vertices[i2].v},
                {(mesh.vertices[i0].u + mesh.vertices[i1].u +
                  mesh.vertices[i2].u) /
                     3.0f,
                 (mesh.vertices[i0].v + mesh.vertices[i1].v +
                  mesh.vertices[i2].v) /
                     3.0f},
            }};
            for (const auto &uv : uv_samples) {
              const ResolvedMaterialTexel &texel =
                  material->sample(uv[0], uv[1]);
              for (std::size_t channel = 0; channel < 3u; ++channel) {
                average_emission[channel] +=
                    texel.emission_linear[channel] *
                    std::clamp(texel.opacity, 0.0f, 1.0f) * 0.25f;
              }
            }
          }
        }
        rest.primitive_emission.push_back(average_emission);
      }
      for (std::size_t i = 0; i < rt_scenes_.size(); ++i) {
        if (i + 1u == rt_scenes_.size()) {
          rt_scenes_[i].setRestGeometry(std::move(rest));
        } else {
          rt_scenes_[i].setRestGeometry(rest);
        }
      }
      rt_scene_built_.fill(false);
      last_rt_scene_hash_.fill(0);
    }
    return true;
  }

  void readCompletedTimestamps(FrameSync &frame) {
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

  bool pickDevice() {
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

  bool supportsDeviceExtension(VkPhysicalDevice device,
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

  bool supportsRequiredDeviceExtensions(VkPhysicalDevice device) const {
    return supportsDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

  [[nodiscard]] static bool
  deviceHasExtension(const std::vector<VkExtensionProperties> &props,
                     const char *name) {
    return std::any_of(props.begin(), props.end(), [&](const auto &p) {
      return std::strcmp(p.extensionName, name) == 0;
    });
  }

  // Probe NVIDIA + Vulkan RT extension/feature support on the selected GPU.
  // Mirrors Kickstart RT / VK RT device requirements used by NVIDIA RT SDK.
  void probeRayTracingCapability() {
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

  bool createDevice() {
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

  bool querySupport(VkPhysicalDevice device, SwapchainSupport &s) const {
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

  bool createSwapchain(
      VkSwapchainKHR old_swapchain = VK_NULL_HANDLE,
      SwapchainOwnership target_ownership = SwapchainOwnership::Native) {
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
      destroySwapchainObjects();
      return false;
    }
    swap_images_.resize(ic);
    images_result =
        streamline_vulkan_runtime_.getSwapchainImages(
            device_, swapchain_, &ic, swap_images_.data());
    if (images_result != VK_SUCCESS || ic == 0) {
      SDL_Log("Vulkan swapchain image query failed: %d",
              static_cast<int>(images_result));
      destroySwapchainObjects();
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
        destroySwapchainObjects();
        return false;
      }

      SwapchainImageResource &resource = swap_image_resources_[i];
      result = vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                 &resource.render_finished);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan present semaphore creation failed: %d",
                static_cast<int>(result));
        destroySwapchainObjects();
        return false;
      }
      if (presentFenceLifecycleEnabled()) {
        result = vkCreateFence(device_, &present_fence_info, nullptr,
                               &resource.present_fence);
        if (result != VK_SUCCESS) {
          SDL_Log("Vulkan present fence creation failed: %d",
                  static_cast<int>(result));
          destroySwapchainObjects();
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
        destroySwapchainObjects();
        return false;
      }
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(device_, resource.depth_image,
                                   &requirements);
      const auto memory_type = findMemoryType(
          requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!memory_type) {
        writeLog("Vulkan depth image has no compatible memory type");
        destroySwapchainObjects();
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
        destroySwapchainObjects();
        return false;
      }
      result = vkBindImageMemory(device_, resource.depth_image,
                                 resource.depth_memory, 0);
      if (result != VK_SUCCESS) {
        SDL_Log("Vulkan depth memory bind failed: %d",
                static_cast<int>(result));
        destroySwapchainObjects();
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
        destroySwapchainObjects();
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

  bool waitForPendingPresentFences(const char *reason) {
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

  void destroySwapchainImageObjects() {
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

  void destroySwapchainObjects() {
    if (swapchain_ != VK_NULL_HANDLE ||
        streamline_vulkan_runtime_.swapchainOwnership() ==
            SwapchainOwnership::StreamlineFrameGenerationProxy) {
      streamline_vulkan_runtime_.notifyFrameGenerationSwapchainDestroyed(
          static_cast<std::uint64_t>(frame_index_),
          "swapchain destroyed");
    }
    destroySwapchainImageObjects();
    if (swapchain_) {
      streamline_vulkan_runtime_.destroySwapchain(
          device_, swapchain_, nullptr);
      swapchain_ = VK_NULL_HANDLE;
    }
    swapchain_recreate_target_ = SwapchainOwnership::Native;
  }

  bool recreateSwapchain() {
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
    if (!streamline_vulkan_runtime_.beginFrameGenerationSwapchainTransition(
            target_ownership, transition_frame, transition_reason)) {
      fg_force_native_recovery_ = true;
      swapchain_recreate_target_ = SwapchainOwnership::Native;
      return false;
    }

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
      fatal_error_ = true;
      return false;
    }
    // Proxy presents do not expose an application maintenance1 fence. Native
    // fences are still drained before their swapchain is destroyed.
    if (!waitForPendingPresentFences("swapchain_recreate")) {
      fatal_error_ = true;
      return false;
    }

    VkSwapchainKHR old_swapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;
    destroySwapchainImageObjects();
    streamline_vulkan_runtime_.notifyFrameGenerationSwapchainDestroyed(
        transition_frame, "old swapchain destroyed");
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
          fatal_error_ = true;
          return false;
        }
      } else {
        xpbd::log::error(
            "DLSS Frame Generation could not reach a safe Native state");
        fatal_error_ = true;
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
      destroySwapchainObjects();
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
      destroySwapchainObjects();
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
        destroySwapchainObjects();
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
      destroySwapchainObjects();
      return false;
    }
    actual_ownership = streamline_vulkan_runtime_.swapchainOwnership();
    streamline_vulkan_runtime_.completeFrameGenerationSwapchainTransition(
        actual_ownership,
        actual_ownership == SwapchainOwnership::StreamlineFrameGenerationProxy &&
        fg_swapchain_resources_ready_,
        transition_frame, transition_reason);
    if (actual_ownership == SwapchainOwnership::Native) {
      // A forced recovery is consumed only after a complete Native rebuild.
      fg_force_native_recovery_ = false;
      fg_recovery_reason_.clear();
    }
    return true;
  }

  bool createRenderPass() {
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

  bool createFramebuffers() {
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

  bool createDescriptors() {
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
    static_sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    static_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    static_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    static_sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    static_sampler_info.minLod = 0.0f;
    static_sampler_info.maxLod = 0.0f;
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

  bool uploadSkyboxCubemap(const PreviewSkybox &sky) {
    if (!sky.valid() || !device_ || !cmd_pool_ || !graphics_queue_) {
      skybox_ready_ = false;
      return false;
    }
    if (skybox_ready_ && skybox_generation_ == sky.generation &&
        skybox_face_size_ == static_cast<std::uint32_t>(sky.face_size) &&
        skybox_cubemap_.view) {
      return true;
    }

    const std::uint32_t face = static_cast<std::uint32_t>(sky.face_size);
    const VkDeviceSize face_bytes =
        static_cast<VkDeviceSize>(face) * face * 4u;
    const VkDeviceSize total_bytes = face_bytes * 6u;

    // Create cubemap image with correct memory type.
    destroyImage(skybox_cubemap_);
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {face, face, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 6;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkResult create_result =
        vkCreateImage(device_, &image_info, nullptr, &skybox_cubemap_.image);
    if (create_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImage", create_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage, total_bytes,
          (std::numeric_limits<std::uint32_t>::max)());
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, skybox_cubemap_.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    const auto memory_type = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      destroyImage(skybox_cubemap_);
      return false;
    }
    allocation.memoryTypeIndex = *memory_type;
    const VkResult allocation_result = vkAllocateMemory(
        device_, &allocation, nullptr, &skybox_cubemap_.memory);
    if (allocation_result != VK_SUCCESS) {
      logImageResourceError(
          "vkAllocateMemory", allocation_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage,
          requirements.size, *memory_type);
      destroyImage(skybox_cubemap_);
      return false;
    }
    const VkResult bind_result = vkBindImageMemory(
        device_, skybox_cubemap_.image, skybox_cubemap_.memory, 0);
    if (bind_result != VK_SUCCESS) {
      logImageResourceError(
          "vkBindImageMemory", bind_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage,
          requirements.size, *memory_type);
      destroyImage(skybox_cubemap_);
      return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = skybox_cubemap_.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    const VkResult view_result = vkCreateImageView(
        device_, &view_info, nullptr, &skybox_cubemap_.view);
    if (view_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImageView", view_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage,
          requirements.size, *memory_type);
      destroyImage(skybox_cubemap_);
      return false;
    }
    skybox_cubemap_.width = face;
    skybox_cubemap_.height = face;

    Buffer staging{};
    if (!createBuffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, "preview-skybox-staging")) {
      destroyImage(skybox_cubemap_);
      return false;
    }
    std::memcpy(staging.mapped, sky.rgba.data(),
                static_cast<std::size_t>(total_bytes));

    if (!skybox_vbo_.buffer) {
      if (!createBuffer(sizeof(kSkyboxCubePositions),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        skybox_vbo_) ||
          !uploadBuffer(skybox_vbo_, 0, sizeof(kSkyboxCubePositions),
                        kSkyboxCubePositions)) {
        destroyBuffer(staging);
        destroyImage(skybox_cubemap_);
        return false;
      }
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) {
      destroyBuffer(staging);
      destroyImage(skybox_cubemap_);
      return false;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult begin_result = vkBeginCommandBuffer(cmd, &bi);
    if (begin_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Preview skybox command begin failed: API=vkBeginCommandBuffer "
          "VkResult=%s(%d) frame_slot=%u still_job_id=%llu",
          vkResultName(begin_result), static_cast<int>(begin_result),
          frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      destroyImage(skybox_cubemap_);
      return false;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = skybox_cubemap_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    std::array<VkBufferImageCopy, 6> regions{};
    for (std::uint32_t layer = 0; layer < 6; ++layer) {
      regions[layer].bufferOffset = face_bytes * layer;
      regions[layer].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer,
                                         1};
      regions[layer].imageExtent = {face, face, 1};
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, skybox_cubemap_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6,
                           regions.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    const VkResult end_result = vkEndCommandBuffer(cmd);
    if (end_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Preview skybox command end failed: API=vkEndCommandBuffer "
          "VkResult=%s(%d) frame_slot=%u still_job_id=%llu",
          vkResultName(end_result), static_cast<int>(end_result),
          frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      destroyImage(skybox_cubemap_);
      return false;
    }

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    if (submit_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Preview skybox submit failed: API=vkQueueSubmit "
          "VkResult=%s(%d) extent=%ux%u frame_slot=%u still_job_id=%llu",
          vkResultName(submit_result), static_cast<int>(submit_result), face,
          face, frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueSubmit(preview_skybox)",
                               submit_result);
      }
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      destroyImage(skybox_cubemap_);
      return false;
    }
    const VkResult wait_result = vkQueueWaitIdle(graphics_queue_);
    if (wait_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Preview skybox wait failed: API=vkQueueWaitIdle "
          "VkResult=%s(%d) extent=%ux%u frame_slot=%u still_job_id=%llu",
          vkResultName(wait_result), static_cast<int>(wait_result), face,
          face, frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueWaitIdle(preview_skybox)",
                               wait_result);
      }
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      destroyImage(skybox_cubemap_);
      return false;
    }
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    destroyBuffer(staging);

    VkDescriptorImageInfo desc_image{};
    desc_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    desc_image.imageView = skybox_cubemap_.view;
    desc_image.sampler = skybox_sampler_;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = skybox_desc_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &desc_image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    skybox_face_size_ = face;
    skybox_generation_ = sky.generation;
    skybox_ready_ = true;
    return true;
  }

  VkShaderModule makeModule(const uint32_t *words, size_t word_count) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = word_count * 4;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return m;
  }

  bool createPipelines();
  bool createBuffers() {

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

  bool createCommandPool() {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = graphics_family_;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(device_, &pi, nullptr, &cmd_pool_));
    return true;
  }

  bool createSync() {
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

  void createTimestampQueryPools() {
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

  int drawUi(FrameSync &frame, const UiDrawData &ui, bool overlay_only);
};




bool VulkanBackend::createPipelines() {
  VkShaderModule ui_vs = makeModule(kSpvUiVert, sizeof(kSpvUiVert) / 4);
  VkShaderModule ui_fs = makeModule(kSpvUiFrag, sizeof(kSpvUiFrag) / 4);
  VkShaderModule mesh_vs = makeModule(kSpvMeshVert, sizeof(kSpvMeshVert) / 4);
  VkShaderModule mesh_fs = makeModule(kSpvMeshFrag, sizeof(kSpvMeshFrag) / 4);
  VkShaderModule static_mesh_vs =
      makeModule(kSpvStaticMeshVert, sizeof(kSpvStaticMeshVert) / 4);
  VkShaderModule static_mesh_fs =
      makeModule(kSpvStaticMeshFrag, sizeof(kSpvStaticMeshFrag) / 4);
  VkShaderModule sky_vs =
      makeModule(kSpvSkyboxVert, sizeof(kSpvSkyboxVert) / 4);
  VkShaderModule sky_fs =
      makeModule(kSpvSkyboxFrag, sizeof(kSpvSkyboxFrag) / 4);
  auto destroy_modules = [&] {
    if (ui_vs) {
      vkDestroyShaderModule(device_, ui_vs, nullptr);
    }
    if (ui_fs) {
      vkDestroyShaderModule(device_, ui_fs, nullptr);
    }
    if (mesh_vs) {
      vkDestroyShaderModule(device_, mesh_vs, nullptr);
    }
    if (mesh_fs) {
      vkDestroyShaderModule(device_, mesh_fs, nullptr);
    }
    if (static_mesh_vs) {
      vkDestroyShaderModule(device_, static_mesh_vs, nullptr);
    }
    if (static_mesh_fs) {
      vkDestroyShaderModule(device_, static_mesh_fs, nullptr);
    }
    if (sky_vs) {
      vkDestroyShaderModule(device_, sky_vs, nullptr);
    }
    if (sky_fs) {
      vkDestroyShaderModule(device_, sky_fs, nullptr);
    }
  };
  auto fail = [&](const char *message) {
    if (message) {
      writeLog(message);
    }
    destroy_modules();
    destroyGraphicsPipelines();
    return false;
  };
  if (!ui_vs || !ui_fs || !mesh_vs || !mesh_fs || !static_mesh_vs ||
      !static_mesh_fs || !sky_vs || !sky_fs) {
    return fail("SPIR-V module create failed");
  }


  VkPushConstantRange ui_pc{};
  ui_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  ui_pc.offset = 0;
  ui_pc.size = sizeof(float) * 4;

  VkPipelineLayoutCreateInfo ul{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ul.setLayoutCount = 1;
  ul.pSetLayouts = &desc_layout_;
  ul.pushConstantRangeCount = 1;
  ul.pPushConstantRanges = &ui_pc;
  if (vkCreatePipelineLayout(device_, &ul, nullptr, &ui_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan UI pipeline-layout creation failed");
  }

  VkPushConstantRange mesh_pc{};
  mesh_pc.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  mesh_pc.offset = 0;
  mesh_pc.size = sizeof(MeshScenePushConstants);
  VkPipelineLayoutCreateInfo ml{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ml.pushConstantRangeCount = 1;
  ml.pPushConstantRanges = &mesh_pc;
  if (vkCreatePipelineLayout(device_, &ml, nullptr, &mesh_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan mesh pipeline-layout creation failed");
  }

  ml.setLayoutCount = 1;
  ml.pSetLayouts = &static_desc_layout_;
  if (vkCreatePipelineLayout(device_, &ml, nullptr, &static_mesh_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan static-mesh pipeline-layout creation failed");
  }

  VkPushConstantRange sky_pc{};
  sky_pc.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  sky_pc.offset = 0;
  sky_pc.size = sizeof(SkyboxPushConstants);
  VkPipelineLayoutCreateInfo sl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  sl.setLayoutCount = 1;
  sl.pSetLayouts = &skybox_desc_layout_;
  sl.pushConstantRangeCount = 1;
  sl.pPushConstantRanges = &sky_pc;
  if (vkCreatePipelineLayout(device_, &sl, nullptr, &skybox_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan skybox pipeline-layout creation failed");
  }

  auto makePipe = [&](VkShaderModule vs, VkShaderModule fs,
                      VkPipelineLayout layout, bool ui, bool static_mesh,
                      bool lines, bool mesh_trans, bool overlay_lines,
                      bool temporal_hud_lines,
                      VkPipeline *out) -> bool {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attrs;
    if (ui) {
      bind.stride = sizeof(NkVertex);
      attrs = {
          {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NkVertex, pos)},
          {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NkVertex, uv)},
          {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(NkVertex, col)},
      };
    } else if (static_mesh) {
      bind.stride = sizeof(StaticGpuVertex);
      attrs = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticGpuVertex, px)},
          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticGpuVertex, nx)},
          {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(StaticGpuVertex, u)},
          {3, 0, VK_FORMAT_R32_UINT, offsetof(StaticGpuVertex, bone_index)},
          {4, 0, VK_FORMAT_R32_UINT, offsetof(StaticGpuVertex, flags)},
          {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
           offsetof(StaticGpuVertex, tx)},
      };
    } else {
      bind.stride = sizeof(MeshVertex);
      attrs = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, px)},
          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, nx)},
          {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, r)},
      };
    }
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    if (overlay_lines) {
      // Selection outlines sit just outside their source cubes. Pull them a
      // minimal amount toward the camera to tolerate depth quantization while
      // retaining occlusion by genuinely foreground geometry.
      rs.depthBiasEnable = VK_TRUE;
      rs.depthBiasConstantFactor = -1.0f;
      rs.depthBiasSlopeFactor = -1.0f;
    }

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = (ui || temporal_hud_lines) ? VK_FALSE : VK_TRUE;
    ds.depthWriteEnable =
        (ui || mesh_trans || overlay_lines || temporal_hud_lines) ? VK_FALSE
                                                                  : VK_TRUE;
    ds.depthCompareOp = overlay_lines ? VK_COMPARE_OP_LESS_OR_EQUAL
                                      : VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    if (ui || mesh_trans || temporal_hud_lines) {
      blend.blendEnable = VK_TRUE;
      blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend.colorBlendOp = VK_BLEND_OP_ADD;
      blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend.alphaBlendOp = VK_BLEND_OP_ADD;
    } else {
      blend.blendEnable = VK_FALSE;
    }
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    std::array<VkDynamicState, 2> dyn = {VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn.data();

    VkGraphicsPipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dsi;
    pi.layout = layout;
    pi.renderPass = render_pass_;
    pi.subpass = 0;
    const VkResult result = vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pi, nullptr, out);
    if (result != VK_SUCCESS) {
      SDL_Log("Vulkan graphics-pipeline creation failed: %d",
              static_cast<int>(result));
      return false;
    }
    return true;
  };

  if (!makePipe(ui_vs, ui_fs, ui_layout_, true, false, false, false, false,
                 false, &ui_pipeline_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, false, false,
                 false, false,
                 &mesh_pipeline_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, false, true,
                 false, false,
                 &mesh_pipeline_trans_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, true, false,
                 false, false,
                 &mesh_pipeline_lines_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, true, false,
                 true, false, &mesh_pipeline_overlay_lines_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, true, false,
                 false, true, &mesh_pipeline_temporal_hud_lines_) ||
      !makePipe(static_mesh_vs, static_mesh_fs, static_mesh_layout_, false,
                 true, false, false, false, false,
                 &static_mesh_pipeline_) ||
      !makePipe(static_mesh_vs, static_mesh_fs, static_mesh_layout_, false,
                 true, false, true, false, false,
                 &static_mesh_pipeline_blend_)) {
    return fail("Vulkan graphics-pipeline bundle creation failed");
  }

  // Hybrid RT pipelines (ray-query shadows) when NVIDIA RT is armed.
  rt_pipelines_ready_ = false;
  if (rt_capability_.device_extensions_enabled && mesh_rt_desc_layout_ &&
      static_rt_desc_layout_) {
    VkShaderModule mesh_rt_vs =
        makeModule(kSpvMeshRtVert, sizeof(kSpvMeshRtVert) / 4);
    VkShaderModule mesh_rt_fs =
        makeModule(kSpvMeshRtFrag, sizeof(kSpvMeshRtFrag) / 4);
    VkShaderModule static_rt_vs =
        makeModule(kSpvStaticMeshRtVert, sizeof(kSpvStaticMeshRtVert) / 4);
    VkShaderModule static_rt_fs =
        makeModule(kSpvStaticMeshRtFrag, sizeof(kSpvStaticMeshRtFrag) / 4);
    if (!mesh_rt_vs || !mesh_rt_fs || !static_rt_vs || !static_rt_fs) {
      if (mesh_rt_vs)
        vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
      if (mesh_rt_fs)
        vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
      if (static_rt_vs)
        vkDestroyShaderModule(device_, static_rt_vs, nullptr);
      if (static_rt_fs)
        vkDestroyShaderModule(device_, static_rt_fs, nullptr);
      return fail("Vulkan RT shader module creation failed");
    }

    VkPushConstantRange mesh_pc_rt{};
    mesh_pc_rt.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    mesh_pc_rt.offset = 0;
    mesh_pc_rt.size = sizeof(MeshScenePushConstants);

    VkPipelineLayoutCreateInfo ml_rt{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ml_rt.setLayoutCount = 1;
    ml_rt.pSetLayouts = &mesh_rt_desc_layout_;
    ml_rt.pushConstantRangeCount = 1;
    ml_rt.pPushConstantRanges = &mesh_pc_rt;
    if (vkCreatePipelineLayout(device_, &ml_rt, nullptr, &mesh_rt_layout_) !=
        VK_SUCCESS) {
      vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
      vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
      vkDestroyShaderModule(device_, static_rt_vs, nullptr);
      vkDestroyShaderModule(device_, static_rt_fs, nullptr);
      return fail("Vulkan mesh RT pipeline-layout creation failed");
    }
    ml_rt.pSetLayouts = &static_rt_desc_layout_;
    if (vkCreatePipelineLayout(device_, &ml_rt, nullptr, &static_rt_layout_) !=
        VK_SUCCESS) {
      vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
      vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
      vkDestroyShaderModule(device_, static_rt_vs, nullptr);
      vkDestroyShaderModule(device_, static_rt_fs, nullptr);
      return fail("Vulkan static-mesh RT pipeline-layout creation failed");
    }

    const bool rt_ok =
        makePipe(mesh_rt_vs, mesh_rt_fs, mesh_rt_layout_, false, false, false,
                 false, false, false, &mesh_rt_pipeline_) &&
        makePipe(mesh_rt_vs, mesh_rt_fs, mesh_rt_layout_, false, false, false,
                 true, false, false, &mesh_rt_pipeline_trans_) &&
        makePipe(static_rt_vs, static_rt_fs, static_rt_layout_, false, true,
                 false, false, false, false, &static_rt_pipeline_) &&
        makePipe(static_rt_vs, static_rt_fs, static_rt_layout_, false, true,
                 false, true, false, false, &static_rt_pipeline_blend_);
    vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
    vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
    vkDestroyShaderModule(device_, static_rt_vs, nullptr);
    vkDestroyShaderModule(device_, static_rt_fs, nullptr);
    if (!rt_ok) {
      return fail("Vulkan RT graphics-pipeline bundle creation failed");
    }
    rt_pipelines_ready_ = true;
    writeLog("Vulkan hybrid RT shadow pipelines ready");
  }

  // Skybox pipeline: position-only verts, depth == far, no depth write.
  {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sky_vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = sky_fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(float) * 3;
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{};
    attr.location = 0;
    attr.binding = 0;
    attr.format = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset = 0;
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // Both windings: skybox is a unit cube sampled from the inside.
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    blend.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    std::array<VkDynamicState, 2> dyn = {VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn.data();

    VkGraphicsPipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dsi;
    pi.layout = skybox_layout_;
    pi.renderPass = render_pass_;
    pi.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr,
                                  &skybox_pipeline_) != VK_SUCCESS) {
      return fail("Vulkan skybox pipeline creation failed");
    }
  }

  destroy_modules();
  return true;
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
  return std::make_unique<VulkanBackend>();
}

}
