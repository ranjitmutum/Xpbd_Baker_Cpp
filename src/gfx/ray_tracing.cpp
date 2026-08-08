#include "xpbd/gfx/ray_tracing.hpp"
#include "xpbd/gfx/world_environment.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>

namespace xpbd::gfx {
namespace {

bool g_vulkan_path_tracer_ready = false;
bool g_vulkan_rt_pipeline_ready = false;

[[nodiscard]] constexpr bool isPowerOfTwo(std::uint64_t value) noexcept {
  return value != 0u && (value & (value - 1u)) == 0u;
}

[[nodiscard]] bool checkedAdd(std::uint64_t lhs, std::uint64_t rhs,
                              std::uint64_t &result) noexcept {
  if (rhs > (std::numeric_limits<std::uint64_t>::max)() - lhs) {
    return false;
  }
  result = lhs + rhs;
  return true;
}

[[nodiscard]] bool checkedMultiply(std::uint64_t lhs, std::uint64_t rhs,
                                   std::uint64_t &result) noexcept {
  if (lhs != 0u &&
      rhs > (std::numeric_limits<std::uint64_t>::max)() / lhs) {
    return false;
  }
  result = lhs * rhs;
  return true;
}

[[nodiscard]] bool alignUpChecked(std::uint64_t value,
                                  std::uint64_t alignment,
                                  std::uint64_t &result) noexcept {
  std::uint64_t biased = 0;
  if (!isPowerOfTwo(alignment) ||
      !checkedAdd(value, alignment - 1u, biased)) {
    return false;
  }
  result = biased & ~(alignment - 1u);
  return true;
}

[[nodiscard]] constexpr std::uint32_t
hashPathTraceWord(std::uint32_t value) noexcept {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return value;
}

[[nodiscard]] bool finite3(const std::array<float, 3> &value) noexcept {
  return std::isfinite(value[0]) && std::isfinite(value[1]) &&
         std::isfinite(value[2]);
}

using Vec3 = std::array<float, 3>;

[[nodiscard]] constexpr float dot3(const Vec3 &lhs,
                                   const Vec3 &rhs) noexcept {
  return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2];
}

[[nodiscard]] Vec3 normalize3(const Vec3 &value,
                              const Vec3 &fallback) noexcept {
  if (!finite3(value)) {
    return fallback;
  }
  const double length_squared =
      static_cast<double>(value[0]) * value[0] +
      static_cast<double>(value[1]) * value[1] +
      static_cast<double>(value[2]) * value[2];
  if (!std::isfinite(length_squared) || length_squared <= 1.0e-24) {
    return fallback;
  }
  const float inverse_length =
      static_cast<float>(1.0 / std::sqrt(length_squared));
  return {value[0] * inverse_length, value[1] * inverse_length,
          value[2] * inverse_length};
}

[[nodiscard]] Vec3 cross3(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return {lhs[1] * rhs[2] - lhs[2] * rhs[1],
          lhs[2] * rhs[0] - lhs[0] * rhs[2],
          lhs[0] * rhs[1] - lhs[1] * rhs[0]};
}

[[nodiscard]] Vec3 add3(const Vec3 &lhs, const Vec3 &rhs) noexcept {
  return {lhs[0] + rhs[0], lhs[1] + rhs[1], lhs[2] + rhs[2]};
}

[[nodiscard]] Vec3 multiply3(const Vec3 &value, float scalar) noexcept {
  return {value[0] * scalar, value[1] * scalar, value[2] * scalar};
}

[[nodiscard]] float max3(const Vec3 &value) noexcept {
  return std::max(value[0], std::max(value[1], value[2]));
}

[[nodiscard]] float luminance3(const Vec3 &value) noexcept {
  return value[0] * 0.2126f + value[1] * 0.7152f +
         value[2] * 0.0722f;
}

[[nodiscard]] RtBsdfMaterial
normalizeBsdfMaterial(RtBsdfMaterial material) noexcept {
  for (float &component : material.base_color) {
    component =
        std::isfinite(component) ? std::clamp(component, 0.0f, 1.0f) : 0.0f;
  }
  for (float &component : material.f0) {
    component = std::isfinite(component)
                    ? std::clamp(component, 0.0f, 0.99f)
                    : 0.04f;
  }
  material.ggx_alpha =
      std::isfinite(material.ggx_alpha)
          ? std::clamp(material.ggx_alpha, 0.0f, 1.0f)
          : 0.25f;
  material.transmission =
      !material.metal && std::isfinite(material.transmission)
          ? std::clamp(material.transmission, 0.0f, 1.0f)
          : 0.0f;
  material.ior =
      std::isfinite(material.ior)
          ? std::clamp(material.ior, 1.0001f, 99.0f)
          : rtDielectricIorFromF0(luminance3(material.f0));
  return material;
}

} // namespace

RtLightRegistry buildRtLightRegistry(
    const ResolvedEnvironmentView &environment,
    bool environment_sampling_available, const ResolvedSunLight &sun,
    std::uint64_t emissive_generation, float emissive_power_estimate,
    bool emissive_enabled) noexcept {
  RtLightRegistry registry;
  registry.families[0].stable_id = kRtEnvironmentLightId;
  registry.families[0].type = RtLightType::Environment;
  registry.families[0].generation = environmentGeneration(environment);
  registry.families[0].power_estimate =
      std::isfinite(environment.power_estimate)
          ? std::max(environment.power_estimate, 0.0f)
          : 0.0f;
  registry.families[0].enabled =
      environment.lighting_enabled && environment_sampling_available;
  registry.families[0].casts_shadow = true;

  registry.families[1].stable_id = kRtSunDiskLightId;
  registry.families[1].type = RtLightType::SunDisk;
  registry.families[1].generation = sun.generation;
  registry.families[1].power_estimate =
      std::isfinite(sun.power_estimate) ? std::max(sun.power_estimate, 0.0f)
                                        : 0.0f;
  registry.families[1].enabled =
      sun.lighting_enabled && registry.families[1].power_estimate > 0.0f;
  registry.families[1].casts_shadow = sun.casts_shadow;

  registry.families[2].stable_id = kRtEmissiveFamilyLightId;
  registry.families[2].type = RtLightType::EmissiveTriangle;
  registry.families[2].generation = emissive_generation;
  registry.families[2].power_estimate =
      std::isfinite(emissive_power_estimate)
          ? std::max(emissive_power_estimate, 0.0f)
          : 0.0f;
  registry.families[2].enabled =
      emissive_enabled && registry.families[2].power_estimate > 0.0f;
  registry.families[2].casts_shadow = true;
  // Existing material emission is evaluated from both triangle sides. The
  // per-emitter GPU flag makes this explicit and leaves one-sided ABI room.
  registry.families[2].two_sided = true;

  constexpr float kMinimumEnabledWeight = 1.0e-6f;
  double total_weight = 0.0;
  for (RtLightRecord &record : registry.families) {
    if (!record.enabled) {
      continue;
    }
    record.sampling_weight =
        std::max(record.power_estimate, kMinimumEnabledWeight);
    total_weight += record.sampling_weight;
    ++registry.enabled_family_count;
  }
  registry.total_sampling_weight = std::isfinite(total_weight)
                                       ? static_cast<float>(std::min(
                                             total_weight,
                                             static_cast<double>(
                                                 (std::numeric_limits<float>::max)())))
                                       : 0.0f;
  if (total_weight > 0.0 && std::isfinite(total_weight)) {
    for (RtLightRecord &record : registry.families) {
      record.selection_probability =
          record.enabled
              ? static_cast<float>(
                    static_cast<double>(record.sampling_weight) / total_weight)
              : 0.0f;
    }
  }

  auto mix_generation = [](std::uint64_t hash, std::uint64_t value) {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    for (std::uint32_t byte = 0; byte < 8u; ++byte) {
      hash ^= (value >> (byte * 8u)) & 0xffu;
      hash *= kPrime;
    }
    return hash;
  };
  registry.generation = 14695981039346656037ull;
  for (const RtLightRecord &record : registry.families) {
    registry.generation =
        mix_generation(registry.generation, record.stable_id.value);
    registry.generation =
        mix_generation(registry.generation, record.generation);
    registry.generation = mix_generation(
        registry.generation,
        static_cast<std::uint64_t>(
            std::bit_cast<std::uint32_t>(record.power_estimate)));
    registry.generation = mix_generation(
        registry.generation,
        static_cast<std::uint64_t>(
            std::bit_cast<std::uint32_t>(record.selection_probability)));
    registry.generation = mix_generation(
        registry.generation, static_cast<std::uint64_t>(record.enabled));
    registry.generation = mix_generation(
        registry.generation, static_cast<std::uint64_t>(record.casts_shadow));
    registry.generation = mix_generation(
        registry.generation, static_cast<std::uint64_t>(record.two_sided));
  }
  return registry;
}

const RtLightRecord *findRtLight(const RtLightRegistry &registry,
                                RtLightType type) noexcept {
  for (const RtLightRecord &record : registry.families) {
    if (record.type == type) {
      return &record;
    }
  }
  return nullptr;
}

RtLightSelection sampleRtLight(const RtLightRegistry &registry,
                               float family_sample) noexcept {
  RtLightSelection selection;
  if (!(registry.total_sampling_weight > 0.0f)) {
    return selection;
  }
  const float sample =
      std::isfinite(family_sample)
          ? std::clamp(family_sample, 0.0f, std::nextafter(1.0f, 0.0f))
          : 0.0f;
  float cumulative = 0.0f;
  const RtLightRecord *last_enabled = nullptr;
  for (const RtLightRecord &record : registry.families) {
    if (!record.enabled || !(record.selection_probability > 0.0f)) {
      continue;
    }
    last_enabled = &record;
    cumulative += record.selection_probability;
    if (sample < cumulative) {
      selection.stable_id = record.stable_id;
      selection.type = record.type;
      selection.selection_probability = record.selection_probability;
      selection.valid = true;
      return selection;
    }
  }
  if (last_enabled != nullptr) {
    selection.stable_id = last_enabled->stable_id;
    selection.type = last_enabled->type;
    selection.selection_probability = last_enabled->selection_probability;
    selection.valid = true;
  }
  return selection;
}

float lightPdf(const RtLightRegistry &registry, RtLightType type) noexcept {
  const RtLightRecord *record = findRtLight(registry, type);
  return record != nullptr && record->enabled
             ? record->selection_probability
             : 0.0f;
}

float powerEstimate(const RtLightRecord &light) noexcept {
  return std::isfinite(light.power_estimate)
             ? std::max(light.power_estimate, 0.0f)
             : 0.0f;
}

bool isDeltaLight(const RtLightRecord &light) noexcept { return light.delta; }

bool castsShadow(const RtLightRecord &light) noexcept {
  return light.casts_shadow;
}

bool isTwoSided(const RtLightRecord &light) noexcept {
  return light.two_sided;
}

RtStableLightId makeRtEmissiveTriangleStableId(
    const std::array<std::uint32_t, 4> &source_identity,
    std::uint32_t source_instance) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  const auto mix_word = [&](std::uint32_t word) {
    for (std::uint32_t byte = 0; byte < 4u; ++byte) {
      hash ^= (word >> (byte * 8u)) & 0xffu;
      hash *= kPrime;
    }
  };
  mix_word(0x454d4954u);
  mix_word(source_instance);
  for (const std::uint32_t word : source_identity) {
    mix_word(word);
  }
  if (hash == 0u || hash == kRtEnvironmentLightId.value ||
      hash == kRtSunDiskLightId.value ||
      hash == kRtEmissiveFamilyLightId.value) {
    hash ^= 0x9e3779b97f4a7c15ull;
  }
  return {hash};
}

std::array<float, 3> evaluateRtSunDisk(
    const ResolvedSunLight &sun,
    const std::array<float, 3> &direction) noexcept {
  if (!sun.lighting_enabled || !(sun.angular_radius > 0.0f) ||
      !(sun.solid_angle > 0.0f)) {
    return {0.0f, 0.0f, 0.0f};
  }
  const Vec3 axis = normalize3(sun.direction, {0.0f, 1.0f, 0.0f});
  const Vec3 sample_direction =
      normalize3(direction, {0.0f, 1.0f, 0.0f});
  const float cosine = dot3(axis, sample_direction);
  const float edge_cosine = std::cos(sun.angular_radius);
  if (cosine < edge_cosine) {
    return {0.0f, 0.0f, 0.0f};
  }
  const float angular_distance =
      std::acos(std::clamp(cosine, -1.0f, 1.0f));
  const float radial = angular_distance / sun.angular_radius;
  const float transition = std::clamp((radial - 0.94f) / 0.06f, 0.0f, 1.0f);
  const float disk = 1.0f - transition * transition * (3.0f - 2.0f * transition);
  return {sun.radiance[0] * disk, sun.radiance[1] * disk,
          sun.radiance[2] * disk};
}

float rtSunDiskPdf(const ResolvedSunLight &sun,
                   const std::array<float, 3> &direction) noexcept {
  if (!sun.lighting_enabled || !(sun.solid_angle > 0.0f)) {
    return 0.0f;
  }
  const Vec3 axis = normalize3(sun.direction, {0.0f, 1.0f, 0.0f});
  const Vec3 sample_direction =
      normalize3(direction, {0.0f, 1.0f, 0.0f});
  return dot3(axis, sample_direction) >= std::cos(sun.angular_radius)
             ? 1.0f / sun.solid_angle
             : 0.0f;
}

RtSunDiskSample sampleRtSunDisk(const ResolvedSunLight &sun, float sample_u,
                                float sample_v) noexcept {
  RtSunDiskSample sample;
  if (!sun.lighting_enabled || !(sun.solid_angle > 0.0f)) {
    return sample;
  }
  const float u = std::isfinite(sample_u)
                      ? std::clamp(sample_u, 0.0f,
                                   std::nextafter(1.0f, 0.0f))
                      : 0.0f;
  const float v = std::isfinite(sample_v)
                      ? std::clamp(sample_v, 0.0f,
                                   std::nextafter(1.0f, 0.0f))
                      : 0.0f;
  const Vec3 axis = normalize3(sun.direction, {0.0f, 1.0f, 0.0f});
  const Vec3 helper = std::abs(axis[1]) < 0.999f
                          ? Vec3{0.0f, 1.0f, 0.0f}
                          : Vec3{1.0f, 0.0f, 0.0f};
  const Vec3 tangent = normalize3(cross3(helper, axis),
                                  {1.0f, 0.0f, 0.0f});
  const Vec3 bitangent = normalize3(cross3(axis, tangent),
                                    {0.0f, 0.0f, 1.0f});
  const float cosine_theta =
      1.0f - u * (1.0f - std::cos(sun.angular_radius));
  const float sine_theta =
      std::sqrt(std::max(1.0f - cosine_theta * cosine_theta, 0.0f));
  const float phi = 6.28318530717958647692f * v;
  sample.direction = normalize3(
      add3(multiply3(axis, cosine_theta),
           add3(multiply3(tangent, sine_theta * std::cos(phi)),
                multiply3(bitangent, sine_theta * std::sin(phi)))),
      axis);
  sample.radiance = evaluateRtSunDisk(sun, sample.direction);
  sample.pdf = rtSunDiskPdf(sun, sample.direction);
  sample.valid = sample.pdf > 0.0f && max3(sample.radiance) > 0.0f;
  return sample;
}

PathTraceSettings
normalizePathTraceSettings(PathTraceSettings settings) noexcept {
  settings.preset = static_cast<PathTracePreset>(std::clamp(
      static_cast<int>(settings.preset), 0,
      static_cast<int>(PathTracePreset::Custom)));
  settings.source_preset = static_cast<PathTracePreset>(std::clamp(
      static_cast<int>(settings.source_preset), 0,
      static_cast<int>(PathTracePreset::Reference)));
  settings.samples_per_frame =
      std::clamp(settings.samples_per_frame, 1u, 32u);
  if (settings.maximum_samples != 0u) {
    settings.maximum_samples =
        std::clamp(settings.maximum_samples, 32u, 65'536u);
  }
  settings.max_bounces = std::clamp(settings.max_bounces, 1u, 64u);
  settings.max_diffuse_bounces =
      std::min(settings.max_diffuse_bounces, 16u);
  settings.max_glossy_bounces =
      std::min(settings.max_glossy_bounces, 16u);
  settings.max_transmission_bounces =
      std::min(settings.max_transmission_bounces, 32u);
  settings.max_transparent_bounces =
      std::min(settings.max_transparent_bounces, 64u);
  settings.russian_roulette_start =
      std::clamp(settings.russian_roulette_start, 1u,
                 settings.max_bounces);
  if (!std::isfinite(settings.adaptive_noise_threshold)) {
    settings.adaptive_noise_threshold = 0.01f;
  }
  settings.adaptive_noise_threshold =
      std::clamp(settings.adaptive_noise_threshold, 0.0001f, 1.0f);
  settings.adaptive_minimum_samples =
      std::clamp(settings.adaptive_minimum_samples, 1u, 65'536u);
  // MIS is only defined when explicit light sampling is active, and analytic
  // lights have no BSDF-hit endpoint at all. Close both dependencies here so
  // persisted legacy combinations cannot reach the GPU integrator.
  if (settings.multiple_importance_sampling || settings.analytic_lights) {
    settings.next_event_estimation = true;
  }
  if (!std::isfinite(settings.emissive_multiplier)) {
    settings.emissive_multiplier = 1.0f;
  }
  settings.emissive_multiplier =
      std::clamp(settings.emissive_multiplier, 0.0f, 1000.0f);
  settings.light_samples_per_path =
      std::clamp(settings.light_samples_per_path, 1u, 16u);
  if (!std::isfinite(settings.direct_clamp)) {
    settings.direct_clamp = 0.0f;
  }
  if (!std::isfinite(settings.indirect_clamp)) {
    settings.indirect_clamp = 0.0f;
  }
  settings.direct_clamp =
      std::clamp(settings.direct_clamp, 0.0f, 1.0e6f);
  settings.indirect_clamp =
      std::clamp(settings.indirect_clamp, 0.0f, 1.0e6f);
  const int requested_denoiser =
      static_cast<int>(settings.requested_denoiser);
  if (requested_denoiser !=
          static_cast<int>(PathTraceDenoiser::Auto) &&
      requested_denoiser !=
          static_cast<int>(
              PathTraceDenoiser::DlssRayReconstruction) &&
      requested_denoiser !=
          static_cast<int>(PathTraceDenoiser::Raw)) {
    // Retired NRD values 2/3 and any corrupt values migrate to Raw.
    settings.requested_denoiser = PathTraceDenoiser::Raw;
  }
  settings.requested_upscale =
      static_cast<PathTraceUpscale>(std::clamp(
          static_cast<int>(settings.requested_upscale), 0,
          static_cast<int>(PathTraceUpscale::UltraPerformance)));
  // Auto and Ultra Quality were exposed by the early UI prototype but are
  // not user-facing DLSS SR tiers. Preserve their numeric values for old JSON
  // files and migrate both deterministically to Quality.
  if (settings.requested_upscale == PathTraceUpscale::Auto ||
      settings.requested_upscale == PathTraceUpscale::UltraQuality) {
    settings.requested_upscale = PathTraceUpscale::Quality;
  }
  settings.requested_frame_generation =
      static_cast<PathTraceFrameGeneration>(std::clamp(
          static_cast<int>(settings.requested_frame_generation),
          static_cast<int>(PathTraceFrameGeneration::Off),
          static_cast<int>(PathTraceFrameGeneration::On)));
  settings.requested_reflex_mode =
      static_cast<PathTraceReflexMode>(std::clamp(
          static_cast<int>(settings.requested_reflex_mode),
          static_cast<int>(PathTraceReflexMode::Off),
          static_cast<int>(PathTraceReflexMode::OnBoost)));
  if (!std::isfinite(settings.analytic_environment_strength)) {
    settings.analytic_environment_strength = 0.0f;
  }
  settings.analytic_environment_strength =
      std::clamp(settings.analytic_environment_strength, 0.0f, 16.0f);
  if (!std::isfinite(settings.display_exposure_ev)) {
    settings.display_exposure_ev = 0.0f;
  }
  settings.display_exposure_ev =
      std::clamp(settings.display_exposure_ev, -16.0f, 16.0f);
  settings.tone_mapping =
      static_cast<PathTraceToneMapping>(std::clamp(
          static_cast<int>(settings.tone_mapping), 0,
          static_cast<int>(PathTraceToneMapping::Aces)));
  if (!std::isfinite(settings.white_balance_kelvin)) {
    settings.white_balance_kelvin = 6500.0f;
  }
  settings.white_balance_kelvin =
      std::clamp(settings.white_balance_kelvin, 1000.0f, 40'000.0f);
  if (!std::isfinite(settings.bloom_strength)) {
    settings.bloom_strength = 0.0f;
  }
  settings.bloom_strength =
      std::clamp(settings.bloom_strength, 0.0f, 4.0f);
  if (!std::isfinite(settings.preview_resolution_scale)) {
    settings.preview_resolution_scale = 1.0f;
  }
  settings.preview_resolution_scale =
      std::clamp(settings.preview_resolution_scale, 0.25f, 1.0f);
  if (!std::isfinite(settings.target_frame_time_ms)) {
    settings.target_frame_time_ms = 33.3f;
  }
  settings.target_frame_time_ms =
      std::clamp(settings.target_frame_time_ms, 4.0f, 1000.0f);
  settings.interactive_quality =
      static_cast<PathTraceInteractiveQuality>(std::clamp(
          static_cast<int>(settings.interactive_quality), 0,
          static_cast<int>(PathTraceInteractiveQuality::Fast)));
  return settings;
}

PathTraceLightSamplingMode resolvedPathTraceLightSamplingMode(
    const PathTraceSettings &settings) noexcept {
  if (settings.multiple_importance_sampling) {
    return PathTraceLightSamplingMode::Combined;
  }
  if (settings.next_event_estimation || settings.analytic_lights) {
    return PathTraceLightSamplingMode::LightOnly;
  }
  return PathTraceLightSamplingMode::BsdfOnly;
}

float pathTraceLightEndpointWeight(PathTraceLightSamplingMode mode,
                                   bool primary_or_previous_delta,
                                   float bsdf_pdf,
                                   float light_pdf) noexcept {
  const float explicit_pdf =
      std::isfinite(light_pdf) ? std::max(light_pdf, 0.0f) : 0.0f;
  if (primary_or_previous_delta || !(explicit_pdf > 0.0f) ||
      mode == PathTraceLightSamplingMode::BsdfOnly) {
    return 1.0f;
  }
  if (mode == PathTraceLightSamplingMode::LightOnly) {
    return 0.0f;
  }
  if (mode != PathTraceLightSamplingMode::Combined) {
    return 1.0f;
  }

  float sampled_pdf =
      std::isfinite(bsdf_pdf) ? std::max(bsdf_pdf, 0.0f) : 0.0f;
  float other_pdf = explicit_pdf;
  const float scale = std::max(sampled_pdf, other_pdf);
  if (!(scale > 0.0f)) {
    return 0.0f;
  }
  sampled_pdf /= scale;
  other_pdf /= scale;
  const float sampled_squared = sampled_pdf * sampled_pdf;
  const float other_squared = other_pdf * other_pdf;
  return sampled_squared /
         std::max(sampled_squared + other_squared, 1.0e-20f);
}

PathTraceSettings
pathTraceSettingsForPreset(PathTracePreset preset) noexcept {
  PathTraceSettings settings;
  if (preset == PathTracePreset::Custom) {
    preset = PathTracePreset::Realtime;
  }
  settings.preset = preset;
  settings.source_preset = preset;
  switch (preset) {
  case PathTracePreset::Realtime:
    break;
  case PathTracePreset::Balanced:
    settings.samples_per_frame = 2;
    settings.maximum_samples = 1024;
    settings.max_bounces = 6;
    settings.max_diffuse_bounces = 4;
    settings.max_glossy_bounces = 4;
    settings.max_transmission_bounces = 6;
    settings.max_transparent_bounces = 12;
    settings.preview_resolution_scale = 0.75f;
    break;
  case PathTracePreset::HighQuality:
    settings.samples_per_frame = 4;
    settings.maximum_samples = 4096;
    settings.max_bounces = 8;
    settings.max_diffuse_bounces = 8;
    settings.max_glossy_bounces = 8;
    settings.max_transmission_bounces = 12;
    settings.max_transparent_bounces = 24;
    settings.preview_resolution_scale = 1.0f;
    break;
  case PathTracePreset::Reference:
    settings.samples_per_frame = 8;
    settings.maximum_samples = 16'384;
    settings.max_bounces = 12;
    settings.max_diffuse_bounces = 12;
    settings.max_glossy_bounces = 12;
    settings.max_transmission_bounces = 24;
    settings.max_transparent_bounces = 48;
    settings.preview_resolution_scale = 1.0f;
    settings.interactive_quality = PathTraceInteractiveQuality::Full;
    break;
  case PathTracePreset::Custom:
    break;
  }
  return normalizePathTraceSettings(settings);
}

PathTraceSettings
applyPathTracePreset(const PathTraceSettings &current,
                     PathTracePreset preset) noexcept {
  if (preset == PathTracePreset::Custom) {
    PathTraceSettings custom = normalizePathTraceSettings(current);
    custom.preset = PathTracePreset::Custom;
    return custom;
  }
  PathTraceSettings next = pathTraceSettingsForPreset(preset);
  next.requested_frame_generation =
      current.requested_frame_generation;
  next.requested_reflex_mode = current.requested_reflex_mode;
  next.reset_generation = current.reset_generation;
  next.target_generation = current.target_generation;
  next.post_process_generation = current.post_process_generation;
  next.display_generation = current.display_generation;
  return next;
}

PathTraceSettings
restorePathTraceSourcePreset(const PathTraceSettings &current) noexcept {
  return applyPathTracePreset(current, current.source_preset);
}

PathTraceChangeClass classifyPathTraceSettingsChange(
    const PathTraceSettings &before_value,
    const PathTraceSettings &after_value) noexcept {
  const PathTraceSettings before =
      normalizePathTraceSettings(before_value);
  const PathTraceSettings after =
      normalizePathTraceSettings(after_value);
  PathTraceChangeClass changes = PathTraceChangeClass::None;
  if (before.samples_per_frame != after.samples_per_frame ||
      before.maximum_samples != after.maximum_samples ||
      before.target_frame_time_ms != after.target_frame_time_ms ||
      before.interactive_quality != after.interactive_quality ||
      before.accumulate_while_moving != after.accumulate_while_moving ||
      before.pause_accumulation != after.pause_accumulation) {
    changes |= PathTraceChangeClass::SamplingSchedule;
  }
  if (before.max_bounces != after.max_bounces ||
      before.max_diffuse_bounces != after.max_diffuse_bounces ||
      before.max_glossy_bounces != after.max_glossy_bounces ||
      before.max_transmission_bounces !=
          after.max_transmission_bounces ||
      before.max_transparent_bounces !=
          after.max_transparent_bounces ||
      before.russian_roulette_start !=
          after.russian_roulette_start ||
      before.russian_roulette != after.russian_roulette ||
      before.automatic_seed != after.automatic_seed ||
      before.seed != after.seed ||
      before.adaptive_sampling != after.adaptive_sampling ||
      before.adaptive_noise_threshold !=
          after.adaptive_noise_threshold ||
      before.adaptive_minimum_samples !=
          after.adaptive_minimum_samples ||
      before.analytic_lights != after.analytic_lights ||
      before.emissive_surfaces != after.emissive_surfaces ||
      before.next_event_estimation != after.next_event_estimation ||
      before.multiple_importance_sampling !=
          after.multiple_importance_sampling ||
      before.environment_importance_sampling !=
          after.environment_importance_sampling ||
      before.emissive_mesh_sampling != after.emissive_mesh_sampling ||
      before.emissive_multiplier != after.emissive_multiplier ||
      before.light_samples_per_path != after.light_samples_per_path ||
      before.direct_clamp != after.direct_clamp ||
      before.indirect_clamp != after.indirect_clamp ||
      before.analytic_environment_strength !=
          after.analytic_environment_strength ||
      before.nvidia_rt_core_acceleration !=
          after.nvidia_rt_core_acceleration ||
      before.force_software_fallback != after.force_software_fallback) {
    changes |= PathTraceChangeClass::ResetAccumulation;
  }
  if (before.preview_resolution_scale !=
      after.preview_resolution_scale) {
    changes |= PathTraceChangeClass::RecreateTarget;
  }
  if (before.requested_denoiser != after.requested_denoiser ||
      before.requested_upscale != after.requested_upscale ||
      before.requested_frame_generation !=
          after.requested_frame_generation ||
      before.requested_reflex_mode != after.requested_reflex_mode) {
    changes |= PathTraceChangeClass::ReconfigurePostProcess;
  }
  if (before.transparent_background !=
          after.transparent_background ||
      before.display_exposure_ev != after.display_exposure_ev ||
      before.tone_mapping != after.tone_mapping ||
      before.white_balance_kelvin != after.white_balance_kelvin ||
      before.bloom_strength != after.bloom_strength ||
      before.developer_controls != after.developer_controls) {
    changes |= PathTraceChangeClass::DisplayOnly;
  }
  return changes;
}

std::uint32_t
resolvedPathTraceSeed(const PathTraceSettings &settings_value) noexcept {
  const PathTraceSettings settings =
      normalizePathTraceSettings(settings_value);
  if (!settings.automatic_seed) {
    return settings.seed;
  }
  const std::uint32_t low =
      static_cast<std::uint32_t>(settings.reset_generation);
  const std::uint32_t high =
      static_cast<std::uint32_t>(settings.reset_generation >> 32u);
  return hashPathTraceWord(0x52a31f6du ^ low ^ hashPathTraceWord(high));
}

PathTracePostProcessState resolvePathTracePostProcess(
    const PathTraceSettings &settings_value,
    const PathTracePostProcessCapabilities &capabilities) noexcept {
  const PathTraceSettings settings =
      normalizePathTraceSettings(settings_value);
  PathTracePostProcessState state;
  state.requested_denoiser = settings.requested_denoiser;
  state.requested_upscale = settings.requested_upscale;
  const bool rr_quality_mode =
      settings.requested_upscale == PathTraceUpscale::Quality ||
      settings.requested_upscale == PathTraceUpscale::Balanced ||
      settings.requested_upscale == PathTraceUpscale::Performance ||
      settings.requested_upscale == PathTraceUpscale::UltraPerformance;

  switch (settings.requested_denoiser) {
  case PathTraceDenoiser::Auto:
    if (capabilities.dlss_ray_reconstruction && rr_quality_mode) {
      state.active_denoiser =
          PathTraceDenoiser::DlssRayReconstruction;
    } else {
      state.denoiser_supported = false;
      state.rr_mode_required =
          capabilities.dlss_ray_reconstruction && !rr_quality_mode;
    }
    break;
  case PathTraceDenoiser::DlssRayReconstruction:
    state.denoiser_supported =
        capabilities.dlss_ray_reconstruction;
    state.active_denoiser =
        state.denoiser_supported && rr_quality_mode
            ? PathTraceDenoiser::DlssRayReconstruction
            : PathTraceDenoiser::Raw;
    state.rr_mode_required =
        state.denoiser_supported && !rr_quality_mode;
    break;
  case PathTraceDenoiser::Raw:
    break;
  default:
    state.requested_denoiser = PathTraceDenoiser::Raw;
    state.active_denoiser = PathTraceDenoiser::Raw;
    state.denoiser_supported = false;
    break;
  }

  const bool wants_upscale =
      settings.requested_upscale != PathTraceUpscale::Off;
  if (state.active_denoiser ==
      PathTraceDenoiser::DlssRayReconstruction) {
    // RR owns both denoising and reconstruction. The selected low-resolution
    // quality tier configures RR itself, not a second SR evaluation.
    state.upscale_supported = true;
    state.reconstruction_mode = settings.requested_upscale;
    state.active_upscale = PathTraceUpscale::Off;
    state.conflict_resolved = true;
  } else {
    const bool upscale_available =
        settings.requested_upscale == PathTraceUpscale::Dlaa
            ? capabilities.dlaa
            : capabilities.dlss_super_resolution;
    state.upscale_supported = !wants_upscale || upscale_available;
    state.active_upscale =
        state.upscale_supported ? settings.requested_upscale
                                : PathTraceUpscale::Off;
  }
  return state;
}

PathTraceRenderSnapshot
makePathTraceRenderSnapshot(
    const PathTraceSettings &settings) noexcept {
  PathTraceRenderSnapshot snapshot;
  snapshot.settings = normalizePathTraceSettings(settings);
  snapshot.source_generation = snapshot.settings.reset_generation;
  return snapshot;
}

std::uint32_t pathTraceRandomBits(
    std::uint32_t pixel_x, std::uint32_t pixel_y,
    std::uint32_t sample_index, std::uint32_t dimension,
    std::uint32_t seed) noexcept {
  std::uint32_t state = hashPathTraceWord(seed ^ 0x9e3779b9u);
  state = hashPathTraceWord(state ^ pixel_x);
  state = hashPathTraceWord(state ^ (pixel_y + 0x85ebca6bu));
  state = hashPathTraceWord(state ^ (sample_index + 0xc2b2ae35u));
  return hashPathTraceWord(state ^ (dimension + 0x27d4eb2fu));
}

float pathTraceRandom01(
    std::uint32_t pixel_x, std::uint32_t pixel_y,
    std::uint32_t sample_index, std::uint32_t dimension,
    std::uint32_t seed) noexcept {
  // Preserve the upper 24 random bits, producing [0, 1) exactly like RayGen.
  return static_cast<float>(pathTraceRandomBits(
             pixel_x, pixel_y, sample_index, dimension, seed) >>
                            8u) *
         (1.0f / 16'777'216.0f);
}

PathTraceRngState makePathTraceRngState(
    std::uint32_t pixel_x, std::uint32_t pixel_y,
    std::uint32_t sample_index, std::uint32_t bounce,
    PathTraceRngDomain domain, std::uint32_t stream,
    std::uint32_t seed) noexcept {
  std::uint32_t domain_key =
      hashPathTraceWord(static_cast<std::uint32_t>(domain));
  domain_key =
      hashPathTraceWord(domain_key ^ (bounce + 0x68bc21ebu));
  domain_key =
      hashPathTraceWord(domain_key ^ (stream + 0x02e5be93u));
  return {pathTraceRandomBits(pixel_x, pixel_y, sample_index, domain_key,
                              seed),
          0u};
}

std::uint32_t
pathTraceNextRandomBits(PathTraceRngState &rng) noexcept {
  rng.state = hashPathTraceWord(
      rng.state ^ (rng.dimension + 0x9e3779b9u));
  ++rng.dimension;
  return rng.state;
}

float pathTraceNextRandom01(PathTraceRngState &rng) noexcept {
  return static_cast<float>(pathTraceNextRandomBits(rng) >> 8u) *
         (1.0f / 16'777'216.0f);
}

std::array<float, 3> pathTraceTriangleGeometricNormal(
    const std::array<float, 3> &position0,
    const std::array<float, 3> &position1,
    const std::array<float, 3> &position2,
    const std::array<float, 3> &fallback) noexcept {
  const Vec3 edge1{position1[0] - position0[0],
                   position1[1] - position0[1],
                   position1[2] - position0[2]};
  const Vec3 edge2{position2[0] - position0[0],
                   position2[1] - position0[1],
                   position2[2] - position0[2]};
  return normalize3(cross3(edge1, edge2),
                    normalize3(fallback, {0.0f, 1.0f, 0.0f}));
}

std::array<float, 3> offsetPathTraceRayOrigin(
    const std::array<float, 3> &position,
    const std::array<float, 3> &geometric_normal,
    const std::array<float, 3> &outgoing_direction) noexcept {
  if (!finite3(position) || !finite3(geometric_normal) ||
      !finite3(outgoing_direction)) {
    return position;
  }
  Vec3 oriented =
      normalize3(geometric_normal, {0.0f, 1.0f, 0.0f});
  if (dot3(oriented, outgoing_direction) < 0.0f) {
    oriented = multiply3(oriented, -1.0f);
  }

  constexpr float kNearOrigin = 1.0f / 32.0f;
  constexpr float kNearScale = 1.0f / 65'536.0f;
  constexpr float kUlpScale = 256.0f;
  Vec3 candidate = position;
  for (std::size_t component = 0u; component < candidate.size();
       ++component) {
    if (std::abs(position[component]) < kNearOrigin) {
      candidate[component] =
          position[component] + kNearScale * oriented[component];
      continue;
    }
    const std::int32_t ulps =
        static_cast<std::int32_t>(kUlpScale * oriented[component]);
    const std::uint32_t bits =
        std::bit_cast<std::uint32_t>(position[component]);
    const std::int64_t adjusted =
        static_cast<std::int64_t>(bits) +
        (position[component] < 0.0f ? -static_cast<std::int64_t>(ulps)
                                    : static_cast<std::int64_t>(ulps));
    if (adjusted >= 0 &&
        adjusted <=
            static_cast<std::int64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
      candidate[component] =
          std::bit_cast<float>(static_cast<std::uint32_t>(adjusted));
    }
  }
  const Vec3 movement{candidate[0] - position[0],
                      candidate[1] - position[1],
                      candidate[2] - position[2]};
  if (finite3(candidate) && dot3(movement, oriented) > 0.0f) {
    return candidate;
  }

  const float scene_scale =
      std::max(1.0f, std::max(std::abs(position[0]),
                             std::max(std::abs(position[1]),
                                      std::abs(position[2]))));
  const float fallback_distance =
      std::max(kNearScale,
               scene_scale * std::numeric_limits<float>::epsilon() * 4.0f);
  const Vec3 fallback_candidate =
      add3(position, multiply3(oriented, fallback_distance));
  return finite3(fallback_candidate) ? fallback_candidate : position;
}

PathTraceRayCone initializePathTraceRayCone(
    std::uint32_t render_width, std::uint32_t render_height,
    float vertical_fov_radians) noexcept {
  if (render_width == 0u || render_height == 0u ||
      !std::isfinite(vertical_fov_radians) ||
      !(vertical_fov_radians > 0.0f) ||
      !(vertical_fov_radians < 3.14159265358979323846f)) {
    return {};
  }
  const float projected_pixel_height =
      2.0f * std::tan(vertical_fov_radians * 0.5f) /
      static_cast<float>(render_height);
  const float aspect = static_cast<float>(render_width) /
                       static_cast<float>(render_height);
  const float horizontal_fov =
      2.0f * std::atan(std::tan(vertical_fov_radians * 0.5f) * aspect);
  const float projected_pixel_width =
      2.0f * std::tan(horizontal_fov * 0.5f) /
      static_cast<float>(render_width);
  const float spread =
      std::atan(std::max(projected_pixel_width, projected_pixel_height));
  return {0.0f, std::isfinite(spread) ? spread : 0.0f};
}

float pathTraceRayConeWidthAtDistance(
    const PathTraceRayCone &cone, float distance) noexcept {
  if (!std::isfinite(cone.width) ||
      !std::isfinite(cone.spread_angle) || !std::isfinite(distance)) {
    return 0.0f;
  }
  return std::max(0.0f, cone.width) +
         std::abs(distance) * std::max(0.0f, cone.spread_angle);
}

PathTraceRayCone propagatePathTraceRayCone(
    const PathTraceRayCone &cone, float distance, PathTraceLobe lobe,
    float ggx_alpha, float eta_ratio) noexcept {
  PathTraceRayCone propagated;
  propagated.width = pathTraceRayConeWidthAtDistance(cone, distance);
  propagated.spread_angle =
      std::isfinite(cone.spread_angle)
          ? std::max(0.0f, cone.spread_angle)
          : 0.0f;
  const float alpha =
      std::isfinite(ggx_alpha) ? std::clamp(ggx_alpha, 0.0f, 1.0f) : 1.0f;
  float lobe_spread = 0.0f;
  switch (lobe) {
  case PathTraceLobe::Diffuse:
    lobe_spread = 0.5f;
    break;
  case PathTraceLobe::Glossy:
    lobe_spread = 0.5f * std::sqrt(alpha);
    break;
  case PathTraceLobe::Transmission:
    lobe_spread =
        0.25f * std::sqrt(alpha) +
        0.05f * std::abs((std::isfinite(eta_ratio) ? eta_ratio : 1.0f) -
                         1.0f);
    break;
  case PathTraceLobe::Transparent:
    break;
  }
  propagated.spread_angle =
      std::min(1.57079632679489661923f,
               std::max(propagated.spread_angle, lobe_spread));
  return propagated;
}

float pathTraceRayConeTextureLod(
    const PathTraceRayCone &cone, float distance,
    float triangle_world_double_area, float triangle_uv_double_area,
    std::uint32_t texture_width, std::uint32_t texture_height) noexcept {
  if (texture_width == 0u || texture_height == 0u ||
      !std::isfinite(triangle_world_double_area) ||
      !std::isfinite(triangle_uv_double_area) ||
      !(triangle_world_double_area > 1.0e-20f) ||
      !(triangle_uv_double_area > 0.0f)) {
    return 0.0f;
  }
  const float uv_per_world =
      std::sqrt(triangle_uv_double_area / triangle_world_double_area);
  const float footprint =
      pathTraceRayConeWidthAtDistance(cone, distance) * uv_per_world;
  const float texels =
      footprint * static_cast<float>(std::max(texture_width, texture_height));
  if (!std::isfinite(texels) || !(texels > 1.0f)) {
    return 0.0f;
  }
  return std::max(0.0f, std::log2(texels));
}

float reconstructionMipBias(
    std::uint32_t render_width, std::uint32_t render_height,
    std::uint32_t output_width, std::uint32_t output_height,
    bool reconstruction_enabled) noexcept {
  if (!reconstruction_enabled || render_width == 0u ||
      render_height == 0u || output_width == 0u ||
      output_height == 0u) {
    return 0.0f;
  }

  const double ratio_x = static_cast<double>(render_width) /
                         static_cast<double>(output_width);
  const double ratio_y = static_cast<double>(render_height) /
                         static_cast<double>(output_height);
  // Integer-rounded optimal settings can differ by a fraction of a percent
  // between axes. Prefer the less-negative bias to avoid oversharpening one
  // axis while still correcting the reconstruction-scale footprint.
  const double conservative_ratio = std::max(ratio_x, ratio_y);
  if (!std::isfinite(conservative_ratio) ||
      !(conservative_ratio > 0.0) || conservative_ratio >= 1.0) {
    return 0.0f;
  }
  const double bias = std::log2(conservative_ratio);
  if (!std::isfinite(bias)) {
    return 0.0f;
  }
  return static_cast<float>(std::clamp(bias, -2.0, 0.0));
}

std::array<float, 2>
pathTraceTemporalJitter(std::uint32_t frame_index,
                        std::uint32_t render_width,
                        std::uint32_t output_width) noexcept {
  const auto halton = [](std::uint32_t index,
                         std::uint32_t base) noexcept {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index != 0u) {
      fraction /= static_cast<float>(base);
      result +=
          fraction * static_cast<float>(index % base);
      index /= base;
    }
    return result;
  };
  // Match NVIDIA's Vulkan Streamline sample. DLSS needs more jitter phases as
  // the reconstruction ratio increases; a fixed long sequence converges more
  // slowly and does not repeat at the cadence expected by the selected mode.
  const float scaling_ratio =
      static_cast<float>((std::max)(output_width, 1u)) /
      static_cast<float>((std::max)(render_width, 1u));
  const std::uint32_t jitter_phases =
      (std::max)(1u, static_cast<std::uint32_t>(
                         8.0f * scaling_ratio * scaling_ratio + 0.5f));
  const std::uint32_t index = frame_index % jitter_phases + 1u;
  return {
      halton(index, 2u) - 0.5f,
      halton(index, 3u) - 0.5f,
  };
}

PathTraceHemisphereSample samplePathTraceCosineHemisphere(
    const std::array<float, 3> &normal, float sample_u,
    float sample_v) noexcept {
  PathTraceHemisphereSample result;
  if (!finite3(normal) || !std::isfinite(sample_u) ||
      !std::isfinite(sample_v)) {
    result.used_fallback = true;
    return result;
  }

  const double nx = normal[0];
  const double ny = normal[1];
  const double nz = normal[2];
  const double normal_length =
      std::sqrt(nx * nx + ny * ny + nz * nz);
  if (!std::isfinite(normal_length) || normal_length <= 1.0e-12) {
    result.used_fallback = true;
    return result;
  }
  const std::array<double, 3> n{
      nx / normal_length, ny / normal_length, nz / normal_length};

  const double u = std::clamp(static_cast<double>(sample_u), 0.0,
                              1.0 - 1.0 / 16'777'216.0);
  const double v = std::clamp(static_cast<double>(sample_v), 0.0,
                              1.0 - 1.0 / 16'777'216.0);
  const double radius = std::sqrt(u);
  constexpr double kTau = 6.28318530717958647692;
  const double angle = kTau * v;
  const double local_x = radius * std::cos(angle);
  const double local_y = radius * std::sin(angle);
  const double local_z = std::sqrt(std::max(0.0, 1.0 - u));

  const std::array<double, 3> up =
      std::abs(n[2]) < 0.999
          ? std::array<double, 3>{0.0, 0.0, 1.0}
          : std::array<double, 3>{0.0, 1.0, 0.0};
  std::array<double, 3> tangent{
      up[1] * n[2] - up[2] * n[1],
      up[2] * n[0] - up[0] * n[2],
      up[0] * n[1] - up[1] * n[0]};
  const double tangent_length = std::sqrt(
      tangent[0] * tangent[0] + tangent[1] * tangent[1] +
      tangent[2] * tangent[2]);
  if (!std::isfinite(tangent_length) || tangent_length <= 1.0e-12) {
    result.used_fallback = true;
    return result;
  }
  tangent[0] /= tangent_length;
  tangent[1] /= tangent_length;
  tangent[2] /= tangent_length;
  const std::array<double, 3> bitangent{
      n[1] * tangent[2] - n[2] * tangent[1],
      n[2] * tangent[0] - n[0] * tangent[2],
      n[0] * tangent[1] - n[1] * tangent[0]};
  std::array<double, 3> direction{
      tangent[0] * local_x + bitangent[0] * local_y + n[0] * local_z,
      tangent[1] * local_x + bitangent[1] * local_y + n[1] * local_z,
      tangent[2] * local_x + bitangent[2] * local_y + n[2] * local_z};
  const double direction_length =
      std::sqrt(direction[0] * direction[0] +
                direction[1] * direction[1] +
                direction[2] * direction[2]);
  if (!std::isfinite(direction_length) ||
      direction_length <= 1.0e-12) {
    result.used_fallback = true;
    return result;
  }
  result.direction = {
      static_cast<float>(direction[0] / direction_length),
      static_cast<float>(direction[1] / direction_length),
      static_cast<float>(direction[2] / direction_length)};
  if (!finite3(result.direction)) {
    result.direction = {0.0f, 1.0f, 0.0f};
    result.used_fallback = true;
  }
  return result;
}

bool pathTraceBounceAllowed(const PathTraceSettings &settings,
                            const PathTraceDepthState &state,
                            PathTraceLobe lobe) noexcept {
  const PathTraceSettings normalized = normalizePathTraceSettings(settings);
  if (state.total >= normalized.max_bounces) {
    return false;
  }
  switch (lobe) {
  case PathTraceLobe::Diffuse:
    return state.diffuse < normalized.max_diffuse_bounces;
  case PathTraceLobe::Glossy:
    return state.glossy < normalized.max_glossy_bounces;
  case PathTraceLobe::Transmission:
    return state.transmission < normalized.max_transmission_bounces;
  case PathTraceLobe::Transparent:
    return state.transparent < normalized.max_transparent_bounces;
  default:
    return false;
  }
}

std::optional<PathTraceDepthState> advancePathTraceDepth(
    const PathTraceSettings &settings, const PathTraceDepthState &state,
    PathTraceLobe lobe) noexcept {
  if (!pathTraceBounceAllowed(settings, state, lobe)) {
    return std::nullopt;
  }
  PathTraceDepthState next = state;
  ++next.total;
  switch (lobe) {
  case PathTraceLobe::Diffuse:
    ++next.diffuse;
    break;
  case PathTraceLobe::Glossy:
    ++next.glossy;
    break;
  case PathTraceLobe::Transmission:
    ++next.transmission;
    break;
  case PathTraceLobe::Transparent:
    ++next.transparent;
    break;
  }
  return next;
}

PathTraceRussianRouletteStep evaluatePathTraceRussianRoulette(
    const PathTraceSettings &settings, const PathTraceDepthState &state,
    float throughput_max, float sample_u) noexcept {
  const PathTraceSettings normalized = normalizePathTraceSettings(settings);
  PathTraceRussianRouletteStep result;
  if (!normalized.russian_roulette ||
      state.total < normalized.russian_roulette_start) {
    return result;
  }
  result.applied = true;
  if (!std::isfinite(throughput_max) || !(throughput_max > 0.0f) ||
      !std::isfinite(sample_u)) {
    result.continuation_probability = 0.0f;
    result.throughput_scale = 0.0f;
    result.survives = false;
    return result;
  }
  result.continuation_probability =
      std::clamp(throughput_max, 0.05f, 0.95f);
  result.survives =
      std::clamp(sample_u, 0.0f, 1.0f) <
      result.continuation_probability;
  result.throughput_scale =
      result.survives ? 1.0f / result.continuation_probability : 0.0f;
  return result;
}

RtSurfaceOptics normalizeRtSurfaceOptics(RtSurfaceOptics optics) noexcept {
  optics.transmission =
      std::isfinite(optics.transmission)
          ? std::clamp(optics.transmission, 0.0f, 1.0f)
          : 0.0f;
  optics.ior = std::isfinite(optics.ior)
                   ? std::clamp(optics.ior, 1.0001f, 99.0f)
                   : 1.5f;
  for (float &component : optics.attenuation_color) {
    component = std::isfinite(component)
                    ? std::clamp(component, 1.0e-6f, 1.0f)
                    : 1.0f;
  }
  optics.attenuation_distance =
      std::isfinite(optics.attenuation_distance) &&
              optics.attenuation_distance > 0.0f
          ? optics.attenuation_distance
          : 0.0f;
  return optics;
}

std::array<float, 3> rtBeerLambertTransmittance(
    const RtSurfaceOptics &input, float traveled_distance) noexcept {
  const RtSurfaceOptics optics = normalizeRtSurfaceOptics(input);
  std::array<float, 3> result{1.0f, 1.0f, 1.0f};
  if (optics.thin_walled || !(optics.attenuation_distance > 0.0f) ||
      !std::isfinite(traveled_distance) || !(traveled_distance > 0.0f)) {
    return result;
  }
  const double normalized_distance =
      static_cast<double>(traveled_distance) /
      static_cast<double>(optics.attenuation_distance);
  for (std::size_t channel = 0u; channel < result.size(); ++channel) {
    const double value = std::pow(
        static_cast<double>(optics.attenuation_color[channel]),
        normalized_distance);
    result[channel] = std::isfinite(value)
                          ? static_cast<float>(std::clamp(value, 0.0, 1.0))
                          : 0.0f;
  }
  return result;
}

float rtDielectricF0FromIor(float ior) noexcept {
  if (!std::isfinite(ior)) {
    return 0.04f;
  }
  const float normalized_ior = std::clamp(ior, 1.0001f, 99.0f);
  const float ratio =
      (normalized_ior - 1.0f) / (normalized_ior + 1.0f);
  return std::clamp(ratio * ratio, 0.0f, 0.99f);
}

float rtDielectricIorFromF0(float f0) noexcept {
  if (!std::isfinite(f0)) {
    return 1.5f;
  }
  const float root = std::sqrt(std::clamp(f0, 0.0f, 0.9604f));
  return std::clamp((1.0f + root) / std::max(1.0f - root, 0.02f),
                    1.0001f, 99.0f);
}

float rtFresnelDielectric(float cosine_incident, float eta_incident,
                          float eta_transmitted) noexcept {
  if (!std::isfinite(cosine_incident) ||
      !std::isfinite(eta_incident) ||
      !std::isfinite(eta_transmitted) ||
      !(eta_incident > 0.0f) || !(eta_transmitted > 0.0f)) {
    return 1.0f;
  }
  const double cos_i =
      std::clamp(std::abs(static_cast<double>(cosine_incident)), 0.0, 1.0);
  const double eta_i = eta_incident;
  const double eta_t = eta_transmitted;
  const double sin_t =
      eta_i / eta_t * std::sqrt(std::max(0.0, 1.0 - cos_i * cos_i));
  if (sin_t >= 1.0) {
    return 1.0f;
  }
  const double cos_t = std::sqrt(std::max(0.0, 1.0 - sin_t * sin_t));
  const double s_denominator = eta_t * cos_i + eta_i * cos_t;
  const double p_denominator = eta_i * cos_i + eta_t * cos_t;
  if (s_denominator <= 1.0e-12 || p_denominator <= 1.0e-12) {
    return 1.0f;
  }
  const double s =
      (eta_t * cos_i - eta_i * cos_t) / s_denominator;
  const double p =
      (eta_i * cos_i - eta_t * cos_t) / p_denominator;
  return static_cast<float>(
      std::clamp(0.5 * (s * s + p * p), 0.0, 1.0));
}

std::array<float, 3>
rtFresnelSchlick(const std::array<float, 3> &f0,
                 float cosine) noexcept {
  const float c =
      std::isfinite(cosine) ? std::clamp(cosine, 0.0f, 1.0f) : 0.0f;
  const float one_minus = 1.0f - c;
  const float factor =
      one_minus * one_minus * one_minus * one_minus * one_minus;
  Vec3 result{};
  for (std::size_t i = 0; i < result.size(); ++i) {
    const float base =
        std::isfinite(f0[i]) ? std::clamp(f0[i], 0.0f, 1.0f) : 0.0f;
    result[i] = base + (1.0f - base) * factor;
  }
  return result;
}

float rtRrRoughnessFromGgxAlpha(float ggx_alpha) noexcept {
  if (!std::isfinite(ggx_alpha)) {
    return 1.0f;
  }
  return std::sqrt(std::clamp(ggx_alpha, 0.0f, 1.0f));
}

float rtGgxDistribution(float normal_dot_half, float ggx_alpha) noexcept {
  if (!std::isfinite(normal_dot_half) || !std::isfinite(ggx_alpha) ||
      ggx_alpha <= kDeltaMirrorAlpha) {
    return 0.0f;
  }
  const double n_dot_h =
      std::clamp(static_cast<double>(normal_dot_half), 0.0, 1.0);
  if (!(n_dot_h > 0.0)) {
    return 0.0f;
  }
  const double alpha = std::clamp(static_cast<double>(ggx_alpha),
                                  static_cast<double>(kMinFiniteGgxAlpha),
                                  1.0);
  const double alpha_squared = alpha * alpha;
  const double denominator =
      n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0;
  constexpr double kPi = 3.14159265358979323846;
  const double value =
      alpha_squared / (kPi * denominator * denominator);
  return std::isfinite(value) ? static_cast<float>(value) : 0.0f;
}

float rtSmithGgxG1(float normal_dot_direction, float ggx_alpha) noexcept {
  if (!std::isfinite(normal_dot_direction) ||
      !std::isfinite(ggx_alpha) || ggx_alpha <= kDeltaMirrorAlpha) {
    return 0.0f;
  }
  const double cosine =
      std::clamp(static_cast<double>(normal_dot_direction), 0.0, 1.0);
  if (!(cosine > 0.0)) {
    return 0.0f;
  }
  const double alpha = std::clamp(static_cast<double>(ggx_alpha),
                                  static_cast<double>(kMinFiniteGgxAlpha),
                                  1.0);
  const double root =
      std::sqrt(alpha * alpha +
                (1.0 - alpha * alpha) * cosine * cosine);
  const double value = 2.0 * cosine / (cosine + root);
  return std::isfinite(value) ? static_cast<float>(value) : 0.0f;
}

float rtGgxVisibleNormalPdf(
    const Vec3 &shading_normal, const Vec3 &view_direction,
    const Vec3 &half_vector, float ggx_alpha) noexcept {
  if (!std::isfinite(ggx_alpha) || ggx_alpha <= kDeltaMirrorAlpha) {
    return 0.0f;
  }
  const Vec3 normal =
      normalize3(shading_normal, {0.0f, 1.0f, 0.0f});
  const Vec3 view = normalize3(view_direction, {});
  const Vec3 half = normalize3(half_vector, {});
  const float normal_dot_view = dot3(normal, view);
  const float normal_dot_half = dot3(normal, half);
  const float view_dot_half = dot3(view, half);
  if (!finite3(view) || !finite3(half) || !(normal_dot_view > 0.0f) ||
      !(normal_dot_half > 0.0f) || !(view_dot_half > 0.0f)) {
    return 0.0f;
  }
  const double pdf =
      static_cast<double>(rtGgxDistribution(normal_dot_half, ggx_alpha)) *
      static_cast<double>(rtSmithGgxG1(normal_dot_view, ggx_alpha)) *
      static_cast<double>(view_dot_half) /
      static_cast<double>(normal_dot_view);
  return std::isfinite(pdf) && pdf > 0.0
             ? static_cast<float>(pdf)
             : 0.0f;
}

RtGgxVisibleNormalSample sampleRtGgxVndf(
    const Vec3 &shading_normal, const Vec3 &view_direction,
    float ggx_alpha, float sample_u, float sample_v) noexcept {
  RtGgxVisibleNormalSample result;
  if (!std::isfinite(ggx_alpha) ||
      ggx_alpha <= kDeltaMirrorAlpha || !std::isfinite(sample_u) ||
      !std::isfinite(sample_v)) {
    return result;
  }
  const Vec3 normal =
      normalize3(shading_normal, {0.0f, 1.0f, 0.0f});
  const Vec3 view = normalize3(view_direction, {});
  if (!finite3(view) || !(dot3(normal, view) > 0.0f)) {
    return result;
  }
  const Vec3 up = std::abs(normal[2]) < 0.999f
                      ? Vec3{0.0f, 0.0f, 1.0f}
                      : Vec3{0.0f, 1.0f, 0.0f};
  const Vec3 tangent =
      normalize3(cross3(up, normal), {1.0f, 0.0f, 0.0f});
  const Vec3 bitangent = cross3(normal, tangent);
  const Vec3 local_view{dot3(view, tangent), dot3(view, bitangent),
                        dot3(view, normal)};
  const float alpha =
      std::clamp(ggx_alpha, kMinFiniteGgxAlpha, 1.0f);
  const Vec3 stretched_view = normalize3(
      {alpha * local_view[0], alpha * local_view[1], local_view[2]},
      {0.0f, 0.0f, 1.0f});
  const float lensq = stretched_view[0] * stretched_view[0] +
                      stretched_view[1] * stretched_view[1];
  const Vec3 basis_x =
      lensq > 1.0e-20f
          ? Vec3{-stretched_view[1] / std::sqrt(lensq),
                 stretched_view[0] / std::sqrt(lensq), 0.0f}
          : Vec3{1.0f, 0.0f, 0.0f};
  const Vec3 basis_y = cross3(stretched_view, basis_x);
  const float u = std::clamp(sample_u, 0.0f,
                             1.0f - 1.0f / 16'777'216.0f);
  const float v = std::clamp(sample_v, 0.0f,
                             1.0f - 1.0f / 16'777'216.0f);
  const float radius = std::sqrt(u);
  const float phi = 6.28318530717958647692f * v;
  const float disk_x = radius * std::cos(phi);
  float disk_y = radius * std::sin(phi);
  const float blend = 0.5f * (1.0f + stretched_view[2]);
  disk_y = (1.0f - blend) *
               std::sqrt(std::max(0.0f, 1.0f - disk_x * disk_x)) +
           blend * disk_y;
  const float disk_z = std::sqrt(
      std::max(0.0f, 1.0f - disk_x * disk_x - disk_y * disk_y));
  const Vec3 stretched_normal =
      add3(add3(multiply3(basis_x, disk_x),
                multiply3(basis_y, disk_y)),
           multiply3(stretched_view, disk_z));
  const Vec3 local_half = normalize3(
      {alpha * stretched_normal[0], alpha * stretched_normal[1],
       std::max(stretched_normal[2], 0.0f)},
      {0.0f, 0.0f, 1.0f});
  result.half_vector = normalize3(
      add3(add3(multiply3(tangent, local_half[0]),
                multiply3(bitangent, local_half[1])),
           multiply3(normal, local_half[2])),
      normal);
  result.pdf = rtGgxVisibleNormalPdf(
      normal, view, result.half_vector, ggx_alpha);
  result.valid = result.pdf > 0.0f && finite3(result.half_vector);
  if (!result.valid) {
    result = {};
  }
  return result;
}

RtBsdfLobeProbabilities
rtBsdfLobeProbabilities(const RtBsdfMaterial &input) noexcept {
  const RtBsdfMaterial material = normalizeBsdfMaterial(input);
  const float normal_fresnel =
      material.metal
          ? std::clamp(max3(material.f0), 0.0f, 0.99f)
          : rtFresnelDielectric(1.0f, 1.0f, material.ior);
  const float diffuse_importance =
      material.metal
          ? 0.0f
          : (1.0f - material.transmission) *
                (1.0f - normal_fresnel) *
                std::max(luminance3(material.base_color), 0.05f);
  const float interface_importance =
      material.metal
          ? std::max(normal_fresnel, 0.02f)
          : normal_fresnel +
                material.transmission * (1.0f - normal_fresnel);
  const float total = diffuse_importance + interface_importance;
  if (!std::isfinite(total) || !(total > 1.0e-8f)) {
    return {0.0f, 1.0f, 0.0f};
  }
  const float diffuse_probability = diffuse_importance / total;
  const float interface_probability = interface_importance / total;
  const float reflection_weight = material.metal ? 1.0f : normal_fresnel;
  const float transmission_weight =
      material.metal
          ? 0.0f
          : material.transmission * (1.0f - normal_fresnel);
  const float interface_total = reflection_weight + transmission_weight;
  const float reflection_probability =
      interface_total > 0.0f
          ? interface_probability * reflection_weight / interface_total
          : interface_probability;
  return {diffuse_probability, reflection_probability,
          std::max(0.0f,
                   interface_probability - reflection_probability)};
}

RtSubsurfaceLobeSplit rtSubsurfaceLobeSplit(
    float legacy_diffuse_probability, float subsurface,
    bool eligible) noexcept {
  const float diffuse = std::isfinite(legacy_diffuse_probability)
                            ? std::clamp(legacy_diffuse_probability, 0.0f, 1.0f)
                            : 0.0f;
  if (!eligible || !std::isfinite(subsurface) || !(subsurface > 0.0f)) {
    return {diffuse, 0.0f};
  }
  const float split = std::clamp(subsurface, 0.0f, 1.0f);
  return {diffuse * (1.0f - split), diffuse * split};
}

float rtSubsurfaceOpticalDepth(float subsurface) noexcept {
  const float split = std::isfinite(subsurface)
                          ? std::clamp(subsurface, 0.0f, 1.0f)
                          : 0.0f;
  return 0.35f + 1.65f * split;
}

float rtSubsurfaceFreeFlightFraction(float subsurface,
                                     float sample_u) noexcept {
  constexpr float kLargestRandomBelowOne = 0.99999994f;
  const float sample = std::isfinite(sample_u)
                           ? std::clamp(sample_u, 0.0f,
                                        kLargestRandomBelowOne)
                           : 0.0f;
  const float survival = std::max(1.0f - sample, 1.0e-7f);
  return -std::log(survival) / rtSubsurfaceOpticalDepth(subsurface);
}

namespace {

[[nodiscard]] float thinWalledCombinedFresnel(float fresnel) noexcept {
  const float clamped = std::clamp(fresnel, 0.0f, 1.0f);
  return std::clamp(2.0f * clamped / (1.0f + clamped), 0.0f, 1.0f);
}

[[nodiscard]] float evaluateRtMicrofacetTransmission(
    const RtBsdfMaterial &material, const Vec3 &normal, const Vec3 &view,
    const Vec3 &light, const Vec3 &half, bool front_face) noexcept {
  const float normal_dot_view = dot3(normal, view);
  const float normal_dot_light = dot3(normal, light);
  const float view_dot_half = dot3(view, half);
  const float light_dot_half = dot3(light, half);
  if (!(normal_dot_view > 0.0f) || !(normal_dot_light < 0.0f) ||
      !(view_dot_half > 0.0f) || !(light_dot_half < 0.0f) ||
      material.metal || material.thin_walled ||
      !(material.transmission > 0.0f) ||
      material.ggx_alpha <= kDeltaMirrorAlpha) {
    return 0.0f;
  }
  const float eta_incident = front_face ? 1.0f : material.ior;
  const float eta_transmitted = front_face ? material.ior : 1.0f;
  const float eta_path = eta_transmitted / eta_incident;
  const float denominator =
      light_dot_half + view_dot_half / eta_path;
  const double denominator_squared =
      static_cast<double>(denominator) * denominator;
  if (!(denominator_squared > 1.0e-16)) {
    return 0.0f;
  }
  const float distribution =
      rtGgxDistribution(dot3(normal, half), material.ggx_alpha);
  const float geometry =
      rtSmithGgxG1(normal_dot_view, material.ggx_alpha) *
      rtSmithGgxG1(-normal_dot_light, material.ggx_alpha);
  const float fresnel = rtFresnelDielectric(
      view_dot_half, eta_incident, eta_transmitted);
  const double geometric_denominator =
      denominator_squared * static_cast<double>(normal_dot_view) *
      static_cast<double>(normal_dot_light);
  if (geometric_denominator == 0.0) {
    return 0.0f;
  }
  const double eta_scale =
      1.0 / (static_cast<double>(eta_path) * eta_path);
  const double value =
      static_cast<double>(material.transmission) * (1.0 - fresnel) *
      distribution * geometry *
      std::abs(static_cast<double>(light_dot_half) * view_dot_half /
               geometric_denominator) *
      eta_scale;
  return std::isfinite(value) && value >= 0.0
             ? static_cast<float>(value)
             : 0.0f;
}

} // namespace

RtBsdfEval evaluateRtBsdf(
    const RtBsdfMaterial &input, const Vec3 &shading_normal,
    const Vec3 &view_direction, const Vec3 &light_direction,
    bool front_face) noexcept {
  RtBsdfEval result;
  const RtBsdfMaterial material = normalizeBsdfMaterial(input);
  const Vec3 normal =
      normalize3(shading_normal, {0.0f, 1.0f, 0.0f});
  const Vec3 view = normalize3(view_direction, {});
  const Vec3 light = normalize3(light_direction, {});
  const float n_dot_v = dot3(normal, view);
  const float n_dot_l = dot3(normal, light);
  if (!finite3(view) || !finite3(light) || !(n_dot_v > 0.0f) ||
      n_dot_l == 0.0f) {
    return result;
  }
  const bool delta_mirror = material.ggx_alpha <= kDeltaMirrorAlpha;
  // The minimal Thin-Walled model merges both interfaces into one stable
  // straight-through/reflection atom. Keep that atom out of the continuous
  // GGX density even when the source material carries finite roughness.
  const bool delta_interface =
      delta_mirror ||
      (!material.metal && material.thin_walled &&
       material.transmission > 0.0f);
  const RtBsdfLobeProbabilities probabilities =
      rtBsdfLobeProbabilities(material);
  const float interface_probability =
      probabilities.glossy + probabilities.transmission;
  constexpr float kInversePi = 0.31830988618379067154f;
  const float eta_incident = front_face ? 1.0f : material.ior;
  const float eta_transmitted = front_face ? material.ior : 1.0f;

  if (n_dot_l > 0.0f) {
    const Vec3 half_vector = normalize3(add3(view, light), {});
    const float n_dot_h = dot3(normal, half_vector);
    const float v_dot_h = dot3(view, half_vector);
    if (!(n_dot_h > 0.0f) || !(v_dot_h > 0.0f)) {
      return result;
    }
    Vec3 fresnel{};
    float scalar_fresnel = 0.0f;
    if (material.metal) {
      fresnel = rtFresnelSchlick(material.f0, v_dot_h);
      scalar_fresnel = max3(fresnel);
    } else {
      scalar_fresnel = rtFresnelDielectric(
          v_dot_h, eta_incident, eta_transmitted);
      fresnel.fill(scalar_fresnel);
    }
    const float distribution =
        delta_interface
            ? 0.0f
            : rtGgxDistribution(n_dot_h, material.ggx_alpha);
    const float geometry =
        delta_interface
            ? 0.0f
            : rtSmithGgxG1(n_dot_v, material.ggx_alpha) *
                  rtSmithGgxG1(n_dot_l, material.ggx_alpha);
    const float specular_scale =
        distribution * geometry /
        std::max(4.0f * n_dot_v * n_dot_l, 1.0e-8f);
    const float diffuse_scale =
        material.metal
            ? 0.0f
            : (1.0f - material.transmission) *
                  (1.0f - scalar_fresnel) * kInversePi;
    for (std::size_t channel = 0u; channel < result.value.size(); ++channel) {
      result.value[channel] =
          material.base_color[channel] * diffuse_scale +
          fresnel[channel] * specular_scale;
    }
    const float diffuse_pdf = n_dot_l * kInversePi;
    float reflection_pdf = 0.0f;
    if (!delta_interface && interface_probability > 0.0f) {
      const float reflection_weight =
          material.metal ? 1.0f : scalar_fresnel;
      const float transmission_weight =
          material.metal
              ? 0.0f
              : material.transmission * (1.0f - scalar_fresnel);
      const float interface_total =
          reflection_weight + transmission_weight;
      const float conditional_reflection =
          interface_total > 0.0f
              ? reflection_weight / interface_total
              : 1.0f;
      reflection_pdf = interface_probability * conditional_reflection *
                       rtGgxVisibleNormalPdf(
                           normal, view, half_vector, material.ggx_alpha) /
                       std::max(4.0f * std::abs(v_dot_h), 1.0e-8f);
    }
    result.pdf = probabilities.diffuse * diffuse_pdf + reflection_pdf;
  } else {
    if (material.metal || material.thin_walled ||
        !(material.transmission > 0.0f) || delta_mirror ||
        !(interface_probability > 0.0f)) {
      return result;
    }
    const float eta_path = eta_transmitted / eta_incident;
    Vec3 half_vector = normalize3(
        add3(multiply3(light, eta_path), view), {});
    if (dot3(half_vector, normal) < 0.0f) {
      half_vector = multiply3(half_vector, -1.0f);
    }
    const float n_dot_h = dot3(normal, half_vector);
    const float v_dot_h = dot3(view, half_vector);
    const float l_dot_h = dot3(light, half_vector);
    if (!(n_dot_h > 0.0f) || !(v_dot_h > 0.0f) ||
        !(l_dot_h < 0.0f)) {
      return result;
    }
    const float fresnel = rtFresnelDielectric(
        v_dot_h, eta_incident, eta_transmitted);
    const float transmission_weight =
        material.transmission * (1.0f - fresnel);
    const float interface_total = fresnel + transmission_weight;
    if (!(interface_total > 0.0f)) {
      return result;
    }
    const float transmission_value = evaluateRtMicrofacetTransmission(
        material, normal, view, light, half_vector, front_face);
    result.value.fill(transmission_value);
    const float denominator = l_dot_h + v_dot_h / eta_path;
    const float dwm_dwi =
        std::abs(l_dot_h) /
        std::max(denominator * denominator, 1.0e-8f);
    result.pdf =
        interface_probability * transmission_weight / interface_total *
        rtGgxVisibleNormalPdf(normal, view, half_vector,
                              material.ggx_alpha) *
        dwm_dwi;
  }
  result.valid = finite3(result.value) && std::isfinite(result.pdf) &&
                  result.pdf > 0.0f;
  if (!result.valid) {
    result = {};
  }
  return result;
}

RtBsdfSample sampleRtBsdf(
    const RtBsdfMaterial &input, const Vec3 &shading_normal,
    const Vec3 &view_direction, bool front_face, float lobe_sample,
    float direction_sample_u, float direction_sample_v) noexcept {
  RtBsdfSample result;
  const RtBsdfMaterial material = normalizeBsdfMaterial(input);
  if (!std::isfinite(lobe_sample) ||
      !std::isfinite(direction_sample_u) ||
      !std::isfinite(direction_sample_v)) {
    return result;
  }
  const Vec3 normal =
      normalize3(shading_normal, {0.0f, 1.0f, 0.0f});
  const Vec3 view = normalize3(view_direction, {});
  const float n_dot_v = dot3(normal, view);
  if (!(n_dot_v > 0.0f)) {
    return result;
  }
  const float lobe_u =
      std::clamp(lobe_sample, 0.0f, 1.0f - 1.0f / 16'777'216.0f);
  const float sample_u =
      std::clamp(direction_sample_u, 0.0f,
                 1.0f - 1.0f / 16'777'216.0f);
  const float sample_v =
      std::clamp(direction_sample_v, 0.0f,
                 1.0f - 1.0f / 16'777'216.0f);
  const RtBsdfLobeProbabilities probabilities =
      rtBsdfLobeProbabilities(material);
  const float interface_probability =
      probabilities.glossy + probabilities.transmission;
  const float eta_incident = front_face ? 1.0f : material.ior;
  const float eta_transmitted = front_face ? material.ior : 1.0f;
  const float eta = eta_incident / eta_transmitted;
  if (lobe_u < probabilities.diffuse) {
    const PathTraceHemisphereSample sampled =
        samplePathTraceCosineHemisphere(normal, sample_u, sample_v);
    result.direction = sampled.direction;
    result.lobe = PathTraceLobe::Diffuse;
  } else {
    if (!(interface_probability > 0.0f)) {
      return result;
    }
    const float interface_u = std::clamp(
        (lobe_u - probabilities.diffuse) / interface_probability,
        0.0f, 1.0f - 1.0f / 16'777'216.0f);
    const bool thin_sheet =
        material.thin_walled && material.transmission > 0.0f;
    if (thin_sheet) {
      const float single_fresnel = rtFresnelDielectric(
          n_dot_v, eta_incident, eta_transmitted);
      const float fresnel = thinWalledCombinedFresnel(single_fresnel);
      const float reflection_weight = fresnel;
      const float transmission_weight =
          material.transmission * (1.0f - fresnel);
      const float interface_total =
          reflection_weight + transmission_weight;
      if (!(interface_total > 0.0f)) {
        return result;
      }
      const float conditional_reflection =
          reflection_weight / interface_total;
      const bool reflect_event = interface_u < conditional_reflection;
      const float selection_probability =
          interface_probability *
          (reflect_event ? conditional_reflection
                         : 1.0f - conditional_reflection);
      if (!(selection_probability > 0.0f)) {
        return result;
      }
      result.direction = reflect_event
                             ? normalize3(
                                   add3(multiply3(normal, 2.0f * n_dot_v),
                                        multiply3(view, -1.0f)),
                                   {})
                             : multiply3(view, -1.0f);
      result.weight.fill(
          (reflect_event ? reflection_weight : transmission_weight) /
          selection_probability);
      result.pdf = selection_probability;
      result.lobe = reflect_event ? PathTraceLobe::Glossy
                                  : PathTraceLobe::Transmission;
      result.delta = true;
      result.valid = finite3(result.direction) && finite3(result.weight) &&
                     std::isfinite(result.pdf);
      if (!result.valid) {
        result = {};
      }
      return result;
    }

    Vec3 half_vector = normal;
    if (material.ggx_alpha > kDeltaMirrorAlpha) {
      const RtGgxVisibleNormalSample sampled_half = sampleRtGgxVndf(
          normal, view, material.ggx_alpha, sample_u, sample_v);
      if (!sampled_half.valid) {
        return result;
      }
      half_vector = sampled_half.half_vector;
    }
    const float view_dot_half = dot3(view, half_vector);
    if (!(view_dot_half > 0.0f)) {
      return result;
    }
    const float sin_t_squared =
        eta * eta * std::max(0.0f, 1.0f - view_dot_half * view_dot_half);
    const bool can_refract = sin_t_squared < 1.0f;
    float scalar_fresnel = 1.0f;
    Vec3 fresnel{};
    if (material.metal) {
      fresnel = rtFresnelSchlick(material.f0, view_dot_half);
      scalar_fresnel = max3(fresnel);
    } else if (can_refract) {
      scalar_fresnel = rtFresnelDielectric(
          view_dot_half, eta_incident, eta_transmitted);
      fresnel.fill(scalar_fresnel);
    } else {
      fresnel.fill(1.0f);
    }
    const float reflection_weight = material.metal ? 1.0f : scalar_fresnel;
    const float transmission_weight =
        material.metal || !can_refract
            ? 0.0f
            : material.transmission * (1.0f - scalar_fresnel);
    const float interface_total = reflection_weight + transmission_weight;
    if (!(interface_total > 0.0f)) {
      return result;
    }
    const float conditional_reflection =
        reflection_weight / interface_total;
    const bool reflect_event = interface_u < conditional_reflection;
    if (material.ggx_alpha <= kDeltaMirrorAlpha) {
      const float selection_probability =
          interface_probability *
          (reflect_event ? conditional_reflection
                         : 1.0f - conditional_reflection);
      if (!(selection_probability > 0.0f)) {
        return result;
      }
      result.delta = true;
      result.pdf = selection_probability;
      if (reflect_event) {
        result.direction = normalize3(
            add3(multiply3(half_vector, 2.0f * view_dot_half),
                 multiply3(view, -1.0f)),
            {});
        for (std::size_t channel = 0u; channel < result.weight.size();
             ++channel) {
          result.weight[channel] =
              fresnel[channel] / selection_probability;
        }
        result.lobe = PathTraceLobe::Glossy;
        result.total_internal_reflection =
            !can_refract && material.transmission > 0.0f;
      } else {
        const float cosine_transmitted =
            std::sqrt(std::max(0.0f, 1.0f - sin_t_squared));
        result.direction = normalize3(
            add3(multiply3(view, -eta),
                 multiply3(half_vector,
                           eta * view_dot_half - cosine_transmitted)),
            {});
        const float eta_scale = eta * eta;
        result.weight.fill(transmission_weight * eta_scale /
                           selection_probability);
        result.lobe = PathTraceLobe::Transmission;
      }
      result.valid = finite3(result.direction) && finite3(result.weight) &&
                     std::isfinite(result.pdf);
      if (!result.valid) {
        result = {};
      }
      return result;
    }

    if (reflect_event) {
      result.direction = normalize3(
          add3(multiply3(half_vector, 2.0f * view_dot_half),
               multiply3(view, -1.0f)),
          {});
      if (!(dot3(normal, result.direction) > 0.0f)) {
        return {};
      }
      result.lobe = PathTraceLobe::Glossy;
      result.total_internal_reflection =
          !can_refract && material.transmission > 0.0f;
    } else {
      const float cosine_transmitted =
          std::sqrt(std::max(0.0f, 1.0f - sin_t_squared));
      result.direction = normalize3(
          add3(multiply3(view, -eta),
               multiply3(half_vector,
                         eta * view_dot_half - cosine_transmitted)),
          {});
      if (!(dot3(normal, result.direction) < 0.0f)) {
        return {};
      }
      result.lobe = PathTraceLobe::Transmission;
    }
  }

  const RtBsdfEval evaluated =
      evaluateRtBsdf(material, normal, view, result.direction, front_face);
  if (!evaluated.valid) {
    return {};
  }
  result.value = evaluated.value;
  result.pdf = evaluated.pdf;
  const float cosine = std::abs(dot3(normal, result.direction));
  for (std::size_t i = 0; i < result.weight.size(); ++i) {
    result.weight[i] =
        evaluated.value[i] * cosine / evaluated.pdf;
  }
  result.valid = finite3(result.direction) && finite3(result.value) &&
                 finite3(result.weight);
  if (!result.valid) {
    result = {};
  }
  return result;
}

float rtShadingNormalCorrection(
    const Vec3 &geometric_normal, const Vec3 &shading_normal,
    const Vec3 &view_direction, const Vec3 &light_direction) noexcept {
  const Vec3 geometric =
      normalize3(geometric_normal, {0.0f, 1.0f, 0.0f});
  const Vec3 shading = normalize3(shading_normal, geometric);
  const Vec3 view = normalize3(view_direction, {});
  const Vec3 light = normalize3(light_direction, {});
  const float view_geometric = dot3(view, geometric);
  const float view_shading = dot3(view, shading);
  const float light_geometric = dot3(light, geometric);
  const float light_shading = dot3(light, shading);
  if (!finite3(view) || !finite3(light) ||
      view_geometric * view_shading <= 0.0f ||
      light_geometric * light_shading <= 0.0f ||
      std::abs(light_geometric * view_shading) <= 1.0e-8f) {
    return 0.0f;
  }
  const float correction =
      std::abs((light_shading * view_geometric) /
               (light_geometric * view_shading));
  return std::isfinite(correction)
             ? std::clamp(correction, 0.0f,
                          kPathTraceShadingNormalCorrectionLimit)
             : 0.0f;
}

PathTraceAccumulationStep advancePathTraceAccumulation(
    const PathTraceAccumulationRequest &request) noexcept {
  const PathTraceSettings settings =
      normalizePathTraceSettings(request.settings);
  PathTraceAccumulationStep step;
  step.history_key = request.history_key;
  step.history_reset =
      !request.history_valid ||
      request.previous_history_key != request.history_key;
  step.sample_base =
      step.history_reset ? 0u : request.accumulated_samples;

  std::uint32_t available = (std::numeric_limits<std::uint32_t>::max)() -
                            step.sample_base;
  if (settings.maximum_samples != 0u) {
    if (step.sample_base >= settings.maximum_samples) {
      available = 0u;
    } else {
      available = std::min(
          available, settings.maximum_samples - step.sample_base);
    }
  }
  step.dispatch_samples =
      std::min(settings.samples_per_frame, available);
  step.accumulated_samples_after_dispatch =
      step.sample_base + step.dispatch_samples;
  step.maximum_reached =
      settings.maximum_samples != 0u &&
      step.accumulated_samples_after_dispatch >= settings.maximum_samples;
  return step;
}

RtDebugView rtDebugViewFromName(std::string_view name) noexcept {
  if (name == "instance") {
    return RtDebugView::Instance;
  }
  if (name == "primitive") {
    return RtDebugView::Primitive;
  }
  if (name == "cube") {
    return RtDebugView::Cube;
  }
  if (name == "face") {
    return RtDebugView::Face;
  }
  if (name == "material") {
    return RtDebugView::Material;
  }
  if (name == "normal") {
    return RtDebugView::Normal;
  }
  if (name == "albedo") {
    return RtDebugView::Albedo;
  }
  if (name == "roughness") {
    return RtDebugView::Roughness;
  }
  if (name == "emission") {
    return RtDebugView::Emission;
  }
  return RtDebugView::Off;
}

const char *rtDebugViewName(RtDebugView view) noexcept {
  switch (view) {
  case RtDebugView::Instance:
    return "instance";
  case RtDebugView::Primitive:
    return "primitive";
  case RtDebugView::Cube:
    return "cube";
  case RtDebugView::Face:
    return "face";
  case RtDebugView::Material:
    return "material";
  case RtDebugView::Normal:
    return "normal";
  case RtDebugView::Albedo:
    return "albedo";
  case RtDebugView::Roughness:
    return "roughness";
  case RtDebugView::Emission:
    return "emission";
  case RtDebugView::DirectLighting:
    return "direct-lighting";
  case RtDebugView::IndirectLighting:
    return "indirect-lighting";
  case RtDebugView::SampleCount:
    return "sample-count";
  case RtDebugView::PathLength:
    return "path-length";
  case RtDebugView::Firefly:
    return "firefly";
  case RtDebugView::Off:
  default:
    return "off";
  }
}

std::optional<RtSbtLayout>
computeRtSbtLayout(const RtSbtLayoutRequest &request) noexcept {
  if (request.shader_group_handle_size == 0u ||
      !isPowerOfTwo(request.shader_group_handle_alignment) ||
      !isPowerOfTwo(request.shader_group_base_alignment) ||
      request.max_shader_group_stride == 0u ||
      request.miss_group_count == 0u ||
      request.hit_group_count == 0u ||
      request.buffer_bytes == 0u) {
    return std::nullopt;
  }

  std::uint64_t stride = 0;
  if (!alignUpChecked(request.shader_group_handle_size,
                      request.shader_group_handle_alignment, stride) ||
      stride > request.max_shader_group_stride ||
      stride > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::nullopt;
  }

  RtSbtLayout layout;
  layout.shader_group_stride = static_cast<std::uint32_t>(stride);
  layout.raygen_offset = 0u;
  if (!alignUpChecked(stride, request.shader_group_base_alignment,
                      layout.miss_offset)) {
    return std::nullopt;
  }
  std::uint64_t miss_bytes = 0;
  std::uint64_t miss_end = 0;
  if (!checkedMultiply(stride, request.miss_group_count, miss_bytes) ||
      !checkedAdd(layout.miss_offset, miss_bytes, miss_end) ||
      !alignUpChecked(miss_end, request.shader_group_base_alignment,
                      layout.hit_offset)) {
    return std::nullopt;
  }
  std::uint64_t hit_bytes = 0;
  if (!checkedMultiply(stride, request.hit_group_count, hit_bytes) ||
      !checkedAdd(layout.hit_offset, hit_bytes, layout.layout_bytes)) {
    return std::nullopt;
  }

  std::uint64_t aligned_base_address = 0;
  if (!alignUpChecked(request.buffer_device_address,
                      request.shader_group_base_alignment,
                      aligned_base_address)) {
    return std::nullopt;
  }
  layout.base_offset =
      aligned_base_address - request.buffer_device_address;
  std::uint64_t required_bytes = 0;
  if (!checkedAdd(layout.base_offset, layout.layout_bytes, required_bytes) ||
      required_bytes > request.buffer_bytes) {
    return std::nullopt;
  }

  std::uint64_t miss_address = 0;
  std::uint64_t hit_address = 0;
  if (!checkedAdd(aligned_base_address, layout.miss_offset, miss_address) ||
      !checkedAdd(aligned_base_address, layout.hit_offset, hit_address) ||
      aligned_base_address % request.shader_group_base_alignment != 0u ||
      miss_address % request.shader_group_base_alignment != 0u ||
      hit_address % request.shader_group_base_alignment != 0u) {
    return std::nullopt;
  }
  return layout;
}

bool rtDispatchBuffersInBounds(
    const RtDispatchBufferBounds &bounds) noexcept {
  if (bounds.vertex_count == 0u || bounds.primitive_count == 0u ||
      bounds.instance_count == 0u) {
    return false;
  }
  std::uint64_t normal_required = 0;
  std::uint64_t tangent_required = 0;
  std::uint64_t index_elements = 0;
  std::uint64_t index_required = 0;
  std::uint64_t uv_required = 0;
  std::uint64_t color_required = 0;
  std::uint64_t flag_required = 0;
  std::uint64_t primitive_metadata_required = 0;
  std::uint64_t primitive_optics_required = 0;
  std::uint64_t instance_metadata_required = 0;
  return checkedMultiply(bounds.vertex_count, 16u, normal_required) &&
         checkedMultiply(bounds.vertex_count, 16u, tangent_required) &&
         checkedMultiply(bounds.primitive_count, 3u, index_elements) &&
         checkedMultiply(index_elements, 4u, index_required) &&
         checkedMultiply(bounds.vertex_count, 8u, uv_required) &&
         checkedMultiply(bounds.vertex_count, 16u, color_required) &&
         checkedMultiply(bounds.primitive_count, 4u, flag_required) &&
          checkedMultiply(bounds.primitive_count, 16u,
                          primitive_metadata_required) &&
          checkedMultiply(bounds.primitive_count, 32u,
                          primitive_optics_required) &&
         checkedMultiply(bounds.instance_count, 16u,
                         instance_metadata_required) &&
         bounds.normal_bytes >= normal_required &&
         bounds.tangent_bytes >= tangent_required &&
         bounds.index_bytes >= index_required &&
         bounds.uv_bytes >= uv_required &&
         bounds.color_bytes >= color_required &&
          bounds.primitive_flag_bytes >= flag_required &&
          bounds.primitive_metadata_bytes >= primitive_metadata_required &&
          bounds.primitive_optics_bytes >= primitive_optics_required &&
          bounds.instance_metadata_bytes >= instance_metadata_required;
}

std::optional<RtRayTriangleHit>
intersectRtTriangleTwoSided(
    const std::array<float, 3> &origin,
    const std::array<float, 3> &direction,
    const std::array<float, 3> &vertex0,
    const std::array<float, 3> &vertex1,
    const std::array<float, 3> &vertex2, float t_min,
    float t_max) noexcept {
  const auto finite3 = [](const std::array<float, 3> &value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
  };
  if (!finite3(origin) || !finite3(direction) || !finite3(vertex0) ||
      !finite3(vertex1) || !finite3(vertex2) ||
      !std::isfinite(t_min) || !std::isfinite(t_max) ||
      t_min > t_max) {
    return std::nullopt;
  }

  const std::array<double, 3> edge1{
      static_cast<double>(vertex1[0]) - vertex0[0],
      static_cast<double>(vertex1[1]) - vertex0[1],
      static_cast<double>(vertex1[2]) - vertex0[2]};
  const std::array<double, 3> edge2{
      static_cast<double>(vertex2[0]) - vertex0[0],
      static_cast<double>(vertex2[1]) - vertex0[1],
      static_cast<double>(vertex2[2]) - vertex0[2]};
  const std::array<double, 3> p{
      static_cast<double>(direction[1]) * edge2[2] -
          static_cast<double>(direction[2]) * edge2[1],
      static_cast<double>(direction[2]) * edge2[0] -
          static_cast<double>(direction[0]) * edge2[2],
      static_cast<double>(direction[0]) * edge2[1] -
          static_cast<double>(direction[1]) * edge2[0]};
  const double determinant =
      edge1[0] * p[0] + edge1[1] * p[1] + edge1[2] * p[2];
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
    return std::nullopt;
  }
  const double inverse_determinant = 1.0 / determinant;
  const std::array<double, 3> from_vertex0{
      static_cast<double>(origin[0]) - vertex0[0],
      static_cast<double>(origin[1]) - vertex0[1],
      static_cast<double>(origin[2]) - vertex0[2]};
  const double barycentric1 =
      (from_vertex0[0] * p[0] + from_vertex0[1] * p[1] +
       from_vertex0[2] * p[2]) *
      inverse_determinant;
  constexpr double kBarycentricTolerance = 1.0e-7;
  if (!std::isfinite(barycentric1) ||
      barycentric1 < -kBarycentricTolerance ||
      barycentric1 > 1.0 + kBarycentricTolerance) {
    return std::nullopt;
  }
  const std::array<double, 3> q{
      from_vertex0[1] * edge1[2] - from_vertex0[2] * edge1[1],
      from_vertex0[2] * edge1[0] - from_vertex0[0] * edge1[2],
      from_vertex0[0] * edge1[1] - from_vertex0[1] * edge1[0]};
  const double barycentric2 =
      (static_cast<double>(direction[0]) * q[0] +
       static_cast<double>(direction[1]) * q[1] +
       static_cast<double>(direction[2]) * q[2]) *
      inverse_determinant;
  if (!std::isfinite(barycentric2) ||
      barycentric2 < -kBarycentricTolerance ||
      barycentric1 + barycentric2 > 1.0 + kBarycentricTolerance) {
    return std::nullopt;
  }
  const double distance =
      (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) *
      inverse_determinant;
  if (!std::isfinite(distance) || distance < t_min || distance > t_max) {
    return std::nullopt;
  }
  return RtRayTriangleHit{
      static_cast<float>(distance),
      {static_cast<float>(barycentric1),
       static_cast<float>(barycentric2)}};
}

std::optional<RtNearestValidHit>
selectRtNearestValidHit(
    std::span<const RtHitCandidate> candidates, float t_min,
    float t_max, float cutoff) noexcept {
  if (!std::isfinite(t_min) || !std::isfinite(t_max) ||
      !std::isfinite(cutoff) || t_min > t_max) {
    return std::nullopt;
  }
  std::optional<RtNearestValidHit> nearest;
  for (const RtHitCandidate &candidate : candidates) {
    const float opacity =
        rtAcceptedOpacity(candidate.alpha_mode, candidate.alpha, cutoff);
    if (!std::isfinite(candidate.distance) ||
        candidate.distance < t_min || candidate.distance > t_max ||
        !(opacity > 0.0f)) {
      continue;
    }
    if (!nearest || candidate.distance < nearest->distance) {
      nearest = RtNearestValidHit{
          candidate.distance, candidate.primitive_identity, opacity};
    }
  }
  return nearest;
}

RtMotionProjectionResult evaluateRtMotionProjection(
    const RtMotionProjectionInput &input) noexcept {
  RtMotionProjectionResult result;
  if (!input.camera_history_valid || !input.geometry_history_valid ||
      input.viewport_width == 0u || input.viewport_height == 0u ||
      !std::isfinite(input.current_uv[0]) ||
      !std::isfinite(input.current_uv[1]) ||
      input.current_uv[0] < 0.0f || input.current_uv[0] > 1.0f ||
      input.current_uv[1] < 0.0f || input.current_uv[1] > 1.0f) {
    return result;
  }
  for (float component : input.previous_clip) {
    if (!std::isfinite(component)) {
      return result;
    }
  }
  if (!(input.previous_clip[3] > 1.0e-6f)) {
    return result;
  }

  const float inverse_w = 1.0f / input.previous_clip[3];
  const std::array<float, 2> previous_uv{
      input.previous_clip[0] * inverse_w * 0.5f + 0.5f,
      input.previous_clip[1] * inverse_w * 0.5f + 0.5f};
  if (!std::isfinite(previous_uv[0]) ||
      !std::isfinite(previous_uv[1])) {
    return result;
  }

  result.current_to_previous_pixels = {
      (previous_uv[0] - input.current_uv[0]) *
          static_cast<float>(input.viewport_width),
      (previous_uv[1] - input.current_uv[1]) *
          static_cast<float>(input.viewport_height)};
  if (!std::isfinite(result.current_to_previous_pixels[0]) ||
      !std::isfinite(result.current_to_previous_pixels[1])) {
    return {};
  }
  const bool inside_previous_viewport =
      previous_uv[0] >= 0.0f && previous_uv[0] < 1.0f &&
      previous_uv[1] >= 0.0f && previous_uv[1] < 1.0f;
  if (!inside_previous_viewport) {
    return result;
  }
  result.disocclusion = 0.0f;
  result.valid = true;
  return result;
}

bool isNvidiaVendorId(std::uint32_t vendor_id) noexcept {
  return vendor_id == kVendorIdNvidia;
}

bool isNvidiaRtx20OrNewer(
    std::uint32_t device_id, std::string_view device_name) noexcept {
  (void)device_id;
  std::string name(device_name);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::toupper(value));
                 });
  // Consumer and workstation names are stable across desktop/mobile SKUs.
  // Explicitly require a generation marker so a generic "NVIDIA GPU" or a
  // GTX 16/10/9 adapter cannot accidentally opt into the path tracer.
  constexpr std::array<std::string_view, 8> kRtxMarkers{{
      "RTX 20", "RTX20", "RTX 30", "RTX30",
      "RTX 40", "RTX40", "RTX 50", "RTX50",
  }};
  for (const std::string_view marker : kRtxMarkers) {
    if (name.find(marker) != std::string::npos) {
      return true;
    }
  }
  // NVIDIA professional/datacenter products use a non-numbered RTX/A/L/H/B
  // naming scheme but are Turing-or-newer RT hardware.
  constexpr std::array<std::string_view, 8> kProfessionalMarkers{{
      "QUADRO RTX", "RTX A", "A10", "A16", "A40",
      "A100", "L4", "L40",
  }};
  for (const std::string_view marker : kProfessionalMarkers) {
    if (name.find(marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

RayTracingCapability evaluateRayTracingCapability(
    std::uint32_t vendor_id, std::uint32_t device_id, std::string_view device_name,
    bool has_required_extensions, bool has_required_features,
    std::uint32_t max_ray_recursion_depth, std::uint32_t api_version,
    std::uint32_t driver_version) {
  RayTracingCapability cap;
  cap.vendor_id = vendor_id;
  cap.device_id = device_id;
  cap.device_name = std::string(device_name);
  cap.api_version = api_version;
  cap.driver_version = driver_version;
  cap.max_ray_recursion_depth = max_ray_recursion_depth;
  cap.is_nvidia = isNvidiaVendorId(vendor_id);
  cap.is_rtx20_or_newer =
      cap.is_nvidia && isNvidiaRtx20OrNewer(device_id, device_name);
  cap.has_required_extensions = has_required_extensions;
  cap.has_required_features = has_required_features;
  cap.supported =
      cap.is_rtx20_or_newer && cap.has_required_extensions &&
      cap.has_required_features;

  if (cap.supported) {
    cap.unsupported_reason.clear();
    return cap;
  }

  if (!cap.is_nvidia) {
    cap.unsupported_reason =
        "GPU is not NVIDIA (ray tracing option requires an NVIDIA GPU with RT cores)";
  } else if (!cap.is_rtx20_or_newer) {
    cap.unsupported_reason =
        "Path tracing requires an NVIDIA RTX 20-series GPU or newer";
  } else if (!cap.has_required_extensions) {
    cap.unsupported_reason =
        "Vulkan ray-tracing extensions are missing (update GPU driver / Vulkan runtime)";
  } else if (!cap.has_required_features) {
    cap.unsupported_reason =
        "Vulkan ray-tracing features are unavailable on this NVIDIA device";
  } else {
    cap.unsupported_reason = "Ray tracing is not available";
  }
  return cap;
}

void setVulkanPathTraceAvailability(
    bool path_tracer_ready, bool ray_tracing_pipeline_ready) noexcept {
  g_vulkan_path_tracer_ready = path_tracer_ready;
  g_vulkan_rt_pipeline_ready =
      path_tracer_ready && ray_tracing_pipeline_ready;
}

VulkanPathTraceAvailability queryVulkanPathTraceAvailability() noexcept {
  return {g_vulkan_path_tracer_ready, g_vulkan_rt_pipeline_ready};
}

VulkanPathTraceImplementation selectVulkanPathTraceImplementation(
    bool user_wants_rt, const RayTracingCapability &hardware,
    const VulkanPathTraceAvailability &availability) noexcept {
  if (!user_wants_rt || !hardware.supported ||
      !hardware.device_extensions_enabled) {
    return VulkanPathTraceImplementation::None;
  }
  if (availability.path_tracer_ready &&
      availability.ray_tracing_pipeline_ready) {
    return VulkanPathTraceImplementation::RayTracingPipeline;
  }
  return VulkanPathTraceImplementation::RayQuery;
}

const char *vulkanPathTraceImplementationName(
    VulkanPathTraceImplementation implementation) noexcept {
  switch (implementation) {
  case VulkanPathTraceImplementation::RayTracingPipeline:
    return "Built-in Vulkan RT Pipeline";
  case VulkanPathTraceImplementation::RayQuery:
    return "Vulkan Ray Query Compatibility";
  case VulkanPathTraceImplementation::None:
  default:
    return "None";
  }
}

RenderPath resolveRenderPath(bool user_wants_ray_tracing,
                             const RayTracingCapability &capability) noexcept {
  if (user_wants_ray_tracing && capability.supported &&
      capability.device_extensions_enabled) {
    return RenderPath::RayTracing;
  }
  return RenderPath::Raster;
}

const char *renderPathName(RenderPath path) noexcept {
  switch (path) {
  case RenderPath::RayTracing:
    return "RayTracing";
  case RenderPath::Raster:
  default:
    return "Raster";
  }
}

bool clampRayTracingPreference(bool user_wants_ray_tracing,
                               bool hardware_supported) noexcept {
  return user_wants_ray_tracing && hardware_supported;
}

} // namespace xpbd::gfx
