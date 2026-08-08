    uint32_t image_index = 0;
    const auto acquire_start = Clock::now();
    logDiagnosticApi("vkAcquireNextImageKHR", "before", std::nullopt, 0.0,
                     UINT32_MAX, fs.fence, VK_NULL_HANDLE, fs.cmd, false, true);
    VkResult acq = VK_SUCCESS;
    bool acquire_timed_out = false;
    do {
      if (renderThreadStopRequested()) {
        return;
      }
      acq = streamline_vulkan_runtime_.acquireNextImage(
          device_, swapchain_,
          render_thread_control_ ? kControlledWaitSliceNs : UINT64_MAX,
          fs.image_available, VK_NULL_HANDLE, &image_index);
      if (acq == VK_TIMEOUT || acq == VK_NOT_READY) {
        acquire_timed_out = true;
        logDiagnosticApi(
            "vkAcquireNextImageKHR", "timeout", acq,
            std::chrono::duration<double, std::milli>(Clock::now() -
                                                      acquire_start)
                .count(),
            UINT32_MAX, fs.fence, VK_NULL_HANDLE, fs.cmd,
            diagnostics_enabled_, diagnostics_enabled_);
      }
    } while (acq == VK_TIMEOUT || acq == VK_NOT_READY);
    logDiagnosticApi(
        "vkAcquireNextImageKHR", "after", acq,
        std::chrono::duration<double, std::milli>(Clock::now() - acquire_start)
            .count(),
        acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR ? image_index
                                                       : UINT32_MAX,
        fs.fence, VK_NULL_HANDLE, fs.cmd, acquire_timed_out, false);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
      recreate_swapchain_ = true;
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        fg_force_native_recovery_ = true;
        swapchain_recreate_target_ = SwapchainOwnership::Native;
      }
      return;
    }
    if (acq == VK_SUBOPTIMAL_KHR) {
      recreate_swapchain_ = true;
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        fg_force_native_recovery_ = true;
        swapchain_recreate_target_ = SwapchainOwnership::Native;
      }
    } else if (acq != VK_SUCCESS) {
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        const FrameGenerationTransitionResult transition =
            streamline_vulkan_runtime_.classifyFrameGenerationVkResult(
                acq, "Acquire");
        if (transition == FrameGenerationTransitionResult::FatalDeviceLost) {
          enterFatalVulkanError(frame, "vkAcquireNextImageKHR", acq);
        } else {
          fg_force_native_recovery_ = true;
          swapchain_recreate_target_ = SwapchainOwnership::Native;
          recreate_swapchain_ = true;
        }
      } else {
        SDL_Log("Vulkan acquire failed: %d", static_cast<int>(acq));
        enterFatalVulkanError(frame, "vkAcquireNextImageKHR", acq);
      }
      if (fatal_error_ || recreate_swapchain_) {
        return;
      }
    }
    if (image_index >= framebuffers_.size() ||
        image_index >= swap_image_resources_.size()) {
      recordFatalError(
          "vkAcquireNextImageKHR",
          "Vulkan acquire returned an invalid swapchain image index");
      return;
    }
    SwapchainImageResource &image_resource =
        swap_image_resources_[image_index];
    if (!image_resource.render_finished) {
      recordFatalError(
          "swapchain.render_finished",
          "Vulkan swapchain image has no present semaphore");
      return;
    }
    if (presentFenceLifecycleEnabled() &&
        !image_resource.present_fence) {
      recordFatalError(
          "swapchain.present_fence",
          "Vulkan swapchain image has no present completion fence");
      return;
    }
    if (image_resource.last_in_flight &&
        image_resource.last_in_flight != fs.fence) {
      const ControlledWaitResult image_wait = wait_for_fence(
          image_resource.last_in_flight, "vkWaitForFences.image", image_index,
          image_resource.last_in_flight);
      if (!image_wait.completed()) {
        failActiveStillRender(frame, fatal_error_detail_);
        return;
      }
    }

    auto call_with_diagnostics = [&](const char *stage, auto &&call) {
      const auto call_start = Clock::now();
      logDiagnosticApi(stage, "before", std::nullopt, 0.0, image_index,
                       fs.fence, image_resource.last_in_flight, fs.cmd, false,
                       true);
      const VkResult result = call();
      logDiagnosticApi(
          stage, "after", result,
          std::chrono::duration<double, std::milli>(Clock::now() - call_start)
              .count(),
          image_index, fs.fence, image_resource.last_in_flight, fs.cmd, false,
          false);
      return result;
    };

    if (presentFenceLifecycleEnabled() &&
        image_resource.present_pending) {
      const ControlledWaitResult present_wait =
          wait_for_fence(image_resource.present_fence,
                         "vkWaitForFences.present_reuse", image_index,
                         image_resource.last_in_flight);
      if (!present_wait.completed()) {
        failActiveStillRender(frame, fatal_error_detail_);
        return;
      }
      image_resource.present_pending = false;

      const auto reset_present_start = Clock::now();
      logDiagnosticApi("vkResetFences.present", "before", std::nullopt, 0.0,
                       image_index, image_resource.present_fence,
                       image_resource.last_in_flight, fs.cmd, false, true);
      const VkResult reset_present = vkResetFences(
          device_, 1, &image_resource.present_fence);
      logDiagnosticApi(
          "vkResetFences.present", "after", reset_present,
          std::chrono::duration<double, std::milli>(Clock::now() -
                                                    reset_present_start)
              .count(),
          image_index, image_resource.present_fence,
          image_resource.last_in_flight, fs.cmd, false, false);
      if (reset_present != VK_SUCCESS) {
        SDL_Log("Vulkan present fence reset failed: %d",
                static_cast<int>(reset_present));
        enterFatalVulkanError(frame, "vkResetFences(present)",
                              reset_present);
        return;
      }
    }

    const VkResult reset_fence = call_with_diagnostics(
        "vkResetFences", [&] { return vkResetFences(device_, 1, &fs.fence); });
    if (reset_fence != VK_SUCCESS) {
      SDL_Log("Vulkan fence reset failed: %d",
              static_cast<int>(reset_fence));
      enterFatalVulkanError(frame, "vkResetFences(frame)", reset_fence);
      return;
    }
    const VkResult reset_command = call_with_diagnostics(
        "vkResetCommandBuffer",
        [&] { return vkResetCommandBuffer(fs.cmd, 0); });
    if (reset_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer reset failed: %d",
              static_cast<int>(reset_command));
      enterFatalVulkanError(frame, "vkResetCommandBuffer", reset_command);
      return;
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    const VkResult begin_command = call_with_diagnostics(
        "vkBeginCommandBuffer",
        [&] { return vkBeginCommandBuffer(fs.cmd, &bi); });
    if (begin_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer begin failed: %d",
              static_cast<int>(begin_command));
      enterFatalVulkanError(frame, "vkBeginCommandBuffer", begin_command);
      return;
    }

    auto write_timestamp = [&](GpuTimestampQuery query,
                               VkPipelineStageFlagBits stage) {
      if (timestamp_queries_enabled_ && fs.timestamp_pool) {
        vkCmdWriteTimestamp(fs.cmd, stage, fs.timestamp_pool,
                            queryIndex(query));
      }
    };
    if (timestamp_queries_enabled_ && fs.timestamp_pool) {
      vkCmdResetQueryPool(fs.cmd, fs.timestamp_pool, 0,
                          kGpuTimestampQueryCount);
      write_timestamp(GpuTimestampQuery::FrameBegin,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    }

    // Record pending BLAS full-build / animation refit before any ray queries.
    write_timestamp(GpuTimestampQuery::AsBegin,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    if (rt_as_needs_record) {
      const RtSceneStats before_build = rt_scene->stats();
      rt_scene->recordBuilds(fs.cmd);
      const RtSceneStats after_build = rt_scene->stats();
      if (diagnostics_enabled_ &&
          (diagnostic_rt_as_events_logged_ < 64u ||
           after_build.full_builds != before_build.full_builds ||
           after_build.refits != before_build.refits)) {
        xpbd::log::infof(
            "VKDIAG rt_as frame=%llu slot=%zu event=%llu "
            "reason=%s blas_full_delta=%llu blas_refit_delta=%llu "
            "full_total=%llu refit_total=%llu blas=%u instances=%u tlas=1",
            static_cast<unsigned long long>(
                frame.diagnostics.render_frame),
            frame_index_,
            static_cast<unsigned long long>(diagnostic_rt_as_events_logged_),
            rtAccelerationBuildReasonName(after_build.last_build_reason),
            static_cast<unsigned long long>(
                after_build.full_builds - before_build.full_builds),
            static_cast<unsigned long long>(
                after_build.refits - before_build.refits),
            static_cast<unsigned long long>(after_build.full_builds),
            static_cast<unsigned long long>(after_build.refits),
            static_cast<unsigned>(after_build.blas_count),
            static_cast<unsigned>(after_build.instance_count));
      }
      ++diagnostic_rt_as_events_logged_;
    }
    write_timestamp(GpuTimestampQuery::AsEnd,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    const RtSceneStats active_rt_stats = rt_scene->stats();
    stats_.rt_blas_count = 0;
    stats_.rt_tlas_count = 0;
    stats_.rt_instance_count = 0;
    stats_.rt_visible_instance_mask_count = 0;
    stats_.rt_hidden_instance_mask_count = 0;
    stats_.rt_positive_emitter_count = 0;
    stats_.rt_hidden_source_emitter_triangle_count = 0;
    stats_.rt_hidden_positive_weight_triangle_count = 0;
    stats_.rt_as_storage_bytes = 0;
    stats_.rt_scratch_bytes = 0;
    stats_.rt_attribute_bytes = 0;
    stats_.rt_allocated_bytes = 0;
    stats_.rt_full_builds = 0;
    stats_.rt_refits = 0;
    stats_.rt_tlas_full_builds = 0;
    stats_.rt_tlas_updates = 0;
    stats_.rt_upload_bytes = rt_upload_bytes_frame;
    stats_.cpu_emitter_distribution_ms = rt_emitter_distribution_frame_ms;
    stats_.rt_emitter_distribution_rebuilds = 0;
    stats_.rt_descriptor_write_calls = 0;
    stats_.rt_descriptor_cache_hits = 0;
    stats_.rt_descriptor_entries_written = 0;
    for (const auto &scene : rt_scenes_) {
      if (!scene) {
        continue;
      }
      const RtSceneStats scene_stats = scene->stats();
      stats_.rt_blas_count += scene_stats.blas_count;
      stats_.rt_tlas_count += scene_stats.tlas_count;
      stats_.rt_instance_count += scene_stats.instance_count;
      stats_.rt_as_storage_bytes += scene_stats.as_storage_bytes;
      stats_.rt_scratch_bytes += scene_stats.scratch_bytes;
      stats_.rt_attribute_bytes += scene_stats.attribute_bytes;
      stats_.rt_allocated_bytes += scene_stats.allocated_bytes;
      stats_.rt_full_builds += scene_stats.full_builds;
      stats_.rt_refits += scene_stats.refits;
      stats_.rt_tlas_full_builds += scene_stats.tlas_full_builds;
      stats_.rt_tlas_updates += scene_stats.tlas_updates;
      stats_.rt_emitter_distribution_rebuilds +=
          scene_stats.emitter_distribution_rebuilds;
      stats_.rt_descriptor_write_calls += scene_stats.descriptor_write_count;
      stats_.rt_descriptor_cache_hits += scene_stats.descriptor_cache_hits;
    }
    stats_.rt_primitive_count = active_rt_stats.primitive_count;
    stats_.rt_visible_instance_mask_count =
        active_rt_stats.visible_instance_mask_count;
    stats_.rt_hidden_instance_mask_count =
        active_rt_stats.hidden_instance_mask_count;
    stats_.rt_positive_emitter_count =
        active_rt_stats.positive_emitter_count;
    stats_.rt_hidden_source_emitter_triangle_count =
        active_rt_stats.hidden_source_emitter_triangle_count;
    stats_.rt_hidden_positive_weight_triangle_count =
        active_rt_stats.hidden_positive_weight_triangle_count;
    stats_.rt_last_build_reason = active_rt_stats.last_build_reason;
    stats_.rt_last_tlas_reason = active_rt_stats.last_tlas_reason;
    stats_.rt_descriptor_write_calls = rt_descriptor_write_calls_frame_;
    stats_.rt_descriptor_cache_hits = rt_descriptor_cache_hits_frame_;
    stats_.rt_descriptor_entries_written =
        rt_descriptor_entries_written_frame_;
