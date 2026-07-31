#pragma once

#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace xpbd::gfx {

enum class LabPbrOverrideChannel : std::uint8_t {
  Roughness = 0,
  Metal = 1,
  Porosity = 2,
  Emission = 3,
};

struct GroupLabPbrOverride {
  std::string group_name;

  bool emission_enabled = false;
  float emission = 0.0f;

  bool roughness_enabled = false;
  float roughness = 1.0f;

  bool metal_enabled = false;
  bool metal = false;
  std::uint8_t dielectric_f0 = 10u;
  std::uint8_t metal_code = 255u;

  bool porosity_enabled = false;
  bool subsurface_scattering = false;
  float porosity = 0.0f;
  float subsurface = 0.0f;

  bool operator==(const GroupLabPbrOverride &) const = default;
};

struct LabPbrUvCoverage {
  int width = 0;
  int height = 0;
  std::map<std::string, std::vector<std::uint32_t>> group_texels;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const std::vector<std::uint32_t> *
  find(std::string_view group_name) const;
};

struct LabPbrUvConflict {
  std::uint32_t texel_index = 0;
  LabPbrOverrideChannel channel = LabPbrOverrideChannel::Roughness;
  std::vector<std::string> groups;
  std::vector<std::uint8_t> encoded_values;
};

struct LabPbrCompositionResult {
  TextureImage specular;
  std::vector<LabPbrUvConflict> conflicts;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  [[nodiscard]] bool exportable() const noexcept {
    return specular.valid() && conflicts.empty() && errors.empty();
  }
};

struct ReadOnlyIrisNormalAsset {
  std::filesystem::path source_path;
  std::vector<std::uint8_t> original_file_bytes;
  std::string sha256;
  TextureImage decoded;

  [[nodiscard]] bool valid() const noexcept {
    return decoded.valid() && decoded.source_channels == 4 &&
           !original_file_bytes.empty() && sha256.size() == 64u;
  }
  void clear() { *this = {}; }
};

[[nodiscard]] std::uint8_t
encodeLabPbrEmission(float emission) noexcept;
[[nodiscard]] std::uint8_t
encodeLabPbrRoughness(float roughness) noexcept;
[[nodiscard]] std::uint8_t
encodeLabPbrPorosity(float porosity) noexcept;
[[nodiscard]] std::uint8_t
encodeLabPbrSubsurface(float subsurface) noexcept;
[[nodiscard]] bool
validGroupLabPbrOverride(const GroupLabPbrOverride &override_value,
                         std::string *error = nullptr);

[[nodiscard]] LabPbrUvCoverage
rasterizeLabPbrUvCoverage(const StaticIndexedModelMesh &mesh, int width,
                          int height);

[[nodiscard]] LabPbrCompositionResult composeLabPbrSpecular(
    int width, int height, const TextureImage *imported_specular,
    const LabPbrUvCoverage &coverage,
    const std::map<std::string, GroupLabPbrOverride> &overrides);

bool buildAuthoredResolvedMaterial(
    const TextureImage &base, const ResolvedMaterialTable &source,
    const TextureImage *authored_normal,
    const TextureImage *authored_specular, ResolvedMaterialTable &out,
    std::string *error = nullptr);

[[nodiscard]] std::string
sha256Hex(std::span<const std::uint8_t> bytes);

bool importReadOnlyIrisNormal(const std::filesystem::path &path,
                              int expected_width, int expected_height,
                              ReadOnlyIrisNormalAsset &out,
                              std::string *error = nullptr);

bool encodePngRgba8(int width, int height,
                    std::span<const std::uint8_t> rgba,
                    std::vector<std::uint8_t> &png,
                    std::string *error = nullptr);

} // namespace xpbd::gfx
