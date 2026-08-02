#include "test_support/labpbr_synthetic_fixture.hpp"

#include "xpbd/gfx/labpbr_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

namespace xpbd::test_support {
namespace {

using Rgba = std::array<std::uint8_t, 4>;

gfx::TextureImage makeImage(int width, int height, const Rgba &fill,
                            std::string path) {
  gfx::TextureImage image;
  image.width = width;
  image.height = height;
  image.source_channels = 4;
  image.path = std::move(path);
  image.rgba.resize(static_cast<std::size_t>(width) *
                    static_cast<std::size_t>(height) * 4u);
  for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4u) {
    std::copy(fill.begin(), fill.end(), image.rgba.begin() + offset);
  }
  return image;
}

void paintRect(gfx::TextureImage &image, int x0, int y0, int x1, int y1,
               const Rgba &rgba) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) *
               static_cast<std::size_t>(image.width) +
           static_cast<std::size_t>(x)) *
          4u;
      std::copy(rgba.begin(), rgba.end(), image.rgba.begin() + offset);
    }
  }
}

loader::Cube makeNorthFaceCube(double u, double v, double size_u,
                               double size_v,
                               int rotation = 0) {
  loader::Cube cube;
  cube.size[0] = 16.0;
  cube.size[1] = 16.0;
  cube.size[2] = 1.0;
  cube.uv_mode = loader::CubeUVMode::PerFace;
  cube.uv_north.u = u;
  cube.uv_north.v = v;
  cube.uv_north.size_u = size_u;
  cube.uv_north.size_v = size_v;
  cube.uv_north.present = true;
  cube.uv_north.rotation_degrees = rotation;
  return cube;
}

loader::Geometry makeGeometry(std::string identifier,
                              std::vector<std::pair<std::string,
                                                    loader::Cube>> cubes) {
  loader::Geometry geometry;
  geometry.description.identifier = std::move(identifier);
  geometry.description.texture_width =
      SyntheticLargeUvFixture::kDeclaredWidth;
  geometry.description.texture_height =
      SyntheticLargeUvFixture::kDeclaredHeight;
  geometry.description.has_texture_size = true;
  for (auto &[name, cube] : cubes) {
    loader::Bone bone;
    bone.name = std::move(name);
    bone.cubes.push_back(std::move(cube));
    geometry.bones.push_back(std::move(bone));
  }
  return geometry;
}

std::filesystem::path sidecarName(const std::filesystem::path &base,
                                  const char *suffix) {
  std::filesystem::path result = base.stem();
  result += suffix;
  result += base.extension();
  return result;
}

bool writeBytes(const std::filesystem::path &path,
                std::span<const std::uint8_t> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

} // namespace

bool buildSyntheticLargeUvFixture(SyntheticLargeUvFixture &out,
                                  std::string *error) {
  SyntheticLargeUvFixture candidate;
  candidate.base = makeImage(candidate.kAtlasWidth, candidate.kAtlasHeight,
                             {17u, 23u, 31u, 255u}, "synthetic-base.png");
  candidate.normal =
      makeImage(candidate.kAtlasWidth, candidate.kAtlasHeight,
                {128u, 128u, 255u, 255u}, "synthetic-base_n.png");
  candidate.specular =
      makeImage(candidate.kAtlasWidth, candidate.kAtlasHeight,
                {8u, 32u, 0u, 255u}, "synthetic-base_s.png");

  paintRect(candidate.base, 0, 0, 16, 16, {220u, 40u, 20u, 255u});
  paintRect(candidate.base, 192, 0, 208, 16, {20u, 220u, 40u, 255u});
  paintRect(candidate.base, 208, 0, 224, 16, {20u, 40u, 220u, 255u});
  paintRect(candidate.base, 120, 0, 136, 16, {255u, 0u, 255u, 0u});

  paintRect(candidate.normal, 0, 0, 16, 16,
            {128u, 128u, 255u, 255u});
  paintRect(candidate.normal, 192, 0, 208, 16,
            {176u, 112u, 230u, 220u});
  paintRect(candidate.normal, 208, 0, 224, 16,
            {80u, 160u, 210u, 180u});
  paintRect(candidate.normal, 120, 0, 136, 16,
            {0u, 0u, 0u, 0u});

  paintRect(candidate.specular, 0, 0, 16, 16,
            {32u, 48u, 0u, 255u});
  paintRect(candidate.specular, 192, 0, 208, 16,
            {64u, 96u, 16u, 220u});
  paintRect(candidate.specular, 208, 0, 224, 16,
            {192u, 230u, 48u, 180u});
  paintRect(candidate.specular, 120, 0, 136, 16,
            {255u, 255u, 255u, 0u});

  candidate.large_uv_geometry = makeGeometry(
      "geometry.synthetic_large_uv",
      {{"body", makeNorthFaceCube(0.0, 0.0, 16.0, 16.0)},
       {"eye_left", makeNorthFaceCube(192.0, 0.0, 16.0, 16.0)},
       {"eye_right", makeNorthFaceCube(208.0, 0.0, 16.0, 16.0)}});
  candidate.high_resolution_geometry = makeGeometry(
      "geometry.synthetic_high_resolution",
      {{"body", makeNorthFaceCube(0.0, 0.0, 16.0, 16.0)}});
  candidate.out_of_bounds_geometry = makeGeometry(
      "geometry.synthetic_out_of_bounds",
      {{"outside", makeNorthFaceCube(300.0, 0.0, 16.0, 16.0)}});

  loader::Cube box;
  box.size[0] = 2.0;
  box.size[1] = 3.0;
  box.size[2] = 4.0;
  box.uv_mode = loader::CubeUVMode::Box;
  box.uv_box[0] = 8.0;
  box.uv_box[1] = 12.0;
  candidate.uv_cases.push_back(
      {"box", makeGeometry("geometry.synthetic_box", {{"box", box}})});

  box.mirror = true;
  candidate.uv_cases.push_back({
      "box_mirror",
      makeGeometry("geometry.synthetic_box_mirror", {{"box", box}})});
  candidate.uv_cases.push_back(
      {"per_face_rotation",
       makeGeometry("geometry.synthetic_rotation",
                    {{"rotated",
                      makeNorthFaceCube(32.0, 16.0, 16.0, 8.0, 90)}})});
  candidate.uv_cases.push_back(
      {"negative_uv_size",
       makeGeometry("geometry.synthetic_negative_size",
                    {{"negative",
                      makeNorthFaceCube(64.0, 16.0, -16.0, 8.0)}})});

  loader::Cube up_down;
  up_down.size[0] = 4.0;
  up_down.size[1] = 6.0;
  up_down.size[2] = 8.0;
  up_down.uv_mode = loader::CubeUVMode::PerFace;
  up_down.uv_up = {96.0, 32.0, 8.0, 4.0, true, 0, true};
  up_down.uv_down = {112.0, 32.0, 8.0, 4.0, true, 180, true};
  candidate.uv_cases.push_back(
      {"up_down_corners",
       makeGeometry("geometry.synthetic_up_down", {{"up_down", up_down}})});

  auto precision_geometry = makeGeometry(
      "geometry.synthetic_precision",
      {{"precision", makeNorthFaceCube(16'368.0, 0.0, 16.0, 16.0)}});
  precision_geometry.description.texture_width = 16'384;
  precision_geometry.description.texture_height = 16'384;
  candidate.uv_cases.push_back(
      {"precision_16384", std::move(precision_geometry), 16'384, 16'384,
       true});

  auto non_finite = makeGeometry(
      "geometry.synthetic_non_finite",
      {{"non_finite",
        makeNorthFaceCube((std::numeric_limits<double>::quiet_NaN)(), 0.0,
                          16.0, 16.0)}});
  candidate.uv_cases.push_back(
      {"non_finite", std::move(non_finite), 256, 256, false});
  candidate.uv_cases.push_back(
      {"out_of_bounds", candidate.out_of_bounds_geometry, 256, 256, false});

  if (!candidate.base.valid() || !candidate.normal.valid() ||
      !candidate.specular.valid()) {
    if (error != nullptr) {
      *error = "synthetic LabPBR image construction failed";
    }
    return false;
  }
  out = std::move(candidate);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool writeSyntheticLabPbrSuite(
    const SyntheticLargeUvFixture &fixture,
    const std::filesystem::path &directory,
    const std::filesystem::path &base_filename,
    SyntheticLabPbrSuitePaths &out, std::string *error) {
  const auto fail = [&](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (!fixture.base.valid() || !fixture.normal.valid() ||
      !fixture.specular.valid()) {
    return fail("synthetic LabPBR suite source images are invalid");
  }
  if (base_filename.empty() || base_filename.has_parent_path() ||
      base_filename.extension() != ".png") {
    return fail("synthetic LabPBR base filename must be a local .png name");
  }

  std::vector<std::uint8_t> base_png;
  std::vector<std::uint8_t> normal_png;
  std::vector<std::uint8_t> specular_png;
  std::string encode_error;
  if (!gfx::encodePngRgba8(fixture.base.width, fixture.base.height,
                           fixture.base.rgba, base_png, &encode_error) ||
      !gfx::encodePngRgba8(fixture.normal.width, fixture.normal.height,
                           fixture.normal.rgba, normal_png, &encode_error) ||
      !gfx::encodePngRgba8(fixture.specular.width, fixture.specular.height,
                           fixture.specular.rgba, specular_png,
                           &encode_error)) {
    return fail("synthetic LabPBR PNG encode stage failed: " +
                encode_error);
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    return fail("synthetic LabPBR directory creation failed: " +
                filesystem_error.message());
  }

  SyntheticLabPbrSuitePaths candidate;
  candidate.base = directory / base_filename;
  candidate.normal = directory / sidecarName(base_filename, "_n");
  candidate.specular = directory / sidecarName(base_filename, "_s");
  candidate.properties = directory / "texture.properties";
  static constexpr char kProperties[] = "format=lab-pbr/1.3\n";
  const std::span<const std::uint8_t> properties(
      reinterpret_cast<const std::uint8_t *>(kProperties),
      sizeof(kProperties) - 1u);
  if (!writeBytes(candidate.base, base_png) ||
      !writeBytes(candidate.normal, normal_png) ||
      !writeBytes(candidate.specular, specular_png) ||
      !writeBytes(candidate.properties, properties)) {
    std::error_code ignored;
    std::filesystem::remove(candidate.base, ignored);
    std::filesystem::remove(candidate.normal, ignored);
    std::filesystem::remove(candidate.specular, ignored);
    std::filesystem::remove(candidate.properties, ignored);
    return fail("synthetic LabPBR file write stage failed");
  }

  out = std::move(candidate);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::array<std::uint8_t, 4>
fixtureTexel(const gfx::TextureImage &image,
             const std::array<int, 2> &coordinate) {
  if (!image.valid() || coordinate[0] < 0 || coordinate[1] < 0 ||
      coordinate[0] >= image.width || coordinate[1] >= image.height) {
    return {};
  }
  const std::size_t offset =
      (static_cast<std::size_t>(coordinate[1]) *
           static_cast<std::size_t>(image.width) +
       static_cast<std::size_t>(coordinate[0])) *
      4u;
  return {image.rgba[offset], image.rgba[offset + 1u],
          image.rgba[offset + 2u], image.rgba[offset + 3u]};
}

} // namespace xpbd::test_support
