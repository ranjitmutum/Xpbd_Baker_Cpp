#pragma once

#include "xpbd/gfx/ray_tracing.hpp"
#include "xpbd/gfx/vulkan_rt_scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

namespace xpbd::gfx {

struct RtPipelineDispatchParams {
  const float *inverse_view_projection = nullptr;
  const float *view_projection = nullptr;
  float camera_position[3]{0.0f, 0.0f, 0.0f};
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  RtDebugView debug_view = RtDebugView::Off;
  std::uint32_t sample_base = 0;
  std::uint32_t sample_count = 1;
  PathTraceSettings settings{};
  std::uint32_t material_debug_view = 0;
  std::uint32_t material_feature_flags = 0;
  bool temporal_reconstruction_input = false;
  bool ray_reconstruction_guides = false;
  std::uint32_t output_write_mask = 0;
  float camera_jitter[2]{0.0f, 0.0f};
  std::uint32_t frame_index = 0;
  float light_direction[3]{0.35f, 0.85f, 0.40f};
  float ambient = 0.38f;
  float light_color[3]{1.0f, 1.0f, 1.0f};
  float light_intensity = 0.85f;
  bool hdr_environment = false;
  VkImageView environment_view = VK_NULL_HANDLE;
  VkSampler environment_sampler = VK_NULL_HANDLE;
  VkBuffer environment_distribution = VK_NULL_HANDLE;
  VkDeviceSize environment_distribution_bytes = 0;
  VkImageView output_view = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkImageView aov_array_view = VK_NULL_HANDLE;
  VkImageView statistics_view = VK_NULL_HANDLE;
  VkImageView rr_motion_view = VK_NULL_HANDLE;
  VkImageView rr_diffuse_albedo_view = VK_NULL_HANDLE;
  VkImageView rr_specular_albedo_view = VK_NULL_HANDLE;
  VkImageView rr_normal_roughness_view = VK_NULL_HANDLE;
  VkImageView rr_specular_hit_distance_view = VK_NULL_HANDLE;
  VkImageView albedo_view = VK_NULL_HANDLE;
  VkImageView normal_view = VK_NULL_HANDLE;
  VkImageView specular_view = VK_NULL_HANDLE;
  VkSampler albedo_sampler = VK_NULL_HANDLE;
  VkBuffer motion_frame_buffer = VK_NULL_HANDLE;
  VkDeviceSize motion_frame_buffer_bytes = 0;
};

struct RtPipelineStats {
  bool ready = false;
  std::uint32_t shader_group_count = 0;
  std::uint32_t shader_group_handle_size = 0;
  std::uint32_t shader_group_handle_alignment = 0;
  std::uint32_t shader_group_base_alignment = 0;
  std::uint32_t shader_group_stride = 0;
  std::uint64_t sbt_bytes = 0;
  // Per-record descriptor instrumentation.  A cache hit means the descriptor
  // set already described the same handles, ranges, and image views.
  std::uint64_t descriptor_write_calls = 0;
  std::uint64_t descriptor_cache_hits = 0;
  std::uint64_t descriptor_entries_written = 0;
  float descriptor_update_ms = 0.0f;
};

class VulkanRtPipeline {
public:
  VulkanRtPipeline() = default;
  ~VulkanRtPipeline() { shutdown(); }

  VulkanRtPipeline(const VulkanRtPipeline &) = delete;
  VulkanRtPipeline &operator=(const VulkanRtPipeline &) = delete;

  // Returns false when the optional RT-pipeline extension/procedures are not
  // available or a pipeline/SBT validation step fails. Ray-query remains usable.
  [[nodiscard]] bool init(VkPhysicalDevice physical_device, VkDevice device);
  void shutdown();

  [[nodiscard]] bool ready() const noexcept {
    return pipeline_ != VK_NULL_HANDLE && sbt_.buffer != VK_NULL_HANDLE &&
           vkCmdTraceRaysKHR_ != nullptr;
  }

  [[nodiscard]] bool record(
      VkCommandBuffer command_buffer, const VulkanRtScene &scene,
      const RtPipelineDispatchParams &params);

  // Reset the per-record descriptor counters before a new dispatch attempt.
  // The pipeline is owned by one in-flight path-tracer slot, so this is safe
  // immediately before recording that slot's command buffer.
  void resetDescriptorStats() noexcept {
    descriptor_write_calls_ = 0;
    descriptor_cache_hits_ = 0;
    descriptor_entries_written_ = 0;
    descriptor_update_ms_ = 0.0f;
  }

  [[nodiscard]] RtPipelineStats stats() const noexcept;

private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    void *mapped = nullptr;
  };

  struct DescriptorKey {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    std::array<VkImageView, 13> image_views{};
    std::array<VkSampler, 13> image_samplers{};
    std::array<VkBuffer, 14> buffers{};
    std::array<VkDeviceSize, 14> buffer_sizes{};

    [[nodiscard]] constexpr bool operator==(const DescriptorKey &) const =
        default;
  };

  [[nodiscard]] bool loadProcedures();
  [[nodiscard]] std::uint32_t
  findMemoryType(std::uint32_t type_bits,
                 VkMemoryPropertyFlags properties) const;
  [[nodiscard]] bool createBuffer(VkDeviceSize size,
                                  VkBufferUsageFlags usage, Buffer &buffer);
  void destroyBuffer(Buffer &buffer);
  [[nodiscard]] VkShaderModule
  createShaderModule(const std::uint32_t *words,
                     std::size_t word_count) const;
  [[nodiscard]] bool createDescriptorResources();
  [[nodiscard]] bool createPipelineAndSbt();

  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  Buffer sbt_{};
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties_{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  VkStridedDeviceAddressRegionKHR raygen_region_{};
  VkStridedDeviceAddressRegionKHR miss_region_{};
  VkStridedDeviceAddressRegionKHR hit_region_{};
  VkStridedDeviceAddressRegionKHR callable_region_{};
  std::uint32_t shader_group_stride_ = 0;
  bool dispatch_logged_ = false;
  bool bounds_failure_logged_ = false;
  DescriptorKey descriptor_key_{};
  bool descriptor_key_valid_ = false;
  std::uint64_t descriptor_write_calls_ = 0;
  std::uint64_t descriptor_cache_hits_ = 0;
  std::uint64_t descriptor_entries_written_ = 0;
  float descriptor_update_ms_ = 0.0f;

  PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR_ = nullptr;
  PFN_vkGetRayTracingShaderGroupHandlesKHR
      vkGetRayTracingShaderGroupHandlesKHR_ = nullptr;
  PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR_ = nullptr;
  PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress_ = nullptr;
};

} // namespace xpbd::gfx
