    bool fg_inputs_recorded = false;
    if (fg_frame_candidate) {
      auto image_barrier =
          [&](VkImage image, VkImageLayout old_layout,
              VkImageLayout new_layout, VkAccessFlags src_access,
              VkAccessFlags dst_access,
              VkPipelineStageFlags src_stage,
              VkPipelineStageFlags dst_stage) {
            VkImageMemoryBarrier barrier{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = src_access;
            barrier.dstAccessMask = dst_access;
            barrier.oldLayout = old_layout;
            barrier.newLayout = new_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(fs.cmd, src_stage, dst_stage, 0, 0, nullptr,
                                 0, nullptr, 1, &barrier);
          };

      // Selection/hover guides are deliberately deferred, so HUDLess contains
      // only scene color described by the tagged depth and motion vectors.
      image_barrier(
          swap_images_[image_index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
      const VkImageLayout previous_hudless_layout =
          image_resource.fg_hudless_layout;
      image_barrier(
          image_resource.fg_hudless.image, previous_hudless_layout,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          previous_hudless_layout == VK_IMAGE_LAYOUT_UNDEFINED
              ? 0u
              : VK_ACCESS_SHADER_READ_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          previous_hudless_layout == VK_IMAGE_LAYOUT_UNDEFINED
              ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
              : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);

      VkImageCopy copy{};
      copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.srcSubresource.layerCount = 1;
      copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.dstSubresource.layerCount = 1;
      copy.extent = {swap_extent_.width, swap_extent_.height, 1u};
      vkCmdCopyImage(
          fs.cmd, swap_images_[image_index],
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          image_resource.fg_hudless.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      image_barrier(
          image_resource.fg_hudless.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      image_resource.fg_hudless_layout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      // Alpha starts at zero. The regular UI blend state writes
      // premultiplied RGB and source alpha, exactly matching Streamline's
      // UIColorAndAlpha contract. Only UI inside the generated preview
      // subrect needs recomposition; surrounding native UI is outside the
      // tagged Backbuffer extent.
      std::array<VkClearValue, 2> fg_ui_clears{};
      fg_ui_clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
      fg_ui_clears[1].depthStencil = {1.0f, 0};
      VkRenderPassBeginInfo fg_ui_rp{
          VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      fg_ui_rp.renderPass = fg_ui_render_pass_;
      fg_ui_rp.framebuffer = image_resource.fg_ui_framebuffer;
      fg_ui_rp.renderArea.extent = swap_extent_;
      fg_ui_rp.clearValueCount =
          static_cast<std::uint32_t>(fg_ui_clears.size());
      fg_ui_rp.pClearValues = fg_ui_clears.data();
      vkCmdBeginRenderPass(fs.cmd, &fg_ui_rp,
                           VK_SUBPASS_CONTENTS_INLINE);
      draw_selection_lines(mesh_pipeline_temporal_hud_lines_);
      if (draw_ui && frame.ui->overlay_visible) {
        (void)drawUi(fs, *frame.ui, true);
      }
      vkCmdEndRenderPass(fs.cmd);
      image_resource.fg_ui_layout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      image_barrier(
          swap_images_[image_index],
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_ACCESS_TRANSFER_READ_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

      VkRenderPassBeginInfo fg_overlay_rp{
          VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      fg_overlay_rp.renderPass = fg_overlay_render_pass_;
      fg_overlay_rp.framebuffer =
          image_resource.fg_overlay_framebuffer;
      fg_overlay_rp.renderArea.extent = swap_extent_;
      fg_overlay_rp.clearValueCount = 0;
      fg_overlay_rp.pClearValues = nullptr;
      vkCmdBeginRenderPass(fs.cmd, &fg_overlay_rp,
                           VK_SUBPASS_CONTENTS_INLINE);
      draw_selection_lines(mesh_pipeline_temporal_hud_lines_);
      if (draw_ui && frame.ui->overlay_visible) {
        const int overlay_commands = drawUi(fs, *frame.ui, true);
        stats_.ui_commands += overlay_commands;
        draws += overlay_commands;
      }
      vkCmdEndRenderPass(fs.cmd);

      StreamlineFrameGenerationFrame fg_frame{};
      fg_frame.command_buffer = fs.cmd;
      fg_frame.depth_image = path_tracer.depthImage();
      fg_frame.depth_memory = path_tracer.depthMemory();
      fg_frame.depth_view = path_tracer.depthView();
      fg_frame.motion_image = path_tracer.rrMotionImage();
      fg_frame.motion_memory = path_tracer.rrMotionMemory();
      fg_frame.motion_view = path_tracer.rrMotionView();
      fg_frame.hudless_image = image_resource.fg_hudless.image;
      fg_frame.hudless_memory = image_resource.fg_hudless.memory;
      fg_frame.hudless_view = image_resource.fg_hudless.view;
      fg_frame.ui_image = image_resource.fg_ui.image;
      fg_frame.ui_memory = image_resource.fg_ui.memory;
      fg_frame.ui_view = image_resource.fg_ui.view;
      fg_frame.view = pt_params.view;
      fg_frame.projection = pt_params.proj;
      fg_frame.previous_view = pt_params.previous_view;
      fg_frame.previous_projection = pt_params.previous_proj;
      fg_frame.color_format = swap_format_;
      fg_frame.render_width = pt_params.width;
      fg_frame.render_height = pt_params.height;
      fg_frame.output_width = swap_extent_.width;
      fg_frame.output_height = swap_extent_.height;
      fg_frame.viewport_x =
          static_cast<std::uint32_t>(safe_viewport.x);
      fg_frame.viewport_y =
          static_cast<std::uint32_t>(safe_viewport.y);
      fg_frame.viewport_width =
          static_cast<std::uint32_t>(safe_viewport.w);
      fg_frame.viewport_height =
          static_cast<std::uint32_t>(safe_viewport.h);
      fg_frame.swapchain_image_count =
          static_cast<std::uint32_t>(swap_images_.size());
      fg_frame.frame_index = static_cast<std::uint32_t>(
          frame.diagnostics.render_frame);
      fg_frame.jitter_x = pt_params.camera_jitter[0];
      fg_frame.jitter_y = pt_params.camera_jitter[1];
      fg_frame.reset_history = pt_streamline_history_reset;
      fg_inputs_recorded =
          streamline_vulkan_runtime_.recordFrameGenerationInputs(
              fg_frame);
      if (!fg_inputs_recorded) {
        streamline_vulkan_runtime_.clearFrameGenerationInputs(
            fs.cmd, fg_frame.frame_index, fg_frame.output_width,
            fg_frame.output_height, fg_frame.viewport_x,
            fg_frame.viewport_y, fg_frame.viewport_width,
            fg_frame.viewport_height);
      }
    } else if (streamline_vulkan_runtime_.swapchainOwnership() ==
               SwapchainOwnership::StreamlineFrameGenerationProxy) {
      // NVIDIA's checklist requires explicit null tags whenever loading,
      // raster fallback, or a collapsed preview makes depth/motion invalid.
      streamline_vulkan_runtime_.clearFrameGenerationInputs(
          fs.cmd, static_cast<std::uint32_t>(
                      frame.diagnostics.render_frame),
          swap_extent_.width, swap_extent_.height,
          static_cast<std::uint32_t>(safe_viewport.x),
          static_cast<std::uint32_t>(safe_viewport.y),
          static_cast<std::uint32_t>(safe_viewport.w),
          static_cast<std::uint32_t>(safe_viewport.h));
    }
    stats_.draw_calls = draws;
    if (diagnostics_enabled_ && diagnostic_trace_frame_) {
      xpbd::log::infof(
          "VKDIAG command ts_us=%llu thread=%llu frame=%llu slot=%zu "
          "image=%u draw_calls=%d upload=%llu static_bone_upload=%llu "
          "static_resource_upload=%llu fg_recorded=%d "
          "interactive_resize=%d pt_target=%ux%u",
          static_cast<unsigned long long>(diagnosticTimestampUs()),
          static_cast<unsigned long long>(diagnosticThreadId()),
          static_cast<unsigned long long>(frame.diagnostics.render_frame),
          frame_index_, image_index, draws,
          static_cast<unsigned long long>(stats_.upload_bytes),
          static_cast<unsigned long long>(stats_.static_bone_upload_bytes),
          static_cast<unsigned long long>(
              stats_.static_resource_upload_bytes),
          fg_inputs_recorded ? 1 : 0,
          interactive_preview_resize ? 1 : 0,
          path_tracer.targetWidth(), path_tracer.targetHeight());
    }

