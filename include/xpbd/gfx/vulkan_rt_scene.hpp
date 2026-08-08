#pragma once

#include "xpbd/gfx/frame_stats.hpp"
#include "xpbd/gfx/render_thread_runtime.hpp"
#include "xpbd/gfx/rt_scene_records.hpp"
#include "xpbd/gfx/rt_scene_generations.hpp"

// Vulkan ray-tracing scene helpers. Rigid bone groups, dynamic overlays, and
// preview-environment geometry keep distinct BLAS policies while sharing one
// TLAS visibility domain.

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

namespace xpbd::gfx {

struct MeshVertex;

inline constexpr std::uint32_t kRtPrimitiveTextured = 1u << 0u;
inline constexpr std::uint32_t kRtPrimitiveCutout = 1u << 1u;
inline constexpr std::uint32_t kRtPrimitiveBlend = 1u << 2u;
inline constexpr std::uint32_t kRtPrimitiveEnvironment = 1u << 3u;
inline constexpr std::uint32_t kRtEmissiveTriangleTwoSided = 1u << 0u;

using RtRestGeometryRange = RtPackedGeometryRange;

// Rest-pose triangle mesh with rigid ranges in bone-local space.
struct RtRestGeometry {
  std::vector<float> positions;      // xyz * vertex_count
  std::vector<float> normals;        // xyz * vertex_count (optional rest normals)
  std::vector<float> uvs;            // uv * vertex_count (albedo sampling)
  std::vector<float> tangents;       // xyzw * vertex_count (w = handedness)
  std::vector<std::uint32_t> indices;
  std::vector<std::uint32_t> bone_indices; // per vertex
  std::vector<std::uint32_t> primitive_flags; // one flag word per triangle
  // uvec4 per triangle: cube, face, material, original source primitive.
  std::vector<std::array<std::uint32_t, 4>> primitive_metadata;
  std::vector<RtSurfaceOptics> primitive_optics;
  // Deterministic bounded estimate of premultiplied emitted radiance per
  // packed triangle. This builds the world-space mesh-light Alias table only;
  // the GPU evaluates actual radiance at each sampled barycentric UV.
  std::vector<std::array<float, 3>> primitive_emission;
  std::vector<RtRestGeometryRange> geometry_ranges;
};

enum class RtRestGeometryChange : std::uint8_t {
  Unchanged,
  MaterialOnly,
  AlphaClassification,
  Topology,
};

// Classifies a static-asset replacement without consulting Vulkan state.
// Material/texture records can update shader buffers in place; only a change
// to a range's opaque-vs-any-hit contract or true geometry topology permits a
// BLAS rebuild.
[[nodiscard]] inline RtRestGeometryChange classifyRtRestGeometryChange(
    const RtRestGeometry &current, const RtRestGeometry &next) noexcept {
  const auto ranges_equal = [](const RtRestGeometryRange &left,
                               const RtRestGeometryRange &right) {
    return left.kind == right.kind &&
           left.blas_policy == right.blas_policy &&
           left.instance_custom_index == right.instance_custom_index &&
           left.source_bone_index == right.source_bone_index &&
           left.first_index == right.first_index &&
           left.index_count == right.index_count;
  };
  if (current.positions != next.positions ||
      current.normals != next.normals || current.uvs != next.uvs ||
      current.tangents != next.tangents ||
      current.indices != next.indices ||
      current.bone_indices != next.bone_indices ||
      current.geometry_ranges.size() != next.geometry_ranges.size()) {
    return RtRestGeometryChange::Topology;
  }
  for (std::size_t index = 0u; index < current.geometry_ranges.size();
       ++index) {
    if (!ranges_equal(current.geometry_ranges[index],
                      next.geometry_ranges[index])) {
      return RtRestGeometryChange::Topology;
    }
  }

  constexpr std::uint32_t kAlphaFlags =
      kRtPrimitiveCutout | kRtPrimitiveBlend;
  bool alpha_changed =
      current.primitive_flags.size() != next.primitive_flags.size();
  if (!alpha_changed) {
    for (std::size_t primitive = 0u;
         primitive < current.primitive_flags.size(); ++primitive) {
      if ((current.primitive_flags[primitive] & kAlphaFlags) !=
          (next.primitive_flags[primitive] & kAlphaFlags)) {
        alpha_changed = true;
        break;
      }
    }
  }
  if (alpha_changed) {
    return RtRestGeometryChange::AlphaClassification;
  }

  const auto optics_equal = [](const RtSurfaceOptics &left,
                               const RtSurfaceOptics &right) {
    return left.transmission == right.transmission &&
           left.ior == right.ior &&
           left.attenuation_color == right.attenuation_color &&
           left.attenuation_distance == right.attenuation_distance &&
           left.thin_walled == right.thin_walled;
  };
  bool optics_match =
      current.primitive_optics.size() == next.primitive_optics.size();
  for (std::size_t primitive = 0u;
       optics_match && primitive < current.primitive_optics.size();
       ++primitive) {
    optics_match = optics_equal(current.primitive_optics[primitive],
                                next.primitive_optics[primitive]);
  }
  if (current.primitive_flags == next.primitive_flags &&
      current.primitive_metadata == next.primitive_metadata &&
      current.primitive_emission == next.primitive_emission &&
      optics_match) {
    return RtRestGeometryChange::Unchanged;
  }
  return RtRestGeometryChange::MaterialOnly;
}

// std430 mesh-light record cached on the CPU.  Keeping this typed cache alive
// across frames avoids reallocating and rebuilding the dense alias table when
// positions/transforms change in scenes that contain no emissive primitives.
struct alignas(16) RtEmissiveTriangleGpu {
  std::array<float, 4> p0_probability{};
  std::array<float, 4> p1_acceptance{};
  std::array<float, 4> p2_area{};
  // xyz = Alias-weight radiance estimate, w = its luminance. Never use xyz as
  // the actual radiance of a sampled texture point.
  std::array<float, 4> emission_luminance{};
  std::array<std::uint32_t, 4> metadata{};
  // xy: stable 64-bit light ID, z: sidedness flags, w: source primitive.
  std::array<std::uint32_t, 4> stable_light_id{};
};
static_assert(sizeof(RtEmissiveTriangleGpu) == 96u);

// std430 representation of the minimal optics seam. parameters stores
// transmission, IOR, attenuation distance, and a uint-bit Thin-Walled flag.
struct alignas(16) RtSurfaceOpticsGpu {
  std::array<float, 4> parameters{0.0f, 1.5f, 0.0f, 0.0f};
  std::array<float, 4> attenuation_color{1.0f, 1.0f, 1.0f, 0.0f};
};
static_assert(sizeof(RtSurfaceOpticsGpu) == 32u);

// Non-indexed world-space triangle list appended as a distinct BLAS.
// `alpha_blended` identifies the source raster range; per-vertex alpha remains
// authoritative for the exact transmittance.
struct RtColoredGeometryView {
  const MeshVertex *vertices = nullptr;
  std::size_t vertex_count = 0;
  bool alpha_blended = false;
  RtGeometryKind kind = RtGeometryKind::StaticScene;
  RtBlasPolicy blas_policy = RtBlasPolicy::StaticBuildCompact;
  // Zero means that a legacy caller did not provide a generation.  The
  // backend supplies non-zero values so release hot paths never hash vertices.
  std::uint64_t content_generation = 0;
  std::uint64_t topology_generation = 0;
  std::uint64_t material_generation = 0;
  std::uint64_t emission_generation = 0;
  RtSurfaceOptics surface_optics{};
};

struct RtSceneStats {
  std::uint32_t blas_count = 0;
  std::uint32_t tlas_count = 0;
  std::uint32_t instance_count = 0;
  std::uint32_t visible_instance_mask_count = 0;
  std::uint32_t hidden_instance_mask_count = 0;
  std::uint32_t positive_emitter_count = 0;
  std::uint32_t hidden_source_emitter_triangle_count = 0;
  std::uint32_t hidden_positive_weight_triangle_count = 0;
  std::uint32_t primitive_count = 0;
  std::uint64_t as_storage_bytes = 0;
  std::uint64_t scratch_bytes = 0;
  std::uint64_t attribute_bytes = 0;
  std::uint64_t allocated_bytes = 0;
  std::uint64_t cpu_rest_bytes = 0;
  std::uint64_t cpu_workspace_bytes = 0;
  std::uint64_t cpu_allocated_bytes = 0;
  std::uint64_t full_builds = 0;
  std::uint64_t refits = 0;
  std::uint64_t tlas_full_builds = 0;
  std::uint64_t tlas_updates = 0;
  std::uint64_t upload_bytes = 0;
  float emitter_distribution_ms = 0.0f;
  std::uint64_t emitter_distribution_rebuilds = 0;
  std::uint64_t descriptor_write_count = 0;
  std::uint64_t descriptor_cache_hits = 0;
  RtAccelerationBuildReason last_build_reason =
      RtAccelerationBuildReason::None;
  RtAccelerationBuildReason last_tlas_reason =
      RtAccelerationBuildReason::None;
};

enum class RtScenePendingBuildState : std::uint8_t {
  Idle,
  PendingFence,
  ReadyToCommit,
  Failed,
  CompletionUnproven,
};

class VulkanRtScene {
public:
  VulkanRtScene() = default;
  ~VulkanRtScene() { shutdown(); }

  VulkanRtScene(const VulkanRtScene &) = delete;
  VulkanRtScene &operator=(const VulkanRtScene &) = delete;

  [[nodiscard]] bool init(VkPhysicalDevice phys, VkDevice device,
                          std::uint32_t queue_family, VkQueue queue);
  void shutdown();

  void setWaitControl(
      std::shared_ptr<RenderThreadControl> control) noexcept {
    wait_control_ = std::move(control);
  }
  [[nodiscard]] bool completionUnproven() const noexcept {
    return completion_unproven_;
  }

  // True once TLAS objects exist and at least one build has been prepared.
  // GPU build may still be pending in the current frame command buffer.
  [[nodiscard]] bool ready() const noexcept {
    return initialized_ && procs_ok_ && tlas_.handle != VK_NULL_HANDLE &&
           geometry_prepared_;
  }

  // Replace rest-pose geometry (called when the static model rebuilds).
  void setRestGeometry(RtRestGeometry geometry) noexcept;

  // Update rigid TLAS transforms and dynamic world-space geometry, preparing
  // only the BLAS builds/refits required by each range's declared policy.
  // Call recordBuilds() on the frame command buffer before ray queries.
  // Returns false on failure or when both model and scene geometry are empty
  // (caller falls back to raster).
  [[nodiscard]] bool updateGeometry(
      const float *bone_transforms_column_major, std::size_t bone_count,
      const float *bone_tints_rgba = nullptr, std::size_t tint_count = 0,
      std::span<const RtColoredGeometryView> colored_geometry = {},
      bool include_rest_model = true,
      std::span<const float> previous_packed_positions = {},
      const float *previous_bone_transforms_column_major = nullptr,
      std::size_t previous_bone_count = 0,
      bool explicit_motion_history_valid = false,
      RtSceneGenerations generations = {},
      bool generations_valid = false);

  // Record pending BLAS/TLAS build or refit into cmd (before path-trace /
  // ray-query draws). No-op when nothing is pending. Inserts barriers so
  // subsequent compute/fragment ray queries and RT shaders see the new AS.
  void recordBuilds(VkCommandBuffer cmd);

  // Submit the prepared Candidate build without waiting. The scene owns the
  // one-shot command/fence until pollPendingBuild proves completion.
  [[nodiscard]] bool submitPendingBuild();
  [[nodiscard]] RtScenePendingBuildState
  pollPendingBuild(bool wait_for_completion = false);
  [[nodiscard]] bool pendingBuildSubmitted() const noexcept {
    return submitted_build_fence_ != VK_NULL_HANDLE;
  }

  // Complete the currently prepared BLAS/TLAS transaction on this scene's
  // private one-shot command pool and wait only for that submission.  This is
  // used by the synchronous A3 Candidate path: callers keep the published
  // VulkanRtScene untouched until this returns true, then exchange ownership
  // of the complete scene object atomically.
  [[nodiscard]] bool buildPendingAndWait();

  // Compatibility: CPU update + one-shot GPU build (blocks). Prefer
  // updateGeometry + recordBuilds on the frame command buffer.
  [[nodiscard]] bool updateAndBuild(
      const float *bone_transforms_column_major, std::size_t bone_count);

  [[nodiscard]] VkAccelerationStructureKHR tlas() const noexcept {
    return tlas_.handle;
  }

  // Write acceleration-structure descriptor (binding must be AS type).
  void writeTlasDescriptor(VkDescriptorSet set, std::uint32_t binding) const;

  // Path-tracer attribute buffers (world-space normals, UVs, triangle indices).
  // Valid after a successful updateGeometry().
  [[nodiscard]] VkBuffer normalBuffer() const noexcept {
    return host_normals_.buffer;
  }
  [[nodiscard]] VkBuffer indexAttribBuffer() const noexcept {
    return host_indices_.buffer;
  }
  [[nodiscard]] VkBuffer uvBuffer() const noexcept { return host_uvs_.buffer; }
  [[nodiscard]] VkBuffer colorBuffer() const noexcept {
    return host_colors_.buffer;
  }
  [[nodiscard]] VkBuffer tangentBuffer() const noexcept {
    return host_tangents_.buffer;
  }
  [[nodiscard]] VkBuffer primitiveFlagBuffer() const noexcept {
    return host_primitive_flags_.buffer;
  }
  [[nodiscard]] VkBuffer primitiveMetadataBuffer() const noexcept {
    return host_primitive_metadata_.buffer;
  }
  [[nodiscard]] VkBuffer primitiveOpticsBuffer() const noexcept {
    return host_primitive_optics_.buffer;
  }
  [[nodiscard]] VkBuffer instanceMetadataBuffer() const noexcept {
    return host_instance_metadata_.buffer;
  }
  [[nodiscard]] VkBuffer emissiveTriangleBuffer() const noexcept {
    return host_emissive_triangles_.buffer;
  }
  [[nodiscard]] VkBuffer positionBuffer() const noexcept {
    return host_vertices_.buffer;
  }
  [[nodiscard]] VkBuffer previousPositionBuffer() const noexcept {
    return host_previous_vertices_.buffer;
  }
  [[nodiscard]] VkBuffer instanceMotionBuffer() const noexcept {
    return host_instance_motion_.buffer;
  }
  [[nodiscard]] VkDeviceSize normalBufferBytes() const noexcept {
    return host_normals_.capacity;
  }
  [[nodiscard]] VkDeviceSize indexAttribBufferBytes() const noexcept {
    return host_indices_.capacity;
  }
  [[nodiscard]] VkDeviceSize uvBufferBytes() const noexcept {
    return host_uvs_.capacity;
  }
  [[nodiscard]] VkDeviceSize colorBufferBytes() const noexcept {
    return host_colors_.capacity;
  }
  [[nodiscard]] VkDeviceSize tangentBufferBytes() const noexcept {
    return host_tangents_.capacity;
  }
  [[nodiscard]] VkDeviceSize primitiveFlagBufferBytes() const noexcept {
    return host_primitive_flags_.capacity;
  }
  [[nodiscard]] VkDeviceSize primitiveMetadataBufferBytes() const noexcept {
    return host_primitive_metadata_.capacity;
  }
  [[nodiscard]] VkDeviceSize primitiveOpticsBufferBytes() const noexcept {
    return host_primitive_optics_.capacity;
  }
  [[nodiscard]] VkDeviceSize instanceMetadataBufferBytes() const noexcept {
    return host_instance_metadata_.capacity;
  }
  [[nodiscard]] VkDeviceSize emissiveTriangleBufferBytes() const noexcept {
    return host_emissive_triangles_.capacity;
  }
  [[nodiscard]] VkDeviceSize positionBufferBytes() const noexcept {
    return host_vertices_.capacity;
  }
  [[nodiscard]] VkDeviceSize previousPositionBufferBytes() const noexcept {
    return host_previous_vertices_.capacity;
  }
  [[nodiscard]] VkDeviceSize instanceMotionBufferBytes() const noexcept {
    return host_instance_motion_.capacity;
  }
  [[nodiscard]] std::span<const float> packedPositionSnapshot() const noexcept {
    return scratch_positions_;
  }
  [[nodiscard]] bool motionHistoryValid() const noexcept {
    return motion_history_valid_;
  }
  // The authoritative scene generations did not change for this rendered
  // frame. Shaders should use current geometry with the previous camera, not
  // replay the last object-motion sample stored in this frame slot.
  void markMotionStable() noexcept { motion_history_valid_ = false; }
  [[nodiscard]] std::uint32_t emissiveTriangleCount() const noexcept {
    return emissive_triangle_count_;
  }
  [[nodiscard]] float emissivePowerEstimate() const noexcept {
    return cached_emissive_power_estimate_;
  }
  [[nodiscard]] std::uint32_t pathTraceVertexCount() const noexcept {
    return last_vertex_count_;
  }
  [[nodiscard]] std::uint32_t pathTraceIndexCount() const noexcept {
    return last_index_count_;
  }
  [[nodiscard]] RtSceneStats stats() const noexcept;
  [[nodiscard]] const char *lastUpdateFailureReason() const noexcept {
    return last_update_failure_reason_.c_str();
  }

private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
    VkDeviceAddress address = 0;
    void *mapped = nullptr;
  };

  struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer buffer;
    VkDeviceAddress address = 0;
    VkDeviceSize buffer_size = 0;
  };

  struct BoneTransformCacheEntry {
    std::array<float, 16> transform{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Row-major inverse-transpose 3x3 (or the finite upper-3x3 fallback).
    std::array<float, 9> normal_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 9> tangent_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    float determinant_sign = 1.0f;
    bool valid = false;
    bool used_fallback = false;
  };

  enum class PendingBuild : std::uint8_t {
    None = 0,
    TopLevel = 1,
    Full = 2,
    Refit = 3,
  };

  struct GeometryState {
    RtGeometryKind kind = RtGeometryKind::StaticScene;
    RtBlasPolicy blas_policy = RtBlasPolicy::StaticBuildCompact;
    std::uint32_t instance_custom_index = 0;
    std::uint32_t source_bone_index = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint64_t content_hash = 0;
    VkDeviceSize build_scratch = 0;
    VkDeviceSize update_scratch = 0;
    bool built_once = false;
    bool pending_full_build = false;
    bool pending_refit = false;
    bool transform_history_valid = false;
    bool opaque_geometry = false;
    std::array<float, 16> current_transform{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 16> previous_transform{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    AccelerationStructure blas;
  };

  bool loadProcs();
  [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t type_bits,
                                             VkMemoryPropertyFlags props) const;
  [[nodiscard]] bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags mem_props,
                                  Buffer &out, bool map_host);
  void destroyBuffer(Buffer &b);
  void destroyAs(AccelerationStructure &as);
  [[nodiscard]] VkDeviceAddress bufferDeviceAddress(VkBuffer buffer) const;
  [[nodiscard]] VkCommandBuffer beginOneShot();
  [[nodiscard]] bool submitOneShotPending(VkCommandBuffer cmd);
  void releaseSubmittedBuild() noexcept;
  [[nodiscard]] bool ensureScratch(VkDeviceSize size);

  void fillTriangleGeometry(
      const GeometryState &state,
      VkAccelerationStructureGeometryKHR &geometry,
      VkAccelerationStructureGeometryTrianglesDataKHR &tri) const;
  [[nodiscard]] bool ensureBlasStorage(GeometryState &state,
                                       std::uint32_t vertex_count);
  [[nodiscard]] bool ensureTlasStorage(std::uint32_t instance_count);
  void writeInstanceData(
      const float *bone_transforms_column_major, std::size_t bone_count,
      const float *bone_tints_rgba, std::size_t tint_count,
      const float *previous_bone_transforms_column_major,
      std::size_t previous_bone_count, bool motion_history_valid);
  void recordBlasBuild(VkCommandBuffer cmd, GeometryState &state, bool update);
  void recordTlasBuild(VkCommandBuffer cmd, bool update);
  void destroyGeometryStates();

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  std::uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer submitted_build_command_ = VK_NULL_HANDLE;
  VkFence submitted_build_fence_ = VK_NULL_HANDLE;
  bool initialized_ = false;
  bool procs_ok_ = false;
  std::shared_ptr<RenderThreadControl> wait_control_;
  bool completion_unproven_ = false;
  bool submitted_build_ready_ = false;
  bool geometry_prepared_ = false;
  PendingBuild pending_ = PendingBuild::None;

  PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHR_ = nullptr;
  PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR_ =
      nullptr;
  PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR_ =
      nullptr;
  PFN_vkGetAccelerationStructureBuildSizesKHR
      vkGetAccelerationStructureBuildSizesKHR_ = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR
      vkGetAccelerationStructureDeviceAddressKHR_ = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR
      vkCmdBuildAccelerationStructuresKHR_ = nullptr;

  RtRestGeometry rest_{};
  Buffer host_vertices_{};
  Buffer host_previous_vertices_{};
  Buffer host_indices_{};
  Buffer host_normals_{}; // vec4 per vertex (xyz normal) for path tracer
  Buffer host_uvs_{};     // vec2 per vertex (uv) for path-tracer albedo
  Buffer host_colors_{};  // vec4 per vertex (bone tint / scene RGBA)
  Buffer host_tangents_{}; // vec4 per vertex (world tangent + handedness)
  Buffer host_primitive_flags_{}; // uint per triangle
  Buffer host_primitive_metadata_{}; // uvec4 per triangle
  Buffer host_primitive_optics_{}; // two vec4 per triangle
  Buffer host_instance_metadata_{}; // uvec4 per instance
  // mat4 current + mat4 previous + uvec4 metadata per instance.
  Buffer host_instance_motion_{};
  // uvec4 header followed by one std430 mesh-light record per primitive.
  Buffer host_emissive_triangles_{};
  Buffer scratch_{};
  Buffer instance_buffer_{};
  std::vector<GeometryState> geometry_states_;
  std::vector<BoneTransformCacheEntry> bone_transform_cache_;
  std::uint64_t bone_cache_generation_ = 0;
  bool bone_cache_generation_valid_ = false;
  AccelerationStructure tlas_{};
  std::vector<float> scratch_positions_;
  std::vector<float> previous_positions_;
  std::vector<float> scratch_normals_;
  std::vector<float> scratch_uvs_;
  std::vector<float> scratch_colors_;
  std::vector<float> scratch_tangents_;
  std::vector<std::uint32_t> scratch_indices_;
  std::vector<std::uint32_t> scratch_primitive_flags_;
  std::vector<std::array<std::uint32_t, 4>> scratch_primitive_metadata_;
  std::vector<RtSurfaceOpticsGpu> scratch_primitive_optics_;
  std::vector<RtEmissiveTriangleGpu> emissive_records_cache_;
  std::vector<double> emissive_weights_cache_;
  std::uint32_t cached_emissive_count_ = 0;
  float cached_emissive_power_estimate_ = 0.0f;
  std::uint32_t cached_hidden_source_emitter_triangle_count_ = 0;
  std::uint32_t cached_hidden_positive_weight_triangle_count_ = 0;
  bool cached_positive_emission_source_ = false;
  bool emissive_distribution_valid_ = false;
  std::uint32_t last_vertex_count_ = 0;
  std::uint32_t last_index_count_ = 0;
  std::uint32_t visible_instance_mask_count_ = 0;
  std::uint32_t hidden_instance_mask_count_ = 0;
  std::uint32_t emissive_triangle_count_ = 0;
  bool motion_history_valid_ = false;
  VkDeviceSize tlas_build_scratch_ = 0;
  std::uint64_t full_build_count_ = 0;
  std::uint64_t refit_count_ = 0;
  std::uint64_t tlas_full_build_count_ = 0;
  std::uint64_t tlas_update_count_ = 0;
  std::uint64_t upload_bytes_ = 0;
  float emitter_distribution_ms_ = 0.0f;
  std::uint64_t emitter_distribution_rebuild_count_ = 0;
  mutable std::uint64_t descriptor_write_count_ = 0;
  std::uint64_t descriptor_cache_hits_ = 0;
  VkDeviceSize tlas_update_scratch_ = 0;
  bool tlas_built_once_ = false;
  bool tlas_requires_full_build_ = true;
  RtSceneGenerations last_generations_{};
  bool generations_valid_ = false;
  std::uint64_t uploaded_topology_generation_ = 0;
  std::uint64_t uploaded_positions_generation_ = 0;
  std::uint64_t uploaded_transforms_generation_ = 0;
  std::uint64_t uploaded_material_generation_ = 0;
  std::uint64_t uploaded_emission_generation_ = 0;
  bool uploaded_topology_valid_ = false;
  bool uploaded_positions_valid_ = false;
  bool uploaded_transforms_valid_ = false;
  bool uploaded_material_valid_ = false;
  bool uploaded_emission_valid_ = false;
  RtAccelerationBuildReason pending_build_reason_ =
      RtAccelerationBuildReason::None;
  RtAccelerationBuildReason last_build_reason_ =
      RtAccelerationBuildReason::None;
  RtAccelerationBuildReason last_tlas_reason_ =
      RtAccelerationBuildReason::None;
  std::string last_update_failure_reason_;
};

} // namespace xpbd::gfx
