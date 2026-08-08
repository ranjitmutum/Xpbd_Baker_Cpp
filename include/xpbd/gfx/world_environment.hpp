#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xpbd::gfx {

enum class SkyRendering : std::uint8_t {
  Off = 0,
  ProceduralDayNight = 1,
  UserHdri = 2,
};

enum class SkyDirectionMode : std::uint8_t {
  Automatic = 0,
  ArtisticOffset = 1,
};

enum class MoonPhaseMode : std::uint8_t {
  Automatic = 0,
  Manual = 1,
};

enum class CloudQuality : std::uint8_t {
  Low = 0,
  Medium = 1,
  High = 2,
  Still = 3,
};

enum class SkyDebugView : std::uint8_t {
  Off = 0,
  Radiance = 1,
  EnvironmentPdf = 2,
  CloudTransmittance = 3,
};

enum class TwilightPhase : std::uint8_t {
  Day = 0,
  SunriseSunset = 1,
  Civil = 2,
  Nautical = 3,
  Astronomical = 4,
  Night = 5,
};

struct UtcDateTime {
  int year = 2024;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  double second = 0.0;
};

struct ObserverLocation {
  double latitude_degrees = 0.0;
  double longitude_degrees = 0.0;
  double elevation_meters = 0.0;
  // Positive values rotate astronomical north toward world +X around +Y.
  double north_offset_degrees = 0.0;
};

struct CelestialBodyState {
  // Unit vector from the observer toward the body. World +Y is up, +Z is
  // astronomical north when north_offset_degrees is zero, and +X is east.
  std::array<double, 3> direction{0.0, 1.0, 0.0};
  double azimuth_degrees = 0.0;
  double apparent_altitude_degrees = 0.0;
  double geometric_altitude_degrees = 0.0;
  double angular_diameter_degrees = 0.0;
};

struct CelestialState {
  bool valid = false;
  UtcDateTime utc{};
  ObserverLocation observer{};
  CelestialBodyState sun{};
  CelestialBodyState moon{};
  double moon_distance_km = 0.0;
  double moon_phase_angle_degrees = 0.0;
  double moon_illuminated_fraction = 0.0;
  double moon_magnitude = 0.0;
  double moon_libration_latitude_degrees = 0.0;
  double moon_libration_longitude_degrees = 0.0;
  double sidereal_time_hours = 0.0;
  TwilightPhase twilight = TwilightPhase::Night;
};

[[nodiscard]] const char *twilightPhaseName(TwilightPhase phase) noexcept;

[[nodiscard]] bool
computeCelestialState(const UtcDateTime &utc,
                      const ObserverLocation &observer, CelestialState &out,
                      std::string *error = nullptr);

// Calendar-safe UTC shift used by local-time UI, playback, and persistence
// migration. The output is committed only on success.
[[nodiscard]] bool shiftUtcDateTime(const UtcDateTime &utc,
                                    double offset_seconds,
                                    UtcDateTime &out,
                                    std::string *error = nullptr);

// Applies bounded artistic Sun azimuth/elevation offsets in world-space
// while preserving the astronomy-derived Moon and time/location values.
[[nodiscard]] bool applyCelestialSunAngleOffsets(
    CelestialState &state, double azimuth_offset_degrees,
    double altitude_offset_degrees, std::string *error = nullptr);

// Applies bounded artistic Moon azimuth/elevation offsets without changing
// the astronomy-derived time, location, phase, or Sun direction.
[[nodiscard]] bool applyCelestialMoonAngleOffsets(
    CelestialState &state, double azimuth_offset_degrees,
    double altitude_offset_degrees, std::string *error = nullptr);

enum class AtmosphereLutFormat : std::uint8_t {
  Rgba16Float = 1,
};

struct AtmosphereDensityLayer {
  double width_km = 0.0;
  double exp_term = 0.0;
  double exp_scale_per_km = 0.0;
  double linear_term_per_km = 0.0;
  double constant_term = 0.0;
};

struct AtmosphereDensityProfile {
  std::array<AtmosphereDensityLayer, 2> layers{};
};

struct AtmospherePhysicalParameters {
  std::array<double, 3> solar_irradiance{};
  double sun_angular_radius_radians = 0.0;
  double bottom_radius_km = 0.0;
  double top_radius_km = 0.0;
  AtmosphereDensityProfile rayleigh_density{};
  std::array<double, 3> rayleigh_scattering_per_km{};
  AtmosphereDensityProfile mie_density{};
  std::array<double, 3> mie_scattering_per_km{};
  std::array<double, 3> mie_extinction_per_km{};
  double mie_phase_function_g = 0.0;
  AtmosphereDensityProfile absorption_density{};
  std::array<double, 3> absorption_extinction_per_km{};
  std::array<double, 3> ground_albedo{};
  double minimum_sun_cosine = 0.0;
};

struct AtmosphereLutDimensions {
  std::uint32_t transmittance_width = 256;
  std::uint32_t transmittance_height = 64;
  std::uint32_t scattering_radial = 32;
  std::uint32_t scattering_view_cosine = 128;
  std::uint32_t scattering_sun_cosine = 32;
  std::uint32_t scattering_relative_azimuth = 8;
  std::uint32_t irradiance_width = 64;
  std::uint32_t irradiance_height = 16;

  [[nodiscard]] std::uint32_t scatteringWidth() const noexcept;
};

struct BrunetonAtmosphereConfig {
  // Bump whenever the Vulkan adaptation, cache serialization, or LUT meaning
  // changes. The frozen upstream GLSL hashes are also part of the cache key.
  std::uint32_t implementation_revision = 1;
  AtmosphereLutFormat format = AtmosphereLutFormat::Rgba16Float;
  std::uint32_t scattering_orders = 4;
  AtmosphereLutDimensions dimensions{};
  AtmospherePhysicalParameters physical{};

  [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] BrunetonAtmosphereConfig defaultEarthAtmosphereConfig() noexcept;

// Canonical, deterministic cache identity. Returns an empty string for invalid
// configurations. It includes the implementation revision, physical values,
// LUT format/dimensions, and the exact frozen Bruneton shader hashes.
[[nodiscard]] std::string
brunetonAtmosphereCacheKey(const BrunetonAtmosphereConfig &config);

struct VolumetricCloudState {
  bool enabled = false;
  float coverage = 0.55f;
  float density = 1.0f;
  float base_altitude_km = 1.5f;
  float thickness_km = 4.0f;
  std::array<float, 2> wind_direction{1.0f, 0.0f};
  float wind_speed_km_per_hour = 10.0f;
  float shadow_strength = 1.0f;
  CloudQuality quality = CloudQuality::Medium;
  float weather_scale = 1.0f;
  std::array<float, 2> weather_offset_km{0.0f, 0.0f};
  float base_shape_scale = 1.0f;
  float detail_scale = 1.0f;
  float erosion = 0.32f;
  float forward_scattering = 0.72f;
  float silver_lining = 1.0f;
  float absorption = 1.0f;
  float multiple_scattering = 0.35f;
  // Full-resolution procedural environment is 2048x1024. Lower ratios remain
  // available for interactive performance.
  float render_ratio = 1.0f;
  bool reprojection = true;
  float history_weight = 0.92f;
  float lighting_strength = 1.0f;
  std::uint32_t shadow_resolution = 512u;
  float time_seconds = 0.0f;
  std::uint32_t seed = 0x6d2b79f5u;
  std::uint32_t ray_steps = 48u;
  std::uint32_t light_steps = 6u;
  std::uint32_t temporal_frame = 0u;
  std::uint64_t generation = 0u;

  [[nodiscard]] bool valid() const noexcept;
};

// Canonical cloud-state identity used by the dynamic sky cache and PT history.
// Disabled clouds still produce a stable identity; invalid enabled states
// produce an empty string.
[[nodiscard]] std::string
volumetricCloudCacheKey(const VolumetricCloudState &state);

struct FloatEnvironmentImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<float> rgba;

  [[nodiscard]] bool valid() const noexcept {
    return width > 0u && height > 0u &&
           rgba.size() ==
               static_cast<std::size_t>(width) * height * std::size_t{4};
  }
};

struct HdrDecodeLimits {
  std::uint32_t maximum_width = 8192;
  std::uint32_t maximum_height = 4096;
  std::size_t maximum_decoded_bytes = std::size_t{512} * 1024u * 1024u;
  std::uint64_t maximum_asset_peak_bytes =
      std::uint64_t{2} * 1024u * 1024u * 1024u;
  std::uint64_t asset_safety_margin_bytes =
      std::uint64_t{64} * 1024u * 1024u;
};

// Strict, transactional Radiance RGBE decode. On failure, `out` is unchanged.
[[nodiscard]] bool decodeRadianceHdr(
    std::span<const std::uint8_t> encoded, FloatEnvironmentImage &out,
    std::string *error = nullptr, HdrDecodeLimits limits = {});

class AliasTable {
public:
  [[nodiscard]] bool build(std::span<const double> weights);
  void clear() noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return pmf_.size(); }
  [[nodiscard]] std::uint32_t sample(double column_sample,
                                     double coin_sample) const noexcept;
  [[nodiscard]] double probability(std::size_t index) const noexcept;
  [[nodiscard]] double acceptance(std::size_t index) const noexcept {
    return index < accept_.size() ? accept_[index] : 0.0;
  }
  [[nodiscard]] std::uint32_t aliasIndex(std::size_t index) const noexcept {
    return index < alias_.size() ? alias_[index] : 0u;
  }

private:
  std::vector<double> accept_;
  std::vector<double> pmf_;
  std::vector<std::uint32_t> alias_;
};

struct EnvironmentDirectionSample {
  bool valid = false;
  std::uint32_t texel_x = 0;
  std::uint32_t texel_y = 0;
  std::array<double, 3> direction{0.0, 1.0, 0.0};
  double solid_angle_pdf = 0.0;
};

class EnvironmentDistribution {
public:
  [[nodiscard]] bool build(const FloatEnvironmentImage &image);
  void clear() noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] double texelProbability(std::uint32_t x,
                                        std::uint32_t y) const noexcept;
  [[nodiscard]] double aliasAcceptance(std::uint32_t x,
                                       std::uint32_t y) const noexcept;
  [[nodiscard]] std::uint32_t aliasIndex(std::uint32_t x,
                                         std::uint32_t y) const noexcept;
  [[nodiscard]] double texelSolidAngle(std::uint32_t y) const noexcept;
  [[nodiscard]] EnvironmentDirectionSample
  sample(double column_sample, double coin_sample, double jitter_u,
         double jitter_v, double rotation_radians = 0.0) const noexcept;
  [[nodiscard]] double
  solidAnglePdf(const std::array<double, 3> &direction,
                double rotation_radians = 0.0) const noexcept;

private:
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  AliasTable alias_;
};

struct HdrEnvironmentAsset {
  std::string source_identity;
  std::string checksum;
  FloatEnvironmentImage radiance;
  EnvironmentDistribution distribution;
  std::uint64_t generation = 0;

  [[nodiscard]] bool valid() const noexcept {
    return !source_identity.empty() && !checksum.empty() && radiance.valid() &&
           distribution.valid();
  }
};

// Builds a complete HDR environment candidate and commits only on success.
[[nodiscard]] bool buildHdrEnvironmentAsset(
    std::span<const std::uint8_t> encoded, std::string source_identity,
    std::string checksum, std::uint64_t generation, HdrEnvironmentAsset &out,
    std::string *error = nullptr, HdrDecodeLimits limits = {});

// One admission decision covers the retained source asset, the temporary CPU
// resample/Alias build, the persistent runtime candidate, the GPU image/table,
// staging, and a fixed safety margin.  The resolver lowers the requested 2:1
// runtime extent deterministically when the complete transaction would exceed
// this budget; it never upscales beyond the source image.
struct HdriRuntimeBudgetLimits {
  std::uint32_t minimum_runtime_width = 256u;
  std::uint32_t maximum_runtime_width = 8192u;
  std::uint64_t maximum_total_bytes =
      std::uint64_t{2} * 1024u * 1024u * 1024u;
  std::uint64_t maximum_gpu_bytes =
      std::uint64_t{512} * 1024u * 1024u;
  std::uint64_t safety_margin_bytes =
      std::uint64_t{64} * 1024u * 1024u;
};

struct HdriRuntimeBudget {
  std::uint32_t source_width = 0u;
  std::uint32_t source_height = 0u;
  std::uint32_t requested_width = 0u;
  std::uint32_t resolved_width = 0u;
  std::uint32_t resolved_height = 0u;
  std::uint32_t distribution_width = 0u;
  std::uint32_t distribution_height = 0u;
  std::uint64_t source_radiance_bytes = 0u;
  std::uint64_t source_distribution_bytes = 0u;
  std::uint64_t runtime_radiance_bytes = 0u;
  std::uint64_t runtime_distribution_bytes = 0u;
  std::uint64_t distribution_scratch_bytes = 0u;
  std::uint64_t gpu_image_bytes = 0u;
  std::uint64_t gpu_distribution_bytes = 0u;
  std::uint64_t staging_bytes = 0u;
  std::uint64_t safety_margin_bytes = 0u;
  std::uint64_t total_bytes = 0u;

  [[nodiscard]] bool valid() const noexcept {
    return source_width > 0u && source_height > 0u &&
           source_width == source_height * 2u && requested_width > 0u &&
           resolved_width > 0u && resolved_height > 0u &&
           resolved_width == resolved_height * 2u &&
           distribution_width == resolved_width &&
           distribution_height == resolved_height && total_bytes > 0u;
  }
};

// Pure, allocation-free runtime extent/budget resolution.
[[nodiscard]] bool resolveHdriRuntimeBudget(
    const FloatEnvironmentImage &source, std::uint32_t requested_width,
    HdriRuntimeBudget &out, std::string *error = nullptr,
    HdriRuntimeBudgetLimits limits = {});

// Transactional lat-long resampling in linear radiance.  Horizontal taps
// wrap at the longitude seam; vertical taps clamp at the poles.  No tone map
// or [0,1] clamp is applied.
[[nodiscard]] bool resampleHdriLatLong(
    const FloatEnvironmentImage &source, std::uint32_t resolved_width,
    FloatEnvironmentImage &out, std::string *error = nullptr);

struct HdrEnvironmentRuntimeCandidate {
  FloatEnvironmentImage radiance;
  EnvironmentDistribution distribution;
  HdriRuntimeBudget budget;
  std::uint64_t content_generation = 0u;
  std::uint64_t runtime_settings_generation = 0u;

  [[nodiscard]] bool valid() const noexcept {
    return radiance.valid() && distribution.valid() && budget.valid() &&
           radiance.width == budget.resolved_width &&
           radiance.height == budget.resolved_height &&
           distribution.width() == budget.distribution_width &&
           distribution.height() == budget.distribution_height;
  }
};

// Builds the immutable CPU half of the synchronous HDR upload transaction and
// publishes it to `out` only after resampling, distribution construction, and
// the unified budget checks all succeed.
[[nodiscard]] bool buildHdrEnvironmentRuntimeCandidate(
    const HdrEnvironmentAsset &source, std::uint32_t requested_width,
    std::uint64_t runtime_settings_generation,
    HdrEnvironmentRuntimeCandidate &out, std::string *error = nullptr,
    HdriRuntimeBudgetLimits limits = {});

struct SkyTimeControls {
  float utc_offset_hours = 0.0f;
  bool playing = false;
  float time_speed = 60.0f;
};

struct SunControls {
  bool enabled = true;
  float strength = 1.0f;
  SkyDirectionMode direction_mode = SkyDirectionMode::Automatic;
  float color_temperature_kelvin = 5778.0f;
  float angular_diameter_degrees = 0.533f;
  bool disk_visible = true;
  bool cast_shadows = true;
};

struct MoonControls {
  bool enabled = true;
  float strength = 1.0f;
  MoonPhaseMode phase_mode = MoonPhaseMode::Automatic;
  float manual_illuminated_fraction = 0.5f;
  SkyDirectionMode direction_mode = SkyDirectionMode::Automatic;
  float azimuth_offset_degrees = 0.0f;
  float altitude_offset_degrees = 0.0f;
  float angular_diameter_degrees = 0.518f;
  float surface_detail = 1.0f;
  bool disk_visible = true;
  bool cast_shadows = true;
};

struct AtmosphereControls {
  float sky_relative_strength = 1.0f;
  float turbidity = 1.0f;
  float ozone = 1.0f;
  std::uint32_t lut_quality = 1u;
};

struct NightSkyControls {
  bool stars_enabled = true;
  float star_intensity = 1.0f;
  bool milky_way_enabled = true;
  float milky_way_intensity = 1.0f;
  float light_pollution = 0.0f;
  float star_rotation_degrees = 0.0f;
  float night_fill = 0.0f;
};

struct WorldEnvironmentState {
  SkyRendering sky_rendering = SkyRendering::Off;
  std::string selected_hdr_identity;
  bool background_visible = true;
  bool background_transparent = false;
  bool environment_lighting = true;
  bool sun_moon_lighting = true;
  float global_lighting_strength_ev = 0.0f;
  float background_exposure = 0.0f;
  float rotation_radians = 0.0f;
  std::uint32_t hdri_runtime_resolution = 2048u;
  bool procedural_resources_ready = false;
  BrunetonAtmosphereConfig atmosphere = defaultEarthAtmosphereConfig();
  CelestialState celestial{};
  SkyTimeControls time{};
  SunControls sun{};
  MoonControls moon{};
  AtmosphereControls atmosphere_controls{};
  NightSkyControls night{};
  double sun_azimuth_offset_degrees = 0.0;
  double sun_altitude_offset_degrees = 0.0;
  VolumetricCloudState clouds{};
  HdrEnvironmentAsset hdr{};
  SkyDebugView debug_view = SkyDebugView::Off;
  std::uint64_t lighting_generation = 0;
  std::uint64_t celestial_generation = 0;
  std::uint64_t cloud_generation = 0;
  std::uint64_t display_generation = 0;
  std::uint64_t target_generation = 0;
  std::uint64_t hdri_runtime_generation = 0;
  std::uint64_t generation = 0;
};

struct ResolvedWorldEnvironment {
  SkyRendering sky_rendering = SkyRendering::Off;
  bool background_visible = false;
  bool background_transparent = false;
  bool environment_lighting = false;
  bool sun_moon_lighting = false;
  float global_lighting_strength = 0.0f;
  float environment_strength = 0.0f;
  float background_exposure = 0.0f;
  float background_multiplier = 0.0f;
  float rotation_radians = 0.0f;
  const CelestialState *celestial = nullptr;
  const BrunetonAtmosphereConfig *atmosphere = nullptr;
  const SunControls *sun = nullptr;
  const MoonControls *moon = nullptr;
  const AtmosphereControls *atmosphere_controls = nullptr;
  const NightSkyControls *night = nullptr;
  const VolumetricCloudState *clouds = nullptr;
  const HdrEnvironmentAsset *hdr = nullptr;
  HdriRuntimeBudget hdri_runtime_budget{};
  std::uint32_t requested_hdri_runtime_width = 0u;
  std::uint32_t resolved_hdri_runtime_width = 0u;
  std::uint32_t resolved_hdri_runtime_height = 0u;
  std::uint64_t hdri_runtime_generation = 0u;
  SkyDebugView debug_view = SkyDebugView::Off;
  std::uint64_t lighting_generation = 0;
  std::uint64_t celestial_generation = 0;
  std::uint64_t cloud_generation = 0;
  std::uint64_t display_generation = 0;
  std::uint64_t target_generation = 0;
  std::uint64_t generation = 0;
  std::string warning;
};

[[nodiscard]] ResolvedWorldEnvironment
resolveWorldEnvironment(const WorldEnvironmentState &state);

// Read-only rendering projection of the World-owned Sun.  It never stores
// authoring state: every value is derived from ResolvedWorldEnvironment and
// its generation domains for the current frame.
struct ResolvedSunLight {
  std::array<float, 3> direction{0.0f, 1.0f, 0.0f};
  std::array<float, 3> color{1.0f, 1.0f, 1.0f};
  std::array<float, 3> radiance{0.0f, 0.0f, 0.0f};
  float angular_radius = 0.00465047f;
  float solid_angle = 0.0f;
  float strength = 0.0f;
  float power_estimate = 0.0f;
  bool enabled = false;
  bool disk_visible = false;
  bool casts_shadow = true;
  bool lighting_enabled = false;
  std::uint64_t generation = 0;
  std::uint64_t cloud_generation = 0;
};

[[nodiscard]] ResolvedSunLight
resolveSunLight(const ResolvedWorldEnvironment &resolved) noexcept;

enum class ResolvedEnvironmentKind : std::uint8_t {
  Constant = 0,
  UserHdri = 1,
  ProceduralDayNight = 2,
  AnalyticFallback = 3,
};

// Logical sample/evaluate/pdf projection. HDRI owns CPU sampling data directly;
// a procedural GPU cache may attach an equivalent immutable radiance/PDF pair
// for CPU reference tests without exposing Bruneton LUT internals.
struct ResolvedEnvironmentView {
  ResolvedEnvironmentKind kind = ResolvedEnvironmentKind::Constant;
  const FloatEnvironmentImage *radiance = nullptr;
  const EnvironmentDistribution *distribution = nullptr;
  std::array<float, 3> constant_radiance{0.0f, 0.0f, 0.0f};
  float analytic_strength = 0.0f;
  float lighting_strength = 0.0f;
  float background_multiplier = 0.0f;
  float rotation_radians = 0.0f;
  float power_estimate = 0.0f;
  bool lighting_enabled = false;
  bool background_visible = false;
  bool sampling_ready = false;
  bool gpu_cache_required = false;
  std::uint64_t generation = 0;
};

struct EnvironmentSample {
  std::array<float, 3> direction{0.0f, 1.0f, 0.0f};
  std::array<float, 3> radiance{0.0f, 0.0f, 0.0f};
  float pdf = 0.0f;
  bool valid = false;
};

[[nodiscard]] float estimateEnvironmentPower(
    const FloatEnvironmentImage &image, float lighting_strength) noexcept;
[[nodiscard]] ResolvedEnvironmentView resolveEnvironmentView(
    const ResolvedWorldEnvironment &resolved,
    const std::array<float, 3> &constant_fallback =
        {0.0f, 0.0f, 0.0f},
    float analytic_fallback_strength = 0.0f) noexcept;
[[nodiscard]] ResolvedEnvironmentView attachEnvironmentSamplingData(
    ResolvedEnvironmentView view, const FloatEnvironmentImage &radiance,
    const EnvironmentDistribution &distribution,
    std::uint64_t cache_generation) noexcept;
[[nodiscard]] EnvironmentSample sampleEnvironment(
    const ResolvedEnvironmentView &environment, float column_sample,
    float coin_sample, float jitter_u, float jitter_v) noexcept;
[[nodiscard]] std::array<float, 3> evaluateEnvironment(
    const ResolvedEnvironmentView &environment,
    const std::array<float, 3> &direction) noexcept;
[[nodiscard]] float environmentPdf(
    const ResolvedEnvironmentView &environment,
    const std::array<float, 3> &direction) noexcept;
[[nodiscard]] std::uint64_t environmentGeneration(
    const ResolvedEnvironmentView &environment) noexcept;

struct EmissivePatch {
  std::uint32_t triangle_index = 0;
  std::uint16_t tile_x = 0;
  std::uint16_t tile_y = 0;
  double world_area = 0.0;
  double average_luminance = 0.0;
};

[[nodiscard]] std::array<double, 3> sampleUniformTriangleBarycentrics(
    double sample_u, double sample_v) noexcept;
[[nodiscard]] double emissiveTriangleSolidAnglePdf(
    double triangle_selection_probability, double world_area,
    double distance_squared, double absolute_light_cosine) noexcept;

class EmissivePatchDistribution {
public:
  [[nodiscard]] bool build(std::span<const EmissivePatch> patches);
  void clear() noexcept;

  [[nodiscard]] bool valid() const noexcept { return alias_.valid(); }
  [[nodiscard]] std::uint32_t sample(double column_sample,
                                     double coin_sample) const noexcept {
    return alias_.sample(column_sample, coin_sample);
  }
  [[nodiscard]] double probability(std::size_t index) const noexcept {
    return alias_.probability(index);
  }

private:
  AliasTable alias_;
};

[[nodiscard]] double powerHeuristic(double pdf_a, double pdf_b,
                                    double exponent = 2.0) noexcept;

[[nodiscard]] double areaPdfToSolidAngle(double area_pdf,
                                         double distance_squared,
                                         double absolute_light_cosine) noexcept;

} // namespace xpbd::gfx
