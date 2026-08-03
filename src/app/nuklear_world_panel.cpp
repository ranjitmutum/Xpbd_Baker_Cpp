#include "nuklear_ui_internal.hpp"

#include "nuklear_still_settings_panel.hpp"

#include "xpbd/app/i18n.hpp"
#include "xpbd/baker/output_timeline_resampler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace xpbd::app::ui_internal {

void drawSkyEditor(nk_context *ctx, const Geom &g, AppSession &session) {
  const bool busy =
      session.bake_busy.load() || session.stillRenderActive();
  auto &world = session.world_environment;
  heading(ctx, g, tr("sky_page"));
  mutedWrap(ctx, g, tr("sky_page_hint"));
  mutedWrap(ctx, g, tr("sky_scene_independent"));

  std::vector<const char *> modes{tr("sky_rendering_off"),
                                  tr("sky_rendering_procedural"),
                                  tr("sky_rendering_hdri")};
  int mode = static_cast<int>(world.sky_rendering);
  const int previous_mode = mode;
  if (combo(ctx, g, tr("sky_rendering"), modes, mode, busy) &&
      mode != previous_mode) {
    if (!session.setSkyRendering(static_cast<gfx::SkyRendering>(mode))) {
      mode = previous_mode;
    }
  }
  const auto resolved = gfx::resolveWorldEnvironment(session.world_environment);
  if (!resolved.warning.empty()) {
    mutedWrap(ctx, g, resolved.warning.c_str());
  }
  if (!session.last_error.empty() &&
      session.status.find("World Sky") != std::string::npos) {
    mutedWrap(ctx, g, session.last_error.c_str());
  }

  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("sky_save_settings")) && !busy) {
    if (const auto path =
            saveFileDialog(L"Save World Sky Settings",
                           L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0",
                           L"world_sky.json")) {
      session.saveWorldSkySettings(*path);
    }
  }
  if (nk_button_label(ctx, tr("sky_load_settings")) && !busy) {
    if (const auto path = openFileDialog(
            L"Load World Sky Settings",
            L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0")) {
      session.loadWorldSkySettings(*path);
    }
  }

  const bool sky_off = world.sky_rendering == gfx::SkyRendering::Off;
  const bool sky_busy = busy || sky_off;
  heading(ctx, g, tr("sky_global_lighting"));
  if (check(ctx, g, tr("sky_environment_lighting"),
            world.environment_lighting, sky_busy)) {
    session.touchWorldEnvironment();
  }
  if (check(ctx, g, tr("sky_sun_moon_lighting"),
            world.sun_moon_lighting, sky_busy)) {
    session.touchWorldEnvironment();
  }
  if (slider(ctx, g, tr("sky_global_strength_ev"),
             world.global_lighting_strength_ev, -10.0f, 10.0f, sky_busy,
             0.1f)) {
    session.touchWorldEnvironment();
  }
  mutedWrap(ctx, g, tr("sky_global_strength_hint"));
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("sky_reset_physical")) && !sky_busy) {
    session.resetWorldSkyPhysicalDefaults();
  }

  heading(ctx, g, tr("sky_background"));
  if (check(ctx, g, tr("sky_background_visible"),
            world.background_visible, sky_busy)) {
    session.touchWorldEnvironmentDisplay();
  }
  if (check(ctx, g, tr("sky_background_transparent"),
            world.background_transparent, sky_busy)) {
    session.touchWorldEnvironmentDisplay();
  }
  if (slider(ctx, g, tr("sky_background_exposure"),
             world.background_exposure, -10.0f, 10.0f, sky_busy, 0.1f)) {
    session.touchWorldEnvironmentDisplay();
  }
  mutedWrap(ctx, g, tr("sky_background_exposure_hint"));
  float rotation_degrees = world.rotation_radians *
                           (180.0f / 3.14159265358979323846f);
  if (slider(ctx, g, tr("sky_rotation"), rotation_degrees, -180.0f, 180.0f,
             sky_busy, 1.0f)) {
    world.rotation_radians =
        rotation_degrees * (3.14159265358979323846f / 180.0f);
    session.touchWorldEnvironment();
  }

  heading(ctx, g, tr("sky_hdri"));
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("sky_load_hdri")) && !busy) {
    if (const auto path = openFileDialog(
            L"Open Radiance HDRI",
            L"Radiance HDR (*.hdr)\0*.hdr\0All Files (*.*)\0*.*\0")) {
      session.loadWorldHdr(*path);
    }
  }
  int hdri_resolution = static_cast<int>(world.hdri_runtime_resolution);
  if (intProperty(ctx, g, tr("sky_hdri_runtime_resolution"), hdri_resolution,
                  256, 8192, 256, busy)) {
    world.hdri_runtime_resolution =
        static_cast<std::uint32_t>(hdri_resolution);
    session.touchWorldEnvironmentTargets();
  }
  if (world.hdr.valid()) {
    const std::string line = std::string(tr("sky_hdri_active")) +
                             world.hdr.source_identity;
    mutedWrap(ctx, g, line.c_str());
    const std::string checksum = std::string(tr("sky_hdri_checksum")) +
                                 world.hdr.checksum.substr(0, 16);
    mutedWrap(ctx, g, checksum.c_str());
  } else {
    mutedWrap(ctx, g, tr("sky_hdri_required"));
  }

  if (world.sky_rendering ==
      gfx::SkyRendering::ProceduralDayNight) {
    const bool celestial_busy = busy;
    heading(ctx, g, tr("sky_time_location"));
    gfx::UtcDateTime local = world.celestial.utc;
    if (!gfx::shiftUtcDateTime(
            world.celestial.utc,
            static_cast<double>(world.time.utc_offset_hours) * 3600.0,
            local)) {
      local = world.celestial.utc;
    }
    gfx::ObserverLocation observer = world.celestial.observer;
    float sun_azimuth_offset = static_cast<float>(
        world.sun_azimuth_offset_degrees);
    float sun_altitude_offset = static_cast<float>(
        world.sun_altitude_offset_degrees);
    bool celestial_changed = false;
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_year"), local.year, 1900, 2100, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_month"), local.month, 1, 12, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_day"), local.day, 1, 31, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_hour"), local.hour, 0, 23, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_minute"), local.minute, 0, 59, 1,
                    celestial_busy);
    float local_second = static_cast<float>(local.second);
    if (slider(ctx, g, tr("sky_local_second"), local_second, 0.0f, 59.0f,
               celestial_busy, 1.0f)) {
      local.second = local_second;
      celestial_changed = true;
    }
    if (slider(ctx, g, tr("sky_utc_offset"),
               world.time.utc_offset_hours, -14.0f, 14.0f,
               celestial_busy, 0.25f)) {
      session.touchWorldEnvironmentDisplay();
    }
    if (check(ctx, g, tr("sky_time_playing"), world.time.playing,
              celestial_busy)) {
      session.touchWorldEnvironmentCelestial();
    }
    if (slider(ctx, g, tr("sky_time_speed"), world.time.time_speed,
               -86400.0f, 86400.0f, celestial_busy, 60.0f)) {
      session.touchWorldEnvironmentCelestial();
    }
    float observer_latitude = static_cast<float>(observer.latitude_degrees);
    if (slider(ctx, g, tr("sky_observer_latitude"), observer_latitude,
               -90.0f, 90.0f, celestial_busy, 0.1f)) {
      observer.latitude_degrees = observer_latitude;
      celestial_changed = true;
    }
    float observer_longitude = static_cast<float>(observer.longitude_degrees);
    if (slider(ctx, g, tr("sky_observer_longitude"), observer_longitude,
               -180.0f, 180.0f, celestial_busy, 0.1f)) {
      observer.longitude_degrees = observer_longitude;
      celestial_changed = true;
    }
    float observer_elevation = static_cast<float>(observer.elevation_meters);
    if (slider(ctx, g, tr("sky_observer_elevation"), observer_elevation,
               -1000.0f, 100000.0f, celestial_busy, 10.0f)) {
      observer.elevation_meters = observer_elevation;
      celestial_changed = true;
    }
    float north_offset = static_cast<float>(observer.north_offset_degrees);
    if (slider(ctx, g, tr("sky_observer_north_offset"), north_offset,
               -180.0f, 180.0f, celestial_busy, 1.0f)) {
      observer.north_offset_degrees = north_offset;
      celestial_changed = true;
    }
    if (celestial_changed) {
      gfx::UtcDateTime utc;
      std::string shift_error;
      if (gfx::shiftUtcDateTime(
              local,
              -static_cast<double>(world.time.utc_offset_hours) * 3600.0,
              utc, &shift_error)) {
        session.setProceduralSkyControls(
            utc, observer, sun_azimuth_offset, sun_altitude_offset);
      } else {
        session.last_error = shift_error;
        session.status = "World Sky local time rejected";
      }
    }

    heading(ctx, g, tr("sky_sun"));
    auto &sun_controls = world.sun;
    bool sun_recompute = false;
    if (check(ctx, g, tr("sky_enabled"), sun_controls.enabled,
              celestial_busy)) {
      session.touchWorldEnvironment();
    }
    if (slider(ctx, g, tr("sky_relative_strength"), sun_controls.strength,
               0.0f, 32.0f, celestial_busy, 0.05f)) {
      session.touchWorldEnvironment();
    }
    std::vector<const char *> direction_modes{
        tr("sky_direction_automatic"), tr("sky_direction_artistic")};
    int sun_direction = static_cast<int>(sun_controls.direction_mode);
    if (combo(ctx, g, tr("sky_direction"), direction_modes, sun_direction,
              celestial_busy)) {
      sun_controls.direction_mode =
          static_cast<gfx::SkyDirectionMode>(sun_direction);
      sun_recompute = true;
    }
    if (sun_controls.direction_mode ==
        gfx::SkyDirectionMode::ArtisticOffset) {
      sun_recompute |=
          slider(ctx, g, tr("sky_azimuth_offset"), sun_azimuth_offset,
                 -180.0f, 180.0f, celestial_busy, 1.0f);
      sun_recompute |=
          slider(ctx, g, tr("sky_altitude_offset"), sun_altitude_offset,
                 -90.0f, 90.0f, celestial_busy, 1.0f);
    }
    bool sun_physical_changed = false;
    sun_physical_changed |=
        slider(ctx, g, tr("sky_sun_temperature"),
               sun_controls.color_temperature_kelvin, 1000.0f, 40000.0f,
               celestial_busy, 100.0f);
    sun_physical_changed |=
        slider(ctx, g, tr("sky_angular_diameter"),
               sun_controls.angular_diameter_degrees, 0.05f, 5.0f,
               celestial_busy, 0.01f);
    sun_physical_changed |=
        check(ctx, g, tr("sky_cast_shadows"), sun_controls.cast_shadows,
              celestial_busy);
    if (sun_physical_changed) {
      session.touchWorldEnvironment();
    }
    if (check(ctx, g, tr("sky_disk_visible"), sun_controls.disk_visible,
              celestial_busy)) {
      session.touchWorldEnvironmentDisplay();
    }
    if (sun_recompute) {
      session.setProceduralSkyControls(
          world.celestial.utc, world.celestial.observer,
          sun_azimuth_offset, sun_altitude_offset);
    }
    const auto &sun = world.celestial.sun;
    const std::string sun_state =
        std::string(tr("sky_sun_state")) +
        " az=" + std::to_string(sun.azimuth_degrees) +
        " deg, alt=" + std::to_string(sun.apparent_altitude_degrees) +
        " deg, twilight=" +
        gfx::twilightPhaseName(world.celestial.twilight);
    mutedWrap(ctx, g, sun_state.c_str());

    heading(ctx, g, tr("sky_moon"));
    auto &moon_controls = world.moon;
    bool moon_recompute = false;
    bool moon_physical_changed = false;
    moon_physical_changed |=
        check(ctx, g, tr("sky_enabled"), moon_controls.enabled,
              celestial_busy);
    moon_physical_changed |=
        slider(ctx, g, tr("sky_relative_strength"), moon_controls.strength,
               0.0f, 32.0f, celestial_busy, 0.05f);
    if (moon_physical_changed) {
      session.touchWorldEnvironment();
    }
    std::vector<const char *> phase_modes{
        tr("sky_phase_automatic"), tr("sky_phase_manual")};
    int phase_mode = static_cast<int>(moon_controls.phase_mode);
    if (combo(ctx, g, tr("sky_moon_phase"), phase_modes, phase_mode,
              celestial_busy)) {
      moon_controls.phase_mode = static_cast<gfx::MoonPhaseMode>(phase_mode);
      session.touchWorldEnvironment();
    }
    if (moon_controls.phase_mode == gfx::MoonPhaseMode::Manual &&
        slider(ctx, g, tr("sky_moon_fraction"),
               moon_controls.manual_illuminated_fraction, 0.0f, 1.0f,
               celestial_busy, 0.01f)) {
      session.touchWorldEnvironment();
    }
    int moon_direction = static_cast<int>(moon_controls.direction_mode);
    if (combo(ctx, g, tr("sky_direction"), direction_modes, moon_direction,
              celestial_busy)) {
      moon_controls.direction_mode =
          static_cast<gfx::SkyDirectionMode>(moon_direction);
      moon_recompute = true;
    }
    if (moon_controls.direction_mode ==
        gfx::SkyDirectionMode::ArtisticOffset) {
      moon_recompute |=
          slider(ctx, g, tr("sky_azimuth_offset"),
                 moon_controls.azimuth_offset_degrees, -180.0f, 180.0f,
                 celestial_busy, 1.0f);
      moon_recompute |=
          slider(ctx, g, tr("sky_altitude_offset"),
                 moon_controls.altitude_offset_degrees, -90.0f, 90.0f,
                 celestial_busy, 1.0f);
    }
    moon_physical_changed = false;
    moon_physical_changed |=
        slider(ctx, g, tr("sky_angular_diameter"),
               moon_controls.angular_diameter_degrees, 0.05f, 5.0f,
               celestial_busy, 0.01f);
    moon_physical_changed |=
        slider(ctx, g, tr("sky_moon_surface_detail"),
               moon_controls.surface_detail, 0.0f, 1.0f,
               celestial_busy, 0.01f);
    moon_physical_changed |=
        check(ctx, g, tr("sky_cast_shadows"), moon_controls.cast_shadows,
              celestial_busy);
    if (moon_physical_changed) {
      session.touchWorldEnvironment();
    }
    if (check(ctx, g, tr("sky_disk_visible"), moon_controls.disk_visible,
              celestial_busy)) {
      session.touchWorldEnvironmentDisplay();
    }
    if (moon_recompute) {
      session.setProceduralSkyControls(
          world.celestial.utc, world.celestial.observer,
          world.sun_azimuth_offset_degrees,
          world.sun_altitude_offset_degrees);
    }

    heading(ctx, g, tr("sky_atmosphere"));
    auto &physical = world.atmosphere.physical;
    const auto default_atmosphere = gfx::defaultEarthAtmosphereConfig();
    const auto scale_for = [](const std::array<double, 3> &value,
                              const std::array<double, 3> &base) {
      double sum = 0.0;
      int count = 0;
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::isfinite(value[i]) && std::isfinite(base[i]) &&
            std::abs(base[i]) > 1.0e-12) {
          sum += value[i] / base[i];
          ++count;
        }
      }
      return count > 0 ? static_cast<float>(sum / count) : 1.0f;
    };
    const auto scale_array = [](std::array<double, 3> &value,
                                const std::array<double, 3> &base,
                                float scale) {
      for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = base[i] * static_cast<double>(scale);
      }
    };
    auto &atmosphere_controls = world.atmosphere_controls;
    if (slider(ctx, g, tr("sky_atmosphere_sky_strength"),
               atmosphere_controls.sky_relative_strength, 0.0f, 8.0f,
               busy, 0.01f)) {
      session.touchWorldEnvironment();
    }
    if (slider(ctx, g, tr("sky_atmosphere_turbidity"),
               atmosphere_controls.turbidity, 0.0f, 4.0f, busy, 0.01f)) {
      scale_array(physical.mie_scattering_per_km,
                  default_atmosphere.physical.mie_scattering_per_km,
                  atmosphere_controls.turbidity);
      scale_array(physical.mie_extinction_per_km,
                  default_atmosphere.physical.mie_extinction_per_km,
                  atmosphere_controls.turbidity);
      session.touchWorldEnvironment();
    }
    if (slider(ctx, g, tr("sky_atmosphere_ozone"),
               atmosphere_controls.ozone, 0.0f, 4.0f, busy, 0.01f)) {
      scale_array(physical.absorption_extinction_per_km,
                  default_atmosphere.physical.absorption_extinction_per_km,
                  atmosphere_controls.ozone);
      session.touchWorldEnvironment();
    }
    float rayleigh_scale =
        scale_for(physical.rayleigh_scattering_per_km,
                  default_atmosphere.physical.rayleigh_scattering_per_km);
    if (slider(ctx, g, tr("sky_atmosphere_rayleigh"), rayleigh_scale, 0.0f,
               4.0f, busy, 0.01f)) {
      scale_array(physical.rayleigh_scattering_per_km,
                  default_atmosphere.physical.rayleigh_scattering_per_km,
                  rayleigh_scale);
      session.touchWorldEnvironment();
    }
    float mie_g = static_cast<float>(physical.mie_phase_function_g);
    if (slider(ctx, g, tr("sky_atmosphere_mie_g"), mie_g, -0.95f, 0.95f,
               busy, 0.01f)) {
      physical.mie_phase_function_g = mie_g;
      session.touchWorldEnvironment();
    }
    float ground_albedo =
        scale_for(physical.ground_albedo,
                  default_atmosphere.physical.ground_albedo);
    if (slider(ctx, g, tr("sky_atmosphere_ground"), ground_albedo, 0.0f,
               1.0f, busy, 0.01f)) {
      scale_array(physical.ground_albedo,
                  default_atmosphere.physical.ground_albedo, ground_albedo);
      session.touchWorldEnvironment();
    }
    std::vector<const char *> lut_qualities{
        tr("sky_quality_low"), tr("sky_quality_medium"),
        tr("sky_quality_high")};
    int lut_quality = static_cast<int>(atmosphere_controls.lut_quality);
    if (combo(ctx, g, tr("sky_atmosphere_lut_quality"), lut_qualities,
              lut_quality, busy)) {
      atmosphere_controls.lut_quality =
          static_cast<std::uint32_t>(lut_quality);
      world.atmosphere.scattering_orders =
          lut_quality == 0 ? 2u : lut_quality == 1 ? 4u : 6u;
      session.touchWorldEnvironment();
    }
    const std::string lut_status =
        std::string(tr("sky_atmosphere_lut_status")) +
        gfx::brunetonAtmosphereCacheKey(world.atmosphere).substr(0, 16);
    mutedWrap(ctx, g, lut_status.c_str());

    heading(ctx, g, tr("sky_night"));
    auto &night = world.night;
    bool night_changed = false;
    night_changed |=
        check(ctx, g, tr("sky_stars_enabled"), night.stars_enabled, busy);
    night_changed |=
        slider(ctx, g, tr("sky_star_intensity"), night.star_intensity,
               0.0f, 32.0f, busy, 0.05f);
    night_changed |=
        check(ctx, g, tr("sky_milky_way_enabled"),
              night.milky_way_enabled, busy);
    night_changed |=
        slider(ctx, g, tr("sky_milky_way_intensity"),
               night.milky_way_intensity, 0.0f, 32.0f, busy, 0.05f);
    night_changed |=
        slider(ctx, g, tr("sky_light_pollution"), night.light_pollution,
               0.0f, 16.0f, busy, 0.05f);
    night_changed |=
        slider(ctx, g, tr("sky_star_rotation"),
               night.star_rotation_degrees, -180.0f, 180.0f, busy, 1.0f);
    night_changed |=
        slider(ctx, g, tr("sky_night_fill"), night.night_fill,
               0.0f, 4.0f, busy, 0.01f);
    if (night_changed) {
      session.touchWorldEnvironment();
    }

    auto &clouds = world.clouds;
    heading(ctx, g, tr("sky_clouds"));
    if (check(ctx, g, tr("sky_clouds_enabled"), clouds.enabled, busy)) {
      session.touchWorldEnvironment(true);
    }
    const bool cloud_disabled = busy || !clouds.enabled;
    auto cloudSlide = [&](const char *label, float &value, float lo, float hi,
                          float step) {
      if (slider(ctx, g, label, value, lo, hi, cloud_disabled, step)) {
        session.touchWorldEnvironment(true);
      }
    };
    cloudSlide(tr("sky_cloud_coverage"), clouds.coverage, 0.0f, 1.0f, 0.01f);
    cloudSlide(tr("sky_cloud_density"), clouds.density, 0.01f, 8.0f, 0.01f);
    cloudSlide(tr("sky_cloud_base"), clouds.base_altitude_km, 0.1f, 20.0f,
               0.1f);
    cloudSlide(tr("sky_cloud_thickness"), clouds.thickness_km, 0.1f, 20.0f,
               0.1f);
    cloudSlide(tr("sky_cloud_wind_x"), clouds.wind_direction[0], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_wind_z"), clouds.wind_direction[1], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_wind_speed"), clouds.wind_speed_km_per_hour,
               -1000.0f, 1000.0f, 1.0f);
    cloudSlide(tr("sky_cloud_shadow_strength"), clouds.shadow_strength,
               0.0f, 1.0f, 0.01f);
    std::vector<const char *> cloud_qualities{
        tr("sky_quality_low"), tr("sky_quality_medium"),
        tr("sky_quality_high"), tr("sky_quality_still")};
    int cloud_quality = static_cast<int>(clouds.quality);
    if (combo(ctx, g, tr("sky_cloud_quality"), cloud_qualities,
              cloud_quality, cloud_disabled)) {
      clouds.quality = static_cast<gfx::CloudQuality>(cloud_quality);
      if (cloud_quality == 0) {
        clouds.ray_steps = 24u;
        clouds.light_steps = 4u;
        clouds.render_ratio = 0.5f;
      } else if (cloud_quality == 1) {
        clouds.ray_steps = 48u;
        clouds.light_steps = 6u;
        clouds.render_ratio = 1.0f;
      } else if (cloud_quality == 2) {
        clouds.ray_steps = 72u;
        clouds.light_steps = 8u;
        clouds.render_ratio = 1.0f;
      } else {
        clouds.ray_steps = 128u;
        clouds.light_steps = 16u;
        clouds.render_ratio = 1.0f;
      }
      session.touchWorldEnvironmentTargets();
    }
    cloudSlide(tr("sky_cloud_weather_scale"), clouds.weather_scale,
               0.05f, 20.0f, 0.01f);
    cloudSlide(tr("sky_cloud_offset_x"), clouds.weather_offset_km[0], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_offset_z"), clouds.weather_offset_km[1], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_base_shape"), clouds.base_shape_scale,
               0.05f, 20.0f, 0.01f);
    cloudSlide(tr("sky_cloud_detail"), clouds.detail_scale,
               0.05f, 20.0f, 0.01f);
    cloudSlide(tr("sky_cloud_erosion"), clouds.erosion,
               0.0f, 1.0f, 0.01f);
    cloudSlide(tr("sky_cloud_forward_scattering"),
               clouds.forward_scattering, -0.95f, 0.95f, 0.01f);
    cloudSlide(tr("sky_cloud_silver_lining"), clouds.silver_lining,
               0.0f, 4.0f, 0.01f);
    cloudSlide(tr("sky_cloud_absorption"), clouds.absorption,
               0.01f, 8.0f, 0.01f);
    cloudSlide(tr("sky_cloud_multiple_scattering"),
               clouds.multiple_scattering, 0.0f, 2.0f, 0.01f);
    float render_ratio = clouds.render_ratio;
    if (slider(ctx, g, tr("sky_cloud_render_ratio"), render_ratio,
               0.25f, 1.0f, cloud_disabled, 0.05f)) {
      clouds.render_ratio = render_ratio;
      session.touchWorldEnvironmentTargets();
    }
    char cloud_resolution_status[128];
    std::snprintf(
        cloud_resolution_status, sizeof(cloud_resolution_status),
        tr("sky_cloud_resolution_status"),
        static_cast<unsigned int>(
            std::lround(2048.0f * clouds.render_ratio)),
        static_cast<unsigned int>(
            std::lround(1024.0f * clouds.render_ratio)));
    muted(ctx, g, cloud_resolution_status);
    if (check(ctx, g, tr("sky_cloud_reprojection"), clouds.reprojection,
              cloud_disabled)) {
      session.touchWorldEnvironment(true);
    }
    cloudSlide(tr("sky_cloud_history_weight"), clouds.history_weight,
               0.0f, 0.999f, 0.01f);
    cloudSlide(tr("sky_cloud_lighting_strength"),
               clouds.lighting_strength, 0.0f, 8.0f, 0.01f);
    cloudSlide(tr("sky_cloud_time"), clouds.time_seconds, -3600.0f, 3600.0f,
               0.1f);
    int seed = static_cast<int>(std::min<std::uint32_t>(clouds.seed, 1'000'000u));
    if (intProperty(ctx, g, tr("sky_cloud_seed"), seed, 0, 1'000'000, 1,
                    cloud_disabled)) {
      clouds.seed = static_cast<std::uint32_t>(seed);
      session.touchWorldEnvironment(true);
    }
    int ray_steps = static_cast<int>(clouds.ray_steps);
    if (intProperty(ctx, g, tr("sky_cloud_ray_steps"), ray_steps, 8, 128, 1,
                    cloud_disabled)) {
      clouds.ray_steps = static_cast<std::uint32_t>(ray_steps);
      session.touchWorldEnvironment(true);
    }
    int light_steps = static_cast<int>(clouds.light_steps);
    if (intProperty(ctx, g, tr("sky_cloud_light_steps"), light_steps, 1, 16, 1,
                    cloud_disabled)) {
      clouds.light_steps = static_cast<std::uint32_t>(light_steps);
      session.touchWorldEnvironment(true);
    }
    int shadow_resolution = static_cast<int>(clouds.shadow_resolution);
    if (intProperty(ctx, g, tr("sky_cloud_shadow_resolution"),
                    shadow_resolution, 64, 4096, 64, cloud_disabled)) {
      clouds.shadow_resolution =
          static_cast<std::uint32_t>(shadow_resolution);
      session.touchWorldEnvironmentTargets();
    }
  }

  heading(ctx, g, tr("sky_debug"));
  std::vector<const char *> debug_views{
      tr("sky_debug_off"), tr("sky_debug_radiance"),
      tr("sky_debug_environment_pdf"),
      tr("sky_debug_cloud_transmittance")};
  int debug_view = static_cast<int>(world.debug_view);
  if (combo(ctx, g, tr("sky_debug_view"), debug_views, debug_view, busy)) {
    world.debug_view = static_cast<gfx::SkyDebugView>(debug_view);
    session.touchWorldEnvironmentDisplay();
  }
}



void drawWorldPanel(UiPanelContext &ui) {
  drawSkyEditor(ui.nk, ui.geom, ui.session);
}

void drawWorldOptions(UiPanelContext &ui) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;

  heading(ctx, g, tr("options"));
  check(ctx, g, tr("show_bones"), session.show_bones, false);
  heading(ctx, g, tr("preview_scene"));
  mutedWrap(ctx, g, tr("preview_scene_hint"));
  constexpr int kSceneSelectionChoiceCount = 10;
  const char *scene_labels[kSceneSelectionChoiceCount] = {
      tr("scene_empty"),          tr("scene_user_built"),
      tr("scene_loaded"),         tr("preview_scene_studio"),
      tr("preview_scene_sky"),    tr("preview_scene_night"),
      tr("preview_scene_sunset"), tr("preview_scene_desert"),
      tr("preview_scene_ocean"),  tr("preview_scene_overcast")};
  int scene_idx = 0;
  switch (session.scene_selection.kind) {
  case SceneSelectionKind::UserBuilt:
    scene_idx = 1;
    break;
  case SceneSelectionKind::Loaded:
    scene_idx = 2;
    break;
  case SceneSelectionKind::Preset:
    scene_idx =
        2 + xpbd::gfx::previewSceneChoiceIndex(
                  session.scene_selection.preset);
    break;
  case SceneSelectionKind::Empty:
  default:
    scene_idx = 0;
    break;
  }
  nk_layout_row_dynamic(ctx, g.btn, 1);
  const int selected_scene_idx =
      nk_combo(ctx, scene_labels, kSceneSelectionChoiceCount, scene_idx,
               static_cast<int>(g.btn),
               nk_vec2(nk_widget_width(ctx), g.btn * 10.0f));
  if (selected_scene_idx != scene_idx) {
    if (selected_scene_idx == 0) {
      session.selectScene(SceneSelectionKind::Empty);
    } else if (selected_scene_idx == 1) {
      session.selectScene(SceneSelectionKind::UserBuilt);
    } else if (selected_scene_idx == 2) {
      session.selectScene(SceneSelectionKind::Loaded);
    } else {
      session.selectPresetScene(
          xpbd::gfx::previewSceneIdFromChoiceIndex(
              selected_scene_idx - 2));
    }
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("scene_save_settings"))) {
    if (const auto path =
            saveFileDialog(L"Save Scene Selection",
                           L"JSON File (*.json)\0*.json\0All Files "
                           L"(*.*)\0*.*\0",
                           L"scene_selection.json")) {
      session.saveSceneSelectionSettings(*path);
    }
  }
  if (nk_button_label(ctx, tr("scene_load_settings"))) {
    if (const auto path =
            openFileDialog(L"Load Scene Selection",
                           L"JSON File (*.json)\0*.json\0All Files "
                           L"(*.*)\0*.*\0")) {
      session.loadSceneSelectionSettings(*path);
    }
  }
  check(ctx, g, tr("show_preview_grid"), session.show_preview_grid,
        false);
  check(ctx, g, tr("show_preview_axes"), session.show_preview_axes,
        false);
  mutedWrap(ctx, g, tr("preview_scene_independent"));
  check(ctx, g, tr("camera_follow"), session.camera_follow_preview,
        false);
  muted(ctx, g, tr("camera_follow_hint"));
  check(ctx, g, tr("mcbe_coords"), session.use_mcbe_coords, false);
  muted(ctx, g, tr("mcbe_hint"));
  if (check(ctx, g, tr("vsync"), session.vsync_enabled, false)) {
    session.vsync_dirty = true;
  }
  muted(ctx, g, tr("camera_hint"));
  muted(ctx, g, tr("lang"));
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("lang_en"))) {
    setLang(Lang::En);
  }
  if (nk_button_label(ctx, tr("lang_zh_cn"))) {
    setLang(Lang::ZhCn);
  }

}

} // namespace xpbd::app::ui_internal
