

#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/log.hpp"

#if !defined(_WIN32)

namespace xpbd::gfx {
std::unique_ptr<IGpuBackend> createDx11Backend() { return nullptr; }
}

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace xpbd::gfx {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

struct NkVertex {
  float pos[2];
  float uv[2];
  nk_byte col[4];
};

struct MeshCB {
  float mvp[16];
};

struct UiCB {
  float proj[16];
};

const char *kUiVs = R"(
cbuffer UiCB : register(b0) { float4x4 Proj; };
struct VSIn { float2 pos:POSITION; float2 uv:TEXCOORD0; float4 col:COLOR0; };
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 col:COLOR0; };
VSOut main(VSIn i) {
  VSOut o;
  o.pos = mul(Proj, float4(i.pos, 0, 1));
  o.uv = i.uv;
  o.col = i.col;
  return o;
}
)";

const char *kUiPs = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
struct PSIn { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 col:COLOR0; };
float4 main(PSIn i) : SV_Target {
  float a = tex0.Sample(samp0, i.uv).r;
  return float4(i.col.rgb, i.col.a * a);
}
)";

const char *kMeshVs = R"(
cbuffer MeshCB : register(b0) { float4x4 MVP; };
struct VSIn { float3 pos:POSITION; float3 nrm:NORMAL; float4 col:COLOR0; };
struct VSOut { float4 pos:SV_POSITION; float3 nrm:NORMAL; float4 col:COLOR0; };
VSOut main(VSIn i) {
  VSOut o;
  o.pos = mul(MVP, float4(i.pos, 1));
  o.nrm = i.nrm;
  o.col = i.col;
  return o;
}
)";

const char *kMeshPs = R"(
struct PSIn { float4 pos:SV_POSITION; float3 nrm:NORMAL; float4 col:COLOR0; };
float4 main(PSIn i) : SV_Target {
  if (i.col.a < 0.02) discard;
  float3 n = normalize(i.nrm);
  float nd = abs(dot(n, normalize(float3(0.35,0.85,0.4))));
  float shade = 0.92 + 0.08 * nd;
  return float4(i.col.rgb * shade, i.col.a);
}
)";

void mulMat(const float *a, const float *b, float *o) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                     a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}







void glMvpToD3d(const float *m, float *o) {
  for (int c = 0; c < 4; ++c) {
    const float x = m[c * 4 + 0];
    const float y = m[c * 4 + 1];
    const float z = m[c * 4 + 2];
    const float w = m[c * 4 + 3];
    o[c * 4 + 0] = x;
    o[c * 4 + 1] = y;
    o[c * 4 + 2] = 0.5f * z + 0.5f * w;
    o[c * 4 + 3] = w;
  }
}

void makeUiProj(float w, float h, float out[16]) {


  std::memset(out, 0, 16 * sizeof(float));
  const float L = 0.0f, R = w, T = 0.0f, B = h;
  out[0] = 2.0f / (R - L);
  out[5] = 2.0f / (T - B);
  out[10] = -1.0f;
  out[12] = (R + L) / (L - R);
  out[13] = (T + B) / (B - T);
  out[15] = 1.0f;
}

class Dx11Backend final : public IGpuBackend {
public:
  bool init(SDL_Window *window) override {
    window_ = window;
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    hwnd_ = static_cast<HWND>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd_) {
      SDL_Log("DX11: no HWND from SDL");
      return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd_;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;


    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
                DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    allow_tearing_ = true;

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl_out{};
    const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0,
                                     D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 2,
        D3D11_SDK_VERSION, &scd, &swap_, &dev_, &fl_out, &ctx_);
    if (FAILED(hr)) {

      scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
      allow_tearing_ = false;
      hr = D3D11CreateDeviceAndSwapChain(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 2,
          D3D11_SDK_VERSION, &scd, &swap_, &dev_, &fl_out, &ctx_);
    }
    if (FAILED(hr)) {

      scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      scd.Flags = 0;
      allow_tearing_ = false;
      hr = D3D11CreateDeviceAndSwapChain(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 2,
          D3D11_SDK_VERSION, &scd, &swap_, &dev_, &fl_out, &ctx_);
    }
    if (FAILED(hr)) {
      xpbd::log::errorf("D3D11CreateDeviceAndSwapChain failed: 0x%08lx",
                        static_cast<unsigned long>(hr));
      return false;
    }

    if (!createTargets()) {
      return false;
    }
    if (!createShaders()) {
      return false;
    }
    if (!createStates()) {
      return false;
    }

    DXGI_ADAPTER_DESC ad{};
    ComPtr<IDXGIDevice> dxgi_dev;
    if (SUCCEEDED(dev_.As(&dxgi_dev))) {
      ComPtr<IDXGIAdapter> adp;
      if (SUCCEEDED(dxgi_dev->GetAdapter(&adp)) &&
          SUCCEEDED(adp->GetDesc(&ad))) {
        char name[128]{};
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, 128, nullptr,
                            nullptr);
        device_name_ = name;
      }
    }
    xpbd::log::infof("DX11 init: %s", device_name_.c_str());
    return true;
  }

  void shutdown() override {
    font_srv_.Reset();
    font_tex_.Reset();
    rtv_.Reset();
    dsv_.Reset();
    depth_.Reset();
    swap_.Reset();
    ctx_.Reset();
    dev_.Reset();
  }

  void resize(int w, int h) override {
    fb_w_ = (std::max)(1, w);
    fb_h_ = (std::max)(1, h);
    if (!swap_ || !ctx_) {
      return;
    }
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    dsv_.Reset();
    depth_.Reset();
    UINT resize_flags = 0;
    if (allow_tearing_) {
      resize_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    HRESULT hr = swap_->ResizeBuffers(0, fb_w_, fb_h_, DXGI_FORMAT_UNKNOWN,
                                      resize_flags);
    if (FAILED(hr)) {
      SDL_Log("DX11 ResizeBuffers failed");
      return;
    }
    createTargets();
  }

  bool uploadFontAtlas(const void *pixels, int width, int height,
                       bool is_rgba) override {
    if (!pixels || width <= 0 || height <= 0 || !dev_) {
      return false;
    }
    std::vector<uint8_t> rgba;
    const void *data = pixels;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8_UNORM;
    UINT pitch = static_cast<UINT>(width);
    if (is_rgba) {
      fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
      pitch = static_cast<UINT>(width * 4);
    } else {


    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data;
    sd.SysMemPitch = pitch;

    font_srv_.Reset();
    font_tex_.Reset();
    HRESULT hr = dev_->CreateTexture2D(&td, &sd, &font_tex_);
    if (FAILED(hr)) {
      return false;
    }
    hr = dev_->CreateShaderResourceView(font_tex_.Get(), nullptr, &font_srv_);
    return SUCCEEDED(hr);
  }

  unsigned int fontTextureId() const override { return 1; }

  void render(const FrameInput &frame) override {
    if (!ctx_ || !rtv_) {
      return;
    }
    const auto t0 = Clock::now();
    int draws = 0;

    const float clear[4] = {frame.clear_r, frame.clear_g, frame.clear_b, 1.0f};
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), dsv_.Get());
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);
    ctx_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    D3D11_VIEWPORT full{};
    full.Width = static_cast<float>((std::max)(1, frame.fb_width));
    full.Height = static_cast<float>((std::max)(1, frame.fb_height));
    full.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &full);


    if (frame.ui) {
      draws += drawUi(*frame.ui, false);
    }

    bool drew_viewport = false;
    if (frame.viewport.w > 1 && frame.viewport.h > 1 && frame.view_matrix &&
        frame.proj_matrix && frame.scene) {
      drew_viewport = true;
      draws += drawMesh(frame);
    }
    if (drew_viewport && frame.ui && frame.ui->overlay_visible) {
      draws += drawUi(*frame.ui, true);
    }


    stats_.backend_cpu_ms =
        std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
    stats_.gpu_ms = 0.0f;
    stats_.draw_calls = draws;
    if (frame.scene) {
      stats_.cube_count = frame.scene->cube_count;
      stats_.line_count = frame.scene->line_segment_count;
    }


    UINT sync = vsync_ ? 1u : 0u;
    UINT flags = 0;
#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200u
#endif
    if (!vsync_ && allow_tearing_) {
      flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    HRESULT pr = swap_->Present(sync, flags);
    if (FAILED(pr) && flags != 0) {

      allow_tearing_ = false;
      swap_->Present(0, 0);
    }
  }

  void setVSync(bool enabled) override { vsync_ = enabled; }
  bool vsyncEnabled() const override { return vsync_; }
  BackendKind kind() const override { return BackendKind::Dx11; }
  const char *name() const override { return "Direct3D 11"; }
  const char *deviceName() const override { return device_name_.c_str(); }
  FrameStats stats() const override { return stats_; }

private:
  bool createTargets() {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) {
      return false;
    }
    if (FAILED(dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv_))) {
      return false;
    }
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    fb_w_ = static_cast<int>(bd.Width);
    fb_h_ = static_cast<int>(bd.Height);

    D3D11_TEXTURE2D_DESC dd = bd;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev_->CreateTexture2D(&dd, nullptr, &depth_))) {
      return false;
    }
    return SUCCEEDED(
        dev_->CreateDepthStencilView(depth_.Get(), nullptr, &dsv_));
  }

  bool compile(const char *src, const char *entry, const char *target,
               ID3DBlob **blob) {
    ComPtr<ID3DBlob> err;
    HRESULT hr =
        D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry,
                   target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
    if (FAILED(hr)) {
      if (err) {
        SDL_Log("D3DCompile: %s", static_cast<char *>(err->GetBufferPointer()));
      }
      return false;
    }
    return true;
  }

  bool createShaders() {
    ComPtr<ID3DBlob> vs, ps;
    if (!compile(kUiVs, "main", "vs_4_0", &vs) ||
        !compile(kUiPs, "main", "ps_4_0", &ps)) {
      return false;
    }
    dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                             nullptr, &ui_vs_);
    dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                            nullptr, &ui_ps_);

    D3D11_INPUT_ELEMENT_DESC ui_il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev_->CreateInputLayout(ui_il, 3, vs->GetBufferPointer(),
                            vs->GetBufferSize(), &ui_layout_);

    if (!compile(kMeshVs, "main", "vs_4_0", &vs) ||
        !compile(kMeshPs, "main", "ps_4_0", &ps)) {
      return false;
    }
    dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                             nullptr, &mesh_vs_);
    dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                            nullptr, &mesh_ps_);
    D3D11_INPUT_ELEMENT_DESC mesh_il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev_->CreateInputLayout(mesh_il, 3, vs->GetBufferPointer(),
                            vs->GetBufferSize(), &mesh_layout_);

    D3D11_BUFFER_DESC cbd{};
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.ByteWidth = sizeof(UiCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev_->CreateBuffer(&cbd, nullptr, &ui_cb_);
    cbd.ByteWidth = sizeof(MeshCB);
    dev_->CreateBuffer(&cbd, nullptr, &mesh_cb_);
    return true;
  }

  bool createStates() {
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&bd, &blend_);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &rs_scissor_);
    rd.ScissorEnable = FALSE;
    dev_->CreateRasterizerState(&rd, &rs_full_);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev_->CreateDepthStencilState(&dd, &ds_ui_);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dev_->CreateDepthStencilState(&dd, &ds_mesh_);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev_->CreateDepthStencilState(&dd, &ds_mesh_no_write_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev_->CreateSamplerState(&sd, &samp_);
    return true;
  }

  void ensureVb(ComPtr<ID3D11Buffer> &buf, UINT &cap, UINT bytes,
                const void *data) {
    if (!buf || cap < bytes) {
      cap = bytes + bytes / 2 + 4096;
      D3D11_BUFFER_DESC d{};
      d.ByteWidth = cap;
      d.Usage = D3D11_USAGE_DYNAMIC;
      d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      buf.Reset();
      dev_->CreateBuffer(&d, nullptr, &buf);
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(ctx_->Map(buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
      std::memcpy(map.pData, data, bytes);
      ctx_->Unmap(buf.Get(), 0);
    }
  }

  void ensureIb(UINT bytes, const void *data) {
    if (!ib_ || ib_cap_ < bytes) {
      ib_cap_ = bytes + bytes / 2 + 4096;
      D3D11_BUFFER_DESC d{};
      d.ByteWidth = ib_cap_;
      d.Usage = D3D11_USAGE_DYNAMIC;
      d.BindFlags = D3D11_BIND_INDEX_BUFFER;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ib_.Reset();
      dev_->CreateBuffer(&d, nullptr, &ib_);
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(ctx_->Map(ib_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
      std::memcpy(map.pData, data, bytes);
      ctx_->Unmap(ib_.Get(), 0);
    }
  }

  int drawUi(const UiDrawData &ui, bool overlay_only) {
    if (!ui.ctx || !ui.cmds || !ui.vertices || !ui.indices || !font_srv_) {
      return 0;
    }
    const nk_size vsize = ui.vertices->allocated > 0
                              ? ui.vertices->allocated
                              : nk_buffer_total(ui.vertices);
    const nk_size esize = ui.indices->allocated > 0
                              ? ui.indices->allocated
                              : nk_buffer_total(ui.indices);
    if (vsize == 0 || esize == 0) {
      return 0;
    }

    if (!overlay_only) {
      ensureVb(ui_vb_, ui_vb_cap_, static_cast<UINT>(vsize),
               nk_buffer_memory_const(ui.vertices));
      ensureIb(static_cast<UINT>(esize), nk_buffer_memory_const(ui.indices));
    }

    if (!overlay_only) {
      float proj[16];
      makeUiProj(static_cast<float>((std::max)(1, ui.logical_w)),
                 static_cast<float>((std::max)(1, ui.logical_h)), proj);
      D3D11_MAPPED_SUBRESOURCE map{};
      if (SUCCEEDED(
              ctx_->Map(ui_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, proj, sizeof(proj));
        ctx_->Unmap(ui_cb_.Get(), 0);
      }
    }

    const int fbw = (std::max)(1, ui.fb_w);
    const int fbh = (std::max)(1, ui.fb_h);
    const float sx = static_cast<float>(fbw) /
                     static_cast<float>((std::max)(1, ui.logical_w));
    const float sy = static_cast<float>(fbh) /
                     static_cast<float>((std::max)(1, ui.logical_h));

    D3D11_VIEWPORT full_vp{};
    full_vp.Width = static_cast<float>(fbw);
    full_vp.Height = static_cast<float>(fbh);
    full_vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &full_vp);

    UINT stride = sizeof(NkVertex), offset = 0;
    ctx_->IASetInputLayout(ui_layout_.Get());
    ctx_->IASetVertexBuffers(0, 1, ui_vb_.GetAddressOf(), &stride, &offset);
    ctx_->IASetIndexBuffer(ib_.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(ui_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ui_ps_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, ui_cb_.GetAddressOf());
    ctx_->PSSetShaderResources(0, 1, font_srv_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());
    ctx_->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_ui_.Get(), 0);
    ctx_->RSSetState(rs_scissor_.Get());
    D3D11_RECT full_sc{0, 0, fbw, fbh};
    ctx_->RSSetScissorRects(1, &full_sc);

    int draws = 0;
    UINT idx_off = 0;
    const nk_draw_command *cmd = nullptr;
    nk_draw_foreach(cmd, ui.ctx, ui.cmds) {
      if (!cmd || cmd->elem_count == 0) {
        continue;
      }
      D3D11_RECT rc{};
      rc.left = static_cast<LONG>(cmd->clip_rect.x * sx);
      rc.top = static_cast<LONG>(cmd->clip_rect.y * sy);
      rc.right = static_cast<LONG>((cmd->clip_rect.x + cmd->clip_rect.w) * sx);
      rc.bottom = static_cast<LONG>((cmd->clip_rect.y + cmd->clip_rect.h) * sy);
      if (overlay_only) {
        rc.left = (std::max)(
            rc.left, static_cast<LONG>(ui.overlay_x * sx));
        rc.top = (std::max)(
            rc.top, static_cast<LONG>(ui.overlay_y * sy));
        rc.right = (std::min)(
            rc.right,
            static_cast<LONG>((ui.overlay_x + ui.overlay_w) * sx));
        rc.bottom = (std::min)(
            rc.bottom,
            static_cast<LONG>((ui.overlay_y + ui.overlay_h) * sy));
      }
      if (rc.right <= rc.left || rc.bottom <= rc.top) {
        idx_off += cmd->elem_count;
        continue;
      }
      ctx_->RSSetScissorRects(1, &rc);
      ctx_->DrawIndexed(cmd->elem_count, idx_off, 0);
      idx_off += cmd->elem_count;
      ++draws;
    }
    return draws;
  }

  int drawMesh(const FrameInput &frame) {
    if (!frame.scene) {
      return 0;
    }
    int draws = 0;
    const int fbw = (std::max)(1, frame.fb_width);
    const int fbh = (std::max)(1, frame.fb_height);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(frame.viewport.x);
    vp.TopLeftY = static_cast<float>(frame.viewport.y);
    vp.Width = static_cast<float>(frame.viewport.w);
    vp.Height = static_cast<float>(frame.viewport.h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);

    D3D11_RECT sc{};
    sc.left = frame.viewport.x;
    sc.top = frame.viewport.y;
    sc.right = frame.viewport.x + frame.viewport.w;
    sc.bottom = frame.viewport.y + frame.viewport.h;
    ctx_->RSSetScissorRects(1, &sc);
    ctx_->RSSetState(rs_scissor_.Get());



    ctx_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    float mvp_gl[16];
    mulMat(frame.proj_matrix, frame.view_matrix, mvp_gl);
    float mvp[16];
    glMvpToD3d(mvp_gl, mvp);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(
            ctx_->Map(mesh_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
      std::memcpy(map.pData, mvp, sizeof(mvp));
      ctx_->Unmap(mesh_cb_.Get(), 0);
    }

    ctx_->VSSetShader(mesh_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(mesh_ps_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, mesh_cb_.GetAddressOf());
    ctx_->IASetInputLayout(mesh_layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);


    ctx_->OMSetDepthStencilState(ds_mesh_.Get(), 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    if (!frame.scene->solid.empty()) {
      const UINT bytes =
          static_cast<UINT>(frame.scene->solid.size() * sizeof(MeshVertex));
      ensureVb(mesh_vb_, mesh_vb_cap_, bytes, frame.scene->solid.data());
      UINT stride = sizeof(MeshVertex), off = 0;
      ctx_->IASetVertexBuffers(0, 1, mesh_vb_.GetAddressOf(), &stride, &off);
      ctx_->Draw(static_cast<UINT>(frame.scene->solid.size()), 0);
      ++draws;
    }

    if (!frame.scene->transparent.empty()) {
      ctx_->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
      ctx_->OMSetDepthStencilState(ds_mesh_no_write_.Get(), 0);
      const UINT bytes = static_cast<UINT>(frame.scene->transparent.size() *
                                           sizeof(MeshVertex));
      ensureVb(mesh_vb_, mesh_vb_cap_, bytes, frame.scene->transparent.data());
      UINT stride = sizeof(MeshVertex), off = 0;
      ctx_->IASetVertexBuffers(0, 1, mesh_vb_.GetAddressOf(), &stride, &off);
      ctx_->Draw(static_cast<UINT>(frame.scene->transparent.size()), 0);
      ++draws;
      ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
      ctx_->OMSetDepthStencilState(ds_mesh_.Get(), 0);
    }
    if (!frame.scene->lines.empty()) {
      const UINT bytes =
          static_cast<UINT>(frame.scene->lines.size() * sizeof(MeshVertex));
      ensureVb(mesh_vb_, mesh_vb_cap_, bytes, frame.scene->lines.data());
      UINT stride = sizeof(MeshVertex), off = 0;
      ctx_->IASetVertexBuffers(0, 1, mesh_vb_.GetAddressOf(), &stride, &off);
      ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
      ctx_->Draw(static_cast<UINT>(frame.scene->lines.size()), 0);
      ++draws;
    }


    D3D11_VIEWPORT full{};
    full.Width = static_cast<float>(fbw);
    full.Height = static_cast<float>(fbh);
    full.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &full);
    return draws;
  }

  SDL_Window *window_ = nullptr;
  HWND hwnd_ = nullptr;
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain> swap_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<ID3D11Texture2D> depth_;
  ComPtr<ID3D11DepthStencilView> dsv_;

  ComPtr<ID3D11VertexShader> ui_vs_, mesh_vs_;
  ComPtr<ID3D11PixelShader> ui_ps_, mesh_ps_;
  ComPtr<ID3D11InputLayout> ui_layout_, mesh_layout_;
  ComPtr<ID3D11Buffer> ui_cb_, mesh_cb_, ui_vb_, mesh_vb_, ib_;
  UINT ui_vb_cap_ = 0, mesh_vb_cap_ = 0, ib_cap_ = 0;
  ComPtr<ID3D11Texture2D> font_tex_;
  ComPtr<ID3D11ShaderResourceView> font_srv_;
  ComPtr<ID3D11SamplerState> samp_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11RasterizerState> rs_scissor_, rs_full_;
  ComPtr<ID3D11DepthStencilState> ds_ui_, ds_mesh_, ds_mesh_no_write_;
  bool vsync_ = true;
  bool allow_tearing_ = false;

  int fb_w_ = 1, fb_h_ = 1;
  std::string device_name_ = "D3D11";
  FrameStats stats_{};
};

}

std::unique_ptr<IGpuBackend> createDx11Backend() {
  return std::make_unique<Dx11Backend>();
}

}

#endif
