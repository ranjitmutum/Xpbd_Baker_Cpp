    ResolvedWorldEnvironment resolved_world_environment =
        frame.world_environment != nullptr
            ? resolveWorldEnvironment(*frame.world_environment)
            : ResolvedWorldEnvironment{};
    const bool procedural_atmosphere_ready =
        ensureProceduralAtmosphereResources(resolved_world_environment);
    const bool hdr_environment_ready =
        rt_capability_.device_extensions_enabled &&
        ensureWorldEnvironmentResources(resolved_world_environment);
    if (fatal_error_) {
      failActiveStillRender(
          frame, fatal_error_detail_.empty()
                     ? "Vulkan device was lost while updating environment resources"
                     : fatal_error_detail_);
      return;
    }
    if (resolved_world_environment.sky_rendering ==
            SkyRendering::ProceduralDayNight &&
        !procedural_atmosphere_ready) {
      resolved_world_environment.sky_rendering = SkyRendering::Off;
      resolved_world_environment.background_visible = false;
      resolved_world_environment.environment_lighting = false;
      resolved_world_environment.environment_strength = 0.0f;
      resolved_world_environment.celestial = nullptr;
      resolved_world_environment.atmosphere = nullptr;
      resolved_world_environment.clouds = nullptr;
    }
    if (resolved_world_environment.sky_rendering ==
            SkyRendering::UserHdri &&
        !hdr_environment_ready) {
      resolved_world_environment.sky_rendering = SkyRendering::Off;
      resolved_world_environment.background_visible = false;
      resolved_world_environment.environment_lighting = false;
      resolved_world_environment.environment_strength = 0.0f;
      resolved_world_environment.hdr = nullptr;
    }
    ResolvedSunLight resolved_sun_light =
        resolveSunLight(resolved_world_environment);
    const PathTraceSettings resolved_environment_settings =
        normalizePathTraceSettings(frame.path_trace_settings);
    const float analytic_environment_strength =
        std::max(resolved_environment_settings.analytic_environment_strength,
                 0.0f);
    ResolvedEnvironmentView resolved_environment_view =
        resolveEnvironmentView(
            resolved_world_environment,
            {frame.clear_r * analytic_environment_strength,
             frame.clear_g * analytic_environment_strength,
             frame.clear_b * analytic_environment_strength},
            analytic_environment_strength);

    // PreviewSceneLighting remains a compatibility fallback only when World
    // has no active procedural Sun. Once the World Sun is enabled, every
    // consumer below sees exactly that one resolved light and the legacy
    // directional source is ignored.
    if (!resolved_sun_light.lighting_enabled &&
        resolved_world_environment.sky_rendering !=
            SkyRendering::ProceduralDayNight &&
        frame.raster_scene != nullptr) {
      const PreviewSceneLighting &legacy = frame.raster_scene->lighting;
      const double length_squared =
          static_cast<double>(legacy.direction[0]) * legacy.direction[0] +
          static_cast<double>(legacy.direction[1]) * legacy.direction[1] +
          static_cast<double>(legacy.direction[2]) * legacy.direction[2];
      if (length_squared > 1.0e-20 && std::isfinite(length_squared)) {
        const float inverse_length =
            static_cast<float>(1.0 / std::sqrt(length_squared));
        resolved_sun_light.direction = {
            legacy.direction[0] * inverse_length,
            legacy.direction[1] * inverse_length,
            legacy.direction[2] * inverse_length};
      } else {
        resolved_sun_light.direction = {0.0f, 1.0f, 0.0f};
      }
      resolved_sun_light.color = {
          std::isfinite(legacy.color[0])
              ? std::max(legacy.color[0], 0.0f)
              : 0.0f,
          std::isfinite(legacy.color[1])
              ? std::max(legacy.color[1], 0.0f)
              : 0.0f,
          std::isfinite(legacy.color[2])
              ? std::max(legacy.color[2], 0.0f)
              : 0.0f};
      resolved_sun_light.angular_radius = 0.00465047f;
      resolved_sun_light.solid_angle = static_cast<float>(
          6.28318530717958647692 *
          (1.0 - std::cos(resolved_sun_light.angular_radius)));
      const float legacy_irradiance =
          std::isfinite(legacy.intensity)
              ? std::max(legacy.intensity, 0.0f)
              : 0.0f;
      const float inverse_solid_angle =
          resolved_sun_light.solid_angle > 0.0f
              ? 1.0f / resolved_sun_light.solid_angle
              : 0.0f;
      for (std::size_t channel = 0;
           channel < resolved_sun_light.radiance.size(); ++channel) {
        resolved_sun_light.radiance[channel] =
            resolved_sun_light.color[channel] * legacy_irradiance *
            inverse_solid_angle;
      }
      resolved_sun_light.strength = legacy_irradiance;
      resolved_sun_light.power_estimate =
          (0.2126f * resolved_sun_light.radiance[0] +
           0.7152f * resolved_sun_light.radiance[1] +
           0.0722f * resolved_sun_light.radiance[2]) *
          resolved_sun_light.solid_angle;
      resolved_sun_light.enabled = legacy_irradiance > 0.0f;
      resolved_sun_light.disk_visible = false;
      resolved_sun_light.casts_shadow = true;
      resolved_sun_light.lighting_enabled =
          resolved_sun_light.enabled;
      std::uint64_t legacy_generation = 14695981039346656037ull;
      appendPathTraceHistoryBytes(legacy_generation, legacy.direction.data(),
                                  sizeof(legacy.direction));
      appendPathTraceHistoryBytes(legacy_generation, legacy.color.data(),
                                  sizeof(legacy.color));
      appendPathTraceHistoryValue(legacy_generation, legacy.intensity);
      resolved_sun_light.generation = legacy_generation;
    }
    const bool procedural_environment_ready =
        resolved_world_environment.sky_rendering ==
            SkyRendering::ProceduralDayNight &&
        procedural_atmosphere_ready && atmosphere_environment_ready_;
    const bool importance_environment_ready =
        hdr_environment_ready || procedural_environment_ready;
    if (resolved_environment_view.kind ==
            ResolvedEnvironmentKind::ProceduralDayNight &&
        procedural_environment_ready) {
      resolved_environment_view.sampling_ready =
          resolved_environment_view.lighting_enabled;
      resolved_environment_view.gpu_cache_required = false;
      resolved_environment_view.power_estimate =
          atmosphere_environment_power_estimate_;
    } else if (resolved_environment_view.kind ==
                   ResolvedEnvironmentKind::UserHdri &&
               hdr_environment_ready) {
      resolved_environment_view.power_estimate =
          world_environment_power_estimate_;
    }
    const bool environment_sampling_available =
        importance_environment_ready ||
        ((resolved_environment_view.kind ==
              ResolvedEnvironmentKind::Constant ||
          resolved_environment_view.kind ==
              ResolvedEnvironmentKind::AnalyticFallback) &&
         resolved_environment_view.sampling_ready);

    const auto rt_scene_cpu_begin = Clock::now();
    // RT acceleration: CPU-skin when poses change, then record BLAS build/refit
    // into the frame command buffer (no per-frame QueueWaitIdle — that froze
    // the app when playing animations with RT enabled).
    bool path_trace_active = false;
    bool rt_as_needs_record = false;
    std::uint64_t rt_upload_bytes_frame = 0;
    float rt_emitter_distribution_frame_ms = 0.0f;
    std::uint64_t rt_streamline_topology_hash =
        14695981039346656037ull;
    std::uint64_t rt_streamline_material_hash =
        14695981039346656037ull;
    std::uint64_t rt_emissive_generation =
        frame.rt_scene_generations.emission;
    if (active_render_path_ == RenderPath::RayTracing &&
        rt_capability_.device_extensions_enabled && !presentation_suspended_) {
      std::vector<float> packed_bones;
      std::vector<float> packed_tints;
      if (static_input && frame.static_model_frame != nullptr &&
          !frame.static_model_frame->bones.empty()) {
        packed_bones.resize(frame.static_model_frame->bones.size() * 16u);
        packed_tints.resize(frame.static_model_frame->bones.size() * 4u);
        for (std::size_t i = 0; i < frame.static_model_frame->bones.size();
             ++i) {
          std::memcpy(packed_bones.data() + i * 16u,
                      frame.static_model_frame->bones[i].transform.data(),
                      16u * sizeof(float));
          std::memcpy(packed_tints.data() + i * 4u,
                      frame.static_model_frame->bones[i].tint.data(),
                      4u * sizeof(float));
        }
      }

      std::array<RtColoredGeometryView, 4> geometry_views{};
      std::size_t geometry_view_count = 0;
      auto append_geometry = [&](const std::vector<MeshVertex> &vertices,
                                 bool alpha_blended,
                                 RtGeometryKind kind,
                                 RtBlasPolicy blas_policy,
                                 std::uint64_t content_generation,
                                 std::uint64_t topology_generation,
                                 std::uint64_t material_generation,
                                 std::uint64_t emission_generation) {
        if (vertices.empty() ||
            geometry_view_count >= geometry_views.size()) {
          return;
        }
        geometry_views[geometry_view_count] = {
            vertices.data(), vertices.size(), alpha_blended, kind,
            blas_policy, content_generation, topology_generation,
            material_generation, emission_generation};
        ++geometry_view_count;
      };
      if (frame.scene != nullptr) {
        append_geometry(frame.scene->solid, false,
                        RtGeometryKind::SkinnedModel,
                        RtBlasPolicy::DynamicRefit,
                        frame.rt_scene_generations.positions,
                        frame.rt_scene_generations.topology,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
        append_geometry(frame.scene->transparent, true,
                        RtGeometryKind::SkinnedModel,
                        RtBlasPolicy::DynamicRefit,
                        frame.rt_scene_generations.positions,
                        frame.rt_scene_generations.topology,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
      }
      if (frame.raster_scene != nullptr) {
        const bool ocean =
            frame.raster_scene->id == PreviewSceneId::Ocean;
        const RtGeometryKind environment_kind =
            ocean ? RtGeometryKind::Ocean : RtGeometryKind::StaticScene;
        const std::uint64_t raster_static_generation =
            ocean ? frame.raster_scene->topology_generation
                  : frame.raster_scene->geometry_generation;
        append_geometry(frame.raster_scene->environment.solid, false,
                        environment_kind,
                        RtBlasPolicy::StaticBuildCompact,
                        raster_static_generation,
                        frame.raster_scene->topology_generation,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
        append_geometry(frame.raster_scene->environment.transparent, true,
                        environment_kind,
                         ocean && frame.raster_scene->surface_dynamic_baked
                             ? RtBlasPolicy::DynamicRefit
                             : RtBlasPolicy::StaticBuildCompact,
                         frame.raster_scene->geometry_generation,
                         frame.raster_scene->topology_generation,
                        frame.rt_scene_generations.materials,
                        frame.rt_scene_generations.emission);
      }

      const auto scene_hash_begin = Clock::now();
      RtSceneGenerations effective_generations = frame.rt_scene_generations;
      if (!frame.rt_scene_generations_valid) {
        // Compatibility callers may not know the generation ABI.  A
        // conservative serial preserves correctness without scanning vertex
        // arrays; the normal application path always supplies generations.
        effective_generations = {};
        effective_generations.topology = ++rt_fallback_generation_serial_;
        effective_generations.positions = rt_fallback_generation_serial_;
        effective_generations.transforms = rt_fallback_generation_serial_;
        effective_generations.materials = rt_fallback_generation_serial_;
        effective_generations.emission = rt_fallback_generation_serial_;
        effective_generations.visibility = rt_fallback_generation_serial_;
      }
      rt_emissive_generation = effective_generations.emission;
      for (std::size_t i = 0; i < geometry_view_count; ++i) {
        RtColoredGeometryView &view = geometry_views[i];
        if (view.content_generation == 0u) {
          view.content_generation = effective_generations.positions;
        }
        if (view.topology_generation == 0u) {
          view.topology_generation = effective_generations.topology;
        }
        if (view.material_generation == 0u) {
          view.material_generation = effective_generations.materials;
        }
        if (view.emission_generation == 0u) {
          view.emission_generation = effective_generations.emission;
        }
      }
      std::uint64_t scene_hash = rtSceneGenerationKey(effective_generations);
      std::uint64_t topology_hash =
          mixRtGeneration(0xcbf29ce484222325ull,
                          effective_generations.topology);
      topology_hash = mixRtGeneration(topology_hash,
                                      effective_generations.visibility);
      for (std::size_t i = 0; i < geometry_view_count; ++i) {
        const RtColoredGeometryView &view = geometry_views[i];
        const std::uint64_t range_tag =
            static_cast<std::uint64_t>(view.vertex_count) ^
            (view.alpha_blended ? (std::uint64_t{1} << 63u) : 0u) ^
            (static_cast<std::uint64_t>(view.kind) << 48u) ^
            (static_cast<std::uint64_t>(view.blas_policy) << 40u);
        scene_hash = mixRtGeneration(scene_hash, range_tag);
        scene_hash = mixRtGeneration(scene_hash, view.content_generation);
        scene_hash = mixRtGeneration(scene_hash, view.material_generation);
        scene_hash = mixRtGeneration(scene_hash, view.emission_generation);
        topology_hash = mixRtGeneration(topology_hash, range_tag);
        topology_hash = mixRtGeneration(topology_hash,
                                        view.topology_generation);
      }
      scene_hash = mixRtGeneration(scene_hash, geometry_view_count);
      topology_hash = mixRtGeneration(topology_hash, geometry_view_count);
      rt_streamline_topology_hash = topology_hash;
      rt_streamline_material_hash = mixRtGeneration(
          rt_streamline_material_hash, effective_generations.materials);
      rt_streamline_material_hash = mixRtGeneration(
          rt_streamline_material_hash, effective_generations.emission);
      stats_.cpu_scene_hash_ms =
          std::chrono::duration<float, std::milli>(Clock::now() -
                                                   scene_hash_begin)
              .count();

      const bool have_rt_geometry = static_input || geometry_view_count != 0u;
      bool as_ok = !static_refresh_pending && rt_scene_built &&
                   rt_scene.ready() && scene_hash == last_rt_scene_hash;
      if (as_ok) {
        rt_scene.markMotionStable();
      }
      // Empty-scene transition frames are valid: there is no BLAS/TLAS input
      // to build yet, so keep the raster path quiet until geometry arrives.
      if (!as_ok && !static_refresh_pending && have_rt_geometry) {
        // CPU skin + append all scene triangles, then mark BLAS build/refit.
        const bool explicit_motion_history_valid =
            rt_motion_history_valid_ &&
            topology_hash == rt_motion_topology_hash_;
        const RtSceneStats before_scene_update = rt_scene.stats();
        const bool scene_updated = rt_scene.updateGeometry(
            packed_bones.empty() ? nullptr : packed_bones.data(),
            packed_bones.size() / 16u,
            packed_tints.empty() ? nullptr : packed_tints.data(),
            packed_tints.size() / 4u,
            std::span<const RtColoredGeometryView>(
                geometry_views.data(), geometry_view_count),
            static_input,
            explicit_motion_history_valid
                ? std::span<const float>(rt_motion_previous_positions_)
                : std::span<const float>{},
            explicit_motion_history_valid &&
                    !rt_motion_previous_bones_.empty()
                ? rt_motion_previous_bones_.data()
                : nullptr,
            explicit_motion_history_valid
                ? rt_motion_previous_bones_.size() / 16u
                : 0u,
            explicit_motion_history_valid, effective_generations,
            frame.rt_scene_generations_valid);
        const RtSceneStats after_scene_update = rt_scene.stats();
        if (after_scene_update.upload_bytes >=
            before_scene_update.upload_bytes) {
          rt_upload_bytes_frame += after_scene_update.upload_bytes -
                                   before_scene_update.upload_bytes;
        }
        rt_emitter_distribution_frame_ms +=
            after_scene_update.emitter_distribution_ms;
        if (scene_updated) {
          rt_scene_built = true;
          last_rt_scene_hash = scene_hash;
          as_ok = true;
          rt_as_needs_record = true;
          const std::span<const float> current_positions =
              rt_scene.packedPositionSnapshot();
          rt_motion_previous_positions_.assign(
              current_positions.begin(), current_positions.end());
          rt_motion_previous_bones_ = packed_bones;
          rt_motion_topology_hash_ = topology_hash;
          rt_motion_history_valid_ = true;
        } else if (!rt_fallback_logged_) {
          xpbd::log::warnf(
              "Vulkan RT: acceleration rebuild failed; using raster path (%s)",
              rt_scene.lastUpdateFailureReason());
          rt_fallback_logged_ = true;
        }
      }
      if (!have_rt_geometry) {
        rt_motion_previous_positions_.clear();
        rt_motion_previous_bones_.clear();
        rt_motion_topology_hash_ = 0u;
        rt_motion_history_valid_ = false;
      }
      if (have_rt_geometry && as_ok) {
        // Allow a later, genuine rebuild failure to be reported once without
        // reintroducing per-frame warning spam.
        rt_fallback_logged_ = false;
      }
      if (as_ok) {
        rt_shadows_active = rt_scene.ready() && rt_pipelines_ready_;
        if (rt_shadows_active) {
          const VkAccelerationStructureKHR active_tlas = rt_scene.tlas();
          const auto descriptor_update_begin = Clock::now();
          if (mesh_rt_desc_set) {
            if (last_mesh_rt_descriptor_sets_[frame_index_] !=
                    mesh_rt_desc_set ||
                last_mesh_rt_tlas_[frame_index_] != active_tlas) {
              rt_scene.writeTlasDescriptor(mesh_rt_desc_set, 0);
              last_mesh_rt_descriptor_sets_[frame_index_] = mesh_rt_desc_set;
              last_mesh_rt_tlas_[frame_index_] = active_tlas;
              ++rt_descriptor_write_calls_frame_;
              ++rt_descriptor_entries_written_frame_;
            } else {
              ++rt_descriptor_cache_hits_frame_;
            }
          } else {
            // A descriptor pool/layout refresh can temporarily remove the
            // slot.  Do not let a later reuse of the same handle look like a
            // valid cache hit after the set has been reset.
            last_mesh_rt_descriptor_sets_[frame_index_] = VK_NULL_HANDLE;
            last_mesh_rt_tlas_[frame_index_] = VK_NULL_HANDLE;
          }
          const VkDescriptorSet static_rt_set =
              static_rt_descriptor_sets_[frame_index_];
          if (static_rt_set) {
            if (last_static_rt_descriptor_sets_[frame_index_] !=
                    static_rt_set ||
                last_static_rt_tlas_[frame_index_] != active_tlas) {
              rt_scene.writeTlasDescriptor(static_rt_set, 2);
              last_static_rt_descriptor_sets_[frame_index_] = static_rt_set;
              last_static_rt_tlas_[frame_index_] = active_tlas;
              ++rt_descriptor_write_calls_frame_;
              ++rt_descriptor_entries_written_frame_;
            } else {
              ++rt_descriptor_cache_hits_frame_;
            }
          } else {
            last_static_rt_descriptor_sets_[frame_index_] = VK_NULL_HANDLE;
            last_static_rt_tlas_[frame_index_] = VK_NULL_HANDLE;
          }
          stats_.cpu_descriptor_update_ms =
              std::chrono::duration<float, std::milli>(Clock::now() -
                                                       descriptor_update_begin)
                  .count();
        }
        path_trace_active = rt_scene.ready() && path_tracer.ready();
        if (path_trace_active && !unified_rt_logged_) {
          const RtSceneStats rt_stats = rt_scene.stats();
          xpbd::log::infof(
              "Vulkan RT: unified alpha-aware primary path active "
              "(vertices=%u triangles=%u BLAS=%u TLAS=%u instances=%u)",
              rt_scene.pathTraceVertexCount(),
              rt_scene.pathTraceIndexCount() / 3u, rt_stats.blas_count,
              rt_stats.tlas_count, rt_stats.instance_count);
          unified_rt_logged_ = true;
        }
      }
    }
    if (active_render_path_ == RenderPath::RayTracing &&
        rt_capability_.device_extensions_enabled && !presentation_suspended_) {
      stats_.cpu_scene_assembly_ms =
          std::chrono::duration<float, std::milli>(Clock::now() -
                                                   rt_scene_cpu_begin)
              .count();
    }
    ResolvedSunLight registry_sun_light = resolved_sun_light;
    registry_sun_light.lighting_enabled =
        registry_sun_light.lighting_enabled &&
        resolved_environment_settings.analytic_lights;
    const RtLightRegistry rt_light_registry = buildRtLightRegistry(
        resolved_environment_view, environment_sampling_available,
        registry_sun_light, rt_emissive_generation,
        rt_scene.emissivePowerEstimate() *
            std::max(resolved_environment_settings.emissive_multiplier, 0.0f),
        resolved_environment_settings.emissive_surfaces &&
            resolved_environment_settings.emissive_mesh_sampling &&
            rt_scene.ready() && rt_scene.emissiveTriangleCount() > 0u);

