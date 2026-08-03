    // Independent still-render accumulator. It intentionally records only on
    // frame slot zero, so the mapped readback is consumed after the same
    // slot's fence and never races the interactive accumulators.
    if (frame.still_render != nullptr &&
        frame.still_render->status != nullptr &&
        frame.still_render->job_id != 0u) {
      const StillRenderFrameRequest &request = *frame.still_render;
      StillRenderStatus &status = *request.status;
      status.job_id = request.job_id;
      status.target_samples = request.target_samples;
      status.output_path = request.output_path;

      bool terminal =
          status.state == StillRenderJobState::Completed ||
          status.state == StillRenderJobState::Failed ||
          status.state == StillRenderJobState::Cancelled;
      if (!terminal && frame_index_ == 0u &&
          still_active_job_id_ == request.job_id &&
          still_progress_job_id_ == request.job_id &&
          Clock::now() - still_last_progress_time_ >
              std::chrono::seconds(30)) {
        still_path_tracer_.cancelStillCapture();
        still_active_job_id_ = 0u;
        status.state = StillRenderJobState::Failed;
        status.error =
            "Still render made no progress for 30 seconds";
        terminal = true;
        xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                          static_cast<unsigned long long>(request.job_id),
                          status.error.c_str());
      }
      if (request.cancel_requested && !terminal) {
        if (frame_index_ == 0u) {
          still_path_tracer_.cancelStillCapture();
          still_active_job_id_ = 0u;
          status.state = StillRenderJobState::Cancelled;
          status.error.clear();
          xpbd::log::infof("STILL_JOB cancelled job_id=%llu",
                           static_cast<unsigned long long>(request.job_id));
        }
      } else if (!terminal &&
                 (active_render_path_ != RenderPath::RayTracing ||
                  !rt_capability_.device_extensions_enabled ||
                  !still_path_tracer_.ready() ||
                  !still_path_tracer_.rtPipelineReady() ||
                  !request.path_trace_settings
                       .nvidia_rt_core_acceleration ||
                   request.path_trace_settings.force_software_fallback)) {
        still_path_tracer_.cancelStillCapture();
        status.state = StillRenderJobState::Failed;
        status.error =
            "Still rendering requires an active NVIDIA Vulkan RT pipeline";
        still_active_job_id_ = 0u;
        xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                          static_cast<unsigned long long>(request.job_id),
                          status.error.c_str());
      } else if (!terminal && frame_index_ == 0u && !rt_scene.ready()) {
        if (still_waiting_job_id_ != request.job_id) {
          still_waiting_job_id_ = request.job_id;
          still_wait_started_ = Clock::now();
          xpbd::log::infof(
              "STILL_JOB begin_wait job_id=%llu reason=rt_scene_not_ready",
              static_cast<unsigned long long>(request.job_id));
        } else if (Clock::now() - still_wait_started_ >
                   std::chrono::seconds(30)) {
          status.state = StillRenderJobState::Failed;
          status.error =
              "Still render RT scene did not become ready within 30 seconds";
          still_active_job_id_ = 0u;
          xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                            static_cast<unsigned long long>(request.job_id),
                            status.error.c_str());
        }
      } else if (!terminal && rt_scene.ready() && frame_index_ == 0u) {
        still_waiting_job_id_ = 0u;
        const bool new_job = still_active_job_id_ != request.job_id;
        if (new_job) {
          still_path_tracer_.cancelStillCapture();
          still_active_job_id_ = request.job_id;
          still_path_trace_frame_index_ = 0u;
          status.accumulated_samples = 0u;
          status.error.clear();
          PathTraceStillBackgroundInput preview_background{};
          const bool use_preview_background =
              request.preview_skybox != nullptr &&
              request.preview_skybox->valid() &&
              !resolved_world_environment.background_visible;
          if (use_preview_background) {
            preview_background.face_size = static_cast<std::uint32_t>(
                request.preview_skybox->face_size);
            preview_background.rgba8 = request.preview_skybox->rgba.data();
            preview_background.rgba8_size =
                request.preview_skybox->rgba.size();
            preview_background.view = request.view_matrix;
            preview_background.proj = request.proj_matrix;
          }
          if (request.output_path.empty() ||
              !still_path_tracer_.requestStillCapture(
                  std::filesystem::path(request.output_path), request.format,
                  request.transparent_background, request.job_id,
                  use_preview_background ? &preview_background : nullptr)) {
            status.state = StillRenderJobState::Failed;
            status.error = request.output_path.empty()
                               ? "Still render output path is empty"
                               : "Still render capture request was rejected";
            still_active_job_id_ = 0u;
            xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                              static_cast<unsigned long long>(request.job_id),
                              status.error.c_str());
          } else {
            status.state = StillRenderJobState::Rendering;
            still_progress_job_id_ = request.job_id;
            still_last_logged_samples_ = 0u;
            still_last_progress_time_ = Clock::now();
            xpbd::log::infof(
                "STILL_JOB begin job_id=%llu width=%u height=%u "
                "target_samples=%u preview_background=%d",
                static_cast<unsigned long long>(request.job_id), request.width,
                request.height, request.target_samples,
                use_preview_background ? 1 : 0);
          }
        }

        if (still_active_job_id_ == request.job_id &&
            status.state != StillRenderJobState::Failed) {
          const PathTraceTargetResult still_target =
              still_path_tracer_.ensureTarget(
                  request.width, request.height,
                  PathTraceTargetRequirements{0u});
          if (still_target.failure.vk_result == VK_ERROR_DEVICE_LOST) {
            enterFatalVulkanError(frame,
                                  "VulkanPathTracer::ensureTarget(still)",
                                  VK_ERROR_DEVICE_LOST);
            return;
          }
          if (!still_target.exact()) {
            still_path_tracer_.cancelStillCapture();
            still_active_job_id_ = 0u;
            status.state = StillRenderJobState::Failed;
            const PathTraceTargetFailure &failure = still_target.failure;
            status.error =
                "Still render target unavailable: VkResult=" +
                std::string(vkResultName(failure.vk_result)) + "(" +
                std::to_string(static_cast<int>(failure.vk_result)) +
                ") resource=" +
                (failure.resource.empty() ? std::string("target-bundle")
                                          : failure.resource) +
                " format=" +
                std::to_string(static_cast<int>(failure.format)) +
                " extent=" + std::to_string(request.width) + "x" +
                std::to_string(request.height) + " estimated_bytes=" +
                std::to_string(static_cast<unsigned long long>(
                    still_target.estimated_bytes)) +
                " memory_type=" + std::to_string(failure.memory_type) +
                " heap=" + std::to_string(failure.heap) +
                " heap_budget=" +
                std::to_string(static_cast<unsigned long long>(
                    failure.heap_budget)) +
                " heap_usage=" +
                std::to_string(static_cast<unsigned long long>(
                    failure.heap_usage)) +
                " frame_slot=0 still_job_id=" +
                std::to_string(static_cast<unsigned long long>(
                    request.job_id));
            if (still_target.status ==
                PathTraceTargetStatus::RetryDeferred) {
              status.error += " retry_after_ms=" +
                              std::to_string(
                                  still_target.retry_after_ms);
            }
            xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                              static_cast<unsigned long long>(request.job_id),
                              status.error.c_str());
          } else {
            PathTraceFrameParams still_params{};
            still_params.view = request.view_matrix;
            still_params.proj = request.proj_matrix;
            still_params.motion_history_valid = false;
            if (frame.raster_scene != nullptr) {
              still_params.light_dir[0] =
                  frame.raster_scene->lighting.direction[0];
              still_params.light_dir[1] =
                  frame.raster_scene->lighting.direction[1];
              still_params.light_dir[2] =
                  frame.raster_scene->lighting.direction[2];
              still_params.ambient = frame.raster_scene->lighting.ambient;
              still_params.light_color[0] =
                  frame.raster_scene->lighting.color[0];
              still_params.light_color[1] =
                  frame.raster_scene->lighting.color[1];
              still_params.light_color[2] =
                  frame.raster_scene->lighting.color[2];
              still_params.intensity =
                  frame.raster_scene->lighting.intensity;
            }
            still_params.clear_r = frame.clear_r;
            still_params.clear_g = frame.clear_g;
            still_params.clear_b = frame.clear_b;
            still_params.width = request.width;
            still_params.height = request.height;
            still_params.frame_index = still_path_trace_frame_index_++;
            still_params.frame_slot = 0u;
            still_params.settings =
                normalizePathTraceSettings(request.path_trace_settings);
            still_params.settings.samples_per_frame =
                (std::max)(1u, request.samples_per_submit);
            still_params.settings.maximum_samples =
                (std::max)(1u, request.target_samples);
            still_params.settings.pause_accumulation = false;
            still_params.settings.transparent_background =
                request.transparent_background;
            still_params.exposure =
                std::exp2(still_params.settings.display_exposure_ev);
            if (importance_environment_ready) {
              still_params.settings.analytic_environment_strength = 0.0f;
            }
            still_params.material_debug_view =
                request.material_debug_view;
            still_params.rt_debug_view = request.rt_debug_view;
            still_params.material_feature_flags =
                labPbrFeatureFlags(frame.static_model_material);
            if (procedural_environment_ready) {
              still_params.ambient = 0.0f;
              still_params.intensity = 0.0f;
            }
            still_params.hdr_environment = importance_environment_ready;
            still_params.environment_view =
                hdr_environment_ready
                    ? world_environment_texture_.view
                    : (procedural_environment_ready
                           ? atmosphere_environment_cache_.view
                           : VK_NULL_HANDLE);
            still_params.environment_sampler =
                importance_environment_ready ? world_environment_sampler_
                                             : VK_NULL_HANDLE;
            still_params.environment_distribution =
                hdr_environment_ready
                    ? world_environment_distribution_.buffer
                    : (procedural_environment_ready
                           ? atmosphere_environment_distribution_.buffer
                           : VK_NULL_HANDLE);
            still_params.environment_distribution_bytes =
                hdr_environment_ready
                    ? world_environment_distribution_bytes_
                    : (procedural_environment_ready
                           ? atmosphere_environment_distribution_bytes_
                           : 0u);

            std::uint64_t history_key = 14695981039346656037ull;
            appendPathTraceHistoryValue(history_key, request.job_id);
            appendPathTraceHistoryValue(history_key, last_rt_scene_hash);
            appendPathTraceHistoryValue(history_key,
                                        frame.static_model_generation);
            appendPathTraceHistoryValue(history_key,
                                        frame.static_texture_generation);
            appendPathTraceHistoryValue(
                history_key, resolved_world_environment.generation);
            appendPathTraceHistoryValue(
                history_key,
                resolved_world_environment.lighting_generation);
            appendPathTraceHistoryValue(
                history_key,
                resolved_world_environment.celestial_generation);
            appendPathTraceHistoryValue(
                history_key, resolved_world_environment.cloud_generation);
            if (request.view_matrix != nullptr) {
              appendPathTraceHistoryBytes(history_key, request.view_matrix,
                                          sizeof(float) * 16u);
            }
            if (request.proj_matrix != nullptr) {
              appendPathTraceHistoryBytes(history_key, request.proj_matrix,
                                          sizeof(float) * 16u);
            }
            still_params.history_key = history_key;

            const VkImageView albedo_view =
                static_texture_.view != VK_NULL_HANDLE
                    ? static_texture_.view
                    : VK_NULL_HANDLE;
            const VkImageView normal_view =
                static_normal_texture_.view != VK_NULL_HANDLE
                    ? static_normal_texture_.view
                    : VK_NULL_HANDLE;
            const VkImageView specular_view =
                static_specular_texture_.view != VK_NULL_HANDLE
                    ? static_specular_texture_.view
                    : VK_NULL_HANDLE;
            still_path_tracer_.recordDispatch(
                fs.cmd, rt_scene, still_params, albedo_view, normal_view,
                specular_view, static_albedo_sampler_,
                static_normal_sampler_, static_specular_sampler_);
            status.accumulated_samples =
                still_path_tracer_.accumulatedSamples();
            if (still_progress_job_id_ != request.job_id) {
              still_progress_job_id_ = request.job_id;
              still_last_logged_samples_ = 0u;
            }
            if (status.accumulated_samples != still_last_logged_samples_) {
              still_last_logged_samples_ = status.accumulated_samples;
              still_last_progress_time_ = Clock::now();
              xpbd::log::infof(
                  "STILL_JOB progress job_id=%llu samples=%u target=%u",
                  static_cast<unsigned long long>(request.job_id),
                  status.accumulated_samples, status.target_samples);
            }
            switch (still_path_tracer_.captureState()) {
            case PathTraceCaptureState::Completed:
              status.state = StillRenderJobState::Completed;
              status.error.clear();
              still_active_job_id_ = 0u;
              xpbd::log::infof(
                  "STILL_JOB complete job_id=%llu samples=%u path=%s",
                  static_cast<unsigned long long>(request.job_id),
                  status.accumulated_samples, status.output_path.c_str());
              break;
            case PathTraceCaptureState::Failed:
              status.state = StillRenderJobState::Failed;
              status.error = still_path_tracer_.captureError();
              still_active_job_id_ = 0u;
              xpbd::log::errorf("STILL_JOB error job_id=%llu error=%s",
                                static_cast<unsigned long long>(request.job_id),
                                status.error.c_str());
              break;
            case PathTraceCaptureState::Cancelled:
              status.state = StillRenderJobState::Cancelled;
              status.error.clear();
              still_active_job_id_ = 0u;
              xpbd::log::infof("STILL_JOB cancelled job_id=%llu",
                               static_cast<unsigned long long>(request.job_id));
              break;
            case PathTraceCaptureState::PendingGpuReadback:
              if (status.state != StillRenderJobState::Saving) {
                still_last_progress_time_ = Clock::now();
                xpbd::log::infof(
                    "STILL_JOB readback job_id=%llu samples=%u",
                    static_cast<unsigned long long>(request.job_id),
                    status.accumulated_samples);
              }
              status.state = StillRenderJobState::Saving;
              break;
            case PathTraceCaptureState::Requested:
            case PathTraceCaptureState::Idle:
              status.state = StillRenderJobState::Rendering;
              break;
            }
          }
        }
      }
    }

