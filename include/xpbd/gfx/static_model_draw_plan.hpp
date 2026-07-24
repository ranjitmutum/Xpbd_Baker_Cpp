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





struct StaticModelDrawPlan {
  std::vector<std::uint32_t> indices;

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

[[nodiscard]] inline int wrappedTexel(float coordinate, int extent) noexcept {
  const float wrapped = coordinate - std::floor(coordinate);
  return (std::clamp)(static_cast<int>(
                          std::floor(wrapped * static_cast<float>(extent))),
                      0, extent - 1);
}

struct TexelSpan {
  int begin = 0;
  int end = 0;
};

struct TexelSpans {
  std::array<TexelSpan, 2> spans{};
  std::size_t count = 0;
};






[[nodiscard]] inline TexelSpans coveredTexelSpans(float lo, float hi,
                                                  int extent) noexcept {
  if (lo > hi) {
    std::swap(lo, hi);
  }
  TexelSpans result;
  if (hi - lo <= 1.0e-7f) {
    const int texel = wrappedTexel(lo, extent);
    result.spans[0] = {texel, texel + 1};
    result.count = 1;
    return result;
  }
  if (hi - lo >= 1.0f) {
    result.spans[0] = {0, extent};
    result.count = 1;
    return result;
  }

  double cursor = lo;
  const double interval_end = hi;
  while (cursor < interval_end && result.count < result.spans.size()) {
    const double period = std::floor(cursor);
    const double segment_end = (std::min)(interval_end, period + 1.0);
    const double local_begin = cursor - period;
    const double local_end = segment_end - period;
    const int first =
        (std::clamp)(static_cast<int>(std::floor(local_begin * extent)), 0,
                     extent - 1);
    const int end =
        (std::clamp)(static_cast<int>(std::ceil(local_end * extent)), first + 1,
                     extent);
    result.spans[result.count++] = {first, end};
    cursor = segment_end;
    if (cursor < interval_end && cursor == std::floor(cursor)) {

      cursor =
          std::nextafter(cursor, (std::numeric_limits<double>::infinity)());
    }
  }
  return result;
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
        !std::isfinite(vertex.v)) {
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

  float min_u = mesh.vertices[face.first_vertex].u;
  float max_u = min_u;
  float min_v = mesh.vertices[face.first_vertex].v;
  float max_v = min_v;
  const std::size_t vertex_end =
      static_cast<std::size_t>(face.first_vertex) + face.vertex_count;
  for (std::size_t vertex_i = face.first_vertex + 1u; vertex_i < vertex_end;
       ++vertex_i) {
    min_u = (std::min)(min_u, mesh.vertices[vertex_i].u);
    max_u = (std::max)(max_u, mesh.vertices[vertex_i].u);
    min_v = (std::min)(min_v, mesh.vertices[vertex_i].v);
    max_v = (std::max)(max_v, mesh.vertices[vertex_i].v);
  }

  bool has_cutout = false;
  const detail::TexelSpans x_spans =
      detail::coveredTexelSpans(min_u, max_u, texture->width);
  const detail::TexelSpans y_spans =
      detail::coveredTexelSpans(min_v, max_v, texture->height);
  for (std::size_t y_span = 0; y_span < y_spans.count; ++y_span) {
    for (int y = y_spans.spans[y_span].begin; y < y_spans.spans[y_span].end;
         ++y) {
      for (std::size_t x_span = 0; x_span < x_spans.count; ++x_span) {
        for (int x = x_spans.spans[x_span].begin; x < x_spans.spans[x_span].end;
             ++x) {
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
  opaque.reserve(mesh.indices.size());
  cutout.reserve(mesh.indices.size());
  blend.reserve(mesh.indices.size());

  for (const auto &face : mesh.faces) {
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

    std::vector<std::uint32_t> *destination = &opaque;
    switch (staticModelFaceMaterial(mesh, face, texture)) {
    case StaticModelMaterialClass::Opaque:
      break;
    case StaticModelMaterialClass::Cutout:
      destination = &cutout;
      break;
    case StaticModelMaterialClass::Blend:
      destination = &blend;
      break;
    }
    const auto begin = mesh.indices.begin() + face.first_index;
    destination->insert(destination->end(), begin, begin + face.index_count);
  }

  plan.indices.reserve(opaque.size() + cutout.size() + blend.size());
  auto append = [&](const std::vector<std::uint32_t> &source,
                    StaticModelIndexRange &range) {
    range.first_index = static_cast<std::uint32_t>(plan.indices.size());
    range.index_count = static_cast<std::uint32_t>(source.size());
    plan.indices.insert(plan.indices.end(), source.begin(), source.end());
  };
  append(opaque, plan.opaque);
  append(cutout, plan.cutout);
  append(blend, plan.blend);
  return plan;
}

}
