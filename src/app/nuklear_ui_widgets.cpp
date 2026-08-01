#include "nuklear_ui_internal.hpp"

#include "nuklear_still_settings_panel.hpp"

#include "xpbd/app/i18n.hpp"
#include "xpbd/baker/output_timeline_resampler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace xpbd::app::ui_internal {

void formatByteCount(char *buffer, std::size_t buffer_size, double bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  if (bytes >= kMiB) {
    std::snprintf(buffer, buffer_size, "%.2f MiB", bytes / kMiB);
  } else if (bytes >= kKiB) {
    std::snprintf(buffer, buffer_size, "%.1f KiB", bytes / kKiB);
  } else if (bytes > 0.0) {
    std::snprintf(buffer, buffer_size, "%.1f B", bytes);
  } else {
    std::snprintf(buffer, buffer_size, "0 B");
  }
}

UiPersistentState &uiState() {
  static UiPersistentState state;
  return state;
}

Geom makeGeom(float W, float H) {

  float s = std::clamp(std::min(W / 1280.0f, H / 800.0f), 0.72f, 1.6f);
  Geom g;
  g.s = s;
  g.menu_h = 30.0f * s;
  g.bottom_h = 46.0f * s;
  g.left_w = std::clamp(250.0f * s, 140.0f, 340.0f);
  g.right_w = std::clamp(360.0f * s, 190.0f, 480.0f);
  const float min_c = (std::max)(180.0f, 280.0f * s);
  if (W - g.left_w - g.right_w < min_c) {
    const float side = (W - min_c) * 0.5f;
    g.left_w = (std::max)(120.0f, side);
    g.right_w = (std::max)(130.0f, side);
  }
  g.btn = (std::max)(20.0f, 26.0f * s);
  g.row = (std::max)(18.0f, 22.0f * s);
  g.label = (std::max)(14.0f, 16.0f * s);
  return g;
}

void markDirty(
    const char *reason,
    InvalidationReason invalidation_reason) {
  auto &s = AppSession::instance();
  if (!s.bake_busy.load()) {
    s.applyUiToConfig();
    s.invalidatePhysicsArtifacts(invalidation_reason, reason);
  }
}

const char *bakeStateName(BakeState state) {
  switch (state) {
  case BakeState::Idle:
    return "Idle";
  case BakeState::Initialized:
    return "Initialized";
  case BakeState::Running:
    return "Running";
  case BakeState::AwaitingFinalize:
    return "Awaiting finalize";
  case BakeState::Completed:
    return "Completed";
  case BakeState::Invalid:
    return "Invalid";
  case BakeState::Cancelling:
    return "Cancelling";
  case BakeState::Cancelled:
    return "Cancelled";
  case BakeState::Failed:
    return "Failed";
  }
  return "Unknown";
}

const char *workerPhaseName(WorkerPhase phase) {
  switch (phase) {
  case WorkerPhase::Preparing:
    return "Preparing";
  case WorkerPhase::Simulating:
    return "Simulating";
  case WorkerPhase::Finalizing:
    return "Finalizing";
  case WorkerPhase::Auditing:
    return "Auditing";
  case WorkerPhase::Committing:
    return "Committing";
  case WorkerPhase::Finished:
    return "Finished";
  }
  return "Unknown";
}

void unfocusTextEditOnPointerWidget(nk_context *ctx) {
  if (ctx == nullptr) {
    return;
  }
  const struct nk_rect bounds = nk_widget_bounds(ctx);
  if (nk_input_is_mouse_click_down_in_rect(
          &ctx->input, NK_BUTTON_LEFT, bounds, nk_true)) {
    nk_edit_unfocus(ctx);
  }
}

bool slider(nk_context *ctx, const Geom &g, const char *label, float &value,
            float lo, float hi, bool busy, float requested_step) {
  float v = value;
  const std::string property_id =
      std::string("#") + (label != nullptr ? label : "value");
  const float step = requested_step > 0.0f
                         ? requested_step
                         : (std::max)((hi - lo) / 200.0f, 1e-8f);
  const float content_width = nk_window_get_content_region_size(ctx).x;
  const float property_width = (std::max)(104.0f, 112.0f * g.s);
  const float slider_width = (std::max)(72.0f, 90.0f * g.s);
  const float spacing = ctx->style.window.spacing.x * 2.0f;
  const bool wide =
      content_width >= property_width + slider_width + spacing + 72.0f;
  int changed = 0;
  if (wide) {
    const float label_width = (std::max)(72.0f, content_width - property_width -
                                                    slider_width - spacing);
    nk_layout_row_begin(ctx, NK_STATIC, g.btn, 3);
    nk_layout_row_push(ctx, label_width);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, property_width);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_property_float(ctx, property_id.c_str(), lo, &v, hi, step,
                                 step * 0.1f);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_push(ctx, slider_width);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_slider_float(ctx, lo, &v, hi, step);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_end(ctx);
  } else {
    nk_layout_row_begin(ctx, NK_DYNAMIC, g.btn, 2);
    nk_layout_row_push(ctx, 0.35f);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.65f);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_property_float(ctx, property_id.c_str(), lo, &v, hi, step,
                                 step * 0.1f);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, g.row, 1);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_slider_float(ctx, lo, &v, hi, step);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
  }
  if (changed && !busy) {
    value = v;
    return true;
  }
  return false;
}

bool intProperty(nk_context *ctx, const Geom &g, const char *label, int &value,
                 int lo, int hi, int step, bool busy) {
  int v = value;
  const std::string property_id =
      std::string("#") + (label != nullptr ? label : "value");
  const float content_width = nk_window_get_content_region_size(ctx).x;
  const float property_width = (std::max)(104.0f, 112.0f * g.s);
  const float slider_width = (std::max)(72.0f, 90.0f * g.s);
  const float spacing = ctx->style.window.spacing.x * 2.0f;
  const bool wide =
      content_width >= property_width + slider_width + spacing + 72.0f;
  int changed = 0;
  if (wide) {
    const float label_width = (std::max)(72.0f, content_width - property_width -
                                                    slider_width - spacing);
    nk_layout_row_begin(ctx, NK_STATIC, g.btn, 3);
    nk_layout_row_push(ctx, label_width);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, property_width);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_property_int(ctx, property_id.c_str(), lo, &v, hi, step,
                               static_cast<float>(step));
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_push(ctx, slider_width);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_slider_int(ctx, lo, &v, hi, step);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_end(ctx);
  } else {
    nk_layout_row_begin(ctx, NK_DYNAMIC, g.btn, 2);
    nk_layout_row_push(ctx, 0.35f);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.65f);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_property_int(ctx, property_id.c_str(), lo, &v, hi, step,
                               static_cast<float>(step));
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, g.row, 1);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_slider_int(ctx, lo, &v, hi, step);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
  }
  if (changed && !busy) {
    value = v;
    return true;
  }
  return false;
}

bool combo(nk_context *ctx, const Geom &g, const char *label,
           const std::vector<const char *> &items, int &selected, bool busy) {
  if (items.empty()) {
    return false;
  }
  selected = std::clamp(selected, 0, static_cast<int>(items.size()) - 1);
  nk_layout_row_begin(ctx, NK_DYNAMIC, g.btn, 2);
  nk_layout_row_push(ctx, 0.43f);
  nk_label(ctx, label, NK_TEXT_LEFT);
  nk_layout_row_push(ctx, 0.57f);
  if (busy) {
    nk_widget_disable_begin(ctx);
  }
  unfocusTextEditOnPointerWidget(ctx);
  const int next =
      nk_combo(ctx, items.data(), static_cast<int>(items.size()), selected,
               static_cast<int>(g.btn),
               nk_vec2(260.0f * g.s,
                       (std::min)(240.0f * g.s, g.btn * items.size() + 12.0f)));
  if (busy) {
    nk_widget_disable_end(ctx);
  }
  nk_layout_row_end(ctx);
  if (next != selected && !busy) {
    selected = next;
    return true;
  }
  return false;
}

bool check(nk_context *ctx, const Geom &g, const char *label, bool &value,
           bool busy) {
  nk_layout_row_dynamic(ctx, g.row, 1);
  nk_bool v = value ? nk_true : nk_false;
  if (busy) {
    nk_widget_disable_begin(ctx);
  }
  unfocusTextEditOnPointerWidget(ctx);
  const int ch = nk_checkbox_label(ctx, label, &v);
  if (busy) {
    nk_widget_disable_end(ctx);
  }
  if (ch && !busy) {
    value = v != 0;
    return true;
  }
  return false;
}

// 分组标题：左侧强调色竖条 + 亮色文字，视觉上切分各设置区块。
void heading(nk_context *ctx, const Geom &g, const char *t) {
  nk_layout_row_dynamic(ctx, 3.0f, 1);
  nk_spacing(ctx, 1);
  nk_layout_row_begin(ctx, NK_STATIC, g.row + 2.0f, 2);
  nk_layout_row_push(ctx, 8.0f);
  struct nk_rect bar{};
  if (nk_widget(&bar, ctx) != NK_WIDGET_INVALID) {
    nk_fill_rect(nk_window_get_canvas(ctx),
                 nk_rect(bar.x + 2.0f, bar.y + 3.0f, 3.0f, bar.h - 6.0f), 1.5f,
                 nk_rgb(49, 192, 200));
  }
  const float rest = (std::max)(
      40.0f, nk_window_get_content_region_size(ctx).x - 8.0f -
                 ctx->style.window.spacing.x * 3.0f);
  nk_layout_row_push(ctx, rest);
  nk_label_colored(ctx, t, NK_TEXT_LEFT, nk_rgb(231, 235, 244));
  nk_layout_row_end(ctx);
}

void muted(nk_context *ctx, const Geom &g, const char *t) {
  nk_layout_row_dynamic(ctx, g.label, 1);
  nk_label_colored(ctx, t, NK_TEXT_LEFT, nk_rgb(170, 177, 194));
}

void mutedWrap(nk_context *ctx, const Geom &g, const char *t) {
  nk_layout_row_dynamic(ctx, g.label * 2.0f, 1);
  nk_style_push_color(ctx, &ctx->style.text.color, nk_rgb(170, 177, 194));
  nk_label_wrap(ctx, t);
  nk_style_pop_color(ctx);
}

// 大纲树 / 列表行：扁平样式，选中行用强调色填充，悬停行浅色提示。
bool treeRowButton(nk_context *ctx, const char *label, bool selected,
                   bool hover_hint) {
  nk_style_push_style_item(
      ctx, &ctx->style.button.normal,
      selected     ? nk_style_item_color(nk_rgba(27, 140, 148, 255))
      : hover_hint ? nk_style_item_color(nk_rgba(61, 67, 83, 255))
                   : nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  nk_style_push_style_item(
      ctx, &ctx->style.button.hover,
      selected ? nk_style_item_color(nk_rgba(33, 158, 166, 255))
               : nk_style_item_color(nk_rgba(61, 67, 83, 255)));
  nk_style_push_style_item(ctx, &ctx->style.button.active,
                           nk_style_item_color(nk_rgba(41, 182, 190, 255)));
  nk_style_push_color(ctx, &ctx->style.button.text_normal,
                      selected ? nk_rgb(255, 255, 255)
                               : nk_rgb(224, 228, 238));
  nk_style_push_flags(ctx, &ctx->style.button.text_alignment, NK_TEXT_LEFT);
  const bool clicked = nk_button_label(ctx, label) != 0;
  nk_style_pop_flags(ctx);
  nk_style_pop_color(ctx);
  nk_style_pop_style_item(ctx);
  nk_style_pop_style_item(ctx);
  nk_style_pop_style_item(ctx);
  return clicked;
}

// 树展开箭头：无底色的扁平符号按钮。
bool flatSymbolButton(nk_context *ctx, enum nk_symbol_type symbol) {
  nk_style_push_style_item(ctx, &ctx->style.button.normal,
                           nk_style_item_color(nk_rgba(0, 0, 0, 0)));
  const bool clicked = nk_button_symbol(ctx, symbol) != 0;
  nk_style_pop_style_item(ctx);
  return clicked;
}

bool eyeButton(nk_context *ctx, bool visible) {
  const struct nk_rect bounds = nk_widget_bounds(ctx);
  const bool clicked = nk_button_label(ctx, "") != 0;
  auto *canvas = nk_window_get_canvas(ctx);
  const float cx = bounds.x + bounds.w * 0.5f;
  const float cy = bounds.y + bounds.h * 0.5f;
  const float rx = std::max(4.0f, bounds.w * 0.30f);
  const float ry = std::max(2.5f, bounds.h * 0.18f);
  const nk_color color =
      visible ? nk_rgb(205, 215, 235) : nk_rgb(125, 130, 145);
  nk_stroke_line(canvas, cx - rx, cy, cx, cy - ry, 1.2f, color);
  nk_stroke_line(canvas, cx, cy - ry, cx + rx, cy, 1.2f, color);
  nk_stroke_line(canvas, cx + rx, cy, cx, cy + ry, 1.2f, color);
  nk_stroke_line(canvas, cx, cy + ry, cx - rx, cy, 1.2f, color);
  const float pupil = std::max(1.5f, std::min(bounds.w, bounds.h) * 0.10f);
  nk_fill_circle(canvas, nk_rect(cx - pupil, cy - pupil, pupil * 2.0f,
                                pupil * 2.0f),
                 color);
  if (!visible) {
    nk_stroke_line(canvas, cx - rx, cy - ry - 1.0f, cx + rx,
                   cy + ry + 1.0f, 1.6f, nk_rgb(215, 105, 105));
  }
  return clicked;
}

PanelWidths calculatePanelWidths(float inner_width, const Geom &g,
                                 float spacing, UiPersistentState &state) {
  PanelWidths widths;
  widths.splitter = (std::max)(5.0f, 6.0f * g.s);
  const float usable =
      (std::max)(0.0f, inner_width - widths.splitter * 2.0f - spacing * 4.0f);
  const float min_left =
      (std::min)((std::max)(180.0f, 145.0f * g.s), usable * 0.30f);
  const float min_right = (std::min)(190.0f, usable * 0.33f);
  const float min_center =
      (std::min)((std::max)(160.0f, 220.0f * g.s),
                 (std::max)(0.0f, usable - min_left - min_right));
  const float side_capacity = (std::max)(0.0f, usable - min_center);

  const float preferred_left = (std::max)(min_left, state.preferred_left_w);
  const float preferred_right = (std::max)(min_right, state.preferred_right_w);
  if (state.splitter_drag == SplitterDrag::Left) {
    widths.right = std::clamp(state.drag_anchor_other_width, min_right,
                              (std::max)(min_right, side_capacity - min_left));
    widths.left =
        std::clamp(preferred_left, min_left,
                   (std::max)(min_left, side_capacity - widths.right));
    widths.center = (std::max)(0.0f, usable - widths.left - widths.right);
    state.preferred_left_w = widths.left;
    state.preferred_right_w = widths.right;
    return widths;
  }
  if (state.splitter_drag == SplitterDrag::Right) {
    widths.left = std::clamp(state.drag_anchor_other_width, min_left,
                             (std::max)(min_left, side_capacity - min_right));
    widths.right =
        std::clamp(preferred_right, min_right,
                   (std::max)(min_right, side_capacity - widths.left));
    widths.center = (std::max)(0.0f, usable - widths.left - widths.right);
    state.preferred_left_w = widths.left;
    state.preferred_right_w = widths.right;
    return widths;
  }
  const float flexible_capacity =
      (std::max)(0.0f, side_capacity - min_left - min_right);
  const float requested_flexible =
      (std::max)(0.0f, preferred_left - min_left) +
      (std::max)(0.0f, preferred_right - min_right);
  const float scale =
      requested_flexible > flexible_capacity && requested_flexible > 0.0f
          ? flexible_capacity / requested_flexible
          : 1.0f;
  widths.left = min_left + (std::max)(0.0f, preferred_left - min_left) * scale;
  widths.right =
      min_right + (std::max)(0.0f, preferred_right - min_right) * scale;
  widths.center = (std::max)(0.0f, usable - widths.left - widths.right);
  return widths;
}

bool drawSplitter(nk_context *ctx, SplitterDrag side, float current_width,
                  float other_width, UiPersistentState &state) {
  struct nk_rect bounds{};
  const bool visible = nk_widget(&bounds, ctx) != NK_WIDGET_INVALID;
  const bool hovered =
      visible && nk_input_is_mouse_hovering_rect(&ctx->input, bounds);
  if (hovered && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT)) {
    state.splitter_drag = side;
    state.drag_anchor_x = ctx->input.mouse.pos.x;
    state.drag_anchor_width = current_width;
    state.drag_anchor_other_width = other_width;
  }
  const bool active = state.splitter_drag == side;
  if (visible) {
    struct nk_rect bar = bounds;
    if (!hovered && !active) {
      bar.x += (bar.w - 2.0f) * 0.5f;
      bar.w = 2.0f;
    }
    nk_fill_rect(nk_window_get_canvas(ctx), bar, 1.0f,
                 active    ? nk_rgb(106, 222, 228)
                 : hovered ? nk_rgb(49, 192, 200)
                           : nk_rgb(63, 68, 84));
  }
  return hovered || active;
}


void drawViewportOverlay(UiPanelContext &ui, float panel_width,
                         float panel_height, UiFrameResult &result) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;
  const float use_center = panel_width;
  const float mid_row_h = panel_height;

  const float tool = g.btn + 8.0f;
  const float info = g.label + 4.0f;
  const float mode_row = g.btn;
  const float vp_h =
      (std::max)(40.0f,
                 mid_row_h - tool - info - mode_row - 32.0f);
  nk_layout_row_dynamic(ctx, vp_h, 1);
  struct nk_rect space{};
  nk_widget(&space, ctx);

  result.layout.vp_x = space.x;
  result.layout.vp_y = space.y;
  result.layout.vp_w = space.w;
  result.layout.vp_h = space.h;
  result.layout.viewport_hovered =
      nk_input_is_mouse_hovering_rect(&ctx->input, space) != 0;

  char info_buf[192];
  if (session.presentation_mode ==
          PresentationMode::FinalBakedPreview &&
      session.hasCompleteBake()) {
    std::snprintf(info_buf, sizeof(info_buf), tr("preview_baked"),
                  session.preview_frame_index + 1,
                  session.previewFrameCount(),
                  session.preview_time);
  } else if (session.presentation_mode ==
                 PresentationMode::LiveSimulation &&
             session.liveSimulationFrame()) {
    const auto *live = session.liveSimulationFrame();
    std::snprintf(info_buf, sizeof(info_buf), tr("preview_live"),
                  live->current_step, live->total_steps,
                  live->sample_time);
  } else if (session.selected_animation) {
    std::snprintf(info_buf, sizeof(info_buf), tr("preview_source"),
                  session.preview_time, session.previewLength(),
                  session.playback_state == PlaybackState::Playing
                      ? tr("playing")
                      : tr("paused"));
  } else if (!session.geometry.bones.empty()) {
    std::snprintf(info_buf, sizeof(info_buf), tr("preview_bones"),
                  session.geometry.bones.size());
  } else {
    std::snprintf(info_buf, sizeof(info_buf), "%s",
                  tr("preview_open_model"));
  }
  nk_layout_row_dynamic(ctx, info, 1);
  nk_label_colored(ctx, info_buf, NK_TEXT_LEFT, nk_rgb(184, 191, 207));

  nk_layout_row_dynamic(ctx, mode_row, 3);
  const auto preview_mode_button = [&](const char *label,
                                       PresentationMode mode,
                                       bool available) {
    const bool active_mode = session.presentation_mode == mode;
    if (active_mode) {
      nk_style_push_style_item(
          ctx, &ctx->style.button.normal,
          nk_style_item_color(nk_rgba(27, 140, 148, 255)));
      nk_style_push_style_item(
          ctx, &ctx->style.button.hover,
          nk_style_item_color(nk_rgba(33, 158, 166, 255)));
      nk_style_push_color(ctx, &ctx->style.button.text_normal,
                          nk_rgb(255, 255, 255));
    }
    if (!available) {
      nk_widget_disable_begin(ctx);
    }
    if (nk_button_label(ctx, label) && available && !active_mode) {
      session.setPresentationMode(mode);
    }
    if (!available) {
      nk_widget_disable_end(ctx);
    }
    if (active_mode) {
      nk_style_pop_color(ctx);
      nk_style_pop_style_item(ctx);
      nk_style_pop_style_item(ctx);
    }
  };
  preview_mode_button(tr("preview_mode_source"),
                      PresentationMode::SourcePreview,
                      session.selected_animation != nullptr);
  preview_mode_button(tr("preview_mode_live"),
                      PresentationMode::LiveSimulation,
                      session.liveSimulationFrame() != nullptr);
  preview_mode_button(tr("preview_mode_final"),
                      PresentationMode::FinalBakedPreview,
                      session.hasCompleteBake());

  nk_layout_row_begin(ctx, NK_STATIC, g.btn, 6);
  nk_layout_row_push(ctx, 48.0f * g.s);
  if (nk_button_label(ctx, tr("fit"))) {
    session.camera_needs_fit = true;
    session.fitCameraToModel();
  }
  nk_layout_row_push(ctx, 36.0f * g.s);
  if (nk_button_label(ctx, "<") &&
      session.presentation_mode ==
          PresentationMode::FinalBakedPreview &&
      session.previewFrameCount() > 0) {
    session.playback_state = PlaybackState::Paused;
    session.setPreviewFrameIndex(session.preview_frame_index - 1);
  }
  nk_layout_row_push(ctx, 56.0f * g.s);
  if (nk_button_label(
          ctx, session.playback_state == PlaybackState::Playing
                   ? tr("pause")
                   : tr("play")) &&
      session.canPreview()) {
    session.togglePreviewPlayback();
  }
  nk_layout_row_push(ctx, 36.0f * g.s);
  if (nk_button_label(ctx, ">") &&
      session.presentation_mode ==
          PresentationMode::FinalBakedPreview &&
      session.previewFrameCount() > 0) {
    session.playback_state = PlaybackState::Paused;
    session.setPreviewFrameIndex(session.preview_frame_index + 1);
  }
  if (session.canPreview()) {
    float scrub = 0.0f;
    if (session.presentation_mode ==
            PresentationMode::FinalBakedPreview &&
        session.previewFrameCount() > 0) {
      const int n = static_cast<int>(session.previewFrameCount());
      scrub = n > 1 ? static_cast<float>(session.preview_frame_index) /
                          static_cast<float>(n - 1)
                    : 0.0f;
    } else {
      const double len = session.previewLength();
      scrub =
          len > 0 ? static_cast<float>(session.preview_time / len) : 0.0f;
    }
    nk_layout_row_push(ctx, (std::max)(80.0f, use_center - 240.0f * g.s));
    const struct nk_rect scrub_bounds = nk_widget_bounds(ctx);
    if (nk_slider_float(ctx, 0.0f, &scrub, 1.0f, 0.001f)) {
      session.playback_state = PlaybackState::Paused;
      scrub = std::clamp(scrub, 0.0f, 1.0f);
      if (session.presentation_mode ==
              PresentationMode::FinalBakedPreview &&
          session.previewFrameCount() > 0) {
        const int n = static_cast<int>(session.previewFrameCount());
        session.setPreviewFrameIndex(static_cast<int>(
            scrub * static_cast<float>((std::max)(1, n - 1))));
      } else if (session.selected_animation) {
        session.preview_time = scrub * session.previewLength();
      }
    }
    if (session.presentation_mode ==
            PresentationMode::FinalBakedPreview) {
      const auto *final_result = session.finalResult();
      if (final_result != nullptr && final_result->frames != nullptr &&
          final_result->frames->size() >= 2) {
        const double start_time = final_result->frames->front().time;
        const double end_time = final_result->frames->back().time;
        const double span = end_time - start_time;
        if (std::isfinite(span) && span > 0.0) {
          auto *canvas = nk_window_get_canvas(ctx);
          for (const auto &marker :
               final_result->diagnostics.loop.danger_markers) {
            if (!std::isfinite(marker.time) || marker.time < start_time ||
                marker.time > end_time) {
              continue;
            }
            const float fraction = static_cast<float>(std::clamp(
                (marker.time - start_time) / span, 0.0, 1.0));
            const float x = scrub_bounds.x +
                            fraction * scrub_bounds.w;
            const nk_color color =
                marker.kind == "Seam Window Start"
                    ? nk_rgb(255, 196, 64)
                    : nk_rgb(255, 72, 72);
            nk_stroke_line(canvas, x, scrub_bounds.y + 2.0f, x,
                           scrub_bounds.y + scrub_bounds.h - 2.0f,
                           2.0f, color);
          }
        }
      }
    }
  }
  nk_layout_row_end(ctx);

}

void drawPropertiesPanel(UiPanelContext &ui, UiPersistentState &state) {
  nk_context *ctx = ui.nk;
  const Geom &g = ui.geom;

  nk_layout_row_dynamic(ctx, g.btn, 4);
  if (treeRowButton(ctx, tr("properties_physics"), state.properties_page == 0,
                    false)) {
    state.properties_page = 0;
  }
  if (treeRowButton(ctx, tr("properties_renderer"), state.properties_page == 1,
                    false)) {
    state.properties_page = 1;
  }
  if (treeRowButton(ctx, tr("properties_sky"), state.properties_page == 2,
                    false)) {
    state.properties_page = 2;
  }
  if (treeRowButton(ctx, tr("properties_labpbr"), state.properties_page == 3,
                    false)) {
    state.properties_page = 3;
  }

  if (state.properties_page == 3) {
    drawLabPbrPanel(ui);
  } else if (state.properties_page == 2) {
    drawWorldPanel(ui);
  } else if (state.properties_page == 1) {
    drawRenderPanel(ui);
  } else {
    drawBakePanel(ui);
    drawWorldOptions(ui);
    drawDebugStatusPanel(ui);
  }
}


void drawMenuBar(UiPanelContext &ui, float inner_width, bool &show_about) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;
  const bool busy = ui.busy;
  const float inner_w = inner_width;
  const float menu_row_h = g.btn;

  auto file_btn = [&](const char *label, float w, bool enabled, auto fn) {
    nk_layout_row_push(ctx, w);
    if (busy || !enabled) {
      nk_widget_disable_begin(ctx);
    }
    if (nk_button_label(ctx, label) && !busy && enabled) {
      fn();
    }
    if (busy || !enabled) {
      nk_widget_disable_end(ctx);
    }
  };
  const float gap = 4.0f * g.s;
  float w_om = 88.0f * g.s;
  float w_oa = 88.0f * g.s;
  float w_tex = 88.0f * g.s;
  float w_ea = 88.0f * g.s;
  float w_all = 88.0f * g.s;
  float w_ev = 80.0f * g.s;
  float w_about = 72.0f * g.s;
  float need = w_om + w_oa + w_tex + w_ea + w_all + w_ev + w_about +
               gap * 6.0f;
  if (need > inner_w) {
    const float k = inner_w / need;
    w_om *= k;
    w_oa *= k;
    w_tex *= k;
    w_ea *= k;
    w_all *= k;
    w_ev *= k;
    w_about *= k;
  }

  nk_layout_row_begin(ctx, NK_STATIC, menu_row_h, 7);
  file_btn(tr("open_model"), w_om, true, [&] {
    if (auto p = openFileDialog(
            L"Open Bedrock Geometry Model",
            L"Geometry JSON "
            L"(*.geo.json;*.json)\0*.geo.json;*.json\0All\0*.*\0")) {
      session.loadModel(*p);
    }
  });
  file_btn(tr("open_anim"), w_oa, true, [&] {
    if (auto p = openFileDialog(L"Open Bedrock Animation",
                                L"Animation JSON "
                                L"(*.animation.json;*.json)\0*.animation."
                                L"json;*.json\0All\0*.*\0")) {
      session.loadAnimation(*p);
    }
  });
  file_btn(tr("import_tex"), w_tex, true, [&] {
    if (auto p = openFileDialog(L"Import Model Texture",
                                L"Images "
                                L"(*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*."
                                L"jpeg\0PNG\0*.png\0All\0*.*\0")) {
      session.loadTexture(*p);
    }
  });
  file_btn(tr("export_anim"), w_ea,
           !session.force_export_confirmation_pending &&
               (session.canExportAnimation() ||
                session.canForceExportAnimation()),
           [&] {
    if (auto p =
            saveFileDialog(L"Export Baked Animation",
                           L"Animation JSON (*.json)\0*.json\0All\0*.*\0",
                           L"animation.baked.json")) {
      session.requestAnimationExport(*p);
    }
  });
  file_btn(tr("export_all_anim"), w_all,
           !session.force_export_confirmation_pending &&
               (session.canExportAnimation() ||
                session.canForceExportAnimation()),
           [&] {
    if (auto p =
            saveFileDialog(L"Export All Animations",
                           L"Animation JSON (*.json)\0*.json\0All\0*.*\0",
                           L"animations.full.json")) {
      session.requestAllAnimationsExport(*p);
    }
  });
  file_btn(tr("export_vel"), w_ev, session.canExportVelocity(), [&] {
    if (auto p = saveFileDialog(L"Export Velocity Cache",
                                L"Velocity JSON (*.json)\0*.json\0All\0*.*\0",
                                L"animation.velocity.json")) {
      session.exportVelocity(*p);
    }
  });
  nk_layout_row_push(ctx, w_about);

  if (nk_button_label(ctx, show_about ? tr("about_close") : tr("about"))) {
    show_about = !show_about;
  }
  nk_layout_row_end(ctx);


  nk_layout_row_begin(ctx, NK_STATIC, menu_row_h, 2);
  const float w_clr = 80.0f * g.s;
  const float w_status2 = (std::max)(80.0f, inner_w - w_clr - gap);
  nk_layout_row_push(ctx, w_status2);
  nk_label_colored(ctx, session.status.c_str(), NK_TEXT_LEFT,
                   busy ? nk_rgb(94, 212, 218) : nk_rgb(150, 156, 172));
  nk_layout_row_push(ctx, w_clr);
  if (nk_button_label(ctx, tr("clear_tex")) &&
      session.model_texture.valid()) {
    session.clearTexture();
  }
  nk_layout_row_end(ctx);


}

bool drawModalPanel(UiPanelContext &ui, bool &show_about,
                    const UiModalLayout &layout, UiFrameResult &result) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;
  const float use_left = layout.left_width;
  const float use_right = layout.right_width;
  const float menu_row_h = layout.menu_height;
  const float menu_rows = 1.0f;
  const float bottom_row_h = layout.bottom_height;

  if (session.molang_confirmation_pending) {



    heading(ctx, g, tr("molang_warning_title"));

    const auto channelLabel = [](loader::MolangChannel channel) {
      switch (channel) {
      case loader::MolangChannel::Position:
        return tr("molang_channel_position");
      case loader::MolangChannel::Rotation:
        return tr("molang_channel_rotation");
      case loader::MolangChannel::Scale:
        return tr("molang_channel_scale");
      }
      return "";
    };
    const auto roleLabel = [](loader::MolangAnimationRole role) {
      switch (role) {
      case loader::MolangAnimationRole::Source:
        return tr("molang_role_source");
      case loader::MolangAnimationRole::TransitionTarget:
        return tr("molang_role_transition_target");
      }
      return "";
    };
    const auto warningCount = [&](loader::MolangBakeAction action) {
      return static_cast<std::size_t>(
          std::count_if(session.pending_molang_warnings.begin(),
                        session.pending_molang_warnings.end(),
                        [action](const loader::MolangBakeWarning &warning) {
                          return warning.action == action;
                        }));
    };
    const std::size_t preserve_count =
        warningCount(loader::MolangBakeAction::SampleAsZeroPreserve);
    const std::size_t overwrite_count =
        warningCount(loader::MolangBakeAction::OverwriteWithBakedKeys);
    const auto warningGroup =
        [&](loader::MolangBakeAction action, const char *heading_key,
            const char *body_key, std::size_t visible_limit) {
          const std::size_t count = warningCount(action);
          if (count == 0) {
            return;
          }
          heading(ctx, g, tr(heading_key));
          mutedWrap(ctx, g, tr(body_key));
          std::size_t shown = 0;
          for (const auto &entry : session.pending_molang_warnings) {
            if (entry.action != action || shown >= visible_limit) {
              continue;
            }
            const std::string line = std::string(roleLabel(entry.role)) +
                                     "[" + entry.animation_name + "]  ·  " +
                                     entry.bone_name + "  ·  " +
                                     channelLabel(entry.channel);
            nk_layout_row_dynamic(ctx, g.row, 1);
            nk_label(ctx, line.c_str(), NK_TEXT_LEFT);
            ++shown;
          }
          if (count > shown) {
            nk_layout_row_dynamic(ctx, g.row, 1);
            nk_label(ctx, trf("molang_warning_more", count - shown),
                     NK_TEXT_LEFT);
          }
        };

    warningGroup(loader::MolangBakeAction::SampleAsZeroPreserve,
                 "molang_preserve_heading", "molang_preserve_body",
                 overwrite_count == 0 ? 10 : 5);
    warningGroup(loader::MolangBakeAction::OverwriteWithBakedKeys,
                 "molang_overwrite_heading", "molang_overwrite_body",
                 preserve_count == 0 ? 10 : 5);

    nk_layout_row_dynamic(ctx, g.btn + 6.0f, 2);
    if (nk_button_label(ctx, tr("cancel"))) {
      session.confirmMolangBake(false);
    }
    nk_style_push_style_item(ctx, &ctx->style.button.normal,
                             nk_style_item_color(nk_rgba(206, 82, 70, 255)));
    if (nk_button_label(ctx, tr("molang_warning_continue"))) {
      session.confirmMolangBake(true);
    }
    nk_style_pop_style_item(ctx);

    result.layout.vp_w = 0;
    result.layout.vp_h = 0;
    result.layout.viewport_hovered = false;
    result.layout.left_w = use_left;
    result.layout.right_w = use_right;
    result.layout.menu_h = menu_row_h * menu_rows;
    result.layout.bottom_h = bottom_row_h;
  } else if (session.force_export_confirmation_pending) {
    heading(ctx, g, tr("force_export_warning_title"));
    mutedWrap(ctx, g, tr("force_export_warning_body"));
    const auto &reasons = session.exportPreflight().block_reasons;
    constexpr std::size_t visible_limit = 8;
    std::size_t shown = 0;
    for (const auto &reason : reasons) {
      if (shown >= visible_limit) {
        break;
      }
      mutedWrap(ctx, g, reason.detail.c_str());
      ++shown;
    }
    if (reasons.size() > shown) {
      nk_layout_row_dynamic(ctx, g.row, 1);
      nk_label(ctx, trf("force_export_more", reasons.size() - shown),
               NK_TEXT_LEFT);
    }
    nk_layout_row_dynamic(ctx, g.btn + 6.0f, 2);
    if (nk_button_label(ctx, tr("cancel"))) {
      session.confirmAnimationExport(false);
    }
    nk_style_push_style_item(ctx, &ctx->style.button.normal,
                             nk_style_item_color(nk_rgba(206, 82, 70, 255)));
    if (nk_button_label(ctx, tr("force_export_continue"))) {
      session.confirmAnimationExport(true);
    }
    nk_style_pop_style_item(ctx);

    result.layout.vp_w = 0;
    result.layout.vp_h = 0;
    result.layout.viewport_hovered = false;
    result.layout.left_w = use_left;
    result.layout.right_w = use_right;
    result.layout.menu_h = menu_row_h * menu_rows;
    result.layout.bottom_h = bottom_row_h;
  } else if (session.labpbr_import_confirmation_pending) {
    heading(ctx, g, tr("labpbr_suite_confirm_title"));
    mutedWrap(ctx, g, tr("labpbr_suite_confirm_body"));
    nk_layout_row_dynamic(ctx, g.btn + 6.0f, 2);
    if (nk_button_label(ctx, tr("cancel"))) {
      session.confirmLabPbrSuiteImport(false);
    }
    nk_style_push_style_item(ctx, &ctx->style.button.normal,
                             nk_style_item_color(nk_rgba(206, 82, 70, 255)));
    if (nk_button_label(ctx, tr("labpbr_suite_confirm_continue"))) {
      session.confirmLabPbrSuiteImport(true);
    }
    nk_style_pop_style_item(ctx);

    result.layout.vp_w = 0;
    result.layout.vp_h = 0;
    result.layout.viewport_hovered = false;
    result.layout.left_w = use_left;
    result.layout.right_w = use_right;
    result.layout.menu_h = menu_row_h * menu_rows;
    result.layout.bottom_h = bottom_row_h;
  } else if (session.labpbr_candidate_selection_pending) {
    heading(ctx, g, tr("labpbr_suite_candidates_title"));
    mutedWrap(ctx, g, tr("labpbr_suite_candidates_body"));
    for (std::size_t candidate_index = 0;
         candidate_index < session.labpbr_import_candidates.size();
         ++candidate_index) {
      nk_layout_row_dynamic(ctx, g.btn, 1);
      const auto label =
          session.labpbr_import_candidates[candidate_index]
              .filename()
              .string();
      if (nk_button_label(ctx, label.c_str())) {
        session.selectLabPbrSuiteCandidate(candidate_index);
        break;
      }
    }
    nk_layout_row_dynamic(ctx, g.btn + 6.0f, 1);
    if (nk_button_label(ctx, tr("cancel"))) {
      session.cancelLabPbrSuiteCandidateSelection();
    }

    result.layout.vp_w = 0;
    result.layout.vp_h = 0;
    result.layout.viewport_hovered = false;
    result.layout.left_w = use_left;
    result.layout.right_w = use_right;
    result.layout.menu_h = menu_row_h * menu_rows;
    result.layout.bottom_h = bottom_row_h;
  } else if (session.labpbr_export_confirmation_pending) {
    heading(ctx, g, tr("labpbr_overwrite_title"));
    mutedWrap(ctx, g, tr("labpbr_overwrite_body"));
    for (const auto &path : session.labpbr_export_existing_paths) {
      mutedWrap(ctx, g, path.string().c_str());
    }
    nk_layout_row_dynamic(ctx, g.btn + 6.0f, 2);
    if (nk_button_label(ctx, tr("cancel"))) {
      session.confirmLabPbrExport(false);
    }
    nk_style_push_style_item(ctx, &ctx->style.button.normal,
                             nk_style_item_color(nk_rgba(206, 82, 70, 255)));
    if (nk_button_label(ctx, tr("labpbr_overwrite_continue"))) {
      session.confirmLabPbrExport(true);
    }
    nk_style_pop_style_item(ctx);

    result.layout.vp_w = 0;
    result.layout.vp_h = 0;
    result.layout.viewport_hovered = false;
    result.layout.left_w = use_left;
    result.layout.right_w = use_right;
    result.layout.menu_h = menu_row_h * menu_rows;
    result.layout.bottom_h = bottom_row_h;
  } else if (show_about) {

    heading(ctx, g, tr("about_title"));
    nk_layout_row_dynamic(ctx, g.label * 3.5f, 1);
    nk_label_wrap(ctx, tr("about_desc"));
    char line[320];
    nk_layout_row_dynamic(ctx, g.row, 1);
    std::snprintf(line, sizeof(line), "%s: %s", tr("about_author"),
                  kAuthorOriginal);
    nk_label(ctx, line, NK_TEXT_LEFT);
    std::snprintf(line, sizeof(line), "%s: %s", tr("about_cpp"), kAuthorCpp);
    nk_label(ctx, line, NK_TEXT_LEFT);
    std::snprintf(line, sizeof(line), "%s: %s", tr("about_version"),
                  kAppVersion);
    nk_label(ctx, line, NK_TEXT_LEFT);
    std::snprintf(line, sizeof(line), "%s: %s", tr("about_github"),
                  kGithubUrl);
    nk_label(ctx, line, NK_TEXT_LEFT);
    heading(ctx, g, tr("about_licenses"));
    nk_layout_row_dynamic(ctx, g.label * 6.0f, 1);
    nk_label_wrap(ctx, tr("about_licenses_body"));
    nk_layout_row_dynamic(ctx, g.label * 2.0f, 1);
    nk_label_wrap(ctx, "notices/  (next to the executable)");
    nk_layout_row_dynamic(ctx, g.btn + 4.0f, 1);
    if (nk_button_label(ctx, tr("about_close"))) {
      show_about = false;
    }
    result.layout.vp_w = 0;
    result.layout.vp_h = 0;
    result.layout.viewport_hovered = false;
    result.layout.left_w = use_left;
    result.layout.right_w = use_right;
    result.layout.menu_h = menu_row_h * menu_rows;
    result.layout.bottom_h = bottom_row_h;
  } else {
    return false;
  }
  return true;
}

} // namespace xpbd::app::ui_internal






namespace xpbd::app {

void applyDarkStyle(nk_context *ctx, float ) {

  auto &st = ctx->style;

  // 现代深色配色：深底、低对比描边、Blockbench 风格的青色强调色。
  const nk_color bg = nk_rgba(26, 28, 34, 255);
  const nk_color panel = nk_rgba(34, 37, 45, 255);
  const nk_color inset = nk_rgba(22, 24, 30, 255);
  const nk_color raised = nk_rgba(53, 57, 69, 255);
  const nk_color raised_h = nk_rgba(66, 72, 88, 255);
  const nk_color border = nk_rgba(70, 76, 93, 210);
  const nk_color text = nk_rgba(235, 238, 246, 255);
  const nk_color text_dim = nk_rgba(170, 177, 194, 255);
  const nk_color accent = nk_rgba(49, 192, 200, 255);
  const nk_color accent_hi = nk_rgba(106, 222, 228, 255);
  const nk_color accent_dn = nk_rgba(32, 148, 156, 255);

  st.window.background = bg;
  st.window.fixed_background = nk_style_item_color(bg);
  st.window.border_color = border;
  st.window.border = 0.0f;
  st.window.rounding = 0.0f;
  st.window.padding = nk_vec2(8, 8);
  st.window.group_padding = nk_vec2(8, 6);
  st.window.spacing = nk_vec2(6, 5);
  st.window.scrollbar_size = nk_vec2(10, 10);
  st.window.group_border = 1.0f;
  st.window.group_border_color = border;
  st.window.combo_border = 1.0f;
  st.window.combo_border_color = border;
  st.window.popup_border_color = border;
  st.window.tooltip_border_color = border;

  st.button.normal = nk_style_item_color(raised);
  st.button.hover = nk_style_item_color(raised_h);
  st.button.active = nk_style_item_color(accent_dn);
  st.button.border_color = nk_rgba(0, 0, 0, 0);
  st.button.text_normal = text;
  st.button.text_hover = nk_rgb(255, 255, 255);
  st.button.text_active = nk_rgb(255, 255, 255);
  st.button.rounding = 6.0f;
  st.button.padding = nk_vec2(8, 4);
  st.button.border = 0.0f;

  st.checkbox.normal = nk_style_item_color(raised);
  st.checkbox.hover = nk_style_item_color(raised_h);
  st.checkbox.active = nk_style_item_color(raised_h);
  st.checkbox.cursor_normal = nk_style_item_color(accent);
  st.checkbox.cursor_hover = nk_style_item_color(accent_hi);
  st.checkbox.border_color = border;
  st.checkbox.border = 1.0f;
  st.checkbox.padding = nk_vec2(2, 2);
  st.checkbox.text_normal = text;
  st.checkbox.text_hover = text;
  st.checkbox.text_active = text;
  st.option = st.checkbox;

  st.slider.bar_normal = inset;
  st.slider.bar_hover = nk_rgba(38, 41, 50, 255);
  st.slider.bar_active = nk_rgba(43, 47, 59, 255);
  st.slider.bar_filled = accent;
  st.slider.cursor_normal = nk_style_item_color(nk_rgb(226, 231, 242));
  st.slider.cursor_hover = nk_style_item_color(nk_rgb(255, 255, 255));
  st.slider.cursor_active = nk_style_item_color(accent_hi);
  st.slider.cursor_size = nk_vec2(13, 13);
  st.slider.rounding = 4.0f;
  st.slider.bar_height = 6.0f;

  st.progress.normal = nk_style_item_color(inset);
  st.progress.hover = nk_style_item_color(inset);
  st.progress.active = nk_style_item_color(inset);
  st.progress.cursor_normal = nk_style_item_color(accent);
  st.progress.cursor_hover = nk_style_item_color(accent_hi);
  st.progress.cursor_active = nk_style_item_color(accent_hi);
  st.progress.rounding = 5.0f;
  st.progress.cursor_rounding = 5.0f;
  st.progress.padding = nk_vec2(2, 2);

  st.scrollv.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.scrollv.hover = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.scrollv.active = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.scrollv.cursor_normal = nk_style_item_color(nk_rgba(84, 90, 108, 255));
  st.scrollv.cursor_hover = nk_style_item_color(nk_rgba(108, 116, 138, 255));
  st.scrollv.cursor_active = nk_style_item_color(accent);
  st.scrollv.rounding = 5.0f;
  st.scrollv.rounding_cursor = 5.0f;
  st.scrollv.border = 0.0f;
  st.scrollv.border_cursor = 0.0f;
  st.scrollh = st.scrollv;

  st.text.color = text;

  st.edit.normal = nk_style_item_color(inset);
  st.edit.hover = nk_style_item_color(inset);
  st.edit.active = nk_style_item_color(inset);
  st.edit.border_color = border;
  st.edit.border = 1.0f;
  st.edit.rounding = 5.0f;
  st.edit.text_normal = text;
  st.edit.text_hover = text;
  st.edit.text_active = text;
  st.edit.cursor_normal = accent_hi;
  st.edit.cursor_hover = accent_hi;
  st.edit.cursor_text_normal = bg;
  st.edit.cursor_text_hover = bg;
  st.edit.selected_normal = accent_dn;
  st.edit.selected_hover = accent_dn;
  st.edit.selected_text_normal = nk_rgb(255, 255, 255);
  st.edit.selected_text_hover = nk_rgb(255, 255, 255);

  st.combo.normal = nk_style_item_color(raised);
  st.combo.hover = nk_style_item_color(raised_h);
  st.combo.active = nk_style_item_color(raised_h);
  st.combo.border_color = nk_rgba(0, 0, 0, 0);
  st.combo.border = 0.0f;
  st.combo.rounding = 6.0f;
  st.combo.label_normal = text;
  st.combo.label_hover = text;
  st.combo.label_active = text;
  st.combo.symbol_normal = text_dim;
  st.combo.symbol_hover = text;
  st.combo.symbol_active = text;
  st.combo.button.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.combo.button.hover = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.combo.button.active = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.combo.button.border = 0.0f;
  st.combo.button.text_normal = text_dim;
  st.combo.button.text_hover = text;
  st.combo.button.text_active = text;

  st.contextual_button.normal = nk_style_item_color(panel);
  st.contextual_button.hover = nk_style_item_color(raised_h);
  st.contextual_button.active = nk_style_item_color(accent_dn);
  st.contextual_button.border = 0.0f;
  st.contextual_button.text_normal = text;
  st.contextual_button.text_hover = nk_rgb(255, 255, 255);
  st.contextual_button.text_active = nk_rgb(255, 255, 255);

  st.selectable.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.selectable.hover = nk_style_item_color(raised);
  st.selectable.pressed = nk_style_item_color(raised_h);
  st.selectable.normal_active = nk_style_item_color(accent_dn);
  st.selectable.hover_active = nk_style_item_color(accent);
  st.selectable.pressed_active = nk_style_item_color(accent);
  st.selectable.text_normal = text;
  st.selectable.text_hover = text;
  st.selectable.text_pressed = text;
  st.selectable.text_normal_active = nk_rgb(255, 255, 255);
  st.selectable.text_hover_active = nk_rgb(255, 255, 255);
  st.selectable.text_pressed_active = nk_rgb(255, 255, 255);
  st.selectable.rounding = 4.0f;

  st.property.normal = nk_style_item_color(inset);
  st.property.hover = nk_style_item_color(inset);
  st.property.active = nk_style_item_color(inset);
  st.property.border_color = border;
  st.property.label_normal = text;
  st.property.label_hover = text;
  st.property.label_active = text;
  st.property.border = 1.0f;
  st.property.rounding = 5.0f;
  st.property.edit = st.edit;
  st.property.edit.padding = nk_vec2(0, 0);
  st.property.edit.border = 0.0f;
  st.property.edit.rounding = 0.0f;
  st.property.inc_button = st.button;
  st.property.dec_button = st.button;
  st.property.inc_button.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.property.dec_button.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
  st.property.inc_button.hover = nk_style_item_color(raised);
  st.property.dec_button.hover = nk_style_item_color(raised);
  st.property.inc_button.text_normal = text_dim;
  st.property.dec_button.text_normal = text_dim;
  st.property.inc_button.padding = nk_vec2(0, 0);
  st.property.dec_button.padding = nk_vec2(0, 0);
  st.property.inc_button.border = 0.0f;
  st.property.dec_button.border = 0.0f;

  st.tab.background = nk_style_item_color(panel);
  st.tab.border_color = border;
  st.tab.text = text;
}

} // namespace xpbd::app
