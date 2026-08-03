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
#include <limits>
#include <string>
#include <vector>

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
  // Full RT consumes the resolved finite Sun as radiance over a solid-angle
  // disk. The compatibility compute path continues to use the legacy fields
  // above without changing path_trace.comp semantics.
  float sun_radiance[3]{0.0f, 0.0f, 0.0f};
  float sun_angular_radius = 0.0f;
  bool sun_casts_shadow = true;
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
  // UNORM swapchain attachments require an explicit linear-to-sRGB encode;
  // SRGB attachments perform that conversion in the fixed-function write.
  bool output_requires_srgb_encoding = false;
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

struct PathTraceStillBackgroundInput {
  std::uint32_t face_size = 0;
  const std::uint8_t *rgba8 = nullptr;
  std::size_t rgba8_size = 0;
  const float *view = nullptr;
  const float *proj = nullptr;
};

// Full-resolution color and depth are mandatory. Optional storage targets are
// selected with the same ABI mask consumed by both path-tracing shaders.
struct PathTraceTargetRequirements {
  std::uint32_t output_mask = 0;
  // Candidate allocations coexist with the active bundle until commit.
  // Callers may increase this factor for more conservative budget admission.
  float budget_safety_factor = 1.10f;

  [[nodiscard]] constexpr bool operator==(
      const PathTraceTargetRequirements &) const = default;
};

inline constexpr VkImageUsageFlags kPathTraceTargetImageUsage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
inline constexpr VkFormatFeatureFlags kPathTraceTargetFormatFeatures =
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

[[nodiscard]] constexpr bool pathTraceFormatSupports(
    VkFormatFeatureFlags available, VkFormatFeatureFlags required) noexcept {
  return (available & required) == required;
}

// AOVs share one ten-layer image, so one requested AOV bit expands to the
// complete physical AOV group after unsupported outputs have been removed.
[[nodiscard]] constexpr std::uint32_t pathTraceTargetAllocationMask(
    std::uint32_t requested_mask,
    std::uint32_t supported_mask = kPathTraceAllOptionalOutputMask) noexcept {
  std::uint32_t mask = requested_mask & supported_mask &
                       kPathTraceAllOptionalOutputMask;
  if ((mask & kPathTraceAllAovOutputMask) != 0u) {
    mask |= kPathTraceAllAovOutputMask;
  }
  return mask;
}

[[nodiscard]] constexpr std::uint32_t
pathTraceTargetBytesPerPixel(std::uint32_t allocated_mask) noexcept {
  allocated_mask &= kPathTraceAllOptionalOutputMask;
  std::uint32_t bytes = 12u; // RGBA16F color + R32F depth.
  if ((allocated_mask & kPathTraceAllAovOutputMask) != 0u) {
    bytes += 8u * static_cast<std::uint32_t>(PathTraceAovLayer::Count);
  }
  if ((allocated_mask & kPathTraceStatisticsOutputMask) != 0u) {
    bytes += 16u;
  }
  if ((allocated_mask & kPathTraceRrMotionOutputMask) != 0u) {
    bytes += 8u;
  }
  if ((allocated_mask & pathTraceOptionalOutputBit(
                            PathTraceOptionalOutput::RrSpecularHitDistance)) !=
      0u) {
    bytes += 4u;
  }
  constexpr std::array<PathTraceOptionalOutput, 3> kRgba16Guides{
      PathTraceOptionalOutput::RrDiffuseAlbedo,
      PathTraceOptionalOutput::RrSpecularAlbedo,
      PathTraceOptionalOutput::RrNormalRoughness};
  for (const PathTraceOptionalOutput guide : kRgba16Guides) {
    if ((allocated_mask & pathTraceOptionalOutputBit(guide)) != 0u) {
      bytes += 8u;
    }
  }
  constexpr std::array<PathTraceOptionalOutput, 3> kR8Masks{
      PathTraceOptionalOutput::TransparencyAndComposition,
      PathTraceOptionalOutput::ReactiveMask,
      PathTraceOptionalOutput::GuideValidity};
  for (const PathTraceOptionalOutput mask : kR8Masks) {
    if ((allocated_mask & pathTraceOptionalOutputBit(mask)) != 0u) {
      bytes += 1u;
    }
  }
  return bytes;
}

static_assert(pathTraceTargetAllocationMask(1u) ==
                  kPathTraceAllAovOutputMask &&
              pathTraceTargetBytesPerPixel(0u) == 12u &&
              pathTraceTargetBytesPerPixel(kPathTraceAllRrGuideOutputMask) ==
                  48u &&
              pathTraceTargetBytesPerPixel(kPathTraceAllOptionalOutputMask) ==
                  147u,
              "Path-trace target allocation policy regression");

enum class PathTraceTargetError : std::uint8_t {
  None = 0,
  UnsupportedFormat,
  CreateImageFailed,
  MemoryTypeUnavailable,
  OutOfDeviceMemory,
  OutOfHostMemory,
  AllocationFailed,
  BindFailed,
  ViewCreationFailed,
};

enum class PathTraceTargetFailureStage : std::uint8_t {
  None = 0,
  ValidateFormatCapabilities,
  BudgetPreflight,
  CreateImage,
  SelectMemoryType,
  AllocateMemory,
  BindImageMemory,
  CreateImageView,
};

enum class PathTraceTargetStatus : std::uint8_t {
  Uninitialized = 0,
  Exact,
  PreviousRetained,
  RetryDeferred,
  FailedNoTarget,
};

struct PathTraceTargetFailure {
  PathTraceTargetError error = PathTraceTargetError::None;
  PathTraceTargetFailureStage stage = PathTraceTargetFailureStage::None;
  VkResult vk_result = VK_SUCCESS;
  VkFormat format = VK_FORMAT_UNDEFINED;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t requested_output_mask = 0;
  VkDeviceSize estimated_bytes = 0;
  VkImageUsageFlags usage = 0;
  std::uint32_t memory_type =
      (std::numeric_limits<std::uint32_t>::max)();
  std::uint32_t heap =
      (std::numeric_limits<std::uint32_t>::max)();
  VkDeviceSize heap_budget = 0;
  VkDeviceSize heap_usage = 0;
  std::string resource;

  [[nodiscard]] bool failed() const noexcept {
    return error != PathTraceTargetError::None;
  }
};

struct PathTraceTargetResult {
  PathTraceTargetStatus status = PathTraceTargetStatus::Uninitialized;
  std::uint32_t requested_width = 0;
  std::uint32_t requested_height = 0;
  std::uint32_t requested_output_mask = 0;
  std::uint32_t supported_output_mask = 0;
  std::uint32_t masked_output_mask = 0;
  std::uint32_t active_width = 0;
  std::uint32_t active_height = 0;
  std::uint32_t allocated_output_mask = 0;
  VkDeviceSize estimated_bytes = 0;
  VkDeviceSize allocated_bytes = 0;
  std::uint32_t retry_after_ms = 0;
  PathTraceTargetFailure failure{};

  [[nodiscard]] bool exact() const noexcept {
    return status == PathTraceTargetStatus::Exact;
  }
  [[nodiscard]] bool hasActiveTarget() const noexcept {
    return active_width > 0 && active_height > 0;
  }
};

struct PathTraceTargetStats {
  std::uint32_t requested_output_mask = 0;
  std::uint32_t supported_output_mask = 0;
  std::uint32_t masked_output_mask = 0;
  std::uint32_t allocated_output_mask = 0;
  std::uint32_t image_count = 0;
  VkDeviceSize estimated_bytes = 0;
  VkDeviceSize allocated_bytes = 0;
  std::uint64_t successful_rebuilds = 0;
  std::uint64_t failed_rebuilds = 0;
  std::uint64_t deferred_retries = 0;
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
                          bool enable_diagnostic_capture = true,
                          bool descriptor_binding_partially_bound = false);
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
  [[nodiscard]] bool lastDispatchRecorded() const noexcept {
    return last_dispatch_recorded_;
  }
  [[nodiscard]] PathTraceCaptureState captureState() const noexcept {
    return runtime_capture_state_;
  }
  [[nodiscard]] const std::string &captureError() const noexcept {
    return runtime_capture_error_;
  }
  [[nodiscard]] bool requestStillCapture(
      const std::filesystem::path &path, StillImageFormat format,
      bool transparent_background, std::uint64_t job_id = 0u,
      const PathTraceStillBackgroundInput *background = nullptr);
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
  [[nodiscard]] VkImage transparencyAndCompositionImage() const noexcept {
    return transparency_and_composition_image_;
  }
  [[nodiscard]] VkDeviceMemory
  transparencyAndCompositionMemory() const noexcept {
    return transparency_and_composition_image_memory_;
  }
  [[nodiscard]] VkImageView
  transparencyAndCompositionView() const noexcept {
    return transparency_and_composition_image_view_;
  }
  [[nodiscard]] VkImage reactiveMaskImage() const noexcept {
    return reactive_mask_image_;
  }
  [[nodiscard]] VkDeviceMemory reactiveMaskMemory() const noexcept {
    return reactive_mask_image_memory_;
  }
  [[nodiscard]] VkImageView reactiveMaskView() const noexcept {
    return reactive_mask_image_view_;
  }
  [[nodiscard]] VkImage guideValidityImage() const noexcept {
    return guide_validity_image_;
  }
  [[nodiscard]] VkDeviceMemory guideValidityMemory() const noexcept {
    return guide_validity_image_memory_;
  }
  [[nodiscard]] VkImageView guideValidityView() const noexcept {
    return guide_validity_image_view_;
  }
  [[nodiscard]] std::uint32_t targetWidth() const noexcept {
    return image_w_;
  }
  [[nodiscard]] std::uint32_t targetHeight() const noexcept {
    return image_h_;
  }
  [[nodiscard]] const PathTraceTargetResult &lastTargetResult() const noexcept {
    return last_target_result_;
  }
  [[nodiscard]] const PathTraceTargetFailure &
  lastTargetFailure() const noexcept {
    return last_target_result_.failure;
  }
  [[nodiscard]] PathTraceTargetStats targetStats() const noexcept {
    return target_stats_;
  }
  [[nodiscard]] std::uint32_t supportedTargetOutputMask() const noexcept {
    return supported_target_output_mask_;
  }

  // Compatibility path used by the backend before frame parameters are fully
  // assembled. It uses the most recently dispatched optional-output mask.
  [[nodiscard]] bool ensureTarget(std::uint32_t width, std::uint32_t height);
  // Requirements-aware transactional rebuild. A failed candidate never
  // mutates the active target or its accumulation history.
  [[nodiscard]] PathTraceTargetResult
  ensureTarget(std::uint32_t width, std::uint32_t height,
               PathTraceTargetRequirements requirements);

  // Record compute path-trace into storage image (before or outside render pass).
  // Material views/samplers provide the static atlas and its linear-data
  // sidecars. A null view falls back to the 1x1 default texture; a null sampler
  // uses the path tracer's role-specific base-level sampler. Alpha shares the
  // nearest albedo sampler because coverage is stored in the base image.
  void recordDispatch(VkCommandBuffer cmd, const VulkanRtScene &scene,
                      const PathTraceFrameParams &params,
                      VkImageView albedo_view = VK_NULL_HANDLE,
                      VkImageView normal_view = VK_NULL_HANDLE,
                      VkImageView specular_view = VK_NULL_HANDLE,
                      VkSampler albedo_sampler = VK_NULL_HANDLE,
                      VkSampler normal_sampler = VK_NULL_HANDLE,
                      VkSampler specular_sampler = VK_NULL_HANDLE);

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

  struct TargetBundle {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_image_memory = VK_NULL_HANDLE;
    VkImageView depth_image_view = VK_NULL_HANDLE;
    VkImage aov_image = VK_NULL_HANDLE;
    VkDeviceMemory aov_image_memory = VK_NULL_HANDLE;
    VkImageView aov_array_view = VK_NULL_HANDLE;
    std::array<VkImageView,
               static_cast<std::size_t>(PathTraceAovLayer::Count)>
        aov_layer_views{};
    VkImage statistics_image = VK_NULL_HANDLE;
    VkDeviceMemory statistics_image_memory = VK_NULL_HANDLE;
    VkImageView statistics_image_view = VK_NULL_HANDLE;
    VkImage rr_motion_image = VK_NULL_HANDLE;
    VkDeviceMemory rr_motion_image_memory = VK_NULL_HANDLE;
    VkImageView rr_motion_image_view = VK_NULL_HANDLE;
    VkImage rr_diffuse_albedo_image = VK_NULL_HANDLE;
    VkDeviceMemory rr_diffuse_albedo_image_memory = VK_NULL_HANDLE;
    VkImageView rr_diffuse_albedo_image_view = VK_NULL_HANDLE;
    VkImage rr_specular_albedo_image = VK_NULL_HANDLE;
    VkDeviceMemory rr_specular_albedo_image_memory = VK_NULL_HANDLE;
    VkImageView rr_specular_albedo_image_view = VK_NULL_HANDLE;
    VkImage rr_normal_roughness_image = VK_NULL_HANDLE;
    VkDeviceMemory rr_normal_roughness_image_memory = VK_NULL_HANDLE;
    VkImageView rr_normal_roughness_image_view = VK_NULL_HANDLE;
    VkImage rr_specular_hit_distance_image = VK_NULL_HANDLE;
    VkDeviceMemory rr_specular_hit_distance_image_memory = VK_NULL_HANDLE;
    VkImageView rr_specular_hit_distance_image_view = VK_NULL_HANDLE;
    VkImage transparency_and_composition_image = VK_NULL_HANDLE;
    VkDeviceMemory transparency_and_composition_image_memory = VK_NULL_HANDLE;
    VkImageView transparency_and_composition_image_view = VK_NULL_HANDLE;
    VkImage reactive_mask_image = VK_NULL_HANDLE;
    VkDeviceMemory reactive_mask_image_memory = VK_NULL_HANDLE;
    VkImageView reactive_mask_image_view = VK_NULL_HANDLE;
    VkImage guide_validity_image = VK_NULL_HANDLE;
    VkDeviceMemory guide_validity_image_memory = VK_NULL_HANDLE;
    VkImageView guide_validity_image_view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t requested_output_mask = 0;
    std::uint32_t allocated_output_mask = 0;
    std::uint32_t image_count = 0;
    VkDeviceSize estimated_bytes = 0;
    VkDeviceSize allocated_bytes = 0;
    VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depth_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout aov_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout statistics_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout rr_motion_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout rr_diffuse_albedo_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout rr_specular_albedo_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout rr_normal_roughness_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout rr_specular_hit_distance_image_layout =
        VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout transparency_and_composition_image_layout =
        VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout reactive_mask_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout guide_validity_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
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
  void destroyTargetBundle(TargetBundle &bundle);
  void swapActiveTarget(TargetBundle &bundle) noexcept;
  void destroyImage();
  void destroyMotionFrame();
  [[nodiscard]] bool ensureMotionFrame();
  void destroyFallbackAlbedo();
  void destroyCaptureBuffer();
  [[nodiscard]] bool ensureCaptureBuffer(VkDeviceSize size);
  void flushPendingCapture();
  [[nodiscard]] bool ensureFallbackAlbedo();
  [[nodiscard]] bool ensureDummyStorageImages();
  void destroyDummyStorageImages();
  [[nodiscard]] bool queryTargetFormatCapabilities();
  [[nodiscard]] VkFormatFeatureFlags
  targetFormatFeatures(VkFormat format) const noexcept;
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
  VkSampler normal_sampler_ = VK_NULL_HANDLE;   // linear data, base level only
  VkSampler specular_sampler_ = VK_NULL_HANDLE; // linear data, base level only

  // 1x1 white fallback when the static model texture is unavailable.
  VkImage fallback_albedo_image_ = VK_NULL_HANDLE;
  VkDeviceMemory fallback_albedo_memory_ = VK_NULL_HANDLE;
  VkImageView fallback_albedo_view_ = VK_NULL_HANDLE;
  bool fallback_albedo_cleared_ = false;

  // Persistent exact-format 1x1 storage-image fallbacks. The RGBA16F image has
  // ten layers so it can provide both a 2D-array AOV view and a layer-zero 2D
  // view for disabled RR guides.
  VkImage dummy_rgba16_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_rgba16_memory_ = VK_NULL_HANDLE;
  VkImageView dummy_rgba16_array_view_ = VK_NULL_HANDLE;
  VkImageView dummy_rgba16_view_ = VK_NULL_HANDLE;
  VkImageLayout dummy_rgba16_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage dummy_rgba32_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_rgba32_memory_ = VK_NULL_HANDLE;
  VkImageView dummy_rgba32_view_ = VK_NULL_HANDLE;
  VkImageLayout dummy_rgba32_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage dummy_rg32_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_rg32_memory_ = VK_NULL_HANDLE;
  VkImageView dummy_rg32_view_ = VK_NULL_HANDLE;
  VkImageLayout dummy_rg32_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage dummy_r32_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_r32_memory_ = VK_NULL_HANDLE;
  VkImageView dummy_r32_view_ = VK_NULL_HANDLE;
  VkImageLayout dummy_r32_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage dummy_r8_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_r8_memory_ = VK_NULL_HANDLE;
  VkImageView dummy_r8_view_ = VK_NULL_HANDLE;
  VkImageLayout dummy_r8_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  ComputeDescriptorKey compute_descriptor_key_{};
  bool compute_descriptor_key_valid_ = false;
  std::uint64_t descriptor_write_calls_ = 0;
  std::uint64_t descriptor_cache_hits_ = 0;
  std::uint64_t descriptor_entries_written_ = 0;
  float descriptor_update_ms_ = 0.0f;
  std::uint32_t last_output_write_mask_ = 0;
  bool last_dispatch_recorded_ = false;
  std::uint32_t target_requirement_hint_mask_ = 0;
  std::uint32_t target_requested_output_mask_ = 0;
  std::uint32_t supported_target_output_mask_ = 0;
  std::uint32_t target_allocated_output_mask_ = 0;
  std::uint32_t target_image_count_ = 0;
  VkDeviceSize target_estimated_bytes_ = 0;
  VkDeviceSize target_allocated_bytes_ = 0;
  PathTraceTargetResult last_target_result_{};
  PathTraceTargetStats target_stats_{};
  std::uint32_t failed_target_width_ = 0;
  std::uint32_t failed_target_height_ = 0;
  std::uint32_t failed_target_output_mask_ = 0;
  std::uint32_t consecutive_target_failures_ = 0;
  std::uint64_t target_retry_not_before_ns_ = 0;
  VkFormatFeatureFlags rgba16_target_features_ = 0;
  VkFormatFeatureFlags rgba32_target_features_ = 0;
  VkFormatFeatureFlags rg32_target_features_ = 0;
  VkFormatFeatureFlags r32_target_features_ = 0;
  VkFormatFeatureFlags r8_target_features_ = 0;
  bool target_formats_queried_ = false;
  bool required_target_formats_supported_ = false;
  bool memory_budget_supported_ = false;
  bool descriptor_binding_partially_bound_enabled_ = false;

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
  VkImage transparency_and_composition_image_ = VK_NULL_HANDLE;
  VkDeviceMemory transparency_and_composition_image_memory_ = VK_NULL_HANDLE;
  VkImageView transparency_and_composition_image_view_ = VK_NULL_HANDLE;
  VkImage reactive_mask_image_ = VK_NULL_HANDLE;
  VkDeviceMemory reactive_mask_image_memory_ = VK_NULL_HANDLE;
  VkImageView reactive_mask_image_view_ = VK_NULL_HANDLE;
  VkImage guide_validity_image_ = VK_NULL_HANDLE;
  VkDeviceMemory guide_validity_image_memory_ = VK_NULL_HANDLE;
  VkImageView guide_validity_image_view_ = VK_NULL_HANDLE;
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
  VkImageLayout transparency_and_composition_image_layout_ =
      VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout reactive_mask_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout guide_validity_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
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
  std::uint64_t pending_capture_job_id_ = 0u;
  std::uint32_t pending_capture_background_face_size_ = 0u;
  std::vector<std::uint8_t> pending_capture_background_rgba8_;
  std::array<float, 16> pending_capture_background_inverse_view_projection_{};
  std::array<float, 3> pending_capture_background_camera_position_{};
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
  std::uint64_t runtime_capture_job_id_ = 0u;
  std::uint32_t runtime_capture_background_face_size_ = 0u;
  std::vector<std::uint8_t> runtime_capture_background_rgba8_;
  std::array<float, 16> runtime_capture_background_inverse_view_projection_{};
  std::array<float, 3> runtime_capture_background_camera_position_{};
};

} // namespace xpbd::gfx
