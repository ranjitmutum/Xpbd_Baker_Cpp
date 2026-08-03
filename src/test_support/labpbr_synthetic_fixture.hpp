#pragma once

#include "xpbd/gfx/texture_image.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace xpbd::test_support {

struct SyntheticUvCase {
  std::string name;
  loader::Geometry geometry;
  int imported_width = 256;
  int imported_height = 256;
  bool expected_domain_success = true;
};

struct SyntheticLargeUvFixture {
  static constexpr int kDeclaredWidth = 16;
  static constexpr int kDeclaredHeight = 16;
  static constexpr int kAtlasWidth = 256;
  static constexpr int kAtlasHeight = 256;

  loader::Geometry large_uv_geometry;
  loader::Geometry high_resolution_geometry;
  loader::Geometry out_of_bounds_geometry;
  std::vector<SyntheticUvCase> uv_cases;

  gfx::TextureImage base;
  gfx::TextureImage normal;
  gfx::TextureImage specular;

  std::array<int, 2> body_texel{8, 8};
  std::array<int, 2> left_eye_texel{200, 8};
  std::array<int, 2> right_eye_texel{216, 8};
  std::array<int, 2> repeat_trap_texel{128, 8};
};

struct SyntheticLabPbrSuitePaths {
  std::filesystem::path base;
  std::filesystem::path normal;
  std::filesystem::path specular;
  std::filesystem::path properties;
};

bool buildSyntheticLargeUvFixture(SyntheticLargeUvFixture &out,
                                  std::string *error = nullptr);

bool writeSyntheticLabPbrSuite(
    const SyntheticLargeUvFixture &fixture,
    const std::filesystem::path &directory,
    const std::filesystem::path &base_filename,
    SyntheticLabPbrSuitePaths &out, std::string *error = nullptr);

[[nodiscard]] std::array<std::uint8_t, 4>
fixtureTexel(const gfx::TextureImage &image,
             const std::array<int, 2> &coordinate);

} // namespace xpbd::test_support
