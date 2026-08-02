#pragma once

#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace xpbd::gfx {

enum class BedrockUvFace : std::uint8_t {
  West = 0,
  East = 1,
  Down = 2,
  Up = 3,
  North = 4,
  South = 5,
};

struct ResolvedFaceUv {
  double u0 = 0.0;
  double v0 = 0.0;
  double u1 = 0.0;
  double v1 = 0.0;
  int rotation_quarter_turns = 0;
  bool present = false;

  bool operator==(const ResolvedFaceUv &) const = default;
};

using FaceUvCorners = std::array<std::array<double, 2>, 4>;

bool resolveBedrockFaceUv(const loader::Cube &cube, BedrockUvFace face,
                          ResolvedFaceUv &out,
                          std::string *error = nullptr);

[[nodiscard]] FaceUvCorners
bedrockFaceUvCorners(const ResolvedFaceUv &face_uv) noexcept;

struct UvBounds {
  double min_u = 0.0;
  double min_v = 0.0;
  double max_u = 0.0;
  double max_v = 0.0;
  std::uint64_t face_count = 0;

  [[nodiscard]] bool empty() const noexcept { return face_count == 0u; }
  bool operator==(const UvBounds &) const = default;
};

bool scanGeometryUvBounds(const loader::Geometry &geometry, UvBounds &out,
                          std::string *error = nullptr);

enum class UvDomainKind : std::uint8_t {
  Declared,
  Recovered,
  ImportedTexture,
};

[[nodiscard]] const char *uvDomainKindName(UvDomainKind kind) noexcept;

struct ResolvedUvDomain {
  UvDomainKind kind = UvDomainKind::ImportedTexture;
  double width = 0.0;
  double height = 0.0;
  int imported_width = 0;
  int imported_height = 0;
  int declared_width = 0;
  int declared_height = 0;
  bool declaration_reliable = false;
  UvBounds bounds;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] double normalizeU(double texel_u) const noexcept;
  [[nodiscard]] double normalizeV(double texel_v) const noexcept;
  bool operator==(const ResolvedUvDomain &) const = default;
};

bool resolveUvDomain(const loader::GeometryDescription &description,
                     const UvBounds &bounds, int imported_width,
                     int imported_height, ResolvedUvDomain &out,
                     std::string *error = nullptr);

bool resolveGeometryUvDomain(const loader::Geometry &geometry,
                             int imported_width, int imported_height,
                             ResolvedUvDomain &out,
                             std::string *error = nullptr);

} // namespace xpbd::gfx
