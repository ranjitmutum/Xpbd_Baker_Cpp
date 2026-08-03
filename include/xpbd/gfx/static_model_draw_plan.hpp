#pragma once

#include "xpbd/gfx/viewport_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace xpbd::gfx {

enum class StaticModelMaterialClass : std::uint8_t {
  Opaque,
  Cutout,
  Blend,
};

struct StaticModelIndexRange {
  std::uint32_t first_index = 0;
  std::uint32_t index_count = 0;
};

struct StaticModelPrimitiveMaterial {
  StaticModelMaterialClass material = StaticModelMaterialClass::Opaque;
  bool textured = false;
  std::uint32_t source_face_index = 0;
  std::uint32_t source_triangle_index = 0;
  std::uint32_t bone_index = 0;
  std::uint32_t cube_index = 0;
  StaticModelFaceDirection face_direction =
      StaticModelFaceDirection::West;
};




struct StaticModelDrawPlan {
  std::vector<std::uint32_t> indices;
  std::vector<StaticModelPrimitiveMaterial> primitive_materials;

  std::vector<std::uint8_t> textured_vertices;
  StaticModelIndexRange opaque;
  StaticModelIndexRange cutout;
  StaticModelIndexRange blend;
  std::uint32_t skipped_face_count = 0;
};


struct StaticModelGenerationCache {
  bool initialized = false;
  std::uint64_t model = 0;
  std::uint64_t texture = 0;

  [[nodiscard]] constexpr bool
  needsRefresh(std::uint64_t next_model,
               std::uint64_t next_texture) const noexcept {
    return !initialized || model != next_model || texture != next_texture;
  }

  constexpr void accept(std::uint64_t next_model,
                        std::uint64_t next_texture) noexcept {
    initialized = true;
    model = next_model;
    texture = next_texture;
  }
};

[[nodiscard]] inline bool
staticModelFrameMatchesMesh(const StaticIndexedModelMesh &mesh,
                            const StaticModelFrameData &frame) noexcept {
  return frame.bones.size() == mesh.bone_names.size();
}

namespace detail {

[[nodiscard]] inline bool rangeFits(std::size_t first, std::size_t count,
                                    std::size_t size) noexcept {
  return first <= size && count <= size - first;
}

struct TexelSpan {
  int begin = 0;
  int end = 0;
};

[[nodiscard]] inline TexelSpan coveredTexelSpan(double lo, double hi,
                                                int extent) noexcept {
  if (lo > hi) {
    std::swap(lo, hi);
  }
  lo = std::clamp(lo, 0.0, 1.0);
  hi = std::clamp(hi, 0.0, 1.0);
  if (hi - lo <= 1.0e-12) {
    const int texel = std::clamp(
        static_cast<int>(std::floor(lo * static_cast<double>(extent))), 0,
        extent - 1);
    return {texel, texel + 1};
  }
  const int first = std::clamp(
      static_cast<int>(std::floor(lo * static_cast<double>(extent))), 0,
      extent - 1);
  const int end = std::clamp(
      static_cast<int>(std::ceil(hi * static_cast<double>(extent))),
      first + 1, extent);
  return {first, end};
}

[[nodiscard]] inline bool validFace(const StaticIndexedModelMesh &mesh,
                                    const StaticModelFace &face) noexcept {
  if (face.vertex_count == 0 || face.index_count == 0 ||
      face.index_count % 3u != 0u ||
      !rangeFits(face.first_vertex, face.vertex_count, mesh.vertices.size()) ||
      !rangeFits(face.first_index, face.index_count, mesh.indices.size()) ||
      face.bone_index >= mesh.bone_names.size()) {
    return false;
  }

  const std::size_t vertex_end =
      static_cast<std::size_t>(face.first_vertex) + face.vertex_count;
  for (std::size_t vertex_i = face.first_vertex; vertex_i < vertex_end;
       ++vertex_i) {
    const auto &vertex = mesh.vertices[vertex_i];
    if (vertex.bone_index != face.bone_index || !std::isfinite(vertex.px) ||
        !std::isfinite(vertex.py) || !std::isfinite(vertex.pz) ||
        !std::isfinite(vertex.nx) || !std::isfinite(vertex.ny) ||
        !std::isfinite(vertex.nz) || !std::isfinite(vertex.u) ||
        !std::isfinite(vertex.v) || !std::isfinite(vertex.raw_u) ||
        !std::isfinite(vertex.raw_v)) {
      return false;
    }
  }

  const std::size_t index_end =
      static_cast<std::size_t>(face.first_index) + face.index_count;
  for (std::size_t index_i = face.first_index; index_i < index_end; ++index_i) {
    const std::uint32_t vertex_index = mesh.indices[index_i];
    if (vertex_index < face.first_vertex || vertex_index >= vertex_end) {
      return false;
    }
  }
  return true;
}

}






[[nodiscard]] inline StaticModelMaterialClass
staticModelFaceMaterial(const StaticIndexedModelMesh &mesh,
                        const StaticModelFace &face,
                        const TextureImage *texture) noexcept {
  if (!face.textured || texture == nullptr || !texture->valid() ||
      !detail::validFace(mesh, face)) {
    return StaticModelMaterialClass::Opaque;
  }

  double min_u = mesh.vertices[face.first_vertex].u;
  double max_u = min_u;
  double min_v = mesh.vertices[face.first_vertex].v;
  double max_v = min_v;
  const std::size_t vertex_end =
      static_cast<std::size_t>(face.first_vertex) + face.vertex_count;
  for (std::size_t vertex_i = face.first_vertex + 1u; vertex_i < vertex_end;
       ++vertex_i) {
    min_u = (std::min)(
        min_u, static_cast<double>(mesh.vertices[vertex_i].u));
    max_u = (std::max)(
        max_u, static_cast<double>(mesh.vertices[vertex_i].u));
    min_v = (std::min)(
        min_v, static_cast<double>(mesh.vertices[vertex_i].v));
    max_v = (std::max)(
        max_v, static_cast<double>(mesh.vertices[vertex_i].v));
  }

  bool has_cutout = false;
  const detail::TexelSpan x_span =
      detail::coveredTexelSpan(min_u, max_u, texture->width);
  const detail::TexelSpan y_span =
      detail::coveredTexelSpan(min_v, max_v, texture->height);
  for (int y = y_span.begin; y < y_span.end; ++y) {
    for (int x = x_span.begin; x < x_span.end; ++x) {
      const std::size_t alpha_offset =
          (static_cast<std::size_t>(y) * texture->width +
           static_cast<std::size_t>(x)) *
              4u +
          3u;
      const float alpha = texture->rgba[alpha_offset] / 255.0f;
      if (alpha >= 0.02f && alpha < 0.98f) {
        return StaticModelMaterialClass::Blend;
      }
      has_cutout = has_cutout || alpha < 0.02f;
    }
  }
  return has_cutout ? StaticModelMaterialClass::Cutout
                    : StaticModelMaterialClass::Opaque;
}

[[nodiscard]] inline StaticModelDrawPlan
makeStaticModelDrawPlan(const StaticIndexedModelMesh &mesh,
                        const TextureImage *texture) {
  StaticModelDrawPlan plan;
  plan.textured_vertices.resize(mesh.vertices.size(), 0u);

  std::vector<std::uint32_t> opaque;
  std::vector<std::uint32_t> cutout;
  std::vector<std::uint32_t> blend;
  std::vector<StaticModelPrimitiveMaterial> opaque_materials;
  std::vector<StaticModelPrimitiveMaterial> cutout_materials;
  std::vector<StaticModelPrimitiveMaterial> blend_materials;
  opaque.reserve(mesh.indices.size());
  cutout.reserve(mesh.indices.size());
  blend.reserve(mesh.indices.size());
  opaque_materials.reserve(mesh.indices.size() / 3u);
  cutout_materials.reserve(mesh.indices.size() / 3u);
  blend_materials.reserve(mesh.indices.size() / 3u);

  for (std::size_t face_index = 0; face_index < mesh.faces.size();
       ++face_index) {
    const auto &face = mesh.faces[face_index];
    if (!detail::validFace(mesh, face)) {
      ++plan.skipped_face_count;
      continue;
    }

    const bool textured =
        face.textured && texture != nullptr && texture->valid();
    if (textured) {
      const std::size_t vertex_end =
          static_cast<std::size_t>(face.first_vertex) + face.vertex_count;
      std::fill(plan.textured_vertices.begin() + face.first_vertex,
                plan.textured_vertices.begin() + vertex_end, 1u);
    }

    const StaticModelMaterialClass material =
        staticModelFaceMaterial(mesh, face, texture);
    std::vector<std::uint32_t> *destination = &opaque;
    std::vector<StaticModelPrimitiveMaterial> *material_destination =
        &opaque_materials;
    switch (material) {
    case StaticModelMaterialClass::Opaque:
      break;
    case StaticModelMaterialClass::Cutout:
      destination = &cutout;
      material_destination = &cutout_materials;
      break;
    case StaticModelMaterialClass::Blend:
      destination = &blend;
      material_destination = &blend_materials;
      break;
    }
    const auto begin = mesh.indices.begin() + face.first_index;
    destination->insert(destination->end(), begin, begin + face.index_count);
    for (std::uint32_t triangle = 0; triangle < face.index_count / 3u;
         ++triangle) {
      StaticModelPrimitiveMaterial primitive;
      primitive.material = material;
      primitive.textured = textured;
      primitive.source_face_index =
          static_cast<std::uint32_t>(face_index);
      primitive.source_triangle_index = triangle;
      primitive.bone_index = face.bone_index;
      primitive.cube_index = face.cube_index;
      primitive.face_direction = face.direction;
      material_destination->push_back(primitive);
    }
  }

  plan.indices.reserve(opaque.size() + cutout.size() + blend.size());
  auto append = [&](const std::vector<std::uint32_t> &source,
                    const std::vector<StaticModelPrimitiveMaterial> &materials,
                    StaticModelIndexRange &range) {
    range.first_index = static_cast<std::uint32_t>(plan.indices.size());
    range.index_count = static_cast<std::uint32_t>(source.size());
    plan.indices.insert(plan.indices.end(), source.begin(), source.end());
    plan.primitive_materials.insert(plan.primitive_materials.end(),
                                    materials.begin(), materials.end());
  };
  append(opaque, opaque_materials, plan.opaque);
  append(cutout, cutout_materials, plan.cutout);
  append(blend, blend_materials, plan.blend);
  return plan;
}

}
