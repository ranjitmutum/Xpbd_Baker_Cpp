#pragma once

#include "xpbd/gfx/frame_stats.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"

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

enum class BackendKind {
  OpenGL33,
  Vulkan,
  Dx11,
  Metal,
};

struct UiDrawData {
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
  std::uint64_t static_model_generation = 0;
  std::uint64_t static_texture_generation = 0;
  const UiDrawData *ui = nullptr;
  FrameDiagnosticContext diagnostics;
  float clear_r = 0.176f;
  float clear_g = 0.176f;
  float clear_b = 0.176f;
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

// GPU 渲染后端抽象，供 Vulkan、Direct3D 与 OpenGL 实现共享调用方式。
class IGpuBackend {
public:
  virtual ~IGpuBackend() = default;
  virtual bool init(SDL_Window *window) = 0;
  virtual void shutdown() = 0;
  virtual void resize(int fb_w, int fb_h) = 0;
  virtual bool uploadFontAtlas(const void *pixels, int width, int height,
                               bool is_rgba = false) = 0;
  [[nodiscard]] virtual unsigned int fontTextureId() const = 0;

  [[nodiscard]] virtual bool supportsStaticModel() const { return false; }
  virtual void render(const FrameInput &frame) = 0;

  virtual void setVSync(bool enabled) { (void)enabled; }
  [[nodiscard]] virtual bool vsyncEnabled() const { return true; }
  [[nodiscard]] virtual BackendKind kind() const = 0;
  [[nodiscard]] virtual const char *name() const = 0;
  [[nodiscard]] virtual const char *deviceName() const = 0;
  [[nodiscard]] virtual FrameStats stats() const = 0;
};

std::unique_ptr<IGpuBackend> createOpenGLBackend();
std::unique_ptr<IGpuBackend> createVulkanBackend();
std::unique_ptr<IGpuBackend> createDx11Backend();
std::unique_ptr<IGpuBackend> createMetalBackend();

}
