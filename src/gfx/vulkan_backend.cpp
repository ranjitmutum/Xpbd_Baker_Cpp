


#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"
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
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
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
    writeLog("VulkanBackend::init");

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

    if (diagnostics_enabled_) {
      xpbd::log::infof(
          "VKDIAG config ts_us=%llu thread=%llu enabled=1 "
          "application_enabled_layers=0 instance_extensions=%u wait_slice_ms=250",
          static_cast<unsigned long long>(diagnosticTimestampUs()),
          static_cast<unsigned long long>(diagnosticThreadId()),
          static_cast<unsigned>(instance_exts.size()));
      for (const char *extension : instance_exts) {
        xpbd::log::infof("VKDIAG instance_extension name=%s",
                         extension != nullptr ? extension : "<null>");
      }
      std::uint32_t layer_count = 0;
      VkResult layer_result =
          vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
      std::vector<VkLayerProperties> layers;
      if (layer_result == VK_SUCCESS && layer_count > 0) {
        layers.resize(layer_count);
        layer_result =
            vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
      }
      xpbd::log::infof(
          "VKDIAG available_instance_layers result=%s(%d) count=%u",
          vkResultName(layer_result), static_cast<int>(layer_result),
          layer_result == VK_SUCCESS ? layer_count : 0u);
      if (layer_result == VK_SUCCESS) {
        for (const auto &layer : layers) {
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
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));

    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
      writeLog(SDL_GetError());
      return false;
    }

    if (!pickDevice()) {
      return false;
    }
    if (!createDevice()) {
      return false;
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
    if (!createDescriptors()) {
      return false;
    }
    if (!createPipelines()) {
      return false;
    }
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
      const FrameSync &sync = frames_[frame_index_];
      const auto idle_start = Clock::now();
      logDiagnosticApi("vkDeviceWaitIdle.shutdown", "before", std::nullopt,
                       0.0, UINT32_MAX, sync.fence, VK_NULL_HANDLE, sync.cmd,
                       true, true);
      const VkResult idle_result = vkDeviceWaitIdle(device_);
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
    if (static_sampler_) {
      vkDestroySampler(device_, static_sampler_, nullptr);
      static_sampler_ = VK_NULL_HANDLE;
    }
    if (static_desc_pool_) {
      vkDestroyDescriptorPool(device_, static_desc_pool_, nullptr);
      static_desc_pool_ = VK_NULL_HANDLE;
    }
    if (static_desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, static_desc_layout_, nullptr);
      static_desc_layout_ = VK_NULL_HANDLE;
    }
    if (desc_pool_) {
      vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    }
    if (desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, desc_layout_, nullptr);
    }
    destroyGraphicsPipelines();
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
    if (device_) {
      vkDestroyDevice(device_, nullptr);
    }
    if (surface_) {
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_) {
      vkDestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
  }

  void resize(int , int ) override { recreate_swapchain_ = true; }

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

  void render(const FrameInput &frame) override {
    const auto t0 = Clock::now();
    diagnostic_context_ = frame.diagnostics;
    diagnostic_trace_frame_ =
        diagnostics_enabled_ && frame.diagnostics.active;
    if (fatal_error_ || !device_) {
      return;
    }
    FrameSync &fs = frames_[frame_index_];
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
      return;
    }

    readCompletedTimestamps(fs);
    stats_.backend_cpu_ms = 0.0f;

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
    const bool static_input =
        frame.static_model != nullptr && frame.static_model_frame != nullptr;
    const DynamicMeshUploadLayout mesh_upload =
        draw_mesh ? makeDynamicMeshUploadLayout(frame.scene->solid.size(),
                                                frame.scene->transparent.size(),
                                                frame.scene->lines.size())
                  : DynamicMeshUploadLayout{};

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

    if (static_input &&
        static_generations_.needsRefresh(frame.static_model_generation,
                                         frame.static_texture_generation)) {
      const auto idle_start = Clock::now();
      logDiagnosticApi("vkDeviceWaitIdle.static_rebuild", "before",
                       std::nullopt, 0.0, UINT32_MAX, fs.fence, VK_NULL_HANDLE,
                       fs.cmd, true, true);
      const VkResult idle_result = vkDeviceWaitIdle(device_);
      logDiagnosticApi(
          "vkDeviceWaitIdle.static_rebuild", "after", idle_result,
          std::chrono::duration<double, std::milli>(Clock::now() - idle_start)
              .count(),
          UINT32_MAX, fs.fence, VK_NULL_HANDLE, fs.cmd, true, false);
      if (idle_result != VK_SUCCESS) {
        SDL_Log("Vulkan static resource idle wait failed: %d",
                static_cast<int>(idle_result));
        return;
      }
      std::uint64_t resource_upload_bytes = 0;
      if (!rebuildStaticModelResources(
              *frame.static_model, frame.static_model_texture,
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
    const bool draw_viewport = valid_viewport && (draw_mesh || static_input);

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

    if (draw_mesh) {
      if (!ensure_owned_buffer(
              fs.mesh_vbo, static_cast<VkDeviceSize>(mesh_upload.total_bytes),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
          !uploadBuffer(
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
      stats_.mesh_upload_bytes += mesh_upload.total_bytes;
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
      acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                  fs.image_available, VK_NULL_HANDLE,
                                  &image_index);
    } else {
      do {
        acq = vkAcquireNextImageKHR(device_, swapchain_,
                                    kDiagnosticWaitSliceNs,
                                    fs.image_available, VK_NULL_HANDLE,
                                    &image_index);
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
      return;
    }
    if (acq == VK_SUBOPTIMAL_KHR) {


      recreate_swapchain_ = true;
    } else if (acq != VK_SUCCESS) {
      SDL_Log("Vulkan acquire failed: %d", static_cast<int>(acq));
      fatal_error_ = true;
      return;
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
    if (swapchain_maintenance1_enabled_ &&
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
        fatal_error_ = true;
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

    if (swapchain_maintenance1_enabled_ &&
        image_resource.present_pending) {
      const VkResult present_wait =
          wait_for_fence(image_resource.present_fence,
                         "vkWaitForFences.present_reuse", image_index,
                         image_resource.last_in_flight);
      if (present_wait != VK_SUCCESS) {
        SDL_Log("Vulkan present fence wait before reuse failed: %d",
                static_cast<int>(present_wait));
        fatal_error_ = true;
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
        fatal_error_ = true;
        return;
      }
    }

    const VkResult reset_fence = call_with_diagnostics(
        "vkResetFences", [&] { return vkResetFences(device_, 1, &fs.fence); });
    if (reset_fence != VK_SUCCESS) {
      SDL_Log("Vulkan fence reset failed: %d",
              static_cast<int>(reset_fence));
      fatal_error_ = true;
      return;
    }
    const VkResult reset_command = call_with_diagnostics(
        "vkResetCommandBuffer",
        [&] { return vkResetCommandBuffer(fs.cmd, 0); });
    if (reset_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer reset failed: %d",
              static_cast<int>(reset_command));
      fatal_error_ = true;
      return;
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    const VkResult begin_command = call_with_diagnostics(
        "vkBeginCommandBuffer",
        [&] { return vkBeginCommandBuffer(fs.cmd, &bi); });
    if (begin_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer begin failed: %d",
              static_cast<int>(begin_command));
      fatal_error_ = true;
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
    if (draw_ui) {
      stats_.ui_commands = drawUi(fs, *frame.ui, false);
      draws += stats_.ui_commands;
    }
    write_timestamp(GpuTimestampQuery::UiEnd,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);


    if (draw_viewport) {
      VkViewport vp{};
      vp.x = static_cast<float>(safe_viewport.x);
      vp.y = static_cast<float>(safe_viewport.y);
      vp.width = static_cast<float>(safe_viewport.w);
      vp.height = static_cast<float>(safe_viewport.h);
      vp.minDepth = 0.0f;
      vp.maxDepth = 1.0f;
      VkRect2D sc{{safe_viewport.x, safe_viewport.y},
                  {static_cast<uint32_t>(safe_viewport.w),
                   static_cast<uint32_t>(safe_viewport.h)}};
      vkCmdSetViewport(fs.cmd, 0, 1, &vp);
      vkCmdSetScissor(fs.cmd, 0, 1, &sc);


      {
        VkClearAttachment cas[2]{};
        cas[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cas[0].colorAttachment = 0;
        cas[0].clearValue.color = {
            {30.0f / 255.0f, 30.0f / 255.0f, 40.0f / 255.0f, 1.0f}};
        cas[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        cas[1].clearValue.depthStencil = {1.0f, 0};
        VkClearRect cr{};
        cr.rect = sc;
        cr.baseArrayLayer = 0;
        cr.layerCount = 1;
        vkCmdClearAttachments(fs.cmd, 2, cas, 1, &cr);
      }

      float mvp_gl[16];
      mulMat(frame.proj_matrix, frame.view_matrix, mvp_gl);
      float mvp[16];

      glMvpToVulkan(mvp_gl, mvp);

      if (draw_static && (static_draw_plan_.opaque.index_count > 0 ||
                          static_draw_plan_.cutout.index_count > 0)) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          static_mesh_pipeline_);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                static_mesh_layout_, 0, 1,
                                &fs.static_descriptor_set, 0, nullptr);
        vkCmdPushConstants(fs.cmd, static_mesh_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp);
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

      if (draw_mesh && !frame.scene->solid.empty()) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipeline_);
        vkCmdPushConstants(fs.cmd, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(mvp), mvp);
        const VkDeviceSize off = mesh_upload.solid.offset_bytes;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd, static_cast<uint32_t>(frame.scene->solid.size()), 1,
                  0, 0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::OpaqueEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      if (draw_static && static_draw_plan_.blend.index_count > 0) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          static_mesh_pipeline_blend_);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                static_mesh_layout_, 0, 1,
                                &fs.static_descriptor_set, 0, nullptr);
        vkCmdPushConstants(fs.cmd, static_mesh_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &static_model_vbo_.buffer,
                               &offset);
        vkCmdBindIndexBuffer(fs.cmd, static_model_ibo_.buffer, 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(fs.cmd, static_draw_plan_.blend.index_count, 1,
                         static_draw_plan_.blend.first_index, 0, 0);
        ++draws;
      }
      if (draw_mesh && !frame.scene->transparent.empty() &&
          mesh_pipeline_trans_) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipeline_trans_);
        vkCmdPushConstants(fs.cmd, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(mvp), mvp);
        const VkDeviceSize off = mesh_upload.transparent.offset_bytes;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd,
                  static_cast<uint32_t>(frame.scene->transparent.size()), 1, 0,
                  0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::TransparentEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      if (draw_mesh && !frame.scene->lines.empty()) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipeline_lines_);
        vkCmdPushConstants(fs.cmd, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(mvp), mvp);
        const VkDeviceSize off = mesh_upload.lines.offset_bytes;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd, static_cast<uint32_t>(frame.scene->lines.size()), 1,
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
    if (draw_ui && draw_viewport && frame.ui->overlay_visible) {
      const int overlay_commands = drawUi(fs, *frame.ui, true);
      stats_.ui_commands += overlay_commands;
      draws += overlay_commands;
    }

    stats_.draw_calls = draws;
    if (diagnostics_enabled_ && diagnostic_trace_frame_) {
      xpbd::log::infof(
          "VKDIAG command ts_us=%llu thread=%llu frame=%llu slot=%zu "
          "image=%u draw_calls=%d upload=%llu static_bone_upload=%llu "
          "static_resource_upload=%llu",
          static_cast<unsigned long long>(diagnosticTimestampUs()),
          static_cast<unsigned long long>(diagnosticThreadId()),
          static_cast<unsigned long long>(frame.diagnostics.render_frame),
          frame_index_, image_index, draws,
          static_cast<unsigned long long>(stats_.upload_bytes),
          static_cast<unsigned long long>(stats_.static_bone_upload_bytes),
          static_cast<unsigned long long>(
              stats_.static_resource_upload_bytes));
    }

    vkCmdEndRenderPass(fs.cmd);
    write_timestamp(GpuTimestampQuery::FrameEnd,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    const VkResult end_command = call_with_diagnostics(
        "vkEndCommandBuffer",
        [&] { return vkEndCommandBuffer(fs.cmd); });
    if (end_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer end failed: %d",
              static_cast<int>(end_command));
      fatal_error_ = true;
      return;
    }

    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &fs.image_available;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fs.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &image_resource.render_finished;
    const VkResult submit_result = call_with_diagnostics(
        "vkQueueSubmit",
        [&] { return vkQueueSubmit(graphics_queue_, 1, &si, fs.fence); });
    if (submit_result != VK_SUCCESS) {
      SDL_Log("Vulkan queue submit failed: %d",
              static_cast<int>(submit_result));
      fatal_error_ = true;
      return;
    }
    image_resource.last_in_flight = fs.fence;
    fs.timestamps_pending = timestamp_queries_enabled_ && fs.timestamp_pool;



    stats_.backend_cpu_ms =
        std::chrono::duration<float, std::milli>(Clock::now() - t0).count();

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &image_resource.render_finished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;
    VkSwapchainPresentFenceInfoKHR present_fence_info{
        VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR};
    if (swapchain_maintenance1_enabled_) {
      present_fence_info.swapchainCount = 1;
      present_fence_info.pFences = &image_resource.present_fence;
      pi.pNext = &present_fence_info;
    }
    const auto present_start = Clock::now();
    logDiagnosticApi("vkQueuePresentKHR", "before", std::nullopt, 0.0,
                     image_index, fs.fence, image_resource.last_in_flight,
                     fs.cmd, false, true);
    VkResult pr = vkQueuePresentKHR(present_queue_, &pi);
    if (swapchain_maintenance1_enabled_) {
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
      SDL_Log("Vulkan present failed: %d", static_cast<int>(pr));
      fatal_error_ = true;
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
  }
  bool vsyncEnabled() const override { return vsync_; }
  BackendKind kind() const override { return BackendKind::Vulkan; }
  const char *name() const override { return "Vulkan"; }
  const char *deviceName() const override { return device_name_.c_str(); }
  FrameStats stats() const override { return stats_; }

private:
  struct StaticGpuVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
    std::uint32_t bone_index = 0;
    std::uint32_t flags = 0;
  };
  static_assert(sizeof(StaticGpuVertex) == 40);
  static_assert(offsetof(StaticGpuVertex, px) == 0);
  static_assert(offsetof(StaticGpuVertex, nx) == 12);
  static_assert(offsetof(StaticGpuVertex, u) == 24);
  static_assert(offsetof(StaticGpuVertex, bone_index) == 32);
  static_assert(offsetof(StaticGpuVertex, flags) == 36);
  static_assert(sizeof(StaticModelBoneState) == 80);
  static_assert(offsetof(StaticModelBoneState, transform) == 0);
  static_assert(offsetof(StaticModelBoneState, tint) == 64);

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
  };
  struct SwapchainImageResource {
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
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
  };

  SDL_Window *window_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
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
  std::vector<VkImage> swap_images_;
  std::vector<VkImageView> swap_views_;
  std::vector<SwapchainImageResource> swap_image_resources_;
  std::vector<VkFramebuffer> framebuffers_;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkFormat render_pass_format_ = VK_FORMAT_UNDEFINED;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  std::array<FrameSync, 2> frames_{};
  size_t frame_index_ = 0;
  bool recreate_swapchain_ = false;
  Clock::time_point next_swapchain_recreate_attempt_{};
  bool fatal_error_ = false;
  bool surface_maintenance1_khr_enabled_ = false;
  bool surface_maintenance1_ext_enabled_ = false;
  bool swapchain_maintenance1_enabled_ = false;
  std::string swapchain_maintenance1_extension_;
  bool diagnostics_enabled_ = false;
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
  VkDescriptorSetLayout static_desc_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool static_desc_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout static_mesh_layout_ = VK_NULL_HANDLE;
  VkPipeline static_mesh_pipeline_ = VK_NULL_HANDLE;
  VkPipeline static_mesh_pipeline_blend_ = VK_NULL_HANDLE;
  VkSampler static_sampler_ = VK_NULL_HANDLE;
  bool vsync_ = true;

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

  void destroyGraphicsPipelines() {
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
    if (ui_layout_) {
      vkDestroyPipelineLayout(device_, ui_layout_, nullptr);
      ui_layout_ = VK_NULL_HANDLE;
    }
    if (mesh_layout_) {
      vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
      mesh_layout_ = VK_NULL_HANDLE;
    }
    if (static_mesh_layout_) {
      vkDestroyPipelineLayout(device_, static_mesh_layout_, nullptr);
      static_mesh_layout_ = VK_NULL_HANDLE;
    }
  }

  [[nodiscard]] bool graphicsPipelinesReady() const {
    return ui_layout_ && mesh_layout_ && static_mesh_layout_ && ui_pipeline_ &&
           mesh_pipeline_ && mesh_pipeline_trans_ && mesh_pipeline_lines_ &&
           static_mesh_pipeline_ && static_mesh_pipeline_blend_;
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
                    VkMemoryPropertyFlags props, Buffer &out) {
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
      SDL_Log("Vulkan buffer creation failed: %d",
              static_cast<int>(create_result));
      return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    const auto memory_type = findMemoryType(req.memoryTypeBits, props);
    if (!memory_type) {
      writeLog("Vulkan buffer has no compatible memory type");
      destroyBuffer(out);
      return false;
    }
    ai.memoryTypeIndex = *memory_type;
    const VkResult allocation_result =
        vkAllocateMemory(device_, &ai, nullptr, &out.memory);
    if (allocation_result != VK_SUCCESS) {
      SDL_Log("Vulkan buffer memory allocation failed: %d",
              static_cast<int>(allocation_result));
      destroyBuffer(out);
      return false;
    }
    const VkResult bind_result =
        vkBindBufferMemory(device_, out.buffer, out.memory, 0);
    if (bind_result != VK_SUCCESS) {
      SDL_Log("Vulkan buffer memory bind failed: %d",
              static_cast<int>(bind_result));
      destroyBuffer(out);
      return false;
    }
    out.capacity = alloc;
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      const VkResult map_result =
          vkMapMemory(device_, out.memory, 0, alloc, 0, &out.mapped);
      if (map_result != VK_SUCCESS) {
        SDL_Log("Vulkan buffer memory map failed: %d",
                static_cast<int>(map_result));
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

  void destroyStaticModelResources() {
    destroyBuffer(static_model_vbo_);
    destroyBuffer(static_model_ibo_);
    destroyImage(static_texture_);
    static_draw_plan_ = {};
    static_generations_ = {};
    static_bone_count_ = 0;
    static_vertex_bytes_ = 0;
    static_index_bytes_ = 0;
    static_model_ready_ = false;
  }

  bool createStaticTexture(std::uint32_t width, std::uint32_t height,
                           ImageResource &out) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
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
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
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

  void updateStaticTextureDescriptors() {
    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = static_texture_.view;
    image_info.sampler = static_sampler_;
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = frames_[i].static_descriptor_set;
      writes[i].dstBinding = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].descriptorCount = 1;
      writes[i].pImageInfo = &image_info;
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
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
  }

  bool rebuildStaticModelResources(const StaticIndexedModelMesh &mesh,
                                   const TextureImage *texture,
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
      }
    }

    const VkDeviceSize vertex_bytes = static_cast<VkDeviceSize>(
        gpu_vertices.size() * sizeof(StaticGpuVertex));
    const VkDeviceSize index_bytes = static_cast<VkDeviceSize>(
        new_plan.indices.size() * sizeof(std::uint32_t));
    constexpr std::array<std::uint8_t, 4> kWhitePixel = {255, 255, 255, 255};
    const bool has_texture = texture != nullptr && texture->valid();
    const std::uint8_t *texture_pixels =
        has_texture ? texture->rgba.data() : kWhitePixel.data();
    const std::uint32_t texture_width =
        has_texture ? static_cast<std::uint32_t>(texture->width) : 1u;
    const std::uint32_t texture_height =
        has_texture ? static_cast<std::uint32_t>(texture->height) : 1u;
    const VkDeviceSize texture_bytes =
        static_cast<VkDeviceSize>(texture_width) * texture_height * 4u;
    if (vertex_bytes >
            (std::numeric_limits<VkDeviceSize>::max)() - index_bytes ||
        vertex_bytes + index_bytes >
            (std::numeric_limits<VkDeviceSize>::max)() - texture_bytes) {
      writeLog("Vulkan static resource size overflow");
      return false;
    }

    Buffer staging{};
    Buffer new_vertex_buffer{};
    Buffer new_index_buffer{};
    ImageResource new_texture{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (command) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      destroyBuffer(staging);
      destroyBuffer(new_vertex_buffer);
      destroyBuffer(new_index_buffer);
      destroyImage(new_texture);
    };

    const VkDeviceSize staging_bytes =
        vertex_bytes + index_bytes + texture_bytes;
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
        !createStaticTexture(texture_width, texture_height, new_texture)) {
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

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = new_texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy image_copy{};
    image_copy.bufferOffset = vertex_bytes + index_bytes;
    image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    image_copy.imageExtent = {texture_width, texture_height, 1};
    vkCmdCopyBufferToImage(command, staging.buffer, new_texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &image_copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const FrameSync &sync = frames_[frame_index_];
    const auto submit_start = Clock::now();
    logDiagnosticApi("vkQueueSubmit.static_upload", "before", std::nullopt,
                     0.0, UINT32_MAX, sync.fence, VK_NULL_HANDLE, command, true,
                     true);
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
    logDiagnosticApi(
        "vkQueueSubmit.static_upload", "after", submit_result,
        std::chrono::duration<double, std::milli>(Clock::now() - submit_start)
            .count(),
        UINT32_MAX, sync.fence, VK_NULL_HANDLE, command, true, false);
    if (submit_result != VK_SUCCESS) {
      cleanup();
      return false;
    }
    const auto idle_start = Clock::now();
    logDiagnosticApi("vkQueueWaitIdle.static_upload", "before", std::nullopt,
                     0.0, UINT32_MAX, sync.fence, VK_NULL_HANDLE, command, true,
                     true);
    const VkResult idle_result = vkQueueWaitIdle(graphics_queue_);
    logDiagnosticApi(
        "vkQueueWaitIdle.static_upload", "after", idle_result,
        std::chrono::duration<double, std::milli>(Clock::now() - idle_start)
            .count(),
        UINT32_MAX, sync.fence, VK_NULL_HANDLE, command, true, false);
    if (idle_result != VK_SUCCESS) {
      cleanup();
      return false;
    }
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
    command = VK_NULL_HANDLE;
    destroyBuffer(staging);

    destroyBuffer(static_model_vbo_);
    destroyBuffer(static_model_ibo_);
    destroyImage(static_texture_);
    static_model_vbo_ = new_vertex_buffer;
    static_model_ibo_ = new_index_buffer;
    static_texture_ = new_texture;
    new_vertex_buffer = {};
    new_index_buffer = {};
    new_texture = {};
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
    return true;
  }

  void readCompletedTimestamps(FrameSync &frame) {
    stats_.gpu_timestamp_valid = false;
    stats_.gpu_timestamp_total_ms = 0.0f;
    stats_.gpu_timestamp_ui_ms = 0.0f;
    stats_.gpu_timestamp_opaque_ms = 0.0f;
    stats_.gpu_timestamp_transparent_ms = 0.0f;
    stats_.gpu_timestamp_lines_ms = 0.0f;
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
    stats_.gpu_timestamp_valid = true;
    stats_.gpu_ms = stats_.gpu_timestamp_total_ms;
  }

  bool pickDevice() {
    uint32_t count = 0;
    const VkResult count_result =
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count_result != VK_SUCCESS || count == 0) {
      writeLog("No Vulkan devices");
      return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    const VkResult enumerate_result =
        vkEnumeratePhysicalDevices(instance_, &count, devs.data());
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
      std::optional<uint32_t> gfx, pres;
      for (uint32_t i = 0; i < qcount; ++i) {
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          gfx = i;
        }
        VkBool32 support = VK_FALSE;
        const VkResult support_result =
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &support);
        if (support_result != VK_SUCCESS) {
          support = VK_FALSE;
        }
        if (support) {
          pres = i;
        }
      }
      if (gfx && pres) {
        SwapchainSupport swapchain_support;
        if (!querySupport(d, swapchain_support)) {
          continue;
        }
        phys_ = d;
        graphics_family_ = *gfx;
        present_family_ = *pres;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        device_name_ = props.deviceName;
        timestamp_valid_bits_ = qs[*gfx].timestampValidBits;
        timestamp_period_ns_ = props.limits.timestampPeriod;
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

    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext =
        swapchain_maintenance1_enabled_ ? &maintenance_features : nullptr;
    di.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    di.pQueueCreateInfos = qcis.data();
    di.enabledExtensionCount =
        static_cast<uint32_t>(device_extensions.size());
    di.ppEnabledExtensionNames = device_extensions.data();
    VK_CHECK(vkCreateDevice(phys_, &di, nullptr, &device_));
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
      VkSwapchainKHR old_swapchain = VK_NULL_HANDLE) {
    SwapchainSupport support;
    if (!querySupport(phys_, support)) {
      writeLog("Vulkan swapchain surface has no usable formats/present modes");
      return false;
    }
    VkSurfaceFormatKHR format = support.formats[0];
    if (support.formats.size() == 1 &&
        support.formats[0].format == VK_FORMAT_UNDEFINED) {
      format.format = VK_FORMAT_B8G8R8A8_UNORM;
      format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
    for (auto &f : support.formats) {
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM ||
          f.format == VK_FORMAT_B8G8R8A8_SRGB) {
        format = f;
        break;
      }
    }
    swap_format_ = format.format;



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
    const VkResult create_swapchain =
        vkCreateSwapchainKHR(device_, &ci, nullptr, &new_swapchain);
    if (create_swapchain != VK_SUCCESS) {
      SDL_Log("Vulkan swapchain creation failed: %d",
              static_cast<int>(create_swapchain));
      return false;
    }
    swapchain_ = new_swapchain;

    uint32_t ic = 0;
    VkResult images_result =
        vkGetSwapchainImagesKHR(device_, swapchain_, &ic, nullptr);
    if (images_result != VK_SUCCESS || ic == 0) {
      SDL_Log("Vulkan swapchain image-count query failed: %d",
              static_cast<int>(images_result));
      destroySwapchainObjects();
      return false;
    }
    swap_images_.resize(ic);
    images_result =
        vkGetSwapchainImagesKHR(device_, swapchain_, &ic, swap_images_.data());
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
      if (swapchain_maintenance1_enabled_) {
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
    }
    return true;
  }

  bool waitForPendingPresentFences(const char *reason) {
    if (!swapchain_maintenance1_enabled_) {
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
      if (resource.depth_view) {
        vkDestroyImageView(device_, resource.depth_view, nullptr);
      }
      if (resource.depth_image) {
        vkDestroyImage(device_, resource.depth_image, nullptr);
      }
      if (resource.depth_memory) {
        vkFreeMemory(device_, resource.depth_memory, nullptr);
      }
    }
    swap_image_resources_.clear();
    swap_images_.clear();
  }

  void destroySwapchainObjects() {
    destroySwapchainImageObjects();
    if (swapchain_) {
      vkDestroySwapchainKHR(device_, swapchain_, nullptr);
      swapchain_ = VK_NULL_HANDLE;
    }
  }

  bool recreateSwapchain() {
    if (!window_) {
      return false;
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

    const FrameSync &sync = frames_[frame_index_];
    const auto idle_start = Clock::now();
    logDiagnosticApi("vkDeviceWaitIdle.swapchain_recreate", "before",
                     std::nullopt, 0.0, UINT32_MAX, sync.fence,
                     VK_NULL_HANDLE, sync.cmd, true, true);
    const VkResult idle_result = vkDeviceWaitIdle(device_);
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
    if (!waitForPendingPresentFences("swapchain_recreate")) {
      fatal_error_ = true;
      return false;
    }

    VkSwapchainKHR old_swapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;
    destroySwapchainImageObjects();
    const bool created = createSwapchain(old_swapchain);
    if (old_swapchain) {
      vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
    }
    if (!created) {
      return false;
    }
    const bool rebuild_graphics =
        !render_pass_ || render_pass_format_ != swap_format_ ||
        !graphicsPipelinesReady();
    if (rebuild_graphics) {
      destroyGraphicsPipelines();
      if (render_pass_) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
        render_pass_format_ = VK_FORMAT_UNDEFINED;
      }
      if (!createRenderPass() || !createPipelines()) {
        destroyGraphicsPipelines();
        if (render_pass_) {
          vkDestroyRenderPass(device_, render_pass_, nullptr);
          render_pass_ = VK_NULL_HANDLE;
          render_pass_format_ = VK_FORMAT_UNDEFINED;
        }
        destroySwapchainObjects();
        return false;
      }
    }
    if (!createFramebuffers()) {
      destroySwapchainObjects();
      return false;
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

    std::array<VkDescriptorSetLayoutBinding, 2> static_bindings{};
    static_bindings[0].binding = 0;
    static_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    static_bindings[0].descriptorCount = 1;
    static_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    static_bindings[1].binding = 1;
    static_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    static_bindings[1].descriptorCount = 1;
    static_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.bindingCount = static_cast<std::uint32_t>(static_bindings.size());
    li.pBindings = static_bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr,
                                         &static_desc_layout_));

    std::array<VkDescriptorPoolSize, 2> static_pool_sizes{};
    static_pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            static_cast<std::uint32_t>(frames_.size())};
    static_pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                            static_cast<std::uint32_t>(frames_.size())};
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
    static_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    static_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    static_sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    static_sampler_info.minLod = 0.0f;
    static_sampler_info.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(device_, &static_sampler_info, nullptr,
                             &static_sampler_));
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

    for (auto &frame : frames_) {
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
      !static_mesh_fs) {
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
  mesh_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  mesh_pc.offset = 0;
  mesh_pc.size = sizeof(float) * 16;
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

  auto makePipe = [&](VkShaderModule vs, VkShaderModule fs,
                      VkPipelineLayout layout, bool ui, bool static_mesh,
                      bool lines, bool mesh_trans, VkPipeline *out) -> bool {
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

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ui ? VK_FALSE : VK_TRUE;
    ds.depthWriteEnable = (ui || mesh_trans) ? VK_FALSE : VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    if (ui || mesh_trans) {
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

  if (!makePipe(ui_vs, ui_fs, ui_layout_, true, false, false, false,
                &ui_pipeline_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, false, false,
                &mesh_pipeline_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, false, true,
                &mesh_pipeline_trans_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, true, false,
                &mesh_pipeline_lines_) ||
      !makePipe(static_mesh_vs, static_mesh_fs, static_mesh_layout_, false,
                true, false, false, &static_mesh_pipeline_) ||
      !makePipe(static_mesh_vs, static_mesh_fs, static_mesh_layout_, false,
                true, false, true, &static_mesh_pipeline_blend_)) {
    return fail("Vulkan graphics-pipeline bundle creation failed");
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
