#pragma once

#include "xpbd/gfx/ray_tracing.hpp"

#include <cstdint>
#include <memory>
#include <string>

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace xpbd::gfx {

struct StreamlineDlssOptimalSettings {
  bool valid = false;
  std::uint32_t render_width = 0;
  std::uint32_t render_height = 0;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  PathTraceUpscale mode = PathTraceUpscale::Off;
};

struct StreamlineDlssFrame {
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkImage color_image = VK_NULL_HANDLE;
  VkDeviceMemory color_memory = VK_NULL_HANDLE;
  VkImageView color_view = VK_NULL_HANDLE;
  VkImage depth_image = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkImage motion_image = VK_NULL_HANDLE;
  VkDeviceMemory motion_memory = VK_NULL_HANDLE;
  VkImageView motion_view = VK_NULL_HANDLE;
  const float *view = nullptr;
  const float *projection = nullptr;
  const float *previous_view = nullptr;
  const float *previous_projection = nullptr;
  std::uint32_t render_width = 0;
  std::uint32_t render_height = 0;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  std::uint32_t frame_index = 0;
  std::uint32_t frame_slot = 0;
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
  PathTraceUpscale mode = PathTraceUpscale::Off;
  bool reset_history = false;
};

struct StreamlineDlssRayReconstructionFrame : StreamlineDlssFrame {
  VkImage diffuse_albedo_image = VK_NULL_HANDLE;
  VkDeviceMemory diffuse_albedo_memory = VK_NULL_HANDLE;
  VkImageView diffuse_albedo_view = VK_NULL_HANDLE;
  VkImage specular_albedo_image = VK_NULL_HANDLE;
  VkDeviceMemory specular_albedo_memory = VK_NULL_HANDLE;
  VkImageView specular_albedo_view = VK_NULL_HANDLE;
  VkImage normal_roughness_image = VK_NULL_HANDLE;
  VkDeviceMemory normal_roughness_memory = VK_NULL_HANDLE;
  VkImageView normal_roughness_view = VK_NULL_HANDLE;
  VkImage specular_hit_distance_image = VK_NULL_HANDLE;
  VkDeviceMemory specular_hit_distance_memory = VK_NULL_HANDLE;
  VkImageView specular_hit_distance_view = VK_NULL_HANDLE;
};

struct StreamlineFrameGenerationFrame {
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkImage depth_image = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkImage motion_image = VK_NULL_HANDLE;
  VkDeviceMemory motion_memory = VK_NULL_HANDLE;
  VkImageView motion_view = VK_NULL_HANDLE;
  VkImage hudless_image = VK_NULL_HANDLE;
  VkDeviceMemory hudless_memory = VK_NULL_HANDLE;
  VkImageView hudless_view = VK_NULL_HANDLE;
  VkImage ui_image = VK_NULL_HANDLE;
  VkDeviceMemory ui_memory = VK_NULL_HANDLE;
  VkImageView ui_view = VK_NULL_HANDLE;
  const float *view = nullptr;
  const float *projection = nullptr;
  const float *previous_view = nullptr;
  const float *previous_projection = nullptr;
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  std::uint32_t render_width = 0;
  std::uint32_t render_height = 0;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  std::uint32_t viewport_x = 0;
  std::uint32_t viewport_y = 0;
  std::uint32_t viewport_width = 0;
  std::uint32_t viewport_height = 0;
  std::uint32_t swapchain_image_count = 0;
  std::uint32_t frame_index = 0;
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
  bool reset_history = false;
};

// Secure, optional Vulkan bridge around NVIDIA Streamline. The public header
// deliberately contains no Streamline SDK types so an SDK-less build has the
// same application ABI and always falls back to native Vulkan.
class StreamlineVulkanRuntime {
public:
  struct Impl;

  StreamlineVulkanRuntime();
  ~StreamlineVulkanRuntime();

  StreamlineVulkanRuntime(const StreamlineVulkanRuntime &) = delete;
  StreamlineVulkanRuntime &
  operator=(const StreamlineVulkanRuntime &) = delete;

  // Must run before the Vulkan instance/device and any hooked object exists.
  [[nodiscard]] bool initializeBeforeVulkan();
  // Checks DLSS support after the host selected a physical device.
  void inspectPhysicalDevice(VkPhysicalDevice physical_device);

  // Streamline must shut down before the Vulkan instance and device. The
  // module itself remains loaded until releaseAfterVulkan() so proxy entry
  // points stay valid during object destruction.
  void shutdownBeforeVulkan() noexcept;
  void releaseAfterVulkan() noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool dlssSupported() const noexcept;
  [[nodiscard]] bool
  dlssRayReconstructionSupported() const noexcept;
  [[nodiscard]] bool frameGenerationSupported() const noexcept;
  // DLSS-G must be completely unhooked while the user-facing option is Off.
  // Call only after draining Vulkan work and while no Vulkan API is running
  // concurrently; changing this state requires a fresh swapchain.
  [[nodiscard]] bool
  setFrameGenerationFeatureLoaded(bool loaded) noexcept;
  [[nodiscard]] bool reflexSupported() const noexcept;
  [[nodiscard]] bool pclSupported() const noexcept;
  [[nodiscard]] bool frameGenerationActive() const noexcept;
  [[nodiscard]] std::uint32_t
  framesActuallyPresented() const noexcept;
  [[nodiscard]] std::string status() const;
  [[nodiscard]] std::string frameGenerationStatus() const;
  [[nodiscard]] StreamlineDlssOptimalSettings queryDlssOptimalSettings(
      PathTraceUpscale mode, std::uint32_t output_width,
      std::uint32_t output_height);
  [[nodiscard]] bool recordDlss(const StreamlineDlssFrame &frame);
  [[nodiscard]] StreamlineDlssOptimalSettings
  queryDlssRayReconstructionOptimalSettings(
      PathTraceUpscale mode, std::uint32_t output_width,
      std::uint32_t output_height);
  [[nodiscard]] bool recordDlssRayReconstruction(
      const StreamlineDlssRayReconstructionFrame &frame);
  [[nodiscard]] VkImageView dlssOutputView() const noexcept;
  void invalidateDlssHistory() noexcept;

  // Acquire one token for the application frame and use it for Reflex sleep,
  // PCL markers, SR/RR constants/evaluation, FG tags, and present.
  void beginLatencyFrame(std::uint32_t frame_index,
                         PathTraceReflexMode mode,
                         bool frame_generation_requested);
  void endLatencySimulation();
  void markRenderSubmitStart();
  void markRenderSubmitEnd();
  void markPresentStart();
  void markPresentEnd();
  [[nodiscard]] std::uint32_t pclLatencyPingMessage() const noexcept;
  void markPclLatencyPing();

  [[nodiscard]] bool recordFrameGenerationInputs(
      const StreamlineFrameGenerationFrame &frame);
  // Expire every per-frame FG tag when the preview cannot provide valid
  // depth/motion/color inputs (loading, collapsed viewport, raster mode).
  void clearFrameGenerationInputs(VkCommandBuffer command_buffer,
                                  std::uint32_t frame_index,
                                  std::uint32_t output_width,
                                  std::uint32_t output_height,
                                  std::uint32_t viewport_x,
                                  std::uint32_t viewport_y,
                                  std::uint32_t viewport_width,
                                  std::uint32_t viewport_height) noexcept;
  // NVIDIA requires disabling FG before swapchain/window manipulation.
  void disableFrameGeneration() noexcept;
  // Query after each intercepted present; numFramesActuallyPresented then
  // represents this application's most recent present interval.
  void updateFrameGenerationStateAfterPresent() noexcept;

  [[nodiscard]] VkResult createInstance(
      const VkInstanceCreateInfo *create_info,
      const VkAllocationCallbacks *allocator, VkInstance *instance);
  [[nodiscard]] VkResult enumeratePhysicalDevices(
      VkInstance instance, std::uint32_t *count,
      VkPhysicalDevice *physical_devices);
  [[nodiscard]] VkResult createDevice(
      VkInstance instance, VkPhysicalDevice physical_device,
      const VkDeviceCreateInfo *create_info,
      const VkAllocationCallbacks *allocator, VkDevice *device);
  [[nodiscard]] VkResult createWin32Surface(
      VkInstance instance, void *window_handle,
      const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);
  void destroySurface(VkInstance instance, VkSurfaceKHR surface,
                      const VkAllocationCallbacks *allocator);

  [[nodiscard]] VkResult createSwapchain(
      VkDevice device, const VkSwapchainCreateInfoKHR *create_info,
      const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain);
  void destroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
                        const VkAllocationCallbacks *allocator);
  [[nodiscard]] VkResult getSwapchainImages(VkDevice device,
                                            VkSwapchainKHR swapchain,
                                            std::uint32_t *count,
                                            VkImage *images);
  [[nodiscard]] VkResult acquireNextImage(VkDevice device,
                                          VkSwapchainKHR swapchain,
                                          std::uint64_t timeout,
                                          VkSemaphore semaphore,
                                          VkFence fence,
                                          std::uint32_t *image_index);
  [[nodiscard]] VkResult queuePresent(VkQueue queue,
                                      const VkPresentInfoKHR *present_info);
  [[nodiscard]] VkResult deviceWaitIdle(VkDevice device);

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace xpbd::gfx
