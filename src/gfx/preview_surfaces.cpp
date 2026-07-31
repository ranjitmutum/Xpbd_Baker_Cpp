#include "preview_surfaces.hpp"

#include <FastNoiseLite.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xpbd::gfx::detail {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;

struct Rgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct SurfacePoint {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float nx = 0.0f;
  float ny = 1.0f;
  float nz = 0.0f;
  Rgba color{};
};

[[nodiscard]] Rgb mix(Rgb a, Rgb b, float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
          a.b + (b.b - a.b) * t};
}

[[nodiscard]] float smoothstep(float edge0, float edge1, float value) noexcept {
  const float denominator = std::max(edge1 - edge0, 1.0e-6f);
  const float t = std::clamp((value - edge0) / denominator, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] std::uint32_t hashBits(std::uint32_t value) noexcept {
  value ^= value >> 16u;
  value *= 0x7FEB352Du;
  value ^= value >> 15u;
  value *= 0x846CA68Bu;
  value ^= value >> 16u;
  return value;
}

[[nodiscard]] std::uint32_t seedBits(float scene_seed) noexcept {
  const auto quantized =
      static_cast<std::uint32_t>(std::lround(scene_seed * 1048576.0f));
  return hashBits(quantized ^ 0xA511E9B3u);
}

[[nodiscard]] int noiseSeed(std::uint32_t seed, std::uint32_t salt) noexcept {
  return static_cast<int>(hashBits(seed ^ salt) & 0x7FFFFFFFu);
}

[[nodiscard]] float hashUnit(std::uint32_t seed, std::uint32_t index,
                             std::uint32_t salt) noexcept {
  const std::uint32_t bits =
      hashBits(seed ^ hashBits(index * 0x9E3779B9u + salt));
  return static_cast<float>(bits & 0x00FFFFFFu) / 16777215.0f;
}

void appendSurfaceGrid(ViewportGpuScene &out, int segments,
                       const std::vector<SurfacePoint> &points,
                       bool transparent) {
  const int width = segments + 1;
  std::vector<MeshVertex> &destination =
      transparent ? out.transparent : out.solid;
  destination.reserve(
      destination.size() +
      static_cast<std::size_t>(segments) * static_cast<std::size_t>(segments) *
          6u);

  auto append = [&](int x, int z) {
    const SurfacePoint &point =
        points[static_cast<std::size_t>(z * width + x)];
    MeshVertex vertex{};
    vertex.px = point.x;
    vertex.py = point.y;
    vertex.pz = point.z;
    vertex.nx = point.nx;
    vertex.ny = point.ny;
    vertex.nz = point.nz;
    vertex.r = point.color.r;
    vertex.g = point.color.g;
    vertex.b = point.color.b;
    vertex.a = point.color.a;
    destination.push_back(vertex);
  };

  for (int z = 0; z < segments; ++z) {
    for (int x = 0; x < segments; ++x) {
      if (((x + z) & 1) == 0) {
        append(x, z);
        append(x + 1, z);
        append(x + 1, z + 1);
        append(x, z);
        append(x + 1, z + 1);
        append(x, z + 1);
      } else {
        append(x, z);
        append(x + 1, z);
        append(x, z + 1);
        append(x + 1, z);
        append(x + 1, z + 1);
        append(x, z + 1);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Desert: FastNoiseLite OpenSimplex2, ridged FBm, cellular breakup, and
// progressive domain warp. FastNoiseLite is vendored unchanged under MIT.

class DesertFields {
public:
  explicit DesertFields(float scene_seed)
      : seed_(seedBits(scene_seed)),
        warp_(noiseSeed(seed_, 0x10203040u)),
        macro_(noiseSeed(seed_, 0x22334455u)),
        primary_(noiseSeed(seed_, 0x31415926u)),
        secondary_(noiseSeed(seed_, 0x27182818u)),
        cellular_(noiseSeed(seed_, 0xC001D00Du)),
        grain_(noiseSeed(seed_, 0x51A7E77Eu)) {
    warp_.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
    warp_.SetFractalType(
        FastNoiseLite::FractalType_DomainWarpProgressive);
    warp_.SetFractalOctaves(4);
    warp_.SetFractalLacunarity(2.05f);
    warp_.SetFractalGain(0.52f);
    warp_.SetFrequency(0.0042f);
    warp_.SetDomainWarpAmp(34.0f);

    macro_.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
    macro_.SetFractalType(FastNoiseLite::FractalType_FBm);
    macro_.SetFractalOctaves(5);
    macro_.SetFractalLacunarity(2.08f);
    macro_.SetFractalGain(0.48f);
    macro_.SetFractalWeightedStrength(0.35f);
    macro_.SetFrequency(0.0023f);

    primary_.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    primary_.SetFractalType(FastNoiseLite::FractalType_Ridged);
    primary_.SetFractalOctaves(4);
    primary_.SetFractalLacunarity(2.03f);
    primary_.SetFractalGain(0.52f);
    primary_.SetFrequency(0.0064f);

    secondary_.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
    secondary_.SetFractalType(FastNoiseLite::FractalType_Ridged);
    secondary_.SetFractalOctaves(3);
    secondary_.SetFractalLacunarity(2.11f);
    secondary_.SetFractalGain(0.50f);
    secondary_.SetFrequency(0.0092f);

    cellular_.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
    cellular_.SetCellularDistanceFunction(
        FastNoiseLite::CellularDistanceFunction_Euclidean);
    cellular_.SetCellularReturnType(
        FastNoiseLite::CellularReturnType_Distance2Sub);
    cellular_.SetCellularJitter(0.88f);
    cellular_.SetFrequency(0.0034f);

    grain_.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
    grain_.SetFractalType(FastNoiseLite::FractalType_FBm);
    grain_.SetFractalOctaves(3);
    grain_.SetFractalGain(0.46f);
    grain_.SetFrequency(0.055f);

    wind_angle_ =
        0.52f + (hashUnit(seed_, 0u, 0x1234ABCDu) - 0.5f) * 0.46f;
  }

  [[nodiscard]] float rawHeight(float x, float z) const {
    float warped_x = x;
    float warped_z = z;
    warp_.DomainWarp(warped_x, warped_z);

    const float cosine = std::cos(wind_angle_);
    const float sine = std::sin(wind_angle_);
    const float along = warped_x * cosine + warped_z * sine;
    const float across = -warped_x * sine + warped_z * cosine;

    const float primary =
        0.5f +
        0.5f * primary_.GetNoise(along * 0.56f, across * 1.18f);

    const float cross_angle = wind_angle_ + 0.82f;
    const float cross_cosine = std::cos(cross_angle);
    const float cross_sine = std::sin(cross_angle);
    const float cross_along =
        warped_x * cross_cosine + warped_z * cross_sine;
    const float cross_across =
        -warped_x * cross_sine + warped_z * cross_cosine;
    const float secondary =
        0.5f +
        0.5f * secondary_.GetNoise(cross_along * 0.58f,
                                   cross_across * 1.12f);

    const float macro = macro_.GetNoise(warped_x, warped_z);
    const float basin = cellular_.GetNoise(warped_x, warped_z);
    const float primary_shape =
        std::pow(std::clamp(primary, 0.0f, 1.0f), 1.55f);
    const float secondary_shape =
        std::pow(std::clamp(secondary, 0.0f, 1.0f), 1.85f);
    const float envelope = 0.72f + 0.28f * (0.5f + 0.5f * macro);

    return macro * 7.5f + (primary_shape - 0.43f) * 14.5f * envelope +
           (secondary_shape - 0.36f) * 4.5f + basin * 2.4f;
  }

  [[nodiscard]] float duneBand(float x, float z) const {
    float warped_x = x;
    float warped_z = z;
    warp_.DomainWarp(warped_x, warped_z);
    const float cosine = std::cos(wind_angle_);
    const float sine = std::sin(wind_angle_);
    const float along = warped_x * cosine + warped_z * sine;
    const float across = -warped_x * sine + warped_z * cosine;
    return 0.5f +
           0.5f * primary_.GetNoise(along * 0.56f, across * 1.18f);
  }

  [[nodiscard]] float grain(float x, float z) const {
    return 0.5f + 0.5f * grain_.GetNoise(x, z);
  }

private:
  std::uint32_t seed_ = 0u;
  FastNoiseLite warp_;
  FastNoiseLite macro_;
  FastNoiseLite primary_;
  FastNoiseLite secondary_;
  FastNoiseLite cellular_;
  FastNoiseLite grain_;
  float wind_angle_ = 0.52f;
};

[[nodiscard]] Rgba desertColor(const DesertFields &fields, float x, float z,
                                float height, float slope, float radius,
                                float half_extent) {
  const float dune_band = fields.duneBand(x, z);
  const float grain = fields.grain(x, z);
  const Rgb shadow{0.28f, 0.17f, 0.085f};
  const Rgb ochre{0.52f, 0.34f, 0.16f};
  const Rgb sunlit{0.72f, 0.54f, 0.29f};
  const Rgb pale{0.82f, 0.68f, 0.42f};

  const float height_t =
      std::clamp(0.46f + height * 0.018f + dune_band * 0.22f -
                     slope * 0.20f,
                 0.0f, 1.0f);
  Rgb color = mix(shadow, ochre, height_t);
  color = mix(color, sunlit,
              std::clamp(dune_band * 0.46f + grain * 0.18f, 0.0f, 0.62f));
  color = mix(color, pale,
              std::clamp((dune_band - 0.67f) * 1.15f, 0.0f, 0.30f));
  color = mix(color, {0.65f, 0.54f, 0.38f},
              smoothstep(half_extent * 0.76f, half_extent, radius) * 0.42f);
  const float grit = (grain - 0.5f) * 0.045f;
  color.r = std::clamp(color.r + grit, 0.0f, 1.0f);
  color.g = std::clamp(color.g + grit * 0.88f, 0.0f, 1.0f);
  color.b = std::clamp(color.b + grit * 0.68f, 0.0f, 1.0f);
  return {color.r, color.g, color.b, 1.0f};
}

// ---------------------------------------------------------------------------
// Ocean: C++ port of CaffeineViking/osgw's Gerstner position displacement and
// analytic normal equations. See third_party/osgw/gerstner.glsl and LICENSE.md.

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct GerstnerWave {
  float direction_x = 1.0f;
  float direction_z = 0.0f;
  float amplitude = 0.0f;
  float steepness = 0.0f;
  float frequency = 0.0f;
  float speed = 0.0f;
  float phase_bias = 0.0f;
};

using OceanSpectrum = std::array<GerstnerWave, 12>;

[[nodiscard]] OceanSpectrum buildOceanSpectrum(float scene_seed) {
  constexpr std::array<float, 12> wavelengths{
      118.0f, 84.0f, 60.0f, 43.0f, 31.0f, 23.0f,
      17.0f,  12.5f, 9.5f,  7.2f,  5.6f,  4.5f};
  constexpr std::array<float, 12> amplitudes{
      1.55f, 1.15f, 0.84f, 0.63f, 0.47f, 0.35f,
      0.26f, 0.19f, 0.14f, 0.10f, 0.075f, 0.055f};
  constexpr std::array<float, 12> direction_offsets{
      -0.16f, -0.08f, 0.00f, 0.07f, 0.15f, -0.22f,
      0.25f,  0.38f,  -0.46f, 0.58f, -0.72f, 0.91f};
  constexpr std::array<float, 12> steepness{
      0.40f, 0.42f, 0.44f, 0.44f, 0.42f, 0.40f,
      0.38f, 0.36f, 0.34f, 0.32f, 0.30f, 0.28f};

  const std::uint32_t seed = seedBits(scene_seed);
  const float wind_angle =
      0.46f + (hashUnit(seed, 0u, 0xDEADBEEFu) - 0.5f) * 0.38f;
  OceanSpectrum spectrum{};
  for (std::size_t index = 0; index < spectrum.size(); ++index) {
    const float direction_jitter =
        (hashUnit(seed, static_cast<std::uint32_t>(index), 0xA37F13C5u) -
         0.5f) *
        0.09f;
    const float angle =
        wind_angle + direction_offsets[index] + direction_jitter;
    GerstnerWave &wave = spectrum[index];
    wave.direction_x = std::cos(angle);
    wave.direction_z = std::sin(angle);
    wave.amplitude = amplitudes[index];
    wave.steepness = steepness[index];
    wave.frequency = kTau / wavelengths[index];
    wave.speed = -std::sqrt(9.81f * wave.frequency);
    wave.phase_bias =
        hashUnit(seed, static_cast<std::uint32_t>(index), 0x6C8E9CF5u) * kTau;
  }
  return spectrum;
}

[[nodiscard]] Vec3 gerstnerPosition(const OceanSpectrum &spectrum, float x,
                                    float z, float time_sec) {
  Vec3 position{x, 0.0f, z};
  for (const GerstnerWave &wave : spectrum) {
    const float projection = x * wave.direction_x + z * wave.direction_z;
    const float theta = projection * wave.frequency +
                        time_sec * wave.speed + wave.phase_bias;
    const float height = wave.amplitude * std::sin(theta);
    const float width =
        wave.steepness * wave.amplitude * std::cos(theta);
    position.x += wave.direction_x * width;
    position.y += height;
    position.z += wave.direction_z * width;
  }
  return position;
}

[[nodiscard]] Vec3 gerstnerNormal(const OceanSpectrum &spectrum,
                                  const Vec3 &displaced,
                                  float time_sec) {
  Vec3 normal{0.0f, 1.0f, 0.0f};
  for (const GerstnerWave &wave : spectrum) {
    const float projection = displaced.x * wave.direction_x +
                             displaced.z * wave.direction_z;
    const float psi = projection * wave.frequency +
                      time_sec * wave.speed + wave.phase_bias;
    const float amplitude_frequency = wave.amplitude * wave.frequency;
    const float alpha = amplitude_frequency * std::sin(psi);
    const float omega = amplitude_frequency * std::cos(psi);
    normal.y -= wave.steepness * alpha;
    normal.x -= wave.direction_x * omega;
    normal.z -= wave.direction_z * omega;
  }
  return normal;
}

void normalize(Vec3 &vector) {
  const float length =
      std::sqrt(vector.x * vector.x + vector.y * vector.y +
                vector.z * vector.z);
  if (length <= 1.0e-6f) {
    vector = {0.0f, 1.0f, 0.0f};
    return;
  }
  vector.x /= length;
  vector.y /= length;
  vector.z /= length;
}

class OceanColorFields {
public:
  explicit OceanColorFields(float scene_seed)
      : body_(noiseSeed(seedBits(scene_seed), 0x0CEAA001u)),
        foam_(noiseSeed(seedBits(scene_seed), 0xF0A00011u)) {
    body_.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
    body_.SetFractalType(FastNoiseLite::FractalType_FBm);
    body_.SetFractalOctaves(4);
    body_.SetFractalLacunarity(2.07f);
    body_.SetFractalGain(0.48f);
    body_.SetFrequency(0.0065f);

    foam_.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    foam_.SetFractalType(FastNoiseLite::FractalType_FBm);
    foam_.SetFractalOctaves(3);
    foam_.SetFractalGain(0.47f);
    foam_.SetFrequency(0.047f);
  }

  [[nodiscard]] Rgba color(float x, float z, float height,
                           const Vec3 &normal, float time_sec, float radius,
                           float half_extent) const {
    const float body_noise = 0.5f + 0.5f * body_.GetNoise(x, z);
    const Rgb abyss{0.008f, 0.038f, 0.105f};
    const Rgb deep{0.015f, 0.12f, 0.27f};
    const Rgb teal{0.035f, 0.30f, 0.43f};
    const Rgb sky{0.43f, 0.66f, 0.86f};
    const Rgb foam_color{0.90f, 0.95f, 0.98f};

    Rgb result = mix(abyss, deep, body_noise);
    result = mix(result, teal,
                 std::clamp(0.18f + body_noise * 0.42f, 0.0f, 0.58f));
    const float slope =
        std::sqrt(normal.x * normal.x + normal.z * normal.z) /
        std::max(normal.y, 0.18f);
    const float sky_mix =
        std::clamp(0.14f + (1.0f - normal.y) * 0.38f, 0.10f, 0.44f);
    result = mix(result, sky, sky_mix);
    result = mix(result, {0.46f, 0.68f, 0.82f},
                 smoothstep(half_extent * 0.72f, half_extent, radius) * 0.55f);

    const float foam_noise =
        0.5f + 0.5f * foam_.GetNoise(x, z, time_sec * 7.0f);
    float foam_weight =
        (slope - 0.32f) * 1.25f + (height + 0.05f) * 0.12f +
        (foam_noise - 0.58f) * 0.25f;
    foam_weight = std::clamp(foam_weight, 0.0f, 1.0f);
    foam_weight *= foam_weight;
    result = mix(result, foam_color, foam_weight * 0.86f);

    const float alpha =
        std::clamp(0.74f + foam_weight * 0.23f + slope * 0.035f, 0.70f,
                   0.98f);
    return {result.r, result.g, result.b, alpha};
  }

private:
  FastNoiseLite body_;
  FastNoiseLite foam_;
};

void appendOceanDeepBody(ViewportGpuScene &out, float half_extent) {
  constexpr float y = -5.0f;
  const float half = half_extent * 1.025f;
  constexpr Rgba color{0.006f, 0.028f, 0.080f, 1.0f};
  constexpr std::array<int, 6> order{0, 1, 2, 0, 2, 3};
  const std::array<std::array<float, 3>, 4> positions{{
      {-half, y, -half},
      {half, y, -half},
      {half, y, half},
      {-half, y, half},
  }};
  out.solid.reserve(out.solid.size() + order.size());
  for (const int index : order) {
    MeshVertex vertex{};
    vertex.px = positions[static_cast<std::size_t>(index)][0];
    vertex.py = positions[static_cast<std::size_t>(index)][1];
    vertex.pz = positions[static_cast<std::size_t>(index)][2];
    vertex.nx = 0.0f;
    vertex.ny = 1.0f;
    vertex.nz = 0.0f;
    vertex.r = color.r;
    vertex.g = color.g;
    vertex.b = color.b;
    vertex.a = color.a;
    out.solid.push_back(vertex);
  }
}

} // namespace

void appendOpenSourceDesertSurface(ViewportGpuScene &out, float scene_seed) {
  constexpr float half_extent = 280.0f;
  // 256² keeps dune silhouettes smooth even when the inspection camera moves
  // close to a slip face. This surface is static, so the larger topology is
  // uploaded/built once instead of being regenerated every frame.
  constexpr int segments = 256;
  const int width = segments + 1;
  const float step =
      (2.0f * half_extent) / static_cast<float>(segments);
  const std::size_t point_count =
      static_cast<std::size_t>(width * width);
  DesertFields fields(scene_seed);
  const float center_height = fields.rawHeight(0.0f, 0.0f);

  std::vector<float> heights(point_count);
  for (int z_index = 0; z_index <= segments; ++z_index) {
    for (int x_index = 0; x_index <= segments; ++x_index) {
      const float x =
          -half_extent + static_cast<float>(x_index) * step;
      const float z =
          -half_extent + static_cast<float>(z_index) * step;
      const float radius = std::sqrt(x * x + z * z);
      const float inspection_clearance =
          smoothstep(18.0f, 96.0f, radius);
      const std::size_t index =
          static_cast<std::size_t>(z_index * width + x_index);
      heights[index] =
          (fields.rawHeight(x, z) - center_height) * inspection_clearance -
          0.08f * (1.0f - inspection_clearance);
    }
  }

  // Sand dunes need broad, continuous slip faces. A small separable-equivalent
  // 3x3 Gaussian removes grid-scale ridges while retaining the upstream field's
  // large-scale domain-warped structure.
  std::vector<float> filtered(point_count);
  constexpr std::array<float, 3> weights{1.0f, 2.0f, 1.0f};
  for (int pass = 0; pass < 3; ++pass) {
    for (int z_index = 0; z_index <= segments; ++z_index) {
      for (int x_index = 0; x_index <= segments; ++x_index) {
        float sum = 0.0f;
        float weight_sum = 0.0f;
        for (int dz = -1; dz <= 1; ++dz) {
          const int sample_z = std::clamp(z_index + dz, 0, segments);
          for (int dx = -1; dx <= 1; ++dx) {
            const int sample_x = std::clamp(x_index + dx, 0, segments);
            const float weight =
                weights[static_cast<std::size_t>(dx + 1)] *
                weights[static_cast<std::size_t>(dz + 1)];
            sum += heights[static_cast<std::size_t>(sample_z * width +
                                                    sample_x)] *
                   weight;
            weight_sum += weight;
          }
        }
        filtered[static_cast<std::size_t>(z_index * width + x_index)] =
            sum / weight_sum;
      }
    }
    heights.swap(filtered);
  }

  auto heightAt = [&](int x, int z) {
    x = std::clamp(x, 0, segments);
    z = std::clamp(z, 0, segments);
    return heights[static_cast<std::size_t>(z * width + x)];
  };

  std::vector<SurfacePoint> points(point_count);
  for (int z_index = 0; z_index <= segments; ++z_index) {
    for (int x_index = 0; x_index <= segments; ++x_index) {
      const float x =
          -half_extent + static_cast<float>(x_index) * step;
      const float z =
          -half_extent + static_cast<float>(z_index) * step;
      const std::size_t index =
          static_cast<std::size_t>(z_index * width + x_index);
      const float dx =
          (heightAt(x_index + 1, z_index) -
           heightAt(x_index - 1, z_index)) /
          (2.0f * step);
      const float dz =
          (heightAt(x_index, z_index + 1) -
           heightAt(x_index, z_index - 1)) /
          (2.0f * step);
      Vec3 normal{-dx, 1.0f, -dz};
      normalize(normal);
      const float slope = std::sqrt(dx * dx + dz * dz);
      const float radius = std::sqrt(x * x + z * z);
      points[index] = {
          x,
          heights[index],
          z,
          normal.x,
          normal.y,
          normal.z,
          desertColor(fields, x, z, heights[index], slope, radius,
                       half_extent),
      };
    }
  }

  appendSurfaceGrid(out, segments, points, false);
}

void appendOpenSourceOceanSurface(ViewportGpuScene &out, float time_sec,
                                  bool dynamic, float scene_seed) {
  constexpr float half_extent = 180.0f;
  // 176² is the high-resolution real-time candidate: 21% more triangles than
  // the original 160² surface. Measured 224² and 192² candidates reduced
  // interaction to ~14 and ~19 fps respectively.
  constexpr int segments = 176;
  constexpr float water_level = -0.85f;
  const int width = segments + 1;
  const float step =
      (2.0f * half_extent) / static_cast<float>(segments);
  const std::size_t point_count =
      static_cast<std::size_t>(width * width);
  const float simulation_time = dynamic ? time_sec : 0.0f;
  const OceanSpectrum spectrum = buildOceanSpectrum(scene_seed);
  const OceanColorFields colors(scene_seed);
  std::vector<SurfacePoint> points(point_count);

  for (int z_index = 0; z_index <= segments; ++z_index) {
    for (int x_index = 0; x_index <= segments; ++x_index) {
      const float x =
          -half_extent + static_cast<float>(x_index) * step;
      const float z =
          -half_extent + static_cast<float>(z_index) * step;
      const float radius = std::sqrt(x * x + z * z);
      const float inspection_clearance =
          0.10f + 0.90f * smoothstep(46.0f, 78.0f, radius);

      const Vec3 full_position =
          gerstnerPosition(spectrum, x, z, simulation_time);
      Vec3 normal =
          gerstnerNormal(spectrum, full_position, simulation_time);
      normal.x *= inspection_clearance;
      normal.y = 1.0f + (normal.y - 1.0f) * inspection_clearance;
      normal.z *= inspection_clearance;
      normalize(normal);

      const Vec3 position{
          x + (full_position.x - x) * inspection_clearance,
          water_level + full_position.y * inspection_clearance,
          z + (full_position.z - z) * inspection_clearance,
      };
      const std::size_t index =
          static_cast<std::size_t>(z_index * width + x_index);
      points[index] = {
          position.x,
          position.y,
          position.z,
          normal.x,
          normal.y,
          normal.z,
          colors.color(position.x, position.z, position.y, normal,
                       simulation_time, radius, half_extent),
      };
    }
  }

  appendOceanDeepBody(out, half_extent);
  appendSurfaceGrid(out, segments, points, true);
}

} // namespace xpbd::gfx::detail
