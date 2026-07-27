#pragma once

#include "xpbd/baker/bake_profiler.hpp"
#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/body_collider_cache.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/final_pose_reconstructor.hpp"
#include "xpbd/baker/loop_bake_config.hpp"
#include "xpbd/baker/loop_bake_controller.hpp"
#include "xpbd/baker/loop_error_report.hpp"
#include "xpbd/baker/loop_seam_report.hpp"
#include "xpbd/baker/rigid_body_collision_auditor.hpp"
#include "xpbd/baker/rigid_body_joint_auditor.hpp"
#include "xpbd/baker/transition_bake_controller.hpp"
#include "xpbd/baker/transition_bake_request.hpp"
#include "xpbd/baker/xpbd_periodic_state_tracker.hpp"
#include "xpbd/constraints/ground_collision_constraint.hpp"
#include "xpbd/constraints/target_constraint.hpp"
#include "xpbd/constraints/vertex_face_collision_constraint.hpp"
#include "xpbd/core/xpbd_engine.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/models/particle.hpp"
#include "xpbd/rigidbody/rigid_body_bake_session.hpp"

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace xpbd::baker {

class BakeCancelled final : public std::runtime_error {
public:
  BakeCancelled() : std::runtime_error("bake cancelled") {}
};





// 烘焙协调器：将动画目标、物理求解、循环修正及导出安全审计串成完整流程。
class PhysicsBaker {
public:
  static constexpr int kMaxBakeSteps = 36000;

  struct LoopSeamWindowDiagnostics {
    double window_duration_seconds = 0.0;
    double window_ratio = 0.0;
    int window_start_index = 0;
    double window_start_time = 0.0;
    bool corrected = false;
    bool valid = false;
    bool c0_pass = false;
    bool c1_pass = false;
    bool c2_pass = false;
    bool driver_pass = false;
    bool driver_c0_pass = false;
    bool driver_c1_pass = false;
    bool driver_c2_pass = false;
    bool validation_pass = false;
    bool physics_seam_pass = false;
    bool driver_seam_pass = false;
    bool quantization_pass = false;
    bool collision_pass = false;
    bool joint_pass = false;
    bool export_pass = false;
    bool collision_safe = false;
    bool joint_safe = false;
    bool accepted = false;
    bool best_preview = false;
    bool best_safe_export = false;
    bool selected_for_output = false;
    double score = std::numeric_limits<double>::infinity();
    double maximum_penetration = 0.0;
    double maximum_penetration_time = -1.0;
    double joint_failure_time = -1.0;
    double interpolation_failure_time = -1.0;
    int interpolated_sample_count = 0;
    int canonicalized_bone_count = 0;
    int preserved_driver_bone_count = 0;
    int driver_endpoint_conflict_count = 0;
    std::string first_invalid_bone;
    std::string first_invalid_field;
    std::vector<std::string> rejection_reasons;
  };

  explicit PhysicsBaker(BoneMapper &bone_mapper);

  void setSourceAnimation(const loader::Animation *anim);
  void setTransitionAnimation(const loader::Animation *anim);
  void setTransitionRequest(const TransitionBakeRequest *request);
  void setDt(double dt);
  void setProfiler(BakeProfiler profiler);
  void setCancellationCheck(std::function<bool()> check);
  void setAuditPhaseCallback(std::function<void()> callback);

  void initialize();
  void step();
  void runToEnd();
  void runSteps(int n);
  void finalizeFrames();
  void reset();
  void close();

  [[nodiscard]] const std::vector<BakedFrame> &frames() const {
    return frames_;
  }
  [[nodiscard]] std::vector<BakedFrame> takeFinalizedFrames();

  [[nodiscard]] BakedFrame captureCurrentFrameForPreview();
  [[nodiscard]] int currentStep() const { return current_step_; }
  [[nodiscard]] int totalSteps() const { return total_steps_; }
  [[nodiscard]] double currentSampleTime() const {
    return current_sample_time_;
  }
  [[nodiscard]] double cycleDt() const { return cycle_dt_; }
  [[nodiscard]] double outputFrameInterval() const {
    return output_frame_interval_;
  }
  [[nodiscard]] bool isFramesFinalized() const { return frames_finalized_; }
  [[nodiscard]] bool isLoopConverged() const { return loop_converged_; }
  [[nodiscard]] bool isLoopFallbackUsed() const { return loop_fallback_used_; }
  [[nodiscard]] int getCompletedLoopCycles() const {
    return completed_loop_cycles_;
  }
  [[nodiscard]] int getUnsafeFinalCollisionCount() const {
    return unsafe_final_collision_count_;
  }
  [[nodiscard]] int getUnsafeFinalJointCount() const {
    return unsafe_final_joint_count_;
  }
  [[nodiscard]] double getMaximumFinalRigidBodyPenetration() const {
    return maximum_final_rigid_body_penetration_;
  }
  [[nodiscard]] const std::optional<std::pair<std::string, std::string>> &
  getWorstFinalCollisionPair() const {
    return worst_final_collision_pair_;
  }
  [[nodiscard]] double getMaximumFinalJointAnchorSeparation() const {
    return maximum_final_joint_anchor_separation_;
  }
  [[nodiscard]] double getMaximumFinalJointAngularExcessRadians() const {
    return maximum_final_joint_angular_excess_radians_;
  }
  [[nodiscard]] const std::string &getWorstFinalJointLinearParent() const {
    return worst_final_joint_linear_parent_;
  }
  [[nodiscard]] const std::string &getWorstFinalJointLinearChild() const {
    return worst_final_joint_linear_child_;
  }
  [[nodiscard]] const std::string &getWorstFinalJointAngularParent() const {
    return worst_final_joint_angular_parent_;
  }
  [[nodiscard]] const std::string &getWorstFinalJointAngularChild() const {
    return worst_final_joint_angular_child_;
  }
  [[nodiscard]] int getWorstFinalJointAngularAxis() const {
    return worst_final_joint_angular_axis_;
  }
  [[nodiscard]] const std::optional<RigidBodyJointAuditor::Violation> &
  getFirstFinalJointEulerSingularity() const {
    return first_final_joint_euler_singularity_;
  }
  [[nodiscard]] std::optional<int> getBestLoopCycleIndex() const {
    if (best_loop_safe_export_cycle_candidate_) {
      return best_loop_safe_export_cycle_candidate_->cycle_index;
    }
    return best_loop_preview_cycle_candidate_
               ? std::optional<int>(
                     best_loop_preview_cycle_candidate_->cycle_index)
               : std::nullopt;
  }
  [[nodiscard]] std::optional<double> getBestLoopCycleScore() const {
    if (best_loop_safe_export_cycle_candidate_) {
      return best_loop_safe_export_cycle_candidate_->normalized_score;
    }
    return best_loop_preview_cycle_candidate_
               ? std::optional<double>(
                     best_loop_preview_cycle_candidate_->normalized_score)
               : std::nullopt;
  }
  [[nodiscard]] std::optional<int> getBestLoopPreviewCycleIndex() const {
    return best_loop_preview_cycle_candidate_
               ? std::optional<int>(
                     best_loop_preview_cycle_candidate_->cycle_index)
               : std::nullopt;
  }
  [[nodiscard]] std::optional<int> getBestLoopSafeExportCycleIndex() const {
    return best_loop_safe_export_cycle_candidate_
               ? std::optional<int>(
                     best_loop_safe_export_cycle_candidate_->cycle_index)
               : std::nullopt;
  }
  [[nodiscard]] bool isLoopPreviewOnlyCycleUsed() const {
    return loop_preview_only_cycle_used_;
  }
  [[nodiscard]] const LoopErrorReport *getLoopErrorReport() const {
    return loop_error_report_ ? &*loop_error_report_ : nullptr;
  }
  [[nodiscard]] const std::vector<LoopCycleCandidate> &
  getLoopCycleCandidates() const {
    return loop_cycle_candidates_;
  }
  [[nodiscard]] std::optional<rigidbody::CollisionDetectionSnapshot>
  getInitialCollisionSnapshot() const;
  [[nodiscard]] std::optional<rigidbody::FixedSubstepStats>
  getFixedSubstepStats() const;
  [[nodiscard]] std::optional<rigidbody::AdaptiveSubstepStats>
  getAdaptiveSubstepStats() const {
    return getFixedSubstepStats();
  }
  [[nodiscard]] std::optional<rigidbody::KinematicHistoryStats>
  getKinematicHistoryStats() const;
  [[nodiscard]] std::optional<rigidbody::JointPreflightDiagnostics>
  getJointPreflightDiagnostics() const;
  [[nodiscard]] std::optional<rigidbody::ColliderPreflightDiagnostics>
  getColliderPreflightDiagnostics() const;
  [[nodiscard]] std::optional<rigidbody::JointSpringDiagnostics>
  getJointSpringDiagnostics() const;
  [[nodiscard]] int getCurrentRigidBodyContactCount() const;
  [[nodiscard]] int getMaximumRigidBodyContactCount() const;
  [[nodiscard]] double getMaximumRuntimeRigidBodyPenetration() const;
  [[nodiscard]] int getNativeBulletVersion() const;
  [[nodiscard]] std::optional<rigidbody::RigidBodyRuntimeFingerprint>
  getRigidBodyRuntimeFingerprint() const;
  [[nodiscard]] std::optional<rigidbody::RigidBodyStepTrace>
  getRigidBodyStepTrace() const;
  [[nodiscard]] loader::Animation::LoopBehavior getOutputLoopBehavior() const;
  [[nodiscard]] bool isLooping() const {
    return getOutputLoopBehavior() == loader::Animation::LoopBehavior::Loop;
  }
  [[nodiscard]] bool isTransitionBake() const {
    return active_transition_.has_value();
  }
  [[nodiscard]] bool isLoopSeamCorrectionRejected() const {
    return loop_seam_correction_rejected_;
  }
  [[nodiscard]] const LoopSeamReport *getLoopSeamReport() const {
    return loop_seam_report_ ? &*loop_seam_report_ : nullptr;
  }
  [[nodiscard]] const LoopSeamReport *getBestLoopSeamCandidateReport() const {
    return getBestLoopPreviewCandidateReport();
  }
  [[nodiscard]] const LoopSeamReport *
  getBestLoopPreviewCandidateReport() const {
    return best_loop_preview_candidate_report_
               ? &*best_loop_preview_candidate_report_
               : nullptr;
  }
  [[nodiscard]] const LoopSeamReport *
  getBestLoopSafeExportCandidateReport() const {
    return best_loop_safe_export_candidate_report_
               ? &*best_loop_safe_export_candidate_report_
               : nullptr;
  }
  [[nodiscard]] const std::vector<LoopSeamWindowDiagnostics> &
  getLoopSeamWindowDiagnostics() const {
    return loop_seam_window_diagnostics_;
  }
  [[nodiscard]] const loader::Animation *getOutputReferenceAnimation() const;
  [[nodiscard]] double getOutputReferenceTime(double output_time) const;
  [[nodiscard]] std::map<std::string, BonePoseCalculator::Pose>
  sampleOutputReferencePoses(double output_time) const;
  [[nodiscard]] std::set<std::string> getOutputReferenceDependencyBones() const;
  void requireTransitionReferenceExportable() const;

  void requireSafeForExport() const;

  static models::Vector3 windVector(double speed, double direction_degrees,
                                    double elevation_degrees);
  static models::Vector3
  relativeAirVelocity(const BoneMapper::PhysicsGroupConfig &cfg);
  static models::Vector3
  environmentWindVelocity(const BoneMapper::PhysicsGroupConfig &cfg);

private:
  class XpbdPeriodicAdapter;
  class RigidBodyPeriodicAdapter;

  struct CandidateAudit {
    bool valid = true;
    bool collision_safe = true;
    bool joint_safe = true;
    bool safe = true;
    double maximum_penetration = 0.0;
    double maximum_penetration_time = -1.0;
    double joint_failure_time = -1.0;
    double interpolation_failure_time = -1.0;
    int missing_bone_count = 0;
    int non_finite_value_count = 0;
    int interpolated_sample_count = 0;
    std::string first_invalid_bone;
    std::string first_invalid_field;
    std::optional<std::pair<std::string, std::string>> worst_collision_pair;
  };

  BoneMapper &bone_mapper_;
  core::XpbdEngine engine_;
  BakeProfiler profiler_ = BakeProfiler::disabled();
  std::optional<BakeProfiler::ScopedStage> total_bake_scope_;

  std::vector<std::unique_ptr<models::Particle>> owned_particles_;
  std::vector<std::unique_ptr<constraints::Constraint>> owned_constraints_;
  std::vector<constraints::TargetConstraint *> animation_targets_;
  std::vector<models::Particle *> collision_particles_;
  constraints::GroundCollisionConstraint *ground_collision_constraint_ =
      nullptr;
  constraints::VertexFaceCollisionConstraint *body_collision_constraint_ =
      nullptr;
  std::unique_ptr<BodyColliderCache> body_collider_cache_;
  std::unique_ptr<rigidbody::RigidBodyBakeSession> rigid_body_session_;
  std::vector<rigidbody::RigidBodyBakeSession::BoneOutput>
      rigid_body_output_scratch_;
  std::unique_ptr<RigidBodyCollisionAuditor> rigid_body_collision_auditor_;
  std::unique_ptr<RigidBodyJointAuditor> rigid_body_joint_auditor_;

  std::vector<BakedFrame> frames_;
  std::vector<BakedFrame> loop_cycle_frames_;
  std::vector<BakedFrame> best_loop_preview_cycle_frames_;
  std::vector<BakedFrame> best_loop_safe_export_cycle_frames_;
  std::map<std::string, loader::Bone> bones_by_name_;
  std::map<std::string, std::vector<std::string>> physics_children_by_bone_;
  std::vector<std::string> ordered_physics_bone_cache_;
  std::optional<BonePoseCalculator::Evaluator> pose_evaluator_;
  std::optional<BonePoseCalculator::Evaluator::AnimationBinding>
      source_pose_binding_;
  BonePoseCalculator::Evaluator::EvaluationScratch
      current_reference_pose_scratch_;
  std::optional<FinalPoseReconstructor::Evaluator>
      final_pose_reconstructor_evaluator_;
  FinalPoseReconstructor::Evaluator::ReconstructionScratch
      final_pose_reconstruction_scratch_;

  const loader::Animation *source_animation_ = nullptr;
  const loader::Animation *transition_animation_ = nullptr;
  const TransitionBakeRequest *requested_transition_ = nullptr;
  std::optional<TransitionBakeRequest> active_transition_;
  std::unique_ptr<TransitionBakeController> transition_controller_;
  int transition_pre_roll_steps_ = 0;
  int transition_steps_ = 0;
  double transition_pre_roll_dt_ = 0.0;
  double transition_dt_ = 0.0;
  bool transition_started_ = false;

  double dt_ = 1.0 / 60.0;
  double cycle_dt_ = dt_;
  double output_frame_interval_ = dt_;
  int total_steps_ = 0;
  int current_step_ = 0;
  double current_sample_time_ = 0.0;
  bool frames_finalized_ = false;
  bool initialized_ = false;
  bool compiled_pose_evaluator_enabled_ = true;
  std::function<bool()> cancellation_check_;
  std::function<void()> audit_phase_callback_;

  std::unique_ptr<LoopBakeController> loop_controller_;
  XpbdPeriodicStateTracker xpbd_periodic_state_tracker_;
  std::optional<LoopBakeConfig> loop_bake_config_;
  std::optional<LoopErrorReport> loop_error_report_;
  std::optional<LoopCycleCandidate> best_loop_preview_cycle_candidate_;
  std::optional<LoopCycleCandidate> best_loop_safe_export_cycle_candidate_;
  std::vector<LoopCycleCandidate> loop_cycle_candidates_;
  bool loop_preview_only_cycle_used_ = false;
  bool loop_converged_ = false;
  bool loop_fallback_used_ = false;
  bool loop_seam_correction_rejected_ = false;
  int completed_loop_cycles_ = 0;
  int unsafe_final_collision_count_ = 0;
  int unsafe_final_joint_count_ = 0;
  double maximum_final_rigid_body_penetration_ = 0.0;
  std::optional<std::pair<std::string, std::string>>
      worst_final_collision_pair_;
  double maximum_final_joint_anchor_separation_ = 0.0;
  double maximum_final_joint_angular_excess_radians_ = 0.0;
  std::string worst_final_joint_linear_parent_;
  std::string worst_final_joint_linear_child_;
  std::string worst_final_joint_angular_parent_;
  std::string worst_final_joint_angular_child_;
  int worst_final_joint_angular_axis_ = -1;
  std::optional<RigidBodyJointAuditor::Violation>
      first_final_joint_euler_singularity_;
  std::optional<LoopSeamReport> loop_seam_report_;
  std::optional<LoopSeamReport> best_loop_preview_candidate_report_;
  std::optional<LoopSeamReport> best_loop_safe_export_candidate_report_;
  std::vector<LoopSeamWindowDiagnostics> loop_seam_window_diagnostics_;

  void configureTransition();
  void configureLoopTiming();
  [[nodiscard]] int calculateTotalSteps() const;
  [[nodiscard]] bool hasPeriodicLoop() const;
  [[nodiscard]] int getCycleSteps() const;
  void rebuildStructureCaches();
  [[nodiscard]] int hierarchyDepth(const std::string &bone_name) const;
  [[nodiscard]] std::set<std::string> animationInputDependencyBones() const;
  void validateAnimationInputTransforms(
      const loader::Animation *animation, const std::string &animation_role,
      const std::set<std::string> &dependency_bones) const;
  void validateAnimationInputs() const;
  void stepTransition();
  void beginTransition(const BakedFrame *previous_physical_state,
                       double physical_frame_span);
  void advanceSolvers(double end_time, double step_dt, bool continuous_history,
                      double forcing_period);
  void finalizeLoopSeam();
  [[nodiscard]] std::set<std::string> fixedBones() const;
  [[nodiscard]] LoopSeamReport
  measureLoopSeam(const std::vector<BakedFrame> &candidate_frames,
                  bool corrected, double window_duration_seconds,
                  double window_ratio,
                  const CandidateAudit &audit) const;
  [[nodiscard]] CandidateAudit
  auditLoopCandidate(const std::vector<BakedFrame> &candidate_frames) const;
  [[nodiscard]] static std::vector<double>
  correctionWindowRatios(double requested_ratio);

  [[nodiscard]] std::map<std::string, BonePoseCalculator::Pose>
  calculatePoses(const loader::Animation *animation, double time) const;
  [[nodiscard]] std::map<std::string, BonePoseCalculator::Pose> calculatePoses(
      const loader::Animation *animation, double time,
      const std::map<std::string, std::array<double, 3>> *position_overrides,
      const std::map<std::string, std::array<double, 3>> *rotation_overrides)
      const;
  void calculateCurrentSourcePoses(double time);
  [[nodiscard]] const BonePoseCalculator::Evaluator::PoseMap &
  currentReferencePoses() const;

  void
  updateFixedBones(const std::map<std::string, BonePoseCalculator::Pose> &poses,
                   double step_dt, bool continuous_history);
  void updateAnimationTargets(
      const std::map<std::string, BonePoseCalculator::Pose> &poses);
  void recordFrame(
      const std::map<std::string, BonePoseCalculator::Pose> &reference_poses);
  void recordAdvancedState(bool cycle_boundary);
  [[nodiscard]] BakedFrame createXpbdFrame(
      const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
      double frame_time,
      XpbdPeriodicStateTracker::WorldRotations *final_world_rotations =
          nullptr);
  [[nodiscard]] BakedFrame createRigidBodyFrame(
      const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
      double frame_time);
  [[nodiscard]] double currentFrameTime() const;

  void unwrapFinalRotations();
  void resampleOutputTimeline();
  void normalizePeriodicOutputTimeline();
  void blendTransitionFramesToMovingTarget();
  void blendOrdinaryFramesToReferenceEdges();
  void rebuildFinalWorldPositionsAndAudit();
  void recomputeFinalLinearVelocities();

  [[nodiscard]] static BakedFrame copyFrameAtTime(const BakedFrame &frame,
                                                  double time);
  [[nodiscard]] static std::vector<BakedFrame>
  copyFrames(const std::vector<BakedFrame> &source);
  [[nodiscard]] static double smootherStep(double value);
  static models::Vector3 finiteVector(const models::Vector3 &v);
  void checkCancellation() const;
};

}
