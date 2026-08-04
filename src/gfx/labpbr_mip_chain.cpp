#include "xpbd/gfx/labpbr_mip_chain.hpp"

#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace xpbd::gfx {
namespace {

constexpr float kInv255 = 1.0f / 255.0f;
constexpr float kAlphaCutoff = 0.02f;
constexpr float kAlphaEpsilon = 1.0e-6f;
constexpr double kIntegerTolerance = 1.0e-7;

struct Rect {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

[[nodiscard]] bool checkedRgbaBytes(std::uint32_t width,
                                    std::uint32_t height,
                                    std::size_t &out) noexcept {
  return checkedTextureRgbaByteCount(static_cast<std::size_t>(width),
                                     static_cast<std::size_t>(height), out);
}

[[nodiscard]] std::uint8_t encodeByte(float value) noexcept {
  if (!std::isfinite(value)) {
    value = 0.0f;
  }
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<std::uint8_t>(std::lround(value * 255.0f));
}

[[nodiscard]] std::uint8_t encodeSrgb(float linear) noexcept {
  if (!std::isfinite(linear)) {
    linear = 0.0f;
  }
  linear = std::clamp(linear, 0.0f, 1.0f);
  const float srgb = linear <= 0.0031308f
                         ? linear * 12.92f
                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
  return encodeByte(srgb);
}

[[nodiscard]] std::uint8_t encodeEmission(float value) noexcept {
  if (!std::isfinite(value) || value <= kAlphaEpsilon) {
    return 255u;
  }
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<std::uint8_t>(std::clamp(
      static_cast<int>(std::lround(value * 254.0f)), 0, 254));
}

[[nodiscard]] bool rectOverlap(const Rect &a, const Rect &b) noexcept {
  const std::uint64_t a_x1 = static_cast<std::uint64_t>(a.x) + a.width;
  const std::uint64_t a_y1 = static_cast<std::uint64_t>(a.y) + a.height;
  const std::uint64_t b_x1 = static_cast<std::uint64_t>(b.x) + b.width;
  const std::uint64_t b_y1 = static_cast<std::uint64_t>(b.y) + b.height;
  return a.x < b_x1 && b.x < a_x1 && a.y < b_y1 && b.y < a_y1;
}

[[nodiscard]] Rect nextRect(const Rect &rect) noexcept {
  const std::uint64_t x1 = static_cast<std::uint64_t>(rect.x) + rect.width;
  const std::uint64_t y1 = static_cast<std::uint64_t>(rect.y) + rect.height;
  Rect result;
  result.x = rect.x / 2u;
  result.y = rect.y / 2u;
  result.width = static_cast<std::uint32_t>((x1 + 1u) / 2u - result.x);
  result.height = static_cast<std::uint32_t>((y1 + 1u) / 2u - result.y);
  return result;
}

[[nodiscard]] std::size_t pixelOffset(const LabPbrMipLevel &level,
                                      std::uint32_t x,
                                      std::uint32_t y) noexcept {
  return (static_cast<std::size_t>(y) * level.width + x) * 4u;
}

using Pixel = std::array<std::uint8_t, 4>;

[[nodiscard]] Pixel
readPixel(const LabPbrMipLevel &level, std::uint32_t x,
          std::uint32_t y) noexcept {
  const std::size_t offset = pixelOffset(level, x, y);
  return {level.rgba[offset + 0u], level.rgba[offset + 1u],
          level.rgba[offset + 2u], level.rgba[offset + 3u]};
}

struct PixelFootprint {
  std::array<Pixel, 4> samples{};
  std::array<std::array<std::uint32_t, 2>, 4> coordinates{};
  std::size_t count = 0u;
};

[[nodiscard]] PixelFootprint sourceFootprint(const LabPbrMipLevel &level,
                                             const Rect &rect,
                                             std::uint32_t dst_x,
                                             std::uint32_t dst_y) noexcept {
  PixelFootprint footprint;
  const std::uint64_t raw_x = static_cast<std::uint64_t>(dst_x) * 2u;
  const std::uint64_t raw_y = static_cast<std::uint64_t>(dst_y) * 2u;
  const std::uint32_t x_begin = std::max(
      rect.x, static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    raw_x, (std::numeric_limits<std::uint32_t>::max)())));
  const std::uint32_t y_begin = std::max(
      rect.y, static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    raw_y, (std::numeric_limits<std::uint32_t>::max)())));
  const std::uint32_t x_end = std::min(
      rect.x + rect.width,
      static_cast<std::uint32_t>(std::min<std::uint64_t>(
          raw_x + 2u, (std::numeric_limits<std::uint32_t>::max)())));
  const std::uint32_t y_end = std::min(
      rect.y + rect.height,
      static_cast<std::uint32_t>(std::min<std::uint64_t>(
          raw_y + 2u, (std::numeric_limits<std::uint32_t>::max)())));
  for (std::uint32_t y = y_begin; y < y_end && y < level.height; ++y) {
    for (std::uint32_t x = x_begin; x < x_end && x < level.width; ++x) {
      footprint.samples[footprint.count] = readPixel(level, x, y);
      footprint.coordinates[footprint.count] = {x, y};
      ++footprint.count;
    }
  }
  if (footprint.count == 0u) {
    const std::uint32_t x = std::clamp(
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            raw_x, level.width - 1u)),
        rect.x, rect.x + rect.width - 1u);
    const std::uint32_t y = std::clamp(
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            raw_y, level.height - 1u)),
        rect.y, rect.y + rect.height - 1u);
    footprint.samples[0] = readPixel(level, x, y);
    footprint.coordinates[0] = {x, y};
    footprint.count = 1u;
  }
  return footprint;
}

void setPixel(LabPbrMipLevel &level, std::uint32_t x, std::uint32_t y,
              const Pixel &pixel) noexcept {
  const std::size_t offset = pixelOffset(level, x, y);
  std::copy(pixel.begin(), pixel.end(), level.rgba.begin() + offset);
}

[[nodiscard]] Pixel filterBase(std::span<const Pixel> samples) noexcept {
  std::array<float, 3> premul{};
  float alpha = 0.0f;
  for (const auto &sample : samples) {
    const float a = static_cast<float>(sample[3]) * kInv255;
    alpha += a;
    for (std::size_t c = 0; c < 3u; ++c) {
      premul[c] += srgb8ToLinear(sample[c]) * a;
    }
  }
  const float inv_count = 1.0f / static_cast<float>(samples.size());
  alpha *= inv_count;
  Pixel result{};
  for (std::size_t c = 0; c < 3u; ++c) {
    const float linear = alpha > kAlphaEpsilon
                             ? premul[c] * inv_count / alpha
                             : 0.0f;
    result[c] = encodeSrgb(linear);
  }
  result[3] = encodeByte(alpha);
  return result;
}

[[nodiscard]] Pixel filterNormal(std::span<const Pixel> samples) noexcept {
  std::array<float, 3> normal{};
  float ao = 0.0f;
  float height = 0.0f;
  for (const auto &sample : samples) {
    float x = 2.0f * static_cast<float>(sample[0]) * kInv255 - 1.0f;
    float y = 1.0f - 2.0f * static_cast<float>(sample[1]) * kInv255;
    const float xy2 = x * x + y * y;
    if (xy2 > 1.0f) {
      const float inv = 1.0f / std::sqrt(xy2);
      x *= inv;
      y *= inv;
    }
    normal[0] += x;
    normal[1] += y;
    normal[2] += std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    ao += static_cast<float>(sample[2]) * kInv255;
    height += static_cast<float>(sample[3]) * kInv255;
  }
  const float inv_count = 1.0f / static_cast<float>(samples.size());
  normal[0] *= inv_count;
  normal[1] *= inv_count;
  normal[2] *= inv_count;
  const float length = std::sqrt(normal[0] * normal[0] +
                                 normal[1] * normal[1] +
                                 normal[2] * normal[2]);
  if (!(length > kAlphaEpsilon) || !std::isfinite(length)) {
    normal = {0.0f, 0.0f, 1.0f};
  } else {
    const float inv_length = 1.0f / length;
    normal[0] *= inv_length;
    normal[1] *= inv_length;
    normal[2] *= inv_length;
  }
  return {encodeByte(0.5f * (normal[0] + 1.0f)),
          encodeByte(0.5f * (1.0f - normal[1])), encodeByte(ao * inv_count),
          encodeByte(height * inv_count)};
}

struct SpecSample {
  std::uint8_t smoothness = 0;
  std::uint8_t reflectance = 0;
  std::uint8_t auxiliary = 0;
  std::uint8_t emission = 255;
  float coverage = 1.0f;
};

[[nodiscard]] std::uint8_t
filterSpecular(std::span<const Pixel> samples,
               std::span<const float> coverages, std::size_t channel) noexcept {
  std::array<SpecSample, 4> decoded{};
  const std::size_t count = (std::min)(samples.size(), decoded.size());
  float alpha_sum = 0.0f;
  std::array<float, 3> class_weight{}; // dielectric, predefined, custom
  std::array<float, 8> predefined{};
  float dielectric_f0 = 0.0f;
  std::size_t dielectric_count = 0u;
  float emission = 0.0f;
  std::array<std::uint8_t, 4> auxiliaries{};
  for (std::size_t i = 0; i < count; ++i) {
    decoded[i].smoothness = samples[i][0];
    decoded[i].reflectance = samples[i][1];
    decoded[i].auxiliary = samples[i][2];
    decoded[i].emission = samples[i][3];
    decoded[i].coverage = i < coverages.size()
                              ? std::clamp(coverages[i], 0.0f, 1.0f)
                              : 1.0f;
    const float weight = decoded[i].coverage;
    alpha_sum += std::pow(
        1.0f - static_cast<float>(decoded[i].smoothness) * kInv255, 2.0f);
    emission += decoded[i].emission == 255u
                    ? 0.0f
                    : static_cast<float>(decoded[i].emission) / 254.0f;
    auxiliaries[i] = decoded[i].auxiliary;
    if (decoded[i].reflectance <= 229u) {
      class_weight[0] += weight;
      dielectric_f0 += static_cast<float>(decoded[i].reflectance) * kInv255;
      ++dielectric_count;
    } else if (decoded[i].reflectance <= 237u) {
      class_weight[1] += weight;
      predefined[decoded[i].reflectance - 230u] += weight;
    } else {
      class_weight[2] += weight;
    }
  }
  if (channel == 0u) {
    const float mean_alpha = alpha_sum / static_cast<float>(count);
    return encodeByte(1.0f - std::sqrt(std::clamp(mean_alpha, 0.0f, 1.0f)));
  }
  if (channel == 2u) {
    std::sort(auxiliaries.begin(), auxiliaries.begin() + count);
    return auxiliaries[(count - 1u) / 2u];
  }
  if (channel == 3u) {
    return encodeEmission(emission / static_cast<float>(count));
  }

  const float epsilon = 1.0e-7f;
  const float dielectric = class_weight[0];
  const float predefined_weight = class_weight[1];
  const float custom = class_weight[2];
  const float metal = predefined_weight + custom;
  if (dielectric + epsilon >= metal) {
    const float mean_f0 = dielectric_count > 0u
                              ? dielectric_f0 /
                                    static_cast<float>(dielectric_count)
                              : 0.0f;
    return static_cast<std::uint8_t>(std::min(
        229, static_cast<int>(std::lround(std::clamp(mean_f0, 0.0f,
                                                     229.0f * kInv255) *
                                            255.0f))));
  }
  if (predefined_weight + epsilon >= custom) {
    std::uint8_t selected = 230u;
    float selected_weight = -1.0f;
    for (std::size_t i = 0; i < 8u; ++i) {
      const float weight = predefined[i];
      if (weight > selected_weight + epsilon) {
        selected_weight = weight;
        selected = static_cast<std::uint8_t>(230u + i);
      }
    }
    return selected;
  }
  return 255u;
}

[[nodiscard]] Pixel filterSpec(std::span<const Pixel> samples,
                               std::span<const float> coverages) noexcept {
  return {filterSpecular(samples, coverages, 0u),
          filterSpecular(samples, coverages, 1u),
          filterSpecular(samples, coverages, 2u),
          filterSpecular(samples, coverages, 3u)};
}

void preserveCutoutCoverage(LabPbrMipLevel &level, const Rect &previous_rect,
                            const Rect &destination_rect,
                            const LabPbrMipLevel &previous) noexcept {
  std::size_t source_covered = 0u;
  std::size_t source_count = 0u;
  for (std::uint32_t y = previous_rect.y;
       y < previous_rect.y + previous_rect.height; ++y) {
    for (std::uint32_t x = previous_rect.x;
         x < previous_rect.x + previous_rect.width; ++x) {
      ++source_count;
      if (previous.rgba[pixelOffset(previous, x, y) + 3u] * kInv255 >=
          kAlphaCutoff) {
        ++source_covered;
      }
    }
  }
  if (source_count == 0u) {
    return;
  }
  const float target = static_cast<float>(source_covered) /
                       static_cast<float>(source_count);
  const std::size_t destination_count =
      static_cast<std::size_t>(destination_rect.width) *
      destination_rect.height;
  if (destination_count == 0u) {
    return;
  }
  auto covered = [&](float scale) {
    std::size_t count = 0u;
    for (std::uint32_t y = destination_rect.y;
         y < destination_rect.y + destination_rect.height; ++y) {
      for (std::uint32_t x = destination_rect.x;
           x < destination_rect.x + destination_rect.width; ++x) {
        const float value =
            level.rgba[pixelOffset(level, x, y) + 3u] * kInv255;
        count += value * scale >= kAlphaCutoff ? 1u : 0u;
      }
    }
    return static_cast<float>(count) /
           static_cast<float>(destination_count);
  };
  float low = 0.0f;
  float high = 8.0f;
  for (int iteration = 0; iteration < 24; ++iteration) {
    const float middle = 0.5f * (low + high);
    if (covered(middle) < target) {
      low = middle;
    } else {
      high = middle;
    }
  }
  const float scale = target <= 0.0f ? 0.0f : high;
  for (std::uint32_t y = destination_rect.y;
       y < destination_rect.y + destination_rect.height; ++y) {
    for (std::uint32_t x = destination_rect.x;
         x < destination_rect.x + destination_rect.width; ++x) {
      const std::size_t offset = pixelOffset(level, x, y) + 3u;
      const float adjusted =
          std::clamp(level.rgba[offset] * kInv255 * scale, 0.0f, 1.0f);
      std::uint8_t encoded = encodeByte(adjusted);
      if (adjusted >= kAlphaCutoff && encoded * kInv255 < kAlphaCutoff) {
        encoded = static_cast<std::uint8_t>(
            std::ceil(kAlphaCutoff * 255.0f));
      }
      level.rgba[offset] = encoded;
    }
  }
}

[[nodiscard]] bool normalizedIsland(const LabPbrAtlasIsland &island,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    Rect &out) noexcept {
  if (island.width == 0u || island.height == 0u || island.x >= width ||
      island.y >= height || island.width > width - island.x ||
      island.height > height - island.y) {
    return false;
  }
  out = {island.x, island.y, island.width, island.height};
  return true;
}

[[nodiscard]] bool approximatelyInteger(double value,
                                         std::uint32_t &out) noexcept {
  if (!std::isfinite(value) || value < 0.0 ||
      value > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())) {
    return false;
  }
  const double rounded = std::round(value);
  if (std::abs(value - rounded) > kIntegerTolerance) {
    return false;
  }
  out = static_cast<std::uint32_t>(rounded);
  return true;
}

void setError(std::string *error, std::string_view message) {
  if (error != nullptr) {
    *error = std::string(message);
  }
}

} // namespace

bool LabPbrMipLevel::valid() const noexcept {
  std::size_t expected = 0u;
  return width > 0u && height > 0u && checkedRgbaBytes(width, height, expected) &&
         rgba.size() == expected;
}

bool LabPbrMipChain::valid() const noexcept {
  return !levels.empty() && safe_max_lod < levels.size() &&
         std::all_of(levels.begin(), levels.end(),
                     [](const LabPbrMipLevel &level) { return level.valid(); });
}

const LabPbrMipLevel *LabPbrMipChain::baseLevel() const noexcept {
  return levels.empty() ? nullptr : &levels.front();
}

bool buildLabPbrAtlasIslands(const StaticIndexedModelMesh &mesh,
                             const TextureImage &texture,
                             std::vector<LabPbrAtlasIsland> &out,
                             std::string *error) {
  out.clear();
  if (!texture.valid()) {
    setError(error, "invalid source texture");
    return false;
  }
  if (!mesh.uv_domain.valid() || mesh.uv_domain.imported_width != texture.width ||
      mesh.uv_domain.imported_height != texture.height) {
    setError(error, "UV domain is missing or does not match imported texture");
    return false;
  }
  const double scale_u = static_cast<double>(texture.width) / mesh.uv_domain.width;
  const double scale_v = static_cast<double>(texture.height) / mesh.uv_domain.height;
  if (!std::isfinite(scale_u) || !std::isfinite(scale_v) || scale_u <= 0.0 ||
      scale_v <= 0.0) {
    setError(error, "UV domain scale is not finite");
    return false;
  }

  for (std::size_t face_index = 0; face_index < mesh.faces.size();
       ++face_index) {
    const StaticModelFace &face = mesh.faces[face_index];
    if (!face.textured) {
      continue;
    }
    if (face.vertex_count != 4u || face.index_count != 6u ||
        !detail::validFace(mesh, face)) {
      setError(error, "textured face is not a valid four-corner Bedrock face");
      out.clear();
      return false;
    }
    const std::size_t first = face.first_vertex;
    double min_u = mesh.vertices[first].raw_u;
    double max_u = min_u;
    double min_v = mesh.vertices[first].raw_v;
    double max_v = min_v;
    for (std::size_t i = 1u; i < 4u; ++i) {
      const auto &vertex = mesh.vertices[first + i];
      if (!std::isfinite(vertex.raw_u) || !std::isfinite(vertex.raw_v)) {
        setError(error, "textured face contains non-finite raw UV");
        out.clear();
        return false;
      }
      min_u = (std::min)(min_u, vertex.raw_u);
      max_u = (std::max)(max_u, vertex.raw_u);
      min_v = (std::min)(min_v, vertex.raw_v);
      max_v = (std::max)(max_v, vertex.raw_v);
    }
    std::uint32_t x0 = 0u;
    std::uint32_t x1 = 0u;
    std::uint32_t y0 = 0u;
    std::uint32_t y1 = 0u;
    if (!approximatelyInteger(min_u * scale_u, x0) ||
        !approximatelyInteger(max_u * scale_u, x1) ||
        !approximatelyInteger(min_v * scale_v, y0) ||
        !approximatelyInteger(max_v * scale_v, y1) || x1 <= x0 || y1 <= y0 ||
        x0 >= static_cast<std::uint32_t>(texture.width) ||
        y0 >= static_cast<std::uint32_t>(texture.height) ||
        x1 > static_cast<std::uint32_t>(texture.width) ||
        y1 > static_cast<std::uint32_t>(texture.height)) {
      setError(error, "textured face UV is not a finite in-bounds pixel rectangle");
      out.clear();
      return false;
    }
    const std::array<std::array<std::uint32_t, 2>, 4> corners{{
        {{x0, y0}}, {{x1, y0}}, {{x1, y1}}, {{x0, y1}}}};
    std::array<bool, 4> seen{};
    for (std::size_t i = 0u; i < 4u; ++i) {
      std::uint32_t ux = 0u;
      std::uint32_t vy = 0u;
      if (!approximatelyInteger(mesh.vertices[first + i].raw_u * scale_u, ux) ||
          !approximatelyInteger(mesh.vertices[first + i].raw_v * scale_v, vy)) {
        setError(error, "textured face contains non-integral pixel UV");
        out.clear();
        return false;
      }
      bool matched = false;
      for (std::size_t corner = 0u; corner < corners.size(); ++corner) {
        if (corners[corner][0] == ux && corners[corner][1] == vy) {
          if (seen[corner]) {
            matched = false;
            break;
          }
          seen[corner] = true;
          matched = true;
          break;
        }
      }
      if (!matched) {
        setError(error, "textured face UV is non-rectangular");
        out.clear();
        return false;
      }
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) {
      setError(error, "textured face UV does not cover four rectangle corners");
      out.clear();
      return false;
    }
    LabPbrAtlasIsland island{x0, y0, x1 - x0, y1 - y0,
                             static_cast<std::uint32_t>(face_index)};
    const StaticModelMaterialClass material =
        staticModelFaceMaterial(mesh, face, &texture);
    island.used_by_cutout = material == StaticModelMaterialClass::Cutout;
    island.used_by_blend = material == StaticModelMaterialClass::Blend;
    auto duplicate = std::find_if(
        out.begin(), out.end(), [&](const LabPbrAtlasIsland &candidate) {
          return candidate.x == island.x && candidate.y == island.y &&
                 candidate.width == island.width && candidate.height == island.height;
        });
    if (duplicate != out.end()) {
      duplicate->used_by_cutout = duplicate->used_by_cutout || island.used_by_cutout;
      duplicate->used_by_blend = duplicate->used_by_blend || island.used_by_blend;
    } else {
      out.push_back(island);
    }
  }
  if (out.empty()) {
    setError(error, "no valid textured Bedrock face islands");
    return false;
  }
  for (std::size_t i = 0u; i < out.size(); ++i) {
    Rect a{};
    (void)normalizedIsland(out[i], static_cast<std::uint32_t>(texture.width),
                           static_cast<std::uint32_t>(texture.height), a);
    for (std::size_t j = i + 1u; j < out.size(); ++j) {
      Rect b{};
      (void)normalizedIsland(out[j], static_cast<std::uint32_t>(texture.width),
                             static_cast<std::uint32_t>(texture.height), b);
      if (rectOverlap(a, b)) {
        setError(error, "distinct atlas islands overlap");
        out.clear();
        return false;
      }
    }
  }
  setError(error, {});
  return true;
}

LabPbrMipChain buildLabPbrMipChain(
    const TextureImage &source, std::span<const LabPbrAtlasIsland> islands,
    LabPbrMipSemantic semantic, const LabPbrMipChain *base_color_coverage) {
  LabPbrMipChain chain;
  chain.semantic = semantic;
  if (!source.valid()) {
    chain.fallback_reason = "invalid source texture";
    return chain;
  }
  std::size_t base_bytes = 0u;
  if (!checkedRgbaBytes(static_cast<std::uint32_t>(source.width),
                         static_cast<std::uint32_t>(source.height), base_bytes)) {
    chain.fallback_reason = "source byte size overflow";
    return chain;
  }
  if (source.rgba.size() != base_bytes) {
    chain.fallback_reason = "source byte size mismatch";
    return chain;
  }
  try {
    chain.levels.push_back({static_cast<std::uint32_t>(source.width),
                            static_cast<std::uint32_t>(source.height),
                            source.rgba});
  } catch (...) {
    chain.fallback_reason = "base level allocation failed";
    chain.levels.clear();
    return chain;
  }

  std::vector<LabPbrAtlasIsland> unique;
  unique.reserve(islands.size());
  std::vector<Rect> previous_rects;
  previous_rects.reserve(islands.size());
  for (const LabPbrAtlasIsland &island : islands) {
    Rect rect{};
    if (!normalizedIsland(island, static_cast<std::uint32_t>(source.width),
                          static_cast<std::uint32_t>(source.height), rect)) {
      chain.fallback_reason = "atlas island is out of bounds";
      return chain;
    }
    auto duplicate = std::find_if(
        unique.begin(), unique.end(), [&](const LabPbrAtlasIsland &candidate) {
          return candidate.x == island.x && candidate.y == island.y &&
                 candidate.width == island.width && candidate.height == island.height;
        });
    if (duplicate != unique.end()) {
      duplicate->used_by_cutout = duplicate->used_by_cutout || island.used_by_cutout;
      duplicate->used_by_blend = duplicate->used_by_blend || island.used_by_blend;
    } else {
      unique.push_back(island);
      previous_rects.push_back(rect);
    }
  }
  if (unique.empty()) {
    chain.fallback_reason = "no provable atlas islands";
    return chain;
  }
  for (std::size_t i = 0u; i < previous_rects.size(); ++i) {
    for (std::size_t j = i + 1u; j < previous_rects.size(); ++j) {
      if (rectOverlap(previous_rects[i], previous_rects[j])) {
        chain.fallback_reason = "distinct atlas islands overlap";
        return chain;
      }
    }
  }
  if (base_color_coverage != nullptr &&
      (semantic != LabPbrMipSemantic::SpecularPacked ||
       !base_color_coverage->valid() ||
       base_color_coverage->semantic !=
           LabPbrMipSemantic::BaseColorCoverage ||
       base_color_coverage->levels.front().width !=
           static_cast<std::uint32_t>(source.width) ||
       base_color_coverage->levels.front().height !=
           static_cast<std::uint32_t>(source.height))) {
    chain.fallback_reason = "coverage chain is unavailable";
    return chain;
  }
  chain.atlas_isolation_proven = true;
  if (semantic == LabPbrMipSemantic::BaseColorCoverage &&
      std::any_of(unique.begin(), unique.end(), [](const auto &island) {
        return island.used_by_cutout && island.used_by_blend;
      })) {
    chain.fallback_reason =
        "atlas island is shared by conflicting Cutout and Blend primitives";
    return chain;
  }

  std::uint32_t previous_width = static_cast<std::uint32_t>(source.width);
  std::uint32_t previous_height = static_cast<std::uint32_t>(source.height);
  while (previous_width > 1u || previous_height > 1u) {
    if (base_color_coverage != nullptr &&
        chain.levels.size() >= base_color_coverage->levels.size()) {
      chain.fallback_reason = "coverage chain ends before semantic chain";
      break;
    }
    std::vector<Rect> destination_rects;
    destination_rects.reserve(previous_rects.size());
    for (const Rect &rect : previous_rects) {
      if (rect.width == 0u || rect.height == 0u) {
        destination_rects.clear();
        break;
      }
      destination_rects.push_back(nextRect(rect));
    }
    if (destination_rects.size() != previous_rects.size()) {
      chain.fallback_reason = "island dimension collapsed";
      break;
    }
    const std::uint32_t destination_width = std::max(previous_width / 2u, 1u);
    const std::uint32_t destination_height = std::max(previous_height / 2u, 1u);
    bool collision = false;
    for (const Rect &rect : destination_rects) {
      if (rect.width == 0u || rect.height == 0u || rect.x >= destination_width ||
          rect.y >= destination_height || rect.width > destination_width - rect.x ||
          rect.height > destination_height - rect.y) {
        collision = true;
        break;
      }
    }
    for (std::size_t i = 0u; !collision && i < destination_rects.size(); ++i) {
      for (std::size_t j = i + 1u; j < destination_rects.size(); ++j) {
        if (rectOverlap(destination_rects[i], destination_rects[j])) {
          collision = true;
          break;
        }
      }
    }
    if (collision) {
      chain.fallback_reason = "independent atlas islands collide at next mip";
      break;
    }
    std::size_t bytes = 0u;
    if (!checkedRgbaBytes(destination_width, destination_height, bytes)) {
      chain.fallback_reason = "mip byte size overflow";
      break;
    }
    LabPbrMipLevel next{destination_width, destination_height};
    try {
      next.rgba.assign(bytes, 0u);
    } catch (...) {
      chain.fallback_reason = "mip level allocation failed";
      break;
    }
    std::vector<std::int32_t> owners;
    try {
      owners.assign(static_cast<std::size_t>(destination_width) *
                        destination_height,
                    -1);
    } catch (...) {
      chain.fallback_reason = "mip owner allocation failed";
      break;
    }
    const LabPbrMipLevel &previous = chain.levels.back();
    const LabPbrMipLevel *coverage_level = nullptr;
    if (base_color_coverage != nullptr) {
      coverage_level = &base_color_coverage->levels[chain.levels.size() - 1u];
      if (coverage_level->width != previous.width ||
          coverage_level->height != previous.height) {
        chain.fallback_reason = "coverage chain dimensions diverge";
        break;
      }
    }
    for (std::size_t island_index = 0u; island_index < unique.size();
         ++island_index) {
      const Rect &destination = destination_rects[island_index];
      const Rect &previous_rect = previous_rects[island_index];
      for (std::uint32_t y = destination.y;
           y < destination.y + destination.height; ++y) {
        for (std::uint32_t x = destination.x;
             x < destination.x + destination.width; ++x) {
          const std::size_t owner_offset =
              static_cast<std::size_t>(y) * destination_width + x;
          if (owners[owner_offset] >= 0 &&
              owners[owner_offset] != static_cast<std::int32_t>(island_index)) {
            collision = true;
            break;
          }
          owners[owner_offset] = static_cast<std::int32_t>(island_index);
          const PixelFootprint footprint =
              sourceFootprint(previous, previous_rect, x, y);
          const std::span<const Pixel> samples(footprint.samples.data(),
                                               footprint.count);
          Pixel filtered{};
          if (semantic == LabPbrMipSemantic::BaseColorCoverage) {
            filtered = filterBase(samples);
          } else if (semantic == LabPbrMipSemantic::IrisNormalAoHeight) {
            filtered = filterNormal(samples);
          } else {
            std::array<float, 4> coverage_values{};
            std::span<const float> coverages;
            if (coverage_level != nullptr) {
              for (std::size_t i = 0u; i < footprint.count; ++i) {
                const auto [cx, cy] = footprint.coordinates[i];
                coverage_values[i] =
                    coverage_level->rgba[
                        pixelOffset(*coverage_level, cx, cy) + 3u] *
                    kInv255;
              }
              coverages = std::span<const float>(coverage_values.data(),
                                                  footprint.count);
            }
            filtered = filterSpec(samples, coverages);
          }
          setPixel(next, x, y, filtered);
        }
        if (collision) {
          break;
        }
      }
      if (collision) {
        break;
      }
      if (semantic == LabPbrMipSemantic::BaseColorCoverage &&
          unique[island_index].used_by_cutout &&
          !unique[island_index].used_by_blend) {
        preserveCutoutCoverage(next, previous_rect, destination, previous);
      }
    }
    if (collision) {
      chain.fallback_reason = "island core ownership collision";
      break;
    }
    // Add a one-texel nearest-edge gutter only when exactly one island owns the
    // candidate cell and it is not another island's core.
    std::vector<std::int32_t> gutter_owners(
        static_cast<std::size_t>(destination_width) * destination_height, -1);
    std::vector<std::array<std::uint32_t, 2>> gutter_sources(
        static_cast<std::size_t>(destination_width) * destination_height,
        {0u, 0u});
    bool gutter_collision = false;
    for (std::size_t island_index = 0u; island_index < unique.size();
         ++island_index) {
      const Rect &destination = destination_rects[island_index];
      const std::uint32_t x0 = destination.x == 0u ? 0u : destination.x - 1u;
      const std::uint32_t y0 = destination.y == 0u ? 0u : destination.y - 1u;
      const std::uint32_t x1 = std::min(destination_width,
                                        destination.x + destination.width + 1u);
      const std::uint32_t y1 = std::min(destination_height,
                                        destination.y + destination.height + 1u);
      for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
          const std::size_t offset = static_cast<std::size_t>(y) *
                                     destination_width + x;
          if (owners[offset] >= 0) {
            continue;
          }
          const std::int32_t candidate = static_cast<std::int32_t>(island_index);
          if (gutter_owners[offset] == -2) {
            continue;
          }
          if (gutter_owners[offset] >= 0 &&
              gutter_owners[offset] != candidate) {
            gutter_owners[offset] = -2;
            gutter_collision = true;
            continue;
          }
          gutter_owners[offset] = candidate;
          const std::uint32_t source_x = std::clamp(
              x, destination.x, destination.x + destination.width - 1u);
          const std::uint32_t source_y = std::clamp(
              y, destination.y, destination.y + destination.height - 1u);
          gutter_sources[offset] = {source_x, source_y};
        }
      }
    }
    if (gutter_collision) {
      chain.fallback_reason =
          "independent atlas island gutters collide at next mip";
      break;
    }
    for (std::size_t i = 0u; i < gutter_owners.size(); ++i) {
      if (gutter_owners[i] < 0) {
        continue;
      }
      const std::uint32_t x = static_cast<std::uint32_t>(i % destination_width);
      const std::uint32_t y = static_cast<std::uint32_t>(i / destination_width);
      setPixel(next, x, y,
               readPixel(next, gutter_sources[i][0], gutter_sources[i][1]));
    }
    chain.levels.push_back(std::move(next));
    previous_rects = std::move(destination_rects);
    previous_width = destination_width;
    previous_height = destination_height;
  }
  chain.safe_max_lod = chain.levels.empty()
                          ? 0u
                          : static_cast<std::uint32_t>(chain.levels.size() - 1u);
  return chain;
}

std::size_t labPbrMipChainByteSize(const LabPbrMipChain &chain) noexcept {
  std::size_t total = 0u;
  for (const auto &level : chain.levels) {
    if (level.rgba.size() > (std::numeric_limits<std::size_t>::max)() - total) {
      return (std::numeric_limits<std::size_t>::max)();
    }
    total += level.rgba.size();
  }
  return total;
}

} // namespace xpbd::gfx
