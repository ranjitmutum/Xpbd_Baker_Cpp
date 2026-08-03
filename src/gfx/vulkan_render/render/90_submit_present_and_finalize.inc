    write_timestamp(GpuTimestampQuery::FrameEnd,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    const VkResult end_command = call_with_diagnostics(
        "vkEndCommandBuffer",
        [&] { return vkEndCommandBuffer(fs.cmd); });
    if (end_command != VK_SUCCESS) {
      SDL_Log("Vulkan command buffer end failed: %d",
              static_cast<int>(end_command));
      enterFatalVulkanError(frame, "vkEndCommandBuffer", end_command);
      return;
    }

    const bool fg_acquire_handoff =
        frameGenerationAcquireMustPrecedeFrame(
            streamline_vulkan_runtime_.swapchainOwnership());
    // NVIDIA's Vulkan DLSS-G contract requires waiting on the intercepted
    // acquire semaphore before starting any work for the new frame. Native
    // presentation can retain the narrower first-swapchain-use dependency.
    VkPipelineStageFlags wait_stage =
        fg_acquire_handoff
            ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
            : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &fs.image_available;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fs.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &image_resource.render_finished;
    streamline_vulkan_runtime_.markRenderSubmitStart();
    const VkResult submit_result = call_with_diagnostics(
        "vkQueueSubmit",
        [&] { return vkQueueSubmit(graphics_queue_, 1, &si, fs.fence); });
    streamline_vulkan_runtime_.markRenderSubmitEnd();
    if (submit_result != VK_SUCCESS) {
      SDL_Log("Vulkan queue submit failed: %d",
              static_cast<int>(submit_result));
      enterFatalVulkanError(frame, "vkQueueSubmit(frame)", submit_result);
      return;
    }
    image_resource.last_in_flight = fs.fence;
    fs.timestamps_pending = timestamp_queries_enabled_ && fs.timestamp_pool;



    stats_.backend_cpu_ms =
        std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
    // GPU timestamps become readable only when this frame slot's fence is
    // waited on during a later render. Preserve the CPU-side measurements and
    // RT counters in the slot so the eventual diagnostic row cannot mix them
    // with a newer frame recorded through another slot.
    fs.perf_snapshot = stats_;
    fs.perf_render_frame = frame.diagnostics.render_frame;
    fs.perf_pending = diagnostic_trace_frame_;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &image_resource.render_finished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;
    VkSwapchainPresentFenceInfoKHR present_fence_info{
        VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR};
    if (presentFenceLifecycleEnabled()) {
      present_fence_info.swapchainCount = 1;
      present_fence_info.pFences = &image_resource.present_fence;
      pi.pNext = &present_fence_info;
    }
#ifndef NDEBUG
    if (streamline_vulkan_runtime_.swapchainOwnership() ==
        SwapchainOwnership::StreamlineFrameGenerationProxy) {
      assert(pi.pNext == nullptr &&
             "DLSS-G proxy Present must not carry a maintenance1 fence");
      assert(image_resource.present_fence == VK_NULL_HANDLE &&
             "DLSS-G proxy images must not own application present fences");
    }
#endif
    const auto present_start = Clock::now();
    logDiagnosticApi("vkQueuePresentKHR", "before", std::nullopt, 0.0,
                     image_index, fs.fence, image_resource.last_in_flight,
                     fs.cmd, false, true);
    streamline_vulkan_runtime_.markPresentStart();
    VkResult pr =
        streamline_vulkan_runtime_.queuePresent(present_queue_, &pi);
    streamline_vulkan_runtime_.markPresentEnd();
    stats_.present_succeeded =
        pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR;
    if (stats_.present_succeeded) {
      ++stats_.present_success_count;
    }
    const FrameGenerationTransitionResult fg_present_transition =
        streamline_vulkan_runtime_.updateFrameGenerationStateAfterPresent(pr);
    stats_.dlss_frame_generation_active =
        streamline_vulkan_runtime_.frameGenerationActive();
    stats_.dlss_frames_actually_presented =
        streamline_vulkan_runtime_.framesActuallyPresented();
    if (fg_present_transition == FrameGenerationTransitionResult::RecoverNative) {
      const FrameGenerationDiagnostic diagnostic =
          streamline_vulkan_runtime_.frameGenerationDiagnostic();
      fg_force_native_recovery_ = true;
      fg_recovery_reason_ = diagnostic.status;
      swapchain_recreate_target_ = SwapchainOwnership::Native;
      recreate_swapchain_ = true;
    } else if (fg_present_transition ==
               FrameGenerationTransitionResult::FatalDeviceLost) {
      enterFatalVulkanError(frame, "vkQueuePresentKHR", pr);
    }
    if (presentFenceLifecycleEnabled()) {
      image_resource.present_pending =
          pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR ||
          pr == VK_ERROR_OUT_OF_DATE_KHR ||
          pr == VK_ERROR_SURFACE_LOST_KHR ||
          pr == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT ||
          pr == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT;
    }
    logDiagnosticApi(
        "vkQueuePresentKHR", "after", pr,
        std::chrono::duration<double, std::milli>(Clock::now() - present_start)
            .count(),
        image_index, fs.fence, image_resource.last_in_flight, fs.cmd, false,
        false);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
      recreate_swapchain_ = true;
    } else if (pr != VK_SUCCESS) {
      if (streamline_vulkan_runtime_.swapchainOwnership() ==
          SwapchainOwnership::StreamlineFrameGenerationProxy) {
        // The runtime has already classified the intercepted proxy result;
        // non-device-lost errors recover through a Native transaction.
        if (fg_present_transition ==
            FrameGenerationTransitionResult::FatalDeviceLost) {
          if (!fatal_error_) {
            enterFatalVulkanError(frame, "vkQueuePresentKHR", pr);
          }
        } else {
          fg_force_native_recovery_ = true;
          swapchain_recreate_target_ = SwapchainOwnership::Native;
          recreate_swapchain_ = true;
        }
      } else {
        SDL_Log("Vulkan present failed: %d", static_cast<int>(pr));
        enterFatalVulkanError(frame, "vkQueuePresentKHR", pr);
      }
    }

    frame_index_ = (frame_index_ + 1) % frames_.size();
    if (static_input) {
      stats_.cube_count =
          static_cast<int>(frame.static_model_frame->cube_count);
      stats_.line_count = frame.scene ? frame.scene->line_segment_count : 0;
    } else if (frame.scene) {
      stats_.cube_count = frame.scene->cube_count;
      stats_.line_count = frame.scene->line_segment_count;
    }
