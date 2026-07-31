#include "xpbd/gfx/ray_tracing.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>

namespace xpbd::gfx {
namespace {

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
  material.roughness =
      std::isfinite(material.roughness)
          ? std::clamp(material.roughness, 0.02f, 1.0f)
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

float rtGgxDistribution(float normal_dot_half, float roughness) noexcept {
  if (!std::isfinite(normal_dot_half) || !std::isfinite(roughness)) {
    return 0.0f;
  }
  const double n_dot_h =
      std::clamp(static_cast<double>(normal_dot_half), 0.0, 1.0);
  if (!(n_dot_h > 0.0)) {
    return 0.0f;
  }
  const double alpha =
      std::clamp(static_cast<double>(roughness), 0.02, 1.0);
  const double alpha_squared = alpha * alpha;
  const double denominator =
      n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0;
  constexpr double kPi = 3.14159265358979323846;
  const double value =
      alpha_squared / (kPi * denominator * denominator);
  return std::isfinite(value) ? static_cast<float>(value) : 0.0f;
}

float rtSmithGgxG1(float normal_dot_direction, float roughness) noexcept {
  if (!std::isfinite(normal_dot_direction) ||
      !std::isfinite(roughness)) {
    return 0.0f;
  }
  const double cosine =
      std::clamp(static_cast<double>(normal_dot_direction), 0.0, 1.0);
  if (!(cosine > 0.0)) {
    return 0.0f;
  }
  const double alpha =
      std::clamp(static_cast<double>(roughness), 0.02, 1.0);
  const double root =
      std::sqrt(alpha * alpha +
                (1.0 - alpha * alpha) * cosine * cosine);
  const double value = 2.0 * cosine / (cosine + root);
  return std::isfinite(value) ? static_cast<float>(value) : 0.0f;
}

RtBsdfLobeProbabilities
rtBsdfLobeProbabilities(const RtBsdfMaterial &input) noexcept {
  const RtBsdfMaterial material = normalizeBsdfMaterial(input);
  const float f0_importance = std::clamp(max3(material.f0), 0.0f, 0.99f);
  float diffuse_importance =
      material.metal
          ? 0.0f
          : (1.0f - material.transmission) *
                (1.0f - f0_importance) *
                std::max(luminance3(material.base_color), 0.05f);
  float glossy_importance = std::max(f0_importance, 0.02f);
  float transmission_importance =
      material.metal
          ? 0.0f
          : material.transmission * (1.0f - f0_importance) *
                std::max(luminance3(material.base_color), 0.05f);
  const float total =
      diffuse_importance + glossy_importance + transmission_importance;
  if (!std::isfinite(total) || !(total > 1.0e-8f)) {
    return {0.0f, 1.0f, 0.0f};
  }
  return {diffuse_importance / total, glossy_importance / total,
          transmission_importance / total};
}

RtBsdfEval evaluateRtBsdf(
    const RtBsdfMaterial &input, const Vec3 &shading_normal,
    const Vec3 &view_direction, const Vec3 &light_direction) noexcept {
  RtBsdfEval result;
  const RtBsdfMaterial material = normalizeBsdfMaterial(input);
  const Vec3 normal =
      normalize3(shading_normal, {0.0f, 1.0f, 0.0f});
  const Vec3 view = normalize3(view_direction, {});
  const Vec3 light = normalize3(light_direction, {});
  const float n_dot_v = dot3(normal, view);
  const float n_dot_l = dot3(normal, light);
  if (!finite3(view) || !finite3(light) || !(n_dot_v > 0.0f) ||
      !(n_dot_l > 0.0f)) {
    return result;
  }
  const Vec3 half_vector = normalize3(add3(view, light), {});
  const float n_dot_h = std::max(dot3(normal, half_vector), 0.0f);
  const float v_dot_h = std::max(dot3(view, half_vector), 0.0f);
  if (!(n_dot_h > 0.0f) || !(v_dot_h > 0.0f)) {
    return result;
  }

  constexpr float kInversePi = 0.31830988618379067154f;
  const Vec3 fresnel = rtFresnelSchlick(material.f0, v_dot_h);
  const float distribution =
      rtGgxDistribution(n_dot_h, material.roughness);
  const float geometry =
      rtSmithGgxG1(n_dot_v, material.roughness) *
      rtSmithGgxG1(n_dot_l, material.roughness);
  const float specular_scale =
      distribution * geometry /
      std::max(4.0f * n_dot_v * n_dot_l, 1.0e-8f);
  const float diffuse_scale =
      material.metal
          ? 0.0f
          : (1.0f - material.transmission) *
                (1.0f - max3(fresnel)) * kInversePi;
  for (std::size_t i = 0; i < result.value.size(); ++i) {
    result.value[i] =
        material.base_color[i] * diffuse_scale +
        fresnel[i] * specular_scale;
  }

  const RtBsdfLobeProbabilities probabilities =
      rtBsdfLobeProbabilities(material);
  const float diffuse_pdf = n_dot_l * kInversePi;
  const float glossy_pdf =
      distribution * n_dot_h / std::max(4.0f * v_dot_h, 1.0e-8f);
  result.pdf = probabilities.diffuse * diffuse_pdf +
               probabilities.glossy * glossy_pdf;
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

  if (lobe_u < probabilities.diffuse) {
    const PathTraceHemisphereSample sampled =
        samplePathTraceCosineHemisphere(normal, sample_u, sample_v);
    result.direction = sampled.direction;
    result.lobe = PathTraceLobe::Diffuse;
  } else if (lobe_u <
             probabilities.diffuse + probabilities.glossy) {
    const float alpha = material.roughness;
    const float alpha_squared = alpha * alpha;
    const float phi = 6.28318530717958647692f * sample_v;
    const float cos_theta = std::sqrt(
        std::max(0.0f, (1.0f - sample_u) /
                           (1.0f +
                            (alpha_squared - 1.0f) * sample_u)));
    const float sin_theta =
        std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    const Vec3 up = std::abs(normal[2]) < 0.999f
                        ? Vec3{0.0f, 0.0f, 1.0f}
                        : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 tangent =
        normalize3(cross3(up, normal), {1.0f, 0.0f, 0.0f});
    const Vec3 bitangent = cross3(normal, tangent);
    const Vec3 half_vector = normalize3(
        add3(add3(multiply3(tangent, sin_theta * std::cos(phi)),
                  multiply3(bitangent, sin_theta * std::sin(phi))),
             multiply3(normal, cos_theta)),
        normal);
    const float v_dot_h = dot3(view, half_vector);
    if (!(v_dot_h > 0.0f)) {
      return result;
    }
    result.direction =
        normalize3(add3(multiply3(half_vector, 2.0f * v_dot_h),
                        multiply3(view, -1.0f)),
                   {});
    result.lobe = PathTraceLobe::Glossy;
  } else {
    const float eta_incident = front_face ? 1.0f : material.ior;
    const float eta_transmitted = front_face ? material.ior : 1.0f;
    const float eta = eta_incident / eta_transmitted;
    const float cos_i = std::clamp(n_dot_v, 0.0f, 1.0f);
    const float sin_t_squared =
        eta * eta * std::max(0.0f, 1.0f - cos_i * cos_i);
    result.lobe = PathTraceLobe::Transmission;
    result.delta = true;
    result.pdf = probabilities.transmission;
    if (sin_t_squared >= 1.0f) {
      result.direction =
          normalize3(add3(multiply3(normal, 2.0f * cos_i),
                          multiply3(view, -1.0f)),
                     {});
      result.total_internal_reflection = true;
      const float reflected_share =
          max3(rtFresnelSchlick(material.f0, cos_i));
      const float inverse_probability =
          1.0f / std::max(probabilities.transmission, 1.0e-8f);
      for (std::size_t i = 0; i < result.weight.size(); ++i) {
        result.weight[i] =
            material.base_color[i] * material.transmission *
            (1.0f - reflected_share) *
            inverse_probability;
      }
    } else {
      const float cos_t =
          std::sqrt(std::max(0.0f, 1.0f - sin_t_squared));
      result.direction = normalize3(
          add3(multiply3(view, -eta),
               multiply3(normal, eta * cos_i - cos_t)),
          {});
      const float fresnel = rtFresnelDielectric(
          cos_i, eta_incident, eta_transmitted);
      const float inverse_probability =
          1.0f / std::max(probabilities.transmission, 1.0e-8f);
      for (std::size_t i = 0; i < result.weight.size(); ++i) {
        result.weight[i] =
            material.base_color[i] * material.transmission *
            (1.0f - fresnel) * inverse_probability;
      }
    }
    result.valid = probabilities.transmission > 0.0f &&
                   finite3(result.direction) && finite3(result.weight);
    if (!result.valid) {
      result = {};
    }
    return result;
  }

  const RtBsdfEval evaluated =
      evaluateRtBsdf(material, normal, view, result.direction);
  if (!evaluated.valid) {
    return {};
  }
  result.value = evaluated.value;
  result.pdf = evaluated.pdf;
  const float cosine = std::max(dot3(normal, result.direction), 0.0f);
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
             ? std::clamp(correction, 0.0f, 16.0f)
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
         checkedMultiply(bounds.instance_count, 16u,
                         instance_metadata_required) &&
         bounds.normal_bytes >= normal_required &&
         bounds.tangent_bytes >= tangent_required &&
         bounds.index_bytes >= index_required &&
         bounds.uv_bytes >= uv_required &&
         bounds.color_bytes >= color_required &&
         bounds.primitive_flag_bytes >= flag_required &&
         bounds.primitive_metadata_bytes >= primitive_metadata_required &&
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
      !std::isfinite(previous_uv[1]) ||
      previous_uv[0] < 0.0f || previous_uv[0] > 1.0f ||
      previous_uv[1] < 0.0f || previous_uv[1] > 1.0f) {
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
