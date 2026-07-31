#pragma once

#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xpbd::gfx {

enum class RtGeometryKind : std::uint8_t {
  StaticScene,
  RigidModel,
  SkinnedModel,
  Ocean,
};

enum class RtBlasPolicy : std::uint8_t {
  StaticBuildCompact,
  RigidLocalSpace,
  DynamicRefit,
};

enum class RtGeometryUpdateKind : std::uint8_t {
  None,
  FullBuild,
  Refit,
};

struct RtTransformedNormal {
  std::array<float, 3> value{0.0f, 1.0f, 0.0f};
  bool used_fallback = false;
};

struct RtPrimitiveRecord {
  std::uint32_t primitive_index = 0;
  std::uint32_t geometry_index = 0;
  std::uint32_t source_face_index = 0;
  std::uint32_t source_triangle_index = 0;
  std::uint32_t bone_index = 0;
  std::uint32_t cube_index = 0;
  StaticModelFaceDirection face_direction =
      StaticModelFaceDirection::West;
  StaticModelMaterialClass opacity_class =
      StaticModelMaterialClass::Opaque;
  std::uint32_t material_index = 0;
  std::array<std::uint32_t, 3> vertex_indices{};
  std::array<float, 6> uvs{};
  bool textured = false;
  bool uses_emission_texture = false;
};

struct RtGeometryRecord {
  std::uint32_t geometry_index = 0;
  RtGeometryKind kind = RtGeometryKind::StaticScene;
  RtBlasPolicy blas_policy = RtBlasPolicy::StaticBuildCompact;
  std::uint32_t source_bone_index = 0;
  std::string name;
  std::vector<std::uint32_t> primitive_indices;
  bool local_space = false;
  bool dynamic_vertices = false;
};

struct RtMaterialRecord {
  std::uint32_t material_index = 0;
  std::uint32_t feature_flags = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool read_only = true;
  bool normal_map_active = false;
  bool specular_map_active = false;
};

struct RtInstanceRecord {
  std::uint32_t instance_custom_index = 0;
  std::uint32_t geometry_index = 0;
  std::uint32_t source_bone_index = 0;
  std::array<float, 16> current_transform{
      1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::array<float, 16> previous_transform{
      1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  bool history_valid = false;
};

struct RtPackedGeometryRange {
  RtGeometryKind kind = RtGeometryKind::StaticScene;
  RtBlasPolicy blas_policy = RtBlasPolicy::StaticBuildCompact;
  std::uint32_t instance_custom_index = 0;
  std::uint32_t source_bone_index = 0;
  std::uint32_t first_index = 0;
  std::uint32_t index_count = 0;
};

struct RtPackedPrimitiveLayout {
  std::vector<std::uint32_t> indices;
  std::vector<std::uint32_t> source_primitive_indices;
  std::vector<RtPackedGeometryRange> geometry_ranges;
  std::vector<std::string> errors;

  [[nodiscard]] bool valid() const noexcept {
    return errors.empty() && indices.size() % 3u == 0u &&
           source_primitive_indices.size() == indices.size() / 3u;
  }
};

struct RtResolvedPrimitiveIdentity {
  std::uint32_t packed_primitive_index = 0;
  std::uint32_t source_primitive_index = 0;
  std::uint32_t instance_custom_index = 0;
  std::uint32_t local_primitive_index = 0;
  std::uint32_t geometry_index = 0;
  std::uint32_t bone_index = 0;
  std::uint32_t cube_index = 0;
  StaticModelFaceDirection face_direction =
      StaticModelFaceDirection::West;
  std::uint32_t material_index = 0;
};

struct RtSceneRecords {
  std::vector<RtGeometryRecord> geometries;
  std::vector<RtInstanceRecord> instances;
  std::vector<RtPrimitiveRecord> primitives;
  std::vector<RtMaterialRecord> materials;
  std::vector<std::string> errors;

  [[nodiscard]] bool valid() const noexcept {
    return errors.empty() && geometries.size() == instances.size() &&
           (primitives.empty() || !materials.empty());
  }
};

[[nodiscard]] RtSceneRecords buildRigidModelRtSceneRecords(
    const StaticIndexedModelMesh &mesh, const StaticModelDrawPlan &draw_plan,
    const ResolvedMaterialTable *material);

[[nodiscard]] RtPackedPrimitiveLayout buildRtPackedPrimitiveLayout(
    const StaticModelDrawPlan &draw_plan, const RtSceneRecords &records);

// Mirrors the GPU hit mapping:
// instanceMetadata[instanceCustomIndex].x + localPrimitiveIndex.
[[nodiscard]] std::optional<RtResolvedPrimitiveIdentity>
resolveRtPackedPrimitiveIdentity(
    const RtPackedPrimitiveLayout &layout, const RtSceneRecords &records,
    std::uint32_t instance_custom_index,
    std::uint32_t local_primitive_index) noexcept;

bool updateRigidRtInstanceTransforms(
    RtSceneRecords &records,
    std::span<const StaticModelBoneState> bone_states,
    bool reset_history = false);

[[nodiscard]] RtGeometryUpdateKind classifyRtGeometryUpdate(
    std::uint64_t previous_content_hash, std::uint64_t next_content_hash,
    RtBlasPolicy policy, bool built_once) noexcept;

[[nodiscard]] RtTransformedNormal transformRtNormalInverseTranspose(
    std::span<const float, 16> column_major_transform,
    const std::array<float, 3> &normal) noexcept;

} // namespace xpbd::gfx
