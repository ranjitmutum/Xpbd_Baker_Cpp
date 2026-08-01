#include "nuklear_ui_internal.hpp"

#include "xpbd/app/i18n.hpp"
#include "xpbd/baker/output_timeline_resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace xpbd::app::ui_internal {

void drawBakePanel(UiPanelContext &ui) {
  nk_context *ctx = ui.nk;
  AppSession &session = ui.session;
  const Geom &g = ui.geom;
  const bool busy = ui.busy;

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


}

} // namespace xpbd::app::ui_internal
