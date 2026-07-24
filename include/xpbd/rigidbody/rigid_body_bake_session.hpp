#pragma once

#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/final_pose_reconstructor.hpp"
#include "xpbd/baker/periodic_state_adapter.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/rigidbody/rigid_body_backend.hpp"
#include "xpbd/rigidbody/rigid_body_types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace xpbd::rigidbody {

// Bullet 刚体烘焙会话：构建碰撞体、推进仿真并采集骨骼输出。
class RigidBodyBakeSession {
public:
  static constexpr const char *kGroundBodyName = "__ground__";

  struct BoneOutput {
    std::string bone_name;

    std::array<double, 3> position{0, 0, 0};

    std::array<double, 3> rotation{0, 0, 0};

    std::array<double, 3> linear_velocity{0, 0, 0};

    std::array<double, 3> world_position{0, 0, 0};
  };

  using PoseSampler =
      std::function<std::map<std::string, baker::BonePoseCalculator::Pose>(
          double)>;
  using PoseMap = std::map<std::string, baker::BonePoseCalculator::Pose>;
  using PoseAtFraction = std::function<const PoseMap &(double fraction)>;






  using MotionEventFractionProvider = std::function<std::vector<double>(
      double start_sample_time, double end_sample_time, bool continuous_history,
      double forcing_period)>;

  static std::unique_ptr<RigidBodyBakeSession>
  create(baker::BoneMapper &bone_mapper,
         const loader::Animation *source_animation,
         const std::map<std::string, baker::BonePoseCalculator::Pose>
             &initial_poses);

  ~RigidBodyBakeSession();



  void setPoseSampler(PoseSampler sampler);

  void setPoseSampler(PoseSampler sampler,
                      MotionEventFractionProvider event_fraction_provider);
  void advance(double start_sample_time, double end_sample_time,
               double output_dt, bool continuous_history,
               double forcing_period = 0.0,
               const std::map<std::string, baker::BonePoseCalculator::Pose>
                   *endpoint_poses = nullptr);

  [[nodiscard]] std::vector<BoneOutput> captureBoneOutputs(
      const std::map<std::string, baker::BonePoseCalculator::Pose>
          &reference_poses) const;


  void captureBoneOutputsInto(
      const std::map<std::string, baker::BonePoseCalculator::Pose>
          &reference_poses,
      std::vector<BoneOutput> &outputs) const;

  [[nodiscard]] int getPhysicsBodyCount() const {
    return static_cast<int>(physics_bodies_.size());
  }
  [[nodiscard]] int getCollisionBodyCount() const {
    return collision_body_count_;
  }
  [[nodiscard]] int getSourceCubeCount() const { return source_cube_count_; }
  [[nodiscard]] int getSkippedBodyBoneCount() const {
    return skipped_body_bone_count_;
  }
  [[nodiscard]] int getCurrentContactCount() const {
    return current_contact_count_;
  }
  [[nodiscard]] int getMaximumContactCount() const {
    return maximum_contact_count_;
  }
  [[nodiscard]] int getLastEffectiveSubsteps() const {
    return last_effective_substeps_;
  }
  [[nodiscard]] double getMaximumPenetration() const {
    return maximum_penetration_;
  }
  [[nodiscard]] const CollisionDetectionSnapshot &initialCollision() const {
    return initial_collision_;
  }
  [[nodiscard]] const AdaptiveSubstepStats &adaptiveSubstepStats() const {
    return fixed_substep_stats_;
  }
  [[nodiscard]] const FixedSubstepStats &fixedSubstepStats() const {
    return fixed_substep_stats_;
  }
  [[nodiscard]] const KinematicHistoryStats &kinematicHistoryStats() const {
    return kinematic_history_stats_;
  }
  [[nodiscard]] const std::vector<JointConstructionSnapshot> &
  jointConstructionSnapshots() const {
    return joint_construction_snapshots_;
  }
  [[nodiscard]] const JointPreflightDiagnostics &jointPreflightDiagnostics()
      const {
    return joint_preflight_diagnostics_;
  }
  [[nodiscard]] const ColliderPreflightDiagnostics &
  colliderPreflightDiagnostics() const {
    return collider_preflight_diagnostics_;
  }
  [[nodiscard]] const JointSpringDiagnostics &jointSpringDiagnostics() const {
    return joint_spring_diagnostics_;
  }
  [[nodiscard]] int getUnsafeCollisionCount() const;
  [[nodiscard]] baker::PeriodicStateAdapter::Snapshot capturePeriodicSnapshot();
  [[nodiscard]] int getNativeBulletVersion() const;
  [[nodiscard]] RigidBodyRuntimeFingerprint runtimeFingerprint() const;
  [[nodiscard]] const RigidBodyStepTrace &stepTrace() const;
  [[nodiscard]] const std::map<std::string, BodyDefinition> &
  physicsBodyDefinitions() const {
    return physics_body_definitions_;
  }


  void
  validateCompoundDescendantAnimation(const loader::Animation *animation,
                                      const std::string &animation_role) const;


  void validateJointBridgeAnimation(
      const loader::Animation *animation,
      const std::string &animation_role) const;


  void validateCompoundDescendantTransition(
      const loader::Animation *target_animation, double target_sample_time);
  [[nodiscard]] int getSkippedDegenerateCubeCount() const {
    return skipped_degenerate_cube_count_;
  }

private:
  struct DynamicRuntimeSlot {
    std::string bone_name;
    BodyHandle handle;
    double mass = 0.0;
    double gravity_scale = 1.0;
    double wind_influence = 1.0;
    double turbulence_amplitude = 0.0;
    double turbulence_phase = 0.0;
    double pull_compliance = 0.0;
    double pull_stiffness = 0.0;
    double pull_damping = 0.0;
    Transform bone_to_com{};
    bool has_bone_to_com = false;
    double minimum_half_extent = 0.0;
    double maximum_radius = 0.0;
    bool has_motion_geometry = false;
  };

  struct KinematicRuntimeSlot {
    std::string bone_name;
    BodyHandle handle;
    Transform bone_to_com{};
    Transform previous_transform{};
    bool has_bone_to_com = false;
    bool has_previous_transform = false;
    double minimum_half_extent = 0.0;
    double maximum_radius = 0.0;
    bool has_motion_geometry = false;
  };

  struct CaptureRuntimeSlot {
    std::string bone_name;
    BodyHandle handle;
    baker::FinalPoseReconstructor::WorldTarget *physics_target = nullptr;
    mutable const baker::FinalPoseReconstructor::LocalChannels
        *local_channels = nullptr;
    mutable const baker::BonePoseCalculator::Pose *world_pose = nullptr;
  };

  struct TraceBodySlot {
    std::string name;
    BodyHandle handle;
    MotionType motion_type = MotionType::Dynamic;
    bool physics_body = false;
  };

  RigidBodyBakeSession(
      baker::BoneMapper &bone_mapper, const loader::Animation *source_animation,
      const std::map<std::string, baker::BonePoseCalculator::Pose>
          &initial_poses,
      std::unique_ptr<RigidBodyBackend> backend);

  void initialize(const std::map<std::string, baker::BonePoseCalculator::Pose>
                      &initial_poses);
  void compileRuntimeSlots();
  void buildJoints(const std::map<std::string, baker::BonePoseCalculator::Pose>
                       &initial_poses);
  void validateJointConfiguration();
  void appendColliderDiagnostics(
      const std::vector<ColliderDiagnostic> &diagnostics);
  void driveKinematicBodies(
      const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
      double fixed_dt, KinematicHistoryMode history_mode);
  void applyDynamicForces(
      const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
      double sample_time, double forcing_period);
  [[nodiscard]] std::array<double, 3> evaluateAdditionalDynamicForce(
      const DynamicRuntimeSlot &slot, const BodyState &state,
      const PoseMap &poses, double sample_time, double forcing_period) const;
  void analyzeFixedStepRisk(double start_sample_time, double end_sample_time,
                            double output_dt, bool continuous_history,
                            double forcing_period,
                            const PoseAtFraction &pose_at_fraction);
  [[nodiscard]] std::vector<double>
  motionProbeFractions(double start_sample_time, double end_sample_time,
                       bool continuous_history, double forcing_period) const;
  void appendAnimationEventFractions(std::set<double> &fractions,
                                     const loader::Animation &animation,
                                     double sample_origin,
                                     double start_parameter,
                                     double end_parameter) const;
  [[nodiscard]] std::set<std::string> kinematicPoseDrivers() const;
  [[nodiscard]] std::string
  nearestPhysicsAncestor(const std::string &bone_name) const;
  [[nodiscard]] int hierarchyDepth(const std::string &bone_name) const;
  [[nodiscard]] const loader::Bone &requireBone(const std::string &name) const;
  [[nodiscard]] baker::BonePoseCalculator::Pose requirePose(
      const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
      const std::string &name) const;
  void captureStepTraceSample(const PoseMap &poses,
                              std::uint64_t output_step_index,
                              int substep_index, double sample_time,
                              double physics_dt, double substep_dt);
  void normalizeStepTraceOrder() const;

  baker::BoneMapper &bone_mapper_;
  baker::BoneMapper::PhysicsGroupConfig config_;
  SnapshotLevel snapshot_level_ = SnapshotLevel::ContactsOnly;
  std::unique_ptr<RigidBodyBackend> backend_;
  baker::FinalPoseReconstructor::Evaluator
      final_pose_reconstructor_evaluator_;
  mutable baker::FinalPoseReconstructor::Evaluator::ReconstructionScratch
      final_pose_reconstruction_scratch_;
  mutable std::map<std::string,
                   baker::FinalPoseReconstructor::WorldTarget>
      capture_physics_targets_;
  std::vector<CaptureRuntimeSlot> capture_runtime_slots_;
  std::vector<std::size_t> capture_body_query_slot_indices_;
  mutable std::vector<BodyState> capture_body_states_;
  std::map<std::string, loader::Bone> bones_by_name_;
  std::map<std::string, std::vector<std::string>> children_by_bone_;
  std::map<std::string, BodyHandle> physics_bodies_;
  std::map<std::string, BodyDefinition> physics_body_definitions_;
  std::vector<JointConstructionSnapshot> joint_construction_snapshots_;
  JointPreflightDiagnostics joint_preflight_diagnostics_{};
  ColliderPreflightDiagnostics collider_preflight_diagnostics_{};
  JointSpringDiagnostics joint_spring_diagnostics_{};
  std::map<std::string, Transform> bone_to_com_transforms_;
  std::map<std::string, BodyHandle> kinematic_bodies_;
  std::map<std::string, BodyDefinition> kinematic_body_definitions_;
  std::map<std::string, std::set<std::string>>
      compound_descendant_dependencies_;
  std::map<std::string, std::map<std::string, Transform>>
      compound_descendant_reference_transforms_;


  mutable std::map<std::string, Transform>
      joint_bridge_reference_transforms_;
  std::map<std::string,
           std::map<std::string, std::array<double, 9>>>
      geometry_source_reference_linear_;
  std::vector<std::string> ordered_physics_bones_;
  std::vector<DynamicRuntimeSlot> dynamic_runtime_slots_;
  std::vector<std::size_t> dynamic_risk_slot_indices_;
  std::vector<KinematicRuntimeSlot> kinematic_runtime_slots_;
  std::array<double, 3> air_velocity_{};
  double base_gravity_acceleration_ = 0.0;
  double unit_scale_ = 1.0 / 16.0;
  int substeps_ = 2;
  int last_effective_substeps_ = 2;
  PoseSampler pose_sampler_;
  MotionEventFractionProvider motion_event_fraction_provider_;
  const loader::Animation *source_animation_ = nullptr;
  const loader::Animation *transition_target_animation_ = nullptr;
  double transition_target_entry_time_ = 0.0;
  bool custom_pose_sampler_ = false;
  int collision_body_count_ = 0;
  int source_cube_count_ = 0;
  int skipped_degenerate_cube_count_ = 0;
  int skipped_body_bone_count_ = 0;
  int current_contact_count_ = 0;
  int maximum_contact_count_ = 0;
  double current_step_maximum_penetration_ = 0.0;
  double periodic_interval_maximum_penetration_ = 0.0;
  double periodic_interval_maximum_penetration_time_ = -1.0;
  double periodic_sample_time_ = 0.0;
  double maximum_penetration_ = 0.0;
  CollisionDetectionSnapshot initial_collision_{};
  FixedSubstepStats fixed_substep_stats_{};
  KinematicHistoryStats kinematic_history_stats_{};
  std::uint64_t executed_substep_sum_ = 0;
  double last_output_dt_ = 0.0;
  double last_substep_dt_ = 0.0;
  mutable RigidBodyStepTrace step_trace_{};
  bool closed_ = false;
  std::vector<TraceBodySlot> trace_body_slots_;
  mutable std::size_t step_trace_write_index_ = 0;
  RigidBodyStepTraceSample trace_sample_scratch_{};
};

}
