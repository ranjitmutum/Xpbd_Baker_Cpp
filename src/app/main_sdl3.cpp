#include "xpbd/app/app_session.hpp"
#include "xpbd/app/i18n.hpp"
#include "xpbd/app/native_dialog.hpp"
#include "xpbd/app/nuklear_ui.hpp"
#include "xpbd/gfx/backend_select.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/preview_scene.hpp"
#include "xpbd/gfx/rtxpt_bridge.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/log.hpp"

#include <charconv>
#include <cstdio>
#include <system_error>


#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <SDL3/SDL_system.h>
#endif

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

#if defined(_WIN32)
struct PclWindowsMessageHookContext {
  xpbd::gfx::IGpuBackend *backend = nullptr;
};

bool SDLCALL pclWindowsMessageHook(void *userdata, MSG *message) {
  auto *context =
      static_cast<PclWindowsMessageHookContext *>(userdata);
  if (context != nullptr && context->backend != nullptr &&
      message != nullptr) {
    const std::uint32_t ping_message =
        context->backend->latencyPingMessage();
    if (ping_message != 0u && message->message == ping_message) {
      context->backend->markLatencyPing();
    }
  }
  return true;
}
#endif


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

  std::filesystem::path executable_base_path;
  if (const char *base = SDL_GetBasePath()) {
    // SDL3 owns and releases this cached const path during SDL_Quit().
    executable_base_path = std::filesystem::path(base);
  }
  const std::filesystem::path preview_scene_asset_root =
      executable_base_path.empty()
          ? std::filesystem::path("assets") / "preview_scenes"
          : executable_base_path / "assets" / "preview_scenes";

  {
    std::string log_path = "xpbd_baker.log";
    if (!executable_base_path.empty()) {
      log_path = (executable_base_path / "xpbd_baker.log").string();
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

  // Wire native file dialogs to the app window and pause the GPU while open.
  {
    xpbd::app::NativeDialogHooks hooks{};
#if defined(_WIN32)
    void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                        nullptr);
    hooks.owner_window = hwnd;
#endif
    // Static pointer: dialogs run on the UI thread while backend is alive.
    static xpbd::gfx::IGpuBackend *s_backend_for_dialog = nullptr;
    s_backend_for_dialog = backend.get();
    hooks.prepare = []() {
      if (s_backend_for_dialog) {
        s_backend_for_dialog->prepareForSystemDialog();
      }
    };
    hooks.finish = []() {
      if (s_backend_for_dialog) {
        s_backend_for_dialog->resumeAfterSystemDialog();
      }
    };
    xpbd::app::setNativeDialogHooks(hooks);
  }

  {
    const auto rtxpt = xpbd::gfx::queryRtxptStatus();
    xpbd::log::infof("Preview RT: build=%d tree=%d runtime=%d ver=%s — %s",
                     rtxpt.build_enabled ? 1 : 0, rtxpt.tree_available ? 1 : 0,
                     rtxpt.runtime_ready ? 1 : 0,
                     rtxpt.version.empty() ? "-" : rtxpt.version.c_str(),
                     rtxpt.detail.c_str());
  }
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
  xpbd::gfx::ViewportRasterScene raster_scene;
  std::string logged_preview_skybox_source;
  const bool vulkan_raster_path =
      backend->kind() == xpbd::gfx::BackendKind::Vulkan;
  xpbd::gfx::StaticIndexedModelMesh static_model;
  xpbd::gfx::StaticModelFrameData static_model_frame;
  xpbd::gfx::StaticModelGenerationCache static_cpu_generations;
  xpbd::gfx::FrameStats frame_stats;
  float ema_ms = 16.0f;


  float disp_fps = 60.0f;
  float disp_original_fps = 60.0f;
  float disp_dlss_fg_fps = 0.0f;
  bool disp_dlss_fg_active = false;
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
  const auto app_start_time = sample_window_start;

  bool running = true;
  auto last = std::chrono::steady_clock::now();
  auto &session = xpbd::app::AppSession::instance();
  session.setApplicationDirectory(executable_base_path);
  if (const char *startup_model = std::getenv("XPBD_MODEL");
      startup_model != nullptr && startup_model[0] != '\0') {
    session.loadModel(std::filesystem::path(startup_model));
    if (session.last_error.empty()) {
      xpbd::log::infof("Startup model: %s", startup_model);
    } else {
      xpbd::log::warnf("Startup model failed: %s (%s)", startup_model,
                       session.last_error.c_str());
    }
  }
  if (const char *startup_animation = std::getenv("XPBD_ANIMATION");
      startup_animation != nullptr && startup_animation[0] != '\0') {
    session.loadAnimation(std::filesystem::path(startup_animation));
    if (session.last_error.empty()) {
      xpbd::log::infof("Startup animation: %s", startup_animation);
    } else {
      xpbd::log::warnf("Startup animation failed: %s (%s)",
                       startup_animation, session.last_error.c_str());
    }
  }
  if (const char *startup_texture = std::getenv("XPBD_TEXTURE");
      startup_texture != nullptr && startup_texture[0] != '\0') {
    if (session.loadTexture(std::filesystem::path(startup_texture))) {
      xpbd::log::infof("Startup texture: %s", startup_texture);
    } else {
      xpbd::log::warnf("Startup texture failed: %s (%s)", startup_texture,
                       session.last_error.c_str());
    }
  }
  if (const char *startup_specular =
          std::getenv("XPBD_LABPBR_SPECULAR");
      startup_specular != nullptr && startup_specular[0] != '\0') {
    if (session.importLabPbrSpecular(
            std::filesystem::path(startup_specular))) {
      xpbd::log::infof("Startup LabPBR specular: %s",
                       startup_specular);
    } else {
      xpbd::log::warnf(
          "Startup LabPBR specular failed: %s (%s)",
          startup_specular, session.last_error.c_str());
    }
  }
  if (const char *startup_normal = std::getenv("XPBD_IRIS_NORMAL");
      startup_normal != nullptr && startup_normal[0] != '\0') {
    if (session.importLabPbrNormal(
            std::filesystem::path(startup_normal))) {
      xpbd::log::infof("Startup Iris normal: %s", startup_normal);
    } else {
      xpbd::log::warnf("Startup Iris normal failed: %s (%s)",
                       startup_normal, session.last_error.c_str());
    }
  }
  if (const char *material_debug = std::getenv("XPBD_LABPBR_DEBUG");
      material_debug != nullptr && material_debug[0] != '\0') {
    session.labpbr_debug_view =
        xpbd::gfx::labPbrDebugViewFromName(material_debug);
    xpbd::log::infof(
        "LabPBR debug view: %s",
        xpbd::gfx::labPbrDebugViewName(session.labpbr_debug_view));
  }
  if (const char *rt_debug = std::getenv("XPBD_RT_DEBUG");
      rt_debug != nullptr && rt_debug[0] != '\0') {
    session.rt_debug_view = xpbd::gfx::rtDebugViewFromName(rt_debug);
    xpbd::log::infof("RT debug view: %s",
                     xpbd::gfx::rtDebugViewName(session.rt_debug_view));
  }
  if (const char *startup_rt = std::getenv("XPBD_RT")) {
    session.enable_ray_tracing =
        std::strcmp(startup_rt, "1") == 0 ||
        std::strcmp(startup_rt, "true") == 0 ||
        std::strcmp(startup_rt, "TRUE") == 0;
  }
  auto apply_path_trace_uint = [](const char *name,
                                  std::uint32_t &target) {
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
      return;
    }
    const char *end = text + std::strlen(text);
    std::uint32_t parsed_value = 0;
    const auto parsed = std::from_chars(text, end, parsed_value);
    if (parsed.ec == std::errc{} && parsed.ptr == end) {
      target = parsed_value;
    } else {
      xpbd::log::warnf("Ignoring invalid %s=%s", name, text);
    }
  };
  apply_path_trace_uint("XPBD_PT_SPP",
                        session.path_trace_settings.samples_per_frame);
  apply_path_trace_uint("XPBD_PT_MAX_SAMPLES",
                        session.path_trace_settings.maximum_samples);
  apply_path_trace_uint("XPBD_PT_BOUNCES",
                        session.path_trace_settings.max_bounces);
  apply_path_trace_uint(
      "XPBD_PT_DIFFUSE_BOUNCES",
      session.path_trace_settings.max_diffuse_bounces);
  apply_path_trace_uint(
      "XPBD_PT_GLOSSY_BOUNCES",
      session.path_trace_settings.max_glossy_bounces);
  apply_path_trace_uint(
      "XPBD_PT_TRANSMISSION_BOUNCES",
      session.path_trace_settings.max_transmission_bounces);
  apply_path_trace_uint(
      "XPBD_PT_TRANSPARENT_BOUNCES",
      session.path_trace_settings.max_transparent_bounces);
  apply_path_trace_uint(
      "XPBD_PT_RR_START",
      session.path_trace_settings.russian_roulette_start);
  std::uint32_t russian_roulette =
      session.path_trace_settings.russian_roulette ? 1u : 0u;
  apply_path_trace_uint("XPBD_PT_RR", russian_roulette);
  session.path_trace_settings.russian_roulette =
      russian_roulette != 0u;
  apply_path_trace_uint("XPBD_PT_SEED",
                        session.path_trace_settings.seed);
  if (const char *fixed_seed = std::getenv("XPBD_PT_SEED");
      fixed_seed != nullptr && fixed_seed[0] != '\0') {
    session.path_trace_settings.automatic_seed = false;
  }
  auto apply_path_trace_bool = [](const char *name, bool &target) {
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
      return;
    }
    if (std::strcmp(text, "1") == 0 ||
        std::strcmp(text, "true") == 0 ||
        std::strcmp(text, "TRUE") == 0) {
      target = true;
    } else if (std::strcmp(text, "0") == 0 ||
               std::strcmp(text, "false") == 0 ||
               std::strcmp(text, "FALSE") == 0) {
      target = false;
    } else {
      xpbd::log::warnf("Ignoring invalid %s=%s", name, text);
    }
  };
  apply_path_trace_bool(
      "XPBD_PT_NVIDIA_RT_CORE",
      session.path_trace_settings.nvidia_rt_core_acceleration);
  apply_path_trace_bool("XPBD_PT_ANALYTIC_LIGHTS",
                        session.path_trace_settings.analytic_lights);
  apply_path_trace_bool("XPBD_PT_EMISSIVE_SURFACES",
                        session.path_trace_settings.emissive_surfaces);
  apply_path_trace_bool("XPBD_PT_NEE",
                        session.path_trace_settings.next_event_estimation);
  apply_path_trace_bool(
      "XPBD_PT_MIS",
      session.path_trace_settings.multiple_importance_sampling);
  apply_path_trace_bool(
      "XPBD_PT_ENV_IMPORTANCE",
      session.path_trace_settings.environment_importance_sampling);
  apply_path_trace_uint(
      "XPBD_PT_LIGHT_SAMPLES",
      session.path_trace_settings.light_samples_per_path);
  auto apply_path_trace_float = [](const char *name, float &target) {
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
      return;
    }
    char *end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end != text && end != nullptr && end[0] == '\0' &&
        std::isfinite(parsed)) {
      target = parsed;
    } else {
      xpbd::log::warnf("Ignoring invalid %s=%s", name, text);
    }
  };
  apply_path_trace_float("XPBD_PT_EXPOSURE_EV",
                         session.path_trace_settings.display_exposure_ev);
  apply_path_trace_float(
      "XPBD_PT_EMISSIVE_MULTIPLIER",
      session.path_trace_settings.emissive_multiplier);
  apply_path_trace_float("XPBD_PT_DIRECT_CLAMP",
                         session.path_trace_settings.direct_clamp);
  apply_path_trace_float("XPBD_PT_INDIRECT_CLAMP",
                         session.path_trace_settings.indirect_clamp);
  apply_path_trace_float(
      "XPBD_PT_PREVIEW_SCALE",
      session.path_trace_settings.preview_resolution_scale);
  if (const char *denoiser = std::getenv("XPBD_PT_DENOISER");
      denoiser != nullptr && denoiser[0] != '\0') {
    if (std::strcmp(denoiser, "auto") == 0) {
      session.path_trace_settings.requested_denoiser =
          xpbd::gfx::PathTraceDenoiser::Auto;
    } else if (std::strcmp(denoiser, "raw") == 0 ||
               std::strcmp(denoiser, "off") == 0) {
      session.path_trace_settings.requested_denoiser =
          xpbd::gfx::PathTraceDenoiser::Raw;
    } else if (std::strcmp(denoiser, "nrd_reblur") == 0 ||
               std::strcmp(denoiser, "reblur") == 0 ||
               std::strcmp(denoiser, "nrd_relax") == 0 ||
               std::strcmp(denoiser, "relax") == 0) {
      session.path_trace_settings.requested_denoiser =
          xpbd::gfx::PathTraceDenoiser::Raw;
      xpbd::log::warnf(
          "XPBD_PT_DENOISER=%s names a retired NRD mode; using raw",
          denoiser);
    } else if (std::strcmp(denoiser, "dlss_rr") == 0 ||
               std::strcmp(denoiser, "rr") == 0) {
      session.path_trace_settings.requested_denoiser =
          xpbd::gfx::PathTraceDenoiser::DlssRayReconstruction;
    } else {
      xpbd::log::warnf("Ignoring invalid XPBD_PT_DENOISER=%s",
                       denoiser);
    }
  }
  if (const char *upscale = std::getenv("XPBD_PT_UPSCALE");
      upscale != nullptr && upscale[0] != '\0') {
    if (std::strcmp(upscale, "off") == 0 ||
        std::strcmp(upscale, "native") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::Off;
    } else if (std::strcmp(upscale, "auto") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::Quality;
      xpbd::log::warn(
          "XPBD_PT_UPSCALE=auto is deprecated; using quality");
    } else if (std::strcmp(upscale, "dlaa") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::Dlaa;
    } else if (std::strcmp(upscale, "ultra_quality") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::Quality;
      xpbd::log::warn(
          "XPBD_PT_UPSCALE=ultra_quality is deprecated; using quality");
    } else if (std::strcmp(upscale, "quality") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::Quality;
    } else if (std::strcmp(upscale, "balanced") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::Balanced;
    } else if (std::strcmp(upscale, "performance") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::Performance;
    } else if (std::strcmp(upscale, "ultra_performance") == 0) {
      session.path_trace_settings.requested_upscale =
          xpbd::gfx::PathTraceUpscale::UltraPerformance;
    } else {
      xpbd::log::warnf("Ignoring invalid XPBD_PT_UPSCALE=%s",
                       upscale);
    }
  }
  if (const char *environment = std::getenv("XPBD_PT_ENVIRONMENT");
      environment != nullptr && environment[0] != '\0') {
    char *end = nullptr;
    const float parsed = std::strtof(environment, &end);
    if (end != environment && end != nullptr && end[0] == '\0' &&
        std::isfinite(parsed)) {
      session.path_trace_settings.analytic_environment_strength = parsed;
    } else {
      xpbd::log::warnf("Ignoring invalid XPBD_PT_ENVIRONMENT=%s",
                       environment);
    }
  }
  session.path_trace_settings = xpbd::gfx::normalizePathTraceSettings(
      session.path_trace_settings);
  xpbd::log::infof(
      "Path tracing settings: spp=%u max_samples=%u bounces=%u "
      "diffuse=%u glossy=%u transmission=%u transparent=%u rr=%d "
      "rr_start=%u seed=%u auto_seed=%d environment=%.3f "
      "exposure_ev=%.3f rt_core=%d nee=%d mis=%d env_importance=%d "
      "light_samples=%u emissive=%.3f clamp=%.3f/%.3f scale=%.3f "
      "denoiser=%u upscale=%u frame_generation=%u reflex=%u",
      session.path_trace_settings.samples_per_frame,
      session.path_trace_settings.maximum_samples,
      session.path_trace_settings.max_bounces,
      session.path_trace_settings.max_diffuse_bounces,
      session.path_trace_settings.max_glossy_bounces,
      session.path_trace_settings.max_transmission_bounces,
      session.path_trace_settings.max_transparent_bounces,
      session.path_trace_settings.russian_roulette ? 1 : 0,
      session.path_trace_settings.russian_roulette_start,
      session.path_trace_settings.seed,
      session.path_trace_settings.automatic_seed ? 1 : 0,
      session.path_trace_settings.analytic_environment_strength,
      session.path_trace_settings.display_exposure_ev,
      session.path_trace_settings.nvidia_rt_core_acceleration ? 1 : 0,
      session.path_trace_settings.next_event_estimation ? 1 : 0,
      session.path_trace_settings.multiple_importance_sampling ? 1 : 0,
      session.path_trace_settings.environment_importance_sampling ? 1 : 0,
      session.path_trace_settings.light_samples_per_path,
      session.path_trace_settings.emissive_multiplier,
      session.path_trace_settings.direct_clamp,
      session.path_trace_settings.indirect_clamp,
      session.path_trace_settings.preview_resolution_scale,
      static_cast<unsigned>(
          session.path_trace_settings.requested_denoiser),
      static_cast<unsigned>(
          session.path_trace_settings.requested_upscale),
      static_cast<unsigned>(
          session.path_trace_settings.requested_frame_generation),
      static_cast<unsigned>(
          session.path_trace_settings.requested_reflex_mode));
  if (const char *startup_hdri = std::getenv("XPBD_HDRI");
      startup_hdri != nullptr && startup_hdri[0] != '\0') {
    if (session.loadWorldHdr(std::filesystem::path(startup_hdri))) {
      xpbd::log::infof(
          "Startup World HDRI: %s (%ux%u checksum=%s)",
          session.world_environment.hdr.source_identity.c_str(),
          session.world_environment.hdr.radiance.width,
          session.world_environment.hdr.radiance.height,
          session.world_environment.hdr.checksum.c_str());
    } else {
      xpbd::log::warnf("Startup World HDRI failed: %s (%s)",
                       startup_hdri, session.last_error.c_str());
    }
  }
  auto apply_world_float = [&](const char *name, float &target,
                               float minimum, float maximum) {
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
      return;
    }
    char *end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end != text && end != nullptr && end[0] == '\0' &&
        std::isfinite(parsed)) {
      target = std::clamp(parsed, minimum, maximum);
    } else {
      xpbd::log::warnf("Ignoring invalid %s=%s", name, text);
    }
  };
  apply_world_float("XPBD_SKY_STRENGTH_EV",
                    session.world_environment.global_lighting_strength_ev,
                    -10.0f, 10.0f);
  if (const char *legacy_strength = std::getenv("XPBD_HDRI_STRENGTH");
      legacy_strength != nullptr && legacy_strength[0] != '\0') {
    char *end = nullptr;
    const float parsed = std::strtof(legacy_strength, &end);
    if (end != legacy_strength && end != nullptr && end[0] == '\0' &&
        std::isfinite(parsed) && parsed > 0.0f) {
      session.world_environment.global_lighting_strength_ev =
          std::clamp(std::log2(parsed), -10.0f, 10.0f);
    } else {
      xpbd::log::warnf("Ignoring invalid XPBD_HDRI_STRENGTH=%s",
                       legacy_strength);
    }
  }
  apply_world_float("XPBD_HDRI_BACKGROUND_EXPOSURE",
                    session.world_environment.background_exposure, -10.0f,
                    10.0f);
  float hdri_rotation_degrees =
      session.world_environment.rotation_radians *
      (180.0f / 3.14159265358979323846f);
  apply_world_float("XPBD_HDRI_ROTATION_DEGREES", hdri_rotation_degrees,
                    -360000.0f, 360000.0f);
  session.world_environment.rotation_radians =
      hdri_rotation_degrees * (3.14159265358979323846f / 180.0f);
  auto apply_world_bool = [&](const char *name, bool &target) {
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
      return;
    }
    if (std::strcmp(text, "1") == 0 ||
        std::strcmp(text, "true") == 0 ||
        std::strcmp(text, "TRUE") == 0) {
      target = true;
    } else if (std::strcmp(text, "0") == 0 ||
               std::strcmp(text, "false") == 0 ||
               std::strcmp(text, "FALSE") == 0) {
      target = false;
    } else {
      xpbd::log::warnf("Ignoring invalid %s=%s", name, text);
    }
  };
  apply_world_bool("XPBD_HDRI_BACKGROUND_VISIBLE",
                   session.world_environment.background_visible);
  apply_world_bool("XPBD_HDRI_LIGHTING",
                   session.world_environment.environment_lighting);
  bool startup_procedural_sky = false;
  apply_world_bool("XPBD_SKY_PROCEDURAL", startup_procedural_sky);
  apply_world_bool("XPBD_SKY_TIME_PLAYING",
                   session.world_environment.time.playing);
  apply_world_float("XPBD_SKY_TIME_SPEED",
                    session.world_environment.time.time_speed,
                    -86400.0f, 86400.0f);
  auto &startup_clouds = session.world_environment.clouds;
  apply_world_bool("XPBD_CLOUDS", startup_clouds.enabled);
  apply_world_float("XPBD_CLOUD_COVERAGE", startup_clouds.coverage, 0.0f,
                    1.0f);
  apply_world_float("XPBD_CLOUD_DENSITY", startup_clouds.density, 0.0001f,
                    8.0f);
  apply_world_float("XPBD_CLOUD_BASE_KM", startup_clouds.base_altitude_km,
                    0.1f, 20.0f);
  apply_world_float("XPBD_CLOUD_THICKNESS_KM", startup_clouds.thickness_km,
                    0.1f, 20.0f);
  apply_world_float("XPBD_CLOUD_WIND_X", startup_clouds.wind_direction[0],
                    -1000.0f, 1000.0f);
  apply_world_float("XPBD_CLOUD_WIND_Z", startup_clouds.wind_direction[1],
                    -1000.0f, 1000.0f);
  apply_world_float("XPBD_CLOUD_OFFSET_X_KM",
                    startup_clouds.weather_offset_km[0], -1.0e6f, 1.0e6f);
  apply_world_float("XPBD_CLOUD_OFFSET_Z_KM",
                    startup_clouds.weather_offset_km[1], -1.0e6f, 1.0e6f);
  apply_world_float("XPBD_CLOUD_TIME_SECONDS", startup_clouds.time_seconds,
                    -1.0e7f, 1.0e7f);
  apply_world_float("XPBD_CLOUD_RENDER_RATIO", startup_clouds.render_ratio,
                    0.25f, 1.0f);
  apply_world_bool("XPBD_CLOUD_REPROJECTION",
                   startup_clouds.reprojection);
  apply_world_float("XPBD_CLOUD_HISTORY_WEIGHT",
                    startup_clouds.history_weight, 0.0f, 0.999f);
  apply_world_float("XPBD_CLOUD_SHADOW_STRENGTH",
                    startup_clouds.shadow_strength, 0.0f, 1.0f);
  apply_world_float("XPBD_CLOUD_LIGHTING_STRENGTH",
                    startup_clouds.lighting_strength, 0.0f, 8.0f);
  apply_path_trace_uint("XPBD_CLOUD_SHADOW_RESOLUTION",
                        startup_clouds.shadow_resolution);
  startup_clouds.shadow_resolution =
      std::clamp(startup_clouds.shadow_resolution, 64u, 4096u);
  apply_path_trace_uint("XPBD_CLOUD_SEED", startup_clouds.seed);
  apply_path_trace_uint("XPBD_CLOUD_RAY_STEPS", startup_clouds.ray_steps);
  apply_path_trace_uint("XPBD_CLOUD_LIGHT_STEPS",
                        startup_clouds.light_steps);
  apply_path_trace_uint("XPBD_CLOUD_TEMPORAL_FRAME",
                        startup_clouds.temporal_frame);
  if (startup_clouds.enabled) {
    xpbd::log::infof(
        "Startup volumetric clouds: valid=%s coverage=%.3f density=%.3f "
        "layer=%.3f+%.3fkm wind=%.3f/%.3f offset=%.3f/%.3fkm "
        "time=%.3fs seed=%u steps=%u/%u temporal=%u ratio=%.2f "
        "reprojection=%d history=%.3f shadow=%u",
        startup_clouds.valid() ? "true" : "false", startup_clouds.coverage,
        startup_clouds.density, startup_clouds.base_altitude_km,
        startup_clouds.thickness_km, startup_clouds.wind_direction[0],
        startup_clouds.wind_direction[1],
        startup_clouds.weather_offset_km[0],
        startup_clouds.weather_offset_km[1], startup_clouds.time_seconds,
        startup_clouds.seed, startup_clouds.ray_steps,
        startup_clouds.light_steps, startup_clouds.temporal_frame,
        startup_clouds.render_ratio,
        startup_clouds.reprojection ? 1 : 0,
        startup_clouds.history_weight,
        startup_clouds.shadow_resolution);
  }
  if (startup_procedural_sky) {
    int reference_utc_hour = 0;
    if (const char *hour_text = std::getenv("XPBD_SKY_UTC_HOUR");
        hour_text != nullptr && hour_text[0] != '\0') {
      const char *hour_end = hour_text + std::strlen(hour_text);
      int parsed_hour = 0;
      const auto parsed =
          std::from_chars(hour_text, hour_end, parsed_hour);
      if (parsed.ec == std::errc{} && parsed.ptr == hour_end &&
          parsed_hour >= 0 && parsed_hour <= 23) {
        reference_utc_hour = parsed_hour;
      } else {
        xpbd::log::warnf("Ignoring invalid XPBD_SKY_UTC_HOUR=%s",
                         hour_text);
      }
    }
    const xpbd::gfx::UtcDateTime reference_utc{
        2024, 1, 1, reference_utc_hour, 0, 0.0};
    const xpbd::gfx::ObserverLocation reference_observer{
        31.2304, 121.4737, 5.0, 0.0};
    std::string celestial_error;
    xpbd::gfx::CelestialState celestial;
    if (xpbd::gfx::computeCelestialState(
            reference_utc, reference_observer, celestial, &celestial_error)) {
      session.world_environment.celestial = celestial;
      session.world_environment.atmosphere =
          xpbd::gfx::defaultEarthAtmosphereConfig();
      session.world_environment.procedural_resources_ready = true;
      session.world_environment.sky_rendering =
          xpbd::gfx::SkyRendering::ProceduralDayNight;
      session.world_environment.generation =
          session.world_environment.generation <
                  (std::numeric_limits<std::uint64_t>::max)()
              ? session.world_environment.generation + 1u
              : session.world_environment.generation;
      xpbd::log::infof(
          "Startup procedural sky requested with frozen Phase 6 "
          "UTC/Shanghai diagnostic state (UTC hour=%d, twilight=%s, "
          "Sun altitude=%.3f, Moon azimuth=%.3f, Moon altitude=%.3f, "
          "Moon phase=%.6f)",
          reference_utc_hour,
          xpbd::gfx::twilightPhaseName(celestial.twilight),
          celestial.sun.geometric_altitude_degrees,
          celestial.moon.azimuth_degrees,
          celestial.moon.geometric_altitude_degrees,
          celestial.moon_illuminated_fraction);
    } else {
      xpbd::log::warnf("Startup procedural sky failed: %s",
                       celestial_error.c_str());
    }
  }
  if (const char *startup_scene = std::getenv("XPBD_PREVIEW_SCENE")) {
    int scene_index = -1;
    const char *scene_end = startup_scene + std::strlen(startup_scene);
    const auto parsed =
        std::from_chars(startup_scene, scene_end, scene_index);
    if (parsed.ec == std::errc{} && parsed.ptr == scene_end &&
        scene_index >= 0 && scene_index < xpbd::gfx::kPreviewSceneCount) {
      const auto preset = xpbd::gfx::previewSceneIdFromIndex(scene_index);
      if (preset == xpbd::gfx::PreviewSceneId::None) {
        session.selectScene(xpbd::app::SceneSelectionKind::Empty);
      } else {
        session.selectPresetScene(preset);
      }
    }
  }
  if (const char *startup_dynamic = std::getenv("XPBD_PREVIEW_DYNAMIC")) {
    session.dynamic_preview_scene =
        std::strcmp(startup_dynamic, "1") == 0 ||
        std::strcmp(startup_dynamic, "true") == 0 ||
        std::strcmp(startup_dynamic, "TRUE") == 0;
  }
  if (const char *startup_show_bones = std::getenv("XPBD_SHOW_BONES")) {
    session.show_bones =
        std::strcmp(startup_show_bones, "1") == 0 ||
        std::strcmp(startup_show_bones, "true") == 0 ||
        std::strcmp(startup_show_bones, "TRUE") == 0;
    xpbd::log::infof("Startup bone visualization: %s",
                     session.show_bones ? "on" : "off");
  }
  bool startup_camera_overridden = false;
  auto apply_camera_override = [&](const char *name, float &target,
                                   float minimum, float maximum) {
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
      return;
    }
    char *end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end != text && end != nullptr && end[0] == '\0' &&
        std::isfinite(parsed)) {
      target = (std::max)(minimum, (std::min)(maximum, parsed));
      startup_camera_overridden = true;
    }
  };
  apply_camera_override("XPBD_CAMERA_YAW", session.camera.yaw_deg, -360.0f,
                        360.0f);
  apply_camera_override("XPBD_CAMERA_PITCH", session.camera.pitch_deg, -89.0f,
                        89.0f);
  apply_camera_override("XPBD_CAMERA_DISTANCE", session.camera.distance, 2.0f,
                        500.0f);
  apply_camera_override("XPBD_CAMERA_PAN_X", session.camera.pan_x, -100000.0f,
                        100000.0f);
  apply_camera_override("XPBD_CAMERA_PAN_Y", session.camera.pan_y, -100000.0f,
                        100000.0f);
  apply_camera_override("XPBD_CAMERA_PAN_Z", session.camera.pan_z, -100000.0f,
                         100000.0f);
  apply_camera_override("XPBD_CAMERA_FOV_Y", session.camera.fov_y_deg, 1.0f,
                         120.0f);
  if (startup_camera_overridden) {
    session.camera_needs_fit = false;
    xpbd::log::infof("Startup camera override: yaw=%.2f pitch=%.2f "
                     "distance=%.2f pan=%.2f/%.2f/%.2f fov_y=%.2f",
                     session.camera.yaw_deg, session.camera.pitch_deg,
                     session.camera.distance, session.camera.pan_x,
                     session.camera.pan_y, session.camera.pan_z,
                     session.camera.fov_y_deg);
  }
  if (const char *startup_autoplay = std::getenv("XPBD_AUTOPLAY");
      startup_autoplay != nullptr &&
      (std::strcmp(startup_autoplay, "1") == 0 ||
       std::strcmp(startup_autoplay, "true") == 0 ||
       std::strcmp(startup_autoplay, "TRUE") == 0)) {
    if (session.canPreview()) {
      session.togglePreviewPlayback();
      xpbd::log::info("Startup animation autoplay enabled");
    } else {
      xpbd::log::warn(
          "Startup autoplay requested without a previewable model+animation");
    }
  }
  bool startup_still_render = false;
  apply_path_trace_bool("XPBD_STILL_RENDER", startup_still_render);
  if (startup_still_render) {
    auto &settings = session.still_render_job.settings;
    apply_path_trace_uint("XPBD_STILL_WIDTH", settings.width);
    apply_path_trace_uint("XPBD_STILL_HEIGHT", settings.height);
    apply_path_trace_uint("XPBD_STILL_SAMPLES", settings.target_samples);
    apply_path_trace_uint("XPBD_STILL_SPP", settings.samples_per_submit);
    apply_path_trace_bool("XPBD_STILL_TRANSPARENT",
                          settings.transparent_background);
    if (const char *filename = std::getenv("XPBD_STILL_FILENAME");
        filename != nullptr && filename[0] != '\0') {
      settings.filename = filename;
    }
    if (const char *format = std::getenv("XPBD_STILL_FORMAT");
        format != nullptr && format[0] != '\0') {
      if (std::strcmp(format, "exr") == 0 ||
          std::strcmp(format, "EXR") == 0) {
        settings.format = xpbd::gfx::StillImageFormat::Exr;
      } else if (std::strcmp(format, "png") == 0 ||
                 std::strcmp(format, "PNG") == 0) {
        settings.format = xpbd::gfx::StillImageFormat::Png;
      } else {
        xpbd::log::warnf("Ignoring invalid XPBD_STILL_FORMAT=%s", format);
      }
    }
    if (session.queueStillRender()) {
      xpbd::log::infof(
          "Startup still render queued: %ux%u samples=%u spp=%u format=%s "
          "transparent=%d output=%s",
          settings.width, settings.height, settings.target_samples,
          settings.samples_per_submit,
          settings.format == xpbd::gfx::StillImageFormat::Exr ? "exr"
                                                               : "png",
          settings.transparent_background ? 1 : 0,
          session.still_render_job.status.output_path.c_str());
    } else {
      xpbd::log::warnf("Startup still render failed to queue: %s",
                       session.last_error.c_str());
    }
  }
  std::uint64_t render_frame_number = 0;
  std::uint64_t unattended_frame_limit = 0;
  if (const char *text = std::getenv("XPBD_UNATTENDED_FRAMES");
      text != nullptr && text[0] != '\0') {
    const char *end = text + std::strlen(text);
    const auto parsed =
        std::from_chars(text, end, unattended_frame_limit);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
      xpbd::log::warnf("Ignoring invalid XPBD_UNATTENDED_FRAMES=%s",
                       text);
      unattended_frame_limit = 0;
    } else if (unattended_frame_limit > 0) {
      xpbd::log::infof("Unattended frame limit: %llu",
                       static_cast<unsigned long long>(
                           unattended_frame_limit));
    }
  }
  bool unattended_resize_gate = false;
  apply_path_trace_bool(
      "XPBD_UNATTENDED_RESIZE_GATE", unattended_resize_gate);
  int unattended_initial_width = 0;
  int unattended_initial_height = 0;
  if (unattended_resize_gate) {
    SDL_GetWindowSize(
        window, &unattended_initial_width, &unattended_initial_height);
    xpbd::log::infof(
        "Unattended preview resize gate enabled: initial=%dx%d",
        unattended_initial_width, unattended_initial_height);
  }
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
  struct WindowedGeometry {
    int x = 0;
    int y = 0;
    int width = kWinW;
    int height = kWinH;
    bool maximized = false;
    bool valid = false;
  } windowed_geometry;

  const auto toggle_borderless_fullscreen = [&]() {
    const bool entering =
        (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0;
    if (entering) {
      windowed_geometry.valid =
          SDL_GetWindowPosition(window, &windowed_geometry.x,
                                &windowed_geometry.y) &&
          SDL_GetWindowSize(window, &windowed_geometry.width,
                            &windowed_geometry.height);
      windowed_geometry.maximized =
          (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    }

    // DLSS-G must be disabled and all tagged resources retired before a
    // window-mode/swapchain transition.
    backend->prepareForSystemDialog();
    bool changed = true;
    if (entering) {
      changed = SDL_SetWindowFullscreenMode(window, nullptr) &&
                SDL_SetWindowFullscreen(window, true);
    } else {
      changed = SDL_SetWindowFullscreen(window, false);
    }
    if (changed) {
      changed = SDL_SyncWindow(window);
    }
    if (changed && !entering && windowed_geometry.valid) {
      if (windowed_geometry.maximized) {
        changed = SDL_MaximizeWindow(window);
      } else {
        changed =
            SDL_SetWindowSize(window, windowed_geometry.width,
                              windowed_geometry.height) &&
            SDL_SetWindowPosition(window, windowed_geometry.x,
                                  windowed_geometry.y);
      }
      if (changed) {
        changed = SDL_SyncWindow(window);
      }
    }

    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
    backend->resumeAfterSystemDialog();
    if (pixel_width > 0 && pixel_height > 0) {
      backend->resize(pixel_width, pixel_height);
    }
    if (!changed) {
      xpbd::log::warnf(
          "F11 borderless fullscreen transition failed: %s",
          SDL_GetError() != nullptr ? SDL_GetError() : "unknown error");
    } else {
      xpbd::log::infof(
          "F11 borderless fullscreen: %s (%dx%d)",
          entering ? "on" : "off", pixel_width, pixel_height);
    }
  };

#if defined(_WIN32)
  PclWindowsMessageHookContext pcl_message_hook_context{backend.get()};
  SDL_SetWindowsMessageHook(
      pclWindowsMessageHook, &pcl_message_hook_context);
#endif

  while (running) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    const bool latency_frame_started =
        !backend->presentationSuspended();
    if (latency_frame_started) {
      backend->beginLatencyFrame(
          static_cast<std::uint32_t>(render_frame_number),
          session.path_trace_settings.requested_reflex_mode,
          session.enable_ray_tracing &&
              session.path_trace_settings.requested_frame_generation ==
                  xpbd::gfx::PathTraceFrameGeneration::On);
    }
    const float frame_ms = dt * 1000.0f;
    if (frame_ms > 0.0f && frame_ms < 1000.0f) {
      constexpr float kAlpha = 0.12f;
      ema_ms = ema_ms * (1.0f - kAlpha) + frame_ms * kAlpha;
    }

    frame_stats.frame_ms = frame_ms;
    frame_stats.ema_frame_ms = ema_ms;
    frame_stats.fps = ema_ms > 0.01f ? 1000.0f / ema_ms : 0.0f;

    // Streamline 2.12 supports VSync with Frame Generation only on D3D12.
    // Vulkan FG is opt-in, and the explicit request therefore also makes the
    // mutually exclusive VSync setting visibly Off before swapchain rebuild.
    const bool vulkan_frame_generation_requested =
        backend->kind() == xpbd::gfx::BackendKind::Vulkan &&
        session.enable_ray_tracing &&
        session.path_trace_settings.requested_frame_generation ==
            xpbd::gfx::PathTraceFrameGeneration::On &&
        backend->pathTracePostProcessCapabilities()
            .dlss_frame_generation;
    if (vulkan_frame_generation_requested && session.vsync_enabled) {
      session.vsync_enabled = false;
      session.vsync_dirty = true;
      xpbd::log::info(
          "Vulkan VSync disabled for requested DLSS Frame Generation");
    }
    if (session.vsync_dirty) {
      backend->setVSync(session.vsync_enabled);
      session.vsync_dirty = false;
    }

    // NVIDIA RT: clamp persisted preference when hardware cannot do RT.
    {
      const auto rt_cap = backend->rayTracingCapability();
      const bool rt_ok =
          rt_cap.supported && rt_cap.device_extensions_enabled;
      session.enable_ray_tracing =
          xpbd::gfx::clampRayTracingPreference(session.enable_ray_tracing,
                                               rt_ok);
      session.path_trace_post_process_capabilities =
          backend->pathTracePostProcessCapabilities();
      session.path_trace_post_process_status =
          backend->pathTracePostProcessStatus();
    }


    session.debug_fps = disp_fps;
    session.debug_original_fps = disp_original_fps;
    session.debug_dlss_fg_fps = disp_dlss_fg_fps;
    session.debug_dlss_frame_generation_active =
        disp_dlss_fg_active;
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
      if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F11 &&
          !ev.key.repeat) {
        toggle_borderless_fullscreen();
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

    session.synchronizeStillRenderState();
    if (!session.stillRenderActive() &&
        session.playback_state == xpbd::app::PlaybackState::Playing &&
        !session.bake_busy.load()) {
      session.advancePreview(dt);
    }
    if (!session.stillRenderActive() && !session.bake_busy.load()) {
      session.advanceWorldSkyTime(dt);
    }
    if (latency_frame_started) {
      backend->endLatencySimulation();
    }

    const auto rt_cap_ui = backend->rayTracingCapability();
    const auto ui_result = xpbd::app::composeNuklearUi(
        &ctx, win_w, win_h, 1.0f, backend->name(), backend->deviceName(),
        frame_stats, &rt_cap_ui);
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
    const auto *still_snapshot =
        session.stillRenderActive() &&
                session.still_render_job.snapshot.has_value()
            ? &*session.still_render_job.snapshot
            : nullptr;
    const bool render_loaded_scene =
        still_snapshot != nullptr
            ? (still_snapshot->scene_selection.kind ==
                   xpbd::app::SceneSelectionKind::Loaded ||
               still_snapshot->scene_selection.kind ==
                   xpbd::app::SceneSelectionKind::UserBuilt)
            : session.sceneRendersLoadedContent();
    if (render_loaded_scene && session.camera_needs_fit &&
        !session.geometry.bones.empty()) {
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
    if (use_static_model && render_loaded_scene) {
      static_mesh_builder.setBoneMapper(&session.bone_mapper);
      static_mesh_builder.setSelectedBone(session.selected_bone_name);
      static_mesh_builder.setHoveredBone(session.hovered_bone_name);
      static_mesh_builder.setHiddenBones(&session.hidden_bone_names);
      static_mesh_builder.setTexture(model_texture);
      static_mesh_builder.setShowBones(session.show_bones);
      // Preview scenes own ground/grid; disable legacy ground plane when active.
      static_mesh_builder.setShowGround(!vulkan_raster_path &&
                                        session.show_ground);
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
    } else if (!use_static_model && render_loaded_scene) {
      xpbd::gfx::buildSessionViewportScene(
          session.geometry, session.bone_mapper, session.selected_bone_name,
          preview_animation, preview_reference_time, preview_frame != nullptr,
          preview_frame, model_texture, session.show_bones,
          session.use_mcbe_coords, scene,
          !vulkan_raster_path && session.show_ground, &session.hidden_bone_names,
          session.hovered_bone_name);
    } else if (!use_static_model) {
      scene = {};
    }

    float raster_scene_time =
          std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                       app_start_time)
              .count();
    if (still_snapshot != nullptr && still_snapshot->camera_frozen) {
      raster_scene_time = still_snapshot->raster_scene_time_seconds;
    }
    if (vulkan_raster_path) {
      const auto raster_preview_scene =
          still_snapshot != nullptr ? still_snapshot->preview_scene_id
                                    : session.preview_scene_id;
      const bool raster_show_grid =
          still_snapshot != nullptr ? still_snapshot->show_preview_grid
                                    : session.show_preview_grid;
      const bool raster_show_axes =
          still_snapshot != nullptr ? still_snapshot->show_preview_axes
                                    : session.show_preview_axes;
      const bool raster_dynamic =
          still_snapshot != nullptr ? still_snapshot->dynamic_preview_scene
                                    : session.dynamic_preview_scene;
      xpbd::gfx::buildViewportRasterScene(
          raster_preview_scene, raster_show_grid, raster_show_axes,
          raster_dynamic, raster_scene_time, raster_scene,
          preview_scene_asset_root);
      if (raster_scene.skybox.valid() &&
          raster_scene.skybox.source_identity !=
              logged_preview_skybox_source) {
        logged_preview_skybox_source =
            raster_scene.skybox.source_identity;
        if (raster_scene.skybox.cc0_asset) {
          xpbd::log::infof("Preview scene CC0 skybox loaded: %s",
                           logged_preview_skybox_source.c_str());
        } else {
          xpbd::log::warnf("Preview scene is using skybox fallback: %s",
                           logged_preview_skybox_source.c_str());
        }
      }
    }
    const auto t_mesh1 = std::chrono::steady_clock::now();
    frame_stats.mesh_ms =
        std::chrono::duration<float, std::milli>(t_mesh1 - t_mesh0).count();

    const auto framebuffer_viewport =
        xpbd::gfx::logicalViewportToFramebuffer(
            ui_result.layout.vp_x, ui_result.layout.vp_y,
            ui_result.layout.vp_w, ui_result.layout.vp_h, scale_x, scale_y,
            fb_w, fb_h);
    float view[16], proj[16];
    const float aspect =
        framebuffer_viewport.h > 0
            ? static_cast<float>(framebuffer_viewport.w) /
                  static_cast<float>(framebuffer_viewport.h)
            : 1.0f;
    session.camera.matrices(aspect, view, proj);
    if (session.freezeQueuedStillRenderCamera(
            view, proj, raster_scene_time)) {
      still_snapshot = &*session.still_render_job.snapshot;
    }
    const float *render_view =
        still_snapshot != nullptr && still_snapshot->camera_frozen
            ? still_snapshot->view_matrix.data()
            : view;
    const float *render_proj =
        still_snapshot != nullptr && still_snapshot->camera_frozen
            ? still_snapshot->proj_matrix.data()
            : proj;



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
    frame.viewport = framebuffer_viewport;
    frame.view_matrix = render_view;
    frame.proj_matrix = render_proj;
    frame.scene = render_loaded_scene
                      ? (use_static_model ? &static_model_frame.overlays
                                          : &scene)
                      : nullptr;
    if (use_static_model && render_loaded_scene) {
      frame.static_model = &static_model;
      frame.static_model_frame = &static_model_frame;
      frame.static_model_texture = model_texture;
      frame.static_model_material =
          session.resolved_material.valid() ? &session.resolved_material
                                            : nullptr;
      frame.static_model_generation =
          still_snapshot != nullptr ? still_snapshot->model_generation
                                    : session.modelGeneration();
      frame.static_texture_generation =
          still_snapshot != nullptr ? still_snapshot->material_generation
                                    : session.materialGeneration();
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
    frame.material_debug_view = session.labpbr_debug_view;
    frame.rt_debug_view = session.rt_debug_view;
    frame.path_trace_settings = session.path_trace_settings;
    frame.world_environment =
        still_snapshot != nullptr ? &still_snapshot->world_environment
                                  : &session.world_environment;
    xpbd::gfx::StillRenderFrameRequest still_request{};
    if (still_snapshot != nullptr && still_snapshot->camera_frozen) {
      const auto &job = session.still_render_job;
      still_request.job_id = job.status.job_id;
      still_request.width = job.settings.width;
      still_request.height = job.settings.height;
      still_request.target_samples = job.settings.target_samples;
      still_request.samples_per_submit =
          job.settings.samples_per_submit;
      still_request.format = job.settings.format;
      still_request.transparent_background =
          job.settings.transparent_background;
      still_request.cancel_requested = job.cancel_requested;
      still_request.output_path = job.status.output_path;
      still_request.view_matrix = still_snapshot->view_matrix.data();
      still_request.proj_matrix = still_snapshot->proj_matrix.data();
      still_request.path_trace_settings =
          still_snapshot->path_trace_settings;
      still_request.material_debug_view =
          still_snapshot->material_debug_view;
      still_request.rt_debug_view = still_snapshot->rt_debug_view;
      still_request.status = &session.still_render_job.status;
      frame.still_render = &still_request;
    }
    frame.ui = &ui_draw;
    // Prefer advanced lighting only when enabled and no modal dialog is open.
    frame.prefer_ray_tracing =
        session.enable_ray_tracing && !xpbd::app::nativeDialogOpen() &&
        !backend->presentationSuspended();
    if (vulkan_raster_path) {
      frame.raster_scene = &raster_scene;
      frame.clear_r = raster_scene.lighting.clear_r;
      frame.clear_g = raster_scene.lighting.clear_g;
      frame.clear_b = raster_scene.lighting.clear_b;
    } else {
      frame.clear_r = 26.0f / 255.0f;
      frame.clear_g = 28.0f / 255.0f;
      frame.clear_b = 34.0f / 255.0f;
    }

    backend->render(frame);
    if (completion_diagnostic_frames > 0) {
      --completion_diagnostic_frames;
    }
    ++render_frame_number;
    if (unattended_resize_gate) {
      auto resize_window = [&](int width, int height) {
        if (!SDL_SetWindowSize(window, width, height)) {
          xpbd::log::warnf(
              "Unattended preview resize failed at frame %llu: %s",
              static_cast<unsigned long long>(render_frame_number),
              SDL_GetError());
        } else {
          xpbd::log::infof(
              "Unattended preview resize: frame=%llu window=%dx%d",
              static_cast<unsigned long long>(render_frame_number), width,
              height);
        }
      };
      if (render_frame_number == 120u) {
        resize_window(1000, 700);
      } else if (render_frame_number == 260u) {
        resize_window(1400, 900);
      } else if (render_frame_number == 400u) {
        resize_window(
            (std::max)(unattended_initial_width, 1),
            (std::max)(unattended_initial_height, 1));
      } else if (render_frame_number == 520u) {
        if (SDL_MinimizeWindow(window)) {
          xpbd::log::info(
              "Unattended preview resize: window minimized");
        }
      } else if (render_frame_number == 560u) {
        if (SDL_RestoreWindow(window)) {
          xpbd::log::info(
              "Unattended preview resize: window restored");
        }
      }
    }
    if (unattended_frame_limit > 0 &&
        render_frame_number >= unattended_frame_limit) {
      xpbd::log::infof(
          "Unattended frame limit reached after %llu frames",
          static_cast<unsigned long long>(render_frame_number));
      running = false;
    }
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
    frame_stats.active_render_path = backend_stats.active_render_path;
    frame_stats.ray_tracing_supported = backend_stats.ray_tracing_supported;
    frame_stats.ray_tracing_requested = backend_stats.ray_tracing_requested;
    frame_stats.dlss_frame_generation_supported =
        backend_stats.dlss_frame_generation_supported;
    frame_stats.dlss_frame_generation_requested =
        backend_stats.dlss_frame_generation_requested;
    frame_stats.dlss_frame_generation_active =
        backend_stats.dlss_frame_generation_active;
    frame_stats.reflex_supported = backend_stats.reflex_supported;
    frame_stats.dlss_frames_actually_presented =
        backend_stats.dlss_frames_actually_presented;
    frame_stats.rt_blas_count = backend_stats.rt_blas_count;
    frame_stats.rt_tlas_count = backend_stats.rt_tlas_count;
    frame_stats.rt_instance_count = backend_stats.rt_instance_count;
    frame_stats.rt_primitive_count = backend_stats.rt_primitive_count;
    frame_stats.rt_as_storage_bytes = backend_stats.rt_as_storage_bytes;
    frame_stats.rt_scratch_bytes = backend_stats.rt_scratch_bytes;
    frame_stats.rt_attribute_bytes = backend_stats.rt_attribute_bytes;
    frame_stats.rt_allocated_bytes = backend_stats.rt_allocated_bytes;
    frame_stats.rt_full_builds = backend_stats.rt_full_builds;
    frame_stats.rt_refits = backend_stats.rt_refits;
    frame_stats.rt_last_build_reason = backend_stats.rt_last_build_reason;




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

        disp_original_fps = disp_fps;
        disp_dlss_fg_active =
            frame_stats.dlss_frame_generation_active;
        disp_dlss_fg_fps =
            disp_dlss_fg_active
                ? disp_original_fps * static_cast<float>(
                      (std::max)(
                          1u,
                          frame_stats.dlss_frames_actually_presented))
                : 0.0f;

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
        if (disp_dlss_fg_active) {
          std::snprintf(
              title, sizeof(title),
              "XPBD Bone Baker | %s | Original %.0f / DLSS-FG %.0f FPS | "
              "%.1f ms | cubes %d%s",
              backend->name(),
              static_cast<double>(disp_original_fps),
              static_cast<double>(disp_dlss_fg_fps),
              disp_frame_ms, disp_cubes,
              session.vsync_enabled ? "" : " | VSync off");
        } else {
          std::snprintf(
              title, sizeof(title),
              "XPBD Bone Baker | %s | Original %.0f FPS | %.1f ms | "
              "cubes %d%s",
              backend->name(),
              static_cast<double>(disp_original_fps),
              disp_frame_ms, disp_cubes,
              session.vsync_enabled ? "" : " | VSync off");
        }
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
#if defined(_WIN32)
  SDL_SetWindowsMessageHook(nullptr, nullptr);
#endif
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
// GUI subsystem entry: let SDL parse the process command line so flags like
// -vk reach parseBackendRequest. Relying on __argc/__argv alone
// is brittle for WIN32_EXECUTABLE targets launched via start/explorer.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return SDL_RunApp(0, nullptr, app_main, nullptr);
}
#else
int main(int argc, char **argv) { return app_main(argc, argv); }
#endif
