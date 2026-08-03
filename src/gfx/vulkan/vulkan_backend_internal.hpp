#pragma once

// Stable private declaration boundary for the Vulkan backend implementation.
// This header intentionally exposes the complete private value layout so the
// backend can be split across translation units without changing ownership or
// its public interface.

#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/preview_scene.hpp"
#include "xpbd/gfx/ray_tracing.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"
#include "xpbd/gfx/streamline_vulkan_runtime.hpp"
#include "xpbd/gfx/vulkan_path_tracer.hpp"
#include "xpbd/gfx/vulkan_queue_selection.hpp"
#include "xpbd/gfx/vulkan_rt_scene.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace xpbd::gfx::detail {

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

struct SwapchainSupport {
  VkSurfaceCapabilitiesKHR caps{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> modes;
};

class VulkanBackend final : public IGpuBackend {
public:
  bool init(SDL_Window *window) override;
  void shutdown() override;
  void resize(int, int) override;
  bool uploadFontAtlas(const void *pixels, int width, int height,
                       bool is_rgba) override;
  unsigned int fontTextureId() const override;
  bool supportsStaticModel() const override;
  void beginLatencyFrame(
      std::uint32_t frame_index, PathTraceReflexMode mode,
      bool frame_generation_requested) override;
  void endLatencySimulation() override;
  std::uint32_t latencyPingMessage() const override;
  void markLatencyPing() override;
  void render(const FrameInput &frame) override;
  void setVSync(bool enabled) override;
  bool vsyncEnabled() const override;
  BackendKind kind() const override;
  const char *name() const override;
  const char *deviceName() const override;
  FrameStats stats() const override;
  PathTracePostProcessCapabilities
  pathTracePostProcessCapabilities() const override;
  std::string_view pathTracePostProcessStatus() const override;
  RayTracingCapability rayTracingCapability() const override;
  bool supportsRayTracing() const override;
  RenderPath activeRenderPath() const override;
  void prepareForSystemDialog() override;
  void resumeAfterSystemDialog() override;
  bool presentationSuspended() const override;

private:
  using Clock = std::chrono::steady_clock;

  struct ImageResource;
  struct FrameSync;
  struct DynamicSkyCpuInput;
  struct DynamicSkyCpuResult;

  static const char *vkResultName(VkResult result);
  static void writeLog(const char *msg);
  static void appendPathTraceHistoryBytes(std::uint64_t &hash,
                                          const void *data,
                                          std::size_t byte_count);
  template <typename T>
  static void appendPathTraceHistoryValue(std::uint64_t &hash,
                                          const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    appendPathTraceHistoryBytes(hash, &value, sizeof(value));
  }

  [[nodiscard]] static DynamicSkyCpuResult buildDynamicSkyDistribution(
      const DynamicSkyCpuInput &input, VkDevice device, VkFence fence,
      Clock::time_point submitted_at);
  bool pollDynamicSkyEnvironmentCache(bool wait_for_completion = false);
  void discardDynamicSkyPending();
  void clearDynamicSkyEnvironmentCache();
  void clearProceduralAtmosphereImage();
  void destroyProceduralAtmosphereGpu();
  bool ensureProceduralAtmospherePipeline();
  bool createAtmosphereImage(std::uint32_t width, std::uint32_t height,
                             std::uint32_t depth, ImageResource &out);
  bool buildProceduralAtmosphereLuts(
      const ResolvedWorldEnvironment &resolved,
      const std::string &resource_key);
  std::string proceduralEnvironmentResourceKey(
      const ResolvedWorldEnvironment &resolved) const;
  bool buildDynamicSkyEnvironmentCache(
      const ResolvedWorldEnvironment &resolved,
      const std::string &environment_key);
  bool ensureProceduralAtmosphereResources(
      const ResolvedWorldEnvironment &resolved);
  void clearWorldEnvironmentResources();
  void destroyWorldEnvironmentGpu();
  std::uint64_t worldEnvironmentResourceKey(
      const ResolvedWorldEnvironment &resolved) const;
  bool ensureWorldEnvironmentSampler();
  bool uploadWorldEnvironment(const ResolvedWorldEnvironment &resolved);
  bool ensureWorldEnvironmentResources(
      const ResolvedWorldEnvironment &resolved);

  void destroySkyboxGpu();
  bool uploadSkyboxCubemap(const PreviewSkybox &sky);

  void destroyStaticModelResources();
  bool createStaticTexture(std::uint32_t width, std::uint32_t height,
                           VkFormat format, ImageResource &out);
  void destroyStaticMaterialSamplers();
  void updateStaticTextureDescriptors();
  void updateStaticBoneDescriptor(FrameSync &frame);
  bool rebuildStaticModelResources(
      const StaticIndexedModelMesh &mesh, const TextureImage *texture,
      const ResolvedMaterialTable *material, std::uint64_t model_generation,
      std::uint64_t texture_generation, std::uint64_t &uploaded_bytes);

  void failActiveStillRender(const FrameInput &frame,
                             const std::string &detail);
  void enterFatalVulkanError(const FrameInput &frame, const char *api,
                             VkResult result);
  void recordFatalVulkanError(const char *api, VkResult result);
  [[nodiscard]] bool presentFenceLifecycleEnabled() const noexcept;
  [[nodiscard]] bool
  frameGenerationPlatformSupported() const noexcept;
  [[nodiscard]] bool
  frameGenerationSwapchainReady() const noexcept;


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
                        bool force = false, bool flush_after = false) const;
  void logDiagnosticFrame(const FrameInput &frame,
                          const FrameSync &sync) const;
  void logDiagnosticPerf(const FrameSync &sync) const;
  void logDiagnosticResources(const FrameInput &frame,
                              const FrameSync &sync,
                              VkDeviceSize requested_bone_bytes,
                              VkDeviceSize requested_mesh_bytes,
                              VkDeviceSize requested_ui_vertex_bytes,
                              VkDeviceSize requested_ui_index_bytes,
                              bool force = false) const;
  static void mulMat(const float *a, const float *b, float *o);
  static void glMvpToVulkan(const float *m, float *o);
  void destroyGraphicsPipelines();
  [[nodiscard]] bool graphicsPipelinesReady() const;
  std::optional<uint32_t>
  findMemoryType(uint32_t bits, VkMemoryPropertyFlags props) const;


  struct MemoryHeapDiagnostic {
    std::uint32_t memory_type = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t heap = (std::numeric_limits<std::uint32_t>::max)();
    VkDeviceSize budget = 0;
    VkDeviceSize usage = 0;
  };
  [[nodiscard]] MemoryHeapDiagnostic
  memoryHeapDiagnostic(std::uint32_t memory_type) const;
  void logBufferResourceError(const char *api, VkResult result,
                              const char *resource_name, VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              std::uint32_t memory_type);
  void logImageResourceError(const char *api, VkResult result,
                             const char *resource_name, VkFormat format,
                             std::uint32_t width, std::uint32_t height,
                             std::uint32_t depth, VkImageUsageFlags usage,
                             VkDeviceSize allocation_size,
                             std::uint32_t memory_type);
  void destroyBuffer(Buffer &b);
  bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props, Buffer &out,
                    const char *resource_name = "buffer");
  bool ensureBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage,
                    bool *reallocated = nullptr);
  bool uploadBuffer(Buffer &b, VkDeviceSize offset, VkDeviceSize bytes,
                    const void *src);
  void destroyImage(ImageResource &image);
  bool createFrameGenerationImage(std::uint32_t width,
                                  std::uint32_t height,
                                  VkImageUsageFlags usage,
                                  ImageResource &out);

  void readCompletedTimestamps(FrameSync &frame);

  bool pickDevice();
  bool supportsDeviceExtension(VkPhysicalDevice device,
                               const char *extension_name) const;
  bool supportsRequiredDeviceExtensions(VkPhysicalDevice device) const;
  [[nodiscard]] static bool
  deviceHasExtension(const std::vector<VkExtensionProperties> &props,
                     const char *name);
  void probeRayTracingCapability();
  bool createDevice();
  bool querySupport(VkPhysicalDevice device, SwapchainSupport &s) const;
  bool createSwapchain(
      VkSwapchainKHR old_swapchain = VK_NULL_HANDLE,
      SwapchainOwnership target_ownership = SwapchainOwnership::Native);
  bool waitForPendingPresentFences(const char *reason);
  void destroySwapchainImageObjects();
  void destroySwapchainObjects();
  bool recreateSwapchain();
  bool createRenderPass();
  bool createFramebuffers();
  bool createDescriptors();
  VkShaderModule makeModule(const uint32_t *words, size_t word_count);

  bool createPipelines();
  bool createBuffers();
  bool createCommandPool();
  bool createSync();
  void createTimestampQueryPools();


  int drawUi(FrameSync &frame, const UiDrawData &ui, bool overlay_only);
};

} // namespace xpbd::gfx::detail
