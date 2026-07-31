#pragma once

#include "xpbd/gfx/frame_stats.hpp"
#include "xpbd/gfx/ray_tracing.hpp"

struct nk_context;

namespace xpbd::app {

struct UiLayout {
  float menu_h = 28.0f;
  float bottom_h = 44.0f;
  float left_w = 250.0f;
  float right_w = 300.0f;
  float scale = 1.0f;

  float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
  bool viewport_hovered = false;
  // True from splitter press through the release frame. Render backends use
  // this to defer expensive temporal-target reallocations until the size is
  // stable again.
  bool viewport_resize_active = false;

  bool overlay_visible = false;
  float overlay_x = 0.0f;
  float overlay_y = 0.0f;
  float overlay_w = 0.0f;
  float overlay_h = 0.0f;

  bool horizontal_resize_cursor = false;
};

struct UiFrameResult {
  UiLayout layout;
};

void applyDarkStyle(nk_context *ctx, float scale);





UiFrameResult composeNuklearUi(nk_context *ctx, int win_w, int win_h,
                               float ui_scale, const char *backend_name,
                               const char *device_name,
                               const gfx::FrameStats &stats,
                               const gfx::RayTracingCapability *rt_cap = nullptr);

}
