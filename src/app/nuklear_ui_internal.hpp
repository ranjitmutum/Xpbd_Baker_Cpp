#pragma once

#include "xpbd/app/app_session.hpp"
#include "xpbd/app/nuklear_ui.hpp"

#include "nuklear.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace xpbd::app::ui_internal {

struct Geom {
  float menu_h = 30.0f;
  float bottom_h = 46.0f;
  float left_w = 250.0f;
  float right_w = 360.0f;
  float btn = 26.0f;
  float row = 22.0f;
  float label = 16.0f;
  float s = 1.0f;
};

enum class SplitterDrag { None, Left, Right };

struct UiPersistentState {
  bool widths_initialized = false;
  float preferred_left_w = 0.0f;
  float preferred_right_w = 0.0f;
  SplitterDrag splitter_drag = SplitterDrag::None;
  float drag_anchor_x = 0.0f;
  float drag_anchor_width = 0.0f;
  float drag_anchor_other_width = 0.0f;
  std::set<std::string> expanded_bones;
  std::string bone_hierarchy_signature;
  std::string last_selected_bone;
  bool scroll_tree_to_selection = false;
  int properties_page = 0;
};

struct UiPanelContext {
  nk_context *nk = nullptr;
  AppSession &session;
  const Geom &geom;
  bool busy = false;
  const gfx::FrameStats &stats;
  const gfx::RayTracingCapability *rt_cap = nullptr;
  const char *backend_name = nullptr;
  const char *device_name = nullptr;
};

struct UiModalLayout {
  float left_width = 0.0f;
  float right_width = 0.0f;
  float menu_height = 0.0f;
  float bottom_height = 0.0f;
};

struct PanelWidths {
  float left = 0.0f;
  float center = 0.0f;
  float right = 0.0f;
  float splitter = 0.0f;
};

UiPersistentState &uiState();
Geom makeGeom(float width, float height);

void formatByteCount(char *buffer, std::size_t buffer_size, double bytes);
void markDirty(
    const char *reason,
    InvalidationReason invalidation_reason = InvalidationReason::GlobalConfig);
const char *bakeStateName(BakeState state);
const char *workerPhaseName(WorkerPhase phase);

bool slider(nk_context *ctx, const Geom &g, const char *label, float &value,
            float lo, float hi, bool busy, float requested_step = 0.0f);
bool intProperty(nk_context *ctx, const Geom &g, const char *label, int &value,
                 int lo, int hi, int step, bool busy);
bool combo(nk_context *ctx, const Geom &g, const char *label,
           const std::vector<const char *> &items, int &selected, bool busy);
bool check(nk_context *ctx, const Geom &g, const char *label, bool &value,
           bool busy);
void heading(nk_context *ctx, const Geom &g, const char *text);
void muted(nk_context *ctx, const Geom &g, const char *text);
void mutedWrap(nk_context *ctx, const Geom &g, const char *text);
bool treeRowButton(nk_context *ctx, const char *label, bool selected,
                   bool viewport_hovered = false);
bool flatSymbolButton(nk_context *ctx, enum nk_symbol_type symbol);
bool eyeButton(nk_context *ctx, bool visible);

void drawMenuBar(UiPanelContext &ui, float inner_width, bool &show_about);
bool drawModalPanel(UiPanelContext &ui, bool &show_about,
                    const UiModalLayout &layout, UiFrameResult &result);
void drawBonePanel(UiPanelContext &ui, float panel_width, float panel_height,
                   UiPersistentState &state);
void drawViewportOverlay(UiPanelContext &ui, float panel_width,
                         float panel_height, UiFrameResult &result);
void drawPropertiesPanel(UiPanelContext &ui, UiPersistentState &state);
void drawBakePanel(UiPanelContext &ui);
void drawLabPbrPanel(UiPanelContext &ui);
void drawRenderPanel(UiPanelContext &ui);
void drawWorldPanel(UiPanelContext &ui);
void drawWorldOptions(UiPanelContext &ui);
void drawDebugStatusPanel(UiPanelContext &ui);
void drawStatusPanel(UiPanelContext &ui, float inner_width);

void drawBoneHierarchy(nk_context *ctx, const Geom &g, AppSession &session,
                       float panel_width, bool busy,
                       UiPersistentState &state);
void drawSelectedBoneOverrideEditor(nk_context *ctx, const Geom &g,
                                    AppSession &session, bool busy);
void drawBoneContextPopup(nk_context *ctx, const Geom &g, AppSession &session,
                          bool busy, float window_width, float window_height,
                          UiFrameResult &result);
void drawLabPbrEditor(nk_context *ctx, const Geom &g, AppSession &session);
void drawRendererEditor(nk_context *ctx, const Geom &g, AppSession &session,
                        const gfx::FrameStats &stats,
                        const gfx::RayTracingCapability *rt_cap);
void drawSkyEditor(nk_context *ctx, const Geom &g, AppSession &session);

PanelWidths calculatePanelWidths(float inner_width, const Geom &g,
                                 float spacing, UiPersistentState &state);
bool drawSplitter(nk_context *ctx, SplitterDrag side, float current_width,
                  float other_width, UiPersistentState &state);

} // namespace xpbd::app::ui_internal
