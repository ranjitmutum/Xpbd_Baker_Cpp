#pragma once

// Built-in Vulkan viewport path tracer (RT Pipeline + Ray Query fallback).
// Composites into the existing Vulkan render pass as a fullscreen quad.

#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/path_trace_aov.hpp"
#include "xpbd/gfx/still_image_export.hpp"
#include "xpbd/gfx/vulkan_rt_pipeline.hpp"
#include "xpbd/gfx/vulkan_rt_scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include <vulkan/vulkan.h>

namespace xpbd::gfx {

struct PathTraceFrameParams {
  // Column-major OpenGL-style matrices (same as FrameInput).
  const float *view = nullptr;
  const float *proj = nullptr;
  const float *previous_view = nullptr;
  const float *previous_proj = nullptr;
  bool motion_history_valid = false;
  // Temporal reconstruction consumes a fresh noisy frame rather than the
  // progressively averaged preview. This does not invalidate its own history.
  bool temporal_reconstruction_input = false;
  bool ray_reconstruction_guides = false;
  // Optional AOV/RR/statistics writes. Color and depth remain mandatory.
  std::uint32_t output_write_mask = 0;
  float camera_jitter[2]{0.0f, 0.0f};
  float light_dir[3]{0.35f, 0.85f, 0.40f};
  float ambient = 0.38f;
  float light_color[3]{1.0f, 1.0f, 1.0f};
  float intensity = 0.85f;
  float clear_r = 0.1f;
  float clear_g = 0.12f;
  float clear_b = 0.16f;
  float exposure = 1.0f;
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  std::uint32_t frame_index = 0;
  std::uint32_t frame_slot = 0;
  std::uint64_t history_key = 0;
  PathTraceSettings settings{};
  LabPbrDebugView material_debug_view = LabPbrDebugView::Shaded;
  RtDebugView rt_debug_view = RtDebugView::Off;
  std::uint32_t material_feature_flags = 0;
  bool hdr_environment = false;
  VkImageView environment_view = VK_NULL_HANDLE;
  VkSampler environment_sampler = VK_NULL_HANDLE;
  VkBuffer environment_distribution = VK_NULL_HANDLE;
  VkDeviceSize environment_distribution_bytes = 0;
  int viewport_x = 0;
  int viewport_y = 0;
  int viewport_w = 1;
  int viewport_h = 1;
};

enum class PathTraceCaptureState : std::uint8_t {
  Idle = 0,
  Requested,
  PendingGpuReadback,
  Completed,
  Failed,
  Cancelled,
};

class VulkanPathTracer {
public:
  struct DescriptorStats {
    std::uint64_t write_calls = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t entries_written = 0;
    float update_ms = 0.0f;
  };

  VulkanPathTracer() = default;
  ~VulkanPathTracer() { shutdown(); }

  VulkanPathTracer(const VulkanPathTracer &) = delete;
  VulkanPathTracer &operator=(const VulkanPathTracer &) = delete;

  [[nodiscard]] bool init(VkPhysicalDevice phys, VkDevice device,
                          VkRenderPass render_pass,
                          bool enable_diagnostic_capture = true);
  void shutdown();

  [[nodiscard]] bool ready() const noexcept {
    return compute_pipeline_ != VK_NULL_HANDLE &&
           composite_pipeline_ != VK_NULL_HANDLE;
  }
  [[nodiscard]] bool rtPipelineReady() const noexcept {
    return rt_pipeline_.ready();
  }
  [[nodiscard]] RtPipelineStats rtPipelineStats() const noexcept {
    return rt_pipeline_.stats();
  }
  [[nodiscard]] std::uint32_t accumulatedSamples() const noexcept {
    return accumulated_samples_;
  }
  [[nodiscard]] std::uint64_t historyResetCount() const noexcept {
    return history_reset_count_;
  }
  [[nodiscard]] std::uint64_t historyKey() const noexcept {
    return history_key_;
  }
  [[nodiscard]] std::uint64_t historyGeneration() const noexcept {
    return history_generation_;
  }
  [[nodiscard]] DescriptorStats descriptorStats() const noexcept {
    const RtPipelineStats pipeline_stats = rt_pipeline_.stats();
    return {descriptor_write_calls_ + pipeline_stats.descriptor_write_calls,
            descriptor_cache_hits_ + pipeline_stats.descriptor_cache_hits,
            descriptor_entries_written_ +
                pipeline_stats.descriptor_entries_written,
            descriptor_update_ms_ + pipeline_stats.descriptor_update_ms};
  }
  [[nodiscard]] std::uint32_t lastOutputWriteMask() const noexcept {
    return last_output_write_mask_;
  }
  [[nodiscard]] PathTraceCaptureState captureState() const noexcept {
    return runtime_capture_state_;
  }
  [[nodiscard]] const std::string &captureError() const noexcept {
    return runtime_capture_error_;
  }
  [[nodiscard]] bool requestStillCapture(
      const std::filesystem::path &path, StillImageFormat format,
      bool transparent_background);
  void cancelStillCapture() noexcept;
  [[nodiscard]] VkImageView
  aovView(PathTraceAovLayer layer) const noexcept {
    const std::size_t index = static_cast<std::size_t>(layer);
    return index < aov_layer_views_.size()
               ? aov_layer_views_[index]
               : VK_NULL_HANDLE;
  }
  [[nodiscard]] VkImageView statisticsAovView() const noexcept {
    return statistics_image_view_;
  }
  [[nodiscard]] VkImageView aovArrayView() const noexcept {
    return aov_array_view_;
  }
  [[nodiscard]] VkImage colorImage() const noexcept { return image_; }
  [[nodiscard]] VkDeviceMemory colorMemory() const noexcept {
    return image_memory_;
  }
  [[nodiscard]] VkImageView colorView() const noexcept {
    return image_view_;
  }
  [[nodiscard]] VkImage depthImage() const noexcept {
    return depth_image_;
  }
  [[nodiscard]] VkDeviceMemory depthMemory() const noexcept {
    return depth_image_memory_;
  }
  [[nodiscard]] VkImageView depthView() const noexcept {
    return depth_image_view_;
  }
  [[nodiscard]] VkImage aovImage() const noexcept { return aov_image_; }
  [[nodiscard]] VkDeviceMemory aovMemory() const noexcept {
    return aov_image_memory_;
  }
  [[nodiscard]] VkImage rrMotionImage() const noexcept {
    return rr_motion_image_;
  }
  [[nodiscard]] VkDeviceMemory rrMotionMemory() const noexcept {
    return rr_motion_image_memory_;
  }
  [[nodiscard]] VkImageView rrMotionView() const noexcept {
    return rr_motion_image_view_;
  }
  [[nodiscard]] VkImage rrDiffuseAlbedoImage() const noexcept {
    return rr_diffuse_albedo_image_;
  }
  [[nodiscard]] VkDeviceMemory rrDiffuseAlbedoMemory() const noexcept {
    return rr_diffuse_albedo_image_memory_;
  }
  [[nodiscard]] VkImageView rrDiffuseAlbedoView() const noexcept {
    return rr_diffuse_albedo_image_view_;
  }
  [[nodiscard]] VkImage rrSpecularAlbedoImage() const noexcept {
    return rr_specular_albedo_image_;
  }
  [[nodiscard]] VkDeviceMemory rrSpecularAlbedoMemory() const noexcept {
    return rr_specular_albedo_image_memory_;
  }
  [[nodiscard]] VkImageView rrSpecularAlbedoView() const noexcept {
    return rr_specular_albedo_image_view_;
  }
  [[nodiscard]] VkImage rrNormalRoughnessImage() const noexcept {
    return rr_normal_roughness_image_;
  }
  [[nodiscard]] VkDeviceMemory rrNormalRoughnessMemory() const noexcept {
    return rr_normal_roughness_image_memory_;
  }
  [[nodiscard]] VkImageView rrNormalRoughnessView() const noexcept {
    return rr_normal_roughness_image_view_;
  }
  [[nodiscard]] VkImage rrSpecularHitDistanceImage() const noexcept {
    return rr_specular_hit_distance_image_;
  }
  [[nodiscard]] VkDeviceMemory
  rrSpecularHitDistanceMemory() const noexcept {
    return rr_specular_hit_distance_image_memory_;
  }
  [[nodiscard]] VkImageView
  rrSpecularHitDistanceView() const noexcept {
    return rr_specular_hit_distance_image_view_;
  }
  [[nodiscard]] std::uint32_t targetWidth() const noexcept {
    return image_w_;
  }
  [[nodiscard]] std::uint32_t targetHeight() const noexcept {
    return image_h_;
  }

  // Resize path-trace storage image to at least width x height.
  [[nodiscard]] bool ensureTarget(std::uint32_t width, std::uint32_t height);

  // Record compute path-trace into storage image (before or outside render pass).
  // albedo_view/sampler provide model base-color (Minecraft atlas); either may
  // be null to fall back to a 1x1 white texture.
  void recordDispatch(VkCommandBuffer cmd, const VulkanRtScene &scene,
                      const PathTraceFrameParams &params,
                      VkImageView albedo_view = VK_NULL_HANDLE,
                      VkImageView normal_view = VK_NULL_HANDLE,
                      VkImageView specular_view = VK_NULL_HANDLE,
                      VkSampler albedo_sampler = VK_NULL_HANDLE);

  // Draw composite quad into current render pass (viewport already set).
  void recordComposite(
      VkCommandBuffer cmd, const PathTraceFrameParams &params,
      VkImageView reconstructed_color = VK_NULL_HANDLE);

private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
  };

  struct ComputeDescriptorKey {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    std::array<VkImageView, 12> image_views{};
    std::array<VkSampler, 3> image_samplers{};
    std::array<VkBuffer, 11> buffers{};
    std::array<VkDeviceSize, 11> buffer_sizes{};

    [[nodiscard]] constexpr bool operator==(
        const ComputeDescriptorKey &) const = default;
  };

  [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t bits,
                                             VkMemoryPropertyFlags props) const;
  [[nodiscard]] VkShaderModule makeModule(const std::uint32_t *words,
                                          std::size_t count) const;
  void destroyImage();
  void destroyMotionFrame();
  [[nodiscard]] bool ensureMotionFrame();
  void destroyFallbackAlbedo();
  void destroyCaptureBuffer();
  [[nodiscard]] bool ensureCaptureBuffer(VkDeviceSize size);
  void flushPendingCapture();
  [[nodiscard]] bool ensureFallbackAlbedo();
  [[nodiscard]] bool createPipelines(VkRenderPass render_pass);

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VulkanRtPipeline rt_pipeline_{};
  bool rt_fallback_logged_ = false;

  VkDescriptorSetLayout compute_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout compute_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool compute_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet compute_set_ = VK_NULL_HANDLE;

  VkDescriptorSetLayout composite_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout composite_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline composite_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool composite_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet composite_set_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;          // composite (nearest)
  VkSampler albedo_sampler_ = VK_NULL_HANDLE;   // path-trace albedo (nearest)

  // 1x1 white fallback when the static model texture is unavailable.
  VkImage fallback_albedo_image_ = VK_NULL_HANDLE;
  VkDeviceMemory fallback_albedo_memory_ = VK_NULL_HANDLE;
  VkImageView fallback_albedo_view_ = VK_NULL_HANDLE;
  bool fallback_albedo_cleared_ = false;
  ComputeDescriptorKey compute_descriptor_key_{};
  bool compute_descriptor_key_valid_ = false;
  std::uint64_t descriptor_write_calls_ = 0;
  std::uint64_t descriptor_cache_hits_ = 0;
  std::uint64_t descriptor_entries_written_ = 0;
  float descriptor_update_ms_ = 0.0f;
  std::uint32_t last_output_write_mask_ = 0;

  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
  VkImageView image_view_ = VK_NULL_HANDLE;
  VkImage depth_image_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_image_memory_ = VK_NULL_HANDLE;
  VkImageView depth_image_view_ = VK_NULL_HANDLE;
  VkImage aov_image_ = VK_NULL_HANDLE;
  VkDeviceMemory aov_image_memory_ = VK_NULL_HANDLE;
  VkImageView aov_array_view_ = VK_NULL_HANDLE;
  std::array<VkImageView,
             static_cast<std::size_t>(PathTraceAovLayer::Count)>
      aov_layer_views_{};
  VkImage statistics_image_ = VK_NULL_HANDLE;
  VkDeviceMemory statistics_image_memory_ = VK_NULL_HANDLE;
  VkImageView statistics_image_view_ = VK_NULL_HANDLE;
  VkImage rr_motion_image_ = VK_NULL_HANDLE;
  VkDeviceMemory rr_motion_image_memory_ = VK_NULL_HANDLE;
  VkImageView rr_motion_image_view_ = VK_NULL_HANDLE;
  VkImage rr_diffuse_albedo_image_ = VK_NULL_HANDLE;
  VkDeviceMemory rr_diffuse_albedo_image_memory_ = VK_NULL_HANDLE;
  VkImageView rr_diffuse_albedo_image_view_ = VK_NULL_HANDLE;
  VkImage rr_specular_albedo_image_ = VK_NULL_HANDLE;
  VkDeviceMemory rr_specular_albedo_image_memory_ = VK_NULL_HANDLE;
  VkImageView rr_specular_albedo_image_view_ = VK_NULL_HANDLE;
  VkImage rr_normal_roughness_image_ = VK_NULL_HANDLE;
  VkDeviceMemory rr_normal_roughness_image_memory_ = VK_NULL_HANDLE;
  VkImageView rr_normal_roughness_image_view_ = VK_NULL_HANDLE;
  VkImage rr_specular_hit_distance_image_ = VK_NULL_HANDLE;
  VkDeviceMemory rr_specular_hit_distance_image_memory_ = VK_NULL_HANDLE;
  VkImageView rr_specular_hit_distance_image_view_ = VK_NULL_HANDLE;
  Buffer motion_frame_buffer_{};
  void *motion_frame_mapped_ = nullptr;
  std::uint32_t image_w_ = 0;
  std::uint32_t image_h_ = 0;
  VkImageLayout image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout depth_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout aov_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout statistics_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout rr_motion_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout rr_diffuse_albedo_image_layout_ =
      VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout rr_specular_albedo_image_layout_ =
      VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout rr_normal_roughness_image_layout_ =
      VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout rr_specular_hit_distance_image_layout_ =
      VK_IMAGE_LAYOUT_UNDEFINED;
  std::uint64_t history_key_ = 0;
  std::uint64_t history_generation_ = 0;
  std::uint64_t history_reset_count_ = 0;
  std::uint32_t accumulated_samples_ = 0;
  bool history_valid_ = false;

  // Optional unattended diagnostic readback. XPBD_PT_CAPTURE names one PNG;
  // only frame slot zero schedules it when Maximum Samples is reached.
  Buffer capture_buffer_{};
  void *capture_mapped_ = nullptr;
  std::string capture_path_;
  std::string aov_summary_path_;
  std::string pending_capture_path_;
  std::string pending_aov_summary_path_;
  StillImageFormat pending_capture_format_ = StillImageFormat::Png;
  StillImageDisplayTransform pending_capture_display_{};
  bool pending_capture_transparent_background_ = true;
  bool pending_capture_is_runtime_ = false;
  std::uint32_t aov_capture_samples_ = 0;
  std::uint32_t pending_capture_width_ = 0;
  std::uint32_t pending_capture_height_ = 0;
  VkDeviceSize pending_aov_offset_ = 0;
  VkDeviceSize pending_statistics_offset_ = 0;
  VkDeviceSize pending_depth_offset_ = 0;
  bool capture_pending_ = false;
  bool capture_completed_ = false;

  std::string runtime_capture_path_;
  std::string runtime_capture_error_;
  StillImageFormat runtime_capture_format_ = StillImageFormat::Png;
  PathTraceCaptureState runtime_capture_state_ =
      PathTraceCaptureState::Idle;
  bool runtime_capture_transparent_background_ = false;
};

} // namespace xpbd::gfx
