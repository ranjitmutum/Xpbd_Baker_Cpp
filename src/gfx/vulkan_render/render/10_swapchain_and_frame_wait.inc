    if (recreate_swapchain_ || !swapchain_) {
      if (Clock::now() < next_swapchain_recreate_attempt_) {
        (void)interruptibleRetryDelay((std::min)(
            next_swapchain_recreate_attempt_,
            Clock::now() + std::chrono::milliseconds(16)));
        return;
      }
      if (!recreateSwapchain()) {
        recreate_swapchain_ = true;
        if (!fatal_error_) {
          next_swapchain_recreate_attempt_ =
              Clock::now() + kSwapchainRecreateRetryDelay;
          (void)interruptibleRetryDelay(
              Clock::now() + std::chrono::milliseconds(16));
        }
        return;
      }
      recreate_swapchain_ = false;
      next_swapchain_recreate_attempt_ = {};
    }

    auto wait_for_fence = [&](VkFence fence, const char *stage,
                              std::uint32_t image_index,
                              VkFence image_fence) {
      // Once a preview submission owns this fence, ordinary shutdown must let
      // it reach a proven completion before teardown. The outer worker join
      // deadline escalates to FatalQuarantine, which still interrupts this
      // sliced wait and preserves bounded shutdown.
      return waitForFenceControlled(fence, stage, image_index, image_fence,
                                    fs.cmd, false, true);
    };

    const ControlledWaitResult wait_result =
        wait_for_fence(fs.fence, "vkWaitForFences.frame", UINT32_MAX,
                       VK_NULL_HANDLE);
    if (!wait_result.completed()) {
      failActiveStillRender(frame, fatal_error_detail_);
      return;
    }

    readCompletedTimestamps(fs);
    logDiagnosticPerf(fs);
    fs.perf_pending = false;
    stats_.backend_cpu_ms = 0.0f;
    stats_.cpu_scene_assembly_ms = 0.0f;
    stats_.cpu_scene_hash_ms = 0.0f;
    stats_.cpu_emitter_distribution_ms = 0.0f;
    stats_.cpu_descriptor_update_ms = 0.0f;
    stats_.gpu_as_build_ms = 0.0f;
    stats_.gpu_path_trace_ms = 0.0f;
    stats_.gpu_rr_ms = 0.0f;
    stats_.gpu_sr_ms = 0.0f;
    stats_.gpu_fg_present_ms = 0.0f;
    stats_.rt_aov_write_mask = 0;
