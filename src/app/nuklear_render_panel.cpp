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

void drawRendererEditor(nk_context *ctx, const Geom &g, AppSession &session,
                        const gfx::FrameStats &stats,
                        const gfx::RayTracingCapability *rt_cap) {
  const bool busy =
      session.bake_busy.load() || session.stillRenderActive();
  heading(ctx, g, tr("renderer_page"));
  mutedWrap(ctx, g, tr("renderer_page_hint"));

  // Physics owns scene selection and World/Sky owns the sky source. Renderer
  // owns only the engine/integrator/post/display policy.
  heading(ctx, g, tr("pt_engine"));
  const auto availability = gfx::queryVulkanPathTraceAvailability();
  const bool hw_ok = rt_cap != nullptr && rt_cap->supported &&
                     rt_cap->device_extensions_enabled;
  const bool rt_supported = hw_ok || availability.path_tracer_ready;
  if (!rt_supported && session.enable_ray_tracing) {
    session.enable_ray_tracing = false;
  }
  check(ctx, g, tr("enable_ray_tracing"), session.enable_ray_tracing,
        !rt_supported || busy);
  char status_line[320];
  std::snprintf(status_line, sizeof(status_line),
                tr("pt_requested_status"),
                session.enable_ray_tracing ? tr("pt_path_tracing")
                                           : tr("pt_path_raster"));
  muted(ctx, g, status_line);
  if (rt_supported && session.enable_ray_tracing) {
    const auto impl = gfx::selectVulkanPathTraceImplementation(
        true, rt_cap ? *rt_cap : gfx::RayTracingCapability{}, availability);
    const char *active =
        impl == gfx::VulkanPathTraceImplementation::RayTracingPipeline
            ? tr("pt_path_tracing")
            : impl == gfx::VulkanPathTraceImplementation::RayQuery
                  ? tr("pt_path_ray_query")
                  : tr("pt_path_raster");
    std::snprintf(status_line, sizeof(status_line),
                  tr("pt_active_status"), active);
    muted(ctx, g, status_line);
  } else if (!rt_supported) {
    muted(ctx, g, tr("ray_tracing_unsupported"));
    if (rt_cap != nullptr && !rt_cap->unsupported_reason.empty()) {
      mutedWrap(ctx, g, rt_cap->unsupported_reason.c_str());
    }
  } else {
    muted(ctx, g, tr("raster_path_hint"));
  }
  const char *gpu_name =
      rt_cap != nullptr && !rt_cap->device_name.empty()
          ? rt_cap->device_name.c_str()
          : tr("pt_unavailable");
  std::snprintf(status_line, sizeof(status_line), tr("pt_gpu_status"),
                gpu_name);
  mutedWrap(ctx, g, status_line);

  gfx::PathTraceSettings settings = session.path_trace_settings;
  bool settings_changed = false;
  bool preset_parameter_changed = false;
  const bool controls_disabled = busy || !session.enable_ray_tracing;
  const auto changed = [&](bool preset_parameter = true) {
    settings_changed = true;
    preset_parameter_changed =
        preset_parameter_changed || preset_parameter;
  };
  const auto toggle = [&](const char *label, bool &field, bool disabled,
                          bool preset_parameter = true) {
    if (check(ctx, g, label, field, disabled)) {
      changed(preset_parameter);
    }
  };
  const auto uintProperty = [&](const char *label, std::uint32_t &field,
                                int lo, int hi, bool disabled,
                                bool preset_parameter = true) {
    int value = static_cast<int>(std::min<std::uint32_t>(
        field, static_cast<std::uint32_t>(hi)));
    if (intProperty(ctx, g, label, value, lo, hi, 1, disabled)) {
      field = static_cast<std::uint32_t>(std::clamp(value, lo, hi));
      changed(preset_parameter);
    }
  };
  const auto floatSlider = [&](const char *label, float &field, float lo,
                               float hi, float step, bool disabled,
                               bool preset_parameter = true) {
    if (slider(ctx, g, label, field, lo, hi, disabled, step)) {
      changed(preset_parameter);
    }
  };

  const bool nvidia_rt_pipeline_available =
      hw_ok && rt_cap != nullptr && rt_cap->is_nvidia &&
      availability.ray_tracing_pipeline_ready;
  toggle(tr("pt_nvidia_rt_core"),
         settings.nvidia_rt_core_acceleration,
         controls_disabled || !nvidia_rt_pipeline_available, false);
  if (!nvidia_rt_pipeline_available) {
    mutedWrap(ctx, g, tr("pt_nvidia_rt_core_unavailable"));
  } else {
    muted(ctx, g,
          settings.nvidia_rt_core_acceleration &&
                  !settings.force_software_fallback
              ? tr("pt_nvidia_rt_core_active")
              : tr("pt_nvidia_rt_core_inactive"));
  }
  std::snprintf(status_line, sizeof(status_line),
                tr("pt_preview_resolution_status"),
                settings.preview_resolution_scale * 100.0f);
  muted(ctx, g, status_line);

  heading(ctx, g, tr("pt_preset"));
  std::vector<const char *> presets{
      tr("pt_preset_realtime"), tr("pt_preset_balanced"),
      tr("pt_preset_high_quality"), tr("pt_preset_reference"),
      tr("pt_preset_custom")};
  int preset = static_cast<int>(settings.preset);
  const int previous_preset = preset;
  if (combo(ctx, g, tr("pt_preset"), presets, preset,
            controls_disabled) &&
      preset != previous_preset) {
    settings = gfx::applyPathTracePreset(
        settings, static_cast<gfx::PathTracePreset>(preset));
    settings_changed = true;
  }
  const bool can_restore =
      settings.preset == gfx::PathTracePreset::Custom;
  if (!can_restore || controls_disabled) {
    nk_widget_disable_begin(ctx);
  }
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("pt_restore_preset")) &&
      can_restore && !controls_disabled) {
    settings = gfx::restorePathTraceSourcePreset(settings);
    settings_changed = true;
  }
  if (!can_restore || controls_disabled) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("pt_sampling"));
  mutedWrap(ctx, g, tr("path_trace_settings_hint"));
  constexpr std::array<std::uint32_t, 6> kSppValues{
      1u, 2u, 4u, 8u, 16u, 32u};
  std::vector<const char *> spp_labels{"1", "2", "4", "8", "16", "32"};
  int spp_index = 0;
  for (std::size_t i = 0; i < kSppValues.size(); ++i) {
    if (settings.samples_per_frame == kSppValues[i]) {
      spp_index = static_cast<int>(i);
      break;
    }
  }
  const int previous_spp = spp_index;
  if (combo(ctx, g, tr("pt_samples_per_frame"), spp_labels, spp_index,
            controls_disabled) &&
      spp_index != previous_spp) {
    settings.samples_per_frame =
        kSppValues[static_cast<std::size_t>(spp_index)];
    changed();
  }
  uintProperty(tr("pt_max_samples"), settings.maximum_samples,
               0, 65'536, controls_disabled);
  muted(ctx, g, tr("pt_max_samples_hint"));
  toggle(tr("pt_seed_auto"), settings.automatic_seed,
         controls_disabled);
  if (!settings.automatic_seed) {
    uintProperty(tr("pt_seed"), settings.seed, 0, 1'000'000,
                 controls_disabled);
  }
  const bool adaptive_unavailable = true;
  toggle(tr("pt_adaptive"), settings.adaptive_sampling,
         controls_disabled || adaptive_unavailable);
  floatSlider(tr("pt_adaptive_threshold"),
              settings.adaptive_noise_threshold, 0.0001f, 1.0f,
              0.001f, true);
  uintProperty(tr("pt_adaptive_min_samples"),
               settings.adaptive_minimum_samples, 1, 65'536, true);
  mutedWrap(ctx, g, tr("pt_spp_unsupported"));
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("pt_reset_accumulation")) &&
      !controls_disabled) {
    session.resetPathTraceAccumulation();
  }

  heading(ctx, g, tr("pt_light_paths"));
  uintProperty(tr("pt_max_bounces"), settings.max_bounces, 1, 64,
               controls_disabled);
  uintProperty(tr("pt_diffuse_bounces"),
               settings.max_diffuse_bounces, 0, 16,
               controls_disabled);
  uintProperty(tr("pt_glossy_bounces"),
               settings.max_glossy_bounces, 0, 16,
               controls_disabled);
  uintProperty(tr("pt_transmission_bounces"),
               settings.max_transmission_bounces, 0, 32,
               controls_disabled);
  uintProperty(tr("pt_transparent_bounces"),
               settings.max_transparent_bounces, 0, 64,
               controls_disabled);
  mutedWrap(ctx, g, tr("pt_transparent_budget_hint"));
  toggle(tr("pt_russian_roulette"), settings.russian_roulette,
         controls_disabled);
  uintProperty(tr("pt_russian_roulette_start"),
               settings.russian_roulette_start, 1,
               static_cast<int>(settings.max_bounces),
               controls_disabled);

  heading(ctx, g, tr("pt_lighting"));
  toggle(tr("pt_analytic_lights"), settings.analytic_lights,
         controls_disabled);
  toggle(tr("pt_emissive_surfaces"), settings.emissive_surfaces,
         controls_disabled);
  floatSlider(tr("pt_emissive_multiplier"),
              settings.emissive_multiplier, 0.0f, 16.0f, 0.05f,
              controls_disabled);
  uintProperty(tr("pt_light_samples"),
               settings.light_samples_per_path, 1, 16,
               controls_disabled);
  if (check(ctx, g, tr("sky_environment_lighting"),
            session.world_environment.environment_lighting,
            controls_disabled)) {
    session.touchWorldEnvironment();
  }
  if (check(ctx, g, tr("sky_sun_moon_lighting"),
            session.world_environment.sun_moon_lighting,
            controls_disabled)) {
    session.touchWorldEnvironment();
  }
  mutedWrap(ctx, g, tr("pt_world_lighting_hint"));

  heading(ctx, g, tr("pt_denoise_upscale"));
  constexpr std::array<gfx::PathTraceDenoiser, 3> kDenoiserModes{{
      gfx::PathTraceDenoiser::Auto,
      gfx::PathTraceDenoiser::DlssRayReconstruction,
      gfx::PathTraceDenoiser::Raw,
  }};
  std::vector<const char *> denoisers{
      tr("pt_denoiser_auto"), tr("pt_denoiser_rr"),
      tr("pt_denoiser_raw")};
  const auto denoiser_mode_index =
      [&](gfx::PathTraceDenoiser mode) {
        const auto it = std::find(kDenoiserModes.begin(),
                                  kDenoiserModes.end(), mode);
        return it == kDenoiserModes.end()
                   ? static_cast<int>(kDenoiserModes.size() - 1u)
                   : static_cast<int>(
                         std::distance(kDenoiserModes.begin(), it));
      };
  int denoiser =
      denoiser_mode_index(settings.requested_denoiser);
  const int previous_denoiser = denoiser;
  if (combo(ctx, g, tr("pt_denoiser"), denoisers, denoiser, busy) &&
      denoiser != previous_denoiser) {
    settings.requested_denoiser =
        kDenoiserModes[static_cast<std::size_t>(denoiser)];
    changed();
  }
  constexpr std::array<gfx::PathTraceUpscale, 6> kUpscaleModes{{
      gfx::PathTraceUpscale::Off,
      gfx::PathTraceUpscale::Dlaa,
      gfx::PathTraceUpscale::Quality,
      gfx::PathTraceUpscale::Balanced,
      gfx::PathTraceUpscale::Performance,
      gfx::PathTraceUpscale::UltraPerformance,
  }};
  std::vector<const char *> upscalers{
      tr("pt_upscale_off"), tr("pt_upscale_dlaa"),
      tr("pt_upscale_quality"), tr("pt_upscale_balanced"),
      tr("pt_upscale_performance"), tr("pt_upscale_ultra_performance")};
  const auto upscale_mode_index = [&](gfx::PathTraceUpscale mode) {
    const auto it = std::find(
        kUpscaleModes.begin(), kUpscaleModes.end(), mode);
    return it != kUpscaleModes.end()
               ? static_cast<int>(
                     std::distance(kUpscaleModes.begin(), it))
               : 2;
  };
  int upscale = upscale_mode_index(settings.requested_upscale);
  const int previous_upscale = upscale;
  if (combo(ctx, g, tr("pt_upscale"), upscalers, upscale, busy) &&
      upscale != previous_upscale) {
    settings.requested_upscale =
        kUpscaleModes[static_cast<std::size_t>(upscale)];
    changed();
  }

  bool frame_generation =
      settings.requested_frame_generation ==
      gfx::PathTraceFrameGeneration::On;
  if (check(ctx, g, tr("pt_frame_generation"), frame_generation,
            controls_disabled ||
                !session.path_trace_post_process_capabilities
                     .dlss_frame_generation)) {
    settings.requested_frame_generation =
        frame_generation ? gfx::PathTraceFrameGeneration::On
                         : gfx::PathTraceFrameGeneration::Off;
    changed(false);
  }
  if (!session.path_trace_post_process_capabilities
           .dlss_frame_generation) {
    mutedWrap(ctx, g, tr("pt_frame_generation_unavailable"));
  } else if (frame_generation) {
    mutedWrap(ctx, g, tr("pt_fg_vulkan_vsync"));
  }

  constexpr std::array<gfx::PathTraceReflexMode, 3> kReflexModes{{
      gfx::PathTraceReflexMode::Off,
      gfx::PathTraceReflexMode::On,
      gfx::PathTraceReflexMode::OnBoost,
  }};
  std::vector<const char *> reflex_modes{
      tr("pt_reflex_off"), tr("pt_reflex_on"),
      tr("pt_reflex_on_boost")};
  int reflex_mode = static_cast<int>(
      settings.requested_reflex_mode);
  reflex_mode = std::clamp(
      reflex_mode, 0,
      static_cast<int>(kReflexModes.size() - 1u));
  const int previous_reflex_mode = reflex_mode;
  if (combo(ctx, g, tr("pt_reflex"), reflex_modes, reflex_mode,
            controls_disabled ||
                !session.path_trace_post_process_capabilities.reflex) &&
      reflex_mode != previous_reflex_mode) {
    settings.requested_reflex_mode =
        kReflexModes[static_cast<std::size_t>(reflex_mode)];
    changed(false);
  }
  if (!session.path_trace_post_process_capabilities.reflex) {
    mutedWrap(ctx, g, tr("pt_reflex_unavailable"));
  } else if (frame_generation &&
             settings.requested_reflex_mode ==
                 gfx::PathTraceReflexMode::Off) {
    mutedWrap(ctx, g, tr("pt_fg_requires_reflex"));
  }

  const auto post = gfx::resolvePathTracePostProcess(
      settings, session.path_trace_post_process_capabilities);
  std::snprintf(status_line, sizeof(status_line), tr("pt_post_requested"),
                denoisers[static_cast<std::size_t>(
                    denoiser_mode_index(settings.requested_denoiser))],
                upscalers[static_cast<std::size_t>(
                    upscale_mode_index(settings.requested_upscale))]);
  muted(ctx, g, status_line);
  std::snprintf(status_line, sizeof(status_line), tr("pt_post_active"),
                denoisers[static_cast<std::size_t>(
                    denoiser_mode_index(post.active_denoiser))],
                upscalers[static_cast<std::size_t>(
                    upscale_mode_index(post.active_upscale))]);
  muted(ctx, g, status_line);
  if (!post.denoiser_supported || !post.upscale_supported) {
    mutedWrap(ctx, g, tr("pt_post_unavailable"));
    if (!session.path_trace_post_process_status.empty()) {
      std::snprintf(status_line, sizeof(status_line),
                    tr("pt_post_backend_status"),
                    session.path_trace_post_process_status.c_str());
      mutedWrap(ctx, g, status_line);
    }
  }
  if (post.conflict_resolved) {
    mutedWrap(ctx, g, tr("pt_post_conflict"));
  }
  if (post.rr_mode_required) {
    mutedWrap(ctx, g, tr("pt_post_rr_mode_required"));
  }
  if (frame_generation &&
      !session.path_trace_post_process_status.empty()) {
    std::snprintf(status_line, sizeof(status_line),
                  tr("pt_fg_backend_status"),
                  session.path_trace_post_process_status.c_str());
    mutedWrap(ctx, g, status_line);
  }
  if (session.debug_dlss_frame_generation_active) {
    std::snprintf(status_line, sizeof(status_line),
                  tr("pt_fg_fps_status"),
                  session.debug_original_fps,
                  session.debug_dlss_fg_fps);
    muted(ctx, g, status_line);
  } else if (frame_generation) {
    muted(ctx, g, tr("pt_fg_waiting"));
  }

  heading(ctx, g, tr("pt_film"));
  toggle(tr("pt_transparent_background"),
         settings.transparent_background, busy);
  floatSlider(tr("pt_display_exposure"), settings.display_exposure_ev,
              -8.0f, 8.0f, 0.1f, busy);
  std::vector<const char *> tone_maps{
      tr("pt_tone_none"), tr("pt_tone_reinhard"), tr("pt_tone_aces")};
  int tone_map = static_cast<int>(settings.tone_mapping);
  const int previous_tone_map = tone_map;
  if (combo(ctx, g, tr("pt_tone_mapping"), tone_maps, tone_map, busy) &&
      tone_map != previous_tone_map) {
    settings.tone_mapping =
        static_cast<gfx::PathTraceToneMapping>(tone_map);
    changed();
  }
  int white_balance =
      static_cast<int>(settings.white_balance_kelvin);
  if (intProperty(ctx, g, tr("pt_white_balance"), white_balance,
                  1000, 40'000, 50, busy)) {
    settings.white_balance_kelvin =
        static_cast<float>(white_balance);
    changed();
  }
  floatSlider(tr("pt_bloom"), settings.bloom_strength, 0.0f, 4.0f,
              0.05f, busy);
  mutedWrap(ctx, g, tr("pt_film_hint"));

  heading(ctx, g, tr("pt_performance"));
  floatSlider(tr("pt_preview_scale"),
              settings.preview_resolution_scale, 0.25f, 1.0f,
              0.05f, controls_disabled);
  floatSlider(tr("pt_target_frame_time"),
              settings.target_frame_time_ms, 4.0f, 100.0f, 1.0f,
              controls_disabled);
  std::vector<const char *> interactive_qualities{
      tr("pt_interactive_full"), tr("pt_interactive_balanced"),
      tr("pt_interactive_fast")};
  int interactive_quality =
      static_cast<int>(settings.interactive_quality);
  const int previous_interactive_quality = interactive_quality;
  if (combo(ctx, g, tr("pt_interactive_quality"),
            interactive_qualities, interactive_quality,
            controls_disabled) &&
      interactive_quality != previous_interactive_quality) {
    settings.interactive_quality =
        static_cast<gfx::PathTraceInteractiveQuality>(
            interactive_quality);
    changed();
  }
  toggle(tr("pt_accumulate_moving"),
         settings.accumulate_while_moving, true);
  mutedWrap(ctx, g, tr("pt_accumulate_moving_unavailable"));
  toggle(tr("pt_pause_accumulation"), settings.pause_accumulation,
         controls_disabled, false);
  char performance_line[192];
  std::snprintf(performance_line, sizeof(performance_line),
                tr("pt_timing_status"),
                session.debug_backend_cpu_ms, session.debug_gpu_ms,
                static_cast<unsigned long long>(
                    settings.reset_generation));
  muted(ctx, g, performance_line);
  std::snprintf(performance_line, sizeof(performance_line),
                tr("pt_memory_status"),
                static_cast<double>(stats.rt_allocated_bytes) /
                    (1024.0 * 1024.0),
                static_cast<unsigned long long>(
                    settings.target_generation));
  muted(ctx, g, performance_line);

  heading(ctx, g, tr("pt_advanced"));
  toggle(tr("pt_nee"), settings.next_event_estimation,
         controls_disabled);
  toggle(tr("pt_mis"), settings.multiple_importance_sampling,
         controls_disabled);
  toggle(tr("pt_environment_importance"),
         settings.environment_importance_sampling,
         controls_disabled);
  const bool emissive_mesh_available =
      nvidia_rt_pipeline_available &&
      settings.nvidia_rt_core_acceleration &&
      !settings.force_software_fallback;
  toggle(tr("pt_emissive_mesh_sampling"),
         settings.emissive_mesh_sampling,
         controls_disabled || !emissive_mesh_available);
  if (!emissive_mesh_available) {
    mutedWrap(ctx, g, tr("pt_emissive_mesh_unavailable"));
  }
  floatSlider(tr("pt_direct_clamp"), settings.direct_clamp,
              0.0f, 100.0f, 0.25f, controls_disabled);
  floatSlider(tr("pt_indirect_clamp"), settings.indirect_clamp,
              0.0f, 100.0f, 0.25f, controls_disabled);
  mutedWrap(ctx, g, tr("pt_clamp_hint"));

  heading(ctx, g, tr("pt_debug"));
  std::vector<const char *> debug_views{
      tr("pt_debug_off"), tr("pt_debug_instance"),
      tr("pt_debug_primitive"), tr("pt_debug_cube"),
      tr("pt_debug_face"), tr("pt_debug_material"),
      tr("pt_debug_normal"), tr("pt_debug_albedo"),
      tr("pt_debug_roughness"), tr("pt_debug_emission")};
  int debug_view = static_cast<int>(session.rt_debug_view);
  const int previous_debug_view = debug_view;
  if (combo(ctx, g, tr("pt_debug_view"), debug_views, debug_view,
            controls_disabled) &&
      debug_view != previous_debug_view) {
    session.rt_debug_view =
        static_cast<gfx::RtDebugView>(debug_view);
  }
  toggle(tr("pt_developer_controls"), settings.developer_controls,
         busy, false);
  if (settings.developer_controls) {
    toggle(tr("pt_force_compatibility"),
           settings.force_software_fallback,
           controls_disabled, false);

    // Developer-only and deliberately not persisted: this selects only the
    // final composite source and never changes the RR evaluation itself.
    std::vector<const char *> rr_aov_debug_views{
        "Off",
        "Raw Color",
        "RR Output",
        "Device Depth",
        "Linear Depth",
        "Motion",
        "Motion Magnitude",
        "Previous UV Outside",
        "Diffuse Albedo",
        "Specular Albedo",
        "Normal",
        "Roughness",
        "Specular Hit Distance",
        "Reactive Mask",
        "Transparency / Composition",
        "Guide Validity",
        "Temporal Boundary Overlay"};
    int rr_aov_debug_view =
        static_cast<int>(session.rr_aov_debug_view);
    const int previous_rr_aov_debug_view = rr_aov_debug_view;
    if (combo(ctx, g, "RR AOV Debug View", rr_aov_debug_views,
              rr_aov_debug_view, controls_disabled) &&
        rr_aov_debug_view != previous_rr_aov_debug_view) {
      session.rr_aov_debug_view =
          static_cast<gfx::RrAovDebugView>(rr_aov_debug_view);
    }
  }

  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("pt_save_settings")) && !busy) {
    if (const auto path =
            saveFileDialog(L"Save Path Tracing Settings",
                           L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0",
                           L"path_tracing.json")) {
      session.savePathTraceSettings(*path);
    }
  }
  if (nk_button_label(ctx, tr("pt_load_settings")) && !busy) {
    if (const auto path = openFileDialog(
            L"Load Path Tracing Settings",
            L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0")) {
      session.loadPathTraceSettings(*path);
      settings = session.path_trace_settings;
      settings_changed = false;
      preset_parameter_changed = false;
    }
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("pt_freeze_snapshot")) && !busy) {
    session.freezePathTraceRenderSnapshot();
  }
  const bool has_snapshot =
      session.path_trace_render_snapshot.has_value();
  if (!has_snapshot) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("pt_clear_snapshot")) &&
      has_snapshot) {
    session.clearPathTraceRenderSnapshot();
  }
  if (!has_snapshot) {
    nk_widget_disable_end(ctx);
  }
  muted(ctx, g, has_snapshot ? tr("pt_snapshot_frozen")
                             : tr("pt_snapshot_live"));

  heading(ctx, g, tr("still_render"));
  mutedWrap(ctx, g, tr("still_render_hint"));
  mutedWrap(ctx, g, tr("still_raw_accumulation"));
  auto &still_job = session.still_render_job;
  const bool still_active = session.stillRenderActive();
  const bool still_controls_disabled =
      still_active || session.bake_busy.load();
  const StillSettingsPanelLabels still_labels{
      tr("still_filename"),
      tr("still_width"),
      tr("still_height"),
      tr("still_target_samples"),
      tr("still_samples_per_submit"),
      tr("still_format"),
      tr("still_format_png"),
      tr("still_format_exr"),
      tr("still_transparent_background"),
  };
  const StillSettingsPanelGeometry still_geometry{g.s, g.btn, g.row};
  (void)composeStillRenderSettingsPanel(
      ctx, still_job.settings, still_labels, still_geometry,
      still_controls_disabled);
  const std::string output_directory =
      session.stillRenderOutputDirectory().string();
  char output_line[512]{};
  std::snprintf(output_line, sizeof(output_line),
                tr("still_output_directory"),
                output_directory.empty() ? "-" : output_directory.c_str());
  mutedWrap(ctx, g, output_line);

  const bool can_start_still =
      !still_controls_disabled && session.enable_ray_tracing &&
      rt_supported &&
      session.path_trace_settings.nvidia_rt_core_acceleration &&
      !session.path_trace_settings.force_software_fallback;
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (!can_start_still) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("still_start")) && can_start_still) {
    session.queueStillRender();
  }
  if (!can_start_still) {
    nk_widget_disable_end(ctx);
  }
  if (!still_active) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("still_cancel")) && still_active) {
    session.requestStillRenderCancel();
  }
  if (!still_active) {
    nk_widget_disable_end(ctx);
  }
  if (!session.enable_ray_tracing || !rt_supported) {
    mutedWrap(ctx, g, tr("still_requires_rt"));
  }

  const auto &still_status = still_job.status;
  if (still_status.state != gfx::StillRenderJobState::Idle) {
    char progress_line[256]{};
    const double progress =
        still_status.target_samples > 0u
            ? 100.0 * static_cast<double>(
                          (std::min)(still_status.accumulated_samples,
                                     still_status.target_samples)) /
                  static_cast<double>(still_status.target_samples)
            : 0.0;
    std::snprintf(
        progress_line, sizeof(progress_line), tr("still_progress"),
        still_status.accumulated_samples, still_status.target_samples,
        progress);
    muted(ctx, g, progress_line);
    if (still_status.state == gfx::StillRenderJobState::Saving) {
      muted(ctx, g, tr("still_saving"));
    } else if (still_status.state ==
               gfx::StillRenderJobState::Completed) {
      muted(ctx, g, tr("still_completed"));
      mutedWrap(ctx, g, still_status.output_path.c_str());
    } else if (still_status.state ==
               gfx::StillRenderJobState::Cancelled) {
      muted(ctx, g, tr("still_cancelled"));
    } else if (still_status.state ==
               gfx::StillRenderJobState::Failed) {
      muted(ctx, g, tr("still_failed"));
      mutedWrap(ctx, g, still_status.error.c_str());
    }
  }

  if (settings_changed) {
    if (preset_parameter_changed &&
        settings.preset != gfx::PathTracePreset::Custom) {
      settings.source_preset = settings.preset;
      settings.preset = gfx::PathTracePreset::Custom;
    }
    (void)session.applyPathTraceSettings(settings);
  }
}



void drawRenderPanel(UiPanelContext &ui) {
  drawRendererEditor(ui.nk, ui.geom, ui.session, ui.stats, ui.rt_cap);
}

} // namespace xpbd::app::ui_internal
