#pragma once

#include "xpbd/gfx/texture_image.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xpbd::gfx {

enum class LabPbrFormat {
  Fallback,
  LabPbr13,
  Unsupported,
};

enum class LabPbrMetalKind : std::uint8_t {
  Dielectric,
  Predefined,
  Custom,
};

enum class LabPbrDebugView : std::uint32_t {
  Shaded = 0,
  BaseColor = 1,
  Normal = 2,
  AmbientOcclusion = 3,
  LinearRoughness = 4,
  F0 = 5,
  Emission = 6,
  Opacity = 7,
};

inline constexpr std::uint32_t kLabPbrNormalMapActive = 1u << 0u;
inline constexpr std::uint32_t kLabPbrSpecularMapActive = 1u << 1u;

struct LabPbrAssetPaths {
  std::filesystem::path base;
  std::filesystem::path normal;
  std::filesystem::path specular;
  std::filesystem::path properties;
  bool normal_exists = false;
  bool specular_exists = false;
  bool properties_exists = false;
};

struct ResolvedMaterialTexel {
  std::array<float, 3> base_color_linear{1.0f, 1.0f, 1.0f};
  float opacity = 1.0f;

  std::array<float, 3> tangent_normal{0.0f, 0.0f, 1.0f};
  float ambient_occlusion = 1.0f;
  float stored_height = 1.0f;
  float relative_depth = 0.0f;

  float perceptual_smoothness = 0.0f;
  float perceptual_roughness = 1.0f;
  float linear_roughness = 1.0f;
  float dielectric_f0 = 0.04f;
  std::array<float, 3> f0_color{0.04f, 0.04f, 0.04f};
  LabPbrMetalKind metal_kind = LabPbrMetalKind::Dielectric;
  std::uint8_t metal_code = 0;

  float porosity = 0.0f;
  float subsurface_scattering = 0.0f;
  float emission_strength = 0.0f;
  std::array<float, 3> emission_linear{0.0f, 0.0f, 0.0f};

  bool operator==(const ResolvedMaterialTexel &) const = default;
};

struct ResolvedMaterialTable {
  int width = 0;
  int height = 0;
  std::vector<ResolvedMaterialTexel> texels;

  LabPbrAssetPaths assets;
  LabPbrFormat format = LabPbrFormat::Fallback;
  std::string declared_format;
  bool format_declared = false;
  bool normal_map_active = false;
  bool specular_map_active = false;
  TextureImage normal_image;
  TextureImage specular_image;
  std::vector<std::string> warnings;

  [[nodiscard]] bool valid() const noexcept;
  void clear();
  [[nodiscard]] const ResolvedMaterialTexel &sample(float u, float v) const;
};

struct TangentFrame {
  std::array<float, 3> tangent{1.0f, 0.0f, 0.0f};
  float handedness = 1.0f;
  bool used_fallback = false;
};

[[nodiscard]] LabPbrAssetPaths
discoverLabPbrAssets(const std::filesystem::path &base_path);

[[nodiscard]] float srgbToLinear(float value) noexcept;
[[nodiscard]] float srgb8ToLinear(std::uint8_t value) noexcept;
[[nodiscard]] const char *
labPbrDebugViewName(LabPbrDebugView view) noexcept;
[[nodiscard]] LabPbrDebugView
labPbrDebugViewFromName(std::string_view name) noexcept;
[[nodiscard]] std::array<float, 3>
labPbrDebugColor(const ResolvedMaterialTexel &texel,
                 LabPbrDebugView view) noexcept;
[[nodiscard]] std::uint32_t
labPbrFeatureFlags(const ResolvedMaterialTable *material) noexcept;

[[nodiscard]] ResolvedMaterialTexel decodeLabPbrTexel(
    const std::array<std::uint8_t, 4> &base_rgba,
    const std::array<std::uint8_t, 4> *normal_rgba,
    const std::array<std::uint8_t, 4> *specular_rgba,
    int specular_source_channels = 4) noexcept;

bool resolveLabPbrMaterial(const TextureImage &base,
                           const std::filesystem::path &base_path,
                           ResolvedMaterialTable &out,
                           std::string *error = nullptr);

[[nodiscard]] bool
sameResolvedMaterialResource(const ResolvedMaterialTable &lhs,
                             const ResolvedMaterialTable &rhs) noexcept;

[[nodiscard]] TangentFrame computeTangentFrame(
    const std::array<float, 3> &position0,
    const std::array<float, 3> &position1,
    const std::array<float, 3> &position2,
    const std::array<float, 3> &normal,
    const std::array<float, 2> &uv0,
    const std::array<float, 2> &uv1,
    const std::array<float, 2> &uv2) noexcept;

} // namespace xpbd::gfx
