#include "xpbd/app/app_session.hpp"
#include "xpbd/app/i18n.hpp"
#include "xpbd/app/nuklear_ui.hpp"
#include "xpbd/gfx/backend_select.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/log.hpp"

#include <charconv>
#include <cstdio>
#include <system_error>


#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_BOOL




static char *xpbdNkDtoa(char *dst, double value) {
  constexpr int kBufferSize = 64;
  auto write_shortest_fixed = [&](auto number) {
    const auto converted = std::to_chars(dst, dst + kBufferSize - 1, number,
                                         std::chars_format::fixed);
    if (converted.ec != std::errc{}) {
      return false;
    }
    *converted.ptr = '\0';
    if (dst[0] == '-' && dst[1] == '0' && dst[2] == '\0') {
      dst[0] = '0';
      dst[1] = '\0';
    }
    return true;
  };

  const float narrowed = static_cast<float>(value);
  if (static_cast<double>(narrowed) == value &&
      write_shortest_fixed(narrowed)) {
    return dst;
  }
  if (write_shortest_fixed(value)) {
    return dst;
  }



  int length = std::snprintf(dst, kBufferSize, "%.8f", value);
  if (length <= 0) {
    dst[0] = '0';
    dst[1] = '\0';
    return dst;
  }
  if (length >= kBufferSize) {
    length = kBufferSize - 1;
  }
  int end = length - 1;
  while (end > 0 && dst[end] == '0') {
    --end;
  }
  if (end > 0 && dst[end] == '.') {
    --end;
  }
  dst[end + 1] = '\0';
  if (dst[0] == '-' && dst[1] == '0' && dst[2] == '\0') {
    dst[0] = '0';
    dst[1] = '\0';
  }
  return dst;
}

#define NK_DTOA xpbdNkDtoa
#define NK_IMPLEMENTATION
#include "nuklear.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {


struct NkVertex {
  float position[2];
  float uv[2];
  nk_byte col[4];
};
static_assert(sizeof(NkVertex) == 20, "NkVertex packing");

void nkHandleSdlEvent(nk_context *ctx, const SDL_Event &ev) {
  switch (ev.type) {
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP: {
    const int down = ev.type == SDL_EVENT_KEY_DOWN;
    const SDL_Keycode key = ev.key.key;
    if (key == SDLK_RSHIFT || key == SDLK_LSHIFT) {
      nk_input_key(ctx, NK_KEY_SHIFT, down);
    } else if (key == SDLK_DELETE) {
      nk_input_key(ctx, NK_KEY_DEL, down);
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
      nk_input_key(ctx, NK_KEY_ENTER, down);
    } else if (key == SDLK_TAB) {
      nk_input_key(ctx, NK_KEY_TAB, down);
    } else if (key == SDLK_BACKSPACE) {
      nk_input_key(ctx, NK_KEY_BACKSPACE, down);
    } else if (key == SDLK_HOME) {
      nk_input_key(ctx, NK_KEY_TEXT_START, down);
      nk_input_key(ctx, NK_KEY_SCROLL_START, down);
    } else if (key == SDLK_END) {
      nk_input_key(ctx, NK_KEY_TEXT_END, down);
      nk_input_key(ctx, NK_KEY_SCROLL_END, down);
    } else if (key == SDLK_PAGEDOWN) {
      nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down);
    } else if (key == SDLK_PAGEUP) {
      nk_input_key(ctx, NK_KEY_SCROLL_UP, down);
    } else if (key == SDLK_Z && (ev.key.mod & SDL_KMOD_CTRL)) {
      nk_input_key(ctx, NK_KEY_TEXT_UNDO, down);
    } else if (key == SDLK_R && (ev.key.mod & SDL_KMOD_CTRL)) {
      nk_input_key(ctx, NK_KEY_TEXT_REDO, down);
    } else if (key == SDLK_C && (ev.key.mod & SDL_KMOD_CTRL)) {
      nk_input_key(ctx, NK_KEY_COPY, down);
    } else if (key == SDLK_V && (ev.key.mod & SDL_KMOD_CTRL)) {
      nk_input_key(ctx, NK_KEY_PASTE, down);
    } else if (key == SDLK_X && (ev.key.mod & SDL_KMOD_CTRL)) {
      nk_input_key(ctx, NK_KEY_CUT, down);
    } else if (key == SDLK_B && (ev.key.mod & SDL_KMOD_CTRL)) {
      nk_input_key(ctx, NK_KEY_TEXT_LINE_START, down);
    } else if (key == SDLK_E && (ev.key.mod & SDL_KMOD_CTRL)) {
      nk_input_key(ctx, NK_KEY_TEXT_LINE_END, down);
    } else if (key == SDLK_LEFT) {
      nk_input_key(ctx, NK_KEY_LEFT, down);
    } else if (key == SDLK_RIGHT) {
      nk_input_key(ctx, NK_KEY_RIGHT, down);
    } else if (key == SDLK_UP) {
      nk_input_key(ctx, NK_KEY_UP, down);
    } else if (key == SDLK_DOWN) {
      nk_input_key(ctx, NK_KEY_DOWN, down);
    }
    break;
  }
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    const int down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    const float x = ev.button.x;
    const float y = ev.button.y;
    if (ev.button.button == SDL_BUTTON_LEFT) {
      nk_input_button(ctx, NK_BUTTON_LEFT, static_cast<int>(x),
                      static_cast<int>(y), down);
    } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
      nk_input_button(ctx, NK_BUTTON_MIDDLE, static_cast<int>(x),
                      static_cast<int>(y), down);
    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
      nk_input_button(ctx, NK_BUTTON_RIGHT, static_cast<int>(x),
                      static_cast<int>(y), down);
    }
    break;
  }
  case SDL_EVENT_MOUSE_MOTION:
    nk_input_motion(ctx, static_cast<int>(ev.motion.x),
                    static_cast<int>(ev.motion.y));
    break;
  case SDL_EVENT_MOUSE_WHEEL:
    nk_input_scroll(ctx, nk_vec2(ev.wheel.x, ev.wheel.y));
    break;
  case SDL_EVENT_TEXT_INPUT:
    nk_input_glyph(ctx, ev.text.text);
    break;
  default:
    break;
  }
}

struct ViewportPointerGesture {
  bool tracking = false;
  float down_x = 0.0f;
  float down_y = 0.0f;
  bool moved = false;
};

struct PendingViewportClick {
  bool ready = false;
  Uint8 button = 0;
  float x = 0.0f;
  float y = 0.0f;
};

bool pointInsideViewport(const xpbd::app::UiLayout &layout, float x,
                         float y) {
  const bool inside_overlay =
      layout.overlay_visible && layout.overlay_w > 0.0f &&
      layout.overlay_h > 0.0f && x >= layout.overlay_x &&
      y >= layout.overlay_y && x <= layout.overlay_x + layout.overlay_w &&
      y <= layout.overlay_y + layout.overlay_h;
  return !inside_overlay && layout.vp_w > 1.0f && layout.vp_h > 1.0f &&
         x >= layout.vp_x && y >= layout.vp_y &&
         x <= layout.vp_x + layout.vp_w &&
         y <= layout.vp_y + layout.vp_h;
}

}

int app_main(int argc, char **argv) {
  SDL_SetMainReady();

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR, "XPBD Bone Baker",
        SDL_GetError() ? SDL_GetError() : "SDL_Init failed", nullptr);
    return 1;
  }



  auto backend_req = xpbd::gfx::parseBackendRequest(argc, argv);
  if (!backend_req.parse_error.empty()) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "XPBD Bone Baker",
                             backend_req.parse_error.c_str(), nullptr);
    SDL_Quit();
    return 1;
  }

  {
    std::string log_path = "xpbd_baker.log";
    if (const char *base = SDL_GetBasePath()) {
      // SDL3 owns and releases this cached const path during SDL_Quit().
      log_path = std::string(base) + "xpbd_baker.log";
    }
    xpbd::log::init(log_path);
  }

  constexpr int kWinW = 1280;
  constexpr int kWinH = 800;
  const char *kTitle = "XPBD Bone Baker - Bedrock Physics Baking Tool";

  SDL_Window *window = nullptr;
  std::unique_ptr<xpbd::gfx::IGpuBackend> backend;
  std::string backend_err;
  if (!xpbd::gfx::createWindowAndBackend(backend_req, kTitle, kWinW, kWinH,
                                         window, backend, backend_err)) {
    const std::string msg = backend_err.empty()
                                ? std::string("GPU backend init failed")
                                : backend_err;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "XPBD Bone Baker",
                             msg.c_str(), window);
    if (window) {
      SDL_DestroyWindow(window);
    }
    xpbd::log::errorf("GPU backend init failed: %s", msg.c_str());
    xpbd::log::shutdown();
    SDL_Quit();
    return 1;
  }
  xpbd::log::infof("Active backend: %s (%s) pref=%s force=%d",
                   backend->name(), backend->deviceName(),
                   xpbd::gfx::preferenceName(backend_req.pref),
                   backend_req.force ? 1 : 0);
  const char *legacy_uv_env = std::getenv("XPBD_VULKAN_LEGACY_UV");
  const bool use_static_model = xpbd::gfx::useStaticModelViewport(
      backend->supportsStaticModel(),
      legacy_uv_env != nullptr ? legacy_uv_env : "");
  xpbd::log::infof("Viewport model path: %s (static-capable=%d, "
                   "XPBD_VULKAN_LEGACY_UV=%s)",
                   use_static_model ? "Vulkan static indexed UV"
                                    : "legacy dynamic mesh",
                   backend->supportsStaticModel() ? 1 : 0,
                   legacy_uv_env != nullptr ? legacy_uv_env : "unset");
  SDL_Cursor *default_cursor = SDL_GetDefaultCursor();
  SDL_Cursor *horizontal_resize_cursor =
      default_cursor != nullptr
          ? SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE)
          : nullptr;
  if (default_cursor != nullptr && horizontal_resize_cursor == nullptr) {
    const char *error = SDL_GetError();
    xpbd::log::warnf("SDL resize cursor unavailable: %s",
                     error != nullptr ? error : "unknown error");
  }
  if (!SDL_StartTextInput(window)) {
    const char *error = SDL_GetError();
    xpbd::log::warnf("SDL text input unavailable: %s",
                     error != nullptr ? error : "unknown error");
  }

  xpbd::app::initI18n();

  nk_context ctx;
  nk_init_default(&ctx, nullptr);
  xpbd::app::applyDarkStyle(&ctx, 1.0f);



  nk_font_atlas atlas;
  nk_font_atlas_init_default(&atlas);
  nk_font_atlas_begin(&atlas);

  struct nk_font_config cfg = nk_font_config(17.0f);






  static const nk_rune ui_glyph_ranges[] = {
      0x0020, 0x00FF,
      0x2000, 0x22FF,
      0x3000, 0x30FF,
      0x31F0, 0x31FF,
      0x4E00, 0x9FAF,
      0xFF00, 0xFFEF,
      0};
  cfg.range = ui_glyph_ranges;
  cfg.oversample_h = 1;
  cfg.oversample_v = 1;
  cfg.pixel_snap = nk_true;

  nk_font *font = nullptr;

  const char *font_paths[] = {
      "C:\\Windows\\Fonts\\simhei.ttf",
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\simsun.ttc",
      "C:\\Windows\\Fonts\\segoeui.ttf",
  };
  for (const char *path : font_paths) {
    font = nk_font_atlas_add_from_file(&atlas, path, 17.0f, &cfg);
    if (font) {
      xpbd::log::infof("UI font loaded: %s", path);
      break;
    }
  }
  if (!font) {
    font = nk_font_atlas_add_default(&atlas, 16.0f, nullptr);
    xpbd::log::warn("UI font: default Proggy (no CJK)");
  }

  int atlas_w = 0, atlas_h = 0;

  const void *atlas_pixels =
      nk_font_atlas_bake(&atlas, &atlas_w, &atlas_h, NK_FONT_ATLAS_ALPHA8);
  backend->uploadFontAtlas(atlas_pixels, atlas_w, atlas_h);
  struct nk_draw_null_texture null_tex{};
  nk_font_atlas_end(&atlas,
                    nk_handle_id(static_cast<int>(backend->fontTextureId())),
                    &null_tex);
  nk_style_set_font(&ctx, &font->handle);



  nk_buffer cmds;
  nk_buffer_init_default(&cmds);
  constexpr int kMaxVerts = 512 * 1024;
  constexpr int kMaxIdx = 128 * 1024;
  std::vector<NkVertex> vert_slab(static_cast<size_t>(kMaxVerts));
  std::vector<nk_draw_index> idx_slab(static_cast<size_t>(kMaxIdx));

  static const nk_draw_vertex_layout_element vertex_layout[] = {
      {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(NkVertex, position)},
      {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(NkVertex, uv)},
      {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(NkVertex, col)},
      {NK_VERTEX_LAYOUT_END}};

  xpbd::gfx::ViewportGpuScene scene;
  xpbd::gfx::ViewportMeshBuilder static_mesh_builder;
  xpbd::gfx::StaticIndexedModelMesh static_model;
  xpbd::gfx::StaticModelFrameData static_model_frame;
  xpbd::gfx::StaticModelGenerationCache static_cpu_generations;
  xpbd::gfx::FrameStats frame_stats;
  float ema_ms = 16.0f;


  float disp_fps = 60.0f;
  float disp_frame_ms = 16.0f;
  float disp_ema_ms = 16.0f;
  float disp_mesh_ms = 0.0f;
  float disp_upload_ms = 0.0f;
  float disp_pick_ms = 0.0f;
  float disp_pick_queries = 0.0f;
  float disp_pick_cache_rebuilds = 0.0f;
  std::uint32_t disp_pick_candidate_faces = 0;
  std::uint32_t disp_pick_total_faces = 0;
  double disp_upload_bytes = 0.0;
  double disp_static_bone_upload_bytes = 0.0;
  double disp_static_resource_upload_bytes = 0.0;
  double disp_buffer_reallocations = 0.0;
  std::uint64_t disp_static_resource_rebuilds = 0;
  std::uint64_t disp_total_buffer_reallocations = 0;
  std::uint64_t disp_static_model_vertex_bytes = 0;
  std::uint64_t disp_static_model_index_bytes = 0;
  std::uint32_t disp_static_opaque_index_count = 0;
  std::uint32_t disp_static_cutout_index_count = 0;
  std::uint32_t disp_static_blend_index_count = 0;
  float disp_backend_cpu_ms = 0.0f;
  bool disp_gpu_timestamp_valid = false;
  float disp_gpu_timestamp_total_ms = 0.0f;
  float disp_gpu_timestamp_ui_ms = 0.0f;
  float disp_gpu_timestamp_opaque_ms = 0.0f;
  float disp_gpu_timestamp_transparent_ms = 0.0f;
  float disp_gpu_timestamp_lines_ms = 0.0f;
  int disp_cubes = 0;
  double accum_frame_ms = 0.0;
  double accum_mesh_ms = 0.0;
  double accum_upload_ms = 0.0;
  double accum_pick_ms = 0.0;
  std::uint64_t accum_pick_queries = 0;
  std::uint64_t accum_pick_cache_rebuilds = 0;
  std::uint64_t accum_pick_candidate_faces = 0;
  std::uint32_t accum_pick_total_faces = 0;
  double accum_upload_bytes = 0.0;
  double accum_static_bone_upload_bytes = 0.0;
  double accum_static_resource_upload_bytes = 0.0;
  double accum_buffer_reallocations = 0.0;
  double accum_backend_cpu_ms = 0.0;
  double accum_gpu_timestamp_total_ms = 0.0;
  double accum_gpu_timestamp_ui_ms = 0.0;
  double accum_gpu_timestamp_opaque_ms = 0.0;
  double accum_gpu_timestamp_transparent_ms = 0.0;
  double accum_gpu_timestamp_lines_ms = 0.0;
  int accum_gpu_timestamp_frames = 0;
  int accum_frames = 0;
  auto sample_window_start = std::chrono::steady_clock::now();

  bool running = true;
  auto last = std::chrono::steady_clock::now();
  auto &session = xpbd::app::AppSession::instance();
  std::uint64_t render_frame_number = 0;
  std::uint64_t result_commit_frame_number = 0;
  std::uint32_t completion_diagnostic_frames = 0;
  auto previous_bake_state = session.bake_state;
  bool hover_pick_snapshot_valid = false;
  float hover_pick_mouse_x = 0.0f;
  float hover_pick_mouse_y = 0.0f;
  float hover_pick_viewport_x = 0.0f;
  float hover_pick_viewport_y = 0.0f;
  float hover_pick_viewport_w = 0.0f;
  float hover_pick_viewport_h = 0.0f;
  std::uint64_t hover_pick_scene_token = 0;
  auto hover_pick_last_query = std::chrono::steady_clock::time_point{};

  while (running) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    const float frame_ms = dt * 1000.0f;
    if (frame_ms > 0.0f && frame_ms < 1000.0f) {
      constexpr float kAlpha = 0.12f;
      ema_ms = ema_ms * (1.0f - kAlpha) + frame_ms * kAlpha;
    }

    frame_stats.frame_ms = frame_ms;
    frame_stats.ema_frame_ms = ema_ms;
    frame_stats.fps = ema_ms > 0.01f ? 1000.0f / ema_ms : 0.0f;

    if (session.vsync_dirty) {
      backend->setVSync(session.vsync_enabled);
      session.vsync_dirty = false;
    }


    session.debug_fps = disp_fps;
    session.debug_frame_ms = disp_frame_ms;
    session.debug_ema_frame_ms = disp_ema_ms;
    session.debug_mesh_ms = disp_mesh_ms;
    session.debug_upload_ms = disp_upload_ms;
    session.debug_pick_ms = disp_pick_ms;
    session.debug_pick_queries = disp_pick_queries;
    session.debug_pick_cache_rebuilds = disp_pick_cache_rebuilds;
    session.debug_pick_candidate_faces = disp_pick_candidate_faces;
    session.debug_pick_total_faces = disp_pick_total_faces;
    session.debug_upload_bytes = disp_upload_bytes;
    session.debug_static_bone_upload_bytes = disp_static_bone_upload_bytes;
    session.debug_static_resource_upload_bytes =
        disp_static_resource_upload_bytes;
    session.debug_buffer_reallocations = disp_buffer_reallocations;
    session.debug_static_resource_rebuilds = disp_static_resource_rebuilds;
    session.debug_total_buffer_reallocations = disp_total_buffer_reallocations;
    session.debug_static_model_vertex_bytes = disp_static_model_vertex_bytes;
    session.debug_static_model_index_bytes = disp_static_model_index_bytes;
    session.debug_static_opaque_index_count = disp_static_opaque_index_count;
    session.debug_static_cutout_index_count = disp_static_cutout_index_count;
    session.debug_static_blend_index_count = disp_static_blend_index_count;
    session.debug_backend_cpu_ms = disp_backend_cpu_ms;
    session.debug_gpu_timestamp_valid = disp_gpu_timestamp_valid;
    session.debug_gpu_timestamp_total_ms = disp_gpu_timestamp_total_ms;
    session.debug_gpu_timestamp_ui_ms = disp_gpu_timestamp_ui_ms;
    session.debug_gpu_timestamp_opaque_ms = disp_gpu_timestamp_opaque_ms;
    session.debug_gpu_timestamp_transparent_ms =
        disp_gpu_timestamp_transparent_ms;
    session.debug_gpu_timestamp_lines_ms = disp_gpu_timestamp_lines_ms;
    session.debug_gpu_ms =
        disp_gpu_timestamp_valid ? disp_gpu_timestamp_total_ms : 0.0f;
    session.debug_cube_count = disp_cubes;
    session.debug_frame_count++;
    frame_stats.frame_ms = disp_frame_ms;
    frame_stats.ema_frame_ms = disp_ema_ms;
    frame_stats.fps = disp_fps;
    frame_stats.mesh_ms = disp_mesh_ms;
    frame_stats.upload_ms = disp_upload_ms;
    frame_stats.pick_ms = disp_pick_ms;
    frame_stats.pick_queries =
        static_cast<std::uint32_t>(std::max(0.0f, disp_pick_queries));
    frame_stats.pick_cache_rebuilds = static_cast<std::uint32_t>(
        std::max(0.0f, disp_pick_cache_rebuilds));
    frame_stats.pick_candidate_faces = disp_pick_candidate_faces;
    frame_stats.pick_total_faces = disp_pick_total_faces;
    frame_stats.backend_cpu_ms = disp_backend_cpu_ms;
    frame_stats.gpu_timestamp_valid = disp_gpu_timestamp_valid;
    frame_stats.gpu_timestamp_total_ms = disp_gpu_timestamp_total_ms;
    frame_stats.gpu_timestamp_ui_ms = disp_gpu_timestamp_ui_ms;
    frame_stats.gpu_timestamp_opaque_ms = disp_gpu_timestamp_opaque_ms;
    frame_stats.gpu_timestamp_transparent_ms =
        disp_gpu_timestamp_transparent_ms;
    frame_stats.gpu_timestamp_lines_ms = disp_gpu_timestamp_lines_ms;
    frame_stats.gpu_ms =
        disp_gpu_timestamp_valid ? disp_gpu_timestamp_total_ms : 0.0f;
    frame_stats.cube_count = disp_cubes;

    static xpbd::app::UiLayout prev_layout{};
    static ViewportPointerGesture left_viewport_gesture{};
    static ViewportPointerGesture right_viewport_gesture{};
    PendingViewportClick pending_viewport_click{};
    float wheel_y = 0.0f;
    bool hover_pick_force_refresh = false;

    SDL_Event ev;
    nk_input_begin(&ctx);
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT) {
        running = false;
      }
      if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) {
        session.closeBoneContext();
      }
      if (ev.type == SDL_EVENT_WINDOW_RESIZED ||
          ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        backend->resize(pw, ph);
      }
      if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
        wheel_y += ev.wheel.y;
        hover_pick_force_refresh = true;
      }
      if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
          (ev.button.button == SDL_BUTTON_LEFT ||
           ev.button.button == SDL_BUTTON_RIGHT)) {
        auto &gesture = ev.button.button == SDL_BUTTON_LEFT
                            ? left_viewport_gesture
                            : right_viewport_gesture;
        gesture.tracking =
            pointInsideViewport(prev_layout, ev.button.x, ev.button.y);
        gesture.down_x = ev.button.x;
        gesture.down_y = ev.button.y;
        gesture.moved = false;
      }
      if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        const auto update_gesture = [&](ViewportPointerGesture &gesture) {
          if (!gesture.tracking) {
            return;
          }
          const float dx = ev.motion.x - gesture.down_x;
          const float dy = ev.motion.y - gesture.down_y;
          constexpr float kClickMovement = 4.0f;
          if (dx * dx + dy * dy > kClickMovement * kClickMovement) {
            gesture.moved = true;
          }
        };
        update_gesture(left_viewport_gesture);
        update_gesture(right_viewport_gesture);
      }
      if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP &&
          (ev.button.button == SDL_BUTTON_LEFT ||
           ev.button.button == SDL_BUTTON_RIGHT)) {
        hover_pick_force_refresh = true;
        auto &gesture = ev.button.button == SDL_BUTTON_LEFT
                            ? left_viewport_gesture
                            : right_viewport_gesture;
        if (gesture.tracking && !gesture.moved &&
            pointInsideViewport(prev_layout, ev.button.x, ev.button.y)) {
          pending_viewport_click.ready = true;
          pending_viewport_click.button = ev.button.button;
          pending_viewport_click.x = ev.button.x;
          pending_viewport_click.y = ev.button.y;
        }
        gesture.tracking = false;
      }
      nkHandleSdlEvent(&ctx, ev);
    }

    float mx = 0, my = 0;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    static float last_mx = mx, last_my = my;
    const float dmx = mx - last_mx;
    const float dmy = my - last_my;
    last_mx = mx;
    last_my = my;



    if (prev_layout.viewport_hovered &&
        pointInsideViewport(prev_layout, mx, my)) {
      const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
      const bool pan_btn =
          (buttons & SDL_BUTTON_RMASK) != 0 ||
          (session.viewport_right_drag && (buttons & SDL_BUTTON_LMASK) != 0);
      const bool height_btn =
          (buttons & SDL_BUTTON_MMASK) != 0 || (pan_btn && shift);
      if (height_btn) {

        session.camera.panHeight(-dmy);
        if (!shift && (buttons & SDL_BUTTON_MMASK)) {


        }
      } else if (pan_btn) {

        session.camera.pan(dmx, -dmy);
      } else if (buttons & SDL_BUTTON_LMASK) {
        session.camera.orbit(dmx, dmy);
      }
      if (wheel_y != 0.0f) {
        if (shift) {
          session.camera.panHeight(wheel_y * 18.0f);
        } else {
          session.camera.zoom(wheel_y);
        }
      }
    }
    nk_input_end(&ctx);

    frame_stats.pick_ms = 0.0f;
    frame_stats.pick_queries = 0;
    frame_stats.pick_cache_rebuilds = 0;
    frame_stats.pick_candidate_faces = 0;
    frame_stats.pick_total_faces = 0;
    const auto timedPickBone = [&](float local_x, float local_y, float view_w,
                                   float view_h) {
      const auto pick_start = std::chrono::steady_clock::now();
      std::string bone =
          session.pickBoneAt(local_x, local_y, view_w, view_h);
      const auto pick_end = std::chrono::steady_clock::now();
      frame_stats.pick_ms +=
          std::chrono::duration<float, std::milli>(pick_end - pick_start)
              .count();
      ++frame_stats.pick_queries;
      const auto &diagnostics = session.lastViewportPickDiagnostics();
      if (diagnostics.cache_rebuilt) {
        ++frame_stats.pick_cache_rebuilds;
      }
      frame_stats.pick_candidate_faces += diagnostics.candidate_face_count;
      frame_stats.pick_total_faces =
          std::max(frame_stats.pick_total_faces,
                   diagnostics.total_face_count);
      return bone;
    };

    // Blockbench 风格的悬停拾取：光标下的组实时高亮，点击所见即所得。
    {
      const bool any_button_down =
          (buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK |
                      SDL_BUTTON_MMASK)) != 0;
      if (prev_layout.viewport_hovered && !any_button_down &&
          pointInsideViewport(prev_layout, mx, my)) {
        const std::uint64_t scene_token = session.viewportPickStateToken(
            prev_layout.vp_w, prev_layout.vp_h);
        const bool pointer_or_layout_changed =
            !hover_pick_snapshot_valid || mx != hover_pick_mouse_x ||
            my != hover_pick_mouse_y ||
            prev_layout.vp_x != hover_pick_viewport_x ||
            prev_layout.vp_y != hover_pick_viewport_y ||
            prev_layout.vp_w != hover_pick_viewport_w ||
            prev_layout.vp_h != hover_pick_viewport_h;
        const bool scene_changed =
            !hover_pick_snapshot_valid ||
            scene_token != hover_pick_scene_token;
        constexpr auto kSceneOnlyPickInterval =
            std::chrono::milliseconds(50);
        const bool scene_refresh_due =
            scene_changed &&
            now - hover_pick_last_query >= kSceneOnlyPickInterval;
        if (pointer_or_layout_changed || hover_pick_force_refresh ||
            scene_refresh_due) {
          session.hovered_bone_name =
              timedPickBone(mx - prev_layout.vp_x, my - prev_layout.vp_y,
                            prev_layout.vp_w, prev_layout.vp_h);
          hover_pick_last_query = now;
          hover_pick_snapshot_valid = true;
          hover_pick_mouse_x = mx;
          hover_pick_mouse_y = my;
          hover_pick_viewport_x = prev_layout.vp_x;
          hover_pick_viewport_y = prev_layout.vp_y;
          hover_pick_viewport_w = prev_layout.vp_w;
          hover_pick_viewport_h = prev_layout.vp_h;
          hover_pick_scene_token = session.viewportPickStateToken(
              prev_layout.vp_w, prev_layout.vp_h);
        }
      } else if (!prev_layout.viewport_hovered ||
                 !pointInsideViewport(prev_layout, mx, my)) {
        session.hovered_bone_name.clear();
        hover_pick_snapshot_valid = false;
      }
    }

    if (pending_viewport_click.ready) {
      const float local_x = pending_viewport_click.x - prev_layout.vp_x;
      const float local_y = pending_viewport_click.y - prev_layout.vp_y;
      const std::string bone = timedPickBone(
          local_x, local_y, prev_layout.vp_w, prev_layout.vp_h);
      if (!bone.empty()) {
        if (pending_viewport_click.button == SDL_BUTTON_RIGHT) {
          session.openBoneContext(bone, pending_viewport_click.x,
                                  pending_viewport_click.y);
        } else {
          session.selectBone(bone);
        }
      } else if (pending_viewport_click.button == SDL_BUTTON_RIGHT) {
        session.closeBoneContext();
      } else {
        // 左键点击空白处取消当前选择（与 Blockbench 一致）。
        session.selectBone(std::string{});
      }
    }

    int win_w = 0, win_h = 0;
    int fb_w = 0, fb_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
    const float scale_x =
        win_w > 0 ? static_cast<float>(fb_w) / static_cast<float>(win_w) : 1.0f;
    const float scale_y =
        win_h > 0 ? static_cast<float>(fb_h) / static_cast<float>(win_h) : 1.0f;

    if (session.playback_state == xpbd::app::PlaybackState::Playing &&
        !session.bake_busy.load()) {
      session.advancePreview(dt);
    }

    const auto ui_result =
        xpbd::app::composeNuklearUi(&ctx, win_w, win_h, 1.0f, backend->name(),
                                    backend->deviceName());
    if (session.bake_state == xpbd::app::BakeState::Completed &&
        previous_bake_state != xpbd::app::BakeState::Completed) {
      result_commit_frame_number = render_frame_number;
      completion_diagnostic_frames = 30;
    }
    previous_bake_state = session.bake_state;
    prev_layout = ui_result.layout;
    SDL_Cursor *requested_cursor = ui_result.layout.horizontal_resize_cursor &&
                                           horizontal_resize_cursor != nullptr
                                       ? horizontal_resize_cursor
                                       : default_cursor;
    if (requested_cursor != nullptr && SDL_GetCursor() != requested_cursor) {
      SDL_SetCursor(requested_cursor);
    }


    const auto t_mesh0 = std::chrono::steady_clock::now();
    if (session.camera_needs_fit && !session.geometry.bones.empty()) {
      session.fitCameraToModel();
    }


    if (session.camera_follow_preview &&
        session.playback_state == xpbd::app::PlaybackState::Playing) {
      session.updateCameraFollowPreview();
    }
    const xpbd::loader::Animation *preview_animation =
        session.currentPreviewReferenceAnimation();
    const double preview_reference_time =
        session.currentPreviewReferenceTime();
    const auto *preview_frame = session.currentPreviewFrame();
    const auto *model_texture =
        session.model_texture.valid() ? &session.model_texture : nullptr;
    if (use_static_model) {
      static_mesh_builder.setBoneMapper(&session.bone_mapper);
      static_mesh_builder.setSelectedBone(session.selected_bone_name);
      static_mesh_builder.setHoveredBone(session.hovered_bone_name);
      static_mesh_builder.setHiddenBones(&session.hidden_bone_names);
      static_mesh_builder.setTexture(model_texture);
      static_mesh_builder.setShowBones(session.show_bones);
      static_mesh_builder.setShowGround(session.show_ground);
      static_mesh_builder.setMcbeCoords(session.use_mcbe_coords);

      const std::uint64_t model_generation = session.modelGeneration();
      const std::uint64_t texture_generation = session.textureGeneration();
      if (static_cpu_generations.needsRefresh(model_generation,
                                              texture_generation)) {
        if (!static_cpu_generations.initialized ||
            static_cpu_generations.model != model_generation) {
          static_mesh_builder.setGeometry(
              session.geometry.bones.empty() ? nullptr : &session.geometry);
        }
        static_mesh_builder.buildStaticIndexedModel(static_model);
        static_cpu_generations.accept(model_generation, texture_generation);
      }

      if (preview_frame != nullptr) {
        static_mesh_builder.buildStaticBakedFrame(
            *preview_frame, preview_animation, preview_reference_time,
            static_model_frame);
      } else if (preview_animation != nullptr) {
        static_mesh_builder.buildStaticAnimationFrame(
            preview_animation, preview_reference_time, static_model_frame);
      } else {
        static_mesh_builder.buildStaticRestFrame(static_model_frame);
      }
    } else {
      xpbd::gfx::buildSessionViewportScene(
          session.geometry, session.bone_mapper, session.selected_bone_name,
          preview_animation, preview_reference_time, preview_frame != nullptr,
          preview_frame, model_texture, session.show_bones,
          session.use_mcbe_coords, scene, session.show_ground,
          &session.hidden_bone_names, session.hovered_bone_name);
    }
    const auto t_mesh1 = std::chrono::steady_clock::now();
    frame_stats.mesh_ms =
        std::chrono::duration<float, std::milli>(t_mesh1 - t_mesh0).count();

    float view[16], proj[16];
    const float aspect = ui_result.layout.vp_h > 1.0f
                             ? (ui_result.layout.vp_w / ui_result.layout.vp_h)
                             : 1.0f;
    session.camera.matrices(aspect, view, proj);



    nk_buffer vbuf;
    nk_buffer ebuf;
    nk_buffer_init_fixed(
        &vbuf, vert_slab.data(),
        static_cast<nk_size>(vert_slab.size() * sizeof(NkVertex)));
    nk_buffer_init_fixed(
        &ebuf, idx_slab.data(),
        static_cast<nk_size>(idx_slab.size() * sizeof(nk_draw_index)));

    nk_convert_config config{};
    config.vertex_layout = vertex_layout;
    config.vertex_size = sizeof(NkVertex);
    config.vertex_alignment = NK_ALIGNOF(NkVertex);
    config.tex_null = null_tex;
    config.circle_segment_count = 22;
    config.curve_segment_count = 22;
    config.arc_segment_count = 22;
    config.global_alpha = 1.0f;
    config.shape_AA = NK_ANTI_ALIASING_ON;
    config.line_AA = NK_ANTI_ALIASING_ON;
    nk_buffer_clear(&cmds);
    const nk_flags conv = nk_convert(&ctx, &cmds, &vbuf, &ebuf, &config);
    if (conv != NK_CONVERT_SUCCESS) {
      static bool once = false;
      if (!once) {
        xpbd::log::warnf(
            "nk_convert flags=0x%x allocated_v=%zu allocated_e=%zu",
            (unsigned)conv, (size_t)vbuf.allocated, (size_t)ebuf.allocated);
        once = true;
      }
    }

    xpbd::gfx::UiDrawData ui_draw{};
    ui_draw.ctx = &ctx;
    ui_draw.cmds = &cmds;
    ui_draw.vertices = &vbuf;
    ui_draw.indices = &ebuf;
    ui_draw.logical_w = win_w;
    ui_draw.logical_h = win_h;
    ui_draw.fb_w = fb_w;
    ui_draw.fb_h = fb_h;
    ui_draw.overlay_visible = ui_result.layout.overlay_visible;
    ui_draw.overlay_x = ui_result.layout.overlay_x;
    ui_draw.overlay_y = ui_result.layout.overlay_y;
    ui_draw.overlay_w = ui_result.layout.overlay_w;
    ui_draw.overlay_h = ui_result.layout.overlay_h;

    xpbd::gfx::FrameInput frame{};
    frame.fb_width = fb_w;
    frame.fb_height = fb_h;
    frame.viewport.x =
        static_cast<int>(std::lround(ui_result.layout.vp_x * scale_x));
    frame.viewport.y =
        static_cast<int>(std::lround(ui_result.layout.vp_y * scale_y));
    frame.viewport.w =
        static_cast<int>(std::lround(ui_result.layout.vp_w * scale_x));
    frame.viewport.h =
        static_cast<int>(std::lround(ui_result.layout.vp_h * scale_y));

    if (frame.viewport.w < 1) {
      frame.viewport.w = 1;
    }
    if (frame.viewport.h < 1) {
      frame.viewport.h = 1;
    }
    frame.view_matrix = view;
    frame.proj_matrix = proj;
    frame.scene = use_static_model ? &static_model_frame.overlays : &scene;
    if (use_static_model) {
      frame.static_model = &static_model;
      frame.static_model_frame = &static_model_frame;
      frame.static_model_texture = model_texture;
      frame.static_model_generation = session.modelGeneration();
      frame.static_texture_generation = session.textureGeneration();
    }
    frame.diagnostics.active = completion_diagnostic_frames > 0;
    frame.diagnostics.render_frame = render_frame_number;
    frame.diagnostics.result_commit_frame = result_commit_frame_number;
    frame.diagnostics.frames_remaining = completion_diagnostic_frames;
    frame.diagnostics.worker_phase = static_cast<int>(session.worker_phase);
    frame.diagnostics.presentation_mode =
        static_cast<int>(session.presentation_mode);
    frame.diagnostics.playback_state =
        static_cast<int>(session.playback_state);
    frame.diagnostics.preview_frame_index = session.preview_frame_index;
    frame.diagnostics.bake_current = session.bake_current;
    frame.diagnostics.bake_total = session.bake_total;
    frame.diagnostics.preview_time = session.preview_time;
    frame.diagnostics.model_generation = session.modelGeneration();
    frame.diagnostics.animation_generation = session.animationGeneration();
    frame.diagnostics.physics_generation = session.physicsGeneration();
    frame.diagnostics.texture_generation = session.textureGeneration();
    frame.ui = &ui_draw;
    frame.clear_r = 26.0f / 255.0f;
    frame.clear_g = 28.0f / 255.0f;
    frame.clear_b = 34.0f / 255.0f;

    backend->render(frame);
    if (completion_diagnostic_frames > 0) {
      --completion_diagnostic_frames;
    }
    ++render_frame_number;
    const auto backend_stats = backend->stats();
    frame_stats.gpu_ms = backend_stats.gpu_ms;
    frame_stats.upload_ms = backend_stats.upload_ms;
    frame_stats.upload_bytes = backend_stats.upload_bytes;
    frame_stats.static_bone_upload_bytes =
        backend_stats.static_bone_upload_bytes;
    frame_stats.static_resource_upload_bytes =
        backend_stats.static_resource_upload_bytes;
    frame_stats.static_resource_rebuilds =
        backend_stats.static_resource_rebuilds;
    frame_stats.static_model_vertex_bytes =
        backend_stats.static_model_vertex_bytes;
    frame_stats.static_model_index_bytes =
        backend_stats.static_model_index_bytes;
    frame_stats.static_opaque_index_count =
        backend_stats.static_opaque_index_count;
    frame_stats.static_cutout_index_count =
        backend_stats.static_cutout_index_count;
    frame_stats.static_blend_index_count =
        backend_stats.static_blend_index_count;
    frame_stats.buffer_reallocations = backend_stats.buffer_reallocations;
    frame_stats.total_buffer_reallocations =
        backend_stats.total_buffer_reallocations;
    frame_stats.backend_cpu_ms =
        backend_stats.backend_cpu_ms > 0.0f
            ? backend_stats.backend_cpu_ms
            : (backend_stats.gpu_timestamp_valid ? 0.0f : backend_stats.gpu_ms);
    frame_stats.gpu_timestamp_valid = backend_stats.gpu_timestamp_valid;
    frame_stats.gpu_timestamp_total_ms = backend_stats.gpu_timestamp_total_ms;
    frame_stats.gpu_timestamp_ui_ms = backend_stats.gpu_timestamp_ui_ms;
    frame_stats.gpu_timestamp_opaque_ms = backend_stats.gpu_timestamp_opaque_ms;
    frame_stats.gpu_timestamp_transparent_ms =
        backend_stats.gpu_timestamp_transparent_ms;
    frame_stats.gpu_timestamp_lines_ms = backend_stats.gpu_timestamp_lines_ms;
    frame_stats.draw_calls = backend_stats.draw_calls;
    frame_stats.cube_count = backend_stats.cube_count;
    frame_stats.line_count = backend_stats.line_count;




    {
      const float raw_frame =
          frame_ms > 0.0f && frame_ms < 1000.0f ? frame_ms : ema_ms;
      accum_frame_ms += raw_frame;
      accum_mesh_ms += frame_stats.mesh_ms;
      accum_upload_ms += frame_stats.upload_ms;
      accum_pick_ms += frame_stats.pick_ms;
      accum_pick_queries += frame_stats.pick_queries;
      accum_pick_cache_rebuilds += frame_stats.pick_cache_rebuilds;
      accum_pick_candidate_faces += frame_stats.pick_candidate_faces;
      accum_pick_total_faces =
          std::max(accum_pick_total_faces, frame_stats.pick_total_faces);
      accum_upload_bytes += static_cast<double>(frame_stats.upload_bytes);
      accum_static_bone_upload_bytes +=
          static_cast<double>(frame_stats.static_bone_upload_bytes);
      accum_static_resource_upload_bytes +=
          static_cast<double>(frame_stats.static_resource_upload_bytes);
      accum_buffer_reallocations += frame_stats.buffer_reallocations;
      accum_backend_cpu_ms += frame_stats.backend_cpu_ms;
      if (frame_stats.gpu_timestamp_valid) {
        accum_gpu_timestamp_total_ms += frame_stats.gpu_timestamp_total_ms;
        accum_gpu_timestamp_ui_ms += frame_stats.gpu_timestamp_ui_ms;
        accum_gpu_timestamp_opaque_ms += frame_stats.gpu_timestamp_opaque_ms;
        accum_gpu_timestamp_transparent_ms +=
            frame_stats.gpu_timestamp_transparent_ms;
        accum_gpu_timestamp_lines_ms += frame_stats.gpu_timestamp_lines_ms;
        ++accum_gpu_timestamp_frames;
      }
      ++accum_frames;
      const double window_s =
          std::chrono::duration<double>(now - sample_window_start).count();
      const bool publish = session.debug_instant_sample || window_s >= 1.0;
      if (publish && accum_frames > 0) {
        if (session.debug_instant_sample) {
          disp_frame_ms = raw_frame;
          disp_ema_ms = ema_ms;
          disp_fps = ema_ms > 0.01f ? 1000.0f / ema_ms : 0.0f;
          disp_mesh_ms = frame_stats.mesh_ms;
          disp_upload_ms = frame_stats.upload_ms;
          disp_pick_ms = frame_stats.pick_ms;
          disp_pick_queries = static_cast<float>(frame_stats.pick_queries);
          disp_pick_cache_rebuilds =
              static_cast<float>(frame_stats.pick_cache_rebuilds);
          disp_pick_candidate_faces =
              frame_stats.pick_queries > 0
                  ? frame_stats.pick_candidate_faces /
                        frame_stats.pick_queries
                  : 0;
          disp_pick_total_faces = frame_stats.pick_total_faces;
          disp_upload_bytes = static_cast<double>(frame_stats.upload_bytes);
          disp_static_bone_upload_bytes =
              static_cast<double>(frame_stats.static_bone_upload_bytes);
          disp_static_resource_upload_bytes =
              static_cast<double>(frame_stats.static_resource_upload_bytes);
          disp_buffer_reallocations = frame_stats.buffer_reallocations;
          disp_backend_cpu_ms = frame_stats.backend_cpu_ms;
          disp_gpu_timestamp_valid = frame_stats.gpu_timestamp_valid;
          disp_gpu_timestamp_total_ms = frame_stats.gpu_timestamp_total_ms;
          disp_gpu_timestamp_ui_ms = frame_stats.gpu_timestamp_ui_ms;
          disp_gpu_timestamp_opaque_ms = frame_stats.gpu_timestamp_opaque_ms;
          disp_gpu_timestamp_transparent_ms =
              frame_stats.gpu_timestamp_transparent_ms;
          disp_gpu_timestamp_lines_ms = frame_stats.gpu_timestamp_lines_ms;
          disp_cubes = frame_stats.cube_count;
        } else {
          disp_frame_ms = static_cast<float>(accum_frame_ms / accum_frames);
          disp_ema_ms = disp_frame_ms;
          disp_fps = disp_frame_ms > 0.01f ? 1000.0f / disp_frame_ms : 0.0f;
          disp_mesh_ms = static_cast<float>(accum_mesh_ms / accum_frames);
          disp_upload_ms = static_cast<float>(accum_upload_ms / accum_frames);
          disp_pick_ms =
              static_cast<float>(accum_pick_ms / accum_frames);
          disp_pick_queries = static_cast<float>(
              static_cast<double>(accum_pick_queries) / accum_frames);
          disp_pick_cache_rebuilds = static_cast<float>(
              static_cast<double>(accum_pick_cache_rebuilds) / accum_frames);
          disp_pick_candidate_faces =
              accum_pick_queries > 0
                  ? static_cast<std::uint32_t>(
                        accum_pick_candidate_faces / accum_pick_queries)
                  : 0;
          disp_pick_total_faces = accum_pick_total_faces;
          disp_upload_bytes = accum_upload_bytes / accum_frames;
          disp_static_bone_upload_bytes =
              accum_static_bone_upload_bytes / accum_frames;
          disp_static_resource_upload_bytes =
              accum_static_resource_upload_bytes / accum_frames;
          disp_buffer_reallocations = accum_buffer_reallocations / accum_frames;
          disp_backend_cpu_ms =
              static_cast<float>(accum_backend_cpu_ms / accum_frames);
          disp_gpu_timestamp_valid = accum_gpu_timestamp_frames > 0;
          if (disp_gpu_timestamp_valid) {
            disp_gpu_timestamp_total_ms = static_cast<float>(
                accum_gpu_timestamp_total_ms / accum_gpu_timestamp_frames);
            disp_gpu_timestamp_ui_ms = static_cast<float>(
                accum_gpu_timestamp_ui_ms / accum_gpu_timestamp_frames);
            disp_gpu_timestamp_opaque_ms = static_cast<float>(
                accum_gpu_timestamp_opaque_ms / accum_gpu_timestamp_frames);
            disp_gpu_timestamp_transparent_ms =
                static_cast<float>(accum_gpu_timestamp_transparent_ms /
                                   accum_gpu_timestamp_frames);
            disp_gpu_timestamp_lines_ms = static_cast<float>(
                accum_gpu_timestamp_lines_ms / accum_gpu_timestamp_frames);
          } else {
            disp_gpu_timestamp_total_ms = 0.0f;
            disp_gpu_timestamp_ui_ms = 0.0f;
            disp_gpu_timestamp_opaque_ms = 0.0f;
            disp_gpu_timestamp_transparent_ms = 0.0f;
            disp_gpu_timestamp_lines_ms = 0.0f;
          }
          disp_cubes = frame_stats.cube_count;
        }


        disp_static_resource_rebuilds = frame_stats.static_resource_rebuilds;
        disp_total_buffer_reallocations =
            frame_stats.total_buffer_reallocations;
        disp_static_model_vertex_bytes = frame_stats.static_model_vertex_bytes;
        disp_static_model_index_bytes = frame_stats.static_model_index_bytes;
        disp_static_opaque_index_count = frame_stats.static_opaque_index_count;
        disp_static_cutout_index_count = frame_stats.static_cutout_index_count;
        disp_static_blend_index_count = frame_stats.static_blend_index_count;
        accum_frame_ms = 0.0;
        accum_mesh_ms = 0.0;
        accum_upload_ms = 0.0;
        accum_pick_ms = 0.0;
        accum_pick_queries = 0;
        accum_pick_cache_rebuilds = 0;
        accum_pick_candidate_faces = 0;
        accum_pick_total_faces = 0;
        accum_upload_bytes = 0.0;
        accum_static_bone_upload_bytes = 0.0;
        accum_static_resource_upload_bytes = 0.0;
        accum_buffer_reallocations = 0.0;
        accum_backend_cpu_ms = 0.0;
        accum_gpu_timestamp_total_ms = 0.0;
        accum_gpu_timestamp_ui_ms = 0.0;
        accum_gpu_timestamp_opaque_ms = 0.0;
        accum_gpu_timestamp_transparent_ms = 0.0;
        accum_gpu_timestamp_lines_ms = 0.0;
        accum_gpu_timestamp_frames = 0;
        accum_frames = 0;
        sample_window_start = now;
      }
    }

    nk_clear(&ctx);

    {
      char title[220];
      if (session.show_debug_hud) {
        std::snprintf(title, sizeof(title),
                      "XPBD Bone Baker | %s | %.0f FPS | %.1f ms | cubes %d%s",
                      backend->name(), static_cast<double>(disp_fps),
                      disp_frame_ms, disp_cubes,
                      session.vsync_enabled ? "" : " | VSync off");
      } else {
        std::snprintf(title, sizeof(title),
                      "XPBD Bone Baker - Bedrock Physics Baking Tool | %s",
                      backend->name());
      }
      SDL_SetWindowTitle(window, title);
    }
  }

  nk_buffer_free(&cmds);
  nk_font_atlas_clear(&atlas);
  nk_free(&ctx);
  SDL_StopTextInput(window);
  session.shutdownBakeWorker();
  if (horizontal_resize_cursor != nullptr) {
    if (default_cursor != nullptr) {
      SDL_SetCursor(default_cursor);
    }
    SDL_DestroyCursor(horizontal_resize_cursor);
  }
  backend->shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  xpbd::log::shutdown();
  return 0;
}

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>


int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return app_main(__argc, __argv);
}
#else
int main(int argc, char **argv) { return app_main(argc, argv); }
#endif
