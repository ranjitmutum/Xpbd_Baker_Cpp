#include "xpbd/gfx/uv_domain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace xpbd::gfx {
namespace {

bool fail(std::string message, std::string *error) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

bool finiteFace(const ResolvedFaceUv &face) noexcept {
  return std::isfinite(face.u0) && std::isfinite(face.v0) &&
         std::isfinite(face.u1) && std::isfinite(face.v1);
}

bool fromLoaderFace(const loader::FaceUV &source, double default_u,
                    double default_v, bool reverse_top_bottom,
                    ResolvedFaceUv &out, std::string *error) {
  if (!source.present) {
    out = {};
    return true;
  }
  if (!std::isfinite(source.u) || !std::isfinite(source.v) ||
      !std::isfinite(source.size_u) || !std::isfinite(source.size_v) ||
      !std::isfinite(default_u) || !std::isfinite(default_v)) {
    return fail("Bedrock face UV contains a non-finite value", error);
  }
  if (source.rotation_degrees % 90 != 0) {
    return fail("Bedrock face UV rotation must be a multiple of 90 degrees",
                error);
  }

  ResolvedFaceUv candidate;
  candidate.u0 = source.u;
  candidate.v0 = source.v;
  candidate.u1 = source.u +
                 (source.size_explicit ? source.size_u : default_u);
  candidate.v1 = source.v +
                 (source.size_explicit ? source.size_v : default_v);
  if (reverse_top_bottom) {
    std::swap(candidate.u0, candidate.u1);
    std::swap(candidate.v0, candidate.v1);
  }
  candidate.rotation_quarter_turns =
      ((source.rotation_degrees % 360) + 360) % 360 / 90;
  candidate.present = true;
  if (!finiteFace(candidate)) {
    return fail("Bedrock face UV calculation overflowed", error);
  }
  out = candidate;
  return true;
}

bool boundsFit(const UvBounds &bounds, double width,
               double height) noexcept {
  if (bounds.empty()) {
    return true;
  }
  constexpr double kBoundaryTolerance = 1.0e-9;
  return bounds.min_u >= -kBoundaryTolerance &&
         bounds.min_v >= -kBoundaryTolerance &&
         bounds.max_u <= width + kBoundaryTolerance &&
         bounds.max_v <= height + kBoundaryTolerance;
}

bool validDeclaredDimension(bool present, int value, const char *name,
                            std::string *error) {
  if (!present) {
    return true;
  }
  if (value <= 0 ||
      value > static_cast<int>(loader::kBedrockTextureDimensionMaximum)) {
    return fail(std::string("Bedrock ") + name +
                    " declaration is outside the supported range",
                error);
  }
  return true;
}

} // namespace

bool resolveBedrockFaceUv(const loader::Cube &cube, BedrockUvFace face,
                          ResolvedFaceUv &out, std::string *error) {
  for (const double size : cube.size) {
    if (!std::isfinite(size)) {
      return fail("Bedrock cube size used by UV resolution must be finite",
                  error);
    }
  }

  ResolvedFaceUv candidate;
  if (cube.uv_mode == loader::CubeUVMode::PerFace) {
    const double width = std::abs(cube.size[0]);
    const double height = std::abs(cube.size[1]);
    const double depth = std::abs(cube.size[2]);
    bool resolved = false;
    switch (face) {
    case BedrockUvFace::West:
      resolved = fromLoaderFace(cube.uv_west, depth, height, false,
                                candidate, error);
      break;
    case BedrockUvFace::East:
      resolved = fromLoaderFace(cube.uv_east, depth, height, false,
                                candidate, error);
      break;
    case BedrockUvFace::Down:
      resolved = fromLoaderFace(cube.uv_down, width, depth, true, candidate,
                                error);
      break;
    case BedrockUvFace::Up:
      resolved = fromLoaderFace(cube.uv_up, width, depth, true, candidate,
                                error);
      break;
    case BedrockUvFace::North:
      resolved = fromLoaderFace(cube.uv_north, width, height, false,
                                candidate, error);
      break;
    case BedrockUvFace::South:
      resolved = fromLoaderFace(cube.uv_south, width, height, false,
                                candidate, error);
      break;
    }
    if (!resolved) {
      return false;
    }
    out = candidate;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  if (cube.uv_mode == loader::CubeUVMode::Box &&
      (!std::isfinite(cube.uv_box[0]) ||
       !std::isfinite(cube.uv_box[1]))) {
    return fail("Bedrock Box UV origin must be finite", error);
  }
  const double width = std::floor(std::abs(cube.size[0]) + 1.0e-6);
  const double height = std::floor(std::abs(cube.size[1]) + 1.0e-6);
  const double depth = std::floor(std::abs(cube.size[2]) + 1.0e-6);
  const double origin_u = cube.uv_mode == loader::CubeUVMode::None
                              ? 0.0
                              : cube.uv_box[0];
  const double origin_v = cube.uv_mode == loader::CubeUVMode::None
                              ? 0.0
                              : cube.uv_box[1];
  struct BoxFace {
    double u = 0.0;
    double v = 0.0;
    double size_u = 0.0;
    double size_v = 0.0;
  };
  const BoxFace west{0.0, depth, depth, height};
  const BoxFace east{depth + width, depth, depth, height};
  const BoxFace down{depth + width, 0.0, width, depth};
  const BoxFace up{depth, depth, width, -depth};
  const BoxFace north{depth, depth, width, height};
  const BoxFace south{depth * 2.0 + width, depth, width, height};
  BoxFace box_face;
  switch (face) {
  case BedrockUvFace::West:
    box_face = west;
    break;
  case BedrockUvFace::East:
    box_face = east;
    break;
  case BedrockUvFace::Down:
    box_face = down;
    break;
  case BedrockUvFace::Up:
    box_face = up;
    break;
  case BedrockUvFace::North:
    box_face = north;
    break;
  case BedrockUvFace::South:
    box_face = south;
    break;
  }
  if (cube.mirror) {
    if (face == BedrockUvFace::East) {
      box_face = west;
    } else if (face == BedrockUvFace::West) {
      box_face = east;
    }
    box_face.u += box_face.size_u;
    box_face.size_u = -box_face.size_u;
  }

  candidate.u0 = origin_u + box_face.u;
  candidate.v0 = origin_v + box_face.v;
  candidate.u1 = candidate.u0 + box_face.size_u;
  candidate.v1 = candidate.v0 + box_face.size_v;
  candidate.present = true;
  if (!finiteFace(candidate)) {
    return fail("Bedrock Box UV calculation overflowed", error);
  }
  out = candidate;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

FaceUvCorners
bedrockFaceUvCorners(const ResolvedFaceUv &face_uv) noexcept {
  const FaceUvCorners base{{
      {{face_uv.u0, face_uv.v0}},
      {{face_uv.u1, face_uv.v0}},
      {{face_uv.u1, face_uv.v1}},
      {{face_uv.u0, face_uv.v1}},
  }};
  FaceUvCorners result{};
  const int quarter_turns =
      ((face_uv.rotation_quarter_turns % 4) + 4) % 4;
  for (int corner = 0; corner < 4; ++corner) {
    const int source = (corner + 4 - quarter_turns) % 4;
    result[static_cast<std::size_t>(corner)] =
        base[static_cast<std::size_t>(source)];
  }
  return result;
}

bool scanGeometryUvBounds(const loader::Geometry &geometry, UvBounds &out,
                          std::string *error) {
  UvBounds candidate;
  bool initialized = false;
  for (const auto &bone : geometry.bones) {
    for (const auto &cube : bone.cubes) {
      for (int face_index = 0; face_index < 6; ++face_index) {
        ResolvedFaceUv face;
        std::string face_error;
        if (!resolveBedrockFaceUv(
                cube, static_cast<BedrockUvFace>(face_index), face,
                &face_error)) {
          return fail("UV bounds scan failed for bone '" + bone.name +
                          "': " + face_error,
                      error);
        }
        if (!face.present) {
          continue;
        }
        const auto corners = bedrockFaceUvCorners(face);
        for (const auto &corner : corners) {
          if (!std::isfinite(corner[0]) || !std::isfinite(corner[1])) {
            return fail("UV bounds scan encountered a non-finite corner",
                        error);
          }
          if (!initialized) {
            candidate.min_u = candidate.max_u = corner[0];
            candidate.min_v = candidate.max_v = corner[1];
            initialized = true;
          } else {
            candidate.min_u = std::min(candidate.min_u, corner[0]);
            candidate.min_v = std::min(candidate.min_v, corner[1]);
            candidate.max_u = std::max(candidate.max_u, corner[0]);
            candidate.max_v = std::max(candidate.max_v, corner[1]);
          }
        }
        if (candidate.face_count ==
            (std::numeric_limits<std::uint64_t>::max)()) {
          return fail("UV bounds face count overflow", error);
        }
        ++candidate.face_count;
      }
    }
  }
  out = candidate;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

const char *uvDomainKindName(UvDomainKind kind) noexcept {
  switch (kind) {
  case UvDomainKind::Declared:
    return "Declared";
  case UvDomainKind::Recovered:
    return "Recovered";
  case UvDomainKind::ImportedTexture:
    return "ImportedTexture";
  }
  return "ImportedTexture";
}

bool ResolvedUvDomain::valid() const noexcept {
  return width > 0.0 && height > 0.0 && std::isfinite(width) &&
         std::isfinite(height) && imported_width > 0 &&
         imported_height > 0;
}

double ResolvedUvDomain::normalizeU(double texel_u) const noexcept {
  return valid() ? texel_u / width : 0.0;
}

double ResolvedUvDomain::normalizeV(double texel_v) const noexcept {
  return valid() ? texel_v / height : 0.0;
}

bool resolveUvDomain(const loader::GeometryDescription &description,
                     const UvBounds &bounds, int imported_width,
                     int imported_height, ResolvedUvDomain &out,
                     std::string *error) {
  if (imported_width <= 0 || imported_height <= 0 ||
      imported_width >
          static_cast<int>(loader::kBedrockTextureDimensionMaximum) ||
      imported_height >
          static_cast<int>(loader::kBedrockTextureDimensionMaximum)) {
    return fail("Imported texture dimensions are outside the supported UV "
                "domain range",
                error);
  }
  if (!validDeclaredDimension(description.has_texture_width,
                              description.texture_width, "texture_width",
                              error) ||
      !validDeclaredDimension(description.has_texture_height,
                              description.texture_height, "texture_height",
                              error)) {
    return false;
  }
  if (!bounds.empty() &&
      (!std::isfinite(bounds.min_u) || !std::isfinite(bounds.min_v) ||
       !std::isfinite(bounds.max_u) || !std::isfinite(bounds.max_v) ||
       bounds.min_u > bounds.max_u || bounds.min_v > bounds.max_v)) {
    return fail("Raw UV bounds are invalid or non-finite", error);
  }

  ResolvedUvDomain candidate;
  candidate.imported_width = imported_width;
  candidate.imported_height = imported_height;
  candidate.declared_width = description.texture_width;
  candidate.declared_height = description.texture_height;
  candidate.declaration_reliable =
      description.hasCompleteTextureSize();
  candidate.bounds = bounds;
  if (candidate.declaration_reliable &&
      boundsFit(bounds, static_cast<double>(description.texture_width),
                static_cast<double>(description.texture_height))) {
    candidate.kind = UvDomainKind::Declared;
    candidate.width = static_cast<double>(description.texture_width);
    candidate.height = static_cast<double>(description.texture_height);
  } else if (boundsFit(bounds, static_cast<double>(imported_width),
                       static_cast<double>(imported_height))) {
    candidate.kind = candidate.declaration_reliable
                         ? UvDomainKind::Recovered
                         : UvDomainKind::ImportedTexture;
    candidate.width = static_cast<double>(imported_width);
    candidate.height = static_cast<double>(imported_height);
  } else {
    std::ostringstream message;
    message << "Raw UV bounds [" << bounds.min_u << ',' << bounds.min_v
            << " -> " << bounds.max_u << ',' << bounds.max_v
            << "] exceed the imported texture domain " << imported_width
            << 'x' << imported_height;
    if (candidate.declaration_reliable) {
      message << " and declared domain " << description.texture_width << 'x'
              << description.texture_height;
    }
    return fail(message.str(), error);
  }
  out = candidate;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool resolveGeometryUvDomain(const loader::Geometry &geometry,
                             int imported_width, int imported_height,
                             ResolvedUvDomain &out, std::string *error) {
  UvBounds bounds;
  std::string bounds_error;
  if (!scanGeometryUvBounds(geometry, bounds, &bounds_error)) {
    return fail("UV Domain bounds stage failed: " + bounds_error, error);
  }
  std::string domain_error;
  ResolvedUvDomain candidate;
  if (!resolveUvDomain(geometry.description, bounds, imported_width,
                       imported_height, candidate, &domain_error)) {
    return fail("UV Domain resolution stage failed: " + domain_error, error);
  }
  out = candidate;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace xpbd::gfx
