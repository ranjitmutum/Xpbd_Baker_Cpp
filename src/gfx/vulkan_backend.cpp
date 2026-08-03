


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

}

namespace detail {

#include "vulkan/vulkan_backend_internal.hpp"




#include "vulkan_backend_parts/vulkan_backend_pipelines.inc"

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
