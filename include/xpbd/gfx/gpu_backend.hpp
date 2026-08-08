#pragma once

#include "xpbd/gfx/frame_stats.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/path_trace_aov.hpp"
#include "xpbd/gfx/preview_scene.hpp"
#include "xpbd/gfx/ray_tracing.hpp"
#include "xpbd/gfx/rt_scene_generations.hpp"
#include "xpbd/gfx/still_image_export.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/gfx/world_environment.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

struct SDL_Window;
struct nk_context;
struct nk_buffer;
struct nk_draw_null_texture;

namespace xpbd::gfx {

struct VulkanWindowBootstrap;
struct LastPresentedSnapshot;

enum class BackendKind {
  OpenGL33,
  Vulkan,
};

enum class UiLogicalTexture : std::uint64_t {
  FontAtlas = 1u,
};

[[nodiscard]] inline constexpr std::uint64_t
uiLogicalTextureId(UiLogicalTexture texture) noexcept {
  return static_cast<std::uint64_t>(texture);
}

[[nodiscard]] inline constexpr bool
isKnownUiLogicalTextureId(std::uint64_t texture_id) noexcept {
  return texture_id == uiLogicalTextureId(UiLogicalTexture::FontAtlas);
}

struct UiDrawCommand {
  std::uint32_t element_count = 0;
  std::uint32_t index_offset = 0;
  std::uint64_t logical_texture_id = 0;
  float clip_x = 0.0f;
  float clip_y = 0.0f;
  float clip_w = 0.0f;
  float clip_h = 0.0f;
};

struct UiDrawData {
  // Owned/thread-safe representation. The producer owns these ranges for the
  // duration of a synchronous call; RenderFramePacket copies them before a
  // packet can cross threads. Texture IDs are logical IDs, never Vulkan
  // descriptors or image handles.
  const void *vertex_data = nullptr;
  std::size_t vertex_bytes = 0;
  const void *index_data = nullptr;
  std::size_t index_bytes = 0;
  const UiDrawCommand *draw_commands = nullptr;
  std::size_t draw_command_count = 0;

  // Transitional synchronous Nuklear view. The dedicated RenderThread never
  // stores or consumes these pointers.
  const nk_context *ctx = nullptr;
  const nk_buffer *cmds = nullptr;
  const nk_buffer *vertices = nullptr;
  const nk_buffer *indices = nullptr;
  int logical_w = 0;
  int logical_h = 0;
  int fb_w = 0;
  int fb_h = 0;
  bool overlay_visible = false;
  float overlay_x = 0.0f;
  float overlay_y = 0.0f;
  float overlay_w = 0.0f;
  float overlay_h = 0.0f;
};

struct ViewportRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// Converts Nuklear/SDL logical coordinates to the physical framebuffer. This
// is the single DPI boundary used by the renderer and keeps the offscreen
// preview extent independent from UI pixel density. Use shared half-open
// framebuffer endpoints so adjacent logical rectangles cannot disagree by one
// pixel at fractional DPI.
[[nodiscard]] inline int logicalEdgeToFramebuffer(
    float logical_edge, float scale, int framebuffer_limit) noexcept {
  const double safe_scale =
      std::isfinite(scale) && scale > 0.0f ? static_cast<double>(scale) : 1.0;
  const double safe_edge =
      std::isfinite(logical_edge) ? static_cast<double>(logical_edge) : 0.0;
  long long pixel = std::llround(safe_edge * safe_scale);
  if (framebuffer_limit > 0) {
    pixel = std::clamp<long long>(
        pixel, 0ll, static_cast<long long>(framebuffer_limit));
  }
  if (pixel < static_cast<long long>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  if (pixel > static_cast<long long>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(pixel);
}

[[nodiscard]] inline ViewportRect logicalRectToFramebufferHalfOpen(
    float x0, float y0, float x1, float y1, float scale_x, float scale_y,
    int framebuffer_width, int framebuffer_height) noexcept {
  const float safe_x0 = std::isfinite(x0) ? x0 : 0.0f;
  const float safe_y0 = std::isfinite(y0) ? y0 : 0.0f;
  const float safe_x1 = std::isfinite(x1) ? x1 : safe_x0;
  const float safe_y1 = std::isfinite(y1) ? y1 : safe_y0;
  const float ordered_x0 = (std::min)(safe_x0, safe_x1);
  const float ordered_y0 = (std::min)(safe_y0, safe_y1);
  const float ordered_x1 = (std::max)(safe_x0, safe_x1);
  const float ordered_y1 = (std::max)(safe_y0, safe_y1);

  ViewportRect result{};
  result.x = logicalEdgeToFramebuffer(ordered_x0, scale_x, framebuffer_width);
  result.y = logicalEdgeToFramebuffer(ordered_y0, scale_y, framebuffer_height);
  const int pixel_x1 =
      logicalEdgeToFramebuffer(ordered_x1, scale_x, framebuffer_width);
  const int pixel_y1 =
      logicalEdgeToFramebuffer(ordered_y1, scale_y, framebuffer_height);
  result.w = (std::max)(0, pixel_x1 - result.x);
  result.h = (std::max)(0, pixel_y1 - result.y);
  return result;
}

[[nodiscard]] inline ViewportRect logicalViewportToFramebuffer(
    float x, float y, float w, float h, float scale_x, float scale_y,
    int framebuffer_width, int framebuffer_height) noexcept {
  const ViewportRect half_open = logicalRectToFramebufferHalfOpen(
      x, y, x + (std::max)(w, 0.0f), y + (std::max)(h, 0.0f), scale_x,
      scale_y, framebuffer_width, framebuffer_height);
  ViewportRect result = half_open;
  result.w = (std::max)(1, result.w);
  result.h = (std::max)(1, result.h);
  if (framebuffer_width > 0) {
    result.x = std::clamp(result.x, 0, framebuffer_width - 1);
    result.w = std::clamp(result.w, 1, framebuffer_width - result.x);
  }
  if (framebuffer_height > 0) {
    result.y = std::clamp(result.y, 0, framebuffer_height - 1);
    result.h = std::clamp(result.h, 1, framebuffer_height - result.y);
  }
  return result;
}

struct PathTraceTargetExtent {
  std::uint32_t width = 1u;
  std::uint32_t height = 1u;

  [[nodiscard]] constexpr bool operator==(
      const PathTraceTargetExtent &) const noexcept = default;
};

// Splitter extents are transient. Reuse a complete existing target throughout
// the gesture, then accept the requested final extent on the first stable
// frame. A first-frame drag still gets a valid one-pixel-or-larger target.
[[nodiscard]] constexpr PathTraceTargetExtent choosePathTraceTargetExtent(
    std::uint32_t requested_width, std::uint32_t requested_height,
    std::uint32_t current_width, std::uint32_t current_height,
    bool interactive_resize) noexcept {
  PathTraceTargetExtent requested{
      std::max(1u, requested_width), std::max(1u, requested_height)};
  if (interactive_resize && current_width > 0u && current_height > 0u) {
    return {current_width, current_height};
  }
  return requested;
}

struct FrameDiagnosticContext {
  bool active = false;
  std::uint64_t render_frame = 0;
  std::uint64_t result_commit_frame = 0;
  std::uint32_t frames_remaining = 0;
  int worker_phase = 0;
  int presentation_mode = 0;
  int playback_state = 0;
  int preview_frame_index = 0;
  int bake_current = 0;
  int bake_total = 0;
  double preview_time = 0.0;
  std::uint64_t model_generation = 0;
  std::uint64_t animation_generation = 0;
  std::uint64_t physics_generation = 0;
  std::uint64_t texture_generation = 0;
};

enum class StillRenderJobState : std::uint8_t {
  Idle = 0,
  Queued,
  Rendering,
  Saving,
  Completed,
  Failed,
  Cancelled,
};

struct StillRenderStatus {
  StillRenderJobState state = StillRenderJobState::Idle;
  std::uint64_t job_id = 0;
  std::uint32_t accumulated_samples = 0;
  std::uint32_t target_samples = 0;
  std::string output_path;
  std::string error;
};

struct StillRenderFrameRequest {
  std::uint64_t job_id = 0;
  std::uint32_t width = 1920;
  std::uint32_t height = 1080;
  std::uint32_t target_samples = 1024;
  std::uint32_t samples_per_submit = 8;
  StillImageFormat format = StillImageFormat::Png;
  bool transparent_background = false;
  bool cancel_requested = false;
  std::string output_path;
  const float *view_matrix = nullptr;
  const float *proj_matrix = nullptr;
  PathTraceSettings path_trace_settings{};
  LabPbrDebugView material_debug_view = LabPbrDebugView::Shaded;
  RtDebugView rt_debug_view = RtDebugView::Off;
  const PreviewSkybox *preview_skybox = nullptr;
  StillRenderStatus *status = nullptr;
};

struct FrameInput {
  int fb_width = 0;
  int fb_height = 0;
  ViewportRect viewport;
  const float *view_matrix = nullptr;
  const float *proj_matrix = nullptr;
  const ViewportGpuScene *scene = nullptr;

  const StaticIndexedModelMesh *static_model = nullptr;
  const StaticModelFrameData *static_model_frame = nullptr;
  const TextureImage *static_model_texture = nullptr;
  const ResolvedMaterialTable *static_model_material = nullptr;
  std::uint64_t static_model_generation = 0;
  std::uint64_t static_texture_generation = 0;
  LabPbrDebugView material_debug_view = LabPbrDebugView::Shaded;
  RtDebugView rt_debug_view = RtDebugView::Off;
  RrAovDebugView rr_aov_debug_view = RrAovDebugView::Off;
  PathTraceSettings path_trace_settings{};
  const WorldEnvironmentState *world_environment = nullptr;
  const StillRenderFrameRequest *still_render = nullptr;
  const UiDrawData *ui = nullptr;
  FrameDiagnosticContext diagnostics;
  float clear_r = 0.176f;
  float clear_g = 0.176f;
  float clear_b = 0.176f;

  // Blockbench-style preview scene + forward raster path.
  const ViewportRasterScene *raster_scene = nullptr;

  // User preference for NVIDIA hardware ray tracing (default off in session).
  // Backend forces Raster when capability is missing or device RT is not armed.
  bool prefer_ray_tracing = false;

  // The preview splitter is being dragged. A temporal renderer should reuse
  // its current target and suspend frame interpolation until the final extent
  // is known instead of reallocating on every pointer event.
  bool interactive_viewport_resize = false;

  // Explicit RT invalidation ABI.  When false, a backend uses a conservative
  // per-frame serial (never a full vertex-buffer hash) for compatibility with
  // older callers.
  RtSceneGenerations rt_scene_generations{};
  bool rt_scene_generations_valid = false;
};

struct DynamicMeshUploadRange {
  std::size_t offset_bytes = 0;
  std::size_t size_bytes = 0;
};

struct DynamicMeshUploadLayout {
  DynamicMeshUploadRange solid;
  DynamicMeshUploadRange transparent;
  DynamicMeshUploadRange lines;
  std::size_t total_bytes = 0;
};

// CPU-only observability for the asynchronous renderer. These counters are
// deliberately not backend handles: they let unattended gates distinguish UI
// packet production from worker consumption and committed presentation.
struct RenderThreadDiagnostics {
  bool active = false;
  bool fatal = false;
  std::uint64_t latest_ui_frame_serial = 0;
  std::uint64_t latest_packet_serial = 0;
  std::uint64_t latest_render_serial = 0;
  std::uint64_t latest_previous_history_render_serial = 0;
  std::uint64_t latest_history_serial = 0;
  std::uint64_t latest_present_serial = 0;
  std::shared_ptr<const LastPresentedSnapshot> last_presented;
  std::uint64_t mailbox_replacement_count = 0;
  std::uint64_t accepted_world_environment_generation = 0;
  std::uint64_t accepted_hdri_runtime_generation = 0;
  bool environment_upload_pending = false;
};

[[nodiscard]] constexpr bool
useStaticModelViewport(bool backend_supports_static_model,
                       std::string_view legacy_override) noexcept {
  return backend_supports_static_model && legacy_override != "1";
}

[[nodiscard]] constexpr DynamicMeshUploadLayout
makeDynamicMeshUploadLayout(std::size_t solid_vertices,
                            std::size_t transparent_vertices,
                            std::size_t line_vertices) noexcept {
  DynamicMeshUploadLayout layout;
  layout.solid.size_bytes = solid_vertices * sizeof(MeshVertex);
  layout.transparent.offset_bytes = layout.solid.size_bytes;
  layout.transparent.size_bytes = transparent_vertices * sizeof(MeshVertex);
  layout.lines.offset_bytes =
      layout.transparent.offset_bytes + layout.transparent.size_bytes;
  layout.lines.size_bytes = line_vertices * sizeof(MeshVertex);
  layout.total_bytes = layout.lines.offset_bytes + layout.lines.size_bytes;
  return layout;
}

[[nodiscard]] constexpr bool
gpuTimestampSupported(std::uint32_t valid_bits,
                      double timestamp_period_ns) noexcept {
  return valid_bits > 0 && valid_bits <= 64 && timestamp_period_ns > 0.0 &&
         timestamp_period_ns <= (std::numeric_limits<double>::max)();
}

[[nodiscard]] constexpr std::uint64_t
gpuTimestampDeltaTicks(std::uint64_t begin, std::uint64_t end,
                       std::uint32_t valid_bits) noexcept {
  if (valid_bits == 0 || valid_bits > 64) {
    return 0;
  }
  if (valid_bits == 64) {
    return end - begin;
  }
  const std::uint64_t mask = (std::uint64_t{1} << valid_bits) - 1;
  return (end - begin) & mask;
}

[[nodiscard]] constexpr double
gpuTimestampDeltaMilliseconds(std::uint64_t begin, std::uint64_t end,
                              std::uint32_t valid_bits,
                              double timestamp_period_ns) noexcept {
  if (!gpuTimestampSupported(valid_bits, timestamp_period_ns)) {
    return 0.0;
  }
  return static_cast<double>(gpuTimestampDeltaTicks(begin, end, valid_bits)) *
         timestamp_period_ns / 1'000'000.0;
}

// Shared interface implemented by the retained OpenGL and Vulkan backends.
class IGpuBackend {
public:
  virtual ~IGpuBackend() = default;
  virtual bool init(SDL_Window *window) = 0;
  // Vulkan-only initialization contract. The SDL/Main thread owns bootstrap
  // capture; a dedicated RenderThread may consume the immutable result.
  virtual bool init(const VulkanWindowBootstrap &bootstrap) {
    (void)bootstrap;
    return false;
  }
  virtual void shutdown() = 0;
  virtual void resize(int fb_w, int fb_h) = 0;
  virtual bool uploadFontAtlas(const void *pixels, int width, int height,
                               bool is_rgba = false) = 0;
  // This is a producer-side logical ID. Vulkan descriptor/image handles stay
  // worker-owned and are resolved only while recording the draw command.
  [[nodiscard]] virtual unsigned int fontTextureId() const = 0;

  [[nodiscard]] virtual bool supportsStaticModel() const { return false; }
  virtual void render(const FrameInput &frame) = 0;

  // Frame-token and latency-marker boundary shared by Reflex, PCL, SR/RR,
  // and DLSS Frame Generation. Backends without Streamline ignore it.
  virtual void beginLatencyFrame(
      std::uint32_t frame_index, PathTraceReflexMode mode,
      bool frame_generation_requested) {
    (void)frame_index;
    (void)mode;
    (void)frame_generation_requested;
  }
  virtual void endLatencySimulation() {}
  // Streamline PCL periodically posts this Win32 message to measure input
  // sampling latency. The application message hook forwards it to the backend.
  [[nodiscard]] virtual std::uint32_t latencyPingMessage() const {
    return 0u;
  }
  virtual void markLatencyPing() {}

  virtual void setVSync(bool enabled) { (void)enabled; }
  [[nodiscard]] virtual bool vsyncEnabled() const { return true; }
  [[nodiscard]] virtual BackendKind kind() const = 0;
  [[nodiscard]] virtual const char *name() const = 0;
  [[nodiscard]] virtual const char *deviceName() const = 0;
  [[nodiscard]] virtual FrameStats stats() const = 0;
  // The dedicated Vulkan proxy exposes only an immutable CPU snapshot of the
  // last frame the worker actually presented. Backends without asynchronous
  // presentation authority return an empty snapshot.
  [[nodiscard]] virtual std::shared_ptr<const LastPresentedSnapshot>
  lastPresentedSnapshot() const {
    return {};
  }
  [[nodiscard]] virtual RenderThreadDiagnostics
  renderThreadDiagnostics() const {
    return {};
  }
  [[nodiscard]] virtual PathTracePostProcessCapabilities
  pathTracePostProcessCapabilities() const {
    return {};
  }
  [[nodiscard]] virtual std::string_view
  pathTracePostProcessStatus() const {
    return "No path-trace post-process runtime is active";
  }

  // Pause GPU work before a native modal dialog (file picker, etc.).
  // Must idle the device so the system dialog is not fighting in-flight RT.
  [[nodiscard]] virtual bool prepareForSystemDialog() { return true; }
  [[nodiscard]] virtual bool resumeAfterSystemDialog() { return true; }
  [[nodiscard]] virtual bool presentationSuspended() const { return false; }

  // NVIDIA RT capability (Vulkan RT extensions). Default: unsupported.
  [[nodiscard]] virtual RayTracingCapability rayTracingCapability() const {
    return {};
  }
  [[nodiscard]] virtual bool supportsRayTracing() const {
    return rayTracingCapability().supported;
  }
  // Active path used for the last rendered frame (Raster unless RT enabled).
  [[nodiscard]] virtual RenderPath activeRenderPath() const {
    return RenderPath::Raster;
  }
};

std::unique_ptr<IGpuBackend> createOpenGLBackend();
std::unique_ptr<IGpuBackend> createVulkanBackend();

} // namespace xpbd::gfx
