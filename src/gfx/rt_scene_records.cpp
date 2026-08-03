#include "xpbd/gfx/rt_scene_records.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace xpbd::gfx {

RtSceneRecords buildRigidModelRtSceneRecords(
    const StaticIndexedModelMesh &mesh, const StaticModelDrawPlan &draw_plan,
    const ResolvedMaterialTable *material,
    const RtSurfaceOptics *surface_optics_override) {
  RtSceneRecords result;
  if (draw_plan.indices.size() % 3u != 0u ||
      draw_plan.primitive_materials.size() !=
          draw_plan.indices.size() / 3u) {
    result.errors.emplace_back(
        "draw plan primitive metadata does not match its index buffer");
    return result;
  }
  if (mesh.bone_names.size() >
      static_cast<std::size_t>(0x00ffffffu)) {
    result.errors.emplace_back(
        "model exceeds Vulkan instanceCustomIndex capacity");
    return result;
  }

  std::map<std::uint32_t, std::vector<std::uint32_t>> bone_primitives;
  result.primitives.reserve(draw_plan.primitive_materials.size());
  RtMaterialRecord material_record;
  material_record.feature_flags = labPbrFeatureFlags(material);
  if (surface_optics_override != nullptr) {
    material_record.surface_optics =
        normalizeRtSurfaceOptics(*surface_optics_override);
  }
  if (material != nullptr) {
    material_record.width =
        static_cast<std::uint32_t>((std::max)(material->width, 0));
    material_record.height =
        static_cast<std::uint32_t>((std::max)(material->height, 0));
    material_record.normal_map_active = material->normal_map_active;
    material_record.specular_map_active = material->specular_map_active;
  }
  result.materials.push_back(material_record);
  const bool emission_texture =
      material != nullptr && material->specular_map_active;
  for (std::size_t primitive_index = 0;
       primitive_index < draw_plan.primitive_materials.size();
       ++primitive_index) {
    const auto &source = draw_plan.primitive_materials[primitive_index];
    if (source.source_face_index >= mesh.faces.size() ||
        source.bone_index >= mesh.bone_names.size()) {
      result.errors.emplace_back(
          "primitive source identity references an invalid face or bone");
      continue;
    }
    const auto &face = mesh.faces[source.source_face_index];
    if (!detail::validFace(mesh, face) ||
        face.bone_index != source.bone_index ||
        face.cube_index != source.cube_index ||
        face.direction != source.face_direction ||
        source.source_triangle_index >= face.index_count / 3u) {
      result.errors.emplace_back(
          "primitive source identity does not match canonical face metadata");
      continue;
    }

    RtPrimitiveRecord primitive;
    primitive.primitive_index =
        static_cast<std::uint32_t>(primitive_index);
    primitive.source_face_index = source.source_face_index;
    primitive.source_triangle_index = source.source_triangle_index;
    primitive.bone_index = source.bone_index;
    primitive.cube_index = source.cube_index;
    primitive.face_direction = source.face_direction;
    primitive.opacity_class = source.material;
    primitive.textured = source.textured;
    primitive.uses_emission_texture =
        source.textured && emission_texture;
    for (std::size_t corner = 0; corner < 3u; ++corner) {
      const std::size_t plan_index = primitive_index * 3u + corner;
      const std::uint32_t vertex_index =
          draw_plan.indices[plan_index];
      if (vertex_index >= mesh.vertices.size()) {
        result.errors.emplace_back(
            "primitive index references an invalid canonical vertex");
        break;
      }
      if (mesh.vertices[vertex_index].bone_index !=
          source.bone_index) {
        result.errors.emplace_back(
            "primitive vertex crosses a rigid bone geometry boundary");
        break;
      }
      primitive.vertex_indices[corner] = vertex_index;
      primitive.uvs[corner * 2u] = mesh.vertices[vertex_index].u;
      primitive.uvs[corner * 2u + 1u] =
          mesh.vertices[vertex_index].v;
    }
    bone_primitives[source.bone_index].push_back(
        primitive.primitive_index);
    result.primitives.push_back(std::move(primitive));
  }
  if (!result.errors.empty()) {
    result.geometries.clear();
    result.instances.clear();
    result.primitives.clear();
    result.materials.clear();
    return result;
  }

  result.geometries.reserve(bone_primitives.size());
  result.instances.reserve(bone_primitives.size());
  for (auto &[bone_index, primitive_indices] : bone_primitives) {
    if (result.geometries.size() >
        static_cast<std::size_t>(0x00ffffffu)) {
      result.errors.emplace_back(
          "model geometry count exceeds Vulkan instanceCustomIndex capacity");
      result.geometries.clear();
      result.instances.clear();
      result.primitives.clear();
      result.materials.clear();
      return result;
    }
    const std::uint32_t geometry_index =
        static_cast<std::uint32_t>(result.geometries.size());
    RtGeometryRecord geometry;
    geometry.geometry_index = geometry_index;
    geometry.kind = RtGeometryKind::RigidModel;
    geometry.blas_policy = RtBlasPolicy::RigidLocalSpace;
    geometry.source_bone_index = bone_index;
    geometry.name = mesh.bone_names[bone_index];
    geometry.primitive_indices = std::move(primitive_indices);
    geometry.local_space = true;
    geometry.dynamic_vertices = false;
    for (const std::uint32_t primitive_index :
         geometry.primitive_indices) {
      result.primitives[primitive_index].geometry_index =
          geometry_index;
    }
    result.geometries.push_back(std::move(geometry));

    RtInstanceRecord instance;
    instance.instance_custom_index = geometry_index;
    instance.geometry_index = geometry_index;
    instance.source_bone_index = bone_index;
    result.instances.push_back(instance);
  }
  return result;
}

RtPackedPrimitiveLayout buildRtPackedPrimitiveLayout(
    const StaticModelDrawPlan &draw_plan, const RtSceneRecords &records) {
  RtPackedPrimitiveLayout result;
  if (!records.valid()) {
    result.errors.emplace_back("cannot pack invalid RT scene records");
    return result;
  }
  if (draw_plan.indices.size() % 3u != 0u ||
      draw_plan.primitive_materials.size() !=
          draw_plan.indices.size() / 3u ||
      records.primitives.size() !=
          draw_plan.primitive_materials.size()) {
    result.errors.emplace_back(
        "draw plan and RT primitive records do not match");
    return result;
  }

  std::vector<bool> seen(records.primitives.size(), false);
  result.indices.reserve(draw_plan.indices.size());
  result.source_primitive_indices.reserve(records.primitives.size());
  result.geometry_ranges.reserve(records.geometries.size());
  for (std::size_t geometry_index = 0;
       geometry_index < records.geometries.size(); ++geometry_index) {
    const RtGeometryRecord &geometry = records.geometries[geometry_index];
    const RtInstanceRecord &instance = records.instances[geometry_index];
    if (geometry.geometry_index != geometry_index ||
        instance.geometry_index != geometry_index ||
        instance.instance_custom_index != geometry_index ||
        instance.source_bone_index != geometry.source_bone_index ||
        geometry.primitive_indices.empty()) {
      result.errors.emplace_back(
          "RT geometry/instance records are not stable and dense");
      break;
    }
    RtPackedGeometryRange range;
    range.kind = geometry.kind;
    range.blas_policy = geometry.blas_policy;
    range.instance_custom_index = instance.instance_custom_index;
    range.source_bone_index = geometry.source_bone_index;
    range.first_index =
        static_cast<std::uint32_t>(result.indices.size());
    for (const std::uint32_t primitive_index :
         geometry.primitive_indices) {
      if (primitive_index >= records.primitives.size() ||
          seen[primitive_index] ||
          records.primitives[primitive_index].primitive_index !=
              primitive_index ||
          records.primitives[primitive_index].geometry_index !=
              geometry_index) {
        result.errors.emplace_back(
            "RT primitive record cannot be packed exactly once");
        break;
      }
      seen[primitive_index] = true;
      const std::size_t source_index =
          static_cast<std::size_t>(primitive_index) * 3u;
      result.indices.insert(
          result.indices.end(),
          draw_plan.indices.begin() + source_index,
          draw_plan.indices.begin() + source_index + 3u);
      result.source_primitive_indices.push_back(primitive_index);
    }
    if (!result.errors.empty()) {
      break;
    }
    range.index_count =
        static_cast<std::uint32_t>(result.indices.size()) -
        range.first_index;
    result.geometry_ranges.push_back(range);
  }
  if (result.errors.empty() &&
      std::find(seen.begin(), seen.end(), false) != seen.end()) {
    result.errors.emplace_back(
        "RT packed layout omitted one or more primitive records");
  }
  if (!result.errors.empty()) {
    result.indices.clear();
    result.source_primitive_indices.clear();
    result.geometry_ranges.clear();
  }
  return result;
}

std::optional<RtResolvedPrimitiveIdentity>
resolveRtPackedPrimitiveIdentity(
    const RtPackedPrimitiveLayout &layout, const RtSceneRecords &records,
    std::uint32_t instance_custom_index,
    std::uint32_t local_primitive_index) noexcept {
  if (!layout.valid() || !records.valid()) {
    return std::nullopt;
  }
  const auto range = std::find_if(
      layout.geometry_ranges.begin(), layout.geometry_ranges.end(),
      [&](const RtPackedGeometryRange &candidate) {
        return candidate.instance_custom_index == instance_custom_index;
      });
  if (range == layout.geometry_ranges.end() ||
      range->first_index % 3u != 0u ||
      range->index_count % 3u != 0u ||
      local_primitive_index >= range->index_count / 3u) {
    return std::nullopt;
  }
  const std::uint32_t packed_primitive =
      range->first_index / 3u + local_primitive_index;
  if (packed_primitive >= layout.source_primitive_indices.size()) {
    return std::nullopt;
  }
  const std::uint32_t source_primitive =
      layout.source_primitive_indices[packed_primitive];
  if (source_primitive >= records.primitives.size()) {
    return std::nullopt;
  }
  const auto instance = std::find_if(
      records.instances.begin(), records.instances.end(),
      [&](const RtInstanceRecord &candidate) {
        return candidate.instance_custom_index == instance_custom_index;
      });
  if (instance == records.instances.end() ||
      instance->geometry_index >= records.geometries.size()) {
    return std::nullopt;
  }
  const RtPrimitiveRecord &primitive = records.primitives[source_primitive];
  if (primitive.geometry_index != instance->geometry_index ||
      primitive.bone_index != instance->source_bone_index) {
    return std::nullopt;
  }
  return RtResolvedPrimitiveIdentity{
      packed_primitive,
      source_primitive,
      instance_custom_index,
      local_primitive_index,
      primitive.geometry_index,
      primitive.bone_index,
      primitive.cube_index,
      primitive.face_direction,
      primitive.material_index};
}

bool updateRigidRtInstanceTransforms(
    RtSceneRecords &records,
    std::span<const StaticModelBoneState> bone_states,
    bool reset_history) {
  if (!records.valid()) {
    return false;
  }
  for (auto &instance : records.instances) {
    if (instance.geometry_index >= records.geometries.size() ||
        instance.source_bone_index >= bone_states.size()) {
      return false;
    }
    const auto &geometry = records.geometries[instance.geometry_index];
    if (geometry.blas_policy != RtBlasPolicy::RigidLocalSpace ||
        geometry.source_bone_index != instance.source_bone_index) {
      return false;
    }
  }
  for (auto &instance : records.instances) {
    const auto &next =
        bone_states[instance.source_bone_index].transform;
    if (reset_history || !instance.history_valid) {
      instance.previous_transform = next;
    } else {
      instance.previous_transform = instance.current_transform;
    }
    instance.current_transform = next;
    instance.visibility_mask = rtInstanceVisibilityMask(
        bone_states[instance.source_bone_index].tint[3]);
    instance.history_valid = true;
  }
  return true;
}

RtGeometryUpdateKind classifyRtGeometryUpdate(
    std::uint64_t previous_content_hash, std::uint64_t next_content_hash,
    RtBlasPolicy policy, bool built_once) noexcept {
  if (previous_content_hash == next_content_hash) {
    return RtGeometryUpdateKind::None;
  }
  if (policy == RtBlasPolicy::DynamicRefit && built_once) {
    return RtGeometryUpdateKind::Refit;
  }
  return RtGeometryUpdateKind::FullBuild;
}

RtTransformedNormal transformRtNormalInverseTranspose(
    std::span<const float, 16> m,
    const std::array<float, 3> &normal) noexcept {
  RtTransformedNormal result;
  const auto normalize = [](float x, float y, float z,
                            std::array<float, 3> &out) {
    const float length_squared = x * x + y * y + z * z;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f) {
      return false;
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    out = {x * inverse_length, y * inverse_length, z * inverse_length};
    return std::isfinite(out[0]) && std::isfinite(out[1]) &&
           std::isfinite(out[2]);
  };

  const float a = m[0], b = m[4], c = m[8];
  const float d = m[1], e = m[5], f = m[9];
  const float g = m[2], h = m[6], i = m[10];
  const float c00 = e * i - f * h;
  const float c01 = f * g - d * i;
  const float c02 = d * h - e * g;
  const float c10 = c * h - b * i;
  const float c11 = a * i - c * g;
  const float c12 = b * g - a * h;
  const float c20 = b * f - c * e;
  const float c21 = c * d - a * f;
  const float c22 = a * e - b * d;
  const float determinant = a * c00 + b * c01 + c * c02;
  if (std::isfinite(determinant) && std::abs(determinant) > 1.0e-12f) {
    const float inverse_determinant = 1.0f / determinant;
    if (normalize((c00 * normal[0] + c01 * normal[1] +
                   c02 * normal[2]) *
                      inverse_determinant,
                  (c10 * normal[0] + c11 * normal[1] +
                   c12 * normal[2]) *
                      inverse_determinant,
                  (c20 * normal[0] + c21 * normal[1] +
                   c22 * normal[2]) *
                      inverse_determinant,
                  result.value)) {
      return result;
    }
  }

  result.used_fallback = true;
  if (normalize(a * normal[0] + b * normal[1] + c * normal[2],
                d * normal[0] + e * normal[1] + f * normal[2],
                g * normal[0] + h * normal[1] + i * normal[2],
                result.value) ||
      normalize(normal[0], normal[1], normal[2], result.value)) {
    return result;
  }
  result.value = {0.0f, 1.0f, 0.0f};
  return result;
}

} // namespace xpbd::gfx
