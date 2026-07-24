#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace xpbd::rigidbody {

enum class SnapshotLevel { None, ContactsOnly, FullDiagnostics };

[[nodiscard]] inline const char *snapshotLevelName(SnapshotLevel level) {
  switch (level) {
  case SnapshotLevel::None:
    return "None";
  case SnapshotLevel::ContactsOnly:
    return "ContactsOnly";
  case SnapshotLevel::FullDiagnostics:
    return "FullDiagnostics";
  }
  return "Unknown";
}

[[nodiscard]] inline constexpr SnapshotLevel
resolveSnapshotLevel(SnapshotLevel configured, bool legacy_trace_enabled) {
  return legacy_trace_enabled ? SnapshotLevel::FullDiagnostics : configured;
}

enum class MotionType { Static, Kinematic, Dynamic };

[[nodiscard]] inline const char *motionTypeName(MotionType type) {
  switch (type) {
  case MotionType::Static:
    return "Static";
  case MotionType::Kinematic:
    return "Kinematic";
  case MotionType::Dynamic:
    return "Dynamic";
  }
  return "Unknown";
}
enum class KinematicHistoryMode {
  Continuous,



  PeriodicWrapResetVelocity,
  TeleportResetVelocity
};

enum class KinematicDiscontinuityReason {
  None,
  ExplicitHistoryReset,
  ContinuousAnimationTeleport,
  CustomSamplerTeleport,
  TransitionTeleport,
  SampleTimeRegression,
  PeriodicWrap
};


struct BulletSafetyThresholds {
  double maximum_xyz_y_limit_degrees = 89.0;
  double near_half_turn_warning_degrees = 175.0;




  double degenerate_minimum_half_extent = 1e-6;
  double minimum_half_extent_warning = 1e-3;
  double minimum_half_extent_error = 1e-5;
  double aspect_ratio_warning = 1e3;
  double aspect_ratio_error = 1e4;
  double box_collision_margin_maximum = 0.04;
  double box_collision_margin_fraction = 0.2;
  double margin_to_minimum_extent_warning_minimum = 0.01;
  double margin_to_minimum_extent_warning_maximum = 0.5;
};

inline constexpr BulletSafetyThresholds kBulletSafetyThresholds{};

[[nodiscard]] inline double
bulletBoxCollisionMargin(double minimum_half_extent) {
  return std::min(kBulletSafetyThresholds.box_collision_margin_maximum,
                  minimum_half_extent *
                      kBulletSafetyThresholds.box_collision_margin_fraction);
}

[[nodiscard]] inline const char *
kinematicDiscontinuityReasonName(KinematicDiscontinuityReason reason) {
  switch (reason) {
  case KinematicDiscontinuityReason::None:
    return "None";
  case KinematicDiscontinuityReason::ExplicitHistoryReset:
    return "ExplicitHistoryReset";
  case KinematicDiscontinuityReason::ContinuousAnimationTeleport:
    return "ContinuousAnimationTeleport";
  case KinematicDiscontinuityReason::CustomSamplerTeleport:
    return "CustomSamplerTeleport";
  case KinematicDiscontinuityReason::TransitionTeleport:
    return "TransitionTeleport";
  case KinematicDiscontinuityReason::SampleTimeRegression:
    return "SampleTimeRegression";
  case KinematicDiscontinuityReason::PeriodicWrap:
    return "PeriodicWrap";
  }
  return "Unknown";
}

struct Transform {

  std::array<double, 3> translation{0, 0, 0};

  std::array<double, 4> rotation{0, 0, 0, 1};

  static Transform identity() { return {}; }

  void normalizeRotation() {
    double lengthSquared = 0;
    for (double c : rotation) {
      lengthSquared += c * c;
    }
    if (!(lengthSquared > 1e-20)) {
      throw std::invalid_argument("rotation must be invertible");
    }
    const double inv = 1.0 / std::sqrt(lengthSquared);
    for (double &c : rotation) {
      c *= inv;
    }
  }
};

struct BoxShape {
  std::array<double, 3> half_extents{0.1, 0.1, 0.1};

  Transform local_transform{};
};

struct CcdSettings {
  bool enabled = false;
  double motion_threshold = 0.0;
  double swept_sphere_radius = 0.0;

  static CcdSettings disabled() { return {}; }
};

struct BodyDefinition {
  std::string name;
  MotionType motion_type = MotionType::Dynamic;
  std::vector<BoxShape> boxes;

  Transform initial_bone_transform{};
  double mass = 1.0;
  double friction = 0.5;
  double restitution = 0.0;
  double linear_damping = 0.02;
  double angular_damping = 0.05;
  CcdSettings ccd{};
};

enum class ColliderRiskLevel { Safe, Warning, Error };

[[nodiscard]] inline const char *
colliderRiskLevelName(ColliderRiskLevel level) {
  switch (level) {
  case ColliderRiskLevel::Safe:
    return "Safe";
  case ColliderRiskLevel::Warning:
    return "Warning";
  case ColliderRiskLevel::Error:
    return "Error";
  }
  return "Unknown";
}

struct ColliderDiagnostic {
  std::string body_name;
  int box_index = 0;
  std::array<double, 3> bullet_half_extents{0, 0, 0};
  double unit_scale = 0.0;
  double minimum_half_extent = 0.0;
  double maximum_half_extent = 0.0;
  double aspect_ratio = 0.0;
  double collision_margin = 0.0;
  double margin_to_minimum_half_extent = 0.0;
  bool ccd_enabled = false;
  double ccd_radius = 0.0;
  ColliderRiskLevel risk = ColliderRiskLevel::Safe;
  std::vector<std::string> issues;
};

struct ColliderPreflightDiagnostics {
  double unit_scale = 0.0;
  double minimum_half_extent_warning =
      kBulletSafetyThresholds.minimum_half_extent_warning;
  double minimum_half_extent_error =
      kBulletSafetyThresholds.minimum_half_extent_error;
  double aspect_ratio_warning = kBulletSafetyThresholds.aspect_ratio_warning;
  double aspect_ratio_error = kBulletSafetyThresholds.aspect_ratio_error;
  double margin_ratio_warning_minimum =
      kBulletSafetyThresholds.margin_to_minimum_extent_warning_minimum;
  double margin_ratio_warning_maximum =
      kBulletSafetyThresholds.margin_to_minimum_extent_warning_maximum;
  int warning_count = 0;
  int error_count = 0;
  double observed_minimum_half_extent = 0.0;
  double observed_maximum_half_extent = 0.0;
  double observed_maximum_aspect_ratio = 0.0;
  std::vector<ColliderDiagnostic> colliders;
};

struct BodyHandle {
  int id = 0;
  std::string name;
};

struct BodyState {

  Transform bone_transform{};

  Transform com_transform{};

  std::array<double, 3> bone_linear_velocity{0, 0, 0};

  std::array<double, 3> com_linear_velocity{0, 0, 0};

  std::array<double, 3> angular_velocity{0, 0, 0};
};

struct JointSettings {

  std::array<double, 3> angular_lower_limit{-1, -1, -1};
  std::array<double, 3> angular_upper_limit{1, 1, 1};
  std::array<bool, 3> angular_spring_enabled{true, true, true};
  double stiffness = 0.0;
  double damping = 0.0;
};





struct JointConstructionSnapshot {
  std::string parent_body;
  std::string child_body;
  Transform world_anchor{};
  JointSettings settings{};
};

struct JointErrorSnapshot {
  std::string parent_body;
  std::string child_body;

  double anchor_separation = 0.0;

  std::array<double, 3> relative_angles{0, 0, 0};

  std::array<double, 3> angular_excess{0, 0, 0};
  double maximum_angular_excess = 0.0;
};

struct JointLimitWarning {
  std::string parent_body;
  std::string child_body;
  std::string axis;
  double limit_degrees = 0.0;
};

struct JointPreflightDiagnostics {
  std::string rotation_order = "XYZ";
  double maximum_safe_y_limit_degrees =
      kBulletSafetyThresholds.maximum_xyz_y_limit_degrees;
  double near_half_turn_warning_degrees =
      kBulletSafetyThresholds.near_half_turn_warning_degrees;
  std::vector<JointLimitWarning> warnings;
};

struct JointSpringDiagnostics {
  double requested_stiffness = 0.0;
  double requested_damping = 0.0;
  int constructed_joint_count = 0;
  int active_spring_joint_count = 0;
  int active_spring_axis_count = 0;
  int solver_iterations = 0;
  int configured_fixed_substeps = 0;

  int configured_minimum_substeps = 0;
  bool bullet_stability_limiting_enabled = true;
  std::string solver_constraint = "btGeneric6DofSpring2Constraint";
  std::string effective_behavior =
      "Solver-dependent: requested values are passed with Bullet "
      "limitIfNeeded=true and are not calibrated physical constants";
};

struct SweepResult {
  bool hit = false;
  double hit_fraction = 1.0;
  std::string hit_body_name;

  static SweepResult miss() { return {}; }
};

struct ContactSnapshot {
  BodyHandle body_a;
  BodyHandle body_b;
  double penetration = 0.0;
  double combined_friction = 0.0;
  double combined_restitution = 0.0;
};

struct CollisionDetectionSnapshot {
  int contact_count = 0;
  double maximum_penetration = 0.0;
  std::optional<std::pair<BodyHandle, BodyHandle>> worst_pair;
  int ground_contacts = 0;
  int self_contacts = 0;
  double warning_threshold = 0.0;
  double failure_threshold = 0.0;
  bool warning = false;
  bool unsafe = false;
};



inline constexpr int kAdaptiveSubstepHardLimit = 4096;

struct FixedSubstepStats {
  int configured_minimum = 0;
  int last_effective = 0;
  int maximum_effective = 0;
  double average_effective = 0.0;
  std::uint64_t output_step_count = 0;
  std::uint64_t insufficient_step_risk_count = 0;


  std::uint64_t raised_step_count = 0;
  std::optional<int> worst_frame;
  std::string worst_body;
  std::string worst_motion_source;
  int required_before_failure = 0;
  int hard_limit = kAdaptiveSubstepHardLimit;
  int last_kinematic_required = 0;
  int last_dynamic_required = 0;
  int maximum_dynamic_required = 0;
  int maximum_recommended = 0;
  double worst_minimum_half_extent = 0.0;
  double worst_maximum_radius = 0.0;
  double worst_equivalent_linear_travel = 0.0;
  double worst_equivalent_angular_travel = 0.0;
  double worst_acceleration = 0.0;
};

using AdaptiveSubstepStats = FixedSubstepStats;

struct KinematicHistoryStats {
  std::uint64_t continuous_updates = 0;
  std::uint64_t periodic_updates = 0;
  std::uint64_t teleport_resets = 0;
  std::uint64_t detected_discontinuities = 0;
  std::uint64_t rejected_discontinuities = 0;
  std::uint64_t insufficient_step_risks = 0;
  std::string last_teleport_body;
  std::string last_step_risk_body;
  KinematicDiscontinuityReason last_reason = KinematicDiscontinuityReason::None;
  double last_pivot_delta = 0.0;
  double last_com_delta = 0.0;
  double last_angular_delta_radians = 0.0;
  double last_equivalent_travel = 0.0;
  double last_detection_threshold = 0.0;
  double last_fixed_dt = 0.0;
};

struct RuntimeBodyTopology {
  std::string name;
  MotionType motion_type = MotionType::Dynamic;
  bool physics_body = false;
  bool compound_shape = false;
  std::vector<BoxShape> boxes;
};


struct RigidBodyRuntimeFingerprint {
  std::string schema_version = "cpp-java-bullet-runtime-v1";
  std::string stable_name_hash_version =
      "java-utf16-string-hashcode-v1";
  double bake_fps = 0.0;
  int fixed_substeps = 0;
  double physics_dt = 0.0;
  double substep_dt = 0.0;
  SnapshotLevel snapshot_level = SnapshotLevel::ContactsOnly;
  int bullet_version = 0;
  int solver_thread_count = 1;
  int solver_iterations = 0;
  bool fast_math_enabled = false;
  std::string floating_point_mode = "fast-math-disabled";
  double unit_scale = 0.0;
  double linear_damping = 0.0;
  double angular_damping = 0.0;
  double gravity_y = 0.0;
  bool real_gravity_field = false;
  bool ground_collision_enabled = false;
  double wind_speed = 0.0;
  double wind_direction_degrees = 0.0;
  double wind_elevation_degrees = 0.0;
  bool use_wind_components = false;
  std::array<double, 3> wind_components{0, 0, 0};
  double movement_speed = 0.0;
  double movement_direction_degrees = 0.0;
  double movement_elevation_degrees = 0.0;
  double air_drag = 0.0;
  double turbulence = 0.0;
  std::vector<RuntimeBodyTopology> shape_topology;
};

struct RigidBodyTraceBody {
  std::string name;
  MotionType motion_type = MotionType::Dynamic;
  bool physics_body = false;

  Transform driver_bone_transform{};
  BodyState simulated{};
};

struct RigidBodyStepTraceSample {
  std::uint64_t output_step_index = 0;
  int substep_index = 0;
  double sample_time = 0.0;
  double physics_dt = 0.0;
  double substep_dt = 0.0;
  std::vector<RigidBodyTraceBody> bodies;
  CollisionDetectionSnapshot contact_summary{};
  std::vector<ContactSnapshot> contacts;
  bool contacts_truncated = false;
  std::vector<JointErrorSnapshot> joint_errors;
};


struct RigidBodyStepTrace {
  SnapshotLevel snapshot_level = SnapshotLevel::ContactsOnly;
  bool enabled = false;
  int capacity = 0;
  std::uint64_t captured_sample_count = 0;
  std::uint64_t dropped_sample_count = 0;
  std::vector<RigidBodyStepTraceSample> samples;
};

}
