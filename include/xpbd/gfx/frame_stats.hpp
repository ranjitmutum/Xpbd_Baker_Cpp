#pragma once

#include <cstdint>

namespace xpbd::gfx {

struct FrameStats {
  float frame_ms = 0.0f;
  float ema_frame_ms = 16.0f;
  float fps = 0.0f;
  float mesh_ms = 0.0f;
  float pick_ms = 0.0f;
  std::uint32_t pick_queries = 0;
  std::uint32_t pick_cache_rebuilds = 0;
  std::uint32_t pick_candidate_faces = 0;
  std::uint32_t pick_total_faces = 0;


  float gpu_ms = 0.0f;

  float backend_cpu_ms = 0.0f;
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
};

}
