#include "xpbd/gfx/render_thread_contract.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace xpbd::gfx {
namespace {

template <typename T>
std::shared_ptr<const T> refreshCachedSnapshot(
    const T *source, std::uint64_t generation, const T *&cached_source,
    std::uint64_t &cached_generation, std::shared_ptr<const T> &cached_value) {
  if (source == nullptr) {
    return {};
  }
  if (source != cached_source || generation != cached_generation ||
      !cached_value) {
    cached_value = std::make_shared<const T>(*source);
    cached_source = source;
    cached_generation = generation;
  }
  return cached_value;
}

std::uint64_t rasterGenerationKey(const ViewportRasterScene &scene) noexcept {
  std::uint64_t key = mixRtGeneration(scene.geometry_generation,
                                     scene.topology_generation);
  key = mixRtGeneration(key, scene.skybox.generation);
  key = mixRtGeneration(key, static_cast<std::uint64_t>(scene.id));
  key = mixRtGeneration(key, scene.show_grid ? 1u : 0u);
  key = mixRtGeneration(key, scene.show_axes ? 1u : 0u);
  key = mixRtGeneration(key, scene.show_environment ? 1u : 0u);
  key = mixRtGeneration(key, scene.solid_ground ? 1u : 0u);
  key = mixRtGeneration(key, scene.environment_unlit ? 1u : 0u);
  for (const float value : scene.lighting.direction) {
    key = mixRtGeneration(key, std::bit_cast<std::uint32_t>(value));
  }
  key = mixRtGeneration(
      key, std::bit_cast<std::uint32_t>(scene.lighting.ambient));
  for (const float value : scene.lighting.color) {
    key = mixRtGeneration(key, std::bit_cast<std::uint32_t>(value));
  }
  key = mixRtGeneration(
      key, std::bit_cast<std::uint32_t>(scene.lighting.intensity));
  key = mixRtGeneration(
      key, std::bit_cast<std::uint32_t>(scene.lighting.clear_r));
  key = mixRtGeneration(
      key, std::bit_cast<std::uint32_t>(scene.lighting.clear_g));
  key = mixRtGeneration(
      key, std::bit_cast<std::uint32_t>(scene.lighting.clear_b));
  key = mixRtGeneration(key, std::bit_cast<std::uint32_t>(scene.scene_seed));
  key = mixRtGeneration(
      key, std::bit_cast<std::uint32_t>(scene.surface_time_baked));
  key = mixRtGeneration(key, scene.surface_dynamic_baked ? 1u : 0u);
  return key;
}

void copyMatrix(const float *source, std::array<float, 16> &destination) {
  if (source != nullptr) {
    std::copy_n(source, destination.size(), destination.begin());
  }
}

} // namespace

bool OwnedUiDrawData::valid() const noexcept {
  if (vertices.empty() || indices.empty() || commands.empty()) {
    return false;
  }
  return std::all_of(commands.begin(), commands.end(), [&](const auto &command) {
    const std::uint64_t end = static_cast<std::uint64_t>(command.index_offset) +
                              command.element_count;
    return command.element_count > 0u &&
           end <= (std::numeric_limits<std::uint32_t>::max)();
  });
}

UiDrawData OwnedUiDrawData::view() const noexcept {
  UiDrawData result{};
  result.vertex_data = vertices.empty() ? nullptr : vertices.data();
  result.vertex_bytes = vertices.size();
  result.index_data = indices.empty() ? nullptr : indices.data();
  result.index_bytes = indices.size();
  result.draw_commands = commands.empty() ? nullptr : commands.data();
  result.draw_command_count = commands.size();
  result.logical_w = logical_w;
  result.logical_h = logical_h;
  result.fb_w = fb_w;
  result.fb_h = fb_h;
  result.overlay_visible = overlay_visible;
  result.overlay_x = overlay_x;
  result.overlay_y = overlay_y;
  result.overlay_w = overlay_w;
  result.overlay_h = overlay_h;
  return result;
}

OwnedUiDrawData OwnedUiDrawData::copyOf(const UiDrawData *source) {
  OwnedUiDrawData result;
  if (source == nullptr) {
    return result;
  }
  result.logical_w = source->logical_w;
  result.logical_h = source->logical_h;
  result.fb_w = source->fb_w;
  result.fb_h = source->fb_h;
  result.overlay_visible = source->overlay_visible;
  result.overlay_x = source->overlay_x;
  result.overlay_y = source->overlay_y;
  result.overlay_w = source->overlay_w;
  result.overlay_h = source->overlay_h;
  if (source->vertex_data != nullptr && source->vertex_bytes > 0u) {
    const auto *begin = static_cast<const std::byte *>(source->vertex_data);
    result.vertices.assign(begin, begin + source->vertex_bytes);
  }
  if (source->index_data != nullptr && source->index_bytes > 0u) {
    const auto *begin = static_cast<const std::byte *>(source->index_data);
    result.indices.assign(begin, begin + source->index_bytes);
  }
  if (source->draw_commands != nullptr && source->draw_command_count > 0u) {
    result.commands.assign(source->draw_commands,
                           source->draw_commands + source->draw_command_count);
  }
  return result;
}

RenderFramePacketView::RenderFramePacketView(
    const RenderFramePacket &packet) noexcept {
  ui_ = packet.ui.view();
  frame_.fb_width = packet.fb_width;
  frame_.fb_height = packet.fb_height;
  frame_.viewport = packet.viewport;
  frame_.view_matrix = packet.view_matrix.data();
  frame_.proj_matrix = packet.proj_matrix.data();
  frame_.scene = packet.scene.scene.get();
  frame_.static_model = packet.scene.static_model.get();
  frame_.static_model_frame = packet.scene.static_model_frame.get();
  frame_.static_model_texture = packet.scene.static_model_texture.get();
  frame_.static_model_material = packet.scene.static_model_material.get();
  frame_.static_model_generation = packet.static_model_generation;
  frame_.static_texture_generation = packet.static_texture_generation;
  frame_.material_debug_view = packet.material_debug_view;
  frame_.rt_debug_view = packet.rt_debug_view;
  frame_.rr_aov_debug_view = packet.rr_aov_debug_view;
  frame_.path_trace_settings = packet.path_trace_settings;
  frame_.world_environment = packet.scene.world_environment.get();
  frame_.ui = packet.ui.valid() ? &ui_ : nullptr;
  frame_.diagnostics = packet.diagnostics;
  frame_.clear_r = packet.clear_r;
  frame_.clear_g = packet.clear_g;
  frame_.clear_b = packet.clear_b;
  frame_.raster_scene = packet.scene.raster_scene.get();
  frame_.prefer_ray_tracing = packet.prefer_ray_tracing;
  frame_.interactive_viewport_resize = packet.interactive_viewport_resize;
  frame_.rt_scene_generations = packet.rt_scene_generations;
  frame_.rt_scene_generations_valid = packet.rt_scene_generations_valid;
}

std::shared_ptr<const RenderFramePacket> RenderFramePacketBuilder::build(
    std::uint64_t ui_frame_serial, const FrameInput &source) {
  auto packet = std::make_shared<RenderFramePacket>();
  packet->ui_frame_serial = ui_frame_serial;
  packet->packet_serial = next_packet_serial_++;
  packet->fb_width = source.fb_width;
  packet->fb_height = source.fb_height;
  packet->viewport = source.viewport;
  copyMatrix(source.view_matrix, packet->view_matrix);
  copyMatrix(source.proj_matrix, packet->proj_matrix);
  packet->static_model_generation = source.static_model_generation;
  packet->static_texture_generation = source.static_texture_generation;
  packet->material_debug_view = source.material_debug_view;
  packet->rt_debug_view = source.rt_debug_view;
  packet->rr_aov_debug_view = source.rr_aov_debug_view;
  packet->path_trace_settings = source.path_trace_settings;
  packet->diagnostics = source.diagnostics;
  packet->ui = OwnedUiDrawData::copyOf(source.ui);
  packet->clear_r = source.clear_r;
  packet->clear_g = source.clear_g;
  packet->clear_b = source.clear_b;
  packet->prefer_ray_tracing = source.prefer_ray_tracing;
  packet->interactive_viewport_resize = source.interactive_viewport_resize;
  packet->rt_scene_generations = source.rt_scene_generations;
  packet->rt_scene_generations_valid = source.rt_scene_generations_valid;

  packet->scene.static_model = refreshCachedSnapshot(
      source.static_model, source.static_model_generation,
      cached_static_model_source_, cached_static_model_generation_,
      cached_static_model_);
  if (source.static_model_frame != nullptr) {
    packet->scene.static_model_frame =
        std::make_shared<const StaticModelFrameData>(*source.static_model_frame);
  }
  if (source.scene != nullptr && source.static_model_frame != nullptr &&
      source.scene == &source.static_model_frame->overlays &&
      packet->scene.static_model_frame) {
    packet->scene.scene = std::shared_ptr<const ViewportGpuScene>(
        packet->scene.static_model_frame,
        &packet->scene.static_model_frame->overlays);
  } else {
    const std::uint64_t scene_generation =
        source.rt_scene_generations_valid
            ? rtSceneGenerationKey(source.rt_scene_generations)
            : source.diagnostics.render_frame;
    packet->scene.scene = refreshCachedSnapshot(
        source.scene, scene_generation, cached_scene_source_,
        cached_scene_generation_, cached_scene_);
  }
  packet->scene.static_model_texture = refreshCachedSnapshot(
      source.static_model_texture, source.static_texture_generation,
      cached_texture_source_, cached_texture_generation_, cached_texture_);
  packet->scene.static_model_material = refreshCachedSnapshot(
      source.static_model_material, source.static_texture_generation,
      cached_material_source_, cached_material_generation_, cached_material_);
  const std::uint64_t world_generation =
      source.world_environment != nullptr
          ? mixRtGeneration(source.world_environment->generation,
                            source.world_environment->hdri_runtime_generation)
          : 0u;
  packet->scene.world_environment = refreshCachedSnapshot(
      source.world_environment, world_generation, cached_world_source_,
      cached_world_generation_, cached_world_);
  const std::uint64_t raster_generation =
      source.raster_scene != nullptr
          ? rasterGenerationKey(*source.raster_scene)
          : 0u;
  packet->scene.raster_scene = refreshCachedSnapshot(
      source.raster_scene, raster_generation, cached_raster_source_,
      cached_raster_generation_, cached_raster_);
  if (packet->scene.raster_scene &&
      packet->scene.raster_scene->skybox.valid()) {
    packet->scene.preview_skybox = std::shared_ptr<const PreviewSkybox>(
        packet->scene.raster_scene, &packet->scene.raster_scene->skybox);
  }
  return packet;
}

bool StartStillRenderCommand::valid() const noexcept {
  return job_id != 0u && width > 0u && height > 0u && target_samples > 0u &&
         samples_per_submit > 0u && !output_path.empty() &&
         source_packet_serial != 0u;
}

std::optional<StartStillRenderCommand> makeStartStillRenderCommand(
    const RenderFramePacket &packet, const StillRenderFrameRequest &request) {
  if (request.job_id == 0u || request.view_matrix == nullptr ||
      request.proj_matrix == nullptr || request.output_path.empty()) {
    return std::nullopt;
  }
  StartStillRenderCommand result;
  result.job_id = request.job_id;
  result.width = request.width;
  result.height = request.height;
  result.target_samples = request.target_samples;
  result.samples_per_submit = request.samples_per_submit;
  result.format = request.format;
  result.transparent_background = request.transparent_background;
  result.output_path = request.output_path;
  copyMatrix(request.view_matrix, result.view_matrix);
  copyMatrix(request.proj_matrix, result.proj_matrix);
  result.viewport = {0, 0, static_cast<int>(request.width),
                     static_cast<int>(request.height)};
  result.path_trace_settings = request.path_trace_settings;
  result.material_debug_view = request.material_debug_view;
  result.rt_debug_view = request.rt_debug_view;
  result.clear_r = packet.clear_r;
  result.clear_g = packet.clear_g;
  result.clear_b = packet.clear_b;
  result.scene = packet.scene;
  if (!result.scene.preview_skybox && request.preview_skybox != nullptr &&
      request.preview_skybox->valid()) {
    result.scene.preview_skybox =
        std::make_shared<const PreviewSkybox>(*request.preview_skybox);
  }
  result.source_packet_serial = packet.packet_serial;
  result.static_model_generation = packet.static_model_generation;
  result.static_texture_generation = packet.static_texture_generation;
  result.rt_scene_generations = packet.rt_scene_generations;
  result.rt_scene_generations_valid = packet.rt_scene_generations_valid;
  if (!result.valid()) {
    return std::nullopt;
  }
  return result;
}

HistoryCommitSnapshot makeHistoryCommitCandidate(
    const RenderFramePacket &packet, std::uint64_t render_serial) noexcept {
  HistoryCommitSnapshot result;
  result.render_serial = render_serial;
  result.packet_serial = packet.packet_serial;
  result.ui_frame_serial = packet.ui_frame_serial;
  result.view_matrix = packet.view_matrix;
  result.proj_matrix = packet.proj_matrix;
  result.viewport = packet.viewport;
  result.static_model = packet.scene.static_model;
  result.pose = packet.scene.static_model_frame;
  result.visibility_and_instances = packet.scene.raster_scene;
  result.rt_scene_generations = packet.rt_scene_generations;
  result.rt_scene_generations_valid = packet.rt_scene_generations_valid;
  return result;
}

bool TemporalHistoryLedger::commit(HistoryCommitSnapshot candidate) noexcept {
  if (candidate.render_serial == 0u || candidate.packet_serial == 0u ||
      candidate.render_serial <= last_render_serial_) {
    return false;
  }
  candidate.history_serial = next_history_serial_++;
  last_render_serial_ = candidate.render_serial;
  current_ = std::move(candidate);
  return true;
}

render::BonePickIndex
buildLastPresentedBonePickIndex(const LastPresentedSnapshot &snapshot) {
  render::SkeletonDrawList draw_list;
  const HistoryCommitSnapshot &history = snapshot.history;
  const auto &mesh = history.static_model;
  const auto &pose = history.pose;
  const float view_w = static_cast<float>(history.viewport.w);
  const float view_h = static_cast<float>(history.viewport.h);
  if (snapshot.present_serial == 0u || !mesh || !pose || view_w < 1.0f ||
      view_h < 1.0f || pose->bones.size() != mesh->bone_names.size()) {
    return render::buildBonePickIndex(std::move(draw_list), view_w, view_h,
                                      64.0f, 32.0f);
  }

  const auto transformPoint = [](const std::array<float, 16> &matrix,
                                 float x, float y, float z) {
    return std::array<float, 4>{
        matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
        matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
        matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14],
        matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15]};
  };
  const auto transformHomogeneous = [](const std::array<float, 16> &matrix,
                                       const std::array<float, 4> &point) {
    return std::array<float, 4>{
        matrix[0] * point[0] + matrix[4] * point[1] +
            matrix[8] * point[2] + matrix[12] * point[3],
        matrix[1] * point[0] + matrix[5] * point[1] +
            matrix[9] * point[2] + matrix[13] * point[3],
        matrix[2] * point[0] + matrix[6] * point[1] +
            matrix[10] * point[2] + matrix[14] * point[3],
        matrix[3] * point[0] + matrix[7] * point[1] +
            matrix[11] * point[2] + matrix[15] * point[3]};
  };

  draw_list.faces.reserve(mesh->faces.size());
  for (const StaticModelFace &source_face : mesh->faces) {
    const std::size_t bone_index = source_face.bone_index;
    const std::size_t first_vertex = source_face.first_vertex;
    if (source_face.vertex_count < 4u || bone_index >= pose->bones.size() ||
        bone_index >= mesh->bone_names.size() ||
        first_vertex > mesh->vertices.size() ||
        4u > mesh->vertices.size() - first_vertex ||
        !(pose->bones[bone_index].tint[3] > 0.0f)) {
      continue;
    }

    render::ProjectedFace projected;
    projected.bone_name = mesh->bone_names[bone_index];
    bool valid = !projected.bone_name.empty();
    float depth_sum = 0.0f;
    for (std::size_t corner = 0; valid && corner < 4u; ++corner) {
      const StaticModelVertex &vertex = mesh->vertices[first_vertex + corner];
      const auto world = transformPoint(pose->bones[bone_index].transform,
                                        vertex.px, vertex.py, vertex.pz);
      const auto view = transformHomogeneous(history.view_matrix, world);
      const auto clip = transformHomogeneous(history.proj_matrix, view);
      const float depth = -view[2];
      if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) ||
          !std::isfinite(clip[2]) || !std::isfinite(clip[3]) ||
          !std::isfinite(depth) || clip[3] <= 1.0e-6f || depth <= 0.0f ||
          clip[2] < -clip[3] || clip[2] > clip[3]) {
        valid = false;
        break;
      }
      const float inverse_w = 1.0f / clip[3];
      const float ndc_x = clip[0] * inverse_w;
      const float ndc_y = clip[1] * inverse_w;
      projected.xy[corner * 2u] = (ndc_x * 0.5f + 0.5f) * view_w;
      projected.xy[corner * 2u + 1u] =
          (1.0f - (ndc_y * 0.5f + 0.5f)) * view_h;
      if (!std::isfinite(projected.xy[corner * 2u]) ||
          !std::isfinite(projected.xy[corner * 2u + 1u])) {
        valid = false;
        break;
      }
      projected.depths[corner] = depth;
      depth_sum += depth;
    }
    if (!valid) {
      continue;
    }
    projected.depth = depth_sum * 0.25f;
    projected.is_ground = false;
    draw_list.faces.push_back(std::move(projected));
  }
  return render::buildBonePickIndex(std::move(draw_list), view_w, view_h,
                                    64.0f, 32.0f);
}

} // namespace xpbd::gfx
