#include "xpbd/gfx/world_environment.hpp"

#include "astronomy.h"
#include "stb_image.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <numeric>

namespace xpbd::gfx {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kSolarRadiusAu = 0.004650467260962157;

void setError(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
}

double clampUnitOpen(double value) noexcept {
  if (!std::isfinite(value) || value <= 0.0) {
    return 0.0;
  }
  return std::min(value, std::nextafter(1.0, 0.0));
}

double wrapRadians(double value) noexcept {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  value = std::fmod(value, kTwoPi);
  return value < 0.0 ? value + kTwoPi : value;
}

std::uint64_t mixResolvedGeneration(std::uint64_t hash,
                                    std::uint64_t value) noexcept {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  for (std::uint32_t byte = 0; byte < 8u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= kPrime;
  }
  return hash;
}

std::array<float, 3> colorTemperatureRgb(float kelvin) noexcept {
  const double finite_kelvin =
      std::isfinite(kelvin) ? static_cast<double>(kelvin) : 5778.0;
  const double temperature =
      std::clamp(finite_kelvin, 1000.0, 40000.0) / 100.0;
  double red = 255.0;
  double green = 255.0;
  double blue = 255.0;
  if (temperature <= 66.0) {
    green = 99.4708025861 * std::log(temperature) - 161.1195681661;
    blue = temperature <= 19.0
               ? 0.0
               : 138.5177312231 * std::log(temperature - 10.0) -
                     305.0447927307;
  } else {
    red = 329.698727446 * std::pow(temperature - 60.0, -0.1332047592);
    green = 288.1221695283 *
            std::pow(temperature - 60.0, -0.0755148492);
  }
  const auto normalized = [](double channel) {
    return static_cast<float>(std::clamp(channel, 0.0, 255.0) / 255.0);
  };
  return {normalized(red), normalized(green), normalized(blue)};
}

float smoothUnit(float value) noexcept {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return clamped * clamped * (3.0f - 2.0f * clamped);
}

float rgbLuminance(const std::array<float, 3> &value) noexcept {
  return 0.2126f * value[0] + 0.7152f * value[1] + 0.0722f * value[2];
}

std::array<float, 3> normalizedDirection(
    const std::array<float, 3> &direction) noexcept {
  const double length_squared =
      static_cast<double>(direction[0]) * direction[0] +
      static_cast<double>(direction[1]) * direction[1] +
      static_cast<double>(direction[2]) * direction[2];
  if (!(length_squared > 1.0e-20) || !std::isfinite(length_squared)) {
    return {0.0f, 1.0f, 0.0f};
  }
  const float inverse_length =
      static_cast<float>(1.0 / std::sqrt(length_squared));
  return {direction[0] * inverse_length, direction[1] * inverse_length,
          direction[2] * inverse_length};
}

std::array<float, 3> analyticFallbackRadiance(
    const std::array<float, 3> &direction, float strength) noexcept {
  if (!std::isfinite(strength) || !(strength > 0.0f)) {
    return {0.0f, 0.0f, 0.0f};
  }
  const std::array<float, 3> unit = normalizedDirection(direction);
  const float elevation = std::clamp(unit[1] * 0.5f + 0.5f, 0.0f, 1.0f);
  const float blend = std::pow(elevation, 0.65f);
  constexpr std::array<float, 3> kHorizon{0.42f, 0.55f, 0.72f};
  constexpr std::array<float, 3> kZenith{0.08f, 0.20f, 0.48f};
  std::array<float, 3> radiance{};
  for (std::size_t channel = 0; channel < radiance.size(); ++channel) {
    radiance[channel] =
        strength * (kHorizon[channel] +
                    (kZenith[channel] - kHorizon[channel]) * blend);
  }
  return radiance;
}

bool validObserver(const ObserverLocation &observer) noexcept {
  return std::isfinite(observer.latitude_degrees) &&
         std::isfinite(observer.longitude_degrees) &&
         std::isfinite(observer.elevation_meters) &&
         std::isfinite(observer.north_offset_degrees) &&
         observer.latitude_degrees >= -90.0 &&
         observer.latitude_degrees <= 90.0 &&
         observer.longitude_degrees >= -180.0 &&
         observer.longitude_degrees <= 180.0 &&
         observer.elevation_meters >= -1000.0 &&
         observer.elevation_meters <= 100000.0;
}

bool validUtc(const UtcDateTime &utc) noexcept {
  if (utc.year < 1 || utc.year > 9999 || utc.month < 1 || utc.month > 12 ||
      utc.hour < 0 || utc.hour > 23 || utc.minute < 0 || utc.minute > 59 ||
      !std::isfinite(utc.second) || utc.second < 0.0 ||
      utc.second >= 60.0) {
    return false;
  }
  const bool leap = (utc.year % 4 == 0 && utc.year % 100 != 0) ||
                    (utc.year % 400 == 0);
  constexpr std::array<int, 12> kDays{{31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31}};
  const int maximum_day =
      kDays[static_cast<std::size_t>(utc.month - 1)] +
      (utc.month == 2 && leap ? 1 : 0);
  return utc.day >= 1 && utc.day <= maximum_day;
}

std::array<double, 3>
worldDirection(double azimuth_degrees, double altitude_degrees,
               double north_offset_degrees) noexcept {
  const double azimuth =
      (azimuth_degrees + north_offset_degrees) * kPi / 180.0;
  const double altitude = altitude_degrees * kPi / 180.0;
  const double horizontal = std::cos(altitude);
  return {horizontal * std::sin(azimuth), std::sin(altitude),
          horizontal * std::cos(azimuth)};
}

bool bodyState(astro_body_t body, astro_time_t &time,
               astro_observer_t observer,
               double north_offset_degrees,
               CelestialBodyState &out) noexcept {
  const astro_equatorial_t equatorial =
      Astronomy_Equator(body, &time, observer, EQUATOR_OF_DATE, ABERRATION);
  if (equatorial.status != ASTRO_SUCCESS) {
    return false;
  }
  const astro_horizon_t geometric =
      Astronomy_Horizon(&time, observer, equatorial.ra, equatorial.dec,
                        REFRACTION_NONE);
  const astro_horizon_t apparent =
      Astronomy_Horizon(&time, observer, equatorial.ra, equatorial.dec,
                        REFRACTION_NORMAL);
  if (!std::isfinite(apparent.azimuth) ||
      !std::isfinite(apparent.altitude) ||
      !std::isfinite(geometric.altitude)) {
    return false;
  }
  out.azimuth_degrees = apparent.azimuth;
  out.apparent_altitude_degrees = apparent.altitude;
  out.geometric_altitude_degrees = geometric.altitude;
  out.direction = worldDirection(apparent.azimuth, apparent.altitude,
                                 north_offset_degrees);
  return true;
}

TwilightPhase twilightFromSunAltitude(double altitude_degrees) noexcept {
  if (altitude_degrees >= 0.0) {
    return TwilightPhase::Day;
  }
  if (altitude_degrees >= -0.833) {
    return TwilightPhase::SunriseSunset;
  }
  if (altitude_degrees >= -6.0) {
    return TwilightPhase::Civil;
  }
  if (altitude_degrees >= -12.0) {
    return TwilightPhase::Nautical;
  }
  if (altitude_degrees >= -18.0) {
    return TwilightPhase::Astronomical;
  }
  return TwilightPhase::Night;
}

double luminance(float r, float g, float b) noexcept {
  return 0.2126 * static_cast<double>(r) +
         0.7152 * static_cast<double>(g) +
         0.0722 * static_cast<double>(b);
}

bool validDensityProfile(const AtmosphereDensityProfile &profile) noexcept {
  for (const AtmosphereDensityLayer &layer : profile.layers) {
    if (!std::isfinite(layer.width_km) || layer.width_km < 0.0 ||
        !std::isfinite(layer.exp_term) ||
        !std::isfinite(layer.exp_scale_per_km) ||
        !std::isfinite(layer.linear_term_per_km) ||
        !std::isfinite(layer.constant_term)) {
      return false;
    }
  }
  return true;
}

template <std::size_t Size>
bool finiteRange(const std::array<double, Size> &values, double minimum,
                 double maximum) noexcept {
  for (double value : values) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
      return false;
    }
  }
  return true;
}

void appendHex64(std::string &out, std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(digits[(value >> shift) & 0xfu]);
  }
}

void appendKeyUnsigned(std::string &out, std::uint64_t value) {
  appendHex64(out, value);
  out.push_back(';');
}

void appendKeyDouble(std::string &out, double value) {
  appendHex64(out, std::bit_cast<std::uint64_t>(value));
  out.push_back(';');
}

template <std::size_t Size>
void appendKeyArray(std::string &out, const std::array<double, Size> &values) {
  for (double value : values) {
    appendKeyDouble(out, value);
  }
}

void appendKeyProfile(std::string &out,
                      const AtmosphereDensityProfile &profile) {
  for (const AtmosphereDensityLayer &layer : profile.layers) {
    appendKeyDouble(out, layer.width_km);
    appendKeyDouble(out, layer.exp_term);
    appendKeyDouble(out, layer.exp_scale_per_km);
    appendKeyDouble(out, layer.linear_term_per_km);
    appendKeyDouble(out, layer.constant_term);
  }
}

bool checkedAdd(std::uint64_t &total, std::uint64_t value) noexcept;
bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t &out) noexcept;
bool estimateHdriRuntimeBudget(
    const FloatEnvironmentImage &source, std::uint32_t requested_width,
    std::uint32_t resolved_width, const HdriRuntimeBudgetLimits &limits,
    HdriRuntimeBudget &out) noexcept;

} // namespace

const char *twilightPhaseName(TwilightPhase phase) noexcept {
  switch (phase) {
  case TwilightPhase::Day:
    return "day";
  case TwilightPhase::SunriseSunset:
    return "sunrise-sunset";
  case TwilightPhase::Civil:
    return "civil";
  case TwilightPhase::Nautical:
    return "nautical";
  case TwilightPhase::Astronomical:
    return "astronomical";
  case TwilightPhase::Night:
  default:
    return "night";
  }
}

std::uint32_t AtmosphereLutDimensions::scatteringWidth() const noexcept {
  const std::uint64_t width =
      static_cast<std::uint64_t>(scattering_sun_cosine) *
      scattering_relative_azimuth;
  return width <= std::numeric_limits<std::uint32_t>::max()
             ? static_cast<std::uint32_t>(width)
             : 0u;
}

bool BrunetonAtmosphereConfig::valid() const noexcept {
  const AtmospherePhysicalParameters &p = physical;
  const AtmosphereLutDimensions &d = dimensions;
  const bool dimensions_valid =
      d.transmittance_width > 0u && d.transmittance_height > 0u &&
      d.scattering_radial > 0u && d.scattering_view_cosine >= 2u &&
      (d.scattering_view_cosine % 2u) == 0u &&
      d.scattering_sun_cosine > 0u &&
      d.scattering_relative_azimuth >= 2u &&
      d.scatteringWidth() > 0u && d.irradiance_width > 0u &&
      d.irradiance_height > 0u;
  return implementation_revision > 0u &&
         format == AtmosphereLutFormat::Rgba16Float &&
         scattering_orders >= 2u && scattering_orders <= 16u &&
         dimensions_valid && finiteRange(p.solar_irradiance, 0.0, 1.0e6) &&
         p.solar_irradiance[0] > 0.0 && p.solar_irradiance[1] > 0.0 &&
         p.solar_irradiance[2] > 0.0 &&
         std::isfinite(p.sun_angular_radius_radians) &&
         p.sun_angular_radius_radians > 0.0 &&
         p.sun_angular_radius_radians < 0.1 &&
         std::isfinite(p.bottom_radius_km) && p.bottom_radius_km > 0.0 &&
         std::isfinite(p.top_radius_km) &&
         p.top_radius_km > p.bottom_radius_km &&
         validDensityProfile(p.rayleigh_density) &&
         finiteRange(p.rayleigh_scattering_per_km, 0.0, 1.0e3) &&
         validDensityProfile(p.mie_density) &&
         finiteRange(p.mie_scattering_per_km, 0.0, 1.0e3) &&
         finiteRange(p.mie_extinction_per_km, 0.0, 1.0e3) &&
         p.mie_extinction_per_km[0] >= p.mie_scattering_per_km[0] &&
         p.mie_extinction_per_km[1] >= p.mie_scattering_per_km[1] &&
         p.mie_extinction_per_km[2] >= p.mie_scattering_per_km[2] &&
         std::isfinite(p.mie_phase_function_g) &&
         p.mie_phase_function_g > -1.0 && p.mie_phase_function_g < 1.0 &&
         validDensityProfile(p.absorption_density) &&
         finiteRange(p.absorption_extinction_per_km, 0.0, 1.0e3) &&
         finiteRange(p.ground_albedo, 0.0, 1.0) &&
         std::isfinite(p.minimum_sun_cosine) &&
         p.minimum_sun_cosine >= -1.0 && p.minimum_sun_cosine <= 1.0;
}

BrunetonAtmosphereConfig defaultEarthAtmosphereConfig() noexcept {
  BrunetonAtmosphereConfig config;
  AtmospherePhysicalParameters &p = config.physical;
  p.solar_irradiance = {1.474, 1.8504, 1.91198};
  p.sun_angular_radius_radians = 0.00935 / 2.0;
  p.bottom_radius_km = 6360.0;
  p.top_radius_km = 6420.0;
  p.rayleigh_density.layers = {{
      {0.0, 0.0, 0.0, 0.0, 0.0},
      {0.0, 1.0, -1.0 / 8.0, 0.0, 0.0},
  }};
  p.rayleigh_scattering_per_km = {
      0.0058023393817123806,
      0.013557762447920219,
      0.033100005976367732,
  };
  p.mie_density.layers = {{
      {0.0, 0.0, 0.0, 0.0, 0.0},
      {0.0, 1.0, -1.0 / 1.2, 0.0, 0.0},
  }};
  p.mie_scattering_per_km = {0.003996, 0.003996, 0.003996};
  p.mie_extinction_per_km = {0.00444, 0.00444, 0.00444};
  p.mie_phase_function_g = 0.8;
  p.absorption_density.layers = {{
      {25.0, 0.0, 0.0, 1.0 / 15.0, -2.0 / 3.0},
      {0.0, 0.0, 0.0, -1.0 / 15.0, 8.0 / 3.0},
  }};
  p.absorption_extinction_per_km = {
      0.0006497166,
      0.0018809,
      0.00008501668,
  };
  p.ground_albedo = {0.1, 0.1, 0.1};
  p.minimum_sun_cosine = std::cos(102.0 * kPi / 180.0);
  return config;
}

std::string
brunetonAtmosphereCacheKey(const BrunetonAtmosphereConfig &config) {
  if (!config.valid()) {
    return {};
  }

  constexpr const char *kUpstreamCommit =
      "34f14e745cff948f4ca3157d1b62a445ffa7286f";
  constexpr const char *kDefinitionsSha256 =
      "6682de618a277143bba2643830ad9afd6b915f19e7821ab418afd7b6f8dd6c92";
  constexpr const char *kFunctionsSha256 =
      "bdfbe3ba3d60a4879ae34392a28d8917f10338b462f5adda46625d48a6c47597";
  constexpr const char *kWrapperRevision =
      "xpbd-vulkan-compute-radiance-rgb-clouds-v2";
  constexpr const char *kLocalShaderBundleSha256 =
      "00ece24bdfced3bcb6f9934e369c7eb945e0bf59460a850783f845f09c1810dc";

  std::string key = "xpbd-bruneton|";
  key += kUpstreamCommit;
  key.push_back('|');
  key += kDefinitionsSha256;
  key.push_back('|');
  key += kFunctionsSha256;
  key.push_back('|');
  key += kWrapperRevision;
  key.push_back('|');
  key += kLocalShaderBundleSha256;
  key.push_back('|');

  appendKeyUnsigned(key, config.implementation_revision);
  appendKeyUnsigned(key, static_cast<std::uint8_t>(config.format));
  appendKeyUnsigned(key, config.scattering_orders);
  appendKeyUnsigned(key, config.dimensions.transmittance_width);
  appendKeyUnsigned(key, config.dimensions.transmittance_height);
  appendKeyUnsigned(key, config.dimensions.scattering_radial);
  appendKeyUnsigned(key, config.dimensions.scattering_view_cosine);
  appendKeyUnsigned(key, config.dimensions.scattering_sun_cosine);
  appendKeyUnsigned(key, config.dimensions.scattering_relative_azimuth);
  appendKeyUnsigned(key, config.dimensions.irradiance_width);
  appendKeyUnsigned(key, config.dimensions.irradiance_height);

  const AtmospherePhysicalParameters &p = config.physical;
  appendKeyArray(key, p.solar_irradiance);
  appendKeyDouble(key, p.sun_angular_radius_radians);
  appendKeyDouble(key, p.bottom_radius_km);
  appendKeyDouble(key, p.top_radius_km);
  appendKeyProfile(key, p.rayleigh_density);
  appendKeyArray(key, p.rayleigh_scattering_per_km);
  appendKeyProfile(key, p.mie_density);
  appendKeyArray(key, p.mie_scattering_per_km);
  appendKeyArray(key, p.mie_extinction_per_km);
  appendKeyDouble(key, p.mie_phase_function_g);
  appendKeyProfile(key, p.absorption_density);
  appendKeyArray(key, p.absorption_extinction_per_km);
  appendKeyArray(key, p.ground_albedo);
  appendKeyDouble(key, p.minimum_sun_cosine);
  return key;
}

bool VolumetricCloudState::valid() const noexcept {
  const bool finite_values =
      std::isfinite(coverage) && std::isfinite(density) &&
      std::isfinite(base_altitude_km) && std::isfinite(thickness_km) &&
      std::isfinite(wind_direction[0]) &&
      std::isfinite(wind_direction[1]) &&
      std::isfinite(wind_speed_km_per_hour) &&
      std::isfinite(shadow_strength) && std::isfinite(weather_scale) &&
      std::isfinite(weather_offset_km[0]) &&
      std::isfinite(weather_offset_km[1]) &&
      std::isfinite(base_shape_scale) && std::isfinite(detail_scale) &&
      std::isfinite(erosion) && std::isfinite(forward_scattering) &&
      std::isfinite(silver_lining) && std::isfinite(absorption) &&
      std::isfinite(multiple_scattering) && std::isfinite(render_ratio) &&
      std::isfinite(history_weight) && std::isfinite(lighting_strength) &&
      std::isfinite(time_seconds);
  if (!finite_values) {
    return false;
  }
  if (!enabled) {
    return true;
  }
  return coverage >= 0.0f && coverage <= 1.0f && density > 0.0f &&
         density <= 8.0f && base_altitude_km >= 0.1f &&
         base_altitude_km <= 20.0f && thickness_km >= 0.1f &&
         thickness_km <= 20.0f &&
         base_altitude_km + thickness_km <= 30.0f &&
         std::abs(wind_direction[0]) <= 1000.0f &&
         std::abs(wind_direction[1]) <= 1000.0f &&
         std::abs(wind_speed_km_per_hour) <= 1000.0f &&
         shadow_strength >= 0.0f && shadow_strength <= 1.0f &&
         weather_scale >= 0.05f && weather_scale <= 20.0f &&
         std::abs(weather_offset_km[0]) <= 1.0e6f &&
         std::abs(weather_offset_km[1]) <= 1.0e6f &&
         base_shape_scale >= 0.05f && base_shape_scale <= 20.0f &&
         detail_scale >= 0.05f && detail_scale <= 20.0f &&
         erosion >= 0.0f && erosion <= 1.0f &&
         forward_scattering >= -0.95f &&
         forward_scattering <= 0.95f &&
         silver_lining >= 0.0f && silver_lining <= 4.0f &&
         absorption >= 0.01f && absorption <= 8.0f &&
         multiple_scattering >= 0.0f && multiple_scattering <= 2.0f &&
         render_ratio >= 0.25f && render_ratio <= 1.0f &&
         history_weight >= 0.0f && history_weight <= 0.999f &&
         lighting_strength >= 0.0f && lighting_strength <= 8.0f &&
         shadow_resolution >= 64u && shadow_resolution <= 4096u &&
         std::abs(time_seconds) <= 1.0e7f && ray_steps >= 8u &&
         ray_steps <= 128u && light_steps >= 1u && light_steps <= 16u;
}

std::string volumetricCloudCacheKey(const VolumetricCloudState &state) {
  if (!state.valid()) {
    return {};
  }
  std::string key = "xpbd-volumetric-cloud-v2|";
  appendKeyUnsigned(key, state.enabled ? 1u : 0u);
  appendKeyDouble(key, static_cast<double>(state.coverage));
  appendKeyDouble(key, static_cast<double>(state.density));
  appendKeyDouble(key, static_cast<double>(state.base_altitude_km));
  appendKeyDouble(key, static_cast<double>(state.thickness_km));
  appendKeyDouble(key, static_cast<double>(state.wind_direction[0]));
  appendKeyDouble(key, static_cast<double>(state.wind_direction[1]));
  appendKeyDouble(key, static_cast<double>(state.wind_speed_km_per_hour));
  appendKeyDouble(key, static_cast<double>(state.shadow_strength));
  appendKeyUnsigned(key, static_cast<std::uint32_t>(state.quality));
  appendKeyDouble(key, static_cast<double>(state.weather_scale));
  appendKeyDouble(key, static_cast<double>(state.weather_offset_km[0]));
  appendKeyDouble(key, static_cast<double>(state.weather_offset_km[1]));
  appendKeyDouble(key, static_cast<double>(state.base_shape_scale));
  appendKeyDouble(key, static_cast<double>(state.detail_scale));
  appendKeyDouble(key, static_cast<double>(state.erosion));
  appendKeyDouble(key, static_cast<double>(state.forward_scattering));
  appendKeyDouble(key, static_cast<double>(state.silver_lining));
  appendKeyDouble(key, static_cast<double>(state.absorption));
  appendKeyDouble(key, static_cast<double>(state.multiple_scattering));
  appendKeyDouble(key, static_cast<double>(state.render_ratio));
  appendKeyUnsigned(key, state.reprojection ? 1u : 0u);
  appendKeyDouble(key, static_cast<double>(state.history_weight));
  appendKeyDouble(key, static_cast<double>(state.lighting_strength));
  appendKeyUnsigned(key, state.shadow_resolution);
  appendKeyDouble(key, static_cast<double>(state.time_seconds));
  appendKeyUnsigned(key, state.seed);
  appendKeyUnsigned(key, state.ray_steps);
  appendKeyUnsigned(key, state.light_steps);
  appendKeyUnsigned(key, state.temporal_frame);
  appendKeyUnsigned(key, state.generation);
  return key;
}

bool computeCelestialState(const UtcDateTime &utc,
                           const ObserverLocation &observer,
                           CelestialState &out, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!validUtc(utc) || !validObserver(observer)) {
    setError(error, "invalid UTC or observer");
    return false;
  }

  astro_time_t time = Astronomy_MakeTime(
      utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second);
  if (!std::isfinite(time.ut) || !std::isfinite(time.tt)) {
    setError(error, "Astronomy Engine rejected UTC");
    return false;
  }
  const astro_observer_t astro_observer = Astronomy_MakeObserver(
      observer.latitude_degrees, observer.longitude_degrees,
      observer.elevation_meters);

  CelestialState candidate{};
  candidate.utc = utc;
  candidate.observer = observer;
  if (!bodyState(BODY_SUN, time, astro_observer,
                 observer.north_offset_degrees, candidate.sun) ||
      !bodyState(BODY_MOON, time, astro_observer,
                 observer.north_offset_degrees, candidate.moon)) {
    setError(error, "topocentric Sun/Moon calculation failed");
    return false;
  }

  const astro_vector_t sun_vector =
      Astronomy_GeoVector(BODY_SUN, time, ABERRATION);
  const double sun_distance_au = Astronomy_VectorLength(sun_vector);
  if (sun_vector.status != ASTRO_SUCCESS ||
      !(sun_distance_au > kSolarRadiusAu)) {
    setError(error, "solar distance calculation failed");
    return false;
  }
  candidate.sun.angular_diameter_degrees =
      2.0 * std::asin(kSolarRadiusAu / sun_distance_au) * 180.0 / kPi;

  const astro_illum_t illumination = Astronomy_Illumination(BODY_MOON, time);
  const astro_libration_t libration = Astronomy_Libration(time);
  candidate.moon_distance_km = libration.dist_km;
  candidate.moon.angular_diameter_degrees = libration.diam_deg;
  candidate.moon_phase_angle_degrees = illumination.phase_angle;
  candidate.moon_illuminated_fraction = illumination.phase_fraction;
  candidate.moon_magnitude = illumination.mag;
  candidate.moon_libration_latitude_degrees = libration.elat;
  candidate.moon_libration_longitude_degrees = libration.elon;
  candidate.sidereal_time_hours = Astronomy_SiderealTime(&time);
  candidate.twilight =
      twilightFromSunAltitude(candidate.sun.geometric_altitude_degrees);

  const auto finite_body = [](const CelestialBodyState &body) {
    return std::isfinite(body.direction[0]) &&
           std::isfinite(body.direction[1]) &&
           std::isfinite(body.direction[2]) &&
           std::isfinite(body.azimuth_degrees) &&
           std::isfinite(body.apparent_altitude_degrees) &&
           std::isfinite(body.geometric_altitude_degrees) &&
           body.angular_diameter_degrees > 0.0 &&
           std::isfinite(body.angular_diameter_degrees);
  };
  if (illumination.status != ASTRO_SUCCESS ||
      !finite_body(candidate.sun) || !finite_body(candidate.moon) ||
      !(candidate.moon_distance_km > 0.0) ||
      !std::isfinite(candidate.moon_phase_angle_degrees) ||
      !std::isfinite(candidate.moon_illuminated_fraction) ||
      candidate.moon_illuminated_fraction < 0.0 ||
      candidate.moon_illuminated_fraction > 1.0 ||
      !std::isfinite(candidate.moon_magnitude) ||
      !std::isfinite(candidate.moon_libration_latitude_degrees) ||
      !std::isfinite(candidate.moon_libration_longitude_degrees) ||
      !std::isfinite(candidate.sidereal_time_hours)) {
    setError(error, "non-finite celestial state");
    return false;
  }

  candidate.valid = true;
  out = candidate;
  return true;
}

bool shiftUtcDateTime(const UtcDateTime &utc, double offset_seconds,
                      UtcDateTime &out, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!validUtc(utc) || !std::isfinite(offset_seconds) ||
      std::abs(offset_seconds) > 366.0 * 24.0 * 3600.0) {
    setError(error, "invalid UTC shift");
    return false;
  }
  using namespace std::chrono;
  const year_month_day date{year{utc.year},
                            month{static_cast<unsigned>(utc.month)},
                            day{static_cast<unsigned>(utc.day)}};
  if (!date.ok()) {
    setError(error, "invalid UTC date");
    return false;
  }
  const auto from_midnight =
      duration_cast<milliseconds>(hours{utc.hour} + minutes{utc.minute}) +
      milliseconds{static_cast<std::int64_t>(
          std::llround(utc.second * 1000.0))};
  const auto shifted =
      sys_time<milliseconds>{sys_days{date}} + from_midnight +
      milliseconds{static_cast<std::int64_t>(
          std::llround(offset_seconds * 1000.0))};
  const auto shifted_day = floor<days>(shifted);
  const year_month_day shifted_date{shifted_day};
  if (!shifted_date.ok()) {
    setError(error, "shifted UTC date is invalid");
    return false;
  }
  const hh_mm_ss time_of_day{shifted - shifted_day};
  UtcDateTime candidate;
  candidate.year = static_cast<int>(shifted_date.year());
  candidate.month =
      static_cast<int>(static_cast<unsigned>(shifted_date.month()));
  candidate.day =
      static_cast<int>(static_cast<unsigned>(shifted_date.day()));
  candidate.hour = static_cast<int>(time_of_day.hours().count());
  candidate.minute = static_cast<int>(time_of_day.minutes().count());
  candidate.second =
      static_cast<double>(time_of_day.seconds().count()) +
      static_cast<double>(
          duration_cast<milliseconds>(time_of_day.subseconds()).count()) /
          1000.0;
  if (!validUtc(candidate)) {
    setError(error, "shifted UTC is outside the supported range");
    return false;
  }
  out = candidate;
  return true;
}

bool applyCelestialSunAngleOffsets(CelestialState &state,
                                   double azimuth_offset_degrees,
                                   double altitude_offset_degrees,
                                   std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!state.valid || !std::isfinite(azimuth_offset_degrees) ||
      !std::isfinite(altitude_offset_degrees) ||
      azimuth_offset_degrees < -360.0 || azimuth_offset_degrees > 360.0 ||
      altitude_offset_degrees < -90.0 || altitude_offset_degrees > 90.0) {
    setError(error, "invalid celestial Sun angle offset");
    return false;
  }
  const double azimuth =
      std::fmod(state.sun.azimuth_degrees + azimuth_offset_degrees, 360.0);
  const double wrapped_azimuth = azimuth < 0.0 ? azimuth + 360.0 : azimuth;
  const double apparent_altitude = std::clamp(
      state.sun.apparent_altitude_degrees + altitude_offset_degrees, -90.0,
      90.0);
  const double geometric_altitude = std::clamp(
      state.sun.geometric_altitude_degrees + altitude_offset_degrees, -90.0,
      90.0);
  const auto direction = worldDirection(
      wrapped_azimuth, apparent_altitude, state.observer.north_offset_degrees);
  if (!std::isfinite(direction[0]) || !std::isfinite(direction[1]) ||
      !std::isfinite(direction[2])) {
    setError(error, "celestial Sun angle produced a non-finite direction");
    return false;
  }
  state.sun.azimuth_degrees = wrapped_azimuth;
  state.sun.apparent_altitude_degrees = apparent_altitude;
  state.sun.geometric_altitude_degrees = geometric_altitude;
  state.sun.direction = direction;
  state.twilight = twilightFromSunAltitude(geometric_altitude);
  return true;
}

bool applyCelestialMoonAngleOffsets(CelestialState &state,
                                    double azimuth_offset_degrees,
                                    double altitude_offset_degrees,
                                    std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!state.valid || !std::isfinite(azimuth_offset_degrees) ||
      !std::isfinite(altitude_offset_degrees) ||
      azimuth_offset_degrees < -360.0 || azimuth_offset_degrees > 360.0 ||
      altitude_offset_degrees < -90.0 || altitude_offset_degrees > 90.0) {
    setError(error, "invalid celestial Moon angle offset");
    return false;
  }
  const double azimuth =
      std::fmod(state.moon.azimuth_degrees + azimuth_offset_degrees, 360.0);
  const double wrapped_azimuth = azimuth < 0.0 ? azimuth + 360.0 : azimuth;
  const double apparent_altitude = std::clamp(
      state.moon.apparent_altitude_degrees + altitude_offset_degrees, -90.0,
      90.0);
  const double geometric_altitude = std::clamp(
      state.moon.geometric_altitude_degrees + altitude_offset_degrees, -90.0,
      90.0);
  const auto direction = worldDirection(
      wrapped_azimuth, apparent_altitude, state.observer.north_offset_degrees);
  if (!std::isfinite(direction[0]) || !std::isfinite(direction[1]) ||
      !std::isfinite(direction[2])) {
    setError(error, "celestial Moon angle produced a non-finite direction");
    return false;
  }
  state.moon.azimuth_degrees = wrapped_azimuth;
  state.moon.apparent_altitude_degrees = apparent_altitude;
  state.moon.geometric_altitude_degrees = geometric_altitude;
  state.moon.direction = direction;
  return true;
}

bool decodeRadianceHdr(std::span<const std::uint8_t> encoded,
                       FloatEnvironmentImage &out, std::string *error,
                       HdrDecodeLimits limits) {
  if (error != nullptr) {
    error->clear();
  }
  if (encoded.empty() ||
      encoded.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    setError(error, "empty or oversized HDR input");
    return false;
  }
  const int encoded_size = static_cast<int>(encoded.size());
  if (stbi_is_hdr_from_memory(encoded.data(), encoded_size) == 0) {
    setError(error, "input is not Radiance HDR");
    return false;
  }

  int width = 0;
  int height = 0;
  int source_channels = 0;
  if (stbi_info_from_memory(encoded.data(), encoded_size, &width, &height,
                            &source_channels) == 0 ||
      width <= 0 || height <= 0 ||
      width != height * 2 ||
      static_cast<std::uint32_t>(width) > limits.maximum_width ||
      static_cast<std::uint32_t>(height) > limits.maximum_height) {
    setError(error, "HDR must be a supported 2:1 lat-long image");
    return false;
  }
  const std::size_t pixels =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (pixels >
      (std::numeric_limits<std::size_t>::max)() /
          (std::size_t{4} * sizeof(float)) ||
      pixels * std::size_t{4} * sizeof(float) >
          limits.maximum_decoded_bytes) {
    setError(error, "HDR exceeds the decoded-memory budget");
    return false;
  }

  using StbiFloatImage = std::unique_ptr<float, decltype(&stbi_image_free)>;
  StbiFloatImage decoded(
      stbi_loadf_from_memory(encoded.data(), encoded_size, &width, &height,
                             &source_channels, 4),
      &stbi_image_free);
  if (!decoded) {
    const char *reason = stbi_failure_reason();
    setError(error, reason != nullptr ? reason : "HDR decode failed");
    return false;
  }

  FloatEnvironmentImage candidate;
  candidate.width = static_cast<std::uint32_t>(width);
  candidate.height = static_cast<std::uint32_t>(height);
  try {
    candidate.rgba.assign(decoded.get(),
                          decoded.get() + pixels * std::size_t{4});
  } catch (const std::bad_alloc &) {
    setError(error, "HDR decoded-image allocation failed");
    return false;
  } catch (...) {
    setError(error, "HDR decoded-image construction raised an exception");
    return false;
  }
  decoded.reset();

  bool nonempty_radiance = false;
  for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
    for (std::size_t channel = 0; channel < 3u; ++channel) {
      const float value = candidate.rgba[pixel * 4u + channel];
      if (!std::isfinite(value) || value < 0.0f) {
        setError(error, "HDR contains invalid radiance");
        return false;
      }
      nonempty_radiance = nonempty_radiance || value > 0.0f;
    }
    candidate.rgba[pixel * 4u + 3u] = 1.0f;
  }
  if (!nonempty_radiance) {
    setError(error, "HDR contains no radiance");
    return false;
  }

  out = std::move(candidate);
  return true;
}

bool AliasTable::build(std::span<const double> weights) {
  clear();
  if (weights.empty() ||
      weights.size() >
          static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
    return false;
  }

  double total = 0.0;
  for (double weight : weights) {
    if (std::isfinite(weight) && weight > 0.0) {
      total += weight;
    }
  }
  if (!(total > 0.0) || !std::isfinite(total)) {
    return false;
  }

  const std::size_t count = weights.size();
  pmf_.resize(count, 0.0);
  accept_.resize(count, 1.0);
  alias_.resize(count, 0u);
  std::vector<double> scaled(count, 0.0);
  std::vector<std::uint32_t> small;
  std::vector<std::uint32_t> large;
  small.reserve(count);
  large.reserve(count);

  for (std::size_t index = 0; index < count; ++index) {
    const double weight =
        std::isfinite(weights[index]) && weights[index] > 0.0
            ? weights[index]
            : 0.0;
    pmf_[index] = weight / total;
    scaled[index] = pmf_[index] * static_cast<double>(count);
    alias_[index] = static_cast<std::uint32_t>(index);
    (scaled[index] < 1.0 ? small : large)
        .push_back(static_cast<std::uint32_t>(index));
  }

  while (!small.empty() && !large.empty()) {
    const std::uint32_t below = small.back();
    small.pop_back();
    const std::uint32_t above = large.back();
    large.pop_back();
    accept_[below] = std::clamp(scaled[below], 0.0, 1.0);
    alias_[below] = above;
    scaled[above] = (scaled[above] + scaled[below]) - 1.0;
    (scaled[above] < 1.0 ? small : large).push_back(above);
  }
  for (std::uint32_t index : large) {
    accept_[index] = 1.0;
    alias_[index] = index;
  }
  for (std::uint32_t index : small) {
    accept_[index] = 1.0;
    alias_[index] = index;
  }
  return true;
}

void AliasTable::clear() noexcept {
  accept_.clear();
  pmf_.clear();
  alias_.clear();
}

bool AliasTable::valid() const noexcept {
  return !pmf_.empty() && accept_.size() == pmf_.size() &&
         alias_.size() == pmf_.size();
}

std::uint32_t AliasTable::sample(double column_sample,
                                 double coin_sample) const noexcept {
  if (!valid()) {
    return 0u;
  }
  const double scaled =
      clampUnitOpen(column_sample) * static_cast<double>(pmf_.size());
  const std::size_t column =
      std::min(static_cast<std::size_t>(scaled), pmf_.size() - 1u);
  return clampUnitOpen(coin_sample) < accept_[column]
             ? static_cast<std::uint32_t>(column)
             : alias_[column];
}

double AliasTable::probability(std::size_t index) const noexcept {
  return index < pmf_.size() ? pmf_[index] : 0.0;
}

bool EnvironmentDistribution::build(const FloatEnvironmentImage &image) {
  clear();
  if (!image.valid()) {
    return false;
  }
  std::vector<double> weights(
      static_cast<std::size_t>(image.width) * image.height, 0.0);
  width_ = image.width;
  height_ = image.height;
  for (std::uint32_t y = 0; y < height_; ++y) {
    const double solid_angle = texelSolidAngle(y);
    for (std::uint32_t x = 0; x < width_; ++x) {
      const std::size_t pixel =
          static_cast<std::size_t>(y) * width_ + x;
      const float *rgba = image.rgba.data() + pixel * 4u;
      const double value = luminance(rgba[0], rgba[1], rgba[2]);
      weights[pixel] =
          std::isfinite(value) && value > 0.0 ? value * solid_angle : 0.0;
    }
  }
  if (!alias_.build(weights)) {
    clear();
    return false;
  }
  return true;
}

void EnvironmentDistribution::clear() noexcept {
  width_ = 0;
  height_ = 0;
  alias_.clear();
}

bool EnvironmentDistribution::valid() const noexcept {
  return width_ > 0u && height_ > 0u && alias_.valid() &&
         alias_.size() == static_cast<std::size_t>(width_) * height_;
}

double EnvironmentDistribution::texelProbability(
    std::uint32_t x, std::uint32_t y) const noexcept {
  if (!valid() || x >= width_ || y >= height_) {
    return 0.0;
  }
  return alias_.probability(static_cast<std::size_t>(y) * width_ + x);
}

double EnvironmentDistribution::aliasAcceptance(
    std::uint32_t x, std::uint32_t y) const noexcept {
  if (!valid() || x >= width_ || y >= height_) {
    return 0.0;
  }
  return alias_.acceptance(static_cast<std::size_t>(y) * width_ + x);
}

std::uint32_t EnvironmentDistribution::aliasIndex(
    std::uint32_t x, std::uint32_t y) const noexcept {
  if (!valid() || x >= width_ || y >= height_) {
    return 0u;
  }
  return alias_.aliasIndex(static_cast<std::size_t>(y) * width_ + x);
}

double
EnvironmentDistribution::texelSolidAngle(std::uint32_t y) const noexcept {
  if (width_ == 0u || height_ == 0u || y >= height_) {
    return 0.0;
  }
  const double theta0 =
      kPi * static_cast<double>(y) / static_cast<double>(height_);
  const double theta1 =
      kPi * static_cast<double>(y + 1u) / static_cast<double>(height_);
  return (kTwoPi / static_cast<double>(width_)) *
         (std::cos(theta0) - std::cos(theta1));
}

EnvironmentDirectionSample EnvironmentDistribution::sample(
    double column_sample, double coin_sample, double jitter_u,
    double jitter_v, double rotation_radians) const noexcept {
  EnvironmentDirectionSample result;
  if (!valid()) {
    return result;
  }
  const std::uint32_t index =
      alias_.sample(column_sample, coin_sample);
  result.texel_x = index % width_;
  result.texel_y = index / width_;
  const double u =
      (static_cast<double>(result.texel_x) + clampUnitOpen(jitter_u)) /
      static_cast<double>(width_);
  const double v =
      (static_cast<double>(result.texel_y) + clampUnitOpen(jitter_v)) /
      static_cast<double>(height_);
  const double phi = kTwoPi * u + rotation_radians;
  const double theta = kPi * v;
  const double sin_theta = std::sin(theta);
  result.direction = {sin_theta * std::sin(phi), std::cos(theta),
                      sin_theta * std::cos(phi)};
  const double solid_angle = texelSolidAngle(result.texel_y);
  result.solid_angle_pdf =
      solid_angle > 0.0 ? alias_.probability(index) / solid_angle : 0.0;
  result.valid = std::isfinite(result.solid_angle_pdf) &&
                 result.solid_angle_pdf > 0.0;
  return result;
}

double EnvironmentDistribution::solidAnglePdf(
    const std::array<double, 3> &direction,
    double rotation_radians) const noexcept {
  if (!valid() || !std::isfinite(direction[0]) ||
      !std::isfinite(direction[1]) || !std::isfinite(direction[2])) {
    return 0.0;
  }
  const double length = std::sqrt(direction[0] * direction[0] +
                                  direction[1] * direction[1] +
                                  direction[2] * direction[2]);
  if (!(length > 0.0) || !std::isfinite(length)) {
    return 0.0;
  }
  const double x = direction[0] / length;
  const double y = std::clamp(direction[1] / length, -1.0, 1.0);
  const double z = direction[2] / length;
  const double theta = std::acos(y);
  const double phi = wrapRadians(std::atan2(x, z) - rotation_radians);
  const std::uint32_t texel_x = std::min(
      static_cast<std::uint32_t>(
          phi / kTwoPi * static_cast<double>(width_)),
      width_ - 1u);
  const std::uint32_t texel_y = std::min(
      static_cast<std::uint32_t>(
          theta / kPi * static_cast<double>(height_)),
      height_ - 1u);
  const double solid_angle = texelSolidAngle(texel_y);
  return solid_angle > 0.0
             ? texelProbability(texel_x, texel_y) / solid_angle
             : 0.0;
}

bool buildHdrEnvironmentAsset(
    std::span<const std::uint8_t> encoded, std::string source_identity,
    std::string checksum, std::uint64_t generation, HdrEnvironmentAsset &out,
    std::string *error, HdrDecodeLimits limits) {
  if (error != nullptr) {
    error->clear();
  }
  if (source_identity.empty() || checksum.empty()) {
    setError(error, "HDR source identity and checksum are required");
    return false;
  }
  HdrEnvironmentAsset candidate;
  candidate.source_identity = std::move(source_identity);
  candidate.checksum = std::move(checksum);
  candidate.generation = generation;
  if (!decodeRadianceHdr(encoded, candidate.radiance, error, limits)) {
    return false;
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(candidate.radiance.width) *
      candidate.radiance.height;
  // Peak while Alias construction is live: retained RGBA32F (16 B/texel),
  // persistent accept/pmf/alias data (20 B/texel), temporary weights/scaled
  // arrays (16 B/texel), and the two reserved partition lists (8 B/texel).
  std::uint64_t asset_peak_bytes = 0u;
  if (!checkedMultiply(pixels, 60u, asset_peak_bytes) ||
      !checkedAdd(asset_peak_bytes, limits.asset_safety_margin_bytes) ||
      asset_peak_bytes > limits.maximum_asset_peak_bytes) {
    setError(error, "HDR asset exceeds the distribution-build peak budget");
    return false;
  }
  try {
    if (!candidate.distribution.build(candidate.radiance)) {
      setError(error, "HDR environment distribution is empty");
      return false;
    }
  } catch (const std::bad_alloc &) {
    setError(error, "HDR environment distribution allocation failed");
    return false;
  } catch (...) {
    setError(error, "HDR environment distribution raised an exception");
    return false;
  }
  out = std::move(candidate);
  return true;
}

bool resolveHdriRuntimeBudget(
    const FloatEnvironmentImage &source, std::uint32_t requested_width,
    HdriRuntimeBudget &out, std::string *error,
    HdriRuntimeBudgetLimits limits) {
  if (error != nullptr) {
    error->clear();
  }
  if (!source.valid() || source.width != source.height * 2u) {
    setError(error, "HDR runtime source must be a valid 2:1 image");
    return false;
  }
  if (requested_width < 2u || limits.maximum_runtime_width < 2u ||
      limits.maximum_total_bytes == 0u || limits.maximum_gpu_bytes == 0u) {
    setError(error, "HDR runtime limits or requested width are invalid");
    return false;
  }

  std::uint32_t resolved_width =
      std::min({requested_width, source.width,
                limits.maximum_runtime_width});
  resolved_width &= ~std::uint32_t{1};
  if (resolved_width < 2u) {
    setError(error, "HDR runtime width cannot preserve a 2:1 extent");
    return false;
  }
  const std::uint32_t requested_floor = resolved_width;
  std::uint32_t budget_floor = std::min(
      requested_floor,
      std::max(std::min(limits.minimum_runtime_width, source.width), 2u));
  budget_floor &= ~std::uint32_t{1};
  budget_floor = std::max(budget_floor, 2u);

  HdriRuntimeBudget last_estimate;
  for (;;) {
    HdriRuntimeBudget candidate;
    if (!estimateHdriRuntimeBudget(source, requested_width, resolved_width,
                                   limits, candidate)) {
      setError(error, "HDR runtime budget arithmetic overflow");
      return false;
    }
    last_estimate = candidate;
    const bool gpu_resources_fit =
        candidate.gpu_image_bytes <= limits.maximum_gpu_bytes &&
        candidate.gpu_distribution_bytes <=
            limits.maximum_gpu_bytes - candidate.gpu_image_bytes;
    if (gpu_resources_fit &&
        candidate.total_bytes <= limits.maximum_total_bytes) {
      out = candidate;
      return true;
    }
    if (resolved_width <= budget_floor) {
      break;
    }
    std::uint32_t next_width = (resolved_width >> 1u) & ~std::uint32_t{1};
    if (next_width < budget_floor) {
      next_width = budget_floor & ~std::uint32_t{1};
    }
    if (next_width < 2u || next_width >= resolved_width) {
      break;
    }
    resolved_width = next_width;
  }

  setError(error,
           "HDR runtime transaction exceeds the unified memory budget: " +
               std::to_string(last_estimate.total_bytes) + " > " +
               std::to_string(limits.maximum_total_bytes));
  return false;
}

bool resampleHdriLatLong(const FloatEnvironmentImage &source,
                         std::uint32_t resolved_width,
                         FloatEnvironmentImage &out, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!source.valid() || source.width != source.height * 2u ||
      resolved_width < 2u || (resolved_width & 1u) != 0u ||
      resolved_width > source.width) {
    setError(error,
             "HDR resample requires a valid, non-upscaled 2:1 extent");
    return false;
  }
  const std::uint32_t resolved_height = resolved_width / 2u;
  std::uint64_t texels64 = 0u;
  std::uint64_t values64 = 0u;
  if (!checkedMultiply(resolved_width, resolved_height, texels64) ||
      !checkedMultiply(texels64, 4u, values64) ||
      values64 > (std::numeric_limits<std::size_t>::max)()) {
    setError(error, "HDR resample extent overflows host addressing");
    return false;
  }

  for (std::size_t value = 0u; value < source.rgba.size(); value += 4u) {
    for (std::size_t channel = 0u; channel < 3u; ++channel) {
      const float radiance = source.rgba[value + channel];
      if (!std::isfinite(radiance) || radiance < 0.0f) {
        setError(error, "HDR resample source contains invalid radiance");
        return false;
      }
    }
  }

  FloatEnvironmentImage candidate;
  candidate.width = resolved_width;
  candidate.height = resolved_height;
  try {
    candidate.rgba.resize(static_cast<std::size_t>(values64));
  } catch (const std::bad_alloc &) {
    setError(error, "HDR resample allocation failed");
    return false;
  } catch (...) {
    setError(error, "HDR resample allocation raised an exception");
    return false;
  }

  const auto wrapped_x = [&](std::int64_t x) {
    const std::int64_t width = static_cast<std::int64_t>(source.width);
    x %= width;
    return static_cast<std::uint32_t>(x < 0 ? x + width : x);
  };
  const auto clamped_y = [&](std::int64_t y) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        y, 0, static_cast<std::int64_t>(source.height) - 1));
  };
  for (std::uint32_t y = 0u; y < resolved_height; ++y) {
    const double source_y =
        (static_cast<double>(y) + 0.5) * source.height /
            resolved_height -
        0.5;
    const std::int64_t y0i =
        static_cast<std::int64_t>(std::floor(source_y));
    const std::uint32_t y0 = clamped_y(y0i);
    const std::uint32_t y1 = clamped_y(y0i + 1);
    const double ty = source_y - std::floor(source_y);
    for (std::uint32_t x = 0u; x < resolved_width; ++x) {
      const double source_x =
          (static_cast<double>(x) + 0.5) * source.width /
              resolved_width -
          0.5;
      const std::int64_t x0i =
          static_cast<std::int64_t>(std::floor(source_x));
      const std::uint32_t x0 = wrapped_x(x0i);
      const std::uint32_t x1 = wrapped_x(x0i + 1);
      const double tx = source_x - std::floor(source_x);
      const std::size_t destination =
          (static_cast<std::size_t>(y) * resolved_width + x) * 4u;
      for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
        const auto read = [&](std::uint32_t sx, std::uint32_t sy) {
          return source.rgba[
              (static_cast<std::size_t>(sy) * source.width + sx) * 4u +
              channel];
        };
        const float p00 = read(x0, y0);
        const float p10 = read(x1, y0);
        const float p01 = read(x0, y1);
        const float p11 = read(x1, y1);
        if (!std::isfinite(p00) || p00 < 0.0f || !std::isfinite(p10) ||
            p10 < 0.0f || !std::isfinite(p01) || p01 < 0.0f ||
            !std::isfinite(p11) || p11 < 0.0f) {
          setError(error, "HDR resample source contains invalid radiance");
          return false;
        }
        const double top = static_cast<double>(p00) +
                           (static_cast<double>(p10) - p00) * tx;
        const double bottom = static_cast<double>(p01) +
                              (static_cast<double>(p11) - p01) * tx;
        const double value = top + (bottom - top) * ty;
        if (!std::isfinite(value) || value < 0.0 ||
            value > (std::numeric_limits<float>::max)()) {
          setError(error, "HDR resample produced invalid radiance");
          return false;
        }
        candidate.rgba[destination + channel] = static_cast<float>(value);
      }
      candidate.rgba[destination + 3u] = 1.0f;
    }
  }
  out = std::move(candidate);
  return true;
}

bool buildHdrEnvironmentRuntimeCandidate(
    const HdrEnvironmentAsset &source, std::uint32_t requested_width,
    std::uint64_t runtime_settings_generation,
    HdrEnvironmentRuntimeCandidate &out, std::string *error,
    HdriRuntimeBudgetLimits limits) {
  if (error != nullptr) {
    error->clear();
  }
  if (!source.valid()) {
    setError(error, "HDR runtime source asset is incomplete");
    return false;
  }
  HdrEnvironmentRuntimeCandidate candidate;
  candidate.content_generation = source.generation;
  candidate.runtime_settings_generation = runtime_settings_generation;
  if (!resolveHdriRuntimeBudget(source.radiance, requested_width,
                                candidate.budget, error, limits) ||
      !resampleHdriLatLong(source.radiance,
                           candidate.budget.resolved_width,
                           candidate.radiance, error)) {
    return false;
  }
  try {
    if (!candidate.distribution.build(candidate.radiance)) {
      setError(error, "HDR runtime distribution is empty");
      return false;
    }
  } catch (const std::bad_alloc &) {
    setError(error, "HDR runtime distribution allocation failed");
    return false;
  } catch (...) {
    setError(error, "HDR runtime distribution raised an exception");
    return false;
  }
  if (!candidate.valid()) {
    setError(error, "HDR runtime candidate is internally inconsistent");
    return false;
  }
  out = std::move(candidate);
  return true;
}

ResolvedWorldEnvironment
resolveWorldEnvironment(const WorldEnvironmentState &state) {
  ResolvedWorldEnvironment resolved;
  resolved.generation = state.generation;
  resolved.lighting_generation = state.lighting_generation;
  resolved.celestial_generation = state.celestial_generation;
  resolved.cloud_generation = state.cloud_generation;
  resolved.display_generation = state.display_generation;
  resolved.target_generation = state.target_generation;
  resolved.hdri_runtime_generation = state.hdri_runtime_generation;
  resolved.requested_hdri_runtime_width = state.hdri_runtime_resolution;
  resolved.debug_view = state.debug_view;
  resolved.background_exposure =
      std::isfinite(state.background_exposure)
          ? std::clamp(state.background_exposure, -10.0f, 10.0f)
          : 0.0f;
  resolved.rotation_radians = static_cast<float>(
      wrapRadians(static_cast<double>(state.rotation_radians)));
  const float strength_ev =
      std::isfinite(state.global_lighting_strength_ev)
          ? std::clamp(state.global_lighting_strength_ev, -10.0f, 10.0f)
          : 0.0f;
  const float requested_strength = std::exp2(strength_ev);
  resolved.global_lighting_strength = requested_strength;
  resolved.background_multiplier =
      requested_strength * std::exp2(resolved.background_exposure);

  switch (state.sky_rendering) {
  case SkyRendering::ProceduralDayNight:
    if (state.procedural_resources_ready && state.celestial.valid &&
        state.atmosphere.valid() && state.clouds.valid()) {
      resolved.sky_rendering = SkyRendering::ProceduralDayNight;
      resolved.celestial = &state.celestial;
      resolved.atmosphere = &state.atmosphere;
      resolved.sun = &state.sun;
      resolved.moon = &state.moon;
      resolved.atmosphere_controls = &state.atmosphere_controls;
      resolved.night = &state.night;
      resolved.clouds = state.clouds.enabled ? &state.clouds : nullptr;
    } else {
      resolved.warning = "procedural sky resources are unavailable";
    }
    break;
  case SkyRendering::UserHdri:
    if (state.hdr.valid() &&
        (state.selected_hdr_identity.empty() ||
         state.selected_hdr_identity == state.hdr.source_identity)) {
      resolved.sky_rendering = SkyRendering::UserHdri;
      resolved.hdr = &state.hdr;
      std::string budget_error;
      if (resolveHdriRuntimeBudget(
              state.hdr.radiance, state.hdri_runtime_resolution,
              resolved.hdri_runtime_budget, &budget_error)) {
        resolved.resolved_hdri_runtime_width =
            resolved.hdri_runtime_budget.resolved_width;
        resolved.resolved_hdri_runtime_height =
            resolved.hdri_runtime_budget.resolved_height;
      } else {
        resolved.warning = budget_error.empty()
                               ? "HDR runtime extent is unavailable"
                               : budget_error;
      }
    } else {
      resolved.warning = "selected HDR environment is unavailable";
    }
    break;
  case SkyRendering::Off:
  default:
    break;
  }

  if (resolved.sky_rendering != SkyRendering::Off) {
    resolved.background_transparent = state.background_transparent;
    resolved.background_visible =
        state.background_visible && !state.background_transparent;
    resolved.environment_lighting = state.environment_lighting;
    resolved.sun_moon_lighting =
        resolved.sky_rendering == SkyRendering::ProceduralDayNight &&
        state.sun_moon_lighting;
    resolved.environment_strength =
        state.environment_lighting ? requested_strength : 0.0f;
    if (!resolved.background_visible) {
      resolved.background_multiplier = 0.0f;
    }
  }
  return resolved;
}

ResolvedSunLight
resolveSunLight(const ResolvedWorldEnvironment &resolved) noexcept {
  ResolvedSunLight sun;
  std::uint64_t generation = 14695981039346656037ull;
  generation = mixResolvedGeneration(
      generation, static_cast<std::uint64_t>(resolved.sky_rendering));
  generation = mixResolvedGeneration(generation, resolved.generation);
  generation = mixResolvedGeneration(generation,
                                     resolved.lighting_generation);
  generation = mixResolvedGeneration(generation,
                                     resolved.celestial_generation);
  generation = mixResolvedGeneration(generation, resolved.cloud_generation);
  generation = mixResolvedGeneration(
      generation,
      static_cast<std::uint64_t>(
          std::bit_cast<std::uint32_t>(resolved.global_lighting_strength)));
  generation = mixResolvedGeneration(
      generation,
      static_cast<std::uint64_t>(
          std::bit_cast<std::uint32_t>(resolved.rotation_radians)));
  generation = mixResolvedGeneration(
      generation, static_cast<std::uint64_t>(resolved.sun_moon_lighting));
  sun.generation = generation;
  sun.cloud_generation = resolved.cloud_generation;
  if (resolved.sky_rendering != SkyRendering::ProceduralDayNight ||
      resolved.celestial == nullptr || !resolved.celestial->valid ||
      resolved.sun == nullptr) {
    return sun;
  }

  const SunControls &controls = *resolved.sun;
  const double cosine = std::cos(static_cast<double>(resolved.rotation_radians));
  const double sine = std::sin(static_cast<double>(resolved.rotation_radians));
  const auto &local = resolved.celestial->sun.direction;
  sun.direction = normalizedDirection({
      static_cast<float>(local[0] * cosine + local[2] * sine),
      static_cast<float>(local[1]),
      static_cast<float>(-local[0] * sine + local[2] * cosine)});
  sun.color = colorTemperatureRgb(controls.color_temperature_kelvin);
  const double angular_diameter_degrees =
      std::isfinite(controls.angular_diameter_degrees)
          ? std::clamp(static_cast<double>(controls.angular_diameter_degrees),
                       0.05, 5.0)
          : 0.533;
  sun.angular_radius = static_cast<float>(
      angular_diameter_degrees * 0.5 * (kPi / 180.0));
  sun.solid_angle = static_cast<float>(
      kTwoPi * (1.0 - std::cos(static_cast<double>(sun.angular_radius))));
  sun.strength = std::isfinite(controls.strength)
                     ? std::clamp(controls.strength, 0.0f, 32.0f)
                     : 0.0f;
  sun.enabled = controls.enabled;
  sun.disk_visible = controls.enabled && controls.disk_visible;
  sun.casts_shadow = controls.cast_shadows;
  sun.lighting_enabled =
      controls.enabled && resolved.sun_moon_lighting &&
      resolved.global_lighting_strength > 0.0f && sun.strength > 0.0f;

  const float horizon_visibility =
      smoothUnit((sun.direction[1] + 0.05f) / 0.07f);
  const float radiance_scale =
      sun.lighting_enabled
          ? 700.0f * sun.strength * resolved.global_lighting_strength *
                horizon_visibility
          : 0.0f;
  for (std::size_t channel = 0; channel < sun.radiance.size(); ++channel) {
    sun.radiance[channel] = sun.color[channel] * radiance_scale;
  }
  sun.power_estimate =
      std::max(rgbLuminance(sun.radiance) * sun.solid_angle, 0.0f);
  const auto mix_float = [&](float value) {
    generation = mixResolvedGeneration(
        generation, static_cast<std::uint64_t>(
                        std::bit_cast<std::uint32_t>(value)));
  };
  for (const float value : sun.direction) {
    mix_float(value);
  }
  for (const float value : sun.color) {
    mix_float(value);
  }
  mix_float(sun.angular_radius);
  mix_float(sun.strength);
  mix_float(sun.power_estimate);
  generation = mixResolvedGeneration(
      generation, static_cast<std::uint64_t>(sun.enabled));
  generation = mixResolvedGeneration(
      generation, static_cast<std::uint64_t>(sun.disk_visible));
  generation = mixResolvedGeneration(
      generation, static_cast<std::uint64_t>(sun.casts_shadow));
  generation = mixResolvedGeneration(
      generation, static_cast<std::uint64_t>(sun.lighting_enabled));
  sun.generation = generation;
  return sun;
}

float estimateEnvironmentPower(const FloatEnvironmentImage &image,
                               float lighting_strength) noexcept {
  if (!image.valid() || !std::isfinite(lighting_strength) ||
      !(lighting_strength > 0.0f)) {
    return 0.0f;
  }
  double integrated_luminance = 0.0;
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const double theta0 =
        kPi * static_cast<double>(y) / static_cast<double>(image.height);
    const double theta1 = kPi * static_cast<double>(y + 1u) /
                          static_cast<double>(image.height);
    const double texel_solid_angle =
        (kTwoPi / static_cast<double>(image.width)) *
        (std::cos(theta0) - std::cos(theta1));
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * image.width + x) * 4u;
      const double red = std::max(static_cast<double>(image.rgba[base]), 0.0);
      const double green =
          std::max(static_cast<double>(image.rgba[base + 1u]), 0.0);
      const double blue =
          std::max(static_cast<double>(image.rgba[base + 2u]), 0.0);
      const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
      if (std::isfinite(luminance)) {
        integrated_luminance += luminance * texel_solid_angle;
      }
    }
  }
  const double power = integrated_luminance * lighting_strength;
  return std::isfinite(power)
             ? static_cast<float>(std::clamp(
                   power, 0.0,
                   static_cast<double>((std::numeric_limits<float>::max)())))
             : 0.0f;
}

ResolvedEnvironmentView resolveEnvironmentView(
    const ResolvedWorldEnvironment &resolved,
    const std::array<float, 3> &constant_fallback,
    float analytic_fallback_strength) noexcept {
  ResolvedEnvironmentView view;
  std::uint64_t generation = 14695981039346656037ull;
  generation = mixResolvedGeneration(
      generation, static_cast<std::uint64_t>(resolved.sky_rendering));
  generation = mixResolvedGeneration(generation, resolved.generation);
  generation = mixResolvedGeneration(generation,
                                     resolved.lighting_generation);
  generation = mixResolvedGeneration(generation,
                                     resolved.celestial_generation);
  generation = mixResolvedGeneration(generation, resolved.cloud_generation);
  generation = mixResolvedGeneration(generation, resolved.target_generation);
  view.generation = generation;
  view.rotation_radians = resolved.rotation_radians;
  view.background_multiplier = resolved.background_multiplier;
  view.background_visible = resolved.background_visible;
  const auto finalize_generation =
      [&](ResolvedEnvironmentView &candidate,
          std::uint64_t source_generation = 0u) {
        std::uint64_t candidate_generation = candidate.generation;
        candidate_generation = mixResolvedGeneration(
            candidate_generation,
            static_cast<std::uint64_t>(candidate.kind));
        const auto mix_float = [&](float value) {
          candidate_generation = mixResolvedGeneration(
              candidate_generation,
              static_cast<std::uint64_t>(
                  std::bit_cast<std::uint32_t>(value)));
        };
        for (const float value : candidate.constant_radiance) {
          mix_float(value);
        }
        mix_float(candidate.analytic_strength);
        mix_float(candidate.lighting_strength);
        mix_float(candidate.background_multiplier);
        mix_float(candidate.rotation_radians);
        mix_float(candidate.power_estimate);
        candidate_generation = mixResolvedGeneration(
            candidate_generation,
            static_cast<std::uint64_t>(candidate.lighting_enabled));
        candidate_generation = mixResolvedGeneration(
            candidate_generation,
            static_cast<std::uint64_t>(candidate.background_visible));
        candidate_generation = mixResolvedGeneration(
            candidate_generation,
            static_cast<std::uint64_t>(candidate.sampling_ready));
        candidate_generation =
            mixResolvedGeneration(candidate_generation, source_generation);
        candidate.generation = candidate_generation;
      };

  if (resolved.sky_rendering == SkyRendering::UserHdri &&
      resolved.hdr != nullptr && resolved.hdr->valid()) {
    view.kind = ResolvedEnvironmentKind::UserHdri;
    view.radiance = &resolved.hdr->radiance;
    view.distribution = &resolved.hdr->distribution;
    view.lighting_strength = resolved.environment_strength;
    view.lighting_enabled = resolved.environment_lighting &&
                            resolved.environment_strength > 0.0f;
    view.sampling_ready = view.lighting_enabled;
    view.power_estimate = estimateEnvironmentPower(
        resolved.hdr->radiance, resolved.environment_strength);
    finalize_generation(view, resolved.hdr->generation);
    return view;
  }

  if (resolved.sky_rendering == SkyRendering::ProceduralDayNight) {
    view.kind = ResolvedEnvironmentKind::ProceduralDayNight;
    view.lighting_strength = resolved.environment_strength;
    view.lighting_enabled = resolved.environment_lighting &&
                            resolved.environment_strength > 0.0f;
    view.gpu_cache_required = true;
    finalize_generation(view);
    return view;
  }

  const float analytic_strength =
      std::isfinite(analytic_fallback_strength)
          ? std::max(analytic_fallback_strength, 0.0f)
          : 0.0f;
  if (analytic_strength > 0.0f) {
    view.kind = ResolvedEnvironmentKind::AnalyticFallback;
    view.analytic_strength = analytic_strength;
    view.lighting_strength = analytic_strength;
    view.lighting_enabled = true;
    view.background_visible = true;
    view.sampling_ready = true;
    const std::array<float, 3> up =
        analyticFallbackRadiance({0.0f, 1.0f, 0.0f}, analytic_strength);
    const std::array<float, 3> horizon =
        analyticFallbackRadiance({1.0f, 0.0f, 0.0f}, analytic_strength);
    const std::array<float, 3> down =
        analyticFallbackRadiance({0.0f, -1.0f, 0.0f}, analytic_strength);
    const std::array<float, 3> mean{
        (up[0] + horizon[0] + down[0]) / 3.0f,
        (up[1] + horizon[1] + down[1]) / 3.0f,
        (up[2] + horizon[2] + down[2]) / 3.0f};
    view.power_estimate =
        std::max(rgbLuminance(mean) * static_cast<float>(4.0 * kPi), 0.0f);
    finalize_generation(view);
    return view;
  }

  view.kind = ResolvedEnvironmentKind::Constant;
  bool finite_positive = false;
  for (std::size_t channel = 0; channel < view.constant_radiance.size();
       ++channel) {
    const float value = constant_fallback[channel];
    view.constant_radiance[channel] =
        std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
    finite_positive = finite_positive || view.constant_radiance[channel] > 0.0f;
  }
  view.lighting_strength = finite_positive ? 1.0f : 0.0f;
  view.lighting_enabled = finite_positive;
  view.sampling_ready = finite_positive;
  view.power_estimate =
      finite_positive
          ? rgbLuminance(view.constant_radiance) * static_cast<float>(4.0 * kPi)
          : 0.0f;
  finalize_generation(view);
  return view;
}

ResolvedEnvironmentView attachEnvironmentSamplingData(
    ResolvedEnvironmentView view, const FloatEnvironmentImage &radiance,
    const EnvironmentDistribution &distribution,
    std::uint64_t cache_generation) noexcept {
  if (!radiance.valid() || !distribution.valid() ||
      radiance.width != distribution.width() ||
      radiance.height != distribution.height()) {
    return view;
  }
  view.radiance = &radiance;
  view.distribution = &distribution;
  view.sampling_ready = view.lighting_enabled;
  view.gpu_cache_required = false;
  view.power_estimate =
      estimateEnvironmentPower(radiance, view.lighting_strength);
  view.generation = mixResolvedGeneration(view.generation, cache_generation);
  return view;
}

std::array<float, 3> evaluateEnvironment(
    const ResolvedEnvironmentView &environment,
    const std::array<float, 3> &direction) noexcept {
  if (!environment.lighting_enabled) {
    return {0.0f, 0.0f, 0.0f};
  }
  if (environment.sampling_ready && environment.radiance != nullptr &&
      environment.distribution != nullptr && environment.radiance->valid() &&
      environment.distribution->valid()) {
    const std::array<float, 3> unit = normalizedDirection(direction);
    double phi = std::atan2(static_cast<double>(unit[0]),
                            static_cast<double>(unit[2])) -
                 static_cast<double>(environment.rotation_radians);
    phi = wrapRadians(phi);
    const double v = std::acos(std::clamp(static_cast<double>(unit[1]),
                                         -1.0, 1.0)) /
                     kPi;
    const std::uint32_t x = std::min(
        static_cast<std::uint32_t>(
            phi / kTwoPi * static_cast<double>(environment.radiance->width)),
        environment.radiance->width - 1u);
    const std::uint32_t y = std::min(
        static_cast<std::uint32_t>(
            v * static_cast<double>(environment.radiance->height)),
        environment.radiance->height - 1u);
    const std::size_t base =
        (static_cast<std::size_t>(y) * environment.radiance->width + x) * 4u;
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
      const float value = environment.radiance->rgba[base + channel];
      result[channel] =
          std::isfinite(value)
              ? std::max(value, 0.0f) * environment.lighting_strength
              : 0.0f;
    }
    return result;
  }
  if (environment.kind == ResolvedEnvironmentKind::Constant) {
    return environment.constant_radiance;
  }
  if (environment.kind == ResolvedEnvironmentKind::AnalyticFallback) {
    return analyticFallbackRadiance(direction, environment.analytic_strength);
  }
  return {0.0f, 0.0f, 0.0f};
}

float environmentPdf(const ResolvedEnvironmentView &environment,
                     const std::array<float, 3> &direction) noexcept {
  if (!environment.lighting_enabled) {
    return 0.0f;
  }
  if (environment.sampling_ready && environment.distribution != nullptr &&
      environment.distribution->valid()) {
    const std::array<float, 3> unit = normalizedDirection(direction);
    const std::array<double, 3> double_direction{
        unit[0], unit[1], unit[2]};
    const double pdf = environment.distribution->solidAnglePdf(
        double_direction, environment.rotation_radians);
    return std::isfinite(pdf) && pdf > 0.0 ? static_cast<float>(pdf) : 0.0f;
  }
  return (environment.kind == ResolvedEnvironmentKind::Constant ||
          environment.kind == ResolvedEnvironmentKind::AnalyticFallback) &&
                 environment.sampling_ready
             ? static_cast<float>(1.0 / (4.0 * kPi))
             : 0.0f;
}

EnvironmentSample sampleEnvironment(
    const ResolvedEnvironmentView &environment, float column_sample,
    float coin_sample, float jitter_u, float jitter_v) noexcept {
  EnvironmentSample sample;
  if (!environment.lighting_enabled || !environment.sampling_ready) {
    return sample;
  }
  if (environment.distribution != nullptr &&
      environment.distribution->valid()) {
    const EnvironmentDirectionSample direction = environment.distribution->sample(
        column_sample, coin_sample, jitter_u, jitter_v,
        environment.rotation_radians);
    if (!direction.valid) {
      return sample;
    }
    sample.direction = {static_cast<float>(direction.direction[0]),
                        static_cast<float>(direction.direction[1]),
                        static_cast<float>(direction.direction[2])};
  } else if (environment.kind == ResolvedEnvironmentKind::Constant ||
             environment.kind ==
                 ResolvedEnvironmentKind::AnalyticFallback) {
    const float u = std::isfinite(column_sample)
                        ? std::clamp(column_sample, 0.0f,
                                     std::nextafter(1.0f, 0.0f))
                        : 0.0f;
    const float v = std::isfinite(coin_sample)
                        ? std::clamp(coin_sample, 0.0f,
                                     std::nextafter(1.0f, 0.0f))
                        : 0.0f;
    const float y = 1.0f - 2.0f * u;
    const float radius = std::sqrt(std::max(1.0f - y * y, 0.0f));
    const float angle = static_cast<float>(kTwoPi) * v;
    sample.direction = {radius * std::cos(angle), y,
                        radius * std::sin(angle)};
  } else {
    return sample;
  }
  sample.radiance = evaluateEnvironment(environment, sample.direction);
  sample.pdf = environmentPdf(environment, sample.direction);
  sample.valid = sample.pdf > 0.0f &&
                 std::isfinite(sample.radiance[0]) &&
                 std::isfinite(sample.radiance[1]) &&
                 std::isfinite(sample.radiance[2]);
  return sample;
}

std::uint64_t environmentGeneration(
    const ResolvedEnvironmentView &environment) noexcept {
  return environment.generation;
}

namespace {

bool checkedAdd(std::uint64_t &total, std::uint64_t value) noexcept {
  if (value > (std::numeric_limits<std::uint64_t>::max)() - total) {
    return false;
  }
  total += value;
  return true;
}

bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t &out) noexcept {
  if (left != 0u &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  out = left * right;
  return true;
}

bool estimateHdriRuntimeBudget(
    const FloatEnvironmentImage &source, std::uint32_t requested_width,
    std::uint32_t resolved_width, const HdriRuntimeBudgetLimits &limits,
    HdriRuntimeBudget &out) noexcept {
  if (!source.valid() || source.width != source.height * 2u ||
      requested_width == 0u || resolved_width < 2u ||
      (resolved_width & 1u) != 0u || resolved_width > source.width) {
    return false;
  }

  HdriRuntimeBudget candidate;
  candidate.source_width = source.width;
  candidate.source_height = source.height;
  candidate.requested_width = requested_width;
  candidate.resolved_width = resolved_width;
  candidate.resolved_height = resolved_width / 2u;
  candidate.distribution_width = candidate.resolved_width;
  candidate.distribution_height = candidate.resolved_height;

  std::uint64_t source_texels = 0u;
  std::uint64_t runtime_texels = 0u;
  if (!checkedMultiply(source.width, source.height, source_texels) ||
      !checkedMultiply(candidate.resolved_width,
                       candidate.resolved_height, runtime_texels) ||
      !checkedMultiply(source_texels, 4u * sizeof(float),
                       candidate.source_radiance_bytes) ||
      // AliasTable retains two doubles and one uint32 per texel.
      !checkedMultiply(source_texels,
                       2u * sizeof(double) + sizeof(std::uint32_t),
                       candidate.source_distribution_bytes) ||
      !checkedMultiply(runtime_texels, 4u * sizeof(float),
                       candidate.runtime_radiance_bytes) ||
      !checkedMultiply(runtime_texels,
                       2u * sizeof(double) + sizeof(std::uint32_t),
                       candidate.runtime_distribution_bytes) ||
      // Distribution construction temporarily owns weights, scaled weights,
      // and the small/large work lists.
      !checkedMultiply(runtime_texels,
                       2u * sizeof(double) + 2u * sizeof(std::uint32_t),
                       candidate.distribution_scratch_bytes)) {
    return false;
  }

  std::uint64_t mip_texels = runtime_texels;
  std::uint32_t mip_width = candidate.resolved_width;
  std::uint32_t mip_height = candidate.resolved_height;
  while (mip_width > 1u || mip_height > 1u) {
    mip_width = std::max(mip_width >> 1u, 1u);
    mip_height = std::max(mip_height >> 1u, 1u);
    std::uint64_t level_texels = 0u;
    if (!checkedMultiply(mip_width, mip_height, level_texels) ||
        !checkedAdd(mip_texels, level_texels)) {
      return false;
    }
  }
  if (!checkedMultiply(mip_texels, 4u * sizeof(float),
                       candidate.gpu_image_bytes)) {
    return false;
  }
  constexpr std::uint64_t kGpuHeaderBytes = 112u;
  constexpr std::uint64_t kGpuAliasBytes = 16u;
  if (!checkedMultiply(runtime_texels, kGpuAliasBytes,
                       candidate.gpu_distribution_bytes) ||
      !checkedAdd(candidate.gpu_distribution_bytes, kGpuHeaderBytes)) {
    return false;
  }
  candidate.staging_bytes = candidate.runtime_radiance_bytes;
  candidate.safety_margin_bytes = limits.safety_margin_bytes;

  std::uint64_t total = 0u;
  if (!checkedAdd(total, candidate.source_radiance_bytes) ||
      !checkedAdd(total, candidate.source_distribution_bytes) ||
      !checkedAdd(total, candidate.runtime_radiance_bytes) ||
      !checkedAdd(total, candidate.runtime_distribution_bytes) ||
      !checkedAdd(total, candidate.distribution_scratch_bytes) ||
      !checkedAdd(total, candidate.gpu_image_bytes) ||
      !checkedAdd(total, candidate.gpu_distribution_bytes) ||
      !checkedAdd(total, candidate.staging_bytes) ||
      !checkedAdd(total, candidate.safety_margin_bytes)) {
    return false;
  }
  candidate.total_bytes = total;
  out = candidate;
  return true;
}

} // namespace

std::array<double, 3> sampleUniformTriangleBarycentrics(
    double sample_u, double sample_v) noexcept {
  const double u = std::isfinite(sample_u)
                       ? std::clamp(sample_u, 0.0,
                                    std::nextafter(1.0, 0.0))
                       : 0.0;
  const double v = std::isfinite(sample_v)
                       ? std::clamp(sample_v, 0.0,
                                    std::nextafter(1.0, 0.0))
                       : 0.0;
  const double root = std::sqrt(u);
  return {1.0 - root, root * (1.0 - v), root * v};
}

double emissiveTriangleSolidAnglePdf(
    double triangle_selection_probability, double world_area,
    double distance_squared, double absolute_light_cosine) noexcept {
  if (!std::isfinite(triangle_selection_probability) ||
      !std::isfinite(world_area) ||
      triangle_selection_probability <= 0.0 || world_area <= 0.0) {
    return 0.0;
  }
  return areaPdfToSolidAngle(
      triangle_selection_probability / world_area, distance_squared,
      absolute_light_cosine);
}

bool EmissivePatchDistribution::build(
    std::span<const EmissivePatch> patches) {
  clear();
  std::vector<double> weights(patches.size(), 0.0);
  for (std::size_t index = 0; index < patches.size(); ++index) {
    const double area = patches[index].world_area;
    const double light = patches[index].average_luminance;
    if (std::isfinite(area) && std::isfinite(light) && area > 0.0 &&
        light > 0.0) {
      weights[index] = area * light;
    }
  }
  return alias_.build(weights);
}

void EmissivePatchDistribution::clear() noexcept { alias_.clear(); }

double powerHeuristic(double pdf_a, double pdf_b, double exponent) noexcept {
  if (!std::isfinite(pdf_a) || !std::isfinite(pdf_b) ||
      !std::isfinite(exponent) || exponent <= 0.0 || pdf_a < 0.0 ||
      pdf_b < 0.0) {
    return 0.0;
  }
  if (pdf_a == 0.0) {
    return 0.0;
  }
  if (pdf_b == 0.0) {
    return 1.0;
  }
  const double maximum = std::max(pdf_a, pdf_b);
  const double a = std::pow(pdf_a / maximum, exponent);
  const double b = std::pow(pdf_b / maximum, exponent);
  const double denominator = a + b;
  return denominator > 0.0 && std::isfinite(denominator)
             ? a / denominator
             : 0.0;
}

double areaPdfToSolidAngle(double area_pdf, double distance_squared,
                           double absolute_light_cosine) noexcept {
  if (!std::isfinite(area_pdf) || !std::isfinite(distance_squared) ||
      !std::isfinite(absolute_light_cosine) || area_pdf <= 0.0 ||
      distance_squared <= 0.0 || absolute_light_cosine <= 0.0) {
    return 0.0;
  }
  const double result =
      area_pdf * distance_squared / absolute_light_cosine;
  return std::isfinite(result) ? result : 0.0;
}

} // namespace xpbd::gfx
