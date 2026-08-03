#include "nuklear_ui_internal.hpp"

#include "xpbd/app/i18n.hpp"

#include <algorithm>
#include <cstdio>

namespace xpbd::app::ui_internal {

void drawDebugStatusPanel(UiPanelContext &ui) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;
  const gfx::FrameStats &stats = ui.stats;
  const char *backend_name = ui.backend_name;
  const char *device_name = ui.device_name;

  heading(ctx, g, tr("debug_options"));
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, session.show_debug_hud ? tr("hide_debug")
                                                  : tr("show_debug"))) {
    session.show_debug_hud = !session.show_debug_hud;
  }
  check(ctx, g, tr("debug_instant_sample"), session.debug_instant_sample,
        false);
  muted(ctx, g, tr("debug_sample_hint"));
  if (session.show_debug_hud) {



    char upload_bytes[32];
    char bone_bytes[32];
    char resource_bytes[32];
    char static_vertex_bytes[32];
    char static_index_bytes[32];
    char rt_allocated_bytes[32];
    char rt_as_storage_bytes[32];
    char rt_scratch_bytes[32];
    formatByteCount(upload_bytes, sizeof(upload_bytes),
                    session.debug_upload_bytes);
    formatByteCount(bone_bytes, sizeof(bone_bytes),
                    session.debug_static_bone_upload_bytes);
    formatByteCount(resource_bytes, sizeof(resource_bytes),
                    session.debug_static_resource_upload_bytes);
    formatByteCount(static_vertex_bytes, sizeof(static_vertex_bytes),
                    static_cast<double>(
                        session.debug_static_model_vertex_bytes));
    formatByteCount(static_index_bytes, sizeof(static_index_bytes),
                    static_cast<double>(
                        session.debug_static_model_index_bytes));
    formatByteCount(rt_allocated_bytes, sizeof(rt_allocated_bytes),
                    static_cast<double>(stats.rt_allocated_bytes));
    formatByteCount(rt_as_storage_bytes, sizeof(rt_as_storage_bytes),
                    static_cast<double>(stats.rt_as_storage_bytes));
    formatByteCount(rt_scratch_bytes, sizeof(rt_scratch_bytes),
                    static_cast<double>(stats.rt_scratch_bytes));

    char lines[17][160];
    if (session.debug_dlss_frame_generation_active) {
      std::snprintf(
          lines[0], sizeof(lines[0]),
          "Original %.1f | DLSS-FG %.1f FPS [%s]",
          session.debug_original_fps,
          session.debug_dlss_fg_fps,
          session.debug_instant_sample ? "live" : "1s");
    } else {
      std::snprintf(
          lines[0], sizeof(lines[0]),
          "Original %.1f FPS | DLSS-FG off [%s]",
          session.debug_original_fps,
          session.debug_instant_sample ? "live" : "1s");
    }
    std::snprintf(lines[1], sizeof(lines[1]), "Frame %.2f ms",
                  session.debug_ema_frame_ms);
    std::snprintf(lines[2], sizeof(lines[2]), "Mesh/Upload %.2f/%.2f ms",
                  session.debug_mesh_ms, session.debug_upload_ms);
    std::snprintf(lines[3], sizeof(lines[3]),
                  "Pick CPU/f %.2f ms | query/rebuild %.2f/%.2f",
                  session.debug_pick_ms, session.debug_pick_queries,
                  session.debug_pick_cache_rebuilds);
    std::snprintf(lines[4], sizeof(lines[4]),
                  "Pick faces %u/%u (candidate/total)",
                  static_cast<unsigned>(
                      session.debug_pick_candidate_faces),
                  static_cast<unsigned>(session.debug_pick_total_faces));
    std::snprintf(lines[5], sizeof(lines[5]),
                  "Upload/f %s | bones %s | static %s", upload_bytes,
                  bone_bytes, resource_bytes);
    std::snprintf(lines[6], sizeof(lines[6]),
                  "Realloc/f %.2f | total %llu | rebuilds %llu",
                  session.debug_buffer_reallocations,
                  static_cast<unsigned long long>(
                      session.debug_total_buffer_reallocations),
                  static_cast<unsigned long long>(
                      session.debug_static_resource_rebuilds));
    std::snprintf(
        lines[7], sizeof(lines[7]),
        "Static VB %s | IB %s | O/C/B %u/%u/%u", static_vertex_bytes,
        static_index_bytes,
        static_cast<unsigned>(session.debug_static_opaque_index_count),
        static_cast<unsigned>(session.debug_static_cutout_index_count),
        static_cast<unsigned>(session.debug_static_blend_index_count));
    std::snprintf(lines[8], sizeof(lines[8]), "Backend CPU %.2f ms",
                  session.debug_backend_cpu_ms);
    if (session.debug_gpu_timestamp_valid) {
      std::snprintf(lines[9], sizeof(lines[9]), "GPU timestamp %.2f ms",
                    session.debug_gpu_timestamp_total_ms);
      std::snprintf(lines[10], sizeof(lines[10]),
                    "GPU U/O/T/L %.2f/%.2f/%.2f/%.2f ms",
                    session.debug_gpu_timestamp_ui_ms,
                    session.debug_gpu_timestamp_opaque_ms,
                    session.debug_gpu_timestamp_transparent_ms,
                    session.debug_gpu_timestamp_lines_ms);
    } else {
      std::snprintf(lines[9], sizeof(lines[9]),
                    "GPU timestamp unavailable");
      std::snprintf(lines[10], sizeof(lines[10]), "GPU U/O/T/L n/a");
    }
    std::snprintf(lines[11], sizeof(lines[11]), "Backend %s",
                  backend_name ? backend_name : "-");
    std::snprintf(lines[12], sizeof(lines[12]), "Device %.40s",
                  device_name ? device_name : "-");
    std::snprintf(lines[13], sizeof(lines[13]),
                  "Cubes %d | VSync %s | Path %s%s",
                  session.debug_cube_count,
                  session.vsync_enabled ? "on" : "off",
                  stats.ray_tracing_requested
                      ? (stats.active_render_path == 1 ? "RT" : "Raster*")
                      : "Raster",
                  stats.ray_tracing_supported ? " (RT ok)" : " (no RT)");
    std::snprintf(lines[14], sizeof(lines[14]),
                  "RT AS B/T/I %u/%u/%u | triangles %u",
                  static_cast<unsigned>(stats.rt_blas_count),
                  static_cast<unsigned>(stats.rt_tlas_count),
                  static_cast<unsigned>(stats.rt_instance_count),
                  static_cast<unsigned>(stats.rt_primitive_count));
    std::snprintf(lines[15], sizeof(lines[15]),
                  "RT alloc %s | AS %s | scratch %s",
                  rt_allocated_bytes, rt_as_storage_bytes,
                  rt_scratch_bytes);
    std::snprintf(
        lines[16], sizeof(lines[16]),
        "RT builds full/refit %llu/%llu | last %s",
        static_cast<unsigned long long>(stats.rt_full_builds),
        static_cast<unsigned long long>(stats.rt_refits),
        gfx::rtAccelerationBuildReasonName(
            stats.rt_last_build_reason));
    for (auto &line : lines) {
      muted(ctx, g, line);
    }
  }
}

void drawStatusPanel(UiPanelContext &ui, float inner_width) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;
  const bool busy = ui.busy;
  const float inner_w = inner_width;
  const float bottom_row_h = g.btn;

  nk_layout_row_begin(ctx, NK_STATIC, bottom_row_h, 8);
  auto bar_btn = [&](const char *lab, float w, bool en, auto fn) {
    nk_layout_row_push(ctx, w);
    if (!en) {
      nk_widget_disable_begin(ctx);
    }
    if (nk_button_label(ctx, lab) && en) {
      fn();
    }
    if (!en) {
      nk_widget_disable_end(ctx);
    }
  };
  float b_step = 52.0f * g.s;
  float b_bake = 60.0f * g.s;
  float b_cancel = 68.0f * g.s;
  float b_reset = 60.0f * g.s;
  float b_prog = 120.0f * g.s;
  float b_frames = 72.0f * g.s;
  float b_export = 100.0f * g.s;
  float b_need = b_step + b_bake + b_cancel + b_reset + b_prog + b_frames +
                 b_export + 24.0f * g.s;
  if (b_need > inner_w) {
    const float k = inner_w / b_need;
    b_step *= k;
    b_bake *= k;
    b_cancel *= k;
    b_reset *= k;
    b_prog *= k;
    b_frames *= k;
    b_export *= k;
  }
  const float b_status =
      (std::max)(32.0f, inner_w -
                            (b_step + b_bake + b_cancel + b_reset + b_prog +
                             b_frames + b_export) -
                            8.0f * g.s);

  bar_btn(tr("step"), b_step, !busy, [&] { session.stepBake(); });
  bar_btn(tr("bake"), b_bake, !busy, [&] { session.startBake(); });
  const bool cancelling = session.bake_state == BakeState::Cancelling;
  bar_btn(busy ? tr("cancel") : tr("idle"), b_cancel,
          busy && !cancelling, [&] {
    if (session.bake_busy.load()) {
      session.cancelBake();
    }
  });
  bar_btn(tr("reset"), b_reset, !busy, [&] { session.resetBake(); });
  {
    const int cur = session.bake_current.load();
    const int tot = session.bake_total.load();
    const float prog =
        tot > 0 ? static_cast<float>(cur) / static_cast<float>(tot) : 0.0f;
    nk_layout_row_push(ctx, b_prog);
    nk_size p = static_cast<nk_size>(prog * 1000);
    nk_progress(ctx, &p, 1000, NK_FIXED);
    nk_layout_row_push(ctx, b_frames);
    char fl[32];
    std::snprintf(fl, sizeof(fl), "%d/%d", cur, tot);
    nk_label(ctx, fl, NK_TEXT_LEFT);
  }
  nk_layout_row_push(ctx, b_status);
  char compact_status[256];
  std::snprintf(compact_status, sizeof(compact_status), "%s / %s: %s",
                bakeStateName(session.bake_state),
                workerPhaseName(session.worker_phase),
                session.status.c_str());
  nk_label_colored(ctx, compact_status, NK_TEXT_LEFT,
                   nk_rgb(170, 177, 194));
  nk_layout_row_push(ctx, b_export);
  nk_style_push_style_item(ctx, &ctx->style.button.normal,
                           nk_style_item_color(nk_rgba(52, 158, 116, 255)));
  const bool can_export_diagnostics =
      !busy && session.finalResult() != nullptr;
  if (!can_export_diagnostics) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("diagnostics_export")) &&
      can_export_diagnostics) {
    if (auto p = saveFileDialog(
            L"Export Bake Diagnostics",
            L"Diagnostics JSON (*.json)\0*.json\0All\0*.*\0",
            L"animation.bake-diagnostics.json")) {
      session.exportDiagnostics(*p);
    }
  }
  if (!can_export_diagnostics) {
    nk_widget_disable_end(ctx);
  }
  nk_style_pop_style_item(ctx);
  nk_layout_row_end(ctx);
}

} // namespace xpbd::app::ui_internal
