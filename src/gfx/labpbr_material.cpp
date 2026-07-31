#include "xpbd/gfx/labpbr_material.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace xpbd::gfx {
namespace {

constexpr float kInv255 = 1.0f / 255.0f;

std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(first, last - first + 1u));
}

bool sameImagePixels(const TextureImage &lhs,
                     const TextureImage &rhs) noexcept {
  return lhs.width == rhs.width && lhs.height == rhs.height &&
         lhs.source_channels == rhs.source_channels && lhs.rgba == rhs.rgba;
}

void addWarning(ResolvedMaterialTable &table, std::string warning) {
  table.warnings.push_back(std::move(warning));
}

std::array<float, 3> hardcodedMetalF0(std::uint8_t code,
                                     const std::array<float, 3> &fallback) {
  struct OpticalConstants {
    std::array<float, 3> n;
    std::array<float, 3> k;
  };
  static constexpr std::array<OpticalConstants, 8> kMetals{{
      {{2.9114f, 2.9497f, 2.5845f}, {3.0893f, 2.9318f, 2.7670f}},
      {{0.18299f, 0.42108f, 1.3734f}, {3.4242f, 2.3459f, 1.7704f}},
      {{1.3456f, 0.96521f, 0.61722f}, {7.4746f, 6.3995f, 5.3031f}},
      {{3.1071f, 3.1812f, 2.3230f}, {3.3314f, 3.3291f, 3.1350f}},
      {{0.27105f, 0.67693f, 1.3164f}, {3.6092f, 2.6248f, 2.2921f}},
      {{1.9100f, 1.8300f, 1.4400f}, {3.5100f, 3.4000f, 3.1800f}},
      {{2.3757f, 2.0847f, 1.8453f}, {4.2655f, 3.7153f, 3.1365f}},
      {{0.15943f, 0.14512f, 0.13547f}, {3.9291f, 3.1900f, 2.3808f}},
  }};
  if (code < 230u || code > 237u) {
    return fallback;
  }
  const auto &metal = kMetals[static_cast<std::size_t>(code - 230u)];
  std::array<float, 3> result{};
  for (std::size_t channel = 0; channel < result.size(); ++channel) {
    const float n_minus_one = metal.n[channel] - 1.0f;
    const float n_plus_one = metal.n[channel] + 1.0f;
    const float k_squared = metal.k[channel] * metal.k[channel];
    result[channel] =
        (n_minus_one * n_minus_one + k_squared) /
        (n_plus_one * n_plus_one + k_squared);
  }
  return result;
}

std::array<float, 3> cross(const std::array<float, 3> &a,
                           const std::array<float, 3> &b) noexcept {
  return {a[1] * b[2] - a[2] * b[1],
          a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

float dot(const std::array<float, 3> &a,
          const std::array<float, 3> &b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> normalized(const std::array<float, 3> &value,
                                const std::array<float, 3> &fallback) noexcept {
  const float length_squared = dot(value, value);
  if (!std::isfinite(length_squared) || length_squared <= 1.0e-20f) {
    return fallback;
  }
  const float inverse_length = 1.0f / std::sqrt(length_squared);
  return {value[0] * inverse_length, value[1] * inverse_length,
          value[2] * inverse_length};
}

std::array<float, 3>
fallbackTangent(const std::array<float, 3> &normal) noexcept {
  const std::array<float, 3> helper =
      std::abs(normal[2]) < 0.999f
          ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
          : std::array<float, 3>{0.0f, 1.0f, 0.0f};
  return normalized(cross(helper, normal), {1.0f, 0.0f, 0.0f});
}

bool loadCompatibleSidecar(const std::filesystem::path &path,
                           const TextureImage &base, const char *label,
                           TextureImage &image,
                           ResolvedMaterialTable &table) {
  std::string error;
  TextureImage loaded;
  if (!loadTextureImage(path, loaded, &error)) {
    addWarning(table, std::string(label) + " sidecar ignored: " +
                          (error.empty() ? "decode failed" : error));
    return false;
  }
  if (loaded.width != base.width || loaded.height != base.height) {
    addWarning(table, std::string(label) +
                          " sidecar ignored: dimensions do not match base");
    return false;
  }
  if (loaded.source_channels < 3) {
    addWarning(table, std::string(label) +
                          " sidecar ignored: at least RGB channels required");
    return false;
  }
  image = std::move(loaded);
  return true;
}

void readFormatDeclaration(ResolvedMaterialTable &table) {
  if (!table.assets.properties_exists) {
    return;
  }
  std::ifstream input(table.assets.properties, std::ios::binary);
  if (!input) {
    addWarning(table,
               "texture.properties could not be read; assuming LabPBR 1.3");
    return;
  }
  std::string line;
  bool first_line = true;
  while (std::getline(input, line)) {
    if (first_line && line.size() >= 3u &&
        static_cast<unsigned char>(line[0]) == 0xefu &&
        static_cast<unsigned char>(line[1]) == 0xbbu &&
        static_cast<unsigned char>(line[2]) == 0xbfu) {
      line.erase(0, 3);
    }
    first_line = false;
    const std::string cleaned = trim(line);
    if (cleaned.empty() || cleaned[0] == '#' || cleaned[0] == '!') {
      continue;
    }
    const auto separator = cleaned.find_first_of("=:");
    if (separator == std::string::npos) {
      continue;
    }
    if (trim(std::string_view(cleaned).substr(0, separator)) == "format") {
      table.format_declared = true;
      table.declared_format =
          trim(std::string_view(cleaned).substr(separator + 1u));
      return;
    }
  }
  addWarning(table,
             "texture.properties has no format key; assuming LabPBR 1.3");
}

} // namespace

bool ResolvedMaterialTable::valid() const noexcept {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const auto w = static_cast<std::size_t>(width);
  const auto h = static_cast<std::size_t>(height);
  return w <= (std::numeric_limits<std::size_t>::max)() / h &&
         texels.size() == w * h;
}

void ResolvedMaterialTable::clear() {
  *this = {};
}

const ResolvedMaterialTexel &ResolvedMaterialTable::sample(float u,
                                                            float v) const {
  static const ResolvedMaterialTexel kFallback{};
  if (!valid()) {
    return kFallback;
  }
  u -= std::floor(u);
  v -= std::floor(v);
  if (u < 0.0f) {
    u += 1.0f;
  }
  if (v < 0.0f) {
    v += 1.0f;
  }
  const auto x = std::min(
      static_cast<std::size_t>(u * static_cast<float>(width)),
      static_cast<std::size_t>(width - 1));
  const auto y = std::min(
      static_cast<std::size_t>(v * static_cast<float>(height)),
      static_cast<std::size_t>(height - 1));
  return texels[y * static_cast<std::size_t>(width) + x];
}

LabPbrAssetPaths
discoverLabPbrAssets(const std::filesystem::path &base_path) {
  LabPbrAssetPaths paths;
  paths.base = base_path;
  const auto parent = base_path.parent_path();
  const auto stem = base_path.stem().wstring();
  paths.normal = parent / std::filesystem::path(stem + L"_n.png");
  paths.specular = parent / std::filesystem::path(stem + L"_s.png");
  paths.properties = parent / "texture.properties";
  std::error_code error;
  paths.normal_exists = std::filesystem::is_regular_file(paths.normal, error);
  error.clear();
  paths.specular_exists =
      std::filesystem::is_regular_file(paths.specular, error);
  error.clear();
  paths.properties_exists =
      std::filesystem::is_regular_file(paths.properties, error);
  return paths;
}

float srgbToLinear(float value) noexcept {
  value = std::clamp(value, 0.0f, 1.0f);
  if (value <= 0.04045f) {
    return value / 12.92f;
  }
  return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float srgb8ToLinear(std::uint8_t value) noexcept {
  return srgbToLinear(static_cast<float>(value) * kInv255);
}

const char *labPbrDebugViewName(LabPbrDebugView view) noexcept {
  switch (view) {
  case LabPbrDebugView::Shaded:
    return "shaded";
  case LabPbrDebugView::BaseColor:
    return "base-color";
  case LabPbrDebugView::Normal:
    return "normal";
  case LabPbrDebugView::AmbientOcclusion:
    return "ao";
  case LabPbrDebugView::LinearRoughness:
    return "roughness";
  case LabPbrDebugView::F0:
    return "f0";
  case LabPbrDebugView::Emission:
    return "emission";
  case LabPbrDebugView::Opacity:
    return "opacity";
  }
  return "shaded";
}

LabPbrDebugView labPbrDebugViewFromName(std::string_view name) noexcept {
  if (name == "base" || name == "base-color" || name == "albedo") {
    return LabPbrDebugView::BaseColor;
  }
  if (name == "normal") {
    return LabPbrDebugView::Normal;
  }
  if (name == "ao" || name == "ambient-occlusion") {
    return LabPbrDebugView::AmbientOcclusion;
  }
  if (name == "roughness" || name == "linear-roughness") {
    return LabPbrDebugView::LinearRoughness;
  }
  if (name == "f0" || name == "reflectance") {
    return LabPbrDebugView::F0;
  }
  if (name == "emission" || name == "emissive") {
    return LabPbrDebugView::Emission;
  }
  if (name == "opacity" || name == "alpha") {
    return LabPbrDebugView::Opacity;
  }
  return LabPbrDebugView::Shaded;
}

std::array<float, 3>
labPbrDebugColor(const ResolvedMaterialTexel &texel,
                 LabPbrDebugView view) noexcept {
  switch (view) {
  case LabPbrDebugView::BaseColor:
    return texel.base_color_linear;
  case LabPbrDebugView::Normal:
    return {texel.tangent_normal[0] * 0.5f + 0.5f,
            texel.tangent_normal[1] * 0.5f + 0.5f,
            texel.tangent_normal[2] * 0.5f + 0.5f};
  case LabPbrDebugView::AmbientOcclusion:
    return {texel.ambient_occlusion, texel.ambient_occlusion,
            texel.ambient_occlusion};
  case LabPbrDebugView::LinearRoughness:
    return {texel.linear_roughness, texel.linear_roughness,
            texel.linear_roughness};
  case LabPbrDebugView::F0:
    return texel.f0_color;
  case LabPbrDebugView::Emission:
    return texel.emission_linear;
  case LabPbrDebugView::Opacity:
    return {texel.opacity, texel.opacity, texel.opacity};
  case LabPbrDebugView::Shaded:
  default:
    return texel.base_color_linear;
  }
}

std::uint32_t
labPbrFeatureFlags(const ResolvedMaterialTable *material) noexcept {
  if (material == nullptr) {
    return 0u;
  }
  std::uint32_t flags = 0u;
  if (material->normal_map_active) {
    flags |= kLabPbrNormalMapActive;
  }
  if (material->specular_map_active) {
    flags |= kLabPbrSpecularMapActive;
  }
  return flags;
}

ResolvedMaterialTexel decodeLabPbrTexel(
    const std::array<std::uint8_t, 4> &base_rgba,
    const std::array<std::uint8_t, 4> *normal_rgba,
    const std::array<std::uint8_t, 4> *specular_rgba,
    int specular_source_channels) noexcept {
  ResolvedMaterialTexel result;
  for (std::size_t channel = 0; channel < 3u; ++channel) {
    result.base_color_linear[channel] = srgb8ToLinear(base_rgba[channel]);
  }
  result.opacity = static_cast<float>(base_rgba[3]) * kInv255;

  if (normal_rgba != nullptr) {
    float x = 2.0f * static_cast<float>((*normal_rgba)[0]) * kInv255 - 1.0f;
    float y = 1.0f - 2.0f * static_cast<float>((*normal_rgba)[1]) * kInv255;
    const float xy_length_squared = x * x + y * y;
    if (xy_length_squared > 1.0f) {
      const float inverse_length = 1.0f / std::sqrt(xy_length_squared);
      x *= inverse_length;
      y *= inverse_length;
    }
    const float z =
        std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    result.tangent_normal = {x, y, z};
    result.ambient_occlusion =
        static_cast<float>((*normal_rgba)[2]) * kInv255;
    result.stored_height = static_cast<float>((*normal_rgba)[3]) * kInv255;
    result.relative_depth = 0.25f * (1.0f - result.stored_height);
  }

  if (specular_rgba != nullptr) {
    result.perceptual_smoothness =
        static_cast<float>((*specular_rgba)[0]) * kInv255;
    result.perceptual_roughness = 1.0f - result.perceptual_smoothness;
    result.linear_roughness =
        result.perceptual_roughness * result.perceptual_roughness;

    const std::uint8_t reflectance = (*specular_rgba)[1];
    result.metal_code = reflectance;
    if (reflectance <= 229u) {
      result.metal_kind = LabPbrMetalKind::Dielectric;
      result.dielectric_f0 = static_cast<float>(reflectance) * kInv255;
      result.f0_color = {result.dielectric_f0, result.dielectric_f0,
                         result.dielectric_f0};
    } else {
      result.metal_kind = reflectance == 255u
                              ? LabPbrMetalKind::Custom
                              : LabPbrMetalKind::Predefined;
      result.f0_color =
          hardcodedMetalF0(reflectance, result.base_color_linear);
    }

    const std::uint8_t volume = (*specular_rgba)[2];
    if (volume <= 64u) {
      result.porosity = static_cast<float>(volume) / 64.0f;
    } else {
      result.subsurface_scattering =
          static_cast<float>(volume - 65u) / 190.0f;
    }

    const std::uint8_t emission =
        specular_source_channels >= 4 ? (*specular_rgba)[3] : 255u;
    result.emission_strength =
        emission == 255u ? 0.0f : static_cast<float>(emission) / 254.0f;
    for (std::size_t channel = 0; channel < 3u; ++channel) {
      result.emission_linear[channel] =
          result.base_color_linear[channel] * result.emission_strength;
    }
  }
  return result;
}

bool resolveLabPbrMaterial(const TextureImage &base,
                           const std::filesystem::path &base_path,
                           ResolvedMaterialTable &out, std::string *error) {
  ResolvedMaterialTable resolved;
  if (!base.valid()) {
    if (error != nullptr) {
      *error = "cannot resolve LabPBR material from an invalid base texture";
    }
    return false;
  }
  resolved.width = base.width;
  resolved.height = base.height;
  resolved.assets = discoverLabPbrAssets(base_path);
  readFormatDeclaration(resolved);

  const bool has_sidecar =
      resolved.assets.normal_exists || resolved.assets.specular_exists;
  if (resolved.format_declared) {
    if (resolved.declared_format == "lab-pbr/1.3") {
      resolved.format = LabPbrFormat::LabPbr13;
    } else {
      resolved.format = LabPbrFormat::Unsupported;
      addWarning(resolved, "unsupported material format '" +
                               resolved.declared_format +
                               "'; LabPBR sidecars ignored");
    }
  } else if (has_sidecar || resolved.assets.properties_exists) {
    resolved.format = LabPbrFormat::LabPbr13;
    if (!resolved.assets.properties_exists) {
      addWarning(resolved,
                 "LabPBR sidecars found without texture.properties; "
                 "assuming lab-pbr/1.3");
    }
  }

  if (resolved.format != LabPbrFormat::Unsupported) {
    if (resolved.assets.normal_exists) {
      resolved.normal_map_active =
          loadCompatibleSidecar(resolved.assets.normal, base, "normal",
                                resolved.normal_image, resolved);
    }
    if (resolved.assets.specular_exists) {
      resolved.specular_map_active =
          loadCompatibleSidecar(resolved.assets.specular, base, "specular",
                                resolved.specular_image, resolved);
    }
  }

  const auto width = static_cast<std::size_t>(base.width);
  const auto height = static_cast<std::size_t>(base.height);
  if (width > (std::numeric_limits<std::size_t>::max)() / height) {
    if (error != nullptr) {
      *error = "resolved material dimensions overflow";
    }
    return false;
  }
  const std::size_t texel_count = width * height;
  resolved.texels.resize(texel_count);
  for (std::size_t texel = 0; texel < texel_count; ++texel) {
    const std::size_t offset = texel * 4u;
    const std::array<std::uint8_t, 4> base_rgba{
        base.rgba[offset], base.rgba[offset + 1u], base.rgba[offset + 2u],
        base.rgba[offset + 3u]};
    std::array<std::uint8_t, 4> normal_rgba{};
    std::array<std::uint8_t, 4> specular_rgba{};
    const std::array<std::uint8_t, 4> *normal = nullptr;
    const std::array<std::uint8_t, 4> *specular = nullptr;
    if (resolved.normal_map_active) {
      std::copy_n(resolved.normal_image.rgba.data() + offset, 4u,
                  normal_rgba.begin());
      normal = &normal_rgba;
    }
    if (resolved.specular_map_active) {
      std::copy_n(resolved.specular_image.rgba.data() + offset, 4u,
                  specular_rgba.begin());
      specular = &specular_rgba;
    }
    resolved.texels[texel] = decodeLabPbrTexel(
        base_rgba, normal, specular, resolved.specular_image.source_channels);
  }
  out = std::move(resolved);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool sameResolvedMaterialResource(const ResolvedMaterialTable &lhs,
                                  const ResolvedMaterialTable &rhs) noexcept {
  return lhs.width == rhs.width && lhs.height == rhs.height &&
         lhs.format == rhs.format &&
         lhs.declared_format == rhs.declared_format &&
         lhs.format_declared == rhs.format_declared &&
         lhs.normal_map_active == rhs.normal_map_active &&
         lhs.specular_map_active == rhs.specular_map_active &&
         sameImagePixels(lhs.normal_image, rhs.normal_image) &&
         sameImagePixels(lhs.specular_image, rhs.specular_image) &&
         lhs.texels == rhs.texels;
}

TangentFrame computeTangentFrame(
    const std::array<float, 3> &position0,
    const std::array<float, 3> &position1,
    const std::array<float, 3> &position2,
    const std::array<float, 3> &normal,
    const std::array<float, 2> &uv0,
    const std::array<float, 2> &uv1,
    const std::array<float, 2> &uv2) noexcept {
  TangentFrame result;
  const auto n = normalized(normal, {0.0f, 1.0f, 0.0f});
  const std::array<float, 3> edge1{
      position1[0] - position0[0], position1[1] - position0[1],
      position1[2] - position0[2]};
  const std::array<float, 3> edge2{
      position2[0] - position0[0], position2[1] - position0[1],
      position2[2] - position0[2]};
  const float du1 = uv1[0] - uv0[0];
  const float dv1 = uv1[1] - uv0[1];
  const float du2 = uv2[0] - uv0[0];
  const float dv2 = uv2[1] - uv0[1];
  const float determinant = du1 * dv2 - du2 * dv1;
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f) {
    result.tangent = fallbackTangent(n);
    result.used_fallback = true;
    return result;
  }

  const float inverse_determinant = 1.0f / determinant;
  std::array<float, 3> tangent{
      (edge1[0] * dv2 - edge2[0] * dv1) * inverse_determinant,
      (edge1[1] * dv2 - edge2[1] * dv1) * inverse_determinant,
      (edge1[2] * dv2 - edge2[2] * dv1) * inverse_determinant};
  const std::array<float, 3> bitangent{
      (edge2[0] * du1 - edge1[0] * du2) * inverse_determinant,
      (edge2[1] * du1 - edge1[1] * du2) * inverse_determinant,
      (edge2[2] * du1 - edge1[2] * du2) * inverse_determinant};
  const float tangent_dot_normal = dot(tangent, n);
  tangent = {tangent[0] - n[0] * tangent_dot_normal,
             tangent[1] - n[1] * tangent_dot_normal,
             tangent[2] - n[2] * tangent_dot_normal};
  result.tangent = normalized(tangent, fallbackTangent(n));
  result.used_fallback = dot(tangent, tangent) <= 1.0e-20f;
  result.handedness =
      dot(cross(n, result.tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
  return result;
}

} // namespace xpbd::gfx
