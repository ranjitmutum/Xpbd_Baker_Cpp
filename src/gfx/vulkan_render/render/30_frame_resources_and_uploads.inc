    stats_.upload_ms = 0.0f;
    stats_.upload_bytes = 0;
    stats_.ui_upload_bytes = 0;
    stats_.mesh_upload_bytes = 0;
    stats_.static_bone_upload_bytes = 0;
    stats_.static_resource_upload_bytes = 0;
    stats_.mesh_solid_offset_bytes = 0;
    stats_.mesh_transparent_offset_bytes = 0;
    stats_.mesh_line_offset_bytes = 0;
    stats_.buffer_reallocations = 0;
    stats_.draw_calls = 0;
    stats_.ui_commands = 0;
    stats_.active_render_path = static_cast<int>(active_render_path_);
    stats_.ray_tracing_supported = supportsRayTracing();
    stats_.ray_tracing_requested = frame.prefer_ray_tracing;
    stats_.static_resource_rebuilds = static_resource_rebuilds_;
    stats_.static_model_vertex_bytes = static_vertex_bytes_;
    stats_.static_model_index_bytes = static_index_bytes_;
    stats_.static_opaque_index_count = static_draw_plan_.opaque.index_count;
    stats_.static_cutout_index_count = static_draw_plan_.cutout.index_count;
    stats_.static_blend_index_count = static_draw_plan_.blend.index_count;

    const int framebuffer_width = static_cast<int>(std::min<std::uint32_t>(
        swap_extent_.width,
        static_cast<std::uint32_t>((std::numeric_limits<int>::max)())));
    const int framebuffer_height = static_cast<int>(std::min<std::uint32_t>(
        swap_extent_.height,
        static_cast<std::uint32_t>((std::numeric_limits<int>::max)())));
    const auto clamped_edge = [](int origin, int size, int limit) {
      const std::int64_t edge = static_cast<std::int64_t>(origin) +
                                static_cast<std::int64_t>((std::max)(size, 0));
      return static_cast<int>((std::clamp)(
          edge, std::int64_t{0}, static_cast<std::int64_t>(limit)));
    };
    ViewportRect safe_viewport{};
    safe_viewport.x =
        (std::clamp)(frame.viewport.x, 0, framebuffer_width);
    safe_viewport.y =
        (std::clamp)(frame.viewport.y, 0, framebuffer_height);
    safe_viewport.w =
        (std::max)(0, clamped_edge(frame.viewport.x, frame.viewport.w,
                                   framebuffer_width) -
                          safe_viewport.x);
    safe_viewport.h =
        (std::max)(0, clamped_edge(frame.viewport.y, frame.viewport.h,
                                   framebuffer_height) -
                          safe_viewport.y);
    const bool valid_viewport =
        safe_viewport.w > 1 && safe_viewport.h > 1 && frame.view_matrix &&
        frame.proj_matrix;
    const bool draw_mesh =
        valid_viewport && frame.scene &&
        (!frame.scene->solid.empty() || !frame.scene->transparent.empty() ||
         !frame.scene->lines.empty());
    const ViewportRasterScene *raster = frame.raster_scene;
    const bool draw_raster_env =
        valid_viewport && raster != nullptr && raster->show_environment &&
        !raster->environment.solid.empty();
    const bool draw_raster_env_trans =
        valid_viewport && raster != nullptr && raster->show_environment &&
        !raster->environment.transparent.empty();
    const bool draw_raster_grid =
        valid_viewport && raster != nullptr && !raster->grid.lines.empty();
    const bool draw_skybox =
        valid_viewport && raster != nullptr && skybox_pipeline_ &&
        skybox_layout_ && skybox_desc_set_ && raster->skybox.valid();
    const DynamicMeshUploadLayout mesh_upload =
        draw_mesh ? makeDynamicMeshUploadLayout(frame.scene->solid.size(),
                                                frame.scene->transparent.size(),
                                                frame.scene->lines.size())
                  : DynamicMeshUploadLayout{};
    // Append preview-scene environment (solid + transparent water) + grid.
    const std::size_t raster_env_offset = mesh_upload.total_bytes;
    const std::size_t raster_env_bytes =
        draw_raster_env
            ? raster->environment.solid.size() * sizeof(MeshVertex)
            : 0;
    const std::size_t raster_env_trans_offset =
        raster_env_offset + raster_env_bytes;
    const std::size_t raster_env_trans_bytes =
        draw_raster_env_trans
            ? raster->environment.transparent.size() * sizeof(MeshVertex)
            : 0;
    const std::size_t raster_grid_offset =
        raster_env_trans_offset + raster_env_trans_bytes;
    const std::size_t raster_grid_bytes =
        draw_raster_grid ? raster->grid.lines.size() * sizeof(MeshVertex) : 0;
    const std::size_t mesh_arena_bytes =
        raster_grid_offset + raster_grid_bytes;
    const bool upload_dynamic_mesh = draw_mesh || draw_raster_env ||
                                    draw_raster_env_trans || draw_raster_grid;

    VkDeviceSize ui_vertex_bytes = 0;
    VkDeviceSize ui_index_bytes = 0;
    bool draw_ui = frame.ui && font_ready_ && frame.ui->ctx && frame.ui->cmds &&
                   frame.ui->vertices && frame.ui->indices;
    if (draw_ui) {
      ui_vertex_bytes = frame.ui->vertices->allocated > 0
                            ? frame.ui->vertices->allocated
                            : nk_buffer_total(frame.ui->vertices);
      ui_index_bytes = frame.ui->indices->allocated > 0
                           ? frame.ui->indices->allocated
                           : nk_buffer_total(frame.ui->indices);
      draw_ui = ui_vertex_bytes > 0 && ui_index_bytes > 0;
    }




    const auto upload_start = Clock::now();
    auto ensure_owned_buffer = [&](Buffer &buffer, VkDeviceSize bytes,
                                   VkBufferUsageFlags usage) {
      bool reallocated = false;
      if (!ensureBuffer(buffer, bytes, usage, &reallocated)) {
        return false;
      }
      if (reallocated) {
        ++stats_.buffer_reallocations;
        ++total_buffer_reallocations_;
      }
      return true;
    };

    if (static_refresh_pending) {
      // rebuildStaticModelResources submits its upload after all previously
      // queued graphics work and waits only for that submission's fence. Once
      // it signals, the old static resources are no longer referenced and can
      // be replaced without a device-wide idle.
      std::uint64_t resource_upload_bytes = 0;
      if (!rebuildStaticModelResources(
              *frame.static_model, frame.static_model_texture,
              frame.static_model_material,
              frame.static_model_generation, frame.static_texture_generation,
              resource_upload_bytes)) {
        writeLog("Vulkan static model resource rebuild failed");
        return;
      }
      stats_.static_resource_upload_bytes = resource_upload_bytes;
      stats_.static_resource_rebuilds = static_resource_rebuilds_;
      stats_.static_model_vertex_bytes = static_vertex_bytes_;
      stats_.static_model_index_bytes = static_index_bytes_;
      stats_.static_opaque_index_count = static_draw_plan_.opaque.index_count;
      stats_.static_cutout_index_count = static_draw_plan_.cutout.index_count;
      stats_.static_blend_index_count = static_draw_plan_.blend.index_count;
    }

    bool draw_static =
        valid_viewport && static_input && static_model_ready_ &&
        !static_draw_plan_.indices.empty() &&
        staticModelFrameMatchesMesh(*frame.static_model,
                                    *frame.static_model_frame) &&
        frame.static_model_frame->bones.size() == static_bone_count_;
    if (static_input && !static_draw_plan_.indices.empty() && !draw_static &&
        !static_mismatch_logged_) {
      SDL_Log("Vulkan static model/frame mismatch: mesh bones=%zu, frame "
              "bones=%zu; skipping static draw",
              static_bone_count_, frame.static_model_frame->bones.size());
      static_mismatch_logged_ = true;
    }

    VkDeviceSize requested_bone_bytes = 0;
    if (draw_static) {
      const VkDeviceSize bone_bytes =
          static_cast<VkDeviceSize>(frame.static_model_frame->bones.size() *
                                    sizeof(StaticModelBoneState));
      requested_bone_bytes = bone_bytes;
      if (!ensure_owned_buffer(fs.bone_ssbo, bone_bytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
          !uploadBuffer(fs.bone_ssbo, 0, bone_bytes,
                        frame.static_model_frame->bones.data())) {
        writeLog("Vulkan static bone upload failed");
        return;
      }
      updateStaticBoneDescriptor(fs);
      stats_.static_bone_upload_bytes = bone_bytes;
      stats_.mesh_upload_bytes += bone_bytes;
    }
    if (draw_skybox && !uploadSkyboxCubemap(raster->skybox)) {
      writeLog("Vulkan skybox cubemap upload failed");
    }
    const bool skybox_draw = draw_skybox && skybox_ready_;

    const bool draw_viewport =
        valid_viewport &&
        (path_trace_active || draw_mesh || static_input || draw_raster_env ||
         draw_raster_env_trans || draw_raster_grid || skybox_draw);
    if (!path_trace_active || !draw_viewport) {
      // A folded, minimized, or hidden preview has no valid temporal
      // continuity. Force the next DLSS frame to start cleanly.
      streamline_vulkan_runtime_.invalidateDlssHistory();
      streamline_temporal_history_valid_ = false;
    }

    if (draw_ui) {
      if (!ensure_owned_buffer(fs.ui_vbo, ui_vertex_bytes,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
          !ensure_owned_buffer(fs.ui_ibo, ui_index_bytes,
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
          !uploadBuffer(fs.ui_vbo, 0, ui_vertex_bytes,
                        nk_buffer_memory_const(frame.ui->vertices)) ||
          !uploadBuffer(fs.ui_ibo, 0, ui_index_bytes,
                        nk_buffer_memory_const(frame.ui->indices))) {
        writeLog("Vulkan UI dynamic upload failed");
        return;
      }
      stats_.ui_upload_bytes =
          static_cast<std::uint64_t>(ui_vertex_bytes + ui_index_bytes);
    }

    if (upload_dynamic_mesh) {
      if (!ensure_owned_buffer(fs.mesh_vbo,
                               static_cast<VkDeviceSize>(mesh_arena_bytes),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        writeLog("Vulkan mesh arena allocate failed");
        return;
      }
      if (draw_mesh) {
        if (!uploadBuffer(
                fs.mesh_vbo,
                static_cast<VkDeviceSize>(mesh_upload.solid.offset_bytes),
                static_cast<VkDeviceSize>(mesh_upload.solid.size_bytes),
                frame.scene->solid.data()) ||
            !uploadBuffer(
                fs.mesh_vbo,
                static_cast<VkDeviceSize>(mesh_upload.transparent.offset_bytes),
                static_cast<VkDeviceSize>(mesh_upload.transparent.size_bytes),
                frame.scene->transparent.data()) ||
            !uploadBuffer(
                fs.mesh_vbo,
                static_cast<VkDeviceSize>(mesh_upload.lines.offset_bytes),
                static_cast<VkDeviceSize>(mesh_upload.lines.size_bytes),
                frame.scene->lines.data())) {
          writeLog("Vulkan mesh dynamic upload failed");
          return;
        }
      }
      if (draw_raster_env &&
          !uploadBuffer(fs.mesh_vbo,
                        static_cast<VkDeviceSize>(raster_env_offset),
                        static_cast<VkDeviceSize>(raster_env_bytes),
                        raster->environment.solid.data())) {
        writeLog("Vulkan raster environment upload failed");
        return;
      }
      if (draw_raster_env_trans &&
          !uploadBuffer(fs.mesh_vbo,
                        static_cast<VkDeviceSize>(raster_env_trans_offset),
                        static_cast<VkDeviceSize>(raster_env_trans_bytes),
                        raster->environment.transparent.data())) {
        writeLog("Vulkan raster water upload failed");
        return;
      }
      if (draw_raster_grid &&
          !uploadBuffer(fs.mesh_vbo,
                        static_cast<VkDeviceSize>(raster_grid_offset),
                        static_cast<VkDeviceSize>(raster_grid_bytes),
                        raster->grid.lines.data())) {
        writeLog("Vulkan raster grid upload failed");
        return;
      }
      stats_.mesh_upload_bytes += mesh_arena_bytes;
      stats_.mesh_solid_offset_bytes = mesh_upload.solid.offset_bytes;
      stats_.mesh_transparent_offset_bytes =
          mesh_upload.transparent.offset_bytes;
      stats_.mesh_line_offset_bytes = mesh_upload.lines.offset_bytes;
    }

    stats_.mesh_arena_capacity_bytes = fs.mesh_vbo.capacity;
    stats_.ui_vertex_capacity_bytes = fs.ui_vbo.capacity;
    stats_.ui_index_capacity_bytes = fs.ui_ibo.capacity;
    stats_.upload_bytes = stats_.ui_upload_bytes + stats_.mesh_upload_bytes +
                          stats_.static_resource_upload_bytes;
    stats_.total_buffer_reallocations = total_buffer_reallocations_;
    stats_.upload_ms =
        std::chrono::duration<float, std::milli>(Clock::now() - upload_start)
            .count();
    logDiagnosticResources(
        frame, fs, requested_bone_bytes,
        static_cast<VkDeviceSize>(mesh_upload.total_bytes), ui_vertex_bytes,
        ui_index_bytes,
        stats_.buffer_reallocations > 0 ||
            stats_.static_resource_upload_bytes > 0);

