#include "xpbd/app/nuklear_ui.hpp"

#include "nuklear_ui_internal.hpp"

#include "xpbd/app/app_session.hpp"
#include "xpbd/app/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xpbd::app {

using namespace ui_internal;

UiFrameResult composeNuklearUi(nk_context *ctx, int win_w, int win_h,
                               float ui_scale, const char *backend_name,
                               const char *device_name,
                               const gfx::FrameStats &stats,
                               const gfx::RayTracingCapability *rt_cap) {

  UiFrameResult result;
  auto &session = AppSession::instance();
  session.pollBakeProgress();
  static bool show_about = false;

  const float W = static_cast<float>((std::max)(win_w, 640));
  const float H = static_cast<float>((std::max)(win_h, 480));
  const Geom g = makeGeom(W, H);
  const bool busy =
      session.bake_busy.load() || session.stillRenderActive();
  UiPersistentState &ui_state = uiState();
  if (!ui_state.widths_initialized) {
    ui_state.widths_initialized = true;
    ui_state.preferred_left_w = g.left_w;
    ui_state.preferred_right_w = g.right_w;
  }

  bool finish_splitter_drag = false;
  result.layout.viewport_resize_active =
      ui_state.splitter_drag != SplitterDrag::None;
  if (ui_state.splitter_drag != SplitterDrag::None) {
    const float delta_x = ctx->input.mouse.pos.x - ui_state.drag_anchor_x;
    if (ui_state.splitter_drag == SplitterDrag::Left) {
      ui_state.preferred_left_w =
          (std::max)(1.0f, ui_state.drag_anchor_width + delta_x);
    } else {
      ui_state.preferred_right_w =
          (std::max)(1.0f, ui_state.drag_anchor_width - delta_x);
    }
    finish_splitter_drag = !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT);
  }

  result.layout.scale = g.s * ui_scale;
  result.layout.menu_h = g.menu_h * 2.0f;
  result.layout.bottom_h = g.bottom_h;
  result.layout.left_w = ui_state.preferred_left_w;
  result.layout.right_w = ui_state.preferred_right_w;





  const struct nk_rect full = nk_rect(0, 0, W, H);
  if (nk_begin(ctx, "##main", full, NK_WINDOW_NO_SCROLLBAR)) {
    nk_window_set_bounds(ctx, "##main", full);



    const float pad_x = ctx->style.window.padding.x * 2.0f;
    const float pad_y = ctx->style.window.padding.y * 2.0f;

    const float gap_y = ctx->style.window.spacing.y * 3.0f;
    const float inner_w = (std::max)(200.0f, W - pad_x);
    const float inner_h = (std::max)(160.0f, H - pad_y - gap_y);
    const float menu_row_h = g.btn;
    const float menu_rows = 2.0f;
    const float bottom_row_h = g.btn;
    const float mid_row_h =
        (std::max)(100.0f, inner_h - menu_row_h * menu_rows - bottom_row_h);

    const PanelWidths panel_widths =
        calculatePanelWidths(inner_w, g, ctx->style.window.spacing.x, ui_state);
    if (finish_splitter_drag) {
      ui_state.splitter_drag = SplitterDrag::None;
    }
    const float use_left = panel_widths.left;
    const float use_right = panel_widths.right;
    const float use_center = panel_widths.center;
    UiPanelContext panel_ui{ctx, session, g, busy, stats, rt_cap,
                            backend_name, device_name};

    drawMenuBar(panel_ui, inner_w, show_about);

    const UiModalLayout modal_layout{use_left, use_right,
                                     menu_row_h * menu_rows, bottom_row_h};
    if (!drawModalPanel(panel_ui, show_about, modal_layout, result)) {


      nk_layout_row_begin(ctx, NK_STATIC, mid_row_h, 5);


      nk_layout_row_push(ctx, use_left);
      if (nk_group_begin(ctx, "left", NK_WINDOW_BORDER)) {
        drawBonePanel(panel_ui, use_left, mid_row_h, ui_state);
        nk_group_end(ctx);
      }

      nk_layout_row_push(ctx, panel_widths.splitter);
      result.layout.horizontal_resize_cursor |=
          drawSplitter(ctx, SplitterDrag::Left, use_left, use_right, ui_state);


      nk_layout_row_push(ctx, use_center);
      if (nk_group_begin(ctx, "center", NK_WINDOW_BORDER)) {
        drawViewportOverlay(panel_ui, use_center, mid_row_h, result);
        nk_group_end(ctx);
      }

      nk_layout_row_push(ctx, panel_widths.splitter);
      result.layout.horizontal_resize_cursor |=
          drawSplitter(ctx, SplitterDrag::Right, use_right, use_left, ui_state);


      nk_layout_row_push(ctx, use_right);
      if (nk_group_begin(ctx, "props", NK_WINDOW_BORDER)) {
        drawPropertiesPanel(panel_ui, ui_state);
        nk_group_end(ctx);
      }

      nk_layout_row_end(ctx);

      if (ui_state.splitter_drag != SplitterDrag::None) {
        result.layout.viewport_hovered = false;
      }



      drawStatusPanel(panel_ui, inner_w);


      result.layout.left_w = use_left;
      result.layout.right_w = use_right;
      result.layout.menu_h = menu_row_h * menu_rows;
      result.layout.bottom_h = bottom_row_h;
    }
  }
  nk_end(ctx);

  drawBoneContextPopup(ctx, g, session, busy, W, H, result);

  return result;
}

}
