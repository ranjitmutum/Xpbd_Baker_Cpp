    const auto t0 = Clock::now();
    stats_.present_succeeded = false;
    diagnostic_context_ = frame.diagnostics;
    diagnostic_trace_frame_ =
        perf_diagnostics_enabled_ && frame.diagnostics.active;
    const std::uint64_t temporal_render_serial =
        render_thread_frame_context_active_
            ? render_thread_render_serial_
            : frame.diagnostics.render_frame;
    streamline_temporal_commit_candidate_valid_ = false;
    if (fatal_error_ || !device_) {
      failActiveStillRender(
          frame, fatal_error_detail_.empty()
                     ? "Vulkan backend is unavailable after a fatal error"
                     : fatal_error_detail_);
      return;
    }
    bool static_retirement_complete = false;
    if (!pollStaticAssetRetirement(false,
                                   static_retirement_complete)) {
      failActiveStillRender(frame, fatal_error_detail_);
      return;
    }
    bool environment_retirement_complete = false;
    if (!pollWorldEnvironmentRetirement(
            false, environment_retirement_complete)) {
      failActiveStillRender(frame, fatal_error_detail_);
      return;
    }
    // While a native file dialog is open, do not submit GPU work (avoids hang).
    if (presentation_suspended_) {
      return;
    }
    rt_descriptor_write_calls_frame_ = 0;
    rt_descriptor_cache_hits_frame_ = 0;
    rt_descriptor_entries_written_frame_ = 0;

    const PathTraceSettings normalized_frame_settings =
        normalizePathTraceSettings(frame.path_trace_settings);
    const bool requested_frame_generation =
        normalized_frame_settings.requested_frame_generation ==
        PathTraceFrameGeneration::On;
    const bool interactive_preview_resize =
        frame.interactive_viewport_resize;
    // Keep the runtime's atomic request authoritative even when a caller
    // reaches the backend without the normal latency-frame prelude.
    streamline_vulkan_runtime_.requestFrameGeneration(
        requested_frame_generation);
    const bool want_rt =
        frame.prefer_ray_tracing && !presentation_suspended_;
    active_render_path_ = resolveRenderPath(want_rt, rt_capability_);
    const bool frame_generation_supported =
        frameGenerationPlatformSupported();
    const bool frame_generation_available =
        frameGenerationSwapchainReady();
    const FrameGenerationDiagnostic fg_diagnostic =
        streamline_vulkan_runtime_.frameGenerationDiagnostic();
    // Capability and current proxy readiness are deliberately separate.  A
    // requested FG transition must be allowed to create the proxy resources;
    // requiring resources here would make activation impossible from a native
    // swapchain.  The next transaction still enforces Vulkan Immediate mode,
    // shared queues, transfer-source usage, and guide-image allocation.
    const bool desired_frame_generation =
        requested_frame_generation && frame_generation_supported &&
        !vsync_ && !fg_force_native_recovery_ &&
        !fg_diagnostic.recovery_required &&
        active_render_path_ == RenderPath::RayTracing &&
        streamline_vulkan_runtime_.frameGenerationActivationAllowed();
    stats_.dlss_frame_generation_supported =
        frame_generation_supported;
    stats_.dlss_frame_generation_requested =
        requested_frame_generation;
    stats_.reflex_supported =
        streamline_vulkan_runtime_.reflexSupported();
    stats_.dlss_frame_generation_active =
        streamline_vulkan_runtime_.frameGenerationActive();
    stats_.dlss_frames_actually_presented =
        streamline_vulkan_runtime_.framesActuallyPresented();

    const SwapchainOwnership desired_ownership =
        desired_frame_generation
            ? SwapchainOwnership::StreamlineFrameGenerationProxy
            : SwapchainOwnership::Native;
    // Streamline requires a fresh swapchain for every FG on/off transition.
    // Recovery/resize/dialog gates force one Native transaction first.
    if (!recreate_swapchain_ &&
        streamline_vulkan_runtime_.swapchainOwnership() !=
            desired_ownership) {
      swapchain_recreate_target_ = desired_ownership;
      recreate_swapchain_ = true;
    }
    if (recreate_swapchain_ || !swapchain_) {
      swapchain_recreate_target_ =
          fg_force_native_recovery_ ? SwapchainOwnership::Native
                                    : desired_ownership;
    }

    // Path selection: honor user RT preference only when NVIDIA RT is armed.
    // Unsupported / failed RT always falls back to the forward raster path.
    // Never run RT while a system dialog owns the UI thread.
    if (want_rt &&
        active_render_path_ != RenderPath::RayTracing && !rt_fallback_logged_) {
      xpbd::log::warnf(
          "Ray tracing requested but falling back to rasterization: %s",
          rt_capability_.unsupported_reason.empty()
              ? "RT not available on this device"
              : rt_capability_.unsupported_reason.c_str());
      rt_fallback_logged_ = true;
    }
    bool rt_shadows_active = false;

    FrameSync &fs = frames_[frame_index_];
    VulkanRtScene *rt_scene = rt_scenes_[frame_index_].get();
    VulkanPathTracer &path_tracer = path_tracers_[frame_index_];
    bool &rt_scene_built = rt_scene_built_[frame_index_];
    std::uint64_t &last_rt_scene_hash = last_rt_scene_hash_[frame_index_];
    const VkDescriptorSet mesh_rt_desc_set =
        mesh_rt_desc_sets_[frame_index_];
    const bool static_input =
        frame.static_model != nullptr && frame.static_model_frame != nullptr;
    bool static_refresh_pending =
        static_input &&
        static_generations_.needsRefresh(frame.static_model_generation,
                                         frame.static_texture_generation);
    logDiagnosticFrame(frame, fs);
