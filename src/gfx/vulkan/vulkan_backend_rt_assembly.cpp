#include "vulkan_backend_internal.hpp"

#include "xpbd/log.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace xpbd::gfx::detail {

bool VulkanBackend::assembleRtSceneSynchronousUpdate(
    const FrameInput &frame, bool static_input,
    RtSceneSynchronousAssembly &assembly) {
  assembly = {};
  try {
    if (static_input && frame.static_model_frame != nullptr &&
        !frame.static_model_frame->bones.empty()) {
      assembly.packed_bones.resize(
          frame.static_model_frame->bones.size() * 16u);
      assembly.packed_tints.resize(
          frame.static_model_frame->bones.size() * 4u);
      for (std::size_t index = 0u;
           index < frame.static_model_frame->bones.size(); ++index) {
        std::memcpy(assembly.packed_bones.data() + index * 16u,
                    frame.static_model_frame->bones[index].transform.data(),
                    16u * sizeof(float));
        std::memcpy(assembly.packed_tints.data() + index * 4u,
                    frame.static_model_frame->bones[index].tint.data(),
                    4u * sizeof(float));
      }
    }

    const auto append_geometry =
        [&](const std::vector<MeshVertex> &vertices, bool alpha_blended,
            RtGeometryKind kind, RtBlasPolicy blas_policy,
            std::uint64_t content_generation,
            std::uint64_t topology_generation,
            std::uint64_t material_generation,
            std::uint64_t emission_generation) {
          if (vertices.empty() ||
              assembly.geometry_view_count >=
                  assembly.geometry_views.size()) {
            return;
          }
          assembly.geometry_views[assembly.geometry_view_count++] = {
              vertices.data(), vertices.size(), alpha_blended, kind,
              blas_policy, content_generation, topology_generation,
              material_generation, emission_generation};
        };
    if (frame.scene != nullptr) {
      append_geometry(
          frame.scene->solid, false, RtGeometryKind::SkinnedModel,
          RtBlasPolicy::DynamicRefit, frame.rt_scene_generations.positions,
          frame.rt_scene_generations.topology,
          frame.rt_scene_generations.materials,
          frame.rt_scene_generations.emission);
      append_geometry(
          frame.scene->transparent, true, RtGeometryKind::SkinnedModel,
          RtBlasPolicy::DynamicRefit, frame.rt_scene_generations.positions,
          frame.rt_scene_generations.topology,
          frame.rt_scene_generations.materials,
          frame.rt_scene_generations.emission);
    }
    if (frame.raster_scene != nullptr) {
      const bool ocean = frame.raster_scene->id == PreviewSceneId::Ocean;
      const RtGeometryKind environment_kind =
          ocean ? RtGeometryKind::Ocean : RtGeometryKind::StaticScene;
      const std::uint64_t raster_static_generation =
          ocean ? frame.raster_scene->topology_generation
                : frame.raster_scene->geometry_generation;
      append_geometry(
          frame.raster_scene->environment.solid, false, environment_kind,
          RtBlasPolicy::StaticBuildCompact, raster_static_generation,
          frame.raster_scene->topology_generation,
          frame.rt_scene_generations.materials,
          frame.rt_scene_generations.emission);
      append_geometry(
          frame.raster_scene->environment.transparent, true,
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
    assembly.effective_generations = frame.rt_scene_generations;
    if (!frame.rt_scene_generations_valid) {
      assembly.effective_generations = {};
      assembly.effective_generations.topology =
          ++rt_fallback_generation_serial_;
      assembly.effective_generations.positions =
          rt_fallback_generation_serial_;
      assembly.effective_generations.transforms =
          rt_fallback_generation_serial_;
      assembly.effective_generations.materials =
          rt_fallback_generation_serial_;
      assembly.effective_generations.emission =
          rt_fallback_generation_serial_;
      assembly.effective_generations.visibility =
          rt_fallback_generation_serial_;
    }
    assembly.emissive_generation =
        assembly.effective_generations.emission;
    for (std::size_t index = 0u;
         index < assembly.geometry_view_count; ++index) {
      RtColoredGeometryView &view = assembly.geometry_views[index];
      if (view.content_generation == 0u) {
        view.content_generation = assembly.effective_generations.positions;
      }
      if (view.topology_generation == 0u) {
        view.topology_generation = assembly.effective_generations.topology;
      }
      if (view.material_generation == 0u) {
        view.material_generation = assembly.effective_generations.materials;
      }
      if (view.emission_generation == 0u) {
        view.emission_generation = assembly.effective_generations.emission;
      }
    }

    std::uint64_t scene_hash =
        rtSceneGenerationKey(assembly.effective_generations);
    std::uint64_t topology_hash = mixRtGeneration(
        0xcbf29ce484222325ull,
        assembly.effective_generations.topology);
    topology_hash = mixRtGeneration(
        topology_hash, assembly.effective_generations.visibility);
    for (std::size_t index = 0u;
         index < assembly.geometry_view_count; ++index) {
      const RtColoredGeometryView &view = assembly.geometry_views[index];
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
      topology_hash =
          mixRtGeneration(topology_hash, view.topology_generation);
    }
    scene_hash =
        mixRtGeneration(scene_hash, assembly.geometry_view_count);
    topology_hash =
        mixRtGeneration(topology_hash, assembly.geometry_view_count);
    assembly.streamline_topology_hash = topology_hash;
    assembly.streamline_material_hash = mixRtGeneration(
        assembly.streamline_material_hash,
        assembly.effective_generations.materials);
    assembly.streamline_material_hash = mixRtGeneration(
        assembly.streamline_material_hash,
        assembly.effective_generations.emission);
    assembly.scene_hash_ms =
        std::chrono::duration<float, std::milli>(Clock::now() -
                                                 scene_hash_begin)
            .count();

    assembly.have_rt_geometry =
        static_input || assembly.geometry_view_count != 0u;
    const bool explicit_motion_history_valid =
        rt_motion_history_valid_ &&
        topology_hash == rt_motion_topology_hash_;
    assembly.update.bone_transforms =
        assembly.packed_bones.empty()
            ? nullptr
            : assembly.packed_bones.data();
    assembly.update.bone_count = assembly.packed_bones.size() / 16u;
    assembly.update.bone_tints =
        assembly.packed_tints.empty()
            ? nullptr
            : assembly.packed_tints.data();
    assembly.update.tint_count = assembly.packed_tints.size() / 4u;
    assembly.update.colored_geometry =
        std::span<const RtColoredGeometryView>(
            assembly.geometry_views.data(), assembly.geometry_view_count);
    assembly.update.include_rest_model = static_input;
    assembly.update.previous_packed_positions =
        explicit_motion_history_valid
            ? std::span<const float>(rt_motion_previous_positions_)
            : std::span<const float>{};
    assembly.update.previous_bone_transforms =
        explicit_motion_history_valid && !rt_motion_previous_bones_.empty()
            ? rt_motion_previous_bones_.data()
            : nullptr;
    assembly.update.previous_bone_count =
        explicit_motion_history_valid
            ? rt_motion_previous_bones_.size() / 16u
            : 0u;
    assembly.update.explicit_motion_history_valid =
        explicit_motion_history_valid;
    assembly.update.generations = assembly.effective_generations;
    assembly.update.generations_valid = frame.rt_scene_generations_valid;
    assembly.update.scene_hash = scene_hash;
    assembly.update.topology_hash = topology_hash;
    return true;
  } catch (const std::bad_alloc &) {
    writeLog("Vulkan RT synchronous assembly allocation failed");
  } catch (const std::exception &exception) {
    xpbd::log::errorf("Vulkan RT synchronous assembly failed: %s",
                      exception.what());
  } catch (...) {
    writeLog("Vulkan RT synchronous assembly failed");
  }
  return false;
}

bool VulkanBackend::uploadStaticAssetPacket(
    const RenderFramePacket &packet, std::uint64_t &uploaded_bytes,
    bool defer_commit) {
  uploaded_bytes = 0u;
  if (fatal_error_ || quarantine_required_ || device_ == VK_NULL_HANDLE) {
    return false;
  }
  RenderFramePacketView packet_view(packet);
  const FrameInput &frame = packet_view.frame();
  const bool static_input = frame.static_model != nullptr &&
                            frame.static_model_frame != nullptr;
  if (!static_input) {
    return false;
  }
  if (!static_generations_.needsRefresh(frame.static_model_generation,
                                        frame.static_texture_generation)) {
    return true;
  }

  RtSceneSynchronousAssembly assembly;
  const RtSceneSynchronousUpdate *rt_update = nullptr;
  if (rt_capability_.device_extensions_enabled) {
    if (!assembleRtSceneSynchronousUpdate(frame, true, assembly)) {
      return false;
    }
    rt_update = &assembly.update;
  }
  if (!rebuildStaticModelResources(
          *frame.static_model, frame.static_model_texture,
          frame.static_model_material, frame.static_model_generation,
          frame.static_texture_generation, uploaded_bytes, rt_update,
          defer_commit)) {
    return false;
  }

  // Upload publication is not temporal authority. Motion history advances
  // only after a worker-consumed frame is successfully presented.
  return true;
}

bool VulkanBackend::uploadEnvironmentPacket(
    const RenderFramePacket &packet, std::uint64_t &uploaded_bytes,
    bool defer_commit) {
  uploaded_bytes = 0u;
  if (fatal_error_ || quarantine_required_ || device_ == VK_NULL_HANDLE) {
    return false;
  }

  const ResolvedWorldEnvironment resolved_source =
      packet.scene.world_environment
          ? resolveWorldEnvironment(*packet.scene.world_environment)
          : ResolvedWorldEnvironment{};
  ResolvedWorldEnvironment resolved = resolved_source;
  const bool procedural_ready =
      ensureProceduralAtmosphereResources(resolved);
  const bool hdri_ready = rt_capability_.device_extensions_enabled
                              ? ensureWorldEnvironmentResources(
                                    resolved, defer_commit)
                              : false;
  if (atmosphere_environment_pending_.active()) {
    (void)pollDynamicSkyEnvironmentCache(true);
  }
  if (fatal_error_ || quarantine_required_) {
    return false;
  }
  if (world_environment_pending_.active) {
    // The command packet remains owned by the reliable worker while the
    // Candidate fence is pending. Preview skybox publication is deliberately
    // deferred to pollEnvironmentPacket so no frame can observe a half-new
    // environment pair.
    return true;
  }
  if (resolved.sky_rendering == SkyRendering::ProceduralDayNight &&
      (!procedural_ready ||
       (resolved.environment_lighting &&
        !atmosphere_environment_ready_))) {
    return false;
  }
  if (resolved.sky_rendering == SkyRendering::UserHdri && !hdri_ready) {
    return false;
  }

  if (packet.scene.preview_skybox &&
      packet.scene.preview_skybox->valid() &&
      !uploadSkyboxCubemap(*packet.scene.preview_skybox)) {
    return false;
  }

  const auto saturating_add = [&](std::uint64_t bytes) {
    uploaded_bytes =
        bytes > (std::numeric_limits<std::uint64_t>::max)() - uploaded_bytes
            ? (std::numeric_limits<std::uint64_t>::max)()
            : uploaded_bytes + bytes;
  };
  if (resolved.hdr != nullptr && resolved.hdr->valid()) {
    const std::uint64_t texels =
        static_cast<std::uint64_t>(resolved.hdr->radiance.width) *
        resolved.hdr->radiance.height;
    saturating_add(texels * 4u * sizeof(float));
  }
  if (packet.scene.preview_skybox) {
    saturating_add(static_cast<std::uint64_t>(
        packet.scene.preview_skybox->rgba.size()));
  }
  return true;
}

bool VulkanBackend::pollEnvironmentPacket(
    const RenderFramePacket &packet, std::uint64_t &uploaded_bytes,
    bool wait_for_completion, bool &complete, bool &superseded) {
  uploaded_bytes = 0u;
  complete = false;
  superseded = false;
  bool candidate_ready = false;
  if (!pollWorldEnvironmentPending(wait_for_completion,
                                   candidate_ready, superseded)) {
    return false;
  }
  if (superseded) {
    complete = true;
    return true;
  }
  if (!candidate_ready) {
    return true;
  }
  if (!world_environment_pending_.active) {
    complete = true;
    return true;
  }

  bool retirement_complete = false;
  if (!pollWorldEnvironmentRetirement(wait_for_completion,
                                      retirement_complete)) {
    return false;
  }
  if (!retirement_complete) {
    return true;
  }
  if (!beginWorldEnvironmentRetirement()) {
    discardWorldEnvironmentPending(
        "environment-retirement-submit-failure");
    return false;
  }

  // If the packet carries a raster preview cubemap, finish that independent
  // synchronous transaction after the old-owner retirement marker and before
  // the no-fail HDRI pointer swap. The worker cannot render between these
  // operations, so publication remains one observable command boundary.
  if (packet.scene.preview_skybox &&
      packet.scene.preview_skybox->valid() &&
      !uploadSkyboxCubemap(*packet.scene.preview_skybox)) {
    discardWorldEnvironmentPending(
        "environment-preview-skybox-failure");
    return false;
  }

  if (!commitWorldEnvironmentPending(uploaded_bytes)) {
    recordFatalError(
        "commitWorldEnvironmentPending",
        "Vulkan environment publication failed after its retirement "
        "boundary");
    return false;
  }
  if (packet.scene.preview_skybox) {
    const std::uint64_t skybox_bytes = static_cast<std::uint64_t>(
        packet.scene.preview_skybox->rgba.size());
    uploaded_bytes =
        skybox_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - uploaded_bytes
            ? (std::numeric_limits<std::uint64_t>::max)()
            : uploaded_bytes + skybox_bytes;
  }
  complete = true;
  return true;
}

} // namespace xpbd::gfx::detail
