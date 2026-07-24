#include "gl_loader.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/log.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <SDL3/SDL.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_BOOL
#include "nuklear.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace xpbd::gfx {
namespace {

using Clock = std::chrono::steady_clock;

struct NkVertex {
  float pos[2];
  float uv[2];
  nk_byte col[4];
};


constexpr const char *kUiVert = R"GLSL(
#version 330 core
layout(location=0) in vec2 Position;
layout(location=1) in vec2 TexCoord;
layout(location=2) in vec4 Color;
uniform mat4 ProjMtx;
out vec2 Frag_UV;
out vec4 Frag_Color;
void main(){
  Frag_UV = TexCoord;
  Frag_Color = Color;
  gl_Position = ProjMtx * vec4(Position.xy, 0, 1);
}
)GLSL";



constexpr const char *kUiFrag = R"GLSL(
#version 330 core
uniform sampler2D Texture;
in vec2 Frag_UV;
in vec4 Frag_Color;
out vec4 Out_Color;
void main(){
  float a = texture(Texture, Frag_UV).r;
  Out_Color = vec4(Frag_Color.rgb, Frag_Color.a * a);
}
)GLSL";

constexpr const char *kMeshVert = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec4 aCol;
uniform mat4 uMVP;
out vec4 vCol;
out vec3 vNrm;
void main(){
  vCol = aCol;
  vNrm = aNrm;
  gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

constexpr const char *kMeshFrag = R"GLSL(
#version 330 core
in vec4 vCol;
in vec3 vNrm;
out vec4 FragColor;
void main(){
  // 完全透明的片元直接丢弃；半透明片元交给混合阶段处理。
  if (vCol.a < 0.02) discard;
  float nd = abs(dot(normalize(vNrm), normalize(vec3(0.35, 0.85, 0.40))));
  float shade = 0.92 + 0.08 * nd;
  FragColor = vec4(vCol.rgb * shade, vCol.a);
}
)GLSL";

GLuint compile(GLenum type, const char *src) {
  GLuint s = gl::CreateShader(type);
  gl::ShaderSource(s, 1, &src, nullptr);
  gl::CompileShader(s);
  GLint ok = 0;
  gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    gl::GetShaderInfoLog(s, 1024, nullptr, log);
    SDL_Log("shader error: %s", log);
    gl::DeleteShader(s);
    return 0;
  }
  return s;
}

GLuint link(const char *vs, const char *fs) {
  GLuint v = compile(GL_VERTEX_SHADER, vs);
  GLuint f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) {
    if (v)
      gl::DeleteShader(v);
    if (f)
      gl::DeleteShader(f);
    return 0;
  }
  GLuint p = gl::CreateProgram();
  gl::AttachShader(p, v);
  gl::AttachShader(p, f);
  gl::LinkProgram(p);
  gl::DeleteShader(v);
  gl::DeleteShader(f);
  GLint ok = 0;
  gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    gl::GetProgramInfoLog(p, 1024, nullptr, log);
    SDL_Log("link error: %s", log);
    gl::DeleteProgram(p);
    return 0;
  }
  return p;
}

void mul(const float *a, const float *b, float *o) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                     a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

void upload(GLuint buf, GLenum target, GLsizeiptr n, const void *data,
            GLsizeiptr &cap) {
  gl::BindBuffer(target, buf);
  if (n > cap) {
    cap = n + n / 2 + 4096;
    gl::BufferData(target, cap, nullptr, GL_STREAM_DRAW);
  }
  if (n > 0 && data) {
    gl::BufferSubData(target, 0, n, data);
  }
}



void makeUiProj(float width, float height, float out[16]) {

  std::memset(out, 0, 16 * sizeof(float));
  const float L = 0.0f, R = width, T = 0.0f, B = height;
  out[0] = 2.0f / (R - L);
  out[5] = 2.0f / (T - B);
  out[10] = -1.0f;
  out[12] = (R + L) / (L - R);
  out[13] = (T + B) / (B - T);
  out[15] = 1.0f;
}

class Gl33Backend final : public IGpuBackend {
public:
  bool init(SDL_Window *window) override {
    window_ = window;
    gl_ctx_ = SDL_GL_CreateContext(window);
    if (!gl_ctx_ || !SDL_GL_MakeCurrent(window, gl_ctx_)) {
      SDL_Log("GL context failed: %s", SDL_GetError());
      return false;
    }
    vsync_ = true;
    SDL_GL_SetSwapInterval(1);

    auto gp = [](const char *n) -> void * {
      return reinterpret_cast<void *>(SDL_GL_GetProcAddress(n));
    };
    if (!gl::loadGlProcs(gp)) {
      SDL_Log("GL load procs failed");
      return false;
    }

    const char *ren =
        reinterpret_cast<const char *>(gl::GetString(GL_RENDERER));
    device_name_ = ren ? ren : "OpenGL";

    prog_ui_ = link(kUiVert, kUiFrag);
    prog_mesh_ = link(kMeshVert, kMeshFrag);
    if (!prog_ui_ || !prog_mesh_) {
      return false;
    }
    loc_ui_proj_ = gl::GetUniformLocation(prog_ui_, "ProjMtx");
    loc_ui_tex_ = gl::GetUniformLocation(prog_ui_, "Texture");
    loc_mesh_mvp_ = gl::GetUniformLocation(prog_mesh_, "uMVP");

    gl::GenTextures(1, &font_tex_);
    gl::GenVertexArrays(1, &vao_ui_);
    gl::GenBuffers(1, &vbo_ui_);
    gl::GenBuffers(1, &ebo_ui_);
    gl::GenVertexArrays(1, &vao_mesh_);
    gl::GenBuffers(1, &vbo_mesh_);

    gl::BindVertexArray(vao_ui_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_ui_);
    gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_ui_);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(NkVertex),
                            reinterpret_cast<void *>(offsetof(NkVertex, pos)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(NkVertex),
                            reinterpret_cast<void *>(offsetof(NkVertex, uv)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(NkVertex),
                            reinterpret_cast<void *>(offsetof(NkVertex, col)));

    gl::BindVertexArray(vao_mesh_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_mesh_);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                            reinterpret_cast<void *>(offsetof(MeshVertex, px)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                            reinterpret_cast<void *>(offsetof(MeshVertex, nx)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                            reinterpret_cast<void *>(offsetof(MeshVertex, r)));
    gl::BindVertexArray(0);


    gl::Enable(GL_DEPTH_TEST);
    gl::Disable(GL_CULL_FACE);

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    resize(w, h);

    xpbd::log::infof("OpenGL init: renderer=%s prog_ui=%u prog_mesh=%u font=%u",
                     device_name_.c_str(), prog_ui_, prog_mesh_, font_tex_);
    return true;
  }

  void shutdown() override {
    auto dt = [&](GLuint &t) {
      if (t) {
        gl::DeleteTextures(1, &t);
        t = 0;
      }
    };
    auto db = [&](GLuint &b) {
      if (b) {
        gl::DeleteBuffers(1, &b);
        b = 0;
      }
    };
    auto dv = [&](GLuint &v) {
      if (v) {
        gl::DeleteVertexArrays(1, &v);
        v = 0;
      }
    };
    auto dp = [&](GLuint &p) {
      if (p) {
        gl::DeleteProgram(p);
        p = 0;
      }
    };
    dt(font_tex_);
    db(vbo_ui_);
    db(ebo_ui_);
    dv(vao_ui_);
    db(vbo_mesh_);
    dv(vao_mesh_);
    dp(prog_ui_);
    dp(prog_mesh_);
    if (gl_ctx_) {
      SDL_GL_DestroyContext(gl_ctx_);
      gl_ctx_ = nullptr;
    }
  }

  void resize(int w, int h) override {
    fb_w_ = (std::max)(1, w);
    fb_h_ = (std::max)(1, h);
  }

  bool uploadFontAtlas(const void *pixels, int width, int height,
                       bool is_rgba) override {
    if (!pixels || width <= 0 || height <= 0) {
      return false;
    }
    SDL_GL_MakeCurrent(window_, gl_ctx_);
    gl::BindTexture(GL_TEXTURE_2D, font_tex_);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (is_rgba) {
      gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
    } else {

      gl::TexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
                     GL_UNSIGNED_BYTE, pixels);
    }
    xpbd::log::infof("font atlas %dx%d %s id=%u", width, height,
                     is_rgba ? "RGBA" : "ALPHA8", font_tex_);
    return true;
  }

  unsigned int fontTextureId() const override { return font_tex_; }

  void render(const FrameInput &frame) override {
    const auto t0 = Clock::now();
    int draws = 0;
    SDL_GL_MakeCurrent(window_, gl_ctx_);

    const int fbw = (std::max)(1, frame.fb_width);
    const int fbh = (std::max)(1, frame.fb_height);

    gl::Viewport(0, 0, fbw, fbh);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_TRUE);
    gl::ClearColor(frame.clear_r, frame.clear_g, frame.clear_b, 1.0f);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


    if (frame.ui) {
      drawUi(*frame.ui, draws);
    }



    if (frame.viewport.w > 1 && frame.viewport.h > 1 && frame.view_matrix &&
        frame.proj_matrix && frame.scene) {
      const int vx = frame.viewport.x;
      const int vy = fbh - (frame.viewport.y + frame.viewport.h);
      const int vw = frame.viewport.w;
      const int vh = frame.viewport.h;

      gl::Enable(GL_SCISSOR_TEST);
      gl::Scissor(vx, vy, vw, vh);
      gl::Viewport(vx, vy, vw, vh);
      gl::Enable(GL_DEPTH_TEST);
      gl::DepthMask(GL_TRUE);
      gl::ClearColor(30.0f / 255.0f, 30.0f / 255.0f, 40.0f / 255.0f, 1.0f);
      gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      float mvp[16];
      mul(frame.proj_matrix, frame.view_matrix, mvp);
      gl::UseProgram(prog_mesh_);
      gl::UniformMatrix4fv(loc_mesh_mvp_, 1, GL_FALSE, mvp);
      gl::Disable(GL_CULL_FACE);
      gl::BindVertexArray(vao_mesh_);


      gl::Disable(GL_BLEND);
      gl::DepthMask(GL_TRUE);
      if (!frame.scene->solid.empty()) {
        const auto bytes = static_cast<GLsizeiptr>(frame.scene->solid.size() *
                                                   sizeof(MeshVertex));
        upload(vbo_mesh_, GL_ARRAY_BUFFER, bytes, frame.scene->solid.data(),
               mesh_cap_);
        gl::DrawArrays(GL_TRIANGLES, 0,
                       static_cast<GLsizei>(frame.scene->solid.size()));
        ++draws;
      }

      if (!frame.scene->transparent.empty()) {
        gl::Enable(GL_BLEND);
        gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl::DepthMask(GL_FALSE);
        const auto bytes = static_cast<GLsizeiptr>(
            frame.scene->transparent.size() * sizeof(MeshVertex));
        upload(vbo_mesh_, GL_ARRAY_BUFFER, bytes,
               frame.scene->transparent.data(), mesh_cap_);
        gl::DrawArrays(GL_TRIANGLES, 0,
                       static_cast<GLsizei>(frame.scene->transparent.size()));
        ++draws;
        gl::DepthMask(GL_TRUE);
        gl::Disable(GL_BLEND);
      }
      if (!frame.scene->lines.empty()) {
        gl::Disable(GL_BLEND);
        gl::DepthMask(GL_TRUE);
        const auto bytes = static_cast<GLsizeiptr>(frame.scene->lines.size() *
                                                   sizeof(MeshVertex));
        upload(vbo_mesh_, GL_ARRAY_BUFFER, bytes, frame.scene->lines.data(),
               mesh_cap_);
        gl::LineWidth(1.5f);
        gl::DrawArrays(GL_LINES, 0,
                       static_cast<GLsizei>(frame.scene->lines.size()));
        ++draws;
      }
      gl::BindVertexArray(0);
      gl::Disable(GL_SCISSOR_TEST);
      gl::Viewport(0, 0, fbw, fbh);
    }

    stats_.backend_cpu_ms =
        std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
    stats_.gpu_ms = 0.0f;
    stats_.draw_calls = draws;
    if (frame.scene) {
      stats_.cube_count = frame.scene->cube_count;
      stats_.line_count = frame.scene->line_segment_count;
    }
    SDL_GL_SwapWindow(window_);
  }

  void drawUi(const UiDrawData &ui, int &draws) {
    if (!ui.ctx || !ui.cmds || !ui.vertices || !ui.indices) {
      return;
    }


    const nk_size vsize = ui.vertices->allocated > 0
                              ? ui.vertices->allocated
                              : nk_buffer_total(ui.vertices);
    const nk_size esize = ui.indices->allocated > 0
                              ? ui.indices->allocated
                              : nk_buffer_total(ui.indices);
    if (vsize == 0 || esize == 0) {
      static bool once = false;
      if (!once) {
        xpbd::log::warnf("nk empty used_v=%zu used_e=%zu cap_v=%zu cap_e=%zu",
                         (size_t)ui.vertices->allocated,
                         (size_t)ui.indices->allocated,
                         (size_t)nk_buffer_total(ui.vertices),
                         (size_t)nk_buffer_total(ui.indices));
        once = true;
      }
      return;
    }

    const int lw = (std::max)(1, ui.logical_w);
    const int lh = (std::max)(1, ui.logical_h);
    const int fbw = (std::max)(1, ui.fb_w);
    const int fbh = (std::max)(1, ui.fb_h);
    const float scale_x = static_cast<float>(fbw) / static_cast<float>(lw);
    const float scale_y = static_cast<float>(fbh) / static_cast<float>(lh);

    float proj[16];

    makeUiProj(static_cast<float>(lw), static_cast<float>(lh), proj);

    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (gl::BlendEquation) {
      gl::BlendEquation(GL_FUNC_ADD);
    }
    gl::Disable(GL_CULL_FACE);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Enable(GL_SCISSOR_TEST);
    gl::Viewport(0, 0, fbw, fbh);

    gl::Scissor(0, 0, fbw, fbh);

    gl::UseProgram(prog_ui_);
    gl::UniformMatrix4fv(loc_ui_proj_, 1, GL_FALSE, proj);
    gl::Uniform1i(loc_ui_tex_, 0);

    gl::BindVertexArray(vao_ui_);
    upload(vbo_ui_, GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vsize),
           nk_buffer_memory_const(ui.vertices), ui_v_cap_);
    gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_ui_);
    upload(ebo_ui_, GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(esize),
           nk_buffer_memory_const(ui.indices), ui_e_cap_);

    const nk_draw_command *cmd = nullptr;
    nk_size off = 0;
    int cmd_count = 0;
    nk_draw_foreach(cmd, ui.ctx, ui.cmds) {
      if (!cmd || cmd->elem_count == 0) {
        continue;
      }
      ++cmd_count;
      GLuint tex = static_cast<GLuint>(cmd->texture.id);
      if (tex == 0) {
        tex = font_tex_;
      }
      gl::ActiveTexture(GL_TEXTURE0);
      gl::BindTexture(GL_TEXTURE_2D, tex);

      const float x = cmd->clip_rect.x * scale_x;
      const float y = cmd->clip_rect.y * scale_y;
      const float w = cmd->clip_rect.w * scale_x;
      const float h = cmd->clip_rect.h * scale_y;
      gl::Scissor((GLint)x, (GLint)((float)fbh - (y + h)),
                  (GLsizei)(std::max)(w, 0.0f), (GLsizei)(std::max)(h, 0.0f));

      gl::DrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count,
                       GL_UNSIGNED_SHORT, (const void *)(uintptr_t)off);
      off += (nk_size)cmd->elem_count * sizeof(nk_draw_index);
      ++draws;
    }

    static bool logged = false;
    if (!logged) {
      xpbd::log::infof("UI draw: cmds=%d verts_bytes=%zu idx_bytes=%zu "
                       "logical=%dx%d fb=%dx%d",
                       cmd_count, (size_t)vsize, (size_t)esize, lw, lh, fbw,
                       fbh);
      logged = true;
    }

    gl::BindVertexArray(0);
    gl::Disable(GL_SCISSOR_TEST);
  }

  void setVSync(bool enabled) override {
    vsync_ = enabled;
    if (gl_ctx_) {
      SDL_GL_SetSwapInterval(enabled ? 1 : 0);
    }
  }
  bool vsyncEnabled() const override { return vsync_; }
  BackendKind kind() const override { return BackendKind::OpenGL33; }
  const char *name() const override { return "OpenGL 3.3"; }
  const char *deviceName() const override { return device_name_.c_str(); }
  FrameStats stats() const override { return stats_; }

private:
  SDL_Window *window_ = nullptr;
  SDL_GLContext gl_ctx_ = nullptr;
  std::string device_name_;
  bool vsync_ = true;
  FrameStats stats_{};
  GLuint prog_ui_ = 0, prog_mesh_ = 0;
  GLint loc_ui_proj_ = -1, loc_ui_tex_ = -1, loc_mesh_mvp_ = -1;
  GLuint font_tex_ = 0;
  GLuint vao_ui_ = 0, vbo_ui_ = 0, ebo_ui_ = 0;
  GLuint vao_mesh_ = 0, vbo_mesh_ = 0;
  int fb_w_ = 1, fb_h_ = 1;
  GLsizeiptr mesh_cap_ = 0, ui_v_cap_ = 0, ui_e_cap_ = 0;
};

}

std::unique_ptr<IGpuBackend> createOpenGLBackend() {
  return std::make_unique<Gl33Backend>();
}

}
