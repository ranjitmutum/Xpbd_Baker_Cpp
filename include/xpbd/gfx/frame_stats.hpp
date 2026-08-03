#pragma once

#include <cstdint>

namespace xpbd::gfx {

enum class RtAccelerationBuildReason : std::uint32_t {
  None = 0,
  InitialBuild,
  TopologyChanged,
  StorageReallocated,
  MissingTopLevel,
  InstanceTransformsChanged,
  StableGeometryRefit,
  OtherFullBuild,
};

[[nodiscard]] constexpr const char *
rtAccelerationBuildReasonName(RtAccelerationBuildReason reason) noexcept {
  switch (reason) {
  case RtAccelerationBuildReason::None:
    return "none";
  case RtAccelerationBuildReason::InitialBuild:
    return "initial";
  case RtAccelerationBuildReason::TopologyChanged:
    return "topology";
  case RtAccelerationBuildReason::StorageReallocated:
    return "storage";
  case RtAccelerationBuildReason::MissingTopLevel:
    return "missing TLAS";
  case RtAccelerationBuildReason::InstanceTransformsChanged:
    return "instance transforms";
  case RtAccelerationBuildReason::StableGeometryRefit:
    return "stable refit";
  case RtAccelerationBuildReason::OtherFullBuild:
    return "full build";
  }
  return "unknown";
}

struct RtEmitterVisibilityAudit {
  bool hidden_source_emitter = false;
  bool hidden_positive_weight = false;
};

// Read-only classification used while rebuilding the mesh-light table. It
// never changes the source radiance or sampling weight; it only exposes
// whether a hidden primitive was an authored emitter and whether a positive
// final weight incorrectly survived visibility suppression.
[[nodiscard]] inline constexpr RtEmitterVisibilityAudit
auditRtEmitterVisibility(float visibility_scale, bool source_emission_positive,
                         double final_weight) noexcept {
  const bool hidden = !(visibility_scale > 0.0f);
  return {
      hidden && source_emission_positive,
      hidden && final_weight > 0.0,
  };
}

struct FrameStats {
  float frame_ms = 0.0f;
  float ema_frame_ms = 16.0f;
  float fps = 0.0f;
  // Application-rendered cadence and the actual number of frames reported by
  // Streamline after each present. The UI derives presented FPS from these
  // values instead of assuming that requested FG is active.
  bool dlss_frame_generation_supported = false;
  bool dlss_frame_generation_requested = false;
  bool dlss_frame_generation_active = false;
  bool reflex_supported = false;
  std::uint32_t dlss_frames_actually_presented = 1;
  float mesh_ms = 0.0f;
  float pick_ms = 0.0f;
  std::uint32_t pick_queries = 0;
  std::uint32_t pick_cache_rebuilds = 0;
  std::uint32_t pick_candidate_faces = 0;
  std::uint32_t pick_total_faces = 0;


  float gpu_ms = 0.0f;

  float backend_cpu_ms = 0.0f;
  // PERF00 RT instrumentation.  These are CPU-side recording costs; GPU
  // timestamp fields below are populated independently when the device
  // exposes timestamp queries.
  float cpu_scene_assembly_ms = 0.0f;
  float cpu_scene_hash_ms = 0.0f;
  float cpu_emitter_distribution_ms = 0.0f;
  float cpu_descriptor_update_ms = 0.0f;
  float gpu_as_build_ms = 0.0f;
  float gpu_path_trace_ms = 0.0f;
  float gpu_rr_ms = 0.0f;
  float gpu_sr_ms = 0.0f;
  float gpu_fg_present_ms = 0.0f;
  bool gpu_timestamp_valid = false;
  float gpu_timestamp_total_ms = 0.0f;
  float gpu_timestamp_ui_ms = 0.0f;
  float gpu_timestamp_opaque_ms = 0.0f;
  float gpu_timestamp_transparent_ms = 0.0f;
  float gpu_timestamp_lines_ms = 0.0f;
  std::uint32_t gpu_timestamp_valid_bits = 0;
  float gpu_timestamp_period_ns = 0.0f;
  int cube_count = 0;
  int line_count = 0;
  int draw_calls = 0;
  int ui_commands = 0;
  float upload_ms = 0.0f;
  std::uint64_t upload_bytes = 0;
  std::uint64_t ui_upload_bytes = 0;
  std::uint64_t mesh_upload_bytes = 0;

  std::uint64_t static_bone_upload_bytes = 0;

  std::uint64_t static_resource_upload_bytes = 0;

  std::uint64_t static_resource_rebuilds = 0;
  std::uint64_t static_model_vertex_bytes = 0;
  std::uint64_t static_model_index_bytes = 0;
  std::uint32_t static_opaque_index_count = 0;
  std::uint32_t static_cutout_index_count = 0;
  std::uint32_t static_blend_index_count = 0;
  std::uint64_t mesh_arena_capacity_bytes = 0;
  std::uint64_t ui_vertex_capacity_bytes = 0;
  std::uint64_t ui_index_capacity_bytes = 0;
  std::uint64_t mesh_solid_offset_bytes = 0;
  std::uint64_t mesh_transparent_offset_bytes = 0;
  std::uint64_t mesh_line_offset_bytes = 0;
  int buffer_reallocations = 0;
  std::uint64_t total_buffer_reallocations = 0;

  // NVIDIA RT path selection (0 = Raster, 1 = RayTracing). See RenderPath.
  int active_render_path = 0;
  // Set only when the most recent backend render reached a successful
  // presentation result. The sequence is monotonic for the backend lifetime.
  bool present_succeeded = false;
  std::uint64_t present_success_count = 0;
  bool ray_tracing_supported = false;
  bool ray_tracing_requested = false;
  std::uint32_t rt_blas_count = 0;
  std::uint32_t rt_tlas_count = 0;
  std::uint32_t rt_instance_count = 0;
  // Active RT scene diagnostics. Unlike rt_instance_count (which reflects
  // all frame-slot scenes), these describe the TLAS used by this frame.
  std::uint32_t rt_visible_instance_mask_count = 0;
  std::uint32_t rt_hidden_instance_mask_count = 0;
  std::uint32_t rt_positive_emitter_count = 0;
  std::uint32_t rt_hidden_source_emitter_triangle_count = 0;
  std::uint32_t rt_hidden_positive_weight_triangle_count = 0;
  std::uint32_t rt_primitive_count = 0;
  std::uint64_t rt_as_storage_bytes = 0;
  std::uint64_t rt_scratch_bytes = 0;
  std::uint64_t rt_attribute_bytes = 0;
  std::uint64_t rt_allocated_bytes = 0;
  std::uint64_t rt_full_builds = 0;
  std::uint64_t rt_refits = 0;
  std::uint64_t rt_tlas_full_builds = 0;
  std::uint64_t rt_tlas_updates = 0;
  std::uint64_t rt_upload_bytes = 0;
  std::uint64_t rt_emitter_distribution_rebuilds = 0;
  std::uint64_t rt_descriptor_write_calls = 0;
  std::uint64_t rt_descriptor_cache_hits = 0;
  std::uint64_t rt_descriptor_entries_written = 0;
  std::uint32_t rt_aov_write_mask = 0;
  RtAccelerationBuildReason rt_last_build_reason =
      RtAccelerationBuildReason::None;
  RtAccelerationBuildReason rt_last_tlas_reason =
      RtAccelerationBuildReason::None;
};

}
