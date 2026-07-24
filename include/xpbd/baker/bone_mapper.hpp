#pragma once

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/core/simd_dispatch.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/rigidbody/rigid_body_types.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xpbd::baker {

class BoneMapper {
public:
  enum class SimulationMode { Xpbd, RigidBody };
  enum class LoopMode { Auto, ForceLoop, ForceOnce };
  enum class LoopSeamStrategy { PhysicsRelative, VisualSubtree };
  enum class OutputTimelineMode { BakeFps, SourceKeyframeGrid };

  struct PhysicsGroupConfig {
    SimulationMode simulation_mode = SimulationMode::Xpbd;
    double particle_mass = 1.0;
    double compliance = 0.000001;
    double damping_compliance = 0.00001;
    bool enable_angle_constraints = true;
    double max_bend_degrees = 75.0;
    double bend_compliance = 0.00001;
    double gravity_y = -9.8;
    bool enable_real_gravity_field = false;
    bool enable_ground_collision = false;
    int solver_iterations = 8;
    core::SimdMode simd_mode = core::SimdMode::Auto;
    double animation_pull_compliance = 0.1;


    bool allow_input_only_molang_zero_fallback = false;


    bool allow_selected_molang_zero_fallback = false;
    double collision_skin = 0.1;
    double xpbd_collision_restitution = 0.0;
    double wind_speed = 6.0;
    double wind_direction_degrees = 20.0;
    double wind_elevation_degrees = 20.0;
    bool use_wind_components = false;
    double wind_x = 0.0;
    double wind_y = 0.0;
    double wind_z = 0.0;
    double movement_speed = 0.0;
    double movement_direction_degrees = 0.0;
    double movement_elevation_degrees = 0.0;
    double air_drag = 2.0;
    double turbulence = 1.5;
    double transition_duration = 0.25;
    OutputTimelineMode output_timeline_mode = OutputTimelineMode::BakeFps;
    LoopMode loop_mode = LoopMode::Auto;
    int minimum_warmup_cycles = 2;
    int maximum_warmup_cycles = 12;
    int required_stable_cycles = 2;
    double loop_position_tolerance = 0.001;
    double loop_rotation_tolerance_degrees = 0.1;
    double loop_linear_velocity_tolerance = 0.01;
    double loop_angular_velocity_tolerance = 0.01;
    bool loop_seam_fallback_enabled = true;
    LoopSeamStrategy loop_seam_strategy = LoopSeamStrategy::PhysicsRelative;

    double loop_seam_window_ratio = 0.25;
    bool loop_seam_match_acceleration = true;
    double loop_seam_relative_velocity_tolerance = 0.02;
    double loop_seam_minimum_linear_velocity_tolerance = 0.01;
    double loop_seam_minimum_angular_velocity_tolerance = 0.01;
    double loop_seam_relative_acceleration_tolerance = 0.05;
    int rigid_body_substeps = 2;
    double rigid_body_unit_scale = 1.0 / 16.0;
    double rigid_body_linear_damping = 0.02;
    double rigid_body_angular_damping = 0.05;
    double rigid_body_joint_stiffness = 12.0;
    double rigid_body_joint_damping = 0.8;
    double rigid_body_max_bend_x_degrees = 75.0;
    double rigid_body_max_bend_y_degrees = 75.0;
    double rigid_body_max_bend_z_degrees = 75.0;
    double rigid_body_friction = 0.5;
    double rigid_body_restitution = 0.0;
    bool rigid_body_ccd = true;
    double rigid_body_maximum_safe_penetration = 0.2;
    rigidbody::SnapshotLevel rigid_body_snapshot_level =
        rigidbody::SnapshotLevel::ContactsOnly;

    bool rigid_body_step_trace_enabled = false;
    int rigid_body_step_trace_capacity = 256;
  };

  struct BonePhysicsConfig {
    std::optional<double> particle_mass;
    std::optional<double> compliance;
    std::optional<double> damping_compliance;
    std::optional<double> max_bend_degrees;
    std::optional<double> bend_compliance;
    std::optional<double> rigid_body_max_bend_x_degrees;
    std::optional<double> rigid_body_max_bend_y_degrees;
    std::optional<double> rigid_body_max_bend_z_degrees;
    std::optional<double> animation_pull_compliance;
    std::optional<double> gravity_scale;
    std::optional<double> wind_influence;
    std::optional<double> turbulence_influence;
    std::optional<bool> fixed;
  };

  struct ConstraintDef {
    std::string bone_a;
    std::string bone_b;
    double rest_length = 0.0;
    double compliance = 0.0;
    double damping_compliance = 0.0;
  };

  struct CrossSpringDef {
    std::string bone_a;
    std::string bone_c;
    double min_distance = 0.0;
    double max_distance = 0.0;
    double compliance = 0.0;
    double fallback_x = 1.0;
    double fallback_y = 0.0;
    double fallback_z = 0.0;
  };

  struct AngleConstraintDef {
    std::string bone_a;
    std::string bone_b;
    std::string bone_c;
    double min_angle_radians = 0.0;
    double max_angle_radians = 3.14159265358979323846;
    double compliance = 0.0;
    double fallback_normal_x = 0.0;
    double fallback_normal_y = 0.0;
    double fallback_normal_z = 1.0;
  };





  static constexpr double kAnimationFollowMinimumEnabledStrength = 1e-6;
  static constexpr double kAnimationFollowMinimumCompliance = 1e-6;
  static constexpr double kAnimationFollowMaximumCompliance = 1.0;

  [[nodiscard]] static double
  animationFollowStrengthToCompliance(double strength) noexcept;
  [[nodiscard]] static double
  animationFollowComplianceToStrength(double compliance) noexcept;

  explicit BoneMapper(std::vector<loader::Bone> bones = {});

  void replaceModelBones(std::vector<loader::Bone> bones);
  [[nodiscard]] const std::vector<loader::Bone> &allBones() const {
    return all_bones_;
  }

  void addPhysicsBone(const std::string &bone_name);
  void removePhysicsBone(const std::string &bone_name);
  [[nodiscard]] bool isPhysicsBone(const std::string &bone_name) const;
  [[nodiscard]] const std::vector<std::string> &physicsBones() const {
    return physics_bones_order_;
  }

  void addCollisionRoot(const std::string &bone_name);
  void removeCollisionRoot(const std::string &bone_name);
  void clearCollisionRoots();
  [[nodiscard]] bool isCollisionRoot(const std::string &bone_name) const;
  [[nodiscard]] const std::set<std::string> &collisionRoots() const {
    return collision_roots_;
  }
  [[nodiscard]] std::set<std::string> getExpandedCollisionBones() const;

  [[nodiscard]] std::set<std::string> animationInputDependencyBones() const;

  void resetModelState();
  [[nodiscard]] PhysicsGroupConfig &config() { return config_; }
  [[nodiscard]] const PhysicsGroupConfig &config() const { return config_; }

  [[nodiscard]] std::array<double, 3>
  getWorldPivot(const std::string &bone_name) const;
  void buildParticleMapping();
  [[nodiscard]] int getParticleIndex(const std::string &bone_name) const;
  [[nodiscard]] std::size_t
  getModelBoneIndex(const std::string &bone_name) const;

  [[nodiscard]] std::vector<ConstraintDef> generateChainConstraints() const;
  [[nodiscard]] std::vector<CrossSpringDef>
  generateCrossSpringConstraints() const;
  [[nodiscard]] std::vector<AngleConstraintDef>
  generateAngleConstraints() const;

  [[nodiscard]] BonePhysicsConfig *getBoneConfig(const std::string &bone_name);
  [[nodiscard]] const BonePhysicsConfig *
  getBoneConfig(const std::string &bone_name) const;
  void setBoneConfig(const std::string &bone_name,
                     const BonePhysicsConfig *cfg);

  [[nodiscard]] double getEffectiveMass(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveCompliance(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveDampingCompliance(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveMaxBendDegrees(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveBendCompliance(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveAnimPullCompliance(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveGravityScale(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveWindInfluence(const std::string &bone_name) const;
  [[nodiscard]] double
  getEffectiveTurbulenceInfluence(const std::string &bone_name) const;
  [[nodiscard]] std::array<double, 3>
  getEffectiveRigidBodyMaxBendDegrees(const std::string &bone_name) const;
  [[nodiscard]] bool isFixedBone(const std::string &bone_name) const;

private:
  std::vector<loader::Bone> all_bones_;
  std::map<std::string, loader::Bone> bones_by_name_;
  std::map<std::string, std::size_t> bone_to_model_index_;
  std::map<std::string, BonePoseCalculator::Pose> rest_poses_;
  std::vector<std::string> physics_bones_order_;
  std::set<std::string> physics_bones_;
  std::set<std::string> collision_roots_;
  std::map<std::string, int> bone_to_particle_;
  std::map<std::string, BonePhysicsConfig> per_bone_configs_;
  PhysicsGroupConfig config_;

  void clearModelState();
  void refreshModelCache();
  void canonicalizePhysicsBoneOrder();
  [[nodiscard]] int hierarchyDepth(const std::string &bone_name) const;
  [[nodiscard]] bool
  hasPhysicsAncestorOrSelf(const std::string &bone_name) const;
  static double
  effectiveNonNegative(const std::optional<double> &override_value,
                       double fallback, double default_value);
  static double spanForAngle(double length_a, double length_b, double angle);
};

}
