#include "xpbd/gfx/labpbr_material.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <new>
#include <sstream>
#include <string_view>
#include <utility>

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

const TextureImage &ResolvedMaterialTable::imageOrEmpty(
    const SharedTextureImage &asset) noexcept {
  static const TextureImage empty;
  return asset != nullptr ? *asset : empty;
}

ResolvedMaterialTable::ResolvedMaterialTable() noexcept
    : base_image(imageOrEmpty(base_image_asset_)),
      normal_image(imageOrEmpty(normal_image_asset_)),
      specular_image(imageOrEmpty(specular_image_asset_)) {}

ResolvedMaterialTable::ResolvedMaterialTable(
    SharedTextureImage base_image_asset,
    SharedTextureImage normal_image_asset,
    SharedTextureImage specular_image_asset) noexcept
    : base_image_asset_(std::move(base_image_asset)),
      normal_image_asset_(std::move(normal_image_asset)),
      specular_image_asset_(std::move(specular_image_asset)),
      base_image(imageOrEmpty(base_image_asset_)),
      normal_image(imageOrEmpty(normal_image_asset_)),
      specular_image(imageOrEmpty(specular_image_asset_)) {}

ResolvedMaterialTable::ResolvedMaterialTable(
    const ResolvedMaterialTable &other)
    : base_image_asset_(other.base_image_asset_),
      normal_image_asset_(other.normal_image_asset_),
      specular_image_asset_(other.specular_image_asset_), width(other.width),
      height(other.height), base_image(imageOrEmpty(base_image_asset_)),
      assets(other.assets), format(other.format),
      declared_format(other.declared_format),
      format_declared(other.format_declared),
      normal_map_active(other.normal_map_active),
      specular_map_active(other.specular_map_active),
      normal_image(imageOrEmpty(normal_image_asset_)),
      specular_image(imageOrEmpty(specular_image_asset_)),
      warnings(other.warnings) {}

ResolvedMaterialTable::ResolvedMaterialTable(
    ResolvedMaterialTable &&other) noexcept
    // Copy the handles so the moved-from object's public image references
    // remain valid for its remaining lifetime.
    : base_image_asset_(other.base_image_asset_),
      normal_image_asset_(other.normal_image_asset_),
      specular_image_asset_(other.specular_image_asset_), width(other.width),
      height(other.height), base_image(imageOrEmpty(base_image_asset_)),
      assets(std::move(other.assets)), format(other.format),
      declared_format(std::move(other.declared_format)),
      format_declared(other.format_declared),
      normal_map_active(other.normal_map_active),
      specular_map_active(other.specular_map_active),
      normal_image(imageOrEmpty(normal_image_asset_)),
      specular_image(imageOrEmpty(specular_image_asset_)),
      warnings(std::move(other.warnings)) {}

ResolvedMaterialTable &ResolvedMaterialTable::operator=(
    const ResolvedMaterialTable &other) {
  if (this == &other) {
    return *this;
  }
  ResolvedMaterialTable replacement(other);
  this->~ResolvedMaterialTable();
  ::new (static_cast<void *>(this))
      ResolvedMaterialTable(std::move(replacement));
  return *this;
}

ResolvedMaterialTable &ResolvedMaterialTable::operator=(
    ResolvedMaterialTable &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  this->~ResolvedMaterialTable();
  ::new (static_cast<void *>(this)) ResolvedMaterialTable(std::move(other));
  return *this;
}

void ResolvedMaterialTable::setImageAssets(
    SharedTextureImage base_image_asset,
    SharedTextureImage normal_image_asset,
    SharedTextureImage specular_image_asset) {
  ResolvedMaterialTable replacement(std::move(base_image_asset),
                                    std::move(normal_image_asset),
                                    std::move(specular_image_asset));
  replacement.width = width;
  replacement.height = height;
  replacement.assets = assets;
  replacement.format = format;
  replacement.declared_format = declared_format;
  replacement.format_declared = format_declared;
  replacement.normal_map_active = normal_map_active;
  replacement.specular_map_active = specular_map_active;
  replacement.warnings = warnings;
  *this = std::move(replacement);
}

bool ResolvedMaterialTable::valid() const noexcept {
  if (width <= 0 || height <= 0 || !base_image.valid() ||
      base_image.width != width || base_image.height != height) {
    return false;
  }
  const auto compatible = [this](const TextureImage &image,
                                 bool active) noexcept {
    return !active ||
           (image.valid() && image.width == width && image.height == height &&
            image.source_channels >= 3);
  };
  return compatible(normal_image, normal_map_active) &&
         compatible(specular_image, specular_map_active);
}

void ResolvedMaterialTable::clear() {
  *this = {};
}

const ResolvedMaterialTexel &ResolvedMaterialTable::sample(float u,
                                                            float v) const {
  static const ResolvedMaterialTexel kFallback{};
  thread_local ResolvedMaterialTexel sampled;
  if (!valid()) {
    return kFallback;
  }
  u = std::clamp(u, 0.0f, 1.0f);
  v = std::clamp(v, 0.0f, 1.0f);
  const auto x = std::min(
      static_cast<std::size_t>(u * static_cast<float>(width)),
      static_cast<std::size_t>(width - 1));
  const auto y = std::min(
      static_cast<std::size_t>(v * static_cast<float>(height)),
      static_cast<std::size_t>(height - 1));
  const std::size_t offset =
      (y * static_cast<std::size_t>(width) + x) * 4u;
  const std::array<std::uint8_t, 4> base_rgba{
      base_image.rgba[offset], base_image.rgba[offset + 1u],
      base_image.rgba[offset + 2u], base_image.rgba[offset + 3u]};
  std::array<std::uint8_t, 4> normal_rgba{};
  std::array<std::uint8_t, 4> specular_rgba{};
  const std::array<std::uint8_t, 4> *normal = nullptr;
  const std::array<std::uint8_t, 4> *specular = nullptr;
  if (normal_map_active) {
    std::copy_n(normal_image.rgba.data() + offset, 4u,
                normal_rgba.begin());
    normal = &normal_rgba;
  }
  if (specular_map_active) {
    std::copy_n(specular_image.rgba.data() + offset, 4u,
                specular_rgba.begin());
    specular = &specular_rgba;
  }
  sampled = decodeLabPbrTexel(base_rgba, normal, specular,
                              specular_image.source_channels);
  return sampled;
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
  case LabPbrDebugView::GgxAlpha:
    return "roughness";
  case LabPbrDebugView::F0:
    return "f0";
  case LabPbrDebugView::Emission:
    return "emission";
  case LabPbrDebugView::Opacity:
    return "opacity";
  case LabPbrDebugView::Height:
    return "height";
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
    return LabPbrDebugView::GgxAlpha;
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
  if (name == "height" || name == "stored-height") {
    return LabPbrDebugView::Height;
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
  case LabPbrDebugView::GgxAlpha:
    return {texel.ggx_alpha, texel.ggx_alpha, texel.ggx_alpha};
  case LabPbrDebugView::F0:
    return texel.f0_color;
  case LabPbrDebugView::Emission:
    return texel.emission_linear;
  case LabPbrDebugView::Opacity:
    return {texel.opacity, texel.opacity, texel.opacity};
  case LabPbrDebugView::Height:
    return {texel.stored_height, texel.stored_height, texel.stored_height};
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
  const LabPbrAlphaProvenance provenance =
      labPbrAlphaProvenance(material);
  if (provenance.base_alpha_authored) {
    flags |= kLabPbrBaseAlphaAuthored;
  }
  if (provenance.height_alpha_authored) {
    flags |= kLabPbrHeightAlphaAuthored;
  }
  if (provenance.emission_alpha_authored) {
    flags |= kLabPbrEmissionAlphaAuthored;
  }
  return flags;
}

LabPbrAlphaProvenance
labPbrAlphaProvenance(const ResolvedMaterialTable *material) noexcept {
  LabPbrAlphaProvenance provenance;
  if (material == nullptr) {
    return provenance;
  }
  provenance.base_source_channels = material->base_image.source_channels;
  provenance.normal_source_channels =
      material->normal_map_active ? material->normal_image.source_channels : 0;
  provenance.specular_source_channels =
      material->specular_map_active ? material->specular_image.source_channels
                                    : 0;
  provenance.base_alpha_authored = provenance.base_source_channels >= 4;
  provenance.height_alpha_authored =
      provenance.normal_source_channels >= 4;
  provenance.emission_alpha_authored =
      provenance.specular_source_channels >= 4;
  return provenance;
}

float labPbrEmissionAliasSupportFloor(
    const ResolvedMaterialTable *material) noexcept {
  if (material == nullptr || !material->valid() ||
      !material->specular_map_active ||
      material->specular_image.source_channels < 4) {
    return 0.0f;
  }
  const std::size_t pixel_count =
      static_cast<std::size_t>(material->width) *
      static_cast<std::size_t>(material->height);
  float maximum_emission = 0.0f;
  float maximum_covered_base_luminance = 0.0f;
  for (std::size_t pixel = 0u; pixel < pixel_count; ++pixel) {
    const std::size_t offset = pixel * 4u;
    const std::uint8_t emission_code =
        material->specular_image.rgba[offset + 3u];
    const std::uint8_t coverage_code =
        material->base_image.rgba[offset + 3u];
    if (emission_code != 255u) {
      maximum_emission = std::max(
          maximum_emission,
          static_cast<float>(emission_code) / 254.0f);
    }
    if (coverage_code < 6u) {
      continue;
    }
    const float coverage =
        static_cast<float>(coverage_code) / 255.0f;
    const float covered_base_luminance =
        (0.2126f * srgb8ToLinear(material->base_image.rgba[offset + 0u]) +
         0.7152f * srgb8ToLinear(material->base_image.rgba[offset + 1u]) +
         0.0722f * srgb8ToLinear(material->base_image.rgba[offset + 2u])) *
        coverage;
    if (std::isfinite(covered_base_luminance)) {
      maximum_covered_base_luminance = std::max(
          maximum_covered_base_luminance, covered_base_luminance);
    }
  }
  const float maximum_luminance =
      maximum_emission * maximum_covered_base_luminance;
  return maximum_luminance > 0.0f
             ? std::max(maximum_luminance * 1.0e-6f, 1.0e-8f)
             : 0.0f;
}

std::uint64_t labPbrEmissionContentKey(
    const ResolvedMaterialTable *material) noexcept {
  if (material == nullptr || !material->valid() ||
      !material->specular_map_active ||
      material->specular_image.source_channels < 4 ||
      !material->base_image.valid() ||
      !material->specular_image.valid() ||
      material->base_image.width != material->specular_image.width ||
      material->base_image.height != material->specular_image.height) {
    return 0u;
  }
  std::uint64_t key = 14695981039346656037ull;
  const auto mix_byte = [&](std::uint8_t value) {
    key ^= value;
    key *= 1099511628211ull;
  };
  const auto mix_u64 = [&](std::uint64_t value) {
    for (std::uint32_t byte = 0u; byte < 8u; ++byte) {
      mix_byte(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
  };
  bool positive_emission = false;
  mix_u64(material->base_image.width);
  mix_u64(material->base_image.height);
  const std::size_t pixel_count =
      material->specular_image.rgba.size() / 4u;
  for (std::size_t pixel = 0u; pixel < pixel_count; ++pixel) {
    const std::uint8_t emission =
        material->specular_image.rgba[pixel * 4u + 3u];
    if (emission == 0u || emission == 255u) {
      continue;
    }
    positive_emission = true;
    mix_u64(static_cast<std::uint64_t>(pixel));
    mix_byte(emission);
    mix_byte(material->base_image.rgba[pixel * 4u + 0u]);
    mix_byte(material->base_image.rgba[pixel * 4u + 1u]);
    mix_byte(material->base_image.rgba[pixel * 4u + 2u]);
    mix_byte(material->base_image.rgba[pixel * 4u + 3u]);
  }
  return positive_emission ? key : 0u;
}

float labPbrEmissionCoverageWeight(float opacity, bool cutout,
                                   bool blend) noexcept {
  const float coverage = std::isfinite(opacity)
                             ? std::clamp(opacity, 0.0f, 1.0f)
                             : 1.0f;
  if ((cutout || blend) && !(coverage >= 0.02f)) {
    return 0.0f;
  }
  return blend ? coverage : 1.0f;
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
    result.ggx_alpha =
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
      if (reflectance >= 230u && reflectance <= 237u) {
        result.metal_reflection_tint = result.base_color_linear;
      }
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

  TextureImage normal_image;
  TextureImage specular_image;

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
                                normal_image, resolved);
    }
    if (resolved.assets.specular_exists) {
      resolved.specular_map_active =
          loadCompatibleSidecar(resolved.assets.specular, base, "specular",
                                specular_image, resolved);
    }
  }

  resolved.setImageAssets(
      std::make_shared<const TextureImage>(base),
      resolved.normal_map_active
          ? std::make_shared<const TextureImage>(std::move(normal_image))
          : SharedTextureImage{},
      resolved.specular_map_active
          ? std::make_shared<const TextureImage>(std::move(specular_image))
          : SharedTextureImage{});

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
         sameImagePixels(lhs.base_image, rhs.base_image) &&
         sameImagePixels(lhs.normal_image, rhs.normal_image) &&
         sameImagePixels(lhs.specular_image, rhs.specular_image);
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
