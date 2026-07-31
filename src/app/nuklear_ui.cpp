#include "xpbd/app/nuklear_ui.hpp"

#include "xpbd/app/app_session.hpp"
#include "xpbd/app/i18n.hpp"
#include "xpbd/baker/output_timeline_resampler.hpp"

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

  // 视口点选联动：选中变化时展开祖先并滚动到该行。
  std::string last_selected_bone;
  bool scroll_tree_to_selection = false;
  int properties_page = 0;
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
            float lo, float hi, bool busy, float requested_step = 0.0f) {
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
    state.last_selected_bone.clear();
  }

  std::set<std::string> known_names;
  for (const auto &bone : bones) {
    known_names.insert(bone.name);
  }

  // 选中骨骼变化（例如在视口中点选）时：展开全部祖先节点并请求滚动定位。
  if (session.selected_bone_name != state.last_selected_bone) {
    state.last_selected_bone = session.selected_bone_name;
    if (!session.selected_bone_name.empty()) {
      std::map<std::string, const loader::Bone *> by_name;
      for (const auto &bone : bones) {
        by_name[bone.name] = &bone;
      }
      std::set<std::string> walk_guard;
      auto it = by_name.find(session.selected_bone_name);
      const loader::Bone *cursor =
          it == by_name.end() ? nullptr : it->second;
      while (cursor != nullptr && cursor->has_parent &&
             !cursor->parent.empty() && cursor->parent != cursor->name &&
             walk_guard.insert(cursor->name).second) {
        state.expanded_bones.insert(cursor->parent);
        const auto parent_it = by_name.find(cursor->parent);
        cursor = parent_it == by_name.end() ? nullptr : parent_it->second;
      }
      state.scroll_tree_to_selection = true;
    }
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

  int row_index = 0;
  int selected_row = -1;

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
    if (selected) {
      selected_row = row_index;
    }
    ++row_index;

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
      if (flatSymbolButton(ctx, expanded ? NK_SYMBOL_TRIANGLE_DOWN
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
    const bool viewport_hovered_row =
        !selected && session.hovered_bone_name == bone.name;
    if (treeRowButton(ctx, bone.name.c_str(), selected,
                      viewport_hovered_row)) {
      session.selectBone(bone.name);
      // 树内点击的行本来就可见，跳过下一帧的自动展开与滚动。
      state.last_selected_bone = session.selected_bone_name;
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

  // 把选中行滚动到列表可视区域中间（下一帧生效）。
  if (state.scroll_tree_to_selection) {
    state.scroll_tree_to_selection = false;
    if (selected_row >= 0) {
      const float row_step = g.row + ctx->style.window.spacing.y;
      const float group_h = nk_window_get_content_region_size(ctx).y;
      const float target = (std::max)(
          0.0f, static_cast<float>(selected_row) * row_step -
                    (std::max)(0.0f, group_h * 0.5f - row_step));
      nk_group_set_scroll(ctx, "bones", 0, static_cast<nk_uint>(target));
    }
  }
}

bool contextMenuNavButton(nk_context *ctx, const char *label) {
  nk_style_push_style_item(ctx, &ctx->style.button.normal,
                           nk_style_item_color(nk_rgba(48, 52, 64, 255)));
  nk_style_push_style_item(ctx, &ctx->style.button.hover,
                           nk_style_item_color(nk_rgba(64, 70, 86, 255)));
  nk_style_push_style_item(ctx, &ctx->style.button.active,
                           nk_style_item_color(nk_rgba(34, 150, 158, 255)));
  nk_style_push_flags(ctx, &ctx->style.button.text_alignment, NK_TEXT_LEFT);
  const bool clicked = nk_button_label(ctx, label) != 0;
  nk_style_pop_flags(ctx);
  nk_style_pop_style_item(ctx);
  nk_style_pop_style_item(ctx);
  nk_style_pop_style_item(ctx);
  return clicked;
}

void drawSelectedBoneOverrideEditor(nk_context *ctx, const Geom &g,
                                    AppSession &session, bool busy) {
  heading(ctx, g, tr("bone_override"));
  if (session.selected_bone_name.empty()) {
    muted(ctx, g, tr("no_bone_selected"));
    return;
  }

  const std::string selected_label =
      std::string(tr("bone_prefix")) + session.selected_bone_name;
  muted(ctx, g, selected_label.c_str());
  const auto override_toggle = [&](const char *label, bool &enabled,
                                   bool disabled = false) {
    nk_layout_row_dynamic(ctx, g.row, 1);
    nk_bool value = enabled ? nk_true : nk_false;
    if (busy || disabled) {
      nk_widget_disable_begin(ctx);
    }
    if (!busy && !disabled && nk_checkbox_label(ctx, label, &value)) {
      enabled = value != 0;
      session.markSelectedBoneDraftDirty();
    }
    if (busy || disabled) {
      nk_widget_disable_end(ctx);
    }
  };
  const auto override_slider = [&](const char *label, float &value, float lo,
                                   float hi, float step = 0.0f,
                                   bool disabled = false) {
    if (slider(ctx, g, label, value, lo, hi, busy || disabled, step)) {
      session.markSelectedBoneDraftDirty();
    }
  };

  override_toggle(tr("ov_mass"), session.bone_ov_mass);
  if (session.bone_ov_mass) {
    override_slider(tr("mass"), session.bone_mass, 0.01f, 100.0f, 0.1f);
  }
  override_toggle(tr("ov_compliance"), session.bone_ov_compliance,
                  session.solver_mode != 0);
  if (session.bone_ov_compliance) {
    override_slider(tr("compliance"), session.bone_compliance, 0.0f, 10.0f,
                    0.000001f, session.solver_mode != 0);
  }
  override_toggle(tr("ov_damping"), session.bone_ov_damping,
                  session.solver_mode != 0);
  if (session.bone_ov_damping) {
    override_slider(tr("damping"), session.bone_damping, 0.0f, 10.0f,
                    0.00001f, session.solver_mode != 0);
  }
  override_toggle(tr("ov_max_bend"), session.bone_ov_max_bend,
                  session.solver_mode != 0 || !session.enable_angle);
  if (session.bone_ov_max_bend) {
    override_slider(tr("max_bend"), session.bone_max_bend, 0.0f, 180.0f,
                    1.0f,
                    session.solver_mode != 0 || !session.enable_angle);
  }
  override_toggle(tr("ov_rb_bend_x"), session.bone_ov_rb_bend_x,
                  session.solver_mode != 1 || !session.enable_angle);
  if (session.bone_ov_rb_bend_x) {
    override_slider(tr("rb_max_bend_x"), session.bone_rb_bend_x, 0.0f,
                    180.0f, 1.0f,
                    session.solver_mode != 1 || !session.enable_angle);
  }
  override_toggle(tr("ov_rb_bend_y"), session.bone_ov_rb_bend_y,
                  session.solver_mode != 1 || !session.enable_angle);
  if (session.bone_ov_rb_bend_y) {
    override_slider(tr("rb_max_bend_y"), session.bone_rb_bend_y, 0.0f,
                    180.0f, 1.0f,
                    session.solver_mode != 1 || !session.enable_angle);
  }
  override_toggle(tr("ov_rb_bend_z"), session.bone_ov_rb_bend_z,
                  session.solver_mode != 1 || !session.enable_angle);
  if (session.bone_ov_rb_bend_z) {
    override_slider(tr("rb_max_bend_z"), session.bone_rb_bend_z, 0.0f,
                    180.0f, 1.0f,
                    session.solver_mode != 1 || !session.enable_angle);
  }
  override_toggle(tr("ov_bend_compliance"),
                  session.bone_ov_bend_compliance,
                  session.solver_mode != 0 || !session.enable_angle);
  if (session.bone_ov_bend_compliance) {
    override_slider(tr("bend_compliance"), session.bone_bend_compliance, 0.0f,
                    10.0f, 0.00001f,
                    session.solver_mode != 0 || !session.enable_angle);
  }
  override_toggle(tr("ov_pull"), session.bone_ov_pull,
                  session.enable_real_gravity);
  if (session.bone_ov_pull) {
    override_slider(tr("anim_follow"), session.bone_pull, 0.0f, 1.0f, 0.01f,
                    session.enable_real_gravity);
  }
  if (session.transition_mode == 2) {
    override_toggle(tr("ov_transition_follow"),
                    session.bone_ov_transition_follow);
    if (session.bone_ov_transition_follow) {
      override_slider(tr("transition_follow"),
                      session.bone_transition_follow, 0.0f, 1.0f, 0.05f);
    }
  }
  override_toggle(tr("ov_gravity"), session.bone_ov_gravity);
  if (session.bone_ov_gravity) {
    override_slider(tr("gravity_scale"), session.bone_gravity_scale, 0.0f,
                    5.0f, 0.1f);
  }
  override_toggle(tr("ov_wind"), session.bone_ov_wind);
  if (session.bone_ov_wind) {
    override_slider(tr("wind_scale"), session.bone_wind, 0.0f, 5.0f, 0.1f);
  }
  override_toggle(tr("ov_turbulence"), session.bone_ov_turbulence);
  if (session.bone_ov_turbulence) {
    override_slider(tr("turbulence_scale"), session.bone_turbulence, 0.0f,
                    5.0f, 0.1f);
  }
  override_toggle(tr("ov_fixed"), session.bone_ov_fixed);
  if (session.bone_ov_fixed &&
      check(ctx, g, tr("fixed_kinematic"), session.bone_fixed, busy)) {
    session.markSelectedBoneDraftDirty();
  }

  muted(ctx, g, session.hasUnappliedPerBoneDraft() ? tr("draft_unapplied")
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

const char *labPbrChannelLabel(gfx::LabPbrOverrideChannel channel) {
  switch (channel) {
  case gfx::LabPbrOverrideChannel::Roughness:
    return tr("labpbr_channel_roughness");
  case gfx::LabPbrOverrideChannel::Metal:
    return tr("labpbr_channel_metal");
  case gfx::LabPbrOverrideChannel::Porosity:
    return tr("labpbr_channel_volume");
  case gfx::LabPbrOverrideChannel::Emission:
    return tr("labpbr_channel_emission");
  }
  return "?";
}

void drawLabPbrEditor(nk_context *ctx, const Geom &g, AppSession &session) {
  heading(ctx, g, tr("labpbr_material"));
  mutedWrap(ctx, g, tr("labpbr_material_hint"));
  if (!session.model_texture.valid()) {
    muted(ctx, g, tr("labpbr_load_texture"));
    return;
  }

  char atlas_line[160];
  std::snprintf(atlas_line, sizeof(atlas_line), tr("labpbr_atlas_size"),
                session.model_texture.width, session.model_texture.height);
  muted(ctx, g, atlas_line);

  heading(ctx, g, tr("labpbr_specular_image"));
  const bool has_imported_specular =
      session.resolved_material.assets.specular_exists &&
      !session.resolved_material.assets.specular.empty();
  if (has_imported_specular) {
    const std::string file_line =
        std::string(tr("labpbr_specular_file")) +
        session.resolved_material.assets.specular.filename().string();
    mutedWrap(ctx, g, file_line.c_str());
  } else {
    mutedWrap(ctx, g, tr("labpbr_specular_none"));
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("labpbr_import_specular"))) {
    if (const auto path = openFileDialog(
            L"Import PBR Map (LabPBR RGBA PNG)",
            L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0")) {
      session.importLabPbrSpecular(*path);
    }
  }
  if (!has_imported_specular) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("labpbr_remove_specular")) &&
      has_imported_specular) {
    session.removeLabPbrSpecular();
  }
  if (!has_imported_specular) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("labpbr_preview"));
  const std::uint32_t material_flags =
      gfx::labPbrFeatureFlags(session.resolved_material.valid()
                                  ? &session.resolved_material
                                  : nullptr);
  char channel_line[192];
  std::snprintf(channel_line, sizeof(channel_line),
                tr("labpbr_gpu_channels"),
                (material_flags & gfx::kLabPbrNormalMapActive) != 0u
                    ? tr("labpbr_channel_enabled")
                    : tr("labpbr_channel_disabled"),
                (material_flags & gfx::kLabPbrSpecularMapActive) != 0u
                    ? tr("labpbr_channel_enabled")
                    : tr("labpbr_channel_disabled"));
  mutedWrap(ctx, g, channel_line);
  std::vector<const char *> debug_views{
      tr("labpbr_view_shaded"),    tr("labpbr_view_base_color"),
      tr("labpbr_view_normal"),    tr("labpbr_view_ao"),
      tr("labpbr_view_roughness"), tr("labpbr_view_f0"),
      tr("labpbr_view_emission"),  tr("labpbr_view_opacity")};
  int debug_view = static_cast<int>(session.labpbr_debug_view);
  const int previous_debug_view = debug_view;
  if (combo(ctx, g, tr("labpbr_preview_mode"), debug_views, debug_view,
            session.bake_busy.load()) &&
      debug_view != previous_debug_view) {
    session.labpbr_debug_view =
        static_cast<gfx::LabPbrDebugView>(debug_view);
    session.resetPathTraceAccumulation();
  }

  if (session.selected_bone_name.empty()) {
    muted(ctx, g, tr("labpbr_select_group"));
  } else {
    const std::string selected =
        std::string(tr("labpbr_selected_group")) +
        session.selected_bone_name;
    muted(ctx, g, selected.c_str());
    const auto *texels =
        session.labpbr_uv_coverage.find(session.selected_bone_name);
    char coverage_line[160];
    std::snprintf(coverage_line, sizeof(coverage_line),
                  tr("labpbr_coverage_texels"),
                  texels == nullptr ? 0u : texels->size());
    muted(ctx, g, coverage_line);
  }

  const bool no_group = session.selected_bone_name.empty();
  if (no_group) {
    nk_widget_disable_begin(ctx);
  }
  const auto draft_toggle = [&](const char *label, bool &enabled) {
    if (check(ctx, g, label, enabled, no_group)) {
      session.markLabPbrDraftDirty();
    }
  };
  const auto draft_slider = [&](const char *label, float &value) {
    if (slider(ctx, g, label, value, 0.0f, 1.0f, no_group, 0.01f)) {
      session.markLabPbrDraftDirty();
    }
  };

  heading(ctx, g, tr("labpbr_group_overrides"));
  draft_toggle(tr("labpbr_override_emission"),
               session.labpbr_draft.emission_enabled);
  if (session.labpbr_draft.emission_enabled) {
    draft_slider(tr("labpbr_emission"), session.labpbr_draft.emission);
  }
  draft_toggle(tr("labpbr_override_roughness"),
               session.labpbr_draft.roughness_enabled);
  if (session.labpbr_draft.roughness_enabled) {
    draft_slider(tr("labpbr_roughness"), session.labpbr_draft.roughness);
  }
  draft_toggle(tr("labpbr_override_metal"),
               session.labpbr_draft.metal_enabled);
  if (session.labpbr_draft.metal_enabled) {
    if (check(ctx, g, tr("labpbr_is_metal"), session.labpbr_draft.metal,
              no_group)) {
      session.markLabPbrDraftDirty();
    }
    int metal_value =
        session.labpbr_draft.metal
            ? static_cast<int>(session.labpbr_draft.metal_code)
            : static_cast<int>(session.labpbr_draft.dielectric_f0);
    if (intProperty(ctx, g,
                    session.labpbr_draft.metal
                        ? tr("labpbr_metal_code")
                        : tr("labpbr_dielectric_f0"),
                    metal_value, session.labpbr_draft.metal ? 230 : 0,
                    session.labpbr_draft.metal ? 255 : 229, 1, no_group)) {
      if (session.labpbr_draft.metal) {
        session.labpbr_draft.metal_code =
            static_cast<std::uint8_t>(metal_value);
      } else {
        session.labpbr_draft.dielectric_f0 =
            static_cast<std::uint8_t>(metal_value);
      }
      session.markLabPbrDraftDirty();
    }
  }
  draft_toggle(tr("labpbr_override_volume"),
               session.labpbr_draft.porosity_enabled);
  if (session.labpbr_draft.porosity_enabled) {
    if (check(ctx, g, tr("labpbr_use_sss"),
              session.labpbr_draft.subsurface_scattering, no_group)) {
      session.markLabPbrDraftDirty();
    }
    if (session.labpbr_draft.subsurface_scattering) {
      draft_slider(tr("labpbr_sss"), session.labpbr_draft.subsurface);
    } else {
      draft_slider(tr("labpbr_porosity"), session.labpbr_draft.porosity);
    }
  }

  char encoded[192];
  const auto &draft = session.labpbr_draft;
  const unsigned encoded_metal =
      draft.metal ? draft.metal_code : draft.dielectric_f0;
  std::snprintf(encoded, sizeof(encoded), tr("labpbr_encoded_argb"),
                static_cast<unsigned>(
                    gfx::encodeLabPbrEmission(draft.emission)),
                static_cast<unsigned>(
                    gfx::encodeLabPbrRoughness(draft.roughness)),
                encoded_metal,
                static_cast<unsigned>(
                    draft.subsurface_scattering
                        ? gfx::encodeLabPbrSubsurface(draft.subsurface)
                        : gfx::encodeLabPbrPorosity(draft.porosity)));
  muted(ctx, g, encoded);
  muted(ctx, g,
        session.labpbr_draft_dirty ? tr("labpbr_draft_unapplied")
                                   : tr("labpbr_draft_applied"));

  nk_layout_row_dynamic(ctx, g.btn, 3);
  if (nk_button_label(ctx, tr("labpbr_apply")) && !no_group) {
    session.applySelectedLabPbrDraft();
  }
  if (nk_button_label(ctx, tr("labpbr_revert")) && !no_group) {
    session.revertSelectedLabPbrDraft();
  }
  if (nk_button_label(ctx, tr("labpbr_restore_texture")) && !no_group) {
    session.restoreSelectedLabPbrFromTexture();
  }
  if (no_group) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("labpbr_uv_conflicts"));
  if (session.labpbr_composition.conflicts.empty()) {
    muted(ctx, g, tr("labpbr_no_conflicts"));
  } else {
    mutedWrap(ctx, g, tr("labpbr_conflicts_block_export"));
    std::map<std::string, std::size_t> conflict_counts;
    for (const auto &conflict : session.labpbr_composition.conflicts) {
      std::string groups;
      for (const auto &group : conflict.groups) {
        if (!groups.empty()) {
          groups += " / ";
        }
        groups += group;
      }
      const std::string key =
          std::string(labPbrChannelLabel(conflict.channel)) + " · " + groups;
      ++conflict_counts[key];
    }
    for (const auto &[description, count] : conflict_counts) {
      char line[320];
      std::snprintf(line, sizeof(line), tr("labpbr_conflict_line"),
                    description.c_str(), count);
      mutedWrap(ctx, g, line);
    }
  }

  heading(ctx, g, tr("labpbr_iris_normal"));
  if (session.labpbr_imported_normal.valid()) {
    const std::string file_line =
        std::string(tr("labpbr_normal_file")) +
        session.labpbr_imported_normal.source_path.filename().string();
    muted(ctx, g, file_line.c_str());
    const std::string checksum_line =
        std::string(tr("labpbr_normal_checksum")) +
        session.labpbr_imported_normal.sha256;
    mutedWrap(ctx, g, checksum_line.c_str());
  } else {
    mutedWrap(ctx, g, tr("labpbr_normal_none"));
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("labpbr_import_normal"))) {
    if (const auto path = openFileDialog(
            L"Import Normal Map (Iris RGBA PNG)",
            L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0")) {
      session.importLabPbrNormal(*path);
    }
  }
  const bool can_remove_normal = session.labpbr_imported_normal.valid();
  if (!can_remove_normal) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("labpbr_remove_normal")) &&
      can_remove_normal) {
    session.removeLabPbrNormal();
  }
  if (!can_remove_normal) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("labpbr_export"));
  mutedWrap(ctx, g, tr("labpbr_export_hint"));
  const bool can_export = session.labpbr_composition.exportable();
  nk_layout_row_dynamic(ctx, g.btn + 4.0f, 1);
  if (!can_export) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("labpbr_export_button")) && can_export) {
    std::wstring default_name = L"material_s.png";
    if (!session.texture_path.empty()) {
      default_name =
          std::filesystem::path(session.texture_path).stem().wstring() +
          L"_s.png";
    }
    if (const auto path =
            savePngFileDialog(L"Export LabPBR Texture Bundle",
                              default_name.c_str())) {
      session.requestLabPbrExport(*path);
    }
  }
  if (!can_export) {
    nk_widget_disable_end(ctx);
  }
}

void drawRendererEditor(nk_context *ctx, const Geom &g, AppSession &session,
                        const gfx::FrameStats &stats,
                        const gfx::RayTracingCapability *rt_cap) {
  const bool busy =
      session.bake_busy.load() || session.stillRenderActive();
  heading(ctx, g, tr("renderer_page"));
  mutedWrap(ctx, g, tr("renderer_page_hint"));

  // Physics owns scene selection and World/Sky owns the sky source. Renderer
  // owns only the engine/integrator/post/display policy.
  heading(ctx, g, tr("pt_engine"));
  const auto availability = gfx::queryVulkanPathTraceAvailability();
  const bool hw_ok = rt_cap != nullptr && rt_cap->supported &&
                     rt_cap->device_extensions_enabled;
  const bool rt_supported = hw_ok || availability.path_tracer_ready;
  if (!rt_supported && session.enable_ray_tracing) {
    session.enable_ray_tracing = false;
  }
  check(ctx, g, tr("enable_ray_tracing"), session.enable_ray_tracing,
        !rt_supported || busy);
  char status_line[320];
  std::snprintf(status_line, sizeof(status_line),
                tr("pt_requested_status"),
                session.enable_ray_tracing ? tr("pt_path_tracing")
                                           : tr("pt_path_raster"));
  muted(ctx, g, status_line);
  if (rt_supported && session.enable_ray_tracing) {
    const auto impl = gfx::selectVulkanPathTraceImplementation(
        true, rt_cap ? *rt_cap : gfx::RayTracingCapability{}, availability);
    const char *active =
        impl == gfx::VulkanPathTraceImplementation::RayTracingPipeline
            ? tr("pt_path_tracing")
            : impl == gfx::VulkanPathTraceImplementation::RayQuery
                  ? tr("pt_path_ray_query")
                  : tr("pt_path_raster");
    std::snprintf(status_line, sizeof(status_line),
                  tr("pt_active_status"), active);
    muted(ctx, g, status_line);
  } else if (!rt_supported) {
    muted(ctx, g, tr("ray_tracing_unsupported"));
    if (rt_cap != nullptr && !rt_cap->unsupported_reason.empty()) {
      mutedWrap(ctx, g, rt_cap->unsupported_reason.c_str());
    }
  } else {
    muted(ctx, g, tr("raster_path_hint"));
  }
  const char *gpu_name =
      rt_cap != nullptr && !rt_cap->device_name.empty()
          ? rt_cap->device_name.c_str()
          : tr("pt_unavailable");
  std::snprintf(status_line, sizeof(status_line), tr("pt_gpu_status"),
                gpu_name);
  mutedWrap(ctx, g, status_line);

  gfx::PathTraceSettings settings = session.path_trace_settings;
  bool settings_changed = false;
  bool preset_parameter_changed = false;
  const bool controls_disabled = busy || !session.enable_ray_tracing;
  const auto changed = [&](bool preset_parameter = true) {
    settings_changed = true;
    preset_parameter_changed =
        preset_parameter_changed || preset_parameter;
  };
  const auto toggle = [&](const char *label, bool &field, bool disabled,
                          bool preset_parameter = true) {
    if (check(ctx, g, label, field, disabled)) {
      changed(preset_parameter);
    }
  };
  const auto uintProperty = [&](const char *label, std::uint32_t &field,
                                int lo, int hi, bool disabled,
                                bool preset_parameter = true) {
    int value = static_cast<int>(std::min<std::uint32_t>(
        field, static_cast<std::uint32_t>(hi)));
    if (intProperty(ctx, g, label, value, lo, hi, 1, disabled)) {
      field = static_cast<std::uint32_t>(std::clamp(value, lo, hi));
      changed(preset_parameter);
    }
  };
  const auto floatSlider = [&](const char *label, float &field, float lo,
                               float hi, float step, bool disabled,
                               bool preset_parameter = true) {
    if (slider(ctx, g, label, field, lo, hi, disabled, step)) {
      changed(preset_parameter);
    }
  };

  const bool nvidia_rt_pipeline_available =
      hw_ok && rt_cap != nullptr && rt_cap->is_nvidia &&
      availability.ray_tracing_pipeline_ready;
  toggle(tr("pt_nvidia_rt_core"),
         settings.nvidia_rt_core_acceleration,
         controls_disabled || !nvidia_rt_pipeline_available, false);
  if (!nvidia_rt_pipeline_available) {
    mutedWrap(ctx, g, tr("pt_nvidia_rt_core_unavailable"));
  } else {
    muted(ctx, g,
          settings.nvidia_rt_core_acceleration &&
                  !settings.force_software_fallback
              ? tr("pt_nvidia_rt_core_active")
              : tr("pt_nvidia_rt_core_inactive"));
  }
  std::snprintf(status_line, sizeof(status_line),
                tr("pt_preview_resolution_status"),
                settings.preview_resolution_scale * 100.0f);
  muted(ctx, g, status_line);

  heading(ctx, g, tr("pt_preset"));
  std::vector<const char *> presets{
      tr("pt_preset_realtime"), tr("pt_preset_balanced"),
      tr("pt_preset_high_quality"), tr("pt_preset_reference"),
      tr("pt_preset_custom")};
  int preset = static_cast<int>(settings.preset);
  const int previous_preset = preset;
  if (combo(ctx, g, tr("pt_preset"), presets, preset,
            controls_disabled) &&
      preset != previous_preset) {
    settings = gfx::applyPathTracePreset(
        settings, static_cast<gfx::PathTracePreset>(preset));
    settings_changed = true;
  }
  const bool can_restore =
      settings.preset == gfx::PathTracePreset::Custom;
  if (!can_restore || controls_disabled) {
    nk_widget_disable_begin(ctx);
  }
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("pt_restore_preset")) &&
      can_restore && !controls_disabled) {
    settings = gfx::restorePathTraceSourcePreset(settings);
    settings_changed = true;
  }
  if (!can_restore || controls_disabled) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("pt_sampling"));
  mutedWrap(ctx, g, tr("path_trace_settings_hint"));
  constexpr std::array<std::uint32_t, 6> kSppValues{
      1u, 2u, 4u, 8u, 16u, 32u};
  std::vector<const char *> spp_labels{"1", "2", "4", "8", "16", "32"};
  int spp_index = 0;
  for (std::size_t i = 0; i < kSppValues.size(); ++i) {
    if (settings.samples_per_frame == kSppValues[i]) {
      spp_index = static_cast<int>(i);
      break;
    }
  }
  const int previous_spp = spp_index;
  if (combo(ctx, g, tr("pt_samples_per_frame"), spp_labels, spp_index,
            controls_disabled) &&
      spp_index != previous_spp) {
    settings.samples_per_frame =
        kSppValues[static_cast<std::size_t>(spp_index)];
    changed();
  }
  uintProperty(tr("pt_max_samples"), settings.maximum_samples,
               0, 65'536, controls_disabled);
  muted(ctx, g, tr("pt_max_samples_hint"));
  toggle(tr("pt_seed_auto"), settings.automatic_seed,
         controls_disabled);
  if (!settings.automatic_seed) {
    uintProperty(tr("pt_seed"), settings.seed, 0, 1'000'000,
                 controls_disabled);
  }
  const bool adaptive_unavailable = true;
  toggle(tr("pt_adaptive"), settings.adaptive_sampling,
         controls_disabled || adaptive_unavailable);
  floatSlider(tr("pt_adaptive_threshold"),
              settings.adaptive_noise_threshold, 0.0001f, 1.0f,
              0.001f, true);
  uintProperty(tr("pt_adaptive_min_samples"),
               settings.adaptive_minimum_samples, 1, 65'536, true);
  mutedWrap(ctx, g, tr("pt_spp_unsupported"));
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("pt_reset_accumulation")) &&
      !controls_disabled) {
    session.resetPathTraceAccumulation();
  }

  heading(ctx, g, tr("pt_light_paths"));
  uintProperty(tr("pt_max_bounces"), settings.max_bounces, 1, 64,
               controls_disabled);
  uintProperty(tr("pt_diffuse_bounces"),
               settings.max_diffuse_bounces, 0, 16,
               controls_disabled);
  uintProperty(tr("pt_glossy_bounces"),
               settings.max_glossy_bounces, 0, 16,
               controls_disabled);
  uintProperty(tr("pt_transmission_bounces"),
               settings.max_transmission_bounces, 0, 32,
               controls_disabled);
  uintProperty(tr("pt_transparent_bounces"),
               settings.max_transparent_bounces, 0, 64,
               controls_disabled);
  mutedWrap(ctx, g, tr("pt_transparent_budget_hint"));
  toggle(tr("pt_russian_roulette"), settings.russian_roulette,
         controls_disabled);
  uintProperty(tr("pt_russian_roulette_start"),
               settings.russian_roulette_start, 1,
               static_cast<int>(settings.max_bounces),
               controls_disabled);

  heading(ctx, g, tr("pt_lighting"));
  toggle(tr("pt_analytic_lights"), settings.analytic_lights,
         controls_disabled);
  toggle(tr("pt_emissive_surfaces"), settings.emissive_surfaces,
         controls_disabled);
  floatSlider(tr("pt_emissive_multiplier"),
              settings.emissive_multiplier, 0.0f, 16.0f, 0.05f,
              controls_disabled);
  uintProperty(tr("pt_light_samples"),
               settings.light_samples_per_path, 1, 16,
               controls_disabled);
  if (check(ctx, g, tr("sky_environment_lighting"),
            session.world_environment.environment_lighting,
            controls_disabled)) {
    session.touchWorldEnvironment();
  }
  if (check(ctx, g, tr("sky_sun_moon_lighting"),
            session.world_environment.sun_moon_lighting,
            controls_disabled)) {
    session.touchWorldEnvironment();
  }
  mutedWrap(ctx, g, tr("pt_world_lighting_hint"));

  heading(ctx, g, tr("pt_denoise_upscale"));
  constexpr std::array<gfx::PathTraceDenoiser, 3> kDenoiserModes{{
      gfx::PathTraceDenoiser::Auto,
      gfx::PathTraceDenoiser::DlssRayReconstruction,
      gfx::PathTraceDenoiser::Raw,
  }};
  std::vector<const char *> denoisers{
      tr("pt_denoiser_auto"), tr("pt_denoiser_rr"),
      tr("pt_denoiser_raw")};
  const auto denoiser_mode_index =
      [&](gfx::PathTraceDenoiser mode) {
        const auto it = std::find(kDenoiserModes.begin(),
                                  kDenoiserModes.end(), mode);
        return it == kDenoiserModes.end()
                   ? static_cast<int>(kDenoiserModes.size() - 1u)
                   : static_cast<int>(
                         std::distance(kDenoiserModes.begin(), it));
      };
  int denoiser =
      denoiser_mode_index(settings.requested_denoiser);
  const int previous_denoiser = denoiser;
  if (combo(ctx, g, tr("pt_denoiser"), denoisers, denoiser, busy) &&
      denoiser != previous_denoiser) {
    settings.requested_denoiser =
        kDenoiserModes[static_cast<std::size_t>(denoiser)];
    changed();
  }
  constexpr std::array<gfx::PathTraceUpscale, 6> kUpscaleModes{{
      gfx::PathTraceUpscale::Off,
      gfx::PathTraceUpscale::Dlaa,
      gfx::PathTraceUpscale::Quality,
      gfx::PathTraceUpscale::Balanced,
      gfx::PathTraceUpscale::Performance,
      gfx::PathTraceUpscale::UltraPerformance,
  }};
  std::vector<const char *> upscalers{
      tr("pt_upscale_off"), tr("pt_upscale_dlaa"),
      tr("pt_upscale_quality"), tr("pt_upscale_balanced"),
      tr("pt_upscale_performance"), tr("pt_upscale_ultra_performance")};
  const auto upscale_mode_index = [&](gfx::PathTraceUpscale mode) {
    const auto it = std::find(
        kUpscaleModes.begin(), kUpscaleModes.end(), mode);
    return it != kUpscaleModes.end()
               ? static_cast<int>(
                     std::distance(kUpscaleModes.begin(), it))
               : 2;
  };
  int upscale = upscale_mode_index(settings.requested_upscale);
  const int previous_upscale = upscale;
  if (combo(ctx, g, tr("pt_upscale"), upscalers, upscale, busy) &&
      upscale != previous_upscale) {
    settings.requested_upscale =
        kUpscaleModes[static_cast<std::size_t>(upscale)];
    changed();
  }

  bool frame_generation =
      settings.requested_frame_generation ==
      gfx::PathTraceFrameGeneration::On;
  if (check(ctx, g, tr("pt_frame_generation"), frame_generation,
            controls_disabled ||
                !session.path_trace_post_process_capabilities
                     .dlss_frame_generation)) {
    settings.requested_frame_generation =
        frame_generation ? gfx::PathTraceFrameGeneration::On
                         : gfx::PathTraceFrameGeneration::Off;
    changed(false);
  }
  if (!session.path_trace_post_process_capabilities
           .dlss_frame_generation) {
    mutedWrap(ctx, g, tr("pt_frame_generation_unavailable"));
  } else if (frame_generation) {
    mutedWrap(ctx, g, tr("pt_fg_vulkan_vsync"));
  }

  constexpr std::array<gfx::PathTraceReflexMode, 3> kReflexModes{{
      gfx::PathTraceReflexMode::Off,
      gfx::PathTraceReflexMode::On,
      gfx::PathTraceReflexMode::OnBoost,
  }};
  std::vector<const char *> reflex_modes{
      tr("pt_reflex_off"), tr("pt_reflex_on"),
      tr("pt_reflex_on_boost")};
  int reflex_mode = static_cast<int>(
      settings.requested_reflex_mode);
  reflex_mode = std::clamp(
      reflex_mode, 0,
      static_cast<int>(kReflexModes.size() - 1u));
  const int previous_reflex_mode = reflex_mode;
  if (combo(ctx, g, tr("pt_reflex"), reflex_modes, reflex_mode,
            controls_disabled ||
                !session.path_trace_post_process_capabilities.reflex) &&
      reflex_mode != previous_reflex_mode) {
    settings.requested_reflex_mode =
        kReflexModes[static_cast<std::size_t>(reflex_mode)];
    changed(false);
  }
  if (!session.path_trace_post_process_capabilities.reflex) {
    mutedWrap(ctx, g, tr("pt_reflex_unavailable"));
  } else if (frame_generation &&
             settings.requested_reflex_mode ==
                 gfx::PathTraceReflexMode::Off) {
    mutedWrap(ctx, g, tr("pt_fg_requires_reflex"));
  }

  const auto post = gfx::resolvePathTracePostProcess(
      settings, session.path_trace_post_process_capabilities);
  std::snprintf(status_line, sizeof(status_line), tr("pt_post_requested"),
                denoisers[static_cast<std::size_t>(
                    denoiser_mode_index(settings.requested_denoiser))],
                upscalers[static_cast<std::size_t>(
                    upscale_mode_index(settings.requested_upscale))]);
  muted(ctx, g, status_line);
  std::snprintf(status_line, sizeof(status_line), tr("pt_post_active"),
                denoisers[static_cast<std::size_t>(
                    denoiser_mode_index(post.active_denoiser))],
                upscalers[static_cast<std::size_t>(
                    upscale_mode_index(post.active_upscale))]);
  muted(ctx, g, status_line);
  if (!post.denoiser_supported || !post.upscale_supported) {
    mutedWrap(ctx, g, tr("pt_post_unavailable"));
    if (!session.path_trace_post_process_status.empty()) {
      std::snprintf(status_line, sizeof(status_line),
                    tr("pt_post_backend_status"),
                    session.path_trace_post_process_status.c_str());
      mutedWrap(ctx, g, status_line);
    }
  }
  if (post.conflict_resolved) {
    mutedWrap(ctx, g, tr("pt_post_conflict"));
  }
  if (post.rr_mode_required) {
    mutedWrap(ctx, g, tr("pt_post_rr_mode_required"));
  }
  if (frame_generation &&
      !session.path_trace_post_process_status.empty()) {
    std::snprintf(status_line, sizeof(status_line),
                  tr("pt_fg_backend_status"),
                  session.path_trace_post_process_status.c_str());
    mutedWrap(ctx, g, status_line);
  }
  if (session.debug_dlss_frame_generation_active) {
    std::snprintf(status_line, sizeof(status_line),
                  tr("pt_fg_fps_status"),
                  session.debug_original_fps,
                  session.debug_dlss_fg_fps);
    muted(ctx, g, status_line);
  } else if (frame_generation) {
    muted(ctx, g, tr("pt_fg_waiting"));
  }

  heading(ctx, g, tr("pt_film"));
  toggle(tr("pt_transparent_background"),
         settings.transparent_background, busy);
  floatSlider(tr("pt_display_exposure"), settings.display_exposure_ev,
              -8.0f, 8.0f, 0.1f, busy);
  std::vector<const char *> tone_maps{
      tr("pt_tone_none"), tr("pt_tone_reinhard"), tr("pt_tone_aces")};
  int tone_map = static_cast<int>(settings.tone_mapping);
  const int previous_tone_map = tone_map;
  if (combo(ctx, g, tr("pt_tone_mapping"), tone_maps, tone_map, busy) &&
      tone_map != previous_tone_map) {
    settings.tone_mapping =
        static_cast<gfx::PathTraceToneMapping>(tone_map);
    changed();
  }
  int white_balance =
      static_cast<int>(settings.white_balance_kelvin);
  if (intProperty(ctx, g, tr("pt_white_balance"), white_balance,
                  1000, 40'000, 50, busy)) {
    settings.white_balance_kelvin =
        static_cast<float>(white_balance);
    changed();
  }
  floatSlider(tr("pt_bloom"), settings.bloom_strength, 0.0f, 4.0f,
              0.05f, busy);
  mutedWrap(ctx, g, tr("pt_film_hint"));

  heading(ctx, g, tr("pt_performance"));
  floatSlider(tr("pt_preview_scale"),
              settings.preview_resolution_scale, 0.25f, 1.0f,
              0.05f, controls_disabled);
  floatSlider(tr("pt_target_frame_time"),
              settings.target_frame_time_ms, 4.0f, 100.0f, 1.0f,
              controls_disabled);
  std::vector<const char *> interactive_qualities{
      tr("pt_interactive_full"), tr("pt_interactive_balanced"),
      tr("pt_interactive_fast")};
  int interactive_quality =
      static_cast<int>(settings.interactive_quality);
  const int previous_interactive_quality = interactive_quality;
  if (combo(ctx, g, tr("pt_interactive_quality"),
            interactive_qualities, interactive_quality,
            controls_disabled) &&
      interactive_quality != previous_interactive_quality) {
    settings.interactive_quality =
        static_cast<gfx::PathTraceInteractiveQuality>(
            interactive_quality);
    changed();
  }
  toggle(tr("pt_accumulate_moving"),
         settings.accumulate_while_moving, true);
  mutedWrap(ctx, g, tr("pt_accumulate_moving_unavailable"));
  toggle(tr("pt_pause_accumulation"), settings.pause_accumulation,
         controls_disabled, false);
  char performance_line[192];
  std::snprintf(performance_line, sizeof(performance_line),
                tr("pt_timing_status"),
                session.debug_backend_cpu_ms, session.debug_gpu_ms,
                static_cast<unsigned long long>(
                    settings.reset_generation));
  muted(ctx, g, performance_line);
  std::snprintf(performance_line, sizeof(performance_line),
                tr("pt_memory_status"),
                static_cast<double>(stats.rt_allocated_bytes) /
                    (1024.0 * 1024.0),
                static_cast<unsigned long long>(
                    settings.target_generation));
  muted(ctx, g, performance_line);

  heading(ctx, g, tr("pt_advanced"));
  toggle(tr("pt_nee"), settings.next_event_estimation,
         controls_disabled);
  toggle(tr("pt_mis"), settings.multiple_importance_sampling,
         controls_disabled);
  toggle(tr("pt_environment_importance"),
         settings.environment_importance_sampling,
         controls_disabled);
  const bool emissive_mesh_available =
      nvidia_rt_pipeline_available &&
      settings.nvidia_rt_core_acceleration &&
      !settings.force_software_fallback;
  toggle(tr("pt_emissive_mesh_sampling"),
         settings.emissive_mesh_sampling,
         controls_disabled || !emissive_mesh_available);
  if (!emissive_mesh_available) {
    mutedWrap(ctx, g, tr("pt_emissive_mesh_unavailable"));
  }
  floatSlider(tr("pt_direct_clamp"), settings.direct_clamp,
              0.0f, 100.0f, 0.25f, controls_disabled);
  floatSlider(tr("pt_indirect_clamp"), settings.indirect_clamp,
              0.0f, 100.0f, 0.25f, controls_disabled);
  mutedWrap(ctx, g, tr("pt_clamp_hint"));

  heading(ctx, g, tr("pt_debug"));
  std::vector<const char *> debug_views{
      tr("pt_debug_off"), tr("pt_debug_instance"),
      tr("pt_debug_primitive"), tr("pt_debug_cube"),
      tr("pt_debug_face"), tr("pt_debug_material"),
      tr("pt_debug_normal"), tr("pt_debug_albedo"),
      tr("pt_debug_roughness"), tr("pt_debug_emission")};
  int debug_view = static_cast<int>(session.rt_debug_view);
  const int previous_debug_view = debug_view;
  if (combo(ctx, g, tr("pt_debug_view"), debug_views, debug_view,
            controls_disabled) &&
      debug_view != previous_debug_view) {
    session.rt_debug_view =
        static_cast<gfx::RtDebugView>(debug_view);
  }
  toggle(tr("pt_developer_controls"), settings.developer_controls,
         busy, false);
  if (settings.developer_controls) {
    toggle(tr("pt_force_compatibility"),
           settings.force_software_fallback,
           controls_disabled, false);
  }

  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("pt_save_settings")) && !busy) {
    if (const auto path =
            saveFileDialog(L"Save Path Tracing Settings",
                           L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0",
                           L"path_tracing.json")) {
      session.savePathTraceSettings(*path);
    }
  }
  if (nk_button_label(ctx, tr("pt_load_settings")) && !busy) {
    if (const auto path = openFileDialog(
            L"Load Path Tracing Settings",
            L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0")) {
      session.loadPathTraceSettings(*path);
      settings = session.path_trace_settings;
      settings_changed = false;
      preset_parameter_changed = false;
    }
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("pt_freeze_snapshot")) && !busy) {
    session.freezePathTraceRenderSnapshot();
  }
  const bool has_snapshot =
      session.path_trace_render_snapshot.has_value();
  if (!has_snapshot) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("pt_clear_snapshot")) &&
      has_snapshot) {
    session.clearPathTraceRenderSnapshot();
  }
  if (!has_snapshot) {
    nk_widget_disable_end(ctx);
  }
  muted(ctx, g, has_snapshot ? tr("pt_snapshot_frozen")
                             : tr("pt_snapshot_live"));

  heading(ctx, g, tr("still_render"));
  mutedWrap(ctx, g, tr("still_render_hint"));
  mutedWrap(ctx, g, tr("still_raw_accumulation"));
  auto &still_job = session.still_render_job;
  const bool still_active = session.stillRenderActive();
  const bool still_controls_disabled =
      still_active || session.bake_busy.load();
  std::array<char, 128> filename_buffer{};
  const std::size_t filename_bytes =
      (std::min)(still_job.settings.filename.size(),
                 filename_buffer.size() - 1u);
  std::copy_n(still_job.settings.filename.data(), filename_bytes,
              filename_buffer.data());
  int filename_length = static_cast<int>(filename_bytes);
  if (still_controls_disabled) {
    nk_widget_disable_begin(ctx);
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  nk_label(ctx, tr("still_filename"), NK_TEXT_LEFT);
  const nk_flags edit_result = nk_edit_string(
      ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, filename_buffer.data(),
      &filename_length, static_cast<int>(filename_buffer.size() - 1u),
      nk_filter_default);
  if (!still_controls_disabled &&
      (edit_result & (NK_EDIT_ACTIVE | NK_EDIT_COMMITED))) {
    still_job.settings.filename.assign(
        filename_buffer.data(),
        static_cast<std::size_t>((std::max)(0, filename_length)));
  }
  int still_width = static_cast<int>(still_job.settings.width);
  if (intProperty(ctx, g, tr("still_width"), still_width, 64, 4'096,
                  1, still_controls_disabled)) {
    still_job.settings.width =
        static_cast<std::uint32_t>(still_width);
  }
  int still_height = static_cast<int>(still_job.settings.height);
  if (intProperty(ctx, g, tr("still_height"), still_height, 64, 4'096,
                  1, still_controls_disabled)) {
    still_job.settings.height =
        static_cast<std::uint32_t>(still_height);
  }
  int still_samples =
      static_cast<int>(still_job.settings.target_samples);
  if (intProperty(ctx, g, tr("still_target_samples"), still_samples, 32,
                  65'536, 1, still_controls_disabled)) {
    still_job.settings.target_samples =
        static_cast<std::uint32_t>(still_samples);
  }
  int still_submit =
      static_cast<int>(still_job.settings.samples_per_submit);
  if (intProperty(ctx, g, tr("still_samples_per_submit"), still_submit, 1,
                  32, 1, still_controls_disabled)) {
    still_job.settings.samples_per_submit =
        static_cast<std::uint32_t>(still_submit);
  }
  std::vector<const char *> still_formats{tr("still_format_png"),
                                          tr("still_format_exr")};
  int still_format = static_cast<int>(still_job.settings.format);
  if (combo(ctx, g, tr("still_format"), still_formats, still_format,
            still_controls_disabled)) {
    still_job.settings.format =
        static_cast<gfx::StillImageFormat>(still_format);
  }
  (void)check(ctx, g, tr("still_transparent_background"),
              still_job.settings.transparent_background,
              still_controls_disabled);
  if (still_controls_disabled) {
    nk_widget_disable_end(ctx);
  }
  const std::string output_directory =
      session.stillRenderOutputDirectory().string();
  char output_line[512]{};
  std::snprintf(output_line, sizeof(output_line),
                tr("still_output_directory"),
                output_directory.empty() ? "-" : output_directory.c_str());
  mutedWrap(ctx, g, output_line);

  const bool can_start_still =
      !still_controls_disabled && session.enable_ray_tracing &&
      rt_supported &&
      session.path_trace_settings.nvidia_rt_core_acceleration &&
      !session.path_trace_settings.force_software_fallback;
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (!can_start_still) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("still_start")) && can_start_still) {
    session.queueStillRender();
  }
  if (!can_start_still) {
    nk_widget_disable_end(ctx);
  }
  if (!still_active) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("still_cancel")) && still_active) {
    session.requestStillRenderCancel();
  }
  if (!still_active) {
    nk_widget_disable_end(ctx);
  }
  if (!session.enable_ray_tracing || !rt_supported) {
    mutedWrap(ctx, g, tr("still_requires_rt"));
  }

  const auto &still_status = still_job.status;
  if (still_status.state != gfx::StillRenderJobState::Idle) {
    char progress_line[256]{};
    const double progress =
        still_status.target_samples > 0u
            ? 100.0 * static_cast<double>(
                          (std::min)(still_status.accumulated_samples,
                                     still_status.target_samples)) /
                  static_cast<double>(still_status.target_samples)
            : 0.0;
    std::snprintf(
        progress_line, sizeof(progress_line), tr("still_progress"),
        still_status.accumulated_samples, still_status.target_samples,
        progress);
    muted(ctx, g, progress_line);
    if (still_status.state == gfx::StillRenderJobState::Saving) {
      muted(ctx, g, tr("still_saving"));
    } else if (still_status.state ==
               gfx::StillRenderJobState::Completed) {
      muted(ctx, g, tr("still_completed"));
      mutedWrap(ctx, g, still_status.output_path.c_str());
    } else if (still_status.state ==
               gfx::StillRenderJobState::Cancelled) {
      muted(ctx, g, tr("still_cancelled"));
    } else if (still_status.state ==
               gfx::StillRenderJobState::Failed) {
      muted(ctx, g, tr("still_failed"));
      mutedWrap(ctx, g, still_status.error.c_str());
    }
  }

  if (settings_changed) {
    if (preset_parameter_changed &&
        settings.preset != gfx::PathTracePreset::Custom) {
      settings.source_preset = settings.preset;
      settings.preset = gfx::PathTracePreset::Custom;
    }
    (void)session.applyPathTraceSettings(settings);
  }
}

void drawSkyEditor(nk_context *ctx, const Geom &g, AppSession &session) {
  const bool busy =
      session.bake_busy.load() || session.stillRenderActive();
  auto &world = session.world_environment;
  heading(ctx, g, tr("sky_page"));
  mutedWrap(ctx, g, tr("sky_page_hint"));
  mutedWrap(ctx, g, tr("sky_scene_independent"));

  std::vector<const char *> modes{tr("sky_rendering_off"),
                                  tr("sky_rendering_procedural"),
                                  tr("sky_rendering_hdri")};
  int mode = static_cast<int>(world.sky_rendering);
  const int previous_mode = mode;
  if (combo(ctx, g, tr("sky_rendering"), modes, mode, busy) &&
      mode != previous_mode) {
    if (!session.setSkyRendering(static_cast<gfx::SkyRendering>(mode))) {
      mode = previous_mode;
    }
  }
  const auto resolved = gfx::resolveWorldEnvironment(session.world_environment);
  if (!resolved.warning.empty()) {
    mutedWrap(ctx, g, resolved.warning.c_str());
  }
  if (!session.last_error.empty() &&
      session.status.find("World Sky") != std::string::npos) {
    mutedWrap(ctx, g, session.last_error.c_str());
  }

  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("sky_save_settings")) && !busy) {
    if (const auto path =
            saveFileDialog(L"Save World Sky Settings",
                           L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0",
                           L"world_sky.json")) {
      session.saveWorldSkySettings(*path);
    }
  }
  if (nk_button_label(ctx, tr("sky_load_settings")) && !busy) {
    if (const auto path = openFileDialog(
            L"Load World Sky Settings",
            L"JSON File (*.json)\0*.json\0All Files (*.*)\0*.*\0")) {
      session.loadWorldSkySettings(*path);
    }
  }

  const bool sky_off = world.sky_rendering == gfx::SkyRendering::Off;
  const bool sky_busy = busy || sky_off;
  heading(ctx, g, tr("sky_global_lighting"));
  if (check(ctx, g, tr("sky_environment_lighting"),
            world.environment_lighting, sky_busy)) {
    session.touchWorldEnvironment();
  }
  if (check(ctx, g, tr("sky_sun_moon_lighting"),
            world.sun_moon_lighting, sky_busy)) {
    session.touchWorldEnvironment();
  }
  if (slider(ctx, g, tr("sky_global_strength_ev"),
             world.global_lighting_strength_ev, -10.0f, 10.0f, sky_busy,
             0.1f)) {
    session.touchWorldEnvironment();
  }
  mutedWrap(ctx, g, tr("sky_global_strength_hint"));
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("sky_reset_physical")) && !sky_busy) {
    session.resetWorldSkyPhysicalDefaults();
  }

  heading(ctx, g, tr("sky_background"));
  if (check(ctx, g, tr("sky_background_visible"),
            world.background_visible, sky_busy)) {
    session.touchWorldEnvironmentDisplay();
  }
  if (check(ctx, g, tr("sky_background_transparent"),
            world.background_transparent, sky_busy)) {
    session.touchWorldEnvironmentDisplay();
  }
  if (slider(ctx, g, tr("sky_background_exposure"),
             world.background_exposure, -10.0f, 10.0f, sky_busy, 0.1f)) {
    session.touchWorldEnvironmentDisplay();
  }
  mutedWrap(ctx, g, tr("sky_background_exposure_hint"));
  float rotation_degrees = world.rotation_radians *
                           (180.0f / 3.14159265358979323846f);
  if (slider(ctx, g, tr("sky_rotation"), rotation_degrees, -180.0f, 180.0f,
             sky_busy, 1.0f)) {
    world.rotation_radians =
        rotation_degrees * (3.14159265358979323846f / 180.0f);
    session.touchWorldEnvironment();
  }

  heading(ctx, g, tr("sky_hdri"));
  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (nk_button_label(ctx, tr("sky_load_hdri")) && !busy) {
    if (const auto path = openFileDialog(
            L"Open Radiance HDRI",
            L"Radiance HDR (*.hdr)\0*.hdr\0All Files (*.*)\0*.*\0")) {
      session.loadWorldHdr(*path);
    }
  }
  int hdri_resolution = static_cast<int>(world.hdri_runtime_resolution);
  if (intProperty(ctx, g, tr("sky_hdri_runtime_resolution"), hdri_resolution,
                  256, 8192, 256, busy)) {
    world.hdri_runtime_resolution =
        static_cast<std::uint32_t>(hdri_resolution);
    session.touchWorldEnvironmentTargets();
  }
  if (world.hdr.valid()) {
    const std::string line = std::string(tr("sky_hdri_active")) +
                             world.hdr.source_identity;
    mutedWrap(ctx, g, line.c_str());
    const std::string checksum = std::string(tr("sky_hdri_checksum")) +
                                 world.hdr.checksum.substr(0, 16);
    mutedWrap(ctx, g, checksum.c_str());
  } else {
    mutedWrap(ctx, g, tr("sky_hdri_required"));
  }

  if (world.sky_rendering ==
      gfx::SkyRendering::ProceduralDayNight) {
    const bool celestial_busy = busy;
    heading(ctx, g, tr("sky_time_location"));
    gfx::UtcDateTime local = world.celestial.utc;
    gfx::shiftUtcDateTime(
        world.celestial.utc,
        static_cast<double>(world.time.utc_offset_hours) * 3600.0, local);
    gfx::ObserverLocation observer = world.celestial.observer;
    float sun_azimuth_offset = static_cast<float>(
        world.sun_azimuth_offset_degrees);
    float sun_altitude_offset = static_cast<float>(
        world.sun_altitude_offset_degrees);
    bool celestial_changed = false;
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_year"), local.year, 1900, 2100, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_month"), local.month, 1, 12, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_day"), local.day, 1, 31, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_hour"), local.hour, 0, 23, 1,
                    celestial_busy);
    celestial_changed |=
        intProperty(ctx, g, tr("sky_local_minute"), local.minute, 0, 59, 1,
                    celestial_busy);
    float local_second = static_cast<float>(local.second);
    if (slider(ctx, g, tr("sky_local_second"), local_second, 0.0f, 59.0f,
               celestial_busy, 1.0f)) {
      local.second = local_second;
      celestial_changed = true;
    }
    if (slider(ctx, g, tr("sky_utc_offset"),
               world.time.utc_offset_hours, -14.0f, 14.0f,
               celestial_busy, 0.25f)) {
      session.touchWorldEnvironmentDisplay();
    }
    if (check(ctx, g, tr("sky_time_playing"), world.time.playing,
              celestial_busy)) {
      session.touchWorldEnvironmentCelestial();
    }
    if (slider(ctx, g, tr("sky_time_speed"), world.time.time_speed,
               -86400.0f, 86400.0f, celestial_busy, 60.0f)) {
      session.touchWorldEnvironmentCelestial();
    }
    float observer_latitude = static_cast<float>(observer.latitude_degrees);
    if (slider(ctx, g, tr("sky_observer_latitude"), observer_latitude,
               -90.0f, 90.0f, celestial_busy, 0.1f)) {
      observer.latitude_degrees = observer_latitude;
      celestial_changed = true;
    }
    float observer_longitude = static_cast<float>(observer.longitude_degrees);
    if (slider(ctx, g, tr("sky_observer_longitude"), observer_longitude,
               -180.0f, 180.0f, celestial_busy, 0.1f)) {
      observer.longitude_degrees = observer_longitude;
      celestial_changed = true;
    }
    float observer_elevation = static_cast<float>(observer.elevation_meters);
    if (slider(ctx, g, tr("sky_observer_elevation"), observer_elevation,
               -1000.0f, 100000.0f, celestial_busy, 10.0f)) {
      observer.elevation_meters = observer_elevation;
      celestial_changed = true;
    }
    float north_offset = static_cast<float>(observer.north_offset_degrees);
    if (slider(ctx, g, tr("sky_observer_north_offset"), north_offset,
               -180.0f, 180.0f, celestial_busy, 1.0f)) {
      observer.north_offset_degrees = north_offset;
      celestial_changed = true;
    }
    if (celestial_changed) {
      gfx::UtcDateTime utc;
      std::string shift_error;
      if (gfx::shiftUtcDateTime(
              local,
              -static_cast<double>(world.time.utc_offset_hours) * 3600.0,
              utc, &shift_error)) {
        session.setProceduralSkyControls(
            utc, observer, sun_azimuth_offset, sun_altitude_offset);
      } else {
        session.last_error = shift_error;
        session.status = "World Sky local time rejected";
      }
    }

    heading(ctx, g, tr("sky_sun"));
    auto &sun_controls = world.sun;
    bool sun_recompute = false;
    if (check(ctx, g, tr("sky_enabled"), sun_controls.enabled,
              celestial_busy)) {
      session.touchWorldEnvironment();
    }
    if (slider(ctx, g, tr("sky_relative_strength"), sun_controls.strength,
               0.0f, 32.0f, celestial_busy, 0.05f)) {
      session.touchWorldEnvironment();
    }
    std::vector<const char *> direction_modes{
        tr("sky_direction_automatic"), tr("sky_direction_artistic")};
    int sun_direction = static_cast<int>(sun_controls.direction_mode);
    if (combo(ctx, g, tr("sky_direction"), direction_modes, sun_direction,
              celestial_busy)) {
      sun_controls.direction_mode =
          static_cast<gfx::SkyDirectionMode>(sun_direction);
      sun_recompute = true;
    }
    if (sun_controls.direction_mode ==
        gfx::SkyDirectionMode::ArtisticOffset) {
      sun_recompute |=
          slider(ctx, g, tr("sky_azimuth_offset"), sun_azimuth_offset,
                 -180.0f, 180.0f, celestial_busy, 1.0f);
      sun_recompute |=
          slider(ctx, g, tr("sky_altitude_offset"), sun_altitude_offset,
                 -90.0f, 90.0f, celestial_busy, 1.0f);
    }
    bool sun_physical_changed = false;
    sun_physical_changed |=
        slider(ctx, g, tr("sky_sun_temperature"),
               sun_controls.color_temperature_kelvin, 1000.0f, 40000.0f,
               celestial_busy, 100.0f);
    sun_physical_changed |=
        slider(ctx, g, tr("sky_angular_diameter"),
               sun_controls.angular_diameter_degrees, 0.05f, 5.0f,
               celestial_busy, 0.01f);
    sun_physical_changed |=
        check(ctx, g, tr("sky_cast_shadows"), sun_controls.cast_shadows,
              celestial_busy);
    if (sun_physical_changed) {
      session.touchWorldEnvironment();
    }
    if (check(ctx, g, tr("sky_disk_visible"), sun_controls.disk_visible,
              celestial_busy)) {
      session.touchWorldEnvironmentDisplay();
    }
    if (sun_recompute) {
      session.setProceduralSkyControls(
          world.celestial.utc, world.celestial.observer,
          sun_azimuth_offset, sun_altitude_offset);
    }
    const auto &sun = world.celestial.sun;
    const std::string sun_state =
        std::string(tr("sky_sun_state")) +
        " az=" + std::to_string(sun.azimuth_degrees) +
        " deg, alt=" + std::to_string(sun.apparent_altitude_degrees) +
        " deg, twilight=" +
        gfx::twilightPhaseName(world.celestial.twilight);
    mutedWrap(ctx, g, sun_state.c_str());

    heading(ctx, g, tr("sky_moon"));
    auto &moon_controls = world.moon;
    bool moon_recompute = false;
    bool moon_physical_changed = false;
    moon_physical_changed |=
        check(ctx, g, tr("sky_enabled"), moon_controls.enabled,
              celestial_busy);
    moon_physical_changed |=
        slider(ctx, g, tr("sky_relative_strength"), moon_controls.strength,
               0.0f, 32.0f, celestial_busy, 0.05f);
    if (moon_physical_changed) {
      session.touchWorldEnvironment();
    }
    std::vector<const char *> phase_modes{
        tr("sky_phase_automatic"), tr("sky_phase_manual")};
    int phase_mode = static_cast<int>(moon_controls.phase_mode);
    if (combo(ctx, g, tr("sky_moon_phase"), phase_modes, phase_mode,
              celestial_busy)) {
      moon_controls.phase_mode = static_cast<gfx::MoonPhaseMode>(phase_mode);
      session.touchWorldEnvironment();
    }
    if (moon_controls.phase_mode == gfx::MoonPhaseMode::Manual &&
        slider(ctx, g, tr("sky_moon_fraction"),
               moon_controls.manual_illuminated_fraction, 0.0f, 1.0f,
               celestial_busy, 0.01f)) {
      session.touchWorldEnvironment();
    }
    int moon_direction = static_cast<int>(moon_controls.direction_mode);
    if (combo(ctx, g, tr("sky_direction"), direction_modes, moon_direction,
              celestial_busy)) {
      moon_controls.direction_mode =
          static_cast<gfx::SkyDirectionMode>(moon_direction);
      moon_recompute = true;
    }
    if (moon_controls.direction_mode ==
        gfx::SkyDirectionMode::ArtisticOffset) {
      moon_recompute |=
          slider(ctx, g, tr("sky_azimuth_offset"),
                 moon_controls.azimuth_offset_degrees, -180.0f, 180.0f,
                 celestial_busy, 1.0f);
      moon_recompute |=
          slider(ctx, g, tr("sky_altitude_offset"),
                 moon_controls.altitude_offset_degrees, -90.0f, 90.0f,
                 celestial_busy, 1.0f);
    }
    moon_physical_changed = false;
    moon_physical_changed |=
        slider(ctx, g, tr("sky_angular_diameter"),
               moon_controls.angular_diameter_degrees, 0.05f, 5.0f,
               celestial_busy, 0.01f);
    moon_physical_changed |=
        slider(ctx, g, tr("sky_moon_surface_detail"),
               moon_controls.surface_detail, 0.0f, 1.0f,
               celestial_busy, 0.01f);
    moon_physical_changed |=
        check(ctx, g, tr("sky_cast_shadows"), moon_controls.cast_shadows,
              celestial_busy);
    if (moon_physical_changed) {
      session.touchWorldEnvironment();
    }
    if (check(ctx, g, tr("sky_disk_visible"), moon_controls.disk_visible,
              celestial_busy)) {
      session.touchWorldEnvironmentDisplay();
    }
    if (moon_recompute) {
      session.setProceduralSkyControls(
          world.celestial.utc, world.celestial.observer,
          world.sun_azimuth_offset_degrees,
          world.sun_altitude_offset_degrees);
    }

    heading(ctx, g, tr("sky_atmosphere"));
    auto &physical = world.atmosphere.physical;
    const auto default_atmosphere = gfx::defaultEarthAtmosphereConfig();
    const auto scale_for = [](const std::array<double, 3> &value,
                              const std::array<double, 3> &base) {
      double sum = 0.0;
      int count = 0;
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::isfinite(value[i]) && std::isfinite(base[i]) &&
            std::abs(base[i]) > 1.0e-12) {
          sum += value[i] / base[i];
          ++count;
        }
      }
      return count > 0 ? static_cast<float>(sum / count) : 1.0f;
    };
    const auto scale_array = [](std::array<double, 3> &value,
                                const std::array<double, 3> &base,
                                float scale) {
      for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = base[i] * static_cast<double>(scale);
      }
    };
    auto &atmosphere_controls = world.atmosphere_controls;
    if (slider(ctx, g, tr("sky_atmosphere_sky_strength"),
               atmosphere_controls.sky_relative_strength, 0.0f, 8.0f,
               busy, 0.01f)) {
      session.touchWorldEnvironment();
    }
    if (slider(ctx, g, tr("sky_atmosphere_turbidity"),
               atmosphere_controls.turbidity, 0.0f, 4.0f, busy, 0.01f)) {
      scale_array(physical.mie_scattering_per_km,
                  default_atmosphere.physical.mie_scattering_per_km,
                  atmosphere_controls.turbidity);
      scale_array(physical.mie_extinction_per_km,
                  default_atmosphere.physical.mie_extinction_per_km,
                  atmosphere_controls.turbidity);
      session.touchWorldEnvironment();
    }
    if (slider(ctx, g, tr("sky_atmosphere_ozone"),
               atmosphere_controls.ozone, 0.0f, 4.0f, busy, 0.01f)) {
      scale_array(physical.absorption_extinction_per_km,
                  default_atmosphere.physical.absorption_extinction_per_km,
                  atmosphere_controls.ozone);
      session.touchWorldEnvironment();
    }
    float rayleigh_scale =
        scale_for(physical.rayleigh_scattering_per_km,
                  default_atmosphere.physical.rayleigh_scattering_per_km);
    if (slider(ctx, g, tr("sky_atmosphere_rayleigh"), rayleigh_scale, 0.0f,
               4.0f, busy, 0.01f)) {
      scale_array(physical.rayleigh_scattering_per_km,
                  default_atmosphere.physical.rayleigh_scattering_per_km,
                  rayleigh_scale);
      session.touchWorldEnvironment();
    }
    float mie_g = static_cast<float>(physical.mie_phase_function_g);
    if (slider(ctx, g, tr("sky_atmosphere_mie_g"), mie_g, -0.95f, 0.95f,
               busy, 0.01f)) {
      physical.mie_phase_function_g = mie_g;
      session.touchWorldEnvironment();
    }
    float ground_albedo =
        scale_for(physical.ground_albedo,
                  default_atmosphere.physical.ground_albedo);
    if (slider(ctx, g, tr("sky_atmosphere_ground"), ground_albedo, 0.0f,
               1.0f, busy, 0.01f)) {
      scale_array(physical.ground_albedo,
                  default_atmosphere.physical.ground_albedo, ground_albedo);
      session.touchWorldEnvironment();
    }
    std::vector<const char *> lut_qualities{
        tr("sky_quality_low"), tr("sky_quality_medium"),
        tr("sky_quality_high")};
    int lut_quality = static_cast<int>(atmosphere_controls.lut_quality);
    if (combo(ctx, g, tr("sky_atmosphere_lut_quality"), lut_qualities,
              lut_quality, busy)) {
      atmosphere_controls.lut_quality =
          static_cast<std::uint32_t>(lut_quality);
      world.atmosphere.scattering_orders =
          lut_quality == 0 ? 2u : lut_quality == 1 ? 4u : 6u;
      session.touchWorldEnvironment();
    }
    const std::string lut_status =
        std::string(tr("sky_atmosphere_lut_status")) +
        gfx::brunetonAtmosphereCacheKey(world.atmosphere).substr(0, 16);
    mutedWrap(ctx, g, lut_status.c_str());

    heading(ctx, g, tr("sky_night"));
    auto &night = world.night;
    bool night_changed = false;
    night_changed |=
        check(ctx, g, tr("sky_stars_enabled"), night.stars_enabled, busy);
    night_changed |=
        slider(ctx, g, tr("sky_star_intensity"), night.star_intensity,
               0.0f, 32.0f, busy, 0.05f);
    night_changed |=
        check(ctx, g, tr("sky_milky_way_enabled"),
              night.milky_way_enabled, busy);
    night_changed |=
        slider(ctx, g, tr("sky_milky_way_intensity"),
               night.milky_way_intensity, 0.0f, 32.0f, busy, 0.05f);
    night_changed |=
        slider(ctx, g, tr("sky_light_pollution"), night.light_pollution,
               0.0f, 16.0f, busy, 0.05f);
    night_changed |=
        slider(ctx, g, tr("sky_star_rotation"),
               night.star_rotation_degrees, -180.0f, 180.0f, busy, 1.0f);
    night_changed |=
        slider(ctx, g, tr("sky_night_fill"), night.night_fill,
               0.0f, 4.0f, busy, 0.01f);
    if (night_changed) {
      session.touchWorldEnvironment();
    }

    auto &clouds = world.clouds;
    heading(ctx, g, tr("sky_clouds"));
    if (check(ctx, g, tr("sky_clouds_enabled"), clouds.enabled, busy)) {
      session.touchWorldEnvironment(true);
    }
    const bool cloud_disabled = busy || !clouds.enabled;
    auto cloudSlide = [&](const char *label, float &value, float lo, float hi,
                          float step) {
      if (slider(ctx, g, label, value, lo, hi, cloud_disabled, step)) {
        session.touchWorldEnvironment(true);
      }
    };
    cloudSlide(tr("sky_cloud_coverage"), clouds.coverage, 0.0f, 1.0f, 0.01f);
    cloudSlide(tr("sky_cloud_density"), clouds.density, 0.01f, 8.0f, 0.01f);
    cloudSlide(tr("sky_cloud_base"), clouds.base_altitude_km, 0.1f, 20.0f,
               0.1f);
    cloudSlide(tr("sky_cloud_thickness"), clouds.thickness_km, 0.1f, 20.0f,
               0.1f);
    cloudSlide(tr("sky_cloud_wind_x"), clouds.wind_direction[0], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_wind_z"), clouds.wind_direction[1], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_wind_speed"), clouds.wind_speed_km_per_hour,
               -1000.0f, 1000.0f, 1.0f);
    cloudSlide(tr("sky_cloud_shadow_strength"), clouds.shadow_strength,
               0.0f, 1.0f, 0.01f);
    std::vector<const char *> cloud_qualities{
        tr("sky_quality_low"), tr("sky_quality_medium"),
        tr("sky_quality_high"), tr("sky_quality_still")};
    int cloud_quality = static_cast<int>(clouds.quality);
    if (combo(ctx, g, tr("sky_cloud_quality"), cloud_qualities,
              cloud_quality, cloud_disabled)) {
      clouds.quality = static_cast<gfx::CloudQuality>(cloud_quality);
      if (cloud_quality == 0) {
        clouds.ray_steps = 24u;
        clouds.light_steps = 4u;
        clouds.render_ratio = 0.5f;
      } else if (cloud_quality == 1) {
        clouds.ray_steps = 48u;
        clouds.light_steps = 6u;
        clouds.render_ratio = 1.0f;
      } else if (cloud_quality == 2) {
        clouds.ray_steps = 72u;
        clouds.light_steps = 8u;
        clouds.render_ratio = 1.0f;
      } else {
        clouds.ray_steps = 128u;
        clouds.light_steps = 16u;
        clouds.render_ratio = 1.0f;
      }
      session.touchWorldEnvironmentTargets();
    }
    cloudSlide(tr("sky_cloud_weather_scale"), clouds.weather_scale,
               0.05f, 20.0f, 0.01f);
    cloudSlide(tr("sky_cloud_offset_x"), clouds.weather_offset_km[0], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_offset_z"), clouds.weather_offset_km[1], -100.0f,
               100.0f, 0.1f);
    cloudSlide(tr("sky_cloud_base_shape"), clouds.base_shape_scale,
               0.05f, 20.0f, 0.01f);
    cloudSlide(tr("sky_cloud_detail"), clouds.detail_scale,
               0.05f, 20.0f, 0.01f);
    cloudSlide(tr("sky_cloud_erosion"), clouds.erosion,
               0.0f, 1.0f, 0.01f);
    cloudSlide(tr("sky_cloud_forward_scattering"),
               clouds.forward_scattering, -0.95f, 0.95f, 0.01f);
    cloudSlide(tr("sky_cloud_silver_lining"), clouds.silver_lining,
               0.0f, 4.0f, 0.01f);
    cloudSlide(tr("sky_cloud_absorption"), clouds.absorption,
               0.01f, 8.0f, 0.01f);
    cloudSlide(tr("sky_cloud_multiple_scattering"),
               clouds.multiple_scattering, 0.0f, 2.0f, 0.01f);
    float render_ratio = clouds.render_ratio;
    if (slider(ctx, g, tr("sky_cloud_render_ratio"), render_ratio,
               0.25f, 1.0f, cloud_disabled, 0.05f)) {
      clouds.render_ratio = render_ratio;
      session.touchWorldEnvironmentTargets();
    }
    char cloud_resolution_status[128];
    std::snprintf(
        cloud_resolution_status, sizeof(cloud_resolution_status),
        tr("sky_cloud_resolution_status"),
        static_cast<unsigned int>(
            std::lround(2048.0f * clouds.render_ratio)),
        static_cast<unsigned int>(
            std::lround(1024.0f * clouds.render_ratio)));
    muted(ctx, g, cloud_resolution_status);
    if (check(ctx, g, tr("sky_cloud_reprojection"), clouds.reprojection,
              cloud_disabled)) {
      session.touchWorldEnvironment(true);
    }
    cloudSlide(tr("sky_cloud_history_weight"), clouds.history_weight,
               0.0f, 0.999f, 0.01f);
    cloudSlide(tr("sky_cloud_lighting_strength"),
               clouds.lighting_strength, 0.0f, 8.0f, 0.01f);
    cloudSlide(tr("sky_cloud_time"), clouds.time_seconds, -3600.0f, 3600.0f,
               0.1f);
    int seed = static_cast<int>(std::min<std::uint32_t>(clouds.seed, 1'000'000u));
    if (intProperty(ctx, g, tr("sky_cloud_seed"), seed, 0, 1'000'000, 1,
                    cloud_disabled)) {
      clouds.seed = static_cast<std::uint32_t>(seed);
      session.touchWorldEnvironment(true);
    }
    int ray_steps = static_cast<int>(clouds.ray_steps);
    if (intProperty(ctx, g, tr("sky_cloud_ray_steps"), ray_steps, 8, 128, 1,
                    cloud_disabled)) {
      clouds.ray_steps = static_cast<std::uint32_t>(ray_steps);
      session.touchWorldEnvironment(true);
    }
    int light_steps = static_cast<int>(clouds.light_steps);
    if (intProperty(ctx, g, tr("sky_cloud_light_steps"), light_steps, 1, 16, 1,
                    cloud_disabled)) {
      clouds.light_steps = static_cast<std::uint32_t>(light_steps);
      session.touchWorldEnvironment(true);
    }
    int shadow_resolution = static_cast<int>(clouds.shadow_resolution);
    if (intProperty(ctx, g, tr("sky_cloud_shadow_resolution"),
                    shadow_resolution, 64, 4096, 64, cloud_disabled)) {
      clouds.shadow_resolution =
          static_cast<std::uint32_t>(shadow_resolution);
      session.touchWorldEnvironmentTargets();
    }
  }

  heading(ctx, g, tr("sky_debug"));
  std::vector<const char *> debug_views{
      tr("sky_debug_off"), tr("sky_debug_radiance"),
      tr("sky_debug_environment_pdf"),
      tr("sky_debug_cloud_transmittance")};
  int debug_view = static_cast<int>(world.debug_view);
  if (combo(ctx, g, tr("sky_debug_view"), debug_views, debug_view, busy)) {
    world.debug_view = static_cast<gfx::SkyDebugView>(debug_view);
    session.touchWorldEnvironmentDisplay();
  }
}

void drawBoneContextMenuContent(nk_context *ctx, const Geom &g,
                                AppSession &session, bool busy,
                                float child_list_h) {
  const std::string bone_name = session.bone_context_bone_name;
  const auto selected_it = std::find_if(
      session.geometry.bones.begin(), session.geometry.bones.end(),
      [&](const loader::Bone &bone) { return bone.name == bone_name; });
  if (!session.bone_context_open ||
      selected_it == session.geometry.bones.end()) {
    session.closeBoneContext();
    return;
  }

  const float content_w = nk_window_get_content_region_size(ctx).x;
  nk_layout_row_begin(ctx, NK_STATIC, g.btn + 5.0f, 2);
  nk_layout_row_push(ctx, (std::max)(80.0f, content_w - g.btn - 8.0f));
  nk_label_colored(ctx, tr("bone_context_title"), NK_TEXT_LEFT,
                   nk_rgb(112, 224, 230));
  nk_layout_row_push(ctx, g.btn);
  if (flatSymbolButton(ctx, NK_SYMBOL_X)) {
    session.closeBoneContext();
  }
  nk_layout_row_end(ctx);
  if (!session.bone_context_open) {
    return;
  }

  nk_layout_row_dynamic(ctx, g.row + 3.0f, 1);
  nk_label_colored(ctx, bone_name.c_str(), NK_TEXT_LEFT,
                   nk_rgb(238, 241, 248));
  nk_layout_row_dynamic(ctx, 5.0f, 1);
  struct nk_rect separator{};
  if (nk_widget(&separator, ctx) != NK_WIDGET_INVALID) {
    nk_fill_rect(nk_window_get_canvas(ctx),
                 nk_rect(separator.x, separator.y + 2.0f, separator.w, 1.0f),
                 0.0f, nk_rgb(76, 82, 100));
  }

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

  drawSelectedBoneOverrideEditor(ctx, g, session, busy);

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
    if (contextMenuNavButton(ctx, parent->name.c_str())) {
      session.openBoneContext(parent->name);
      return;
    }
  } else {
    muted(ctx, g, tr("bone_context_no_parent"));
  }

  heading(ctx, g, tr("bone_context_children"));
  nk_layout_row_dynamic(ctx, child_list_h, 1);
  if (nk_group_begin(ctx, "bone_context_popup_children", NK_WINDOW_BORDER)) {
    bool has_children = false;
    for (const auto &bone : session.geometry.bones) {
      if (!bone.has_parent || bone.parent != bone_name ||
          bone.name == bone_name) {
        continue;
      }
      has_children = true;
      nk_layout_row_dynamic(ctx, g.btn, 1);
      if (contextMenuNavButton(ctx, bone.name.c_str())) {
        session.openBoneContext(bone.name);
        break;
      }
    }
    if (!has_children) {
      muted(ctx, g, tr("bone_context_no_children"));
    }
    nk_group_end(ctx);
  }

  nk_layout_row_dynamic(ctx, g.btn, 1);
  if (contextMenuNavButton(ctx, tr("close"))) {
    session.closeBoneContext();
  }
}

void drawBoneContextPopup(nk_context *ctx, const Geom &g, AppSession &session,
                          bool busy, float window_w, float window_h,
                          UiFrameResult &result) {
  if (!session.bone_context_open) {
    return;
  }

  std::size_t child_count = 0;
  for (const auto &bone : session.geometry.bones) {
    if (bone.has_parent && bone.parent == session.bone_context_bone_name &&
        bone.name != session.bone_context_bone_name) {
      ++child_count;
    }
  }

  const float margin = 12.0f * g.s;
  const float gap = 14.0f * g.s;
  const float popup_w = std::clamp(310.0f * g.s, 250.0f, 390.0f);
  const float visible_child_rows =
      static_cast<float>((std::min<std::size_t>)(child_count, 4));
  const float child_list_h =
      child_count == 0
          ? g.label + 14.0f
          : visible_child_rows * (g.btn + ctx->style.window.spacing.y) +
                10.0f;
  const float desired_h = 330.0f * g.s + child_list_h;
  const float popup_h =
      std::clamp(desired_h, 280.0f, (std::max)(280.0f, window_h - margin * 2.0f));

  float popup_x = session.bone_context_anchor_x + gap;
  if (popup_x + popup_w > window_w - margin) {
    popup_x = session.bone_context_anchor_x - popup_w - gap;
  }
  popup_x = std::clamp(popup_x, margin,
                       (std::max)(margin, window_w - popup_w - margin));
  float popup_y = session.bone_context_anchor_y - 10.0f * g.s;
  if (popup_y + popup_h > window_h - margin) {
    popup_y = window_h - popup_h - margin;
  }
  popup_y = std::clamp(popup_y, margin,
                       (std::max)(margin, window_h - popup_h - margin));
  const struct nk_rect popup_bounds =
      nk_rect(popup_x, popup_y, popup_w, popup_h);

  if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
      !nk_input_is_mouse_hovering_rect(&ctx->input, popup_bounds)) {
    session.closeBoneContext();
    return;
  }
  result.layout.overlay_visible = true;
  result.layout.overlay_x = popup_bounds.x;
  result.layout.overlay_y = popup_bounds.y;
  result.layout.overlay_w = popup_bounds.w;
  result.layout.overlay_h = popup_bounds.h;

  const nk_color popup_bg = nk_rgba(37, 40, 49, 252);
  const nk_color popup_border = nk_rgba(78, 85, 104, 235);
  nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                           nk_style_item_color(popup_bg));
  nk_style_push_color(ctx, &ctx->style.window.background, popup_bg);
  nk_style_push_color(ctx, &ctx->style.window.border_color, popup_border);
  nk_style_push_color(ctx, &ctx->style.window.group_border_color,
                      nk_rgba(70, 77, 94, 220));
  nk_style_push_float(ctx, &ctx->style.window.border, 1.0f);
  nk_style_push_float(ctx, &ctx->style.window.rounding, 9.0f * g.s);
  nk_style_push_vec2(ctx, &ctx->style.window.padding,
                     nk_vec2(12.0f * g.s, 10.0f * g.s));

  constexpr const char *kPopupName = "##bone_context_popup";
  nk_window_set_bounds(ctx, kPopupName, popup_bounds);
  if (nk_begin(ctx, kPopupName, popup_bounds, NK_WINDOW_BORDER)) {
    nk_window_set_bounds(ctx, kPopupName, popup_bounds);
    drawBoneContextMenuContent(ctx, g, session, busy, child_list_h);
  }
  nk_end(ctx);

  nk_style_pop_vec2(ctx);
  nk_style_pop_float(ctx);
  nk_style_pop_float(ctx);
  nk_style_pop_color(ctx);
  nk_style_pop_color(ctx);
  nk_style_pop_color(ctx);
  nk_style_pop_style_item(ctx);
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
    nk_fill_rect(nk_window_get_canvas(ctx), bar, 1.0f,
                 active    ? nk_rgb(106, 222, 228)
                 : hovered ? nk_rgb(49, 192, 200)
                           : nk_rgb(63, 68, 84));
  }
  return hovered || active;
}

}

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
            if (treeRowButton(ctx, name.c_str(), sel, false) && !busy &&
                !sel) {
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
        nk_layout_row_dynamic(ctx, g.btn, 4);
        if (treeRowButton(ctx, tr("properties_physics"),
                          ui_state.properties_page == 0, false)) {
          ui_state.properties_page = 0;
        }
        if (treeRowButton(ctx, tr("properties_renderer"),
                          ui_state.properties_page == 1, false)) {
          ui_state.properties_page = 1;
        }
        if (treeRowButton(ctx, tr("properties_sky"),
                          ui_state.properties_page == 2, false)) {
          ui_state.properties_page = 2;
        }
        if (treeRowButton(ctx, tr("properties_labpbr"),
                          ui_state.properties_page == 3, false)) {
          ui_state.properties_page = 3;
        }
        if (ui_state.properties_page == 3) {
          drawLabPbrEditor(ctx, g, session);
        } else if (ui_state.properties_page == 2) {
          drawSkyEditor(ctx, g, session);
        } else if (ui_state.properties_page == 1) {
          drawRendererEditor(ctx, g, session, stats, rt_cap);
        } else {
        heading(ctx, g, tr("global_physics"));
        muted(ctx, g,
              session.solver_mode == 0 ? tr("solver_xpbd")
                                       : tr("solver_bullet"));
        nk_layout_row_dynamic(ctx, g.btn, 2);
        const auto solver_mode_button = [&](const char *label, int mode) {
          const bool active_mode = session.solver_mode == mode;
          if (active_mode) {
            nk_style_push_style_item(
                ctx, &ctx->style.button.normal,
                nk_style_item_color(nk_rgba(27, 140, 148, 255)));
            nk_style_push_color(ctx, &ctx->style.button.text_normal,
                                nk_rgb(255, 255, 255));
          }
          if (nk_button_label(ctx, label) && !busy) {
            session.setSolverMode(mode);
          }
          if (active_mode) {
            nk_style_pop_color(ctx);
            nk_style_pop_style_item(ctx);
          }
        };
        solver_mode_button(tr("xpbd"), 0);
        solver_mode_button(tr("bullet"), 1);

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
        if (session.output_timeline_mode == 1 &&
            session.selected_animation != nullptr) {
          const auto &animation = *session.selected_animation;
          const int source_snapping_fps =
              baker::OutputTimelineResampler::detectSourceSnappingFps(
                  animation);
          char source_snapping_text[256];
          if (source_snapping_fps > 0) {
            // Bedrock 循环输出也保留 animation_length 处的闭合关键帧，
            // 所以严格吸附模式显示的数量始终按闭区间计算。
            const std::size_t frame_count =
                baker::OutputTimelineResampler::snappedFrameCount(
                    animation.animation_length, source_snapping_fps,
                    baker::OutputEndpointPolicy::Closed);
            std::snprintf(source_snapping_text,
                          sizeof(source_snapping_text),
                          tr("source_snapping_detected"),
                          source_snapping_fps,
                          1.0 / static_cast<double>(source_snapping_fps),
                          frame_count);
          } else {
            std::snprintf(source_snapping_text,
                          sizeof(source_snapping_text), "%s",
                          tr("source_snapping_undetected"));
          }
          mutedWrap(ctx, g, source_snapping_text);
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

        drawSelectedBoneOverrideEditor(ctx, g, session, busy);

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
        heading(ctx, g, tr("preview_scene"));
        mutedWrap(ctx, g, tr("preview_scene_hint"));
        constexpr int kSceneSelectionChoiceCount = 10;
        const char *scene_labels[kSceneSelectionChoiceCount] = {
            tr("scene_empty"),          tr("scene_user_built"),
            tr("scene_loaded"),         tr("preview_scene_studio"),
            tr("preview_scene_sky"),    tr("preview_scene_night"),
            tr("preview_scene_sunset"), tr("preview_scene_desert"),
            tr("preview_scene_ocean"),  tr("preview_scene_overcast")};
        int scene_idx = 0;
        switch (session.scene_selection.kind) {
        case SceneSelectionKind::UserBuilt:
          scene_idx = 1;
          break;
        case SceneSelectionKind::Loaded:
          scene_idx = 2;
          break;
        case SceneSelectionKind::Preset:
          scene_idx =
              2 + xpbd::gfx::previewSceneChoiceIndex(
                        session.scene_selection.preset);
          break;
        case SceneSelectionKind::Empty:
        default:
          scene_idx = 0;
          break;
        }
        nk_layout_row_dynamic(ctx, g.btn, 1);
        const int selected_scene_idx =
            nk_combo(ctx, scene_labels, kSceneSelectionChoiceCount, scene_idx,
                     static_cast<int>(g.btn),
                     nk_vec2(nk_widget_width(ctx), g.btn * 10.0f));
        if (selected_scene_idx != scene_idx) {
          if (selected_scene_idx == 0) {
            session.selectScene(SceneSelectionKind::Empty);
          } else if (selected_scene_idx == 1) {
            session.selectScene(SceneSelectionKind::UserBuilt);
          } else if (selected_scene_idx == 2) {
            session.selectScene(SceneSelectionKind::Loaded);
          } else {
            session.selectPresetScene(
                xpbd::gfx::previewSceneIdFromChoiceIndex(
                    selected_scene_idx - 2));
          }
        }
        nk_layout_row_dynamic(ctx, g.btn, 2);
        if (nk_button_label(ctx, tr("scene_save_settings"))) {
          if (const auto path =
                  saveFileDialog(L"Save Scene Selection",
                                 L"JSON File (*.json)\0*.json\0All Files "
                                 L"(*.*)\0*.*\0",
                                 L"scene_selection.json")) {
            session.saveSceneSelectionSettings(*path);
          }
        }
        if (nk_button_label(ctx, tr("scene_load_settings"))) {
          if (const auto path =
                  openFileDialog(L"Load Scene Selection",
                                 L"JSON File (*.json)\0*.json\0All Files "
                                 L"(*.*)\0*.*\0")) {
            session.loadSceneSelectionSettings(*path);
          }
        }
        check(ctx, g, tr("show_preview_grid"), session.show_preview_grid,
              false);
        check(ctx, g, tr("show_preview_axes"), session.show_preview_axes,
              false);
        mutedWrap(ctx, g, tr("preview_scene_independent"));
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
                          session.debug_static_model_vertex_bytes);
          formatByteCount(static_index_bytes, sizeof(static_index_bytes),
                          session.debug_static_model_index_bytes);
          formatByteCount(rt_allocated_bytes, sizeof(rt_allocated_bytes),
                          stats.rt_allocated_bytes);
          formatByteCount(rt_as_storage_bytes, sizeof(rt_as_storage_bytes),
                          stats.rt_as_storage_bytes);
          formatByteCount(rt_scratch_bytes, sizeof(rt_scratch_bytes),
                          stats.rt_scratch_bytes);

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
