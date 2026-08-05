    // Built-in path tracer dispatch (RT Pipeline preferred, before the
    // graphics pass).
    PathTraceFrameParams pt_params{};
    bool pt_dlss_active = false;
    bool pt_target_exact_for_frame = false;
    bool pt_motion_guide_ready_for_frame = false;
    bool pt_dispatch_recorded_for_frame = false;
    bool pt_temporal_reconstruction_requested_for_frame = false;
    bool pt_temporal_inputs_ready_for_frame = false;
    std::uint32_t pt_temporal_required_output_mask_for_frame = 0u;
    std::uint32_t pt_actual_output_write_mask_for_frame = 0u;
    bool pt_history_reset = true;
    bool pt_streamline_history_reset = true;
    TemporalReconstructionResetReason pt_streamline_history_reset_reason =
        TemporalReconstructionResetReason::FirstFrame;
    std::uint64_t pt_streamline_history_key = 0u;
    PathTracePostProcessCapabilities pt_post_capabilities{};
    pt_post_capabilities.dlss_ray_reconstruction =
        streamline_vulkan_runtime_.dlssRayReconstructionSupported();
    pt_post_capabilities.dlss_super_resolution =
        streamline_vulkan_runtime_.dlssSupported();
    pt_post_capabilities.dlaa =
        streamline_vulkan_runtime_.dlssSupported();
    pt_post_capabilities.dlss_frame_generation =
        frame_generation_supported;
    pt_post_capabilities.reflex =
        streamline_vulkan_runtime_.reflexSupported();
    PathTracePostProcessState pt_post{};
    if (path_trace_active && draw_viewport) {
      PathTraceSettings path_settings =
          normalizePathTraceSettings(frame.path_trace_settings);
      if (interactive_preview_resize) {
        // Splitter motion produces a stream of temporary output extents. Keep
        // the current PT images alive, render one responsive raw sample, and
        // restart temporal reconstruction once the final extent is known.
        path_settings.samples_per_frame = 1u;
        streamline_vulkan_runtime_.invalidateDlssHistory();
        streamline_temporal_history_valid_ = false;
      }
      pt_post = resolvePathTracePostProcess(
          path_settings, pt_post_capabilities);
      const std::uint32_t preview_output_width =
          static_cast<std::uint32_t>((std::max)(1, safe_viewport.w));
      const std::uint32_t preview_output_height =
          static_cast<std::uint32_t>((std::max)(1, safe_viewport.h));
      StreamlineDlssOptimalSettings dlss_settings{};
      bool pt_rr_requested =
          pt_post.active_denoiser ==
          PathTraceDenoiser::DlssRayReconstruction;
      const std::uint32_t supported_target_outputs =
          path_tracer.supportedTargetOutputMask();
      const std::uint32_t requested_temporal_output_mask =
          pt_rr_requested
              ? kPathTraceRrRequiredOutputMask
              : (pt_post.active_upscale != PathTraceUpscale::Off
                     ? kPathTraceSrRequiredOutputMask
                     : 0u);
      if (requested_temporal_output_mask != 0u &&
          (supported_target_outputs & requested_temporal_output_mask) !=
              requested_temporal_output_mask) {
        if (!streamline_rr_target_format_failure_logged_) {
          xpbd::log::warnf(
              "%s disabled: required target formats unsupported "
              "(required=0x%05x supported=0x%05x)",
              pt_rr_requested ? "DLSS Ray Reconstruction"
                              : "DLSS Super Resolution",
              static_cast<unsigned>(requested_temporal_output_mask),
              static_cast<unsigned>(supported_target_outputs));
          streamline_rr_target_format_failure_logged_ = true;
        }
        if (pt_rr_requested) {
          pt_rr_requested = false;
          pt_post.active_denoiser = PathTraceDenoiser::Raw;
          pt_post.reconstruction_mode = PathTraceUpscale::Off;
        } else {
          pt_post.active_upscale = PathTraceUpscale::Off;
        }
        streamline_vulkan_runtime_.invalidateDlssHistory();
      } else {
        streamline_rr_target_format_failure_logged_ = false;
      }
      if (!interactive_preview_resize && pt_rr_requested) {
        dlss_settings =
            streamline_vulkan_runtime_
                .queryDlssRayReconstructionOptimalSettings(
                    pt_post.reconstruction_mode,
                    preview_output_width, preview_output_height);
        if (!dlss_settings.valid) {
          if (!streamline_dlss_failure_logged_) {
            xpbd::log::warnf(
                "DLSS Ray Reconstruction mode unavailable: %s",
                streamline_vulkan_runtime_.status().c_str());
            streamline_dlss_failure_logged_ = true;
          }
          pt_rr_requested = false;
          pt_post.active_denoiser = PathTraceDenoiser::Raw;
          pt_post.reconstruction_mode = PathTraceUpscale::Off;
          streamline_vulkan_runtime_.invalidateDlssHistory();
        }
      } else if (!interactive_preview_resize &&
                 pt_post.active_upscale != PathTraceUpscale::Off) {
        dlss_settings =
            streamline_vulkan_runtime_.queryDlssOptimalSettings(
                pt_post.active_upscale, preview_output_width,
                preview_output_height);
        if (!dlss_settings.valid) {
          if (!streamline_dlss_failure_logged_) {
            xpbd::log::warnf(
                "DLSS Super Resolution mode unavailable: %s",
                streamline_vulkan_runtime_.status().c_str());
            streamline_dlss_failure_logged_ = true;
          }
          pt_post.active_upscale = PathTraceUpscale::Off;
          streamline_vulkan_runtime_.invalidateDlssHistory();
        }
      }
      pt_temporal_reconstruction_requested_for_frame =
          dlss_settings.valid &&
          (pt_rr_requested ||
           pt_post.active_upscale != PathTraceUpscale::Off);
      pt_temporal_required_output_mask_for_frame =
          dlss_settings.valid
              ? (pt_rr_requested ? kPathTraceRrRequiredOutputMask
                                 : kPathTraceSrRequiredOutputMask)
              : 0u;
      const float preview_scale =
          path_settings.preview_resolution_scale;
      const std::uint32_t requested_ptw =
          dlss_settings.valid
              ? dlss_settings.render_width
              : static_cast<std::uint32_t>((std::max)(
                    1, static_cast<int>(std::lround(
                           static_cast<float>(safe_viewport.w) *
                           preview_scale))));
      const std::uint32_t requested_pth =
          dlss_settings.valid
              ? dlss_settings.render_height
              : static_cast<std::uint32_t>((std::max)(
                    1, static_cast<int>(std::lround(
                           static_cast<float>(safe_viewport.h) *
                           preview_scale))));
      const PathTraceTargetExtent target_extent =
          choosePathTraceTargetExtent(
              requested_ptw, requested_pth, path_tracer.targetWidth(),
              path_tracer.targetHeight(), interactive_preview_resize);
      std::uint32_t requested_output_mask = 0u;
      if (desired_frame_generation && !interactive_preview_resize) {
        requested_output_mask |= kPathTraceRrMotionOutputMask;
      }
      requested_output_mask |=
          pt_temporal_required_output_mask_for_frame;
      requested_output_mask |=
          rrAovDebugRequiredOutputMask(frame.rr_aov_debug_view);
      const PathTraceTargetResult target_result = path_tracer.ensureTarget(
          target_extent.width, target_extent.height,
          PathTraceTargetRequirements{requested_output_mask});
      if (target_result.failure.vk_result == VK_ERROR_DEVICE_LOST) {
        enterFatalVulkanError(frame, "VulkanPathTracer::ensureTarget",
                              VK_ERROR_DEVICE_LOST);
        return;
      }
      const bool target_has_requested_outputs =
          (target_result.allocated_output_mask & requested_output_mask) ==
          requested_output_mask;
      if (target_result.hasActiveTarget()) {
        const std::uint32_t ptw = target_result.active_width;
        const std::uint32_t pth = target_result.active_height;
        if (!target_result.exact() || !target_has_requested_outputs) {
          // Candidate failure keeps the last good color/depth alive. Avoid
          // feeding mismatched extents or dummy guides to Streamline while the
          // same-key retry backoff is active; composite the usable raw target.
          dlss_settings = {};
          pt_post.active_denoiser = PathTraceDenoiser::Raw;
          pt_post.active_upscale = PathTraceUpscale::Off;
          requested_output_mask = 0u;
          pt_temporal_required_output_mask_for_frame = 0u;
          pt_temporal_reconstruction_requested_for_frame = false;
          streamline_vulkan_runtime_.invalidateDlssHistory();
          streamline_temporal_history_valid_ = false;
        }
        pt_target_exact_for_frame = target_result.exact();
        pt_motion_guide_ready_for_frame =
            target_result.exact() &&
            (requested_output_mask & kPathTraceRrMotionOutputMask) != 0u &&
            (target_result.allocated_output_mask &
             kPathTraceRrMotionOutputMask) != 0u;
        pt_params.view = frame.view_matrix;
        pt_params.proj = frame.proj_matrix;
        pt_params.previous_view =
            rt_motion_camera_history_valid_
                ? rt_motion_previous_view_.data()
                : nullptr;
        pt_params.previous_proj =
            rt_motion_camera_history_valid_
                ? rt_motion_previous_proj_.data()
                : nullptr;
        pt_params.motion_history_valid =
            rt_motion_camera_history_valid_;
        pt_params.temporal_reconstruction_input = dlss_settings.valid;
        pt_params.ray_reconstruction_guides =
            dlss_settings.valid && pt_rr_requested;
        pt_params.reconstruction_mip_bias = reconstructionMipBias(
            ptw, pth, preview_output_width, preview_output_height,
            pt_params.temporal_reconstruction_input);
        pt_params.output_write_mask = requested_output_mask;
        if (frame.raster_scene != nullptr) {
          pt_params.light_dir[0] = frame.raster_scene->lighting.direction[0];
          pt_params.light_dir[1] = frame.raster_scene->lighting.direction[1];
          pt_params.light_dir[2] = frame.raster_scene->lighting.direction[2];
          pt_params.ambient = frame.raster_scene->lighting.ambient;
          pt_params.light_color[0] = frame.raster_scene->lighting.color[0];
          pt_params.light_color[1] = frame.raster_scene->lighting.color[1];
          pt_params.light_color[2] = frame.raster_scene->lighting.color[2];
          pt_params.intensity = frame.raster_scene->lighting.intensity;
        }
        // These fields are consumed only by the Full RT pipeline. The legacy
        // fields above intentionally retain path_trace.comp behavior.
        pt_params.sun_radiance[0] = resolved_sun_light.radiance[0];
        pt_params.sun_radiance[1] = resolved_sun_light.radiance[1];
        pt_params.sun_radiance[2] = resolved_sun_light.radiance[2];
        pt_params.sun_angular_radius =
            resolved_sun_light.lighting_enabled
                ? resolved_sun_light.angular_radius
                : 0.0f;
        pt_params.sun_casts_shadow = resolved_sun_light.casts_shadow;
        pt_params.clear_r = frame.clear_r;
        pt_params.clear_g = frame.clear_g;
        pt_params.clear_b = frame.clear_b;
        pt_params.width = ptw;
        pt_params.height = pth;
        pt_params.frame_index = path_trace_frame_index_++;
        if (pt_params.temporal_reconstruction_input) {
          const auto jitter = pathTraceTemporalJitter(
              pt_params.frame_index, ptw, preview_output_width);
          pt_params.camera_jitter[0] = jitter[0];
          pt_params.camera_jitter[1] = jitter[1];
        }
        pt_params.frame_slot =
            static_cast<std::uint32_t>(frame_index_);
        pt_params.settings = path_settings;
        pt_params.output_requires_srgb_encoding =
            swap_format_ == VK_FORMAT_B8G8R8A8_UNORM ||
            swap_format_ == VK_FORMAT_R8G8B8A8_UNORM;
        pt_params.exposure =
            std::exp2(pt_params.settings.display_exposure_ev);
        if (importance_environment_ready) {
          pt_params.settings.analytic_environment_strength = 0.0f;
        }
        pt_params.material_debug_view = frame.material_debug_view;
        pt_params.rt_debug_view = frame.rt_debug_view;
        pt_params.rr_aov_debug_view = frame.rr_aov_debug_view;
        pt_params.material_feature_flags =
            labPbrFeatureFlags(frame.static_model_material);
        if (procedural_environment_ready) {
          // Preserve the compatibility compute path's existing suppression;
          // Full RT reads the separate finite-Sun fields above.
          pt_params.ambient = 0.0f;
          pt_params.intensity = 0.0f;
        }
        pt_params.hdr_environment = importance_environment_ready;
        pt_params.environment_view =
            hdr_environment_ready
                ? world_environment_texture_.view
                : (procedural_environment_ready
                       ? atmosphere_environment_cache_.view
                       : VK_NULL_HANDLE);
        pt_params.environment_sampler =
            importance_environment_ready ? world_environment_sampler_
                                         : VK_NULL_HANDLE;
        pt_params.environment_distribution =
            hdr_environment_ready
                ? world_environment_distribution_.buffer
                : (procedural_environment_ready
                       ? atmosphere_environment_distribution_.buffer
                       : VK_NULL_HANDLE);
        pt_params.environment_distribution_bytes =
            hdr_environment_ready
                ? world_environment_distribution_bytes_
                : (procedural_environment_ready
                       ? atmosphere_environment_distribution_bytes_
                       : 0u);
        pt_params.viewport_x = safe_viewport.x;
        pt_params.viewport_y = safe_viewport.y;
        pt_params.viewport_w = safe_viewport.w;
        pt_params.viewport_h = safe_viewport.h;
        // Raw and debug views are written directly to color/depth. Only
        // temporal reconstruction asks for motion/RR guides; AOV/statistics
        // stay lazy until an explicit export/inspection consumer requests
        // them.

        // The key contains only radiance/visibility-affecting state. SPP and
        // maximum_samples intentionally remain compatible with the current
        // average and are handled by the slot-local accumulation transition.
        std::uint64_t history_key = 14695981039346656037ull;
        appendPathTraceHistoryValue(history_key,
                                    frame.static_model_generation);
        appendPathTraceHistoryValue(history_key,
                                    frame.static_texture_generation);
        appendPathTraceHistoryValue(history_key, safe_viewport.x);
        appendPathTraceHistoryValue(history_key, safe_viewport.y);
        appendPathTraceHistoryValue(history_key, ptw);
        appendPathTraceHistoryValue(history_key, pth);
        appendPathTraceHistoryBytes(history_key, pt_params.light_dir,
                                    sizeof(pt_params.light_dir));
        appendPathTraceHistoryValue(history_key, pt_params.ambient);
        appendPathTraceHistoryBytes(history_key, pt_params.light_color,
                                    sizeof(pt_params.light_color));
        appendPathTraceHistoryValue(history_key, pt_params.intensity);
        appendPathTraceHistoryBytes(history_key, pt_params.sun_radiance,
                                    sizeof(pt_params.sun_radiance));
        appendPathTraceHistoryValue(history_key,
                                    pt_params.sun_angular_radius);
        appendPathTraceHistoryValue(history_key,
                                    pt_params.sun_casts_shadow);
        appendPathTraceHistoryValue(history_key, pt_params.clear_r);
        appendPathTraceHistoryValue(history_key, pt_params.clear_g);
        appendPathTraceHistoryValue(history_key, pt_params.clear_b);
        appendPathTraceHistoryValue(
            history_key,
            static_cast<std::uint32_t>(pt_params.material_debug_view));
        appendPathTraceHistoryValue(
            history_key,
            static_cast<std::uint32_t>(pt_params.rt_debug_view));
        appendPathTraceHistoryValue(
            history_key, pt_params.material_feature_flags);
        appendPathTraceHistoryValue(
            history_key, pt_params.reconstruction_mip_bias);
        appendPathTraceHistoryValue(
            history_key, pt_params.output_write_mask);
        appendPathTraceHistoryValue(
            history_key, resolvedPathTraceSeed(pt_params.settings));
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_diffuse_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_glossy_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_transmission_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.max_transparent_bounces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.russian_roulette_start);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.russian_roulette);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.analytic_lights);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.emissive_surfaces);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.next_event_estimation);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.multiple_importance_sampling);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.environment_importance_sampling);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.emissive_mesh_sampling);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.emissive_multiplier);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.light_samples_per_path);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.direct_clamp);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.indirect_clamp);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.nvidia_rt_core_acceleration);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.force_software_fallback);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.reset_generation);
        appendPathTraceHistoryValue(
            history_key, pt_params.settings.target_generation);
        appendPathTraceHistoryValue(
            history_key,
            pt_params.settings.analytic_environment_strength);
        appendPathTraceHistoryValue(
            history_key,
            static_cast<std::uint32_t>(
                resolved_world_environment.sky_rendering));
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.lighting_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.celestial_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.cloud_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.target_generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.generation);
        appendPathTraceHistoryValue(history_key,
                                    rt_light_registry.generation);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.environment_lighting);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.sun_moon_lighting);
        appendPathTraceHistoryValue(
            history_key,
            resolved_world_environment.global_lighting_strength);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.environment_strength);
        appendPathTraceHistoryValue(
            history_key, resolved_world_environment.rotation_radians);
        if (resolved_world_environment.hdr != nullptr) {
          appendPathTraceHistoryBytes(
              history_key,
              resolved_world_environment.hdr->checksum.data(),
              resolved_world_environment.hdr->checksum.size());
        }
        if (procedural_environment_ready) {
          appendPathTraceHistoryBytes(
              history_key, atmosphere_environment_key_.data(),
              atmosphere_environment_key_.size());
        }

        // Streamline temporal compatibility intentionally excludes current
        // camera matrices and deforming vertex positions. Dense motion vectors
        // describe ordinary camera, skeletal, and preview-surface motion. Only
        // topology/material/radiance changes, a missing motion history, or a
        // post-process/output reconfiguration starts a new network history.
        std::uint64_t streamline_history_key = history_key;
        appendPathTraceHistoryValue(
            streamline_history_key, rt_streamline_topology_hash);
        appendPathTraceHistoryValue(
            streamline_history_key, rt_streamline_material_hash);
        if (frame.raster_scene != nullptr) {
          appendPathTraceHistoryValue(
              streamline_history_key,
              static_cast<std::uint32_t>(frame.raster_scene->id));
        }
        appendPathTraceHistoryValue(
            streamline_history_key,
            pt_params.settings.post_process_generation);
        appendPathTraceHistoryValue(
            streamline_history_key, pt_rr_requested);
        appendPathTraceHistoryValue(
            streamline_history_key,
            static_cast<std::uint32_t>(
                pt_rr_requested ? pt_post.reconstruction_mode
                                : pt_post.active_upscale));
        appendPathTraceHistoryValue(
            streamline_history_key, preview_output_width);
        appendPathTraceHistoryValue(
            streamline_history_key, preview_output_height);
        pt_streamline_history_key = streamline_history_key;
        const bool streamline_resolution_changed =
            streamline_temporal_output_width_ != 0u &&
            (streamline_temporal_render_width_ != ptw ||
             streamline_temporal_render_height_ != pth ||
             streamline_temporal_output_width_ != preview_output_width ||
             streamline_temporal_output_height_ != preview_output_height);
        const PathTraceUpscale streamline_mode =
            pt_rr_requested ? pt_post.reconstruction_mode
                            : pt_post.active_upscale;
        const bool streamline_mode_changed =
            streamline_temporal_output_width_ != 0u &&
            (streamline_temporal_ray_reconstruction_ != pt_rr_requested ||
             streamline_temporal_mode_ != streamline_mode);
        const bool streamline_explicit_discontinuity =
            streamline_temporal_output_width_ != 0u &&
            streamline_temporal_reset_generation_ !=
                pt_params.settings.reset_generation;
        pt_streamline_history_reset_reason =
            temporalReconstructionResetReason(
                TemporalReconstructionResetInput{
                    streamline_temporal_history_valid_,
                    streamline_temporal_history_key_,
                    pt_streamline_history_key,
                    pt_params.motion_history_valid,
                    streamline_resolution_changed,
                    streamline_mode_changed,
                    streamline_temporal_recovery_pending_,
                    streamline_explicit_discontinuity});
        pt_streamline_history_reset =
            pt_streamline_history_reset_reason !=
            TemporalReconstructionResetReason::None;
        if (diagnostics_enabled_ &&
            pt_temporal_reconstruction_requested_for_frame &&
            diagnostic_sl_history_frames_logged_ < 2048u) {
          xpbd::log::infof(
              "SL history: feature=%s reset=%d history_valid=%d "
              "motion_valid=%d reason=%s frame=%u key=%016llx "
              "previous_key=%016llx",
              pt_rr_requested ? "RR" : "SR",
              pt_streamline_history_reset ? 1 : 0,
              streamline_temporal_history_valid_ ? 1 : 0,
              pt_params.motion_history_valid ? 1 : 0,
              temporalReconstructionResetReasonName(
                  pt_streamline_history_reset_reason),
              pt_params.frame_index,
              static_cast<unsigned long long>(pt_streamline_history_key),
              static_cast<unsigned long long>(
                  streamline_temporal_history_key_));
          ++diagnostic_sl_history_frames_logged_;
        }

        // Raw PT accumulation has stricter compatibility: any camera or
        // deforming-scene change invalidates the per-pixel sample average.
        appendPathTraceHistoryValue(history_key, last_rt_scene_hash);
        if (frame.raster_scene != nullptr) {
          appendPathTraceHistoryValue(
              history_key, frame.raster_scene->geometry_generation);
        }
        if (frame.view_matrix != nullptr) {
          appendPathTraceHistoryBytes(history_key, frame.view_matrix,
                                      sizeof(float) * 16u);
        }
        if (frame.proj_matrix != nullptr) {
          appendPathTraceHistoryBytes(history_key, frame.proj_matrix,
                                      sizeof(float) * 16u);
        }
        pt_params.history_key = history_key;

        // Performance controls affect only the dispatch schedule, never the
        // compatibility key. Use the previous completed GPU timing to reduce
        // work toward the target frame time, and use the selected interactive
        // tier for the first sample after camera/scene motion.
        if (stats_.gpu_timestamp_valid &&
            stats_.gpu_timestamp_total_ms >
                pt_params.settings.target_frame_time_ms * 1.05f) {
          const float ratio =
              pt_params.settings.target_frame_time_ms /
              (std::max)(stats_.gpu_timestamp_total_ms, 0.001f);
          pt_params.settings.samples_per_frame =
              (std::max)(1u, static_cast<std::uint32_t>(
                                std::floor(
                                    static_cast<float>(
                                        pt_params.settings
                                            .samples_per_frame) *
                                    std::clamp(ratio, 0.05f, 1.0f))));
        }
        if (path_tracer.historyKey() != history_key) {
          switch (pt_params.settings.interactive_quality) {
          case PathTraceInteractiveQuality::Fast:
            pt_params.settings.samples_per_frame = 1u;
            break;
          case PathTraceInteractiveQuality::Balanced:
            pt_params.settings.samples_per_frame =
                (std::min)(pt_params.settings.samples_per_frame, 2u);
            break;
          case PathTraceInteractiveQuality::Full:
            break;
          }
        }
        if (interactive_preview_resize) {
          pt_params.settings.samples_per_frame = 1u;
        }
        pt_history_reset =
            path_tracer.historyKey() != history_key ||
            !pt_params.motion_history_valid;
        if (pt_history_reset && diagnostics_enabled_ &&
            diagnostic_pt_history_resets_logged_ < 128u) {
          std::uint64_t view_hash = 14695981039346656037ull;
          std::uint64_t projection_hash = 14695981039346656037ull;
          std::uint64_t light_hash = 14695981039346656037ull;
          if (frame.view_matrix != nullptr) {
            appendPathTraceHistoryBytes(
                view_hash, frame.view_matrix, sizeof(float) * 16u);
          }
          if (frame.proj_matrix != nullptr) {
            appendPathTraceHistoryBytes(
                projection_hash, frame.proj_matrix, sizeof(float) * 16u);
          }
          appendPathTraceHistoryBytes(
              light_hash, pt_params.light_dir,
              sizeof(pt_params.light_dir));
          appendPathTraceHistoryValue(light_hash, pt_params.ambient);
          appendPathTraceHistoryBytes(
              light_hash, pt_params.light_color,
              sizeof(pt_params.light_color));
          appendPathTraceHistoryValue(light_hash, pt_params.intensity);
          const std::uint64_t raster_generation =
              frame.raster_scene != nullptr
                  ? frame.raster_scene->geometry_generation
                  : 0u;
          xpbd::log::infof(
              "VKDIAG pt_history_components frame=%u slot=%u "
              "key=%016llx rt=%016llx view=%016llx proj=%016llx "
              "static=%llu/%llu raster=%llu settings=%llu/%llu "
              "world=%llu/%llu/%llu/%llu/%llu light=%016llx "
              "motion=%d sl_key=%016llx sl_reset=%d sl_reason=%s "
              "sl_valid=%d",
              pt_params.frame_index,
              static_cast<unsigned>(frame_index_),
              static_cast<unsigned long long>(history_key),
              static_cast<unsigned long long>(last_rt_scene_hash),
              static_cast<unsigned long long>(view_hash),
              static_cast<unsigned long long>(projection_hash),
              static_cast<unsigned long long>(
                  frame.static_model_generation),
              static_cast<unsigned long long>(
                  frame.static_texture_generation),
              static_cast<unsigned long long>(raster_generation),
              static_cast<unsigned long long>(
                  pt_params.settings.reset_generation),
              static_cast<unsigned long long>(
                  pt_params.settings.target_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.lighting_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.celestial_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.cloud_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.target_generation),
              static_cast<unsigned long long>(
                  resolved_world_environment.generation),
              static_cast<unsigned long long>(light_hash),
              pt_params.motion_history_valid ? 1 : 0,
              static_cast<unsigned long long>(
                  pt_streamline_history_key),
              pt_streamline_history_reset ? 1 : 0,
              temporalReconstructionResetReasonName(
                  pt_streamline_history_reset_reason),
              streamline_temporal_history_valid_ ? 1 : 0);
          ++diagnostic_pt_history_resets_logged_;
        }

        // Sample the static model atlas so path-traced models keep textures
        // (avoids white clay look). Magnification stays nearest. Albedo/Normal
        // minification uses semantic linear mips; packed Specular remains
        // nearest to preserve its discrete LabPBR channel codes.
        const VkImageView albedo_view =
            static_texture_.view != VK_NULL_HANDLE ? static_texture_.view
                                                   : VK_NULL_HANDLE;
        const VkImageView normal_view =
            static_normal_texture_.view != VK_NULL_HANDLE
                ? static_normal_texture_.view
                : VK_NULL_HANDLE;
        const VkImageView specular_view =
            static_specular_texture_.view != VK_NULL_HANDLE
                ? static_specular_texture_.view
                : VK_NULL_HANDLE;
        write_timestamp(GpuTimestampQuery::PathTraceBegin,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (target_result.exact()) {
          path_tracer.recordDispatch(fs.cmd, rt_scene, pt_params, albedo_view,
                                     normal_view, specular_view,
                                     static_albedo_sampler_,
                                     static_normal_sampler_,
                                     static_specular_sampler_);
          pt_dispatch_recorded_for_frame =
              path_tracer.lastDispatchRecorded();
          pt_actual_output_write_mask_for_frame =
              pt_dispatch_recorded_for_frame
                  ? path_tracer.lastOutputWriteMask()
                  : 0u;
          stats_.rt_aov_write_mask =
              pt_actual_output_write_mask_for_frame;
        } else {
          pt_actual_output_write_mask_for_frame = 0u;
          stats_.rt_aov_write_mask = 0u;
        }
        const VulkanPathTracer::DescriptorStats path_descriptor_stats =
            path_tracer.descriptorStats();
        stats_.rt_descriptor_write_calls += path_descriptor_stats.write_calls;
        stats_.rt_descriptor_cache_hits += path_descriptor_stats.cache_hits;
        stats_.rt_descriptor_entries_written +=
            path_descriptor_stats.entries_written;
        stats_.cpu_descriptor_update_ms += path_descriptor_stats.update_ms;
        write_timestamp(GpuTimestampQuery::PathTraceEnd,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        pt_temporal_inputs_ready_for_frame =
            dlss_settings.valid && pt_target_exact_for_frame &&
            pt_dispatch_recorded_for_frame &&
            pt_temporal_required_output_mask_for_frame != 0u &&
            (pt_actual_output_write_mask_for_frame &
             pt_temporal_required_output_mask_for_frame) ==
                pt_temporal_required_output_mask_for_frame;
        if (pt_temporal_inputs_ready_for_frame) {
          streamline_transparency_contract_failure_logged_ = false;
          auto fill_dlss_frame =
              [&](StreamlineDlssFrame &dlss_frame,
                  PathTraceUpscale mode) {
            dlss_frame.command_buffer = fs.cmd;
            dlss_frame.color_image = path_tracer.colorImage();
            dlss_frame.color_memory = path_tracer.colorMemory();
            dlss_frame.color_view = path_tracer.colorView();
            dlss_frame.depth_image = path_tracer.depthImage();
            dlss_frame.depth_memory = path_tracer.depthMemory();
            dlss_frame.depth_view = path_tracer.depthView();
            // DLSS SR and RR both require an exact RG16F/RG32F dense-motion
            // input. Reuse the dedicated RG32F guide instead of the wider
            // diagnostic AOV layer.
            dlss_frame.motion_image = path_tracer.rrMotionImage();
            dlss_frame.motion_memory = path_tracer.rrMotionMemory();
            dlss_frame.motion_view = path_tracer.rrMotionView();
            dlss_frame.transparency_and_composition_image =
                path_tracer.transparencyAndCompositionImage();
            dlss_frame.transparency_and_composition_memory =
                path_tracer.transparencyAndCompositionMemory();
            dlss_frame.transparency_and_composition_view =
                path_tracer.transparencyAndCompositionView();
            dlss_frame.reactive_mask_image =
                path_tracer.reactiveMaskImage();
            dlss_frame.reactive_mask_memory =
                path_tracer.reactiveMaskMemory();
            dlss_frame.reactive_mask_view =
                path_tracer.reactiveMaskView();
            dlss_frame.guide_validity_image =
                path_tracer.guideValidityImage();
            dlss_frame.guide_validity_memory =
                path_tracer.guideValidityMemory();
            dlss_frame.guide_validity_view =
                path_tracer.guideValidityView();
            dlss_frame.view = pt_params.view;
            dlss_frame.projection = pt_params.proj;
            dlss_frame.previous_view = pt_params.previous_view;
            dlss_frame.previous_projection =
                pt_params.previous_proj;
            dlss_frame.render_width = ptw;
            dlss_frame.render_height = pth;
            dlss_frame.output_width = preview_output_width;
            dlss_frame.output_height = preview_output_height;
            // Streamline, Reflex, SR/RR, FG, and present must share the same
            // application-frame token. The path-trace frame index remains a
            // separate temporal sampling sequence.
            dlss_frame.frame_index = static_cast<std::uint32_t>(
                frame.diagnostics.render_frame);
            dlss_frame.frame_slot = pt_params.frame_slot;
            dlss_frame.jitter_x = pt_params.camera_jitter[0];
            dlss_frame.jitter_y = pt_params.camera_jitter[1];
            dlss_frame.mode = mode;
            dlss_frame.reset_history =
                pt_streamline_history_reset;
          };
          if (pt_rr_requested) {
            StreamlineDlssRayReconstructionFrame rr_frame{};
            fill_dlss_frame(rr_frame, pt_post.reconstruction_mode);
            rr_frame.diffuse_albedo_image =
                path_tracer.rrDiffuseAlbedoImage();
            rr_frame.diffuse_albedo_memory =
                path_tracer.rrDiffuseAlbedoMemory();
            rr_frame.diffuse_albedo_view =
                path_tracer.rrDiffuseAlbedoView();
            rr_frame.specular_albedo_image =
                path_tracer.rrSpecularAlbedoImage();
            rr_frame.specular_albedo_memory =
                path_tracer.rrSpecularAlbedoMemory();
            rr_frame.specular_albedo_view =
                path_tracer.rrSpecularAlbedoView();
            rr_frame.normal_roughness_image =
                path_tracer.rrNormalRoughnessImage();
            rr_frame.normal_roughness_memory =
                path_tracer.rrNormalRoughnessMemory();
            rr_frame.normal_roughness_view =
                path_tracer.rrNormalRoughnessView();
            rr_frame.specular_hit_distance_image =
                path_tracer.rrSpecularHitDistanceImage();
            rr_frame.specular_hit_distance_memory =
                path_tracer.rrSpecularHitDistanceMemory();
            rr_frame.specular_hit_distance_view =
                path_tracer.rrSpecularHitDistanceView();
            pt_dlss_active =
                streamline_vulkan_runtime_
                    .recordDlssRayReconstruction(rr_frame);
          } else {
            StreamlineDlssFrame dlss_frame{};
            fill_dlss_frame(dlss_frame, pt_post.active_upscale);
            pt_dlss_active =
                streamline_vulkan_runtime_.recordDlss(dlss_frame);
          }
          if (pt_dlss_active) {
            streamline_temporal_history_key_ =
                pt_streamline_history_key;
            streamline_temporal_history_valid_ = true;
            streamline_temporal_render_width_ = ptw;
            streamline_temporal_render_height_ = pth;
            streamline_temporal_output_width_ = preview_output_width;
            streamline_temporal_output_height_ = preview_output_height;
            streamline_temporal_reset_generation_ =
                pt_params.settings.reset_generation;
            streamline_temporal_mode_ =
                pt_rr_requested ? pt_post.reconstruction_mode
                                : pt_post.active_upscale;
            streamline_temporal_ray_reconstruction_ = pt_rr_requested;
            streamline_temporal_recovery_pending_ = false;
            if (pt_rr_requested &&
                !streamline_rr_active_logged_) {
              xpbd::log::infof(
                  "Path-trace post-process active: PT noisy HDR -> "
                  "DLSS Ray Reconstruction (%ux%u -> %ux%u mode=%u)",
                  ptw, pth, preview_output_width,
                  preview_output_height,
                  static_cast<unsigned>(
                      pt_post.reconstruction_mode));
              streamline_rr_active_logged_ = true;
            }
            if (!pt_rr_requested) {
              streamline_rr_active_logged_ = false;
            }
            if (streamline_dlss_failure_logged_) {
              xpbd::log::infof(
                  "DLSS reconstruction recovered: input=%ux%u "
                  "output=%ux%u",
                  ptw, pth, preview_output_width,
                  preview_output_height);
            }
            streamline_dlss_failure_logged_ = false;
          } else {
            streamline_temporal_history_valid_ = false;
            streamline_temporal_recovery_pending_ = true;
            if (!streamline_dlss_failure_logged_) {
              xpbd::log::warnf(
                  "DLSS reconstruction dispatch disabled: %s",
                  streamline_vulkan_runtime_.status().c_str());
              streamline_dlss_failure_logged_ = true;
            }
          }
        } else {
          if (dlss_settings.valid) {
            // Target exactness alone is insufficient: pause/max-sample and
            // late dispatch prerequisites can leave last frame's guides in
            // place. Never advance Streamline history with stale inputs.
            streamline_vulkan_runtime_.invalidateDlssHistory();
            if (!streamline_transparency_contract_failure_logged_) {
              xpbd::log::warnf(
                  "R0F_G07_TEMPORAL_INPUT_REJECT mode=%s required=0x%05x "
                  "actual=0x%05x allocated=0x%05x exact=%d dispatch=%d",
                  pt_rr_requested ? "rr" : "sr",
                  static_cast<unsigned>(
                      pt_temporal_required_output_mask_for_frame),
                  static_cast<unsigned>(
                      pt_actual_output_write_mask_for_frame),
                  static_cast<unsigned>(
                      target_result.allocated_output_mask),
                  pt_target_exact_for_frame ? 1 : 0,
                  pt_dispatch_recorded_for_frame ? 1 : 0);
              streamline_transparency_contract_failure_logged_ = true;
            }
          }
          streamline_temporal_history_valid_ = false;
          if (dlss_settings.valid) {
            streamline_temporal_recovery_pending_ = true;
          }
        }
        if (pt_dispatch_recorded_for_frame && frame.view_matrix != nullptr &&
            frame.proj_matrix != nullptr) {
          std::copy_n(frame.view_matrix, 16u,
                      rt_motion_previous_view_.begin());
          std::copy_n(frame.proj_matrix, 16u,
                      rt_motion_previous_proj_.begin());
          rt_motion_camera_history_valid_ = true;
        } else if (pt_dispatch_recorded_for_frame) {
          rt_motion_camera_history_valid_ = false;
        }
      } else {
        path_trace_active = false;
      }
    }
    if (!(path_trace_active && draw_viewport)) {
      // Keep the optional timestamp pair valid on raster/fallback frames so a
      // previous slot's query result can never be mistaken for this frame.
      write_timestamp(GpuTimestampQuery::PathTraceBegin,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      write_timestamp(GpuTimestampQuery::PathTraceEnd,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

