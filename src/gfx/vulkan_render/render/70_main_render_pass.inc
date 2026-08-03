    const bool fg_frame_candidate =
        streamline_vulkan_runtime_.swapchainOwnership() ==
            SwapchainOwnership::StreamlineFrameGenerationProxy &&
        frame_generation_available &&
        !interactive_preview_resize &&
        frameGenerationTemporalInputIsReady(
            pt_temporal_reconstruction_requested_for_frame,
            pt_dlss_active) &&
        pt_target_exact_for_frame && pt_motion_guide_ready_for_frame &&
        pt_dispatch_recorded_for_frame &&
        path_trace_active && draw_viewport &&
        image_index < swap_images_.size() &&
        image_resource.fg_hudless.image != VK_NULL_HANDLE &&
        image_resource.fg_hudless.memory != VK_NULL_HANDLE &&
        image_resource.fg_hudless.view != VK_NULL_HANDLE &&
        image_resource.fg_ui.image != VK_NULL_HANDLE &&
        image_resource.fg_ui.memory != VK_NULL_HANDLE &&
        image_resource.fg_ui.view != VK_NULL_HANDLE &&
        image_resource.fg_ui_framebuffer != VK_NULL_HANDLE &&
        image_resource.fg_overlay_framebuffer != VK_NULL_HANDLE &&
        path_tracer.depthImage() != VK_NULL_HANDLE &&
        path_tracer.depthMemory() != VK_NULL_HANDLE &&
        path_tracer.depthView() != VK_NULL_HANDLE &&
        path_tracer.rrMotionImage() != VK_NULL_HANDLE &&
        path_tracer.rrMotionMemory() != VK_NULL_HANDLE &&
        path_tracer.rrMotionView() != VK_NULL_HANDLE;
    const bool temporal_selection_active =
        pt_dlss_active || fg_frame_candidate;

    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{frame.clear_r, frame.clear_g, frame.clear_b, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image_index];
    rp.renderArea.extent = swap_extent_;
    rp.clearValueCount = 2;
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(fs.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);


    int draws = 0;
    VkViewport viewport{};
    VkRect2D viewport_scissor{};
    MeshScenePushConstants mesh_pc{};
    constexpr VkShaderStageFlags kMeshPcStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (draw_viewport) {
      viewport.x = static_cast<float>(safe_viewport.x);
      viewport.y = static_cast<float>(safe_viewport.y);
      viewport.width = static_cast<float>(safe_viewport.w);
      viewport.height = static_cast<float>(safe_viewport.h);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      viewport_scissor = {
          {safe_viewport.x, safe_viewport.y},
          {static_cast<uint32_t>(safe_viewport.w),
           static_cast<uint32_t>(safe_viewport.h)}};

      float mvp_gl[16];
      mulMat(frame.proj_matrix, frame.view_matrix, mvp_gl);
      glMvpToVulkan(mvp_gl, mesh_pc.mvp);
      mesh_pc.light_dir[0] = resolved_sun_light.direction[0];
      mesh_pc.light_dir[1] = resolved_sun_light.direction[1];
      mesh_pc.light_dir[2] = resolved_sun_light.direction[2];
      mesh_pc.ambient =
          raster != nullptr ? raster->lighting.ambient : 0.38f;
      mesh_pc.light_color[0] = resolved_sun_light.color[0];
      mesh_pc.light_color[1] = resolved_sun_light.color[1];
      mesh_pc.light_color[2] = resolved_sun_light.color[2];
      const float resolved_sun_color_luminance =
          0.2126f * resolved_sun_light.color[0] +
          0.7152f * resolved_sun_light.color[1] +
          0.0722f * resolved_sun_light.color[2];
      mesh_pc.intensity =
          resolved_sun_light.lighting_enabled &&
                  resolved_sun_color_luminance > 1.0e-6f
              ? resolved_sun_light.power_estimate /
                    resolved_sun_color_luminance
              : 0.0f;
      mesh_pc.material_debug[0] =
          static_cast<std::uint32_t>(frame.material_debug_view);
      mesh_pc.material_debug[1] =
          labPbrFeatureFlags(frame.static_model_material);
    }
    const auto draw_selection_lines = [&](VkPipeline pipeline) {
      if (!draw_mesh || frame.scene == nullptr || frame.scene->lines.empty() ||
          pipeline == VK_NULL_HANDLE) {
        return;
      }
      vkCmdSetViewport(fs.cmd, 0, 1, &viewport);
      vkCmdSetScissor(fs.cmd, 0, 1, &viewport_scissor);
      vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdPushConstants(fs.cmd, mesh_layout_, kMeshPcStages, 0,
                         sizeof(mesh_pc), &mesh_pc);
      const VkDeviceSize off = mesh_upload.lines.offset_bytes;
      vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
      vkCmdDraw(fs.cmd, static_cast<uint32_t>(frame.scene->lines.size()), 1, 0,
                0);
      ++draws;
    };
    if (draw_ui) {
      stats_.ui_commands = drawUi(fs, *frame.ui, false);
      draws += stats_.ui_commands;
    }
    write_timestamp(GpuTimestampQuery::UiEnd,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);


    if (draw_viewport) {
      vkCmdSetViewport(fs.cmd, 0, 1, &viewport);
      vkCmdSetScissor(fs.cmd, 0, 1, &viewport_scissor);

      // Always clear the viewport. Full RT keeps the raster sky, then
      // composites unified model/environment visibility with explicit depth.
      {
        const float cr = raster ? raster->lighting.clear_r : (30.0f / 255.0f);
        const float cg = raster ? raster->lighting.clear_g : (30.0f / 255.0f);
        const float cb = raster ? raster->lighting.clear_b : (40.0f / 255.0f);
        VkClearAttachment cas[2]{};
        cas[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cas[0].colorAttachment = 0;
        cas[0].clearValue.color = {{cr, cg, cb, 1.0f}};
        cas[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        cas[1].clearValue.depthStencil = {1.0f, 0};
        VkClearRect crct{};
        crct.rect = viewport_scissor;
        crct.baseArrayLayer = 0;
        crct.layerCount = 1;
        vkCmdClearAttachments(fs.cmd, 2, cas, 1, &crct);
      }

      // --- Forward rasterization / unified RT composite ---
      // Raster: sky, environment, opaque model, transparent ranges, lines.
      // RT: sky only, then a unified model+environment BVH composite. Lines
      // remain raster and depth-test against the opaque depth emitted by RT.

      if (skybox_draw) {
        float view_rot[16];
        for (int i = 0; i < 16; ++i) {
          view_rot[i] = frame.view_matrix[i];
        }
        view_rot[12] = 0.0f;
        view_rot[13] = 0.0f;
        view_rot[14] = 0.0f;
        float sky_mvp_gl[16];
        mulMat(frame.proj_matrix, view_rot, sky_mvp_gl);
        SkyboxPushConstants sky_pc{};
        glMvpToVulkan(sky_mvp_gl, sky_pc.mvp);
        sky_pc.flags[0] =
            swap_format_ == VK_FORMAT_B8G8R8A8_SRGB ||
                    swap_format_ == VK_FORMAT_R8G8B8A8_SRGB
                ? 1u
                : 0u;
        constexpr VkShaderStageFlags kSkyPcStages =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          skybox_pipeline_);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                skybox_layout_, 0, 1, &skybox_desc_set_, 0,
                                nullptr);
        vkCmdPushConstants(fs.cmd, skybox_layout_, kSkyPcStages, 0,
                           sizeof(sky_pc), &sky_pc);
        const VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &skybox_vbo_.buffer, &off);
        vkCmdDraw(fs.cmd, 36, 1, 0, 0);
        ++draws;
      }

      const bool use_rt = rt_shadows_active && mesh_rt_pipeline_ &&
                          static_rt_pipeline_ && mesh_rt_layout_ &&
                          static_rt_layout_;
      VkPipeline mesh_pipe = use_rt ? mesh_rt_pipeline_ : mesh_pipeline_;
      VkPipeline mesh_pipe_trans =
          use_rt && mesh_rt_pipeline_trans_ ? mesh_rt_pipeline_trans_
                                            : mesh_pipeline_trans_;
      VkPipelineLayout mesh_lay = use_rt ? mesh_rt_layout_ : mesh_layout_;
      VkPipeline static_pipe =
          use_rt ? static_rt_pipeline_ : static_mesh_pipeline_;
      VkPipeline static_pipe_blend =
          use_rt && static_rt_pipeline_blend_ ? static_rt_pipeline_blend_
                                              : static_mesh_pipeline_blend_;
      VkPipelineLayout static_lay =
          use_rt ? static_rt_layout_ : static_mesh_layout_;
      VkDescriptorSet static_set =
          use_rt ? static_rt_descriptor_sets_[frame_index_]
                 : fs.static_descriptor_set;

      // In full RT, environment/base geometry is already in the unified BLAS.
      if (!path_trace_active && draw_raster_env) {
        const bool env_use_rt = use_rt && !path_trace_active;
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          env_use_rt ? mesh_pipe : mesh_pipeline_);
        if (env_use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, env_use_rt ? mesh_lay : mesh_layout_,
                           kMeshPcStages, 0, sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize off =
            static_cast<VkDeviceSize>(raster_env_offset);
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd,
                  static_cast<uint32_t>(raster->environment.solid.size()), 1,
                  0, 0);
        ++draws;
      }

      // Unified path-traced model + scene over the raster sky.
      if (path_trace_active) {
        path_tracer.recordComposite(
            fs.cmd, pt_params,
            pt_dlss_active
                ? streamline_vulkan_runtime_.dlssOutputView()
                : VK_NULL_HANDLE);
        ++draws;
      }

      if (!path_trace_active && draw_static &&
          (static_draw_plan_.opaque.index_count > 0 ||
           static_draw_plan_.cutout.index_count > 0)) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, static_pipe);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                static_lay, 0, 1, &static_set, 0, nullptr);
        vkCmdPushConstants(fs.cmd, static_lay, kMeshPcStages, 0,
                           sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &static_model_vbo_.buffer,
                               &offset);
        vkCmdBindIndexBuffer(fs.cmd, static_model_ibo_.buffer, 0,
                             VK_INDEX_TYPE_UINT32);
        auto draw_static_range = [&](const StaticModelIndexRange &range) {
          if (range.index_count == 0) {
            return;
          }
          vkCmdDrawIndexed(fs.cmd, range.index_count, 1, range.first_index, 0,
                           0);
          ++draws;
        };
        draw_static_range(static_draw_plan_.opaque);
        draw_static_range(static_draw_plan_.cutout);
      }

      if (!path_trace_active && draw_mesh && !frame.scene->solid.empty()) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipe);
        if (use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, mesh_lay, kMeshPcStages, 0, sizeof(mesh_pc),
                           &mesh_pc);
        const VkDeviceSize off = mesh_upload.solid.offset_bytes;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd, static_cast<uint32_t>(frame.scene->solid.size()), 1,
                  0, 0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::OpaqueEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      // Environment translucency before model alpha so water never covers
      // model semi-transparent materials (glass / stained glass / etc.).
      if (!path_trace_active && draw_raster_env_trans &&
          mesh_pipeline_trans_) {
        const bool env_use_rt = use_rt && !path_trace_active;
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          env_use_rt && mesh_pipe_trans ? mesh_pipe_trans
                                                        : mesh_pipeline_trans_);
        if (env_use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, env_use_rt ? mesh_lay : mesh_layout_,
                           kMeshPcStages, 0, sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize off =
            static_cast<VkDeviceSize>(raster_env_trans_offset);
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(
            fs.cmd,
            static_cast<uint32_t>(raster->environment.transparent.size()), 1,
            0, 0);
        ++draws;
      }
      if (!path_trace_active && draw_static &&
          static_draw_plan_.blend.index_count > 0) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          static_pipe_blend);
        vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                static_lay, 0, 1, &static_set, 0, nullptr);
        vkCmdPushConstants(fs.cmd, static_lay, kMeshPcStages, 0,
                           sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &static_model_vbo_.buffer,
                               &offset);
        vkCmdBindIndexBuffer(fs.cmd, static_model_ibo_.buffer, 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(fs.cmd, static_draw_plan_.blend.index_count, 1,
                         static_draw_plan_.blend.first_index, 0, 0);
        ++draws;
      }
      if (!path_trace_active && draw_mesh &&
          !frame.scene->transparent.empty() && mesh_pipe_trans) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipe_trans);
        if (use_rt) {
          vkCmdBindDescriptorSets(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  mesh_lay, 0, 1, &mesh_rt_desc_set, 0,
                                  nullptr);
        }
        vkCmdPushConstants(fs.cmd, mesh_lay, kMeshPcStages, 0, sizeof(mesh_pc),
                           &mesh_pc);
        const VkDeviceSize off = mesh_upload.transparent.offset_bytes;
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd,
                  static_cast<uint32_t>(frame.scene->transparent.size()), 1, 0,
                  0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::TransparentEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      if (!fg_frame_candidate) {
        draw_selection_lines(
            temporal_selection_active ? mesh_pipeline_temporal_hud_lines_
                                      : mesh_pipeline_overlay_lines_);
      }
      if (draw_raster_grid) {
        vkCmdBindPipeline(fs.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipeline_lines_);
        vkCmdPushConstants(fs.cmd, mesh_layout_, kMeshPcStages, 0,
                           sizeof(mesh_pc), &mesh_pc);
        const VkDeviceSize off =
            static_cast<VkDeviceSize>(raster_grid_offset);
        vkCmdBindVertexBuffers(fs.cmd, 0, 1, &fs.mesh_vbo.buffer, &off);
        vkCmdDraw(fs.cmd, static_cast<uint32_t>(raster->grid.lines.size()), 1,
                  0, 0);
        ++draws;
      }
      write_timestamp(GpuTimestampQuery::LinesEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
      write_timestamp(GpuTimestampQuery::OpaqueEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      write_timestamp(GpuTimestampQuery::TransparentEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      write_timestamp(GpuTimestampQuery::LinesEnd,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    if (!fg_frame_candidate && draw_ui && draw_viewport &&
        frame.ui->overlay_visible) {
      const int overlay_commands = drawUi(fs, *frame.ui, true);
      stats_.ui_commands += overlay_commands;
      draws += overlay_commands;
    }

    vkCmdEndRenderPass(fs.cmd);

