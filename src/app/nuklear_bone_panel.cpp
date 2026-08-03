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



void drawBonePanel(UiPanelContext &ui, float panel_width, float panel_height,
                   UiPersistentState &state) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;
  const bool busy = ui.busy;
  const float use_left = panel_width;
  const float mid_row_h = panel_height;
  UiPersistentState &ui_state = state;

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

}

} // namespace xpbd::app::ui_internal
