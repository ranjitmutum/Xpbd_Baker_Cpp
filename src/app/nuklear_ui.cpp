#include "xpbd/app/nuklear_ui.hpp"

#include "xpbd/app/app_session.hpp"
#include "xpbd/app/i18n.hpp"

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
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xpbd::app {
namespace {

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
};

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
    InvalidationReason invalidation_reason = InvalidationReason::GlobalConfig) {
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

bool slider(nk_context *ctx, const Geom &g, const char *label, float &value,
            float lo, float hi, bool busy, float requested_step = 0.0f) {
  float v = value;
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
    changed |= nk_property_float(ctx, "#", lo, &v, hi, step, step * 0.1f);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_push(ctx, slider_width);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
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
    changed |= nk_property_float(ctx, "#", lo, &v, hi, step, step * 0.1f);
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, g.row, 1);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
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
    changed |=
        nk_property_int(ctx, "#", lo, &v, hi, step, static_cast<float>(step));
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_push(ctx, slider_width);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
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
    changed |=
        nk_property_int(ctx, "#", lo, &v, hi, step, static_cast<float>(step));
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, g.row, 1);
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
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

void heading(nk_context *ctx, const Geom &g, const char *t) {
  nk_layout_row_dynamic(ctx, g.row + 2.0f, 1);
  nk_label_colored(ctx, t, NK_TEXT_LEFT, nk_rgb(210, 210, 220));
}

void muted(nk_context *ctx, const Geom &g, const char *t) {
  nk_layout_row_dynamic(ctx, g.label, 1);
  nk_label_colored(ctx, t, NK_TEXT_LEFT, nk_rgb(170, 170, 170));
}

void mutedWrap(nk_context *ctx, const Geom &g, const char *t) {
  nk_layout_row_dynamic(ctx, g.label * 2.0f, 1);
  nk_style_push_color(ctx, &ctx->style.text.color, nk_rgb(170, 170, 170));
  nk_label_wrap(ctx, t);
  nk_style_pop_color(ctx);
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

std::string boneHierarchySignature(const std::vector<loader::Bone> &bones) {
  std::string signature;
  for (const auto &bone : bones) {
    signature.append(bone.name);
    signature.push_back('\x1f');
    signature.push_back(bone.has_parent ? '1' : '0');
    signature.append(bone.parent);
    signature.push_back('\x1e');
  }
  return signature;
}

void drawBoneHierarchy(nk_context *ctx, const Geom &g, AppSession &session,
                       float panel_width, bool busy, UiPersistentState &state) {
  const auto &bones = session.bone_mapper.allBones();
  const std::string signature = boneHierarchySignature(bones);
  if (signature != state.bone_hierarchy_signature) {
    state.bone_hierarchy_signature = signature;
    state.expanded_bones.clear();
  }

  std::set<std::string> known_names;
  for (const auto &bone : bones) {
    known_names.insert(bone.name);
  }

  std::map<std::string, std::vector<const loader::Bone *>> children;
  std::vector<const loader::Bone *> roots;
  for (const auto &bone : bones) {
    if (bone.has_parent && bone.parent != bone.name &&
        known_names.contains(bone.parent)) {
      children[bone.parent].push_back(&bone);
    } else {
      roots.push_back(&bone);
    }
  }

  std::set<std::string> visited;
  std::set<std::string> structurally_accounted;
  std::function<void(const loader::Bone &)> mark_structure;
  mark_structure = [&](const loader::Bone &bone) {
    if (!structurally_accounted.insert(bone.name).second) {
      return;
    }
    const auto child_it = children.find(bone.name);
    if (child_it != children.end()) {
      for (const loader::Bone *child : child_it->second) {
        mark_structure(*child);
      }
    }
  };

  std::function<void(const loader::Bone &, int)> draw_bone;
  draw_bone = [&](const loader::Bone &bone, int depth) {
    if (!visited.insert(bone.name).second) {
      return;
    }

    const auto child_it = children.find(bone.name);
    const bool has_children =
        child_it != children.end() && !child_it->second.empty();
    const bool expanded = state.expanded_bones.contains(bone.name);
    const bool physics = session.bone_mapper.isPhysicsBone(bone.name);
    const bool collision = session.bone_mapper.isCollisionRoot(bone.name);
    const bool visible = session.isBoneVisible(bone.name);
    const bool selected = session.selected_bone_name == bone.name;

    const float content_width =
        (std::max)(80.0f, nk_window_get_content_region_size(ctx).x);
    const float tree_button = std::clamp(18.0f * g.s, 16.0f, 24.0f);
    const float flag_width = std::clamp(22.0f * g.s, 20.0f, 26.0f);
    const float spacing = ctx->style.window.spacing.x * 5.0f;
    const float max_indent =
        (std::max)(1.0f, content_width - tree_button - flag_width * 3.0f -
                             spacing - 44.0f);
    const float indent =
        (std::min)(max_indent, (std::max)(1.0f, depth * 14.0f * g.s));
    const float row_width =
        (std::min)(content_width, panel_width - 24.0f * g.s);
    const float name_width =
        (std::max)(40.0f, row_width - indent - tree_button - flag_width * 3.0f -
                              spacing);

    nk_layout_row_begin(ctx, NK_STATIC, g.row, 6);
    nk_layout_row_push(ctx, indent);
    nk_label(ctx, "", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, tree_button);
    if (has_children) {
      if (nk_button_symbol(ctx, expanded ? NK_SYMBOL_TRIANGLE_DOWN
                                         : NK_SYMBOL_TRIANGLE_RIGHT)) {
        if (expanded) {
          state.expanded_bones.erase(bone.name);
        } else {
          state.expanded_bones.insert(bone.name);
        }
      }
    } else {
      nk_label(ctx, "", NK_TEXT_LEFT);
    }

    nk_layout_row_push(ctx, flag_width);
    nk_bool physics_checked = physics ? nk_true : nk_false;
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    const bool physics_changed =
        nk_checkbox_label(ctx, "", &physics_checked) != 0;
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    if (physics_changed && !busy) {
      session.togglePhysicsBone(bone.name, physics_checked != 0);
    }

    nk_layout_row_push(ctx, flag_width);
    nk_bool collision_checked = collision ? nk_true : nk_false;
    if (busy) {
      nk_widget_disable_begin(ctx);
    }
    const bool collision_changed =
        nk_checkbox_label(ctx, "", &collision_checked) != 0;
    if (busy) {
      nk_widget_disable_end(ctx);
    }
    if (collision_changed && !busy) {
      session.setCollisionRoot(bone.name, collision_checked != 0);
    }

    nk_layout_row_push(ctx, flag_width);
    if (eyeButton(ctx, visible)) {
      session.setBoneVisible(bone.name, !visible);
    }

    nk_layout_row_push(ctx, name_width);
    const std::string label = selected ? "> " + bone.name : bone.name;
    if (nk_button_label(ctx, label.c_str())) {
      session.selectBone(bone.name);
    }
    nk_layout_row_end(ctx);

    if (has_children && expanded) {
      for (const loader::Bone *child : child_it->second) {
        draw_bone(*child, depth + 1);
      }
    }
  };

  for (const loader::Bone *root : roots) {
    mark_structure(*root);
    draw_bone(*root, 0);
  }




  for (const auto &bone : bones) {
    if (!structurally_accounted.contains(bone.name)) {
      mark_structure(bone);
      draw_bone(bone, 0);
    }
  }
}

void drawBoneContextCard(nk_context *ctx, const Geom &g, AppSession &session,
                         bool busy) {
  const std::string bone_name = session.bone_context_bone_name;
  const auto selected_it = std::find_if(
      session.geometry.bones.begin(), session.geometry.bones.end(),
      [&](const loader::Bone &bone) { return bone.name == bone_name; });
  if (!session.bone_context_open ||
      selected_it == session.geometry.bones.end()) {
    session.closeBoneContext();
    return;
  }

  heading(ctx, g, tr("bone_context_title"));
  const std::string bone_label = std::string(tr("bone_prefix")) + bone_name;
  muted(ctx, g, bone_label.c_str());

  bool physics = session.bone_mapper.isPhysicsBone(bone_name);
  if (check(ctx, g, tr("bone_context_physics_group"), physics, busy)) {
    session.togglePhysicsBone(bone_name, physics);
  }

  bool collision = session.bone_mapper.isCollisionRoot(bone_name);
  if (check(ctx, g, tr("bone_context_collision"), collision, busy)) {
    session.setCollisionRoot(bone_name, collision);
  }

  bool visible = session.isBoneVisible(bone_name);
  if (check(ctx, g, tr("bone_context_visible"), visible, false)) {
    session.setBoneVisible(bone_name, visible);
  }

  const loader::Bone *parent = nullptr;
  if (selected_it->has_parent && !selected_it->parent.empty() &&
      selected_it->parent != bone_name) {
    const auto parent_it = std::find_if(
        session.geometry.bones.begin(), session.geometry.bones.end(),
        [&](const loader::Bone &bone) {
          return bone.name == selected_it->parent;
        });
    if (parent_it != session.geometry.bones.end()) {
      parent = &*parent_it;
    }
  }

  heading(ctx, g, tr("bone_context_parent"));
  if (parent != nullptr) {
    nk_layout_row_dynamic(ctx, g.btn, 1);
    if (nk_button_label(ctx, parent->name.c_str())) {
      session.openBoneContext(parent->name);
      return;
    }
  } else {
    muted(ctx, g, tr("bone_context_no_parent"));
  }

  heading(ctx, g, tr("bone_context_children"));
  bool has_children = false;
  for (const auto &bone : session.geometry.bones) {
    if (!bone.has_parent || bone.parent != bone_name || bone.name == bone_name) {
      continue;
    }
    has_children = true;
    nk_layout_row_dynamic(ctx, g.btn, 1);
    if (nk_button_label(ctx, bone.name.c_str())) {
      session.openBoneContext(bone.name);
      return;
    }
  }
  if (!has_children) {
    muted(ctx, g, tr("bone_context_no_children"));
  }

  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("close"))) {
    session.closeBoneContext();
  }
}

struct PanelWidths {
  float left = 0.0f;
  float center = 0.0f;
  float right = 0.0f;
  float splitter = 0.0f;
};

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
    nk_fill_rect(nk_window_get_canvas(ctx), bar, 0.0f,
                 active    ? nk_rgb(100, 165, 255)
                 : hovered ? nk_rgb(80, 130, 205)
                           : nk_rgb(90, 95, 110));
  }
  return hovered || active;
}

}

void applyDarkStyle(nk_context *ctx, float ) {

  auto &st = ctx->style;
  const nk_color bg = nk_rgba(40, 42, 48, 255);
  const nk_color panel = nk_rgba(50, 52, 60, 255);
  const nk_color border = nk_rgba(90, 95, 110, 255);
  const nk_color text = nk_rgba(235, 238, 245, 255);
  const nk_color accent = nk_rgba(70, 130, 220, 255);
  const nk_color btn = nk_rgba(62, 66, 78, 255);
  const nk_color btn_h = nk_rgba(80, 100, 150, 255);

  st.window.background = bg;
  st.window.fixed_background = nk_style_item_color(bg);
  st.window.border_color = border;
  st.window.border = 1.0f;
  st.window.rounding = 0.0f;
  st.window.padding = nk_vec2(6, 6);
  st.window.spacing = nk_vec2(4, 4);
  st.window.scrollbar_size = nk_vec2(14, 14);

  st.button.normal = nk_style_item_color(btn);
  st.button.hover = nk_style_item_color(btn_h);
  st.button.active = nk_style_item_color(accent);
  st.button.border_color = border;
  st.button.text_normal = text;
  st.button.text_hover = nk_rgb(255, 255, 255);
  st.button.text_active = nk_rgb(255, 255, 255);
  st.button.rounding = 4.0f;

  st.button.padding = nk_vec2(6, 3);
  st.button.border = 1.0f;

  st.checkbox.normal = nk_style_item_color(panel);
  st.checkbox.hover = nk_style_item_color(btn_h);
  st.checkbox.cursor_normal = nk_style_item_color(accent);
  st.checkbox.cursor_hover = nk_style_item_color(nk_rgb(120, 170, 255));
  st.checkbox.text_normal = text;
  st.checkbox.text_hover = text;
  st.checkbox.text_active = text;

  st.slider.bar_normal = nk_rgba(45, 48, 58, 255);
  st.slider.bar_hover = nk_rgba(55, 60, 75, 255);
  st.slider.bar_active = nk_rgba(60, 80, 120, 255);
  st.slider.bar_filled = accent;
  st.slider.cursor_normal = nk_style_item_color(accent);
  st.slider.cursor_hover = nk_style_item_color(nk_rgb(130, 180, 255));
  st.slider.cursor_active = nk_style_item_color(nk_rgb(150, 200, 255));
  st.slider.cursor_size = nk_vec2(16, 16);

  st.progress.normal = nk_style_item_color(panel);
  st.progress.cursor_normal = nk_style_item_color(accent);
  st.progress.cursor_hover = nk_style_item_color(accent);
  st.progress.cursor_active = nk_style_item_color(accent);

  st.scrollv.normal = nk_style_item_color(panel);
  st.scrollv.hover = nk_style_item_color(btn);
  st.scrollv.active = nk_style_item_color(btn_h);
  st.scrollv.cursor_normal = nk_style_item_color(btn_h);
  st.scrollv.cursor_hover = nk_style_item_color(accent);
  st.scrollv.cursor_active = nk_style_item_color(accent);
  st.scrollh = st.scrollv;

  st.text.color = text;
  st.edit.normal = nk_style_item_color(panel);
  st.edit.hover = nk_style_item_color(btn);
  st.edit.active = nk_style_item_color(btn_h);
  st.edit.border_color = border;
  st.edit.text_normal = text;
  st.edit.text_hover = text;
  st.edit.text_active = text;



  st.property.normal = nk_style_item_color(panel);
  st.property.hover = nk_style_item_color(btn);
  st.property.active = nk_style_item_color(btn_h);
  st.property.border_color = border;
  st.property.label_normal = text;
  st.property.label_hover = text;
  st.property.label_active = text;
  st.property.border = 1.0f;
  st.property.rounding = 3.0f;
  st.property.edit = st.edit;
  st.property.edit.padding = nk_vec2(0, 0);
  st.property.edit.border = 0.0f;
  st.property.edit.rounding = 0.0f;
  st.property.inc_button = st.button;
  st.property.dec_button = st.button;
  st.property.inc_button.padding = nk_vec2(0, 0);
  st.property.dec_button.padding = nk_vec2(0, 0);
  st.property.inc_button.border = 0.0f;
  st.property.dec_button.border = 0.0f;
}

UiFrameResult composeNuklearUi(nk_context *ctx, int win_w, int win_h,
                               float ui_scale, const char *backend_name,
                               const char *device_name,
                               const gfx::FrameStats &stats) {
  (void)stats;

  UiFrameResult result;
  auto &session = AppSession::instance();
  session.pollBakeProgress();
  static bool show_about = false;

  const float W = static_cast<float>((std::max)(win_w, 640));
  const float H = static_cast<float>((std::max)(win_h, 480));
  const Geom g = makeGeom(W, H);
  const bool busy = session.bake_busy.load();
  UiPersistentState &ui_state = uiState();
  if (!ui_state.widths_initialized) {
    ui_state.widths_initialized = true;
    ui_state.preferred_left_w = g.left_w;
    ui_state.preferred_right_w = g.right_w;
  }

  bool finish_splitter_drag = false;
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
    float w_ev = 80.0f * g.s;
    float w_about = 72.0f * g.s;
    float need = w_om + w_oa + w_tex + w_ea + w_ev + w_about + gap * 5.0f;
    if (need > inner_w) {
      const float k = inner_w / need;
      w_om *= k;
      w_oa *= k;
      w_tex *= k;
      w_ea *= k;
      w_ev *= k;
      w_about *= k;
    }

    nk_layout_row_begin(ctx, NK_STATIC, menu_row_h, 6);
    file_btn(tr("open_model"), w_om, true, [&] {
      if (auto p = openFileDialog(
              L"Open Bedrock/Blockbench Model",
              L"Models "
              L"(*.geo.json;*.json;*.bbmodel)\0*.geo.json;*.json;*.bbmodel\0"
              L"Blockbench (*.bbmodel)\0*.bbmodel\0"
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
                     busy ? nk_rgb(144, 202, 249) : nk_rgb(160, 165, 180));
    nk_layout_row_push(ctx, w_clr);
    if (nk_button_label(ctx, tr("clear_tex")) &&
        session.model_texture.valid()) {
      session.clearTexture();
    }
    nk_layout_row_end(ctx);

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
                               nk_style_item_color(nk_rgba(176, 83, 66, 255)));
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
                               nk_style_item_color(nk_rgba(176, 83, 66, 255)));
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


      nk_layout_row_begin(ctx, NK_STATIC, mid_row_h, 5);


      nk_layout_row_push(ctx, use_left);
      if (nk_group_begin(ctx, "left", NK_WINDOW_BORDER)) {
        muted(ctx, g, tr("bone_hierarchy"));
        mutedWrap(ctx, g, tr("bone_tree_legend"));
        nk_layout_row_dynamic(
            ctx, (std::max)(48.0f, mid_row_h * 0.55f - g.label * 2.0f - 24.0f),
            1);
        if (nk_group_begin(ctx, "bones", NK_WINDOW_BORDER)) {
          drawBoneHierarchy(ctx, g, session, use_left, busy, ui_state);
          if (session.bone_mapper.allBones().empty()) {
            muted(ctx, g, tr("open_model_hint"));
          }
          nk_group_end(ctx);
        }
        muted(ctx, g, tr("animations"));
        nk_layout_row_dynamic(ctx, mid_row_h * 0.32f, 1);
        if (nk_group_begin(ctx, "anims", NK_WINDOW_BORDER)) {
          auto draw_animation_row = [&](const std::string &name) {
            const bool sel = session.selected_animation_name == name;
            nk_layout_row_dynamic(ctx, g.btn, 1);
            const std::string lab = sel ? ("> " + name) : name;


            if (nk_button_label(ctx, lab.c_str()) && !busy && !sel) {
              session.selectAnimation(name);
            }
          };
          if (!session.animation_root.animation_order.empty()) {
            for (const auto &name : session.animation_root.animation_order) {
              if (session.animation_root.animations.contains(name)) {
                draw_animation_row(name);
              }
            }
          } else {
            for (const auto &[name, anim] : session.animation_root.animations) {
              (void)anim;
              draw_animation_row(name);
            }
          }
          if (session.animation_root.animations.empty()) {
            muted(ctx, g, tr("open_anim_hint"));
          }
          nk_group_end(ctx);
        }
        nk_group_end(ctx);
      }

      nk_layout_row_push(ctx, panel_widths.splitter);
      result.layout.horizontal_resize_cursor |=
          drawSplitter(ctx, SplitterDrag::Left, use_left, use_right, ui_state);


      nk_layout_row_push(ctx, use_center);
      if (nk_group_begin(ctx, "center", NK_WINDOW_BORDER)) {
        const float tool = g.btn + 8.0f;
        const float info = g.label + 4.0f;
        const float mode_row = g.btn;
        const float context_card_h =
            session.bone_context_open
                ? (std::max)(220.0f * g.s, g.btn * 8.5f)
                : 0.0f;
        const float context_spacing =
            session.bone_context_open ? ctx->style.window.spacing.y : 0.0f;
        const float vp_h =
            (std::max)(40.0f, mid_row_h - tool - info - mode_row -
                                   context_card_h - context_spacing - 32.0f);
        nk_layout_row_dynamic(ctx, vp_h, 1);
        struct nk_rect space{};
        nk_widget(&space, ctx);

        result.layout.vp_x = space.x;
        result.layout.vp_y = space.y;
        result.layout.vp_w = space.w;
        result.layout.vp_h = space.h;
        result.layout.viewport_hovered =
            nk_input_is_mouse_hovering_rect(&ctx->input, space) != 0;

        if (session.bone_context_open) {
          nk_layout_row_dynamic(ctx, context_card_h, 1);
          if (nk_group_begin(ctx, "bone_context_card", NK_WINDOW_BORDER)) {
            drawBoneContextCard(ctx, g, session, busy);
            nk_group_end(ctx);
          }
        }

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
        nk_label_colored(ctx, info_buf, NK_TEXT_LEFT, nk_rgb(180, 185, 200));

        nk_layout_row_dynamic(ctx, mode_row, 3);
        const auto preview_mode_button = [&](const char *label,
                                             PresentationMode mode,
                                             bool available) {
          if (!available) {
            nk_widget_disable_begin(ctx);
          }
          if (nk_button_label(ctx, label) && available &&
              session.presentation_mode != mode) {
            session.setPresentationMode(mode);
          }
          if (!available) {
            nk_widget_disable_end(ctx);
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
            const auto *result = session.finalResult();
            if (result != nullptr && result->frames != nullptr &&
                result->frames->size() >= 2) {
              const double start_time = result->frames->front().time;
              const double end_time = result->frames->back().time;
              const double span = end_time - start_time;
              if (std::isfinite(span) && span > 0.0) {
                auto *canvas = nk_window_get_canvas(ctx);
                for (const auto &marker :
                     result->diagnostics.loop.danger_markers) {
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
        nk_group_end(ctx);
      }

      nk_layout_row_push(ctx, panel_widths.splitter);
      result.layout.horizontal_resize_cursor |=
          drawSplitter(ctx, SplitterDrag::Right, use_right, use_left, ui_state);


      nk_layout_row_push(ctx, use_right);
      if (nk_group_begin(ctx, "props", NK_WINDOW_BORDER)) {
        heading(ctx, g, tr("global_physics"));
        muted(ctx, g,
              session.solver_mode == 0 ? tr("solver_xpbd")
                                       : tr("solver_bullet"));
        nk_layout_row_dynamic(ctx, g.btn, 2);
        if (nk_button_label(ctx, tr("xpbd")) && !busy) {
          session.setSolverMode(0);
        }
        if (nk_button_label(ctx, tr("bullet")) && !busy) {
          session.setSolverMode(1);
        }

        auto slide = [&](const char *l, float &v, float lo, float hi,
                         float step = 0.0f, bool disabled = false) {
          if (slider(ctx, g, l, v, lo, hi, busy || disabled, step)) {
            markDirty("Physics parameters changed — play to rebake");
          }
        };

        if (intProperty(ctx, g, tr("rb_substeps"), session.rigid_substeps, 1,
                        16, 1, busy || session.solver_mode != 1)) {
          markDirty("Physics parameters changed — play to rebake");
        }
        if (intProperty(ctx, g, tr("output_fps"), session.output_fps, 1, 240, 1,
                        busy)) {
          markDirty("Output timing changed - play to rebake");
        }
        {
          std::vector<const char *> timelines{
              tr("output_timeline_bake_fps"),
              tr("output_timeline_source_grid")};
          int selected = session.output_timeline_mode;
          if (combo(ctx, g, tr("output_timeline"), timelines, selected, busy)) {
            session.output_timeline_mode = selected;
            markDirty("Output timeline changed - play to rebake");
          }
        }
        if (session.solver_mode == 1) {
          slide(tr("rb_unit_scale"), session.unit_scale, 0.0001f, 10.0f,
                0.0001f);
          slide(tr("joint_stiffness"), session.rb_joint_stiffness, 0, 10000,
                1.0f, !session.enable_angle);
          const bool no_angular_spring =
              !session.enable_angle || session.rb_joint_stiffness <= 0.0f ||
              (session.rb_max_bend_x >= 180.0f &&
               session.rb_max_bend_y >= 180.0f &&
               session.rb_max_bend_z >= 180.0f);
          slide(tr("joint_damping"), session.rb_joint_damping, 0, 100, 0.05f,
                no_angular_spring);
          mutedWrap(ctx, g, tr("joint_spring_help"));
          slide(tr("rb_linear_damping"), session.rb_linear_damping, 0, 1,
                0.01f);
          slide(tr("rb_angular_damping"), session.rb_angular_damping, 0, 1,
                0.01f);
          slide(tr("rb_max_bend_x"), session.rb_max_bend_x, 0, 180, 1.0f,
                !session.enable_angle);
          slide(tr("rb_max_bend_y"), session.rb_max_bend_y, 0, 180, 1.0f,
                !session.enable_angle);
          slide(tr("rb_max_bend_z"), session.rb_max_bend_z, 0, 180, 1.0f,
                !session.enable_angle);
          slide(tr("rb_friction"), session.rb_friction, 0, 10, 0.05f);
          slide(tr("rb_restitution"), session.rb_restitution, 0, 1, 0.05f);
          slide(tr("rb_max_penetration"), session.rb_max_safe_pen, 0, 10,
                0.01f);
          mutedWrap(ctx, g, tr("rb_max_penetration_help"));
          if (check(ctx, g, tr("rb_ccd"), session.rb_ccd, busy)) {
            markDirty("Physics parameters changed — play to rebake");
          }
        }

        slide(tr("mass"), session.particle_mass, 0.01f, 100, 0.1f);
        slide(tr("compliance"), session.compliance, 0, 10, 0.000001f,
              session.solver_mode != 0);
        slide(tr("damping"), session.damping, 0, 10, 0.00001f,
              session.solver_mode != 0);
        if (check(ctx, g, tr("enable_angle"), session.enable_angle, busy)) {
          markDirty("Physics parameters changed — play to rebake");
        }
        slide(tr("max_bend"), session.max_bend, 0, 180, 1.0f,
              !session.enable_angle || session.solver_mode != 0);
        slide(tr("bend_compliance"), session.bend_compliance, 0, 10, 0.00001f,
              !session.enable_angle || session.solver_mode != 0);
        slide(tr("gravity"), session.gravity_y, -50, 10, 0.1f);
        if (check(ctx, g, tr("real_gravity"), session.enable_real_gravity,
                  busy)) {
          markDirty("Physics parameters changed — play to rebake");
        }
        if (check(ctx, g, tr("ground_collision"), session.enable_ground,
                  busy)) {
          markDirty("Physics parameters changed — play to rebake");
        }
        slide(tr("wind_speed"), session.wind_speed, 0, 30, 0.1f,
              session.use_wind_components);
        slide(tr("wind_dir"), session.wind_dir, -360, 360, 5.0f,
              session.use_wind_components);
        slide(tr("wind_elev"), session.wind_elev, -90, 90, 5.0f,
              session.use_wind_components);
        if (check(ctx, g, tr("use_wind_xyz"), session.use_wind_components,
                  busy)) {
          markDirty("Physics parameters changed — play to rebake");
        }
        slide(tr("wind_x"), session.wind_x, -50, 50, 0.1f,
              !session.use_wind_components);
        slide(tr("wind_y"), session.wind_y, -50, 50, 0.1f,
              !session.use_wind_components);
        slide(tr("wind_z"), session.wind_z, -50, 50, 0.1f,
              !session.use_wind_components);
        slide(tr("movement_speed"), session.movement_speed, 0, 50, 0.1f);
        slide(tr("movement_dir"), session.movement_dir, -360, 360, 5.0f);
        slide(tr("movement_elev"), session.movement_elev, -90, 90, 5.0f);
        {
          auto wind = session.use_wind_components
                          ? models::Vector3(session.wind_x, session.wind_y,
                                            session.wind_z)
                          : baker::PhysicsBaker::windVector(session.wind_speed,
                                                            session.wind_dir,
                                                            session.wind_elev);
          const auto movement = baker::PhysicsBaker::windVector(
              session.movement_speed, session.movement_dir,
              session.movement_elev);
          wind.sub(movement);
          char relative_air[160];
          std::snprintf(relative_air, sizeof(relative_air), tr("relative_air"),
                        wind.length(), wind.x, wind.y, wind.z);
          muted(ctx, g, relative_air);
        }
        slide(tr("air_response"), session.air_drag, 0, 10, 0.1f);
        slide(tr("turbulence"), session.turbulence, 0, 20, 0.1f);
        slide(tr("anim_follow"), session.pull, 0, 1, 0.01f,
              session.enable_real_gravity);
        {
          if (intProperty(ctx, g, tr("iterations"), session.solver_iters, 1,
                          100, 1, busy)) {
            markDirty("Physics parameters changed — play to rebake");
          }
        }

        heading(ctx, g, tr("ordinary_edge_blend"));
        bool edge_blend_enabled = session.transition_mode == 1;
        if (check(ctx, g, tr("ordinary_edge_blend_enable"),
                  edge_blend_enabled, busy)) {
          session.transition_mode = edge_blend_enabled ? 1 : 0;
          markDirty("Ordinary edge blend changed - play to rebake",
                    InvalidationReason::Transition);
        }
        mutedWrap(ctx, g, tr("ordinary_edge_blend_hint"));
        if (edge_blend_enabled) {
          if (slider(ctx, g, tr("edge_blend_seconds"),
                     session.transition_duration, 0.0f, 5.0f, busy, 0.05f)) {
            markDirty("Ordinary edge blend duration changed - play to rebake",
                      InvalidationReason::Transition);
          }
          mutedWrap(ctx, g, tr("edge_blend_seconds_hint"));
        }

        heading(ctx, g, tr("a_to_b_transition"));
        bool transition_enabled = session.transition_mode == 2;
        if (check(ctx, g, tr("a_to_b_transition_enable"),
                  transition_enabled, busy)) {
          session.transition_mode = transition_enabled ? 2 : 0;
          markDirty("A to B transition changed - play to rebake",
                    InvalidationReason::Transition);
        }
        mutedWrap(ctx, g, tr("a_to_b_transition_hint"));
        if (transition_enabled) {
          std::vector<std::string> names;
          names.reserve(session.animation_root.animations.size() + 1);
          names.emplace_back(tr("current_animation"));
          int selected = 0;
          auto append_animation = [&](const std::string &name) {
            names.push_back(name);
            if (name == session.transition_target_animation_name) {
              selected = static_cast<int>(names.size()) - 1;
            }
          };
          if (!session.animation_root.animation_order.empty()) {
            for (const auto &name : session.animation_root.animation_order) {
              if (session.animation_root.animations.contains(name)) {
                append_animation(name);
              }
            }
          } else {
            for (const auto &[name, animation] :
                 session.animation_root.animations) {
              (void)animation;
              append_animation(name);
            }
          }
          std::vector<const char *> items;
          items.reserve(names.size());
          for (const auto &name : names) {
            items.push_back(name.c_str());
          }
          if (combo(ctx, g, tr("transition_reference"), items, selected,
                    busy)) {
            session.transition_target_animation_name =
                selected == 0 ? std::string{}
                              : names[static_cast<std::size_t>(selected)];
            markDirty("Transition reference changed - play to rebake",
                      InvalidationReason::Transition);
          }
          if (slider(ctx, g, tr("transition_seconds"),
                     session.transition_duration, 0.001f, 5.0f, busy,
                     0.05f)) {
            markDirty("Transition duration changed - play to rebake",
                      InvalidationReason::Transition);
          }
          mutedWrap(ctx, g, tr("transition_seconds_hint"));
          const float source_max =
              session.selected_animation == nullptr
                  ? 3600.0f
                  : static_cast<float>(
                        (std::max)(0.0, session.selected_animation
                                            ->animation_length));
          float target_max = source_max;
          if (!session.transition_target_animation_name.empty()) {
            const auto target = session.animation_root.animations.find(
                session.transition_target_animation_name);
            if (target != session.animation_root.animations.end()) {
              target_max = static_cast<float>(
                  (std::max)(0.0, target->second.animation_length));
            }
          }
          if (slider(ctx, g, tr("transition_source_exit"),
                     session.transition_source_exit, 0.0f,
                     (std::max)(0.001f, source_max), busy, 0.01f)) {
            markDirty("Transition source time changed - play to rebake",
                      InvalidationReason::Transition);
          }
          if (slider(ctx, g, tr("transition_target_entry"),
                     session.transition_target_entry, 0.0f,
                     (std::max)(0.001f, target_max), busy, 0.01f)) {
            markDirty("Transition target time changed - play to rebake",
                      InvalidationReason::Transition);
          }
        }

        heading(ctx, g, tr("loop_settings"));
        {
          std::vector<const char *> modes{tr("auto"), tr("loop"), tr("once")};
          int selected = session.loop_mode;
          if (combo(ctx, g, tr("loop_mode"), modes, selected, busy)) {
            session.setLoopMode(selected);
          }
        }
        {
          std::vector<const char *> strategies{tr("seam_physics_relative"),
                                               tr("seam_visual_subtree")};
          int selected = session.loop_seam_strategy;
          if (combo(ctx, g, tr("seam_strategy"), strategies, selected, busy)) {
            session.loop_seam_strategy = selected;
            markDirty("Loop seam strategy changed - play to rebake",
                      InvalidationReason::LoopOrSeam);
          }
        }
        if (slider(ctx, g, tr("seam_window"),
                   session.loop_seam_window_ratio, 0.0f, 0.5f, busy,
                   0.125f)) {
          markDirty("Loop seam ratio changed - play to rebake",
                    InvalidationReason::LoopOrSeam);
        }
        mutedWrap(ctx, g, tr("seam_window_hint"));

        heading(ctx, g, tr("body_collision"));
        slide(tr("node_thickness"), session.collision_skin, 0, 10, 0.01f,
              session.solver_mode != 0);
        slide(tr("xpbd_restitution"), session.xpbd_restitution, 0, 1, 0.05f,
              session.solver_mode != 0);
        char collision_count[128];
        std::snprintf(collision_count, sizeof(collision_count),
                      tr("collision_root_count"),
                      session.bone_mapper.collisionRoots().size());
        muted(ctx, g, collision_count);
        if (session.selected_bone_name.empty()) {
          muted(ctx, g, tr("no_bone_selected"));
        } else {
          const std::string selected_collision_bone =
              std::string(tr("bone_prefix")) + session.selected_bone_name;
          muted(ctx, g, selected_collision_bone.c_str());
          bool is_collision_root =
              session.bone_mapper.isCollisionRoot(session.selected_bone_name);
          if (check(ctx, g, tr("selected_collision_root"), is_collision_root,
                    busy)) {
            session.setCollisionRoot(session.selected_bone_name,
                                     is_collision_root);
          }
        }
        nk_layout_row_dynamic(ctx, g.btn, 1);
        const bool can_clear_collision =
            !busy && !session.bone_mapper.collisionRoots().empty();
        if (!can_clear_collision) {
          nk_widget_disable_begin(ctx);
        }
        if (nk_button_label(ctx, tr("clear_collision_roots")) &&
            can_clear_collision) {
          session.clearCollisionRoots();
        }
        if (!can_clear_collision) {
          nk_widget_disable_end(ctx);
        }
        mutedWrap(ctx, g, tr("collision_root_hint"));

        heading(ctx, g, tr("bone_override"));
        if (session.selected_bone_name.empty()) {
          muted(ctx, g, tr("no_bone_selected"));
        } else {
          const std::string t =
              std::string(tr("bone_prefix")) + session.selected_bone_name;
          muted(ctx, g, t.c_str());
          auto ov = [&](const char *l, bool &f, bool disabled = false) {
            nk_layout_row_dynamic(ctx, g.row, 1);
            nk_bool v = f ? nk_true : nk_false;
            if (busy || disabled) {
              nk_widget_disable_begin(ctx);
            }
            if (!busy && !disabled && nk_checkbox_label(ctx, l, &v)) {
              f = v != 0;
              session.markSelectedBoneDraftDirty();
            }
            if (busy || disabled) {
              nk_widget_disable_end(ctx);
            }
          };
          auto bone_slide = [&](const char *l, float &v, float lo, float hi,
                                float step = 0.0f,
                                bool disabled = false) {
            if (slider(ctx, g, l, v, lo, hi, busy || disabled, step)) {
              session.markSelectedBoneDraftDirty();
            }
          };
          ov(tr("ov_mass"), session.bone_ov_mass);
          if (session.bone_ov_mass) {
            bone_slide(tr("mass"), session.bone_mass, 0.01f, 100, 0.1f);
          }
          ov(tr("ov_compliance"), session.bone_ov_compliance,
             session.solver_mode != 0);
          if (session.bone_ov_compliance) {
            bone_slide(tr("compliance"), session.bone_compliance, 0, 10,
                       0.000001f, session.solver_mode != 0);
          }
          ov(tr("ov_damping"), session.bone_ov_damping,
             session.solver_mode != 0);
          if (session.bone_ov_damping) {
            bone_slide(tr("damping"), session.bone_damping, 0, 10, 0.00001f,
                       session.solver_mode != 0);
          }
          ov(tr("ov_max_bend"), session.bone_ov_max_bend,
             session.solver_mode != 0 || !session.enable_angle);
          if (session.bone_ov_max_bend) {
            bone_slide(tr("max_bend"), session.bone_max_bend, 0, 180, 1.0f,
                       session.solver_mode != 0 || !session.enable_angle);
          }
          ov(tr("ov_rb_bend_x"), session.bone_ov_rb_bend_x,
             session.solver_mode != 1 || !session.enable_angle);
          if (session.bone_ov_rb_bend_x) {
            bone_slide(tr("rb_max_bend_x"), session.bone_rb_bend_x, 0, 180,
                       1.0f,
                       session.solver_mode != 1 || !session.enable_angle);
          }
          ov(tr("ov_rb_bend_y"), session.bone_ov_rb_bend_y,
             session.solver_mode != 1 || !session.enable_angle);
          if (session.bone_ov_rb_bend_y) {
            bone_slide(tr("rb_max_bend_y"), session.bone_rb_bend_y, 0, 180,
                       1.0f,
                       session.solver_mode != 1 || !session.enable_angle);
          }
          ov(tr("ov_rb_bend_z"), session.bone_ov_rb_bend_z,
             session.solver_mode != 1 || !session.enable_angle);
          if (session.bone_ov_rb_bend_z) {
            bone_slide(tr("rb_max_bend_z"), session.bone_rb_bend_z, 0, 180,
                       1.0f,
                       session.solver_mode != 1 || !session.enable_angle);
          }
          ov(tr("ov_bend_compliance"), session.bone_ov_bend_compliance,
             session.solver_mode != 0 || !session.enable_angle);
          if (session.bone_ov_bend_compliance) {
            bone_slide(tr("bend_compliance"), session.bone_bend_compliance, 0,
                       10, 0.00001f,
                       session.solver_mode != 0 || !session.enable_angle);
          }
          ov(tr("ov_pull"), session.bone_ov_pull, session.enable_real_gravity);
          if (session.bone_ov_pull) {
            bone_slide(tr("anim_follow"), session.bone_pull, 0, 1, 0.01f,
                       session.enable_real_gravity);
          }
          if (session.transition_mode == 2) {
            ov(tr("ov_transition_follow"), session.bone_ov_transition_follow);
            if (session.bone_ov_transition_follow) {
              bone_slide(tr("transition_follow"),
                         session.bone_transition_follow, 0, 1, 0.05f);
            }
          }
          ov(tr("ov_gravity"), session.bone_ov_gravity);
          if (session.bone_ov_gravity) {
            bone_slide(tr("gravity_scale"), session.bone_gravity_scale, 0, 5,
                       0.1f);
          }
          ov(tr("ov_wind"), session.bone_ov_wind);
          if (session.bone_ov_wind) {
            bone_slide(tr("wind_scale"), session.bone_wind, 0, 5, 0.1f);
          }
          ov(tr("ov_turbulence"), session.bone_ov_turbulence);
          if (session.bone_ov_turbulence) {
            bone_slide(tr("turbulence_scale"), session.bone_turbulence, 0, 5,
                       0.1f);
          }
          ov(tr("ov_fixed"), session.bone_ov_fixed);
          if (session.bone_ov_fixed) {
            if (check(ctx, g, tr("fixed_kinematic"), session.bone_fixed,
                      busy)) {
              session.markSelectedBoneDraftDirty();
            }
          }
          muted(ctx, g, session.hasUnappliedPerBoneDraft()
                            ? tr("draft_unapplied")
                            : tr("draft_applied"));
          nk_layout_row_dynamic(ctx, g.btn, 1);
          if (nk_button_label(ctx, tr("apply_bone")) && !busy) {
            session.applySelectedBoneConfig();
          }
          nk_layout_row_dynamic(ctx, g.btn, 1);
          if (nk_button_label(ctx, tr("discard_draft")) && !busy &&
              session.hasUnappliedPerBoneDraft()) {
            session.discardSelectedBoneDraft();
          }
          nk_layout_row_dynamic(ctx, g.btn, 1);
          if (nk_button_label(ctx, tr("clear_override")) && !busy) {
            session.clearSelectedBoneConfig();
          }
        }

        heading(ctx, g, tr("bake_result"));
        {
          char state_line[160];
          std::snprintf(state_line, sizeof(state_line), tr("state_phase"),
                        bakeStateName(session.bake_state),
                        workerPhaseName(session.worker_phase));
          muted(ctx, g, state_line);
        }
        if (const auto *live = session.liveSimulationFrame()) {
          char live_line[192];
          std::snprintf(live_line, sizeof(live_line), tr("live_diagnostics"),
                        live->collision.current_contact_count,
                        live->collision.maximum_contact_count,
                        live->collision.maximum_penetration,
                        live->substeps ? live->substeps->last_effective : 0);
          mutedWrap(ctx, g, live_line);
        }
        if (const auto *diagnostics = session.diagnostics()) {
          const std::string fingerprint = std::string(tr("fingerprint")) + " " +
                                          diagnostics->fingerprint.hex();
          muted(ctx, g, fingerprint.c_str());
          char timing_line[192];
          std::snprintf(timing_line, sizeof(timing_line),
                        tr("timing_diagnostics"),
                        diagnostics->timing.output_fps,
                        diagnostics->timing.effective_output_dt,
                        diagnostics->timing.resumed_from_step,
                        diagnostics->timing.completed_steps);
          mutedWrap(ctx, g, timing_line);
          if (diagnostics->kinematic_history) {
            char history_line[224];
            std::snprintf(
                history_line, sizeof(history_line),
                tr("kinematic_history_diagnostics"),
                static_cast<unsigned long long>(
                    diagnostics->kinematic_history->continuous_updates),
                static_cast<unsigned long long>(
                    diagnostics->kinematic_history->periodic_updates),
                static_cast<unsigned long long>(
                    diagnostics->kinematic_history->teleport_resets),
                static_cast<unsigned long long>(
                    diagnostics->kinematic_history->rejected_discontinuities));
            mutedWrap(ctx, g, history_line);
          }
          if (diagnostics->substeps) {
            char substep_line[224];
            std::snprintf(substep_line, sizeof(substep_line),
                          tr("fixed_substep_diagnostics"),
                          diagnostics->substeps->configured_minimum,
                          diagnostics->substeps->last_effective,
                          diagnostics->substeps->average_effective,
                          diagnostics->substeps->maximum_effective,
                          static_cast<unsigned long long>(
                              diagnostics->substeps
                                  ->insufficient_step_risk_count),
                          static_cast<unsigned long long>(
                              diagnostics->substeps->output_step_count));
            mutedWrap(ctx, g, substep_line);
            if (!diagnostics->substeps->worst_body.empty()) {
              char motion_line[384];
              std::snprintf(
                  motion_line, sizeof(motion_line),
                  tr("fixed_substep_motion_diagnostics"),
                  diagnostics->substeps->worst_motion_source.c_str(),
                  diagnostics->substeps->worst_body.c_str(),
                  diagnostics->substeps->last_kinematic_required,
                  diagnostics->substeps->last_dynamic_required,
                  diagnostics->substeps->maximum_dynamic_required,
                  diagnostics->substeps->worst_minimum_half_extent,
                  diagnostics->substeps->worst_maximum_radius,
                  diagnostics->substeps->worst_equivalent_linear_travel,
                  diagnostics->substeps->worst_equivalent_angular_travel,
                  diagnostics->substeps->worst_acceleration);
              mutedWrap(ctx, g, motion_line);
            }
          }
          if (diagnostics->collider_preflight) {
            char collider_line[320];
            std::snprintf(
                collider_line, sizeof(collider_line),
                tr("collider_preflight_diagnostics"),
                diagnostics->collider_preflight->colliders.size(),
                diagnostics->collider_preflight->warning_count,
                diagnostics->collider_preflight->error_count,
                diagnostics->collider_preflight->observed_minimum_half_extent,
                diagnostics->collider_preflight->observed_maximum_half_extent,
                diagnostics->collider_preflight->observed_maximum_aspect_ratio,
                diagnostics->collider_preflight->unit_scale);
            mutedWrap(ctx, g, collider_line);
            for (const auto &collider :
                 diagnostics->collider_preflight->colliders) {
              if (collider.risk == rigidbody::ColliderRiskLevel::Safe) {
                continue;
              }
              char collider_issue[384];
              std::snprintf(
                  collider_issue, sizeof(collider_issue),
                  tr("collider_risk_diagnostics"),
                  rigidbody::colliderRiskLevelName(collider.risk),
                  collider.body_name.c_str(), collider.box_index,
                  collider.minimum_half_extent,
                  collider.maximum_half_extent, collider.aspect_ratio,
                  collider.margin_to_minimum_half_extent,
                  collider.ccd_radius);
              mutedWrap(ctx, g, collider_issue);
            }
          }
          if (diagnostics->velocity.available) {
            char velocity_line[224];
            std::snprintf(velocity_line, sizeof(velocity_line),
                          tr("velocity_diagnostics"),
                          diagnostics->velocity.maximum_linear_speed,
                          diagnostics->velocity.maximum_speed_bone.c_str(),
                          diagnostics->velocity.maximum_speed_frame,
                          diagnostics->velocity.maximum_frame_velocity_jump,
                          diagnostics->velocity.maximum_jump_bone.c_str(),
                          diagnostics->velocity.maximum_jump_frame);
            mutedWrap(ctx, g, velocity_line);
          }
          if (diagnostics->bullet_safety_applicable) {
            char collision_line[192];
            std::snprintf(collision_line, sizeof(collision_line),
                          tr("collision_diagnostics"),
                          diagnostics->initial_collision
                              ? diagnostics->initial_collision->contact_count
                              : 0,
                          diagnostics->runtime_collision.maximum_contact_count,
                          diagnostics->final_audit.maximum_penetration);
            mutedWrap(ctx, g, collision_line);
            if (diagnostics->initial_collision &&
                diagnostics->initial_collision->warning) {
              mutedWrap(ctx, g, tr("initial_collision_warning"));
            }
            if (diagnostics->final_audit.worst_collision_pair) {
              char pair_line[192];
              std::snprintf(
                  pair_line, sizeof(pair_line), tr("worst_collision_pair"),
                  diagnostics->final_audit.worst_collision_pair->first.c_str(),
                  diagnostics->final_audit.worst_collision_pair->second.c_str());
              mutedWrap(ctx, g, pair_line);
            }
            char joint_line[192];
            std::snprintf(joint_line, sizeof(joint_line),
                          tr("joint_diagnostics"),
                          diagnostics->joints.unsafe_final_count,
                          diagnostics->joints.maximum_anchor_separation,
                          diagnostics->joints.maximum_angular_excess_radians);
            mutedWrap(ctx, g, joint_line);
          }
          if (diagnostics->bullet_safety_applicable &&
              diagnostics->joint_spring) {
            char spring_line[320];
            std::snprintf(
                spring_line, sizeof(spring_line),
                tr("joint_spring_diagnostics"),
                diagnostics->joint_spring->requested_stiffness,
                diagnostics->joint_spring->requested_damping,
                diagnostics->joint_spring->active_spring_joint_count,
                diagnostics->joint_spring->active_spring_axis_count,
                diagnostics->joint_spring->solver_constraint.c_str());
            mutedWrap(ctx, g, spring_line);
            mutedWrap(ctx, g, tr("joint_spring_effective_behavior"));
          }
          if (diagnostics->bullet_safety_applicable &&
              diagnostics->joint_preflight) {
            for (const auto &warning :
                 diagnostics->joint_preflight->warnings) {
              char warning_line[320];
              std::snprintf(
                  warning_line, sizeof(warning_line),
                  tr("joint_half_turn_warning"), warning.parent_body.c_str(),
                  warning.child_body.c_str(), warning.axis.c_str(),
                  warning.limit_degrees,
                  diagnostics->joint_preflight->rotation_order.c_str());
              mutedWrap(ctx, g, warning_line);
            }
          }
          if (diagnostics->bullet_safety_applicable &&
              diagnostics->joints.euler_singularity) {
            const auto &singularity = *diagnostics->joints.euler_singularity;
            char singular_line[384];
            std::snprintf(
                singular_line, sizeof(singular_line),
                tr("joint_euler_singularity"),
                singularity.parent_bone.c_str(), singularity.child_bone.c_str(),
                singularity.relative_rotation_xyzw[0],
                singularity.relative_rotation_xyzw[1],
                singularity.relative_rotation_xyzw[2],
                singularity.relative_rotation_xyzw[3],
                singularity.rotation_order.c_str());
            mutedWrap(ctx, g, singular_line);
          }
          const std::string &worst_joint_child =
              !diagnostics->joints.worst_angular_child.empty()
                  ? diagnostics->joints.worst_angular_child
                  : diagnostics->joints.worst_linear_child;
          if (diagnostics->bullet_safety_applicable &&
              !worst_joint_child.empty()) {
            char select_joint[192];
            std::snprintf(select_joint, sizeof(select_joint),
                          tr("select_worst_joint"),
                          worst_joint_child.c_str());
            nk_layout_row_dynamic(ctx, g.btn, 1);
            if (nk_button_label(ctx, select_joint) && !busy) {
              session.selectBone(worst_joint_child);
            }
          }
          const std::string stability =
              std::string(tr(diagnostics->bullet_safety_applicable
                                 ? "stability"
                                 : "stability_noncollision")) + " " +
              diagnostics->chain_stability;
          muted(ctx, g, stability.c_str());
          if (diagnostics->loop.source_policy == "Loop" ||
              diagnostics->loop.source_policy == "Forced Loop") {
            char loop_line[192];
            std::snprintf(loop_line, sizeof(loop_line),
                          tr("loop_diagnostics"),
                          diagnostics->loop.completed_cycles,
                          diagnostics->loop.converged ? "yes" : "no",
                          diagnostics->loop.fallback_used ? "yes" : "no",
                          diagnostics->loop.seam_correction_rejected ? "yes"
                                                                     : "no");
            mutedWrap(ctx, g, loop_line);
            char layered_line[512];
            std::snprintf(
                layered_line, sizeof(layered_line),
                tr("loop_layered_diagnostics"),
                diagnostics->loop.source_policy.c_str(),
                diagnostics->loop.physical_state.c_str(),
                diagnostics->loop.seam_state.c_str(),
                diagnostics->loop.driver_state.c_str(),
                diagnostics->loop.collision_state.c_str(),
                diagnostics->loop.export_state.c_str());
            mutedWrap(ctx, g, layered_line);
            char seam_window_summary[256];
            std::snprintf(
                seam_window_summary, sizeof(seam_window_summary),
                tr("loop_seam_window_summary"),
                diagnostics->loop.configured_seam_window_ratio * 100.0,
                diagnostics->loop.effective_seam_window_seconds,
                diagnostics->loop.effective_seam_window_ratio * 100.0);
            mutedWrap(ctx, g, seam_window_summary);
            if (!diagnostics->loop.physics_relative_fallback_reason.empty()) {
              mutedWrap(ctx, g,
                        diagnostics->loop.physics_relative_fallback_reason
                            .c_str());
            }
            for (const auto &candidate :
                 diagnostics->loop.cycle_candidates) {
              char candidate_line[384];
              if (diagnostics->bullet_safety_applicable) {
                std::snprintf(
                    candidate_line, sizeof(candidate_line),
                    tr("loop_candidate_diagnostics"), candidate.cycle,
                    candidate.valid ? "yes" : "no", candidate.score,
                    candidate.pose_error, candidate.velocity_error,
                    candidate.contact_difference_count,
                    candidate.maximum_penetration,
                    candidate.selected ? "yes" : "no");
              } else {
                std::snprintf(
                    candidate_line, sizeof(candidate_line),
                    tr("loop_candidate_diagnostics_noncollision"),
                    candidate.cycle, candidate.valid ? "yes" : "no",
                    candidate.score, candidate.pose_error,
                    candidate.velocity_error,
                    candidate.selected ? "yes" : "no");
              }
              mutedWrap(ctx, g, candidate_line);
            }
            for (const auto &window : diagnostics->loop.seam_windows) {
              char window_line[384];
              if (diagnostics->bullet_safety_applicable) {
                std::snprintf(
                    window_line, sizeof(window_line),
                    tr("loop_window_diagnostics"),
                    window.window_duration_seconds, window.window_ratio * 100.0,
                    window.c0_pass ? "yes" : "no",
                    window.c1_pass ? "yes" : "no",
                    window.c2_pass ? "yes" : "no",
                    window.driver_pass ? "yes" : "no",
                    window.collision_safe ? "yes" : "no",
                    window.joint_safe ? "yes" : "no",
                    window.accepted ? "accepted" : "rejected");
              } else {
                std::snprintf(
                    window_line, sizeof(window_line),
                    tr("loop_window_diagnostics_noncollision"),
                    window.window_duration_seconds, window.window_ratio * 100.0,
                    window.c0_pass ? "yes" : "no",
                    window.c1_pass ? "yes" : "no",
                    window.c2_pass ? "yes" : "no",
                    window.driver_pass ? "yes" : "no",
                    window.accepted ? "accepted" : "rejected");
              }
              mutedWrap(ctx, g, window_line);
            }
            for (const auto &marker : diagnostics->loop.danger_markers) {
              char marker_line[384];
              std::snprintf(marker_line, sizeof(marker_line),
                            tr("loop_danger_marker"), marker.kind.c_str(),
                            marker.time, marker.item.empty()
                                             ? "-"
                                             : marker.item.c_str());
              nk_layout_row_dynamic(ctx, g.btn, 1);
              if (nk_button_label(ctx, marker_line) && !busy &&
                  std::isfinite(marker.time) && marker.time >= 0.0) {
                const auto *result = session.finalResult();
                if (result != nullptr && result->frames != nullptr &&
                    !result->frames->empty()) {
                  int nearest = 0;
                  double distance =
                      std::abs(result->frames->front().time - marker.time);
                  for (std::size_t index = 1;
                       index < result->frames->size(); ++index) {
                    const double candidate = std::abs(
                        (*result->frames)[index].time - marker.time);
                    if (candidate < distance) {
                      distance = candidate;
                      nearest = static_cast<int>(index);
                    }
                  }
                  session.presentation_mode =
                      PresentationMode::FinalBakedPreview;
                  session.playback_state = PlaybackState::Paused;
                  session.setPreviewFrameIndex(nearest);
                }
              }
            }
            const auto selectLoopItem = [&](const char *translation_key,
                                            const std::string &bone) {
              if (bone.empty()) {
                return;
              }
              char select_loop[256];
              std::snprintf(select_loop, sizeof(select_loop),
                            tr(translation_key), bone.c_str());
              nk_layout_row_dynamic(ctx, g.btn, 1);
              if (nk_button_label(ctx, select_loop) && !busy) {
                session.selectBone(bone);
              }
            };
            selectLoopItem("select_worst_position",
                           diagnostics->loop.position_bone);
            selectLoopItem("select_worst_rotation",
                           diagnostics->loop.rotation_bone);
            selectLoopItem("select_worst_linear_velocity",
                           diagnostics->loop.linear_velocity_bone);
            selectLoopItem("select_worst_angular_velocity",
                           diagnostics->loop.angular_velocity_bone);
            selectLoopItem("select_worst_driver",
                           diagnostics->loop.worst_driver_bone);
            selectLoopItem("select_missing_loop_bone",
                           diagnostics->loop.missing_bone);
            selectLoopItem("select_invalid_loop_bone",
                           diagnostics->loop.invalid_numeric_bone);
            if (diagnostics->bullet_safety_applicable &&
                diagnostics->final_audit.worst_collision_pair) {
              selectLoopItem(
                  "select_worst_penetration",
                  diagnostics->final_audit.worst_collision_pair->first);
              if (diagnostics->final_audit.worst_collision_pair->second !=
                  diagnostics->final_audit.worst_collision_pair->first) {
                selectLoopItem(
                    "select_worst_penetration",
                    diagnostics->final_audit.worst_collision_pair->second);
              }
            }
          }
          const auto &preflight = session.exportPreflight();
          const char *availability =
              preflight.animation_allowed && preflight.velocity_allowed
                  ? tr("export_ready")
                  : session.canForceExportAnimation()
                        ? tr("force_export_available")
                        : tr("export_blocked");
          mutedWrap(ctx, g, availability);
          if (!preflight.block_reasons.empty()) {
            mutedWrap(ctx, g, preflight.block_reasons.front().detail.c_str());
          }
          if (!session.selected_bone_name.empty()) {
            const auto effective = diagnostics->effective_config.per_bone.find(
                session.selected_bone_name);
            if (effective != diagnostics->effective_config.per_bone.end()) {
              for (const auto &value : effective->second) {
                const std::string line = value.name + ": " +
                                         value.effective_value + " [" +
                                         value.source + "]";
                muted(ctx, g, line.c_str());
              }
            }
          }
        } else {
          mutedWrap(ctx, g, tr("no_final_diagnostics"));
        }

        heading(ctx, g, tr("options"));
        check(ctx, g, tr("show_bones"), session.show_bones, false);
        check(ctx, g, tr("show_ground"), session.show_ground, false);
        check(ctx, g, tr("camera_follow"), session.camera_follow_preview,
              false);
        muted(ctx, g, tr("camera_follow_hint"));
        check(ctx, g, tr("mcbe_coords"), session.use_mcbe_coords, false);
        muted(ctx, g, tr("mcbe_hint"));
        if (check(ctx, g, tr("vsync"), session.vsync_enabled, false)) {
          session.vsync_dirty = true;
        }
        muted(ctx, g, tr("camera_hint"));
        muted(ctx, g, tr("lang"));
        nk_layout_row_dynamic(ctx, g.btn, 2);
        if (nk_button_label(ctx, tr("lang_en"))) {
          setLang(Lang::En);
        }
        if (nk_button_label(ctx, tr("lang_zh_cn"))) {
          setLang(Lang::ZhCn);
        }
        nk_layout_row_dynamic(ctx, g.btn, 2);
        if (nk_button_label(ctx, tr("lang_zh_hk"))) {
          setLang(Lang::ZhHk);
        }
        if (nk_button_label(ctx, tr("lang_zh_tw"))) {
          setLang(Lang::ZhTw);
        }

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
          formatByteCount(upload_bytes, sizeof(upload_bytes),
                          session.debug_upload_bytes);
          formatByteCount(bone_bytes, sizeof(bone_bytes),
                          session.debug_static_bone_upload_bytes);
          formatByteCount(resource_bytes, sizeof(resource_bytes),
                          session.debug_static_resource_upload_bytes);
          formatByteCount(static_vertex_bytes, sizeof(static_vertex_bytes),
                          session.debug_static_model_vertex_bytes);
          formatByteCount(static_index_bytes, sizeof(static_index_bytes),
                          session.debug_static_model_index_bytes);

          char lines[12][160];
          std::snprintf(lines[0], sizeof(lines[0]), "FPS %.1f [%s]",
                        session.debug_fps,
                        session.debug_instant_sample ? "live" : "1s");
          std::snprintf(lines[1], sizeof(lines[1]), "Frame %.2f ms",
                        session.debug_ema_frame_ms);
          std::snprintf(lines[2], sizeof(lines[2]), "Mesh/Upload %.2f/%.2f ms",
                        session.debug_mesh_ms, session.debug_upload_ms);
          std::snprintf(lines[3], sizeof(lines[3]),
                        "Upload/f %s | bones %s | static %s", upload_bytes,
                        bone_bytes, resource_bytes);
          std::snprintf(lines[4], sizeof(lines[4]),
                        "Realloc/f %.2f | total %llu | rebuilds %llu",
                        session.debug_buffer_reallocations,
                        static_cast<unsigned long long>(
                            session.debug_total_buffer_reallocations),
                        static_cast<unsigned long long>(
                            session.debug_static_resource_rebuilds));
          std::snprintf(
              lines[5], sizeof(lines[5]),
              "Static VB %s | IB %s | O/C/B %u/%u/%u", static_vertex_bytes,
              static_index_bytes,
              static_cast<unsigned>(session.debug_static_opaque_index_count),
              static_cast<unsigned>(session.debug_static_cutout_index_count),
              static_cast<unsigned>(session.debug_static_blend_index_count));
          std::snprintf(lines[6], sizeof(lines[6]), "Backend CPU %.2f ms",
                        session.debug_backend_cpu_ms);
          if (session.debug_gpu_timestamp_valid) {
            std::snprintf(lines[7], sizeof(lines[7]), "GPU timestamp %.2f ms",
                          session.debug_gpu_timestamp_total_ms);
            std::snprintf(lines[8], sizeof(lines[8]),
                          "GPU U/O/T/L %.2f/%.2f/%.2f/%.2f ms",
                          session.debug_gpu_timestamp_ui_ms,
                          session.debug_gpu_timestamp_opaque_ms,
                          session.debug_gpu_timestamp_transparent_ms,
                          session.debug_gpu_timestamp_lines_ms);
          } else {
            std::snprintf(lines[7], sizeof(lines[7]),
                          "GPU timestamp unavailable");
            std::snprintf(lines[8], sizeof(lines[8]), "GPU U/O/T/L n/a");
          }
          std::snprintf(lines[9], sizeof(lines[9]), "Backend %s",
                        backend_name ? backend_name : "-");
          std::snprintf(lines[10], sizeof(lines[10]), "Device %.40s",
                        device_name ? device_name : "-");
          std::snprintf(lines[11], sizeof(lines[11]), "Cubes %d | VSync %s",
                        session.debug_cube_count,
                        session.vsync_enabled ? "on" : "off");
          for (auto &line : lines) {
            muted(ctx, g, line);
          }
        }
        nk_group_end(ctx);
      }

      nk_layout_row_end(ctx);

      if (ui_state.splitter_drag != SplitterDrag::None) {
        result.layout.viewport_hovered = false;
      }



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
                       nk_rgb(150, 155, 170));
      nk_layout_row_push(ctx, b_export);
      nk_style_push_style_item(ctx, &ctx->style.button.normal,
                               nk_style_item_color(nk_rgba(60, 160, 120, 255)));
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

      result.layout.left_w = use_left;
      result.layout.right_w = use_right;
      result.layout.menu_h = menu_row_h * menu_rows;
      result.layout.bottom_h = bottom_row_h;
    }
  }
  nk_end(ctx);

  return result;
}

}
