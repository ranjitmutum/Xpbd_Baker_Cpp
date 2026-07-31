#pragma once

#include "xpbd/gfx/frame_stats.hpp"
#include "xpbd/gfx/rt_scene_records.hpp"

// Vulkan ray-tracing scene helpers. Rigid bone groups, dynamic overlays, and
// preview-environment geometry keep distinct BLAS policies while sharing one
// TLAS visibility domain.

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace xpbd::gfx {

struct MeshVertex;

inline constexpr std::uint32_t kRtPrimitiveTextured = 1u << 0u;
inline constexpr std::uint32_t kRtPrimitiveCutout = 1u << 1u;
inline constexpr std::uint32_t kRtPrimitiveBlend = 1u << 2u;
inline constexpr std::uint32_t kRtPrimitiveEnvironment = 1u << 3u;

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
  // Average premultiplied emitted radiance per packed triangle. This is used
  // to build the world-space GPU mesh-light alias table after rigid instance
  // transforms/tints have been applied.
  std::vector<std::array<float, 3>> primitive_emission;
  std::vector<RtRestGeometryRange> geometry_ranges;
};

// Non-indexed world-space triangle list appended as a distinct BLAS.
// `alpha_blended` identifies the source raster range; per-vertex alpha remains
// authoritative for the exact transmittance.
struct RtColoredGeometryView {
  const MeshVertex *vertices = nullptr;
  std::size_t vertex_count = 0;
  bool alpha_blended = false;
  RtGeometryKind kind = RtGeometryKind::StaticScene;
  RtBlasPolicy blas_policy = RtBlasPolicy::StaticBuildCompact;
};

struct RtSceneStats {
  std::uint32_t blas_count = 0;
  std::uint32_t tlas_count = 0;
  std::uint32_t instance_count = 0;
  std::uint32_t primitive_count = 0;
  std::uint64_t as_storage_bytes = 0;
  std::uint64_t scratch_bytes = 0;
  std::uint64_t attribute_bytes = 0;
  std::uint64_t allocated_bytes = 0;
  std::uint64_t full_builds = 0;
  std::uint64_t refits = 0;
  RtAccelerationBuildReason last_build_reason =
      RtAccelerationBuildReason::None;
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

  // True once TLAS objects exist and at least one build has been prepared.
  // GPU build may still be pending in the current frame command buffer.
  [[nodiscard]] bool ready() const noexcept {
    return initialized_ && procs_ok_ && tlas_.handle != VK_NULL_HANDLE &&
           geometry_prepared_;
  }

  // Replace rest-pose geometry (called when the static model rebuilds).
  void setRestGeometry(RtRestGeometry geometry);

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
      bool explicit_motion_history_valid = false);

  // Record pending BLAS/TLAS build or refit into cmd (before path-trace /
  // ray-query draws). No-op when nothing is pending. Inserts barriers so
  // subsequent compute/fragment ray queries see the new AS.
  void recordBuilds(VkCommandBuffer cmd);

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
  [[nodiscard]] std::uint32_t emissiveTriangleCount() const noexcept {
    return emissive_triangle_count_;
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
  void submitOneShot(VkCommandBuffer cmd);
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
      const float *previous_bone_transforms_column_major,
      std::size_t previous_bone_count, bool motion_history_valid);
  void recordBlasBuild(VkCommandBuffer cmd, GeometryState &state, bool update);
  void recordTlasBuild(VkCommandBuffer cmd);
  void destroyGeometryStates();

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  std::uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  bool initialized_ = false;
  bool procs_ok_ = false;
  bool geometry_prepared_ = false;
  bool static_attribs_uploaded_ = false; // indices + UVs for current topology
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
  Buffer host_instance_metadata_{}; // uvec4 per instance
  // mat4 current + mat4 previous + uvec4 metadata per instance.
  Buffer host_instance_motion_{};
  // uvec4 header followed by one std430 mesh-light record per primitive.
  Buffer host_emissive_triangles_{};
  Buffer scratch_{};
  Buffer instance_buffer_{};
  std::vector<GeometryState> geometry_states_;
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
  std::uint32_t last_vertex_count_ = 0;
  std::uint32_t last_index_count_ = 0;
  std::uint32_t emissive_triangle_count_ = 0;
  bool motion_history_valid_ = false;
  VkDeviceSize tlas_build_scratch_ = 0;
  std::uint64_t full_build_count_ = 0;
  std::uint64_t refit_count_ = 0;
  RtAccelerationBuildReason pending_build_reason_ =
      RtAccelerationBuildReason::None;
  RtAccelerationBuildReason last_build_reason_ =
      RtAccelerationBuildReason::None;
  std::string last_update_failure_reason_;
};

} // namespace xpbd::gfx
