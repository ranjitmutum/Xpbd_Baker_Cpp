#pragma once

#include "xpbd/gfx/gpu_backend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace xpbd::gfx {

// CPU-only UI payload. It contains no Nuklear object and no backend handle.
struct OwnedUiDrawData {
  std::vector<std::byte> vertices;
  std::vector<std::byte> indices;
  std::vector<UiDrawCommand> commands;
  int logical_w = 0;
  int logical_h = 0;
  int fb_w = 0;
  int fb_h = 0;
  bool overlay_visible = false;
  float overlay_x = 0.0f;
  float overlay_y = 0.0f;
  float overlay_w = 0.0f;
  float overlay_h = 0.0f;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] UiDrawData view() const noexcept;
  [[nodiscard]] static OwnedUiDrawData copyOf(const UiDrawData *source);
};

// Immutable scene references shared by preview packets and Still commands.
// Large authored assets are copied only when their authoritative generation
// changes; dynamic pose/visibility data receives a packet-owned snapshot.
struct RenderSceneSnapshot {
  std::shared_ptr<const ViewportGpuScene> scene;
  std::shared_ptr<const StaticIndexedModelMesh> static_model;
  std::shared_ptr<const StaticModelFrameData> static_model_frame;
  std::shared_ptr<const TextureImage> static_model_texture;
  std::shared_ptr<const ResolvedMaterialTable> static_model_material;
  std::shared_ptr<const WorldEnvironmentState> world_environment;
  std::shared_ptr<const ViewportRasterScene> raster_scene;
  std::shared_ptr<const PreviewSkybox> preview_skybox;
};

// Complete CPU-only packet that may safely outlive the UI frame that produced
// it. No member points at stack storage or contains a Vulkan/Streamline handle.
struct RenderFramePacket {
  std::uint64_t ui_frame_serial = 0;
  std::uint64_t packet_serial = 0;
  int fb_width = 0;
  int fb_height = 0;
  ViewportRect viewport{};
  std::array<float, 16> view_matrix{};
  std::array<float, 16> proj_matrix{};
  RenderSceneSnapshot scene{};
  std::uint64_t static_model_generation = 0;
  std::uint64_t static_texture_generation = 0;
  LabPbrDebugView material_debug_view = LabPbrDebugView::Shaded;
  RtDebugView rt_debug_view = RtDebugView::Off;
  RrAovDebugView rr_aov_debug_view = RrAovDebugView::Off;
  PathTraceSettings path_trace_settings{};
  FrameDiagnosticContext diagnostics{};
  OwnedUiDrawData ui{};
  float clear_r = 0.176f;
  float clear_g = 0.176f;
  float clear_b = 0.176f;
  bool prefer_ray_tracing = false;
  bool interactive_viewport_resize = false;
  RtSceneGenerations rt_scene_generations{};
  bool rt_scene_generations_valid = false;
};

// A non-owning view is constructed only on the consumer stack. It cannot be
// moved or copied because FrameInput points into this view and its packet.
class RenderFramePacketView final {
public:
  explicit RenderFramePacketView(const RenderFramePacket &packet) noexcept;
  RenderFramePacketView(const RenderFramePacketView &) = delete;
  RenderFramePacketView &operator=(const RenderFramePacketView &) = delete;
  RenderFramePacketView(RenderFramePacketView &&) = delete;
  RenderFramePacketView &operator=(RenderFramePacketView &&) = delete;

  [[nodiscard]] const FrameInput &frame() const noexcept { return frame_; }

private:
  UiDrawData ui_{};
  FrameInput frame_{};
};

class RenderFramePacketBuilder final {
public:
  [[nodiscard]] std::shared_ptr<const RenderFramePacket>
  build(std::uint64_t ui_frame_serial, const FrameInput &source);

  [[nodiscard]] std::uint64_t lastPacketSerial() const noexcept {
    return next_packet_serial_ - 1u;
  }

private:
  std::uint64_t next_packet_serial_ = 1u;

  const ViewportGpuScene *cached_scene_source_ = nullptr;
  std::uint64_t cached_scene_generation_ = 0u;
  std::shared_ptr<const ViewportGpuScene> cached_scene_;

  const StaticIndexedModelMesh *cached_static_model_source_ = nullptr;
  std::uint64_t cached_static_model_generation_ = 0u;
  std::shared_ptr<const StaticIndexedModelMesh> cached_static_model_;

  const TextureImage *cached_texture_source_ = nullptr;
  std::uint64_t cached_texture_generation_ = 0u;
  std::shared_ptr<const TextureImage> cached_texture_;

  const ResolvedMaterialTable *cached_material_source_ = nullptr;
  std::uint64_t cached_material_generation_ = 0u;
  std::shared_ptr<const ResolvedMaterialTable> cached_material_;

  const WorldEnvironmentState *cached_world_source_ = nullptr;
  std::uint64_t cached_world_generation_ = 0u;
  std::shared_ptr<const WorldEnvironmentState> cached_world_;

  const ViewportRasterScene *cached_raster_source_ = nullptr;
  std::uint64_t cached_raster_generation_ = 0u;
  std::shared_ptr<const ViewportRasterScene> cached_raster_;
};

// Reliable StartStillRender payload. It captures the complete scene at enqueue
// time and deliberately has no mutable status pointer or dependency on a later
// latest-frame packet.
struct StartStillRenderCommand {
  std::uint64_t job_id = 0;
  std::uint32_t width = 1920;
  std::uint32_t height = 1080;
  std::uint32_t target_samples = 1024;
  std::uint32_t samples_per_submit = 8;
  StillImageFormat format = StillImageFormat::Png;
  bool transparent_background = false;
  std::string output_path;
  std::array<float, 16> view_matrix{};
  std::array<float, 16> proj_matrix{};
  ViewportRect viewport{};
  PathTraceSettings path_trace_settings{};
  LabPbrDebugView material_debug_view = LabPbrDebugView::Shaded;
  RtDebugView rt_debug_view = RtDebugView::Off;
  float clear_r = 0.176f;
  float clear_g = 0.176f;
  float clear_b = 0.176f;
  RenderSceneSnapshot scene{};
  std::uint64_t source_packet_serial = 0;
  std::uint64_t static_model_generation = 0;
  std::uint64_t static_texture_generation = 0;
  RtSceneGenerations rt_scene_generations{};
  bool rt_scene_generations_valid = false;

  [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] std::optional<StartStillRenderCommand>
makeStartStillRenderCommand(const RenderFramePacket &packet,
                            const StillRenderFrameRequest &request);

struct HistoryCommitSnapshot {
  std::uint64_t history_serial = 0;
  std::uint64_t render_serial = 0;
  std::uint64_t packet_serial = 0;
  std::uint64_t ui_frame_serial = 0;
  std::array<float, 16> view_matrix{};
  std::array<float, 16> proj_matrix{};
  ViewportRect viewport{};
  std::array<float, 2> jitter{};
  std::shared_ptr<const StaticIndexedModelMesh> static_model;
  std::shared_ptr<const StaticModelFrameData> pose;
  std::shared_ptr<const ViewportRasterScene> visibility_and_instances;
  RtSceneGenerations rt_scene_generations{};
  bool rt_scene_generations_valid = false;
};

[[nodiscard]] HistoryCommitSnapshot
makeHistoryCommitCandidate(const RenderFramePacket &packet,
                           std::uint64_t render_serial) noexcept;

// The ledger advances only when the RenderThread explicitly accepts a
// submitted temporal frame. Merely producing, overwriting, or consuming a
// packet cannot change history authority.
class TemporalHistoryLedger final {
public:
  [[nodiscard]] bool commit(HistoryCommitSnapshot candidate) noexcept;
  void invalidate() noexcept { current_.reset(); }

  [[nodiscard]] std::uint64_t historySerial() const noexcept {
    return next_history_serial_ - 1u;
  }
  [[nodiscard]] const std::optional<HistoryCommitSnapshot> &current() const
      noexcept {
    return current_;
  }

private:
  std::uint64_t next_history_serial_ = 1u;
  std::uint64_t last_render_serial_ = 0u;
  std::optional<HistoryCommitSnapshot> current_;
};

struct LastPresentedSnapshot {
  std::uint64_t present_serial = 0;
  std::uint64_t previous_history_render_serial = 0;
  HistoryCommitSnapshot history{};
};

// Reprojects the exact worker-owned geometry/pose/camera that reached the
// swapchain. Coordinates in the resulting index are local to history.viewport
// and use its framebuffer-pixel extent.
[[nodiscard]] render::BonePickIndex
buildLastPresentedBonePickIndex(const LastPresentedSnapshot &snapshot);

} // namespace xpbd::gfx
