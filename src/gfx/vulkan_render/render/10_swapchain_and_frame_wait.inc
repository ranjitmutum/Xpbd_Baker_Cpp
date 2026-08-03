    if (recreate_swapchain_ || !swapchain_) {
      if (Clock::now() < next_swapchain_recreate_attempt_) {
        SDL_Delay(16);
        return;
      }
      if (!recreateSwapchain()) {
        recreate_swapchain_ = true;
        if (!fatal_error_) {
          next_swapchain_recreate_attempt_ =
              Clock::now() + kSwapchainRecreateRetryDelay;
          SDL_Delay(16);
        }
        return;
      }
      recreate_swapchain_ = false;
      next_swapchain_recreate_attempt_ = {};
    }

    auto wait_for_fence = [&](VkFence fence, const char *stage,
                              std::uint32_t image_index,
                              VkFence image_fence) {
      const auto wait_start = Clock::now();
      logDiagnosticApi(stage, "before", std::nullopt, 0.0, image_index,
                       fence, image_fence, fs.cmd, false, true);
      VkResult result = VK_SUCCESS;
      bool timed_out = false;
      if (!diagnostics_enabled_) {
        result =
            vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
      } else {
        do {
          result = vkWaitForFences(device_, 1, &fence, VK_TRUE,
                                   kDiagnosticWaitSliceNs);
          if (result == VK_TIMEOUT) {
            timed_out = true;
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          wait_start)
                    .count();
            logDiagnosticApi(stage, "timeout", result, elapsed_ms,
                             image_index, fence, image_fence, fs.cmd, true,
                             true);
          }
        } while (result == VK_TIMEOUT);
      }
      const double elapsed_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - wait_start)
              .count();
      logDiagnosticApi(stage, "after", result, elapsed_ms, image_index,
                       fence, image_fence, fs.cmd, timed_out, false);
      return result;
    };

    const VkResult wait_result =
        wait_for_fence(fs.fence, "vkWaitForFences.frame", UINT32_MAX,
                       VK_NULL_HANDLE);
    if (wait_result != VK_SUCCESS) {
      SDL_Log("Vulkan fence wait failed: %d", static_cast<int>(wait_result));
      enterFatalVulkanError(frame, "vkWaitForFences(frame)", wait_result);
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
