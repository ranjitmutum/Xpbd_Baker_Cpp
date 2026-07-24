#include "xpbd/baker/physics_baker.hpp"

#include "xpbd/baker/final_pose_reconstructor.hpp"
#include "xpbd/baker/loop_seam_corrector.hpp"
#include "xpbd/baker/output_timeline_resampler.hpp"
#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/constraints/angle_constraint.hpp"
#include "xpbd/constraints/distance_constraint.hpp"
#include "xpbd/constraints/weld_constraint.hpp"
#include "xpbd/export/animation_exporter.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>

namespace xpbd::baker {

namespace {

void addRigidAuditCounters(
    const BakeProfiler &profiler,
    const RigidBodyCollisionAuditor::AuditResult &audit) {
  if (!profiler.isEnabled()) {
    return;
  }
  profiler.addCounter(
      BakeProfiler::Counter::RigidAuditPossiblePairs,
      static_cast<std::int64_t>(audit.counters.all_possible_pairs));
  profiler.addCounter(
      BakeProfiler::Counter::RigidAuditBroadPhaseCandidates,
      static_cast<std::int64_t>(audit.counters.broad_phase_candidates));
  profiler.addCounter(BakeProfiler::Counter::RigidAuditSatCalls,
                      static_cast<std::int64_t>(audit.counters.sat_calls));
}

double canonicalLoopSampleTime(const loader::Animation &animation,
                               double time) {
  if (!animation.loop || !(animation.animation_length > 0.0)) {
    return std::clamp(time, 0.0, animation.animation_length);
  }
  double wrapped = std::fmod(time, animation.animation_length);
  if (wrapped < 0.0) {
    wrapped += animation.animation_length;
  }
  return wrapped;
}

bool scaleNear(const std::array<double, 3> &left,
               const std::array<double, 3> &right) {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (std::abs(left[axis] - right[axis]) > 1e-9) {
      return false;
    }
  }
  return true;
}

bool scaleSignsMatch(const std::array<double, 3> &left,
                     const std::array<double, 3> &right) {
  for (std::size_t axis = 0; axis < left.size(); ++axis) {
    if ((left[axis] < 0.0) != (right[axis] < 0.0)) {
      return false;
    }
  }
  return true;
}

bool channelChanges(const loader::Keyframes &channel) {
  std::array<double, 3> baseline{};
  bool has_baseline = false;
  const auto inspect = [&](const auto &values) {
    for (const auto &[time, value] : values) {
      (void)time;
      if (!has_baseline) {
        baseline = value;
        has_baseline = true;
      } else if (!scaleNear(baseline, value)) {
        return true;
      }
    }
    return false;
  };
  return inspect(channel.keyframes) || inspect(channel.pre_keyframes);
}

std::array<double, 3>
validatedScale(const loader::Animation *animation,
               const std::string &bone_name,
               const std::string &animation_role, bool require_identity,
               bool require_constant = true) {
  const std::array<double, 3> identity{1.0, 1.0, 1.0};
  if (animation == nullptr) {
    return identity;
  }
  const auto bone_animation = animation->bones.find(bone_name);
  if (bone_animation == animation->bones.end()) {
    return identity;
  }
  const auto &channels = bone_animation->second;
  const auto &scale = channels.scale;
  if (scale.containsMolang()) {
    throw std::invalid_argument(
        animation_role + " bone '" + bone_name +
        "' scale contains Molang, so " +
        (require_identity ? "identity" : "a constant non-zero value") +
        " cannot be proven; physics pose sampling does not support Molang "
        "scale");
  }
  const bool authored = channels.has_scale || !scale.keyframes.empty() ||
                        !scale.pre_keyframes.empty();
  if (!authored) {
    return identity;
  }

  std::array<double, 3> baseline{};
  std::array<int, 3> baseline_sign{};
  bool has_baseline = false;
  const auto inspect = [&](const auto &values, const char *keyframe_role) {
    for (const auto &[time, value] : values) {
      for (std::size_t axis = 0; axis < value.size(); ++axis) {
        const double component = value[axis];
        if (!std::isfinite(component) || !(std::abs(component) > 1e-12)) {
          throw std::invalid_argument(
              animation_role + " bone '" + bone_name + "' scale " +
              keyframe_role + " keyframe at " + std::to_string(time) +
              " must contain finite non-zero values; degenerate scale is "
              "unsupported");
        }
        const int sign = component < 0.0 ? -1 : 1;
        if (has_baseline && sign != baseline_sign[axis]) {
          throw std::invalid_argument(
              animation_role + " bone '" + bone_name + "' scale " +
              keyframe_role + " keyframe at " + std::to_string(time) +
              " changes sign and would cross zero; dynamic reflection is "
              "unsupported");
        }
      }
      if (!has_baseline) {
        baseline = value;
        for (std::size_t axis = 0; axis < value.size(); ++axis) {
          baseline_sign[axis] = value[axis] < 0.0 ? -1 : 1;
        }
        has_baseline = true;
      }
      if (require_identity && !scaleNear(identity, value)) {
        throw std::invalid_argument(
            animation_role + " bone '" + bone_name + "' scale " +
            keyframe_role + " keyframe at " + std::to_string(time) +
            " is not identity [1,1,1]; XPBD pose sampling does not support "
            "scale");
      }
      if (require_constant && !scaleNear(baseline, value)) {
        throw std::invalid_argument(
            animation_role + " bone '" + bone_name + "' scale " +
            keyframe_role + " keyframe at " + std::to_string(time) +
            " changes over time; dynamic scale is unsupported");
      }
    }
  };
  inspect(scale.keyframes, "post");
  inspect(scale.pre_keyframes, "pre");
  if (!has_baseline) {
    throw std::invalid_argument(animation_role + " bone '" + bone_name +
                                "' scale channel has no numeric keyframes");
  }
  return baseline;
}

bool hasOrthogonalNonDegenerateBasis(
    const BonePoseCalculator::Pose &pose) {
  std::array<std::array<double, 3>, 3> columns{};
  std::array<double, 3> lengths{};
  for (std::size_t column = 0; column < 3; ++column) {
    for (std::size_t row = 0; row < 3; ++row) {
      columns[column][row] = pose.world_linear[row * 3 + column];
      lengths[column] += columns[column][row] * columns[column][row];
    }
    lengths[column] = std::sqrt(lengths[column]);
    if (!std::isfinite(lengths[column]) || !(lengths[column] > 1e-12)) {
      return false;
    }
    for (double &component : columns[column]) {
      component /= lengths[column];
    }
  }
  const auto dot = [&](std::size_t left, std::size_t right) {
    double result = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      result += columns[left][axis] * columns[right][axis];
    }
    return result;
  };
  if (std::abs(dot(0, 1)) > 1e-8 || std::abs(dot(0, 2)) > 1e-8 ||
      std::abs(dot(1, 2)) > 1e-8) {
    return false;
  }
  const std::array<double, 3> cross{
      columns[0][1] * columns[1][2] - columns[0][2] * columns[1][1],
      columns[0][2] * columns[1][0] - columns[0][0] * columns[1][2],
      columns[0][0] * columns[1][1] - columns[0][1] * columns[1][0]};
  return std::abs(cross[0] * columns[2][0] +
                  cross[1] * columns[2][1] +
                  cross[2] * columns[2][2]) >
         1.0 - 1e-8;
}

bool hasUniformNonDegenerateBasis(const BonePoseCalculator::Pose &pose) {
  std::array<std::array<double, 3>, 3> columns{};
  std::array<double, 3> lengths{};
  for (std::size_t column = 0; column < 3; ++column) {
    for (std::size_t row = 0; row < 3; ++row) {
      columns[column][row] = pose.world_linear[row * 3 + column];
      lengths[column] += columns[column][row] * columns[column][row];
    }
    lengths[column] = std::sqrt(lengths[column]);
    if (!std::isfinite(lengths[column]) || !(lengths[column] > 1e-12)) {
      return false;
    }
    for (double &component : columns[column]) {
      component /= lengths[column];
    }
  }
  const auto dot = [&](std::size_t left, std::size_t right) {
    double result = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      result += columns[left][axis] * columns[right][axis];
    }
    return result;
  };
  const double maximum_length =
      std::max(lengths[0], std::max(lengths[1], lengths[2]));
  const double minimum_length =
      std::min(lengths[0], std::min(lengths[1], lengths[2]));
  if (maximum_length - minimum_length > maximum_length * 1e-8 ||
      std::abs(dot(0, 1)) > 1e-8 || std::abs(dot(0, 2)) > 1e-8 ||
      std::abs(dot(1, 2)) > 1e-8) {
    return false;
  }
  const std::array<double, 3> cross{
      columns[0][1] * columns[1][2] - columns[0][2] * columns[1][1],
      columns[0][2] * columns[1][0] - columns[0][0] * columns[1][2],
      columns[0][0] * columns[1][1] - columns[0][1] * columns[1][0]};
  return std::abs(cross[0] * columns[2][0] +
                  cross[1] * columns[2][1] +
                  cross[2] * columns[2][2]) >
         1.0 - 1e-8;
}

}

class PhysicsBaker::XpbdPeriodicAdapter final : public PeriodicStateAdapter {
public:
  explicit XpbdPeriodicAdapter(const XpbdPeriodicStateTracker *tracker)
      : tracker_(tracker) {}

  Snapshot capture() override {
    return tracker_ == nullptr ? Snapshot{} : tracker_->capture();
  }

private:
  const XpbdPeriodicStateTracker *tracker_ = nullptr;
};

class PhysicsBaker::RigidBodyPeriodicAdapter final
    : public PeriodicStateAdapter {
public:
  explicit RigidBodyPeriodicAdapter(rigidbody::RigidBodyBakeSession *session)
      : session_(session) {}

  Snapshot capture() override {
    if (session_ == nullptr) {
      return {};
    }
    return session_->capturePeriodicSnapshot();
  }

private:
  rigidbody::RigidBodyBakeSession *session_ = nullptr;
};

PhysicsBaker::PhysicsBaker(BoneMapper &bone_mapper)
    : bone_mapper_(bone_mapper) {}

void PhysicsBaker::setSourceAnimation(const loader::Animation *anim) {
  source_animation_ = anim;
}

void PhysicsBaker::setTransitionAnimation(const loader::Animation *anim) {
  transition_animation_ = anim;
}

void PhysicsBaker::setTransitionRequest(const TransitionBakeRequest *request) {
  if (initialized_) {
    throw std::logic_error(
        "transition request cannot change after initialization");
  }
  requested_transition_ = request;
}

void PhysicsBaker::setDt(double dt) {
  if (!std::isfinite(dt) || dt <= 0.0) {
    throw std::invalid_argument("dt must be a finite value greater than 0");
  }
  if (initialized_) {
    throw std::logic_error("dt cannot change after initialization; re-create "
                           "or reset the baker first");
  }
  dt_ = dt;
}

void PhysicsBaker::setProfiler(BakeProfiler profiler) {
  if (initialized_) {
    throw std::logic_error("profiler cannot change after initialization");
  }
  profiler_ = profiler;
}

void PhysicsBaker::setCancellationCheck(std::function<bool()> check) {
  cancellation_check_ = std::move(check);
}

void PhysicsBaker::setAuditPhaseCallback(std::function<void()> callback) {
  audit_phase_callback_ = std::move(callback);
}

void PhysicsBaker::checkCancellation() const {
  if (cancellation_check_ && cancellation_check_()) {
    throw BakeCancelled();
  }
}

std::optional<rigidbody::CollisionDetectionSnapshot>
PhysicsBaker::getInitialCollisionSnapshot() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->initialCollision();
}

std::optional<rigidbody::FixedSubstepStats>
PhysicsBaker::getFixedSubstepStats() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->fixedSubstepStats();
}

std::optional<rigidbody::KinematicHistoryStats>
PhysicsBaker::getKinematicHistoryStats() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->kinematicHistoryStats();
}

std::optional<rigidbody::JointPreflightDiagnostics>
PhysicsBaker::getJointPreflightDiagnostics() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->jointPreflightDiagnostics();
}

std::optional<rigidbody::ColliderPreflightDiagnostics>
PhysicsBaker::getColliderPreflightDiagnostics() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->colliderPreflightDiagnostics();
}

std::optional<rigidbody::JointSpringDiagnostics>
PhysicsBaker::getJointSpringDiagnostics() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->jointSpringDiagnostics();
}

int PhysicsBaker::getCurrentRigidBodyContactCount() const {
  return rigid_body_session_ ? rigid_body_session_->getCurrentContactCount()
                             : 0;
}

int PhysicsBaker::getMaximumRigidBodyContactCount() const {
  return rigid_body_session_ ? rigid_body_session_->getMaximumContactCount()
                             : 0;
}

double PhysicsBaker::getMaximumRuntimeRigidBodyPenetration() const {
  return rigid_body_session_ ? rigid_body_session_->getMaximumPenetration()
                             : 0.0;
}

int PhysicsBaker::getNativeBulletVersion() const {
  return rigid_body_session_ ? rigid_body_session_->getNativeBulletVersion()
                             : 0;
}

std::optional<rigidbody::RigidBodyRuntimeFingerprint>
PhysicsBaker::getRigidBodyRuntimeFingerprint() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->runtimeFingerprint();
}

std::optional<rigidbody::RigidBodyStepTrace>
PhysicsBaker::getRigidBodyStepTrace() const {
  if (!rigid_body_session_) {
    return std::nullopt;
  }
  return rigid_body_session_->stepTrace();
}

BakedFrame PhysicsBaker::captureCurrentFrameForPreview() {
  if (!initialized_) {
    throw std::logic_error("cannot preview an uninitialized baker");
  }
  return rigid_body_session_
             ? createRigidBodyFrame(currentReferencePoses(),
                                    current_sample_time_)
             : createXpbdFrame(currentReferencePoses(), current_sample_time_);
}

loader::Animation::LoopBehavior PhysicsBaker::getOutputLoopBehavior() const {
  if (active_transition_.has_value()) {
    return loader::Animation::LoopBehavior::Once;
  }
  const auto mode = bone_mapper_.config().loop_mode;
  if (mode == BoneMapper::LoopMode::ForceLoop) {
    return source_animation_ == nullptr ? loader::Animation::LoopBehavior::Once
                                        : loader::Animation::LoopBehavior::Loop;
  }
  if (mode == BoneMapper::LoopMode::ForceOnce || source_animation_ == nullptr) {
    return loader::Animation::LoopBehavior::Once;
  }
  if (source_animation_->loop) {
    return loader::Animation::LoopBehavior::Loop;
  }
  return source_animation_->loop_behavior;
}

const loader::Animation *PhysicsBaker::getOutputReferenceAnimation() const {
  return !active_transition_.has_value() || !transition_started_
             ? source_animation_
             : active_transition_->target_animation;
}

double PhysicsBaker::getOutputReferenceTime(double output_time) const {
  return !active_transition_.has_value() || !transition_started_ ||
                 transition_controller_ == nullptr
             ? output_time
             : transition_controller_->targetSampleTime(output_time);
}

std::map<std::string, BonePoseCalculator::Pose>
PhysicsBaker::sampleOutputReferencePoses(double output_time) const {
  if (active_transition_.has_value()) {
    if (!transition_started_ || transition_controller_ == nullptr) {
      throw std::logic_error(
          "transition reference poses are unavailable before transition start");
    }
    return transition_controller_->sample(output_time);
  }
  return calculatePoses(source_animation_, output_time);
}

std::set<std::string> PhysicsBaker::getOutputReferenceDependencyBones() const {
  return animationInputDependencyBones();
}

void PhysicsBaker::requireTransitionReferenceExportable() const {
  if (!active_transition_.has_value()) {
    return;
  }
  const auto dependencies = animationInputDependencyBones();
  const auto rejectDependencyMolang =
      [this, &dependencies](const loader::Animation *animation,
                            const char *animation_role) {
        if (animation == nullptr) {
          return;
        }
        for (const auto &bone_name : dependencies) {
          if (bone_mapper_.isPhysicsBone(bone_name)) {
            continue;
          }
          const auto channel = animation->bones.find(bone_name);
          if (channel == animation->bones.end()) {
            continue;
          }
          if (channel->second.has_position &&
              channel->second.position.containsMolang()) {
            throw std::invalid_argument(
                std::string("transition ") + animation_role +
                " dependency bone '" + bone_name +
                "' position contains Molang and cannot be flattened for "
                "standalone export");
          }
          if (channel->second.has_rotation &&
              channel->second.rotation.containsMolang()) {
            throw std::invalid_argument(
                std::string("transition ") + animation_role +
                " dependency bone '" + bone_name +
                "' rotation contains Molang and cannot be flattened for "
                "standalone export");
          }
        }
      };
  rejectDependencyMolang(active_transition_->source_animation, "source");
  rejectDependencyMolang(active_transition_->target_animation, "target");
}

bool PhysicsBaker::hasPeriodicLoop() const {
  return !active_transition_.has_value() && source_animation_ != nullptr &&
         isLooping() && std::isfinite(source_animation_->animation_length) &&
         source_animation_->animation_length > 0.0;
}

void PhysicsBaker::configureTransition() {
  active_transition_.reset();
  transition_controller_.reset();
  transition_started_ = false;
  transition_pre_roll_steps_ = 0;
  transition_steps_ = 0;
  transition_pre_roll_dt_ = dt_;
  transition_dt_ = dt_;

  if (requested_transition_ != nullptr) {
    active_transition_ = *requested_transition_;
  } else if (transition_animation_ != nullptr && source_animation_ != nullptr &&
             transition_animation_ != source_animation_ &&
             bone_mapper_.config().transition_duration > 0.0) {
    active_transition_ = TransitionBakeRequest::endingAtClipBoundary(
        *source_animation_, *transition_animation_,
        bone_mapper_.config().transition_duration);
  }
  if (!active_transition_.has_value()) {
    return;
  }
  if (active_transition_->source_animation != source_animation_) {
    throw std::invalid_argument(
        "transition source must be the baker source animation");
  }
  transition_pre_roll_steps_ =
      active_transition_->source_exit_time > 0
          ? std::max(1, static_cast<int>(std::ceil(
                            active_transition_->source_exit_time / dt_)))
          : 0;
  transition_pre_roll_dt_ =
      transition_pre_roll_steps_ > 0
          ? active_transition_->source_exit_time / transition_pre_roll_steps_
          : dt_;
  transition_steps_ =
      std::max(1, static_cast<int>(std::ceil(
                      active_transition_->transition_duration / dt_)));
  transition_dt_ = active_transition_->transition_duration / transition_steps_;
  output_frame_interval_ = transition_dt_;
}

int PhysicsBaker::getCycleSteps() const {
  if (source_animation_ == nullptr ||
      source_animation_->animation_length <= 0.0) {
    return 1;
  }
  return std::max(1, static_cast<int>(
                         std::ceil(source_animation_->animation_length / dt_)));
}

void PhysicsBaker::configureLoopTiming() {
  cycle_dt_ = dt_;
  loop_bake_config_.reset();
  if (!hasPeriodicLoop()) {
    return;
  }
  const int cycleSteps = getCycleSteps();
  cycle_dt_ = source_animation_->animation_length / cycleSteps;
  output_frame_interval_ = cycle_dt_;
  loop_bake_config_ = LoopBakeConfig::from(bone_mapper_.config());
}

int PhysicsBaker::calculateTotalSteps() const {
  if (active_transition_.has_value()) {
    const long required =
        static_cast<long>(transition_pre_roll_steps_) + transition_steps_;
    if (required > kMaxBakeSteps) {
      throw std::invalid_argument(
          "transition requires too many simulation steps");
    }
    return static_cast<int>(required);
  }
  if (source_animation_ == nullptr) {
    return 300;
  }
  const double length = source_animation_->animation_length;
  if (!std::isfinite(length) || length < 0.0) {
    throw std::invalid_argument(
        "animation length must be a finite non-negative number");
  }
  const double baseSteps = std::ceil(length / dt_);
  const double requiredSteps =
      hasPeriodicLoop() ? baseSteps * loop_bake_config_->maximum_warmup_cycles
                        : baseSteps;
  if (!std::isfinite(requiredSteps) || requiredSteps > kMaxBakeSteps) {
    throw std::invalid_argument("animation requires too many simulation steps");
  }
  return static_cast<int>(requiredSteps);
}

void PhysicsBaker::rebuildStructureCaches() {
  bones_by_name_.clear();
  physics_children_by_bone_.clear();
  ordered_physics_bone_cache_.clear();
  for (const auto &bone : bone_mapper_.allBones()) {
    if (!bone.name.empty()) {
      bones_by_name_[bone.name] = bone;
    }
  }
  ordered_physics_bone_cache_ = bone_mapper_.physicsBones();
  std::sort(ordered_physics_bone_cache_.begin(),
            ordered_physics_bone_cache_.end(),
            [this](const std::string &a, const std::string &b) {
              return hierarchyDepth(a) < hierarchyDepth(b);
            });
  for (const auto &name : ordered_physics_bone_cache_) {
    auto it = bones_by_name_.find(name);
    if (it != bones_by_name_.end() && it->second.has_parent &&
        bone_mapper_.isPhysicsBone(it->second.parent)) {
      physics_children_by_bone_[it->second.parent].push_back(name);
    }
  }
}

int PhysicsBaker::hierarchyDepth(const std::string &bone_name) const {
  int depth = 0;
  std::set<std::string> visited;
  auto it = bones_by_name_.find(bone_name);
  while (it != bones_by_name_.end() && it->second.has_parent &&
         visited.insert(it->second.name).second) {
    depth++;
    it = bones_by_name_.find(it->second.parent);
  }
  return depth;
}

std::map<std::string, BonePoseCalculator::Pose>
PhysicsBaker::calculatePoses(const loader::Animation *animation,
                             double time) const {
  return calculatePoses(animation, time, nullptr, nullptr);
}

std::map<std::string, BonePoseCalculator::Pose> PhysicsBaker::calculatePoses(
    const loader::Animation *animation, double time,
    const std::map<std::string, std::array<double, 3>> *position_overrides,
    const std::map<std::string, std::array<double, 3>> *rotation_overrides)
    const {
  auto profile_scope = profiler_.scope(BakeProfiler::Stage::OuterReferencePose);
  if (compiled_pose_evaluator_enabled_ && pose_evaluator_) {
    if (source_pose_binding_ &&
        source_pose_binding_->animation() == animation) {
      return pose_evaluator_->calculate(
          *source_pose_binding_, time, position_overrides,
          rotation_overrides);
    }
    return pose_evaluator_->calculate(animation, time, position_overrides,
                                      rotation_overrides);
  }
  return BonePoseCalculator::calculate(bone_mapper_.allBones(), animation, time,
                                       position_overrides, rotation_overrides);
}

void PhysicsBaker::calculateCurrentSourcePoses(double time) {
  auto profile_scope = profiler_.scope(BakeProfiler::Stage::OuterReferencePose);
  if (compiled_pose_evaluator_enabled_ && pose_evaluator_ &&
      source_pose_binding_) {
    (void)pose_evaluator_->calculateInto(
        *source_pose_binding_, time, current_reference_pose_scratch_);
    return;
  }
  current_reference_pose_scratch_.replaceResult(
      BonePoseCalculator::calculate(bone_mapper_.allBones(),
                                    source_animation_, time));
}

const BonePoseCalculator::Evaluator::PoseMap &
PhysicsBaker::currentReferencePoses() const {
  return current_reference_pose_scratch_.result();
}

void PhysicsBaker::initialize() {
  checkCancellation();
  total_bake_scope_.reset();
  auto total_bake_scope = profiler_.scope(BakeProfiler::Stage::TotalBake);
  auto initialize_scope = profiler_.scope(BakeProfiler::Stage::Initialize);
  initialized_ = false;
  frames_.clear();
  loop_cycle_frames_.clear();
  best_loop_preview_cycle_frames_.clear();
  best_loop_safe_export_cycle_frames_.clear();
  current_step_ = 0;
  current_sample_time_ = 0.0;
  frames_finalized_ = false;
  output_frame_interval_ = dt_;
  owned_particles_.clear();
  owned_constraints_.clear();
  animation_targets_.clear();
  collision_particles_.clear();
  ground_collision_constraint_ = nullptr;
  body_collision_constraint_ = nullptr;
  body_collider_cache_.reset();
  engine_.clear();


  rigid_body_session_.reset();
  rigid_body_output_scratch_.clear();
  loop_controller_.reset();
  xpbd_periodic_state_tracker_.clear();
  loop_error_report_.reset();
  best_loop_preview_cycle_candidate_.reset();
  best_loop_safe_export_cycle_candidate_.reset();
  loop_cycle_candidates_.clear();
  loop_preview_only_cycle_used_ = false;
  loop_converged_ = false;
  loop_fallback_used_ = false;
  loop_seam_correction_rejected_ = false;
  completed_loop_cycles_ = 0;
  unsafe_final_collision_count_ = 0;
  unsafe_final_joint_count_ = 0;
  maximum_final_rigid_body_penetration_ = 0.0;
  worst_final_collision_pair_.reset();
  maximum_final_joint_anchor_separation_ = 0.0;
  maximum_final_joint_angular_excess_radians_ = 0.0;
  worst_final_joint_linear_parent_.clear();
  worst_final_joint_linear_child_.clear();
  worst_final_joint_angular_parent_.clear();
  worst_final_joint_angular_child_.clear();
  worst_final_joint_angular_axis_ = -1;
  first_final_joint_euler_singularity_.reset();
  loop_seam_report_.reset();
  best_loop_preview_candidate_report_.reset();
  best_loop_safe_export_candidate_report_.reset();
  loop_seam_window_diagnostics_.clear();
  rigid_body_collision_auditor_.reset();
  rigid_body_joint_auditor_.reset();
  final_pose_reconstructor_evaluator_.reset();
  final_pose_reconstruction_scratch_.reset();

  const auto &cfg = bone_mapper_.config();
  checkCancellation();
  configureTransition();
  configureLoopTiming();
  total_steps_ = calculateTotalSteps();

  bone_mapper_.buildParticleMapping();
  rebuildStructureCaches();
  validateAnimationInputs();
  pose_evaluator_ = BonePoseCalculator::compile(bone_mapper_.allBones());
  source_pose_binding_ = pose_evaluator_->bind(source_animation_);
  current_reference_pose_scratch_.reset();
  final_pose_reconstructor_evaluator_ =
      FinalPoseReconstructor::compile(bone_mapper_.allBones());
  calculateCurrentSourcePoses(0.0);

  if (cfg.simulation_mode == BoneMapper::SimulationMode::RigidBody) {
    checkCancellation();
    rigid_body_session_ = rigidbody::RigidBodyBakeSession::create(
        bone_mapper_, source_animation_, currentReferencePoses());
    if (active_transition_.has_value()) {
      rigid_body_session_->validateJointBridgeAnimation(
          active_transition_->target_animation,
          "transition target animation");
      rigid_body_session_->validateCompoundDescendantAnimation(
          active_transition_->target_animation, "transition target animation");
      rigid_body_session_->validateCompoundDescendantTransition(
          active_transition_->target_animation,
          canonicalLoopSampleTime(*active_transition_->target_animation,
                                  active_transition_->target_entry_time));
    }
    rigid_body_session_->setPoseSampler([this](double time) {
      return calculatePoses(source_animation_, time);
    });
    rigid_body_collision_auditor_ = std::make_unique<RigidBodyCollisionAuditor>(
        bone_mapper_.allBones(), rigid_body_session_->physicsBodyDefinitions(),
        bone_mapper_.getExpandedCollisionBones(), cfg.rigid_body_unit_scale,
        cfg.rigid_body_maximum_safe_penetration, cfg.enable_ground_collision);
    rigid_body_joint_auditor_ = std::make_unique<RigidBodyJointAuditor>(
        bone_mapper_, currentReferencePoses());
    if (active_transition_.has_value()) {
      if (transition_pre_roll_steps_ == 0) {
        beginTransition(nullptr, 0.0);
      }
    } else if (hasPeriodicLoop()) {
      loop_controller_ = std::make_unique<LoopBakeController>(
          *loop_bake_config_, std::make_unique<RigidBodyPeriodicAdapter>(
                                  rigid_body_session_.get()));
      loop_cycle_frames_.push_back(
          createRigidBodyFrame(currentReferencePoses(), 0.0));
    } else {
      frames_.push_back(createRigidBodyFrame(currentReferencePoses(), 0.0));
    }
    profiler_.addCounter(
        BakeProfiler::Counter::Particles,
        static_cast<long>(
            rigid_body_session_->physicsBodyDefinitions().size()));
    initialized_ = true;
    checkCancellation();
    total_bake_scope_.emplace(std::move(total_bake_scope));
    return;
  }

  engine_.setGravity(models::Vector3(0.0, cfg.gravity_y, 0.0));
  engine_.setSolverIterations(cfg.solver_iterations);
  engine_.setSimdMode(cfg.simd_mode);
  engine_.setAerodynamics(relativeAirVelocity(cfg), cfg.air_drag,
                          cfg.turbulence);

  const auto &physicsBones = bone_mapper_.physicsBones();
  collision_particles_.assign(physicsBones.size(), nullptr);
  animation_targets_.assign(physicsBones.size(), nullptr);

  for (const auto &boneName : physicsBones) {
    checkCancellation();
    const bool isFixed = bone_mapper_.isFixedBone(boneName);
    double mass = isFixed ? 0.0 : bone_mapper_.getEffectiveMass(boneName);
    if (!isFixed && !(mass > 0.0)) {
      throw std::invalid_argument(
          "dynamic physics bone requires positive mass: " + boneName);
    }
    auto particle = std::make_unique<models::Particle>(mass);
    particle->setForceMultipliers(
        bone_mapper_.getEffectiveGravityScale(boneName),
        bone_mapper_.getEffectiveWindInfluence(boneName),
        bone_mapper_.getEffectiveTurbulenceInfluence(boneName));
    const auto &currentReference = currentReferencePoses();
    auto poseIt = currentReference.find(boneName);
    const auto &wp = poseIt == currentReference.end()
                         ? std::array<double, 3>{0, 0, 0}
                         : poseIt->second.world_position;
    particle->position().set(wp[0], wp[1], wp[2]);
    particle->prevPosition().set(wp[0], wp[1], wp[2]);
    engine_.addParticle(particle.get());
    const int particleIndex = bone_mapper_.getParticleIndex(boneName);
    if (particleIndex >= 0) {
      collision_particles_[static_cast<std::size_t>(particleIndex)] =
          particle.get();
    }
    owned_particles_.push_back(std::move(particle));
  }

  for (const auto &def : bone_mapper_.generateChainConstraints()) {
    checkCancellation();
    const int idxA = bone_mapper_.getParticleIndex(def.bone_a);
    const int idxB = bone_mapper_.getParticleIndex(def.bone_b);
    if (idxA < 0 || idxB < 0) {
      continue;
    }
    std::unique_ptr<constraints::Constraint> c;
    if (def.rest_length == 0.0) {
      c = std::make_unique<constraints::WeldConstraint>(
          idxA, idxB, def.compliance, def.damping_compliance);
    } else {
      c = std::make_unique<constraints::DistanceConstraint>(
          idxA, idxB, def.rest_length, def.compliance, def.damping_compliance);
    }
    engine_.addConstraint(c.get());
    owned_constraints_.push_back(std::move(c));
  }

  for (const auto &def : bone_mapper_.generateAngleConstraints()) {
    checkCancellation();
    const int idxA = bone_mapper_.getParticleIndex(def.bone_a);
    const int idxB = bone_mapper_.getParticleIndex(def.bone_b);
    const int idxC = bone_mapper_.getParticleIndex(def.bone_c);
    if (idxA < 0 || idxB < 0 || idxC < 0) {
      continue;
    }
    auto c = std::make_unique<constraints::AngleConstraint>(
        idxA, idxB, idxC, def.min_angle_radians, def.max_angle_radians,
        def.compliance, def.fallback_normal_x, def.fallback_normal_y,
        def.fallback_normal_z);
    engine_.addConstraint(c.get());
    owned_constraints_.push_back(std::move(c));
  }

  for (const auto &boneName : physicsBones) {
    checkCancellation();
    const int particleIndex = bone_mapper_.getParticleIndex(boneName);
    const double pullCompliance =
        bone_mapper_.getEffectiveAnimPullCompliance(boneName);
    if (particleIndex < 0 || bone_mapper_.isFixedBone(boneName) ||
        pullCompliance <= 0.0) {
      continue;
    }
    auto target = std::make_unique<constraints::TargetConstraint>(
        particleIndex, pullCompliance);
    const auto &currentReference = currentReferencePoses();
    auto poseIt = currentReference.find(boneName);
    if (poseIt != currentReference.end()) {
      target->setTarget(poseIt->second.world_position[0],
                        poseIt->second.world_position[1],
                        poseIt->second.world_position[2]);
    }
    animation_targets_[static_cast<std::size_t>(particleIndex)] = target.get();
    engine_.addConstraint(target.get());
    owned_constraints_.push_back(std::move(target));
  }


  const auto collisionBones = bone_mapper_.getExpandedCollisionBones();
  body_collision_constraint_ = nullptr;
  body_collider_cache_.reset();
  if (!collisionBones.empty()) {
    body_collider_cache_ = std::make_unique<BodyColliderCache>(
        bone_mapper_.allBones(), collisionBones);
    body_collider_cache_->initialize(currentReferencePoses());
    if (!body_collider_cache_->colliders().empty()) {
      std::vector<int> indices(collision_particles_.size());
      for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = static_cast<int>(i);
      }
      auto body = std::make_unique<constraints::VertexFaceCollisionConstraint>(
          std::move(indices), static_cast<int>(collision_particles_.size()),
          *body_collider_cache_, cfg.collision_skin,
          cfg.xpbd_collision_restitution);
      body_collision_constraint_ = body.get();
      engine_.addConstraint(body.get());
      body->projectInitial(collision_particles_);
      owned_constraints_.push_back(std::move(body));
    }
  }

  if (cfg.enable_ground_collision) {
    std::vector<int> indices(collision_particles_.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
      indices[i] = static_cast<int>(i);
    }
    auto ground = std::make_unique<constraints::GroundCollisionConstraint>(
        indices, static_cast<int>(collision_particles_.size()), 0.0,
        cfg.collision_skin, cfg.xpbd_collision_restitution);
    ground_collision_constraint_ = ground.get();
    engine_.addConstraint(ground.get());
    ground->projectInitial(collision_particles_);
    owned_constraints_.push_back(std::move(ground));
  }

  if (active_transition_.has_value()) {
    if (transition_pre_roll_steps_ == 0) {
      beginTransition(nullptr, 0.0);
    }
  } else if (hasPeriodicLoop()) {
    XpbdPeriodicStateTracker::WorldRotations initialWorldRotations;
    BakedFrame initialFrame =
        createXpbdFrame(currentReferencePoses(), 0.0, &initialWorldRotations);
    xpbd_periodic_state_tracker_.initialize(initialFrame,
                                            initialWorldRotations);
    loop_cycle_frames_.push_back(std::move(initialFrame));
    loop_controller_ = std::make_unique<LoopBakeController>(
        *loop_bake_config_,
        std::make_unique<XpbdPeriodicAdapter>(&xpbd_periodic_state_tracker_));
  } else {
    recordFrame(currentReferencePoses());
  }
  profiler_.addCounter(BakeProfiler::Counter::Particles,
                       static_cast<long>(engine_.particleCount()));
  profiler_.addCounter(BakeProfiler::Counter::Constraints,
                       static_cast<long>(engine_.constraintCount()));
  if (body_collider_cache_ != nullptr) {
    profiler_.addCounter(
        BakeProfiler::Counter::BodyColliders,
        static_cast<long>(body_collider_cache_->colliders().size()));
  }
  initialized_ = true;
  checkCancellation();
  total_bake_scope_.emplace(std::move(total_bake_scope));
}

void PhysicsBaker::updateFixedBones(
    const std::map<std::string, BonePoseCalculator::Pose> &poses,
    double step_dt, bool continuous_history) {
  std::map<std::string, BonePoseCalculator::Pose> previousPhase;
  std::map<std::string, BonePoseCalculator::Pose> nextPhase;
  if (!continuous_history && hasPeriodicLoop()) {
    const double length = source_animation_->animation_length;
    previousPhase =
        calculatePoses(source_animation_, std::max(0.0, length - cycle_dt_));
    nextPhase = calculatePoses(source_animation_, std::min(length, cycle_dt_));
  }
  for (const auto &boneName : bone_mapper_.physicsBones()) {
    if (!bone_mapper_.isFixedBone(boneName)) {
      continue;
    }
    const int pIdx = bone_mapper_.getParticleIndex(boneName);
    if (pIdx < 0) {
      continue;
    }
    models::Particle *p = engine_.getParticle(static_cast<std::size_t>(pIdx));
    if (!p->isFixed()) {
      continue;
    }
    auto poseIt = poses.find(boneName);
    if (poseIt == poses.end()) {
      continue;
    }
    const double nx = poseIt->second.world_position[0];
    const double ny = poseIt->second.world_position[1];
    const double nz = poseIt->second.world_position[2];
    if (continuous_history) {
      p->setKinematicPosition(nx, ny, nz, step_dt);
    } else if (previousPhase.contains(boneName) &&
               nextPhase.contains(boneName)) {
      const auto &before = previousPhase.at(boneName).world_position;
      const auto &after = nextPhase.at(boneName).world_position;
      const double inverseSpan = 1.0 / (2.0 * cycle_dt_);
      p->synchronizeKinematicPosition(
          nx, ny, nz, (after[0] - before[0]) * inverseSpan,
          (after[1] - before[1]) * inverseSpan,
          (after[2] - before[2]) * inverseSpan, step_dt);
    } else {
      p->synchronizeKinematicPosition(nx, ny, nz);
    }
  }
}

void PhysicsBaker::updateAnimationTargets(
    const std::map<std::string, BonePoseCalculator::Pose> &poses) {
  for (const auto &boneName : bone_mapper_.physicsBones()) {
    const int pIdx = bone_mapper_.getParticleIndex(boneName);
    if (pIdx < 0 ||
        static_cast<std::size_t>(pIdx) >= animation_targets_.size()) {
      continue;
    }
    constraints::TargetConstraint *target =
        animation_targets_[static_cast<std::size_t>(pIdx)];
    if (target == nullptr) {
      continue;
    }
    auto poseIt = poses.find(boneName);
    if (poseIt == poses.end()) {
      continue;
    }
    target->setTarget(poseIt->second.world_position[0],
                      poseIt->second.world_position[1],
                      poseIt->second.world_position[2]);
  }
}

void PhysicsBaker::step() {
  checkCancellation();
  if (current_step_ >= total_steps_) {
    return;
  }
  if (active_transition_.has_value()) {
    stepTransition();
    profiler_.addCounter(BakeProfiler::Counter::SimulationSteps, 1);
    return;
  }
  double time = (current_step_ + 1) * dt_;
  double stepDt = dt_;
  bool historyContinuous = true;
  bool cycleBoundary = false;
  if (source_animation_ != nullptr &&
      source_animation_->animation_length > 0.0) {
    if (hasPeriodicLoop()) {
      const int cycleSteps = getCycleSteps();
      const int phaseStep = (current_step_ + 1) % cycleSteps;
      cycleBoundary = phaseStep == 0;
      time = phaseStep == 0 ? 0.0
                            : std::min(phaseStep * cycle_dt_,
                                       source_animation_->animation_length);
      stepDt = cycle_dt_;
      historyContinuous = !cycleBoundary;
    } else {
      stepDt = std::min(dt_, source_animation_->animation_length -
                                 current_sample_time_);
      time = current_sample_time_ + stepDt;
    }
  }
  if (!std::isfinite(stepDt) || !(stepDt > 0.0)) {
    throw std::logic_error("bake step duration must be positive and finite");
  }

  calculateCurrentSourcePoses(time);
  if (rigid_body_session_) {
    auto solver_scope = profiler_.scope(BakeProfiler::Stage::Solver);
    rigid_body_session_->advance(
        current_sample_time_, time, stepDt,
        historyContinuous || hasPeriodicLoop(),
        hasPeriodicLoop() ? source_animation_->animation_length : 0.0,
        &currentReferencePoses());
  } else {
    advanceSolvers(time, stepDt, historyContinuous,
                   hasPeriodicLoop() ? source_animation_->animation_length
                                     : 0.0);
  }

  current_step_++;
  current_sample_time_ = time;
  recordAdvancedState(cycleBoundary);
  profiler_.addCounter(BakeProfiler::Counter::SimulationSteps, 1);
}

void PhysicsBaker::advanceSolvers(double end_time, double step_dt,
                                  bool continuous_history,
                                  double forcing_period) {
  auto solver_scope = profiler_.scope(BakeProfiler::Stage::Solver);
  if (rigid_body_session_) {
    rigid_body_session_->advance(current_sample_time_, end_time, step_dt,
                                 continuous_history, forcing_period,
                                 &currentReferencePoses());
    return;
  }
  updateFixedBones(currentReferencePoses(), step_dt, continuous_history);
  updateAnimationTargets(currentReferencePoses());
  if (body_collider_cache_ != nullptr) {

    body_collider_cache_->advance(currentReferencePoses(),
                                  continuous_history || hasPeriodicLoop());
  }
  engine_.step(step_dt, end_time, forcing_period);
  if (body_collision_constraint_ != nullptr) {
    body_collision_constraint_->postSolveVelocity(collision_particles_);
  }
  if (ground_collision_constraint_ != nullptr) {
    ground_collision_constraint_->postSolveVelocity(collision_particles_);
  }
}

void PhysicsBaker::stepTransition() {
  if (!transition_started_) {
    BakedFrame previousPhysicalState =
        rigid_body_session_
            ? createRigidBodyFrame(currentReferencePoses(),
                                   current_sample_time_)
            : createXpbdFrame(currentReferencePoses(), current_sample_time_);
    const double previousSampleTime = current_sample_time_;
    const int next = current_step_ + 1;
    const double time = std::min(active_transition_->source_exit_time,
                                 next * transition_pre_roll_dt_);
    calculateCurrentSourcePoses(time);
    advanceSolvers(time, transition_pre_roll_dt_, true,
                   source_animation_->loop ? source_animation_->animation_length
                                           : 0.0);
    current_step_++;
    current_sample_time_ = time;
    if (current_step_ >= transition_pre_roll_steps_) {
      beginTransition(&previousPhysicalState,
                      std::max(0.0, time - previousSampleTime));
    }
    return;
  }

  const int transitionStep = current_step_ - transition_pre_roll_steps_ + 1;
  const double time = std::min(active_transition_->transition_duration,
                               transitionStep * transition_dt_);
  (void)transition_controller_->sampleInto(
      time, current_reference_pose_scratch_);
  const double forcingPeriod =
      active_transition_->target_animation->loop
          ? active_transition_->target_animation->animation_length
          : 0.0;
  advanceSolvers(time, transition_dt_, true, forcingPeriod);
  current_step_++;
  current_sample_time_ = time;
  frames_.push_back(rigid_body_session_
                        ? createRigidBodyFrame(currentReferencePoses(), time)
                        : createXpbdFrame(currentReferencePoses(), time));
}

void PhysicsBaker::beginTransition(const BakedFrame *previous_physical_state,
                                   double physical_frame_span) {
  BakedFrame physicalState =
      rigid_body_session_ ? createRigidBodyFrame(currentReferencePoses(), 0.0)
                          : createXpbdFrame(currentReferencePoses(), 0.0);
  transition_controller_ = std::make_unique<TransitionBakeController>(
      *active_transition_, bone_mapper_.allBones(), &physicalState,
      previous_physical_state, physical_frame_span);
  if (rigid_body_session_) {
    rigid_body_session_->setPoseSampler(
        [this](double time) { return transition_controller_->sample(time); });
  }
  transition_started_ = true;
  current_sample_time_ = 0.0;
  (void)transition_controller_->sampleInto(
      0.0, current_reference_pose_scratch_);
  frames_.clear();
  frames_.push_back(rigid_body_session_
                        ? createRigidBodyFrame(currentReferencePoses(), 0.0)
                        : createXpbdFrame(currentReferencePoses(), 0.0));
}

void PhysicsBaker::recordFrame(
    const std::map<std::string, BonePoseCalculator::Pose> &reference_poses) {
  if (rigid_body_session_) {
    frames_.push_back(
        createRigidBodyFrame(reference_poses, currentFrameTime()));
  } else {
    frames_.push_back(createXpbdFrame(reference_poses, currentFrameTime()));
  }
}

BakedFrame PhysicsBaker::createRigidBodyFrame(
    const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
    double frame_time) {
  auto frame_capture_scope = profiler_.scope(BakeProfiler::Stage::FrameCapture);
  profiler_.addCounter(BakeProfiler::Counter::FrameCaptures, 1);
  BakedFrame frame;
  frame.time = frame_time;
  if (!rigid_body_session_) {
    return frame;
  }
  auto reconstruction_scope =
      profiler_.scope(BakeProfiler::Stage::Reconstruction);
  rigid_body_session_->captureBoneOutputsInto(reference_poses,
                                              rigid_body_output_scratch_);
  reconstruction_scope.stop();
  for (const auto &output : rigid_body_output_scratch_) {
    BoneState state;
    state.bone_name = output.bone_name;
    state.position = output.position;
    state.rotation = output.rotation;
    state.linear_velocity = output.linear_velocity;
    state.world_position = output.world_position;
    state.has_world_position = true;
    frame.bone_states.push_back(state);
  }
  frame.rebuildIndex();
  return frame;
}

void PhysicsBaker::recordAdvancedState(bool cycle_boundary) {
  if (!hasPeriodicLoop()) {
    recordFrame(currentReferencePoses());
    return;
  }
  const double frameTime =
      cycle_boundary ? source_animation_->animation_length
                     : ((current_step_ % getCycleSteps()) * cycle_dt_);
  XpbdPeriodicStateTracker::WorldRotations worldRotations;
  BakedFrame frame;
  if (rigid_body_session_) {




    const auto exportReferencePoses =
        cycle_boundary ? calculatePoses(source_animation_,
                                        source_animation_->animation_length)
                       : currentReferencePoses();
    frame = createRigidBodyFrame(exportReferencePoses, frameTime);
  } else {
    frame =
        createXpbdFrame(currentReferencePoses(), frameTime, &worldRotations);
  }
  if (!rigid_body_session_) {
    xpbd_periodic_state_tracker_.advance(frame, worldRotations, cycle_dt_);
  }
  loop_cycle_frames_.push_back(frame);
  if (!cycle_boundary) {
    return;
  }

  auto loop_controller_scope =
      profiler_.scope(BakeProfiler::Stage::LoopController);
  const auto outcome = loop_controller_->completeCycle();
  profiler_.addCounter(BakeProfiler::Counter::LoopCycles, 1);
  completed_loop_cycles_ = outcome.completed_cycles;
  if (outcome.report) {
    loop_error_report_ = *outcome.report;
  }
  loop_converged_ = outcome.converged;
  if (outcome.report &&
      outcome.completed_cycles >= loop_bake_config_->minimum_warmup_cycles) {
    profiler_.addCounter(BakeProfiler::Counter::LoopCandidates, 1);
    LoopCycleCandidate candidate{outcome.completed_cycles,
                                 outcome.valid,
                                 outcome.collision_safe,
                                 outcome.within_tolerances,
                                 outcome.normalized_score,
                                 *outcome.report,
                                 {}};
    candidate.rejection_reasons =
        candidate.report.rejectionReasons(*loop_bake_config_);
    loop_cycle_candidates_.push_back(candidate);
    if (candidate.isBetterPreviewThan(
            best_loop_preview_cycle_candidate_
                ? &*best_loop_preview_cycle_candidate_
                : nullptr)) {
      best_loop_preview_cycle_candidate_ = candidate;
      best_loop_preview_cycle_frames_ = copyFrames(loop_cycle_frames_);
    }
    if (candidate.isBetterSafeExportThan(
            best_loop_safe_export_cycle_candidate_
                ? &*best_loop_safe_export_cycle_candidate_
                : nullptr)) {
      best_loop_safe_export_cycle_candidate_ = candidate;
      best_loop_safe_export_cycle_frames_ = copyFrames(loop_cycle_frames_);
    }
  }
  frames_ = loop_cycle_frames_;
  if (outcome.finished) {
    loop_preview_only_cycle_used_ = false;
    if (!best_loop_safe_export_cycle_frames_.empty() &&
        best_loop_safe_export_cycle_candidate_) {
      frames_ = copyFrames(best_loop_safe_export_cycle_frames_);
      loop_error_report_ = best_loop_safe_export_cycle_candidate_->report;
    } else if (!best_loop_preview_cycle_frames_.empty() &&
               best_loop_preview_cycle_candidate_) {
      frames_ = copyFrames(best_loop_preview_cycle_frames_);
      loop_error_report_ = best_loop_preview_cycle_candidate_->report;
      loop_preview_only_cycle_used_ = true;
    } else {
      throw std::runtime_error(
          "loop bake produced no valid cycle candidate");
    }
    loop_fallback_used_ = outcome.use_fallback;
    total_steps_ = current_step_;
    return;
  }



  BakedFrame boundary =
      rigid_body_session_ ? createRigidBodyFrame(currentReferencePoses(), 0.0)
                          : copyFrameAtTime(frame, 0.0);
  loop_cycle_frames_.clear();
  loop_cycle_frames_.push_back(boundary);
}

double PhysicsBaker::currentFrameTime() const {
  if (source_animation_ != nullptr && isLooping() &&
      source_animation_->animation_length > 0.0) {
    const int cycleSteps = getCycleSteps();
    return std::min((current_step_ - cycleSteps) * cycle_dt_,
                    source_animation_->animation_length);
  }
  if (source_animation_ != nullptr &&
      source_animation_->animation_length > 0.0) {
    return std::min(current_step_ * dt_, source_animation_->animation_length);
  }
  return current_step_ * dt_;
}

BakedFrame PhysicsBaker::createXpbdFrame(
    const std::map<std::string, BonePoseCalculator::Pose> &reference_poses_in,
    double frame_time,
    XpbdPeriodicStateTracker::WorldRotations *final_world_rotations) {
  auto frame_capture_scope = profiler_.scope(BakeProfiler::Stage::FrameCapture);
  profiler_.addCounter(BakeProfiler::Counter::FrameCaptures, 1);
  auto referencePoses = reference_poses_in;
  if (referencePoses.empty()) {
    referencePoses = calculatePoses(source_animation_, frame_time);
  }
  std::map<std::string, FinalPoseReconstructor::WorldTarget> physicsTargets;

  for (const auto &boneName : ordered_physics_bone_cache_) {
    const int pIdx = bone_mapper_.getParticleIndex(boneName);
    if (pIdx < 0) {
      continue;
    }
    models::Particle *p = engine_.getParticle(static_cast<std::size_t>(pIdx));
    auto boneIt = bones_by_name_.find(boneName);
    auto refIt = referencePoses.find(boneName);
    if (boneIt == bones_by_name_.end() || refIt == referencePoses.end()) {
      continue;
    }
    const auto &reference = refIt->second;
    FinalPoseReconstructor::WorldTarget target;
    target.position = {p->position().x, p->position().y, p->position().z};

    std::vector<RotationUtil::Vec3> referenceDirections;
    std::vector<RotationUtil::Vec3> simulatedDirections;
    auto childrenIt = physics_children_by_bone_.find(boneName);
    if (childrenIt != physics_children_by_bone_.end()) {
      for (const auto &childName : childrenIt->second) {
        auto childRef = referencePoses.find(childName);
        const int childIndex = bone_mapper_.getParticleIndex(childName);
        if (childRef == referencePoses.end() || childIndex < 0) {
          continue;
        }
        models::Particle *childParticle =
            engine_.getParticle(static_cast<std::size_t>(childIndex));
        RotationUtil::Vec3 referenceDirection{
            childRef->second.world_position[0] - reference.world_position[0],
            childRef->second.world_position[1] - reference.world_position[1],
            childRef->second.world_position[2] - reference.world_position[2]};
        RotationUtil::Vec3 simulatedDirection{
            childParticle->position().x - p->position().x,
            childParticle->position().y - p->position().y,
            childParticle->position().z - p->position().z};
        const double refLen2 = referenceDirection[0] * referenceDirection[0] +
                               referenceDirection[1] * referenceDirection[1] +
                               referenceDirection[2] * referenceDirection[2];
        const double simLen2 = simulatedDirection[0] * simulatedDirection[0] +
                               simulatedDirection[1] * simulatedDirection[1] +
                               simulatedDirection[2] * simulatedDirection[2];
        if (refLen2 > 1e-12 && simLen2 > 1e-12) {
          referenceDirections.push_back(referenceDirection);
          simulatedDirections.push_back(simulatedDirection);
        }
      }
    }
    if (!referenceDirections.empty()) {
      const auto delta = RotationUtil::quaternionFromDirectionPairs(
          referenceDirections.data(), simulatedDirections.data(),
          static_cast<int>(referenceDirections.size()));
      target.rotation =
          RotationUtil::quaternionMultiply(delta, reference.world_rotation);
    }
    physicsTargets.emplace(boneName, target);
  }

  auto reconstruction_scope =
      profiler_.scope(BakeProfiler::Stage::Reconstruction);
  if (!final_pose_reconstructor_evaluator_) {
    throw std::logic_error("final pose reconstructor is not initialized");
  }
  const auto &reconstructed =
      final_pose_reconstructor_evaluator_->reconstructInto(
          referencePoses, physicsTargets,
          final_pose_reconstruction_scratch_);
  reconstruction_scope.stop();
  if (final_world_rotations != nullptr) {
    final_world_rotations->clear();
  }

  std::vector<BoneState> boneStates;
  boneStates.reserve(ordered_physics_bone_cache_.size());
  for (const auto &boneName : ordered_physics_bone_cache_) {
    const int pIdx = bone_mapper_.getParticleIndex(boneName);
    auto channels = reconstructed.local_channels.find(boneName);
    auto pose = reconstructed.world_poses.find(boneName);
    if (pIdx < 0 || channels == reconstructed.local_channels.end() ||
        pose == reconstructed.world_poses.end() ||
        !physicsTargets.contains(boneName)) {
      continue;
    }
    models::Particle *p = engine_.getParticle(static_cast<std::size_t>(pIdx));
    BoneState state;
    state.bone_name = boneName;
    state.position = channels->second.position;
    state.rotation = channels->second.rotation;
    state.world_position = pose->second.world_position;
    state.has_world_position = true;
    state.linear_velocity = {p->velocity().x, p->velocity().y, p->velocity().z};
    boneStates.push_back(state);
    if (final_world_rotations != nullptr) {
      final_world_rotations->emplace(boneName, pose->second.world_rotation);
    }
  }
  BakedFrame frame;
  frame.time = frame_time;
  frame.bone_states = std::move(boneStates);
  frame.rebuildIndex();
  return frame;
}

void PhysicsBaker::runToEnd() {
  while (current_step_ < total_steps_) {
    step();
  }
  finalizeFrames();
}

void PhysicsBaker::runSteps(int n) {
  for (int i = 0; i < n && current_step_ < total_steps_; ++i) {
    step();
  }
}

void PhysicsBaker::finalizeFrames() {
  checkCancellation();
  if (frames_finalized_) {
    return;
  }
  if (!active_transition_.has_value()) {
    resampleOutputTimeline();
    blendOrdinaryFramesToReferenceEdges();
  }
  checkCancellation();
  if (hasPeriodicLoop()) {
    finalizeLoopSeam();
  }
  blendTransitionFramesToMovingTarget();
  checkCancellation();
  unwrapFinalRotations();
  checkCancellation();
  if (audit_phase_callback_) {
    audit_phase_callback_();
  }
  rebuildFinalWorldPositionsAndAudit();
  checkCancellation();
  recomputeFinalLinearVelocities();
  checkCancellation();
  normalizePeriodicOutputTimeline();
  checkCancellation();
  if (profiler_.isEnabled() && body_collision_constraint_ != nullptr) {
    const auto diagnostics = body_collision_constraint_->diagnostics();
    profiler_.setCounter(
        BakeProfiler::Counter::XpbdBodyBroadPhaseQueries,
        static_cast<std::int64_t>(diagnostics.broad_phase_queries));
    profiler_.setCounter(
        BakeProfiler::Counter::XpbdBodyBroadPhasePossible,
        static_cast<std::int64_t>(diagnostics.broad_phase_possible));
    profiler_.setCounter(
        BakeProfiler::Counter::XpbdBodyBroadPhaseCandidates,
        static_cast<std::int64_t>(diagnostics.broad_phase_candidates));
    profiler_.setCounter(
        BakeProfiler::Counter::XpbdBodyNarrowPhaseTests,
        static_cast<std::int64_t>(diagnostics.narrow_phase_tests));
  }
  frames_finalized_ = true;
  profiler_.addCounter(BakeProfiler::Counter::OutputFrames,
                       static_cast<long>(frames_.size()));
  profiler_.addCounter(BakeProfiler::Counter::UnsafeFinalCollisions,
                       static_cast<long>(unsafe_final_collision_count_));
  total_bake_scope_.reset();
}

void PhysicsBaker::resampleOutputTimeline() {
  auto resample_scope = profiler_.scope(BakeProfiler::Stage::Resample);
  if (source_animation_ == nullptr || frames_.size() < 3 ||
      bone_mapper_.config().output_timeline_mode !=
          BoneMapper::OutputTimelineMode::SourceKeyframeGrid) {
    return;
  }
  const double interval =
      OutputTimelineResampler::inferSourceFrameInterval(
          *source_animation_, output_frame_interval_);
  if (!std::isfinite(interval) || !(interval > 0.0)) {
    return;
  }
  const double interval_scale = std::max(interval, output_frame_interval_);
  if (std::abs(interval - output_frame_interval_) <=
      interval_scale * 1e-9) {
    return;
  }
  const double length = frames_.back().time;
  if (!std::isfinite(length) || !(length > 0.0)) {
    return;
  }
  frames_ = OutputTimelineResampler::resample(
      frames_, bones_by_name_, interval, length, OutputEndpointPolicy::Closed);
  output_frame_interval_ = interval;
}

void PhysicsBaker::normalizePeriodicOutputTimeline() {
  if (!hasPeriodicLoop() || source_animation_ == nullptr || frames_.size() < 2) {
    return;
  }
  const double length = source_animation_->animation_length;
  if (!std::isfinite(length) || !(length > 0.0) ||
      !std::isfinite(output_frame_interval_) ||
      !(output_frame_interval_ > 0.0)) {
    return;
  }



  frames_ = OutputTimelineResampler::resample(
      frames_, bones_by_name_, output_frame_interval_, length,
      OutputEndpointPolicy::HalfOpenPeriodic);
}

std::set<std::string> PhysicsBaker::fixedBones() const {
  std::set<std::string> fixed;
  for (const auto &name : bone_mapper_.physicsBones()) {
    if (bone_mapper_.isFixedBone(name)) {
      fixed.insert(name);
    }
  }
  return fixed;
}

void PhysicsBaker::finalizeLoopSeam() {
  checkCancellation();
  auto loop_finalization_scope =
      profiler_.scope(BakeProfiler::Stage::LoopFinalization);
  const CandidateAudit baseline_audit = auditLoopCandidate(frames_);
  loop_seam_report_ =
      measureLoopSeam(frames_, false, 0.0, 0.0, baseline_audit);
  loop_fallback_used_ = false;
  best_loop_preview_candidate_report_.reset();
  best_loop_safe_export_candidate_report_.reset();
  loop_seam_window_diagnostics_.clear();
  const auto windowDiagnostics =
      [&](const LoopSeamReport &report, const CandidateAudit &audit,
          int window_start_index, double window_start_time,
          int canonicalized_bones, int preserved_drivers,
          int driver_conflicts, bool accepted) {
        LoopSeamWindowDiagnostics diagnostics;
        const auto continuity = report.physicsSeamGate(bone_mapper_.config());
        const auto driver = report.driverGate(bone_mapper_.config());
        const auto quantization = report.quantizationGate();
        const auto collision = report.collisionGate(bone_mapper_.config());
        const auto joint = report.jointGate();
        const auto export_gate = report.exportGate(bone_mapper_.config());
        diagnostics.window_duration_seconds =
            report.correctionWindowDurationSeconds();
        diagnostics.window_ratio = report.correctionWindowRatio();
        diagnostics.window_start_index = window_start_index;
        diagnostics.window_start_time = window_start_time;
        diagnostics.corrected = report.correctionApplied();
        diagnostics.valid = report.validation().valid && report.auditValid() &&
                            audit.valid;
        diagnostics.c0_pass = continuity.c0_pass;
        diagnostics.c1_pass = continuity.c1_pass;
        diagnostics.c2_pass = continuity.c2_pass;
        diagnostics.driver_pass = driver.passes();
        diagnostics.driver_c0_pass = driver.c0_pass;
        diagnostics.driver_c1_pass = driver.c1_pass;
        diagnostics.driver_c2_pass = driver.c2_pass;
        diagnostics.validation_pass = export_gate.validation_pass;
        diagnostics.physics_seam_pass = export_gate.physics_seam_pass;
        diagnostics.driver_seam_pass = export_gate.driver_seam_pass;
        diagnostics.quantization_pass = export_gate.quantization_pass;
        diagnostics.collision_pass = export_gate.collision_pass;
        diagnostics.joint_pass = export_gate.joint_pass;
        diagnostics.export_pass = export_gate.passes();
        diagnostics.collision_safe = audit.collision_safe;
        diagnostics.joint_safe = audit.joint_safe;
        diagnostics.accepted = accepted;
        diagnostics.score = report.previewQualityScore(bone_mapper_.config());
        diagnostics.maximum_penetration = audit.maximum_penetration;
        diagnostics.maximum_penetration_time =
            audit.maximum_penetration_time;
        diagnostics.joint_failure_time = audit.joint_failure_time;
        diagnostics.interpolation_failure_time =
            audit.interpolation_failure_time;
        diagnostics.interpolated_sample_count =
            audit.interpolated_sample_count;
        diagnostics.canonicalized_bone_count = canonicalized_bones;
        diagnostics.preserved_driver_bone_count = preserved_drivers;
        diagnostics.driver_endpoint_conflict_count = driver_conflicts;
        diagnostics.first_invalid_bone =
            audit.first_invalid_bone.empty()
                ? report.validation().first_missing_bone
                : audit.first_invalid_bone;
        diagnostics.first_invalid_field =
            audit.first_invalid_field.empty()
                ? report.validation().first_invalid_field
                : audit.first_invalid_field;
        if (!diagnostics.valid) {
          diagnostics.rejection_reasons.emplace_back("invalid_candidate");
        } else if (report.validation().verified_continuity_order <
                   (bone_mapper_.config().loop_seam_match_acceleration ? 2
                                                                      : 1)) {
          diagnostics.rejection_reasons.emplace_back("insufficient_samples");
        } else {
          if (!diagnostics.c0_pass) {
            diagnostics.rejection_reasons.emplace_back("physics_c0");
          }
          if (!diagnostics.c1_pass) {
            diagnostics.rejection_reasons.emplace_back("physics_c1");
          }
          if (!diagnostics.c2_pass) {
            diagnostics.rejection_reasons.emplace_back("physics_c2");
          }
        }
        if (driver.available) {
          if (!diagnostics.driver_c0_pass) {
            diagnostics.rejection_reasons.emplace_back("driver_c0");
          }
          if (!diagnostics.driver_c1_pass) {
            diagnostics.rejection_reasons.emplace_back("driver_c1");
          }
          if (!diagnostics.driver_c2_pass) {
            diagnostics.rejection_reasons.emplace_back("driver_c2");
          }
        }
        if (!quantization.local_c0_pass) {
          diagnostics.rejection_reasons.emplace_back("quantization_local");
        }
        if (!quantization.final_world_c0_pass) {
          diagnostics.rejection_reasons.emplace_back(
              "quantization_final_world");
        }
        if (!collision.candidate_safe) {
          diagnostics.rejection_reasons.emplace_back("collision");
        }
        if (!collision.penetration_safe) {
          diagnostics.rejection_reasons.emplace_back("maximum_penetration");
        }
        if (!joint.candidate_safe) {
          diagnostics.rejection_reasons.emplace_back("joint");
        }
        return diagnostics;
      };
  const bool baselineAccepted =
      loop_seam_report_->passes(bone_mapper_.config());
  loop_seam_window_diagnostics_.push_back(windowDiagnostics(
      *loop_seam_report_, baseline_audit, 0,
      frames_.empty() ? 0.0 : frames_.front().time, 0, 0, 0,
      baselineAccepted));
  std::optional<std::size_t> best_preview_index;
  std::optional<std::size_t> best_safe_export_index;
  std::vector<BakedFrame> best_safe_export_frames;
  double best_preview_score = std::numeric_limits<double>::infinity();
  double best_safe_export_score = std::numeric_limits<double>::infinity();
  if (loop_seam_window_diagnostics_.front().valid) {
    best_preview_index = 0;
    best_preview_score = loop_seam_window_diagnostics_.front().score;
    best_loop_preview_candidate_report_ = *loop_seam_report_;
  }
  if (baselineAccepted) {
    best_loop_safe_export_candidate_report_ = *loop_seam_report_;
    loop_seam_window_diagnostics_.front().best_preview = true;
    loop_seam_window_diagnostics_.front().best_safe_export = true;
    loop_seam_window_diagnostics_.front().selected_for_output = true;
    loop_seam_correction_rejected_ = false;
    return;
  }
  if (!loop_bake_config_ || !loop_bake_config_->seam_fallback_enabled) {
    if (best_preview_index) {
      loop_seam_window_diagnostics_[*best_preview_index].best_preview = true;
      loop_seam_window_diagnostics_[*best_preview_index].selected_for_output =
          true;
    }
    loop_seam_correction_rejected_ = true;
    return;
  }

  std::set<std::string> physics;
  for (const auto &n : bone_mapper_.physicsBones()) {
    physics.insert(n);
  }
  const auto fixed = fixedBones();
  for (double window_ratio : correctionWindowRatios(
           bone_mapper_.config().loop_seam_window_ratio)) {
    checkCancellation();
    profiler_.addCounter(BakeProfiler::Counter::LoopCandidates, 1);
    auto correction = LoopSeamCorrector::correctHierarchyCopy(
        frames_, bone_mapper_.allBones(), source_animation_, physics, fixed,
        bone_mapper_.config().loop_seam_strategy, window_ratio,
        bone_mapper_.config().loop_seam_match_acceleration);
    const CandidateAudit audit = auditLoopCandidate(correction.frames);
    LoopSeamReport candidate_report =
        measureLoopSeam(correction.frames, true,
                        correction.window_duration_seconds,
                        correction.window_ratio, audit);
    const bool accepted = candidate_report.passes(bone_mapper_.config());
    const std::size_t diagnostic_index =
        loop_seam_window_diagnostics_.size();
    loop_seam_window_diagnostics_.push_back(windowDiagnostics(
        candidate_report, audit, correction.window_start_index,
        correction.frames[static_cast<std::size_t>(correction.window_start_index)]
            .time,
        correction.canonicalized_bone_count,
        correction.preserved_driver_bone_count,
        correction.driver_endpoint_conflict_count, accepted));
    const auto &diagnostics =
        loop_seam_window_diagnostics_[diagnostic_index];
    if (diagnostics.valid &&
        (!best_preview_index || diagnostics.score < best_preview_score)) {
      best_preview_index = diagnostic_index;
      best_preview_score = diagnostics.score;
      best_loop_preview_candidate_report_ = candidate_report;
    }
    if (accepted &&
        (!best_safe_export_index || diagnostics.score <
                                        best_safe_export_score)) {
      best_safe_export_index = diagnostic_index;
      best_safe_export_score = diagnostics.score;
      best_loop_safe_export_candidate_report_ = candidate_report;
      best_safe_export_frames = copyFrames(correction.frames);
    }
  }
  if (best_preview_index) {
    loop_seam_window_diagnostics_[*best_preview_index].best_preview = true;
  }
  if (best_safe_export_index) {
    loop_seam_window_diagnostics_[*best_safe_export_index].best_safe_export =
        true;
    loop_seam_window_diagnostics_[*best_safe_export_index].selected_for_output =
        true;
    frames_ = std::move(best_safe_export_frames);
    loop_seam_report_ = *best_loop_safe_export_candidate_report_;
    loop_fallback_used_ = loop_seam_report_->correctionApplied();
    loop_seam_correction_rejected_ = false;
    return;
  }




  loop_seam_window_diagnostics_.front().selected_for_output = true;
  loop_seam_correction_rejected_ = true;
}

LoopSeamReport
PhysicsBaker::measureLoopSeam(const std::vector<BakedFrame> &candidate_frames,
                              bool corrected,
                              double window_duration_seconds,
                              double window_ratio,
                              const CandidateAudit &audit) const {
  std::set<std::string> physics;
  for (const auto &n : bone_mapper_.physicsBones()) {
    physics.insert(n);
  }
  return LoopSeamReport::measure(candidate_frames, bone_mapper_.allBones(),
                                 source_animation_, physics, fixedBones(),
                                 corrected, window_ratio, audit.collision_safe,
                                 audit.maximum_penetration, audit.joint_safe,
                                 audit.valid, window_duration_seconds);
}

PhysicsBaker::CandidateAudit PhysicsBaker::auditLoopCandidate(
    const std::vector<BakedFrame> &candidate_frames) const {
  checkCancellation();
  CandidateAudit result;
  const auto finish = [&]() {
    result.safe = result.valid && result.collision_safe && result.joint_safe;
    return result;
  };
  const auto invalidate = [&](const std::string &bone,
                              const std::string &field, bool missing,
                              bool non_finite) {
    result.valid = false;
    if (missing) {
      ++result.missing_bone_count;
    }
    if (non_finite) {
      ++result.non_finite_value_count;
    }
    if (result.first_invalid_field.empty()) {
      result.first_invalid_bone = bone;
      result.first_invalid_field = field;
    }
  };
  if (candidate_frames.empty()) {
    invalidate({}, "empty_candidate", false, false);
    return finish();
  }

  using ChannelMap = std::map<std::string, std::array<double, 3>>;
  using PoseMap = std::map<std::string, BonePoseCalculator::Pose>;
  struct ExportFrame {
    ChannelMap positions;
    ChannelMap rotations;
    PoseMap poses;
  };
  const auto finiteValues = [](const auto &values) {
    return std::all_of(values.begin(), values.end(),
                       [](double value) { return std::isfinite(value); });
  };
  const auto finitePose = [&](const BonePoseCalculator::Pose &pose) {
    return finiteValues(pose.world_position) &&
           finiteValues(pose.world_rotation);
  };
  const auto recordCollision = [&](const auto &audit, double sample_time) {
    if (!std::isfinite(audit.maximum_penetration)) {
      invalidate({}, "maximum_penetration", false, true);
      result.collision_safe = false;
      return;
    }
    if (audit.maximum_penetration > result.maximum_penetration) {
      result.maximum_penetration = audit.maximum_penetration;
      result.maximum_penetration_time = sample_time;
      result.worst_collision_pair = audit.worst_pair;
    }
    result.collision_safe = result.collision_safe && !audit.unsafe;
  };
  const auto auditPose = [&](const PoseMap &poses, double sample_time,
                             bool interpolated) {
    for (const auto &boneName : bone_mapper_.physicsBones()) {
      const auto pose = poses.find(boneName);
      if (pose == poses.end()) {
        invalidate(boneName, "reconstructed_pose", true, false);
      } else if (!finitePose(pose->second)) {
        invalidate(boneName, "reconstructed_pose", false, true);
      }
    }
    if (!result.valid) {
      if (interpolated && result.interpolation_failure_time < 0.0) {
        result.interpolation_failure_time = sample_time;
      }
      return;
    }
    if (rigid_body_collision_auditor_ != nullptr) {
      const auto audit = rigid_body_collision_auditor_->audit(poses);
      addRigidAuditCounters(profiler_, audit);
      recordCollision(audit, sample_time);
      if (interpolated && audit.unsafe &&
          result.interpolation_failure_time < 0.0) {
        result.interpolation_failure_time = sample_time;
      }
    }
    if (rigid_body_joint_auditor_ != nullptr) {
      const auto audit = rigid_body_joint_auditor_->audit(poses);
      if (audit.unsafe && result.joint_failure_time < 0.0) {
        result.joint_failure_time = sample_time;
      }
      if (interpolated && audit.unsafe &&
          result.interpolation_failure_time < 0.0) {
        result.interpolation_failure_time = sample_time;
      }
      result.joint_safe = result.joint_safe && !audit.unsafe;
    }
  };

  std::set<std::string> bakedBoneNames;
  std::vector<ExportFrame> exportFrames(candidate_frames.size());
  for (std::size_t frameIndex = 0; frameIndex < candidate_frames.size();
       ++frameIndex) {
    checkCancellation();
    const auto &frame = candidate_frames[frameIndex];
    if (!std::isfinite(frame.time) ||
        (frameIndex > 0 &&
         !(frame.time > candidate_frames[frameIndex - 1].time))) {
      invalidate({}, "distinct_finite_frame_time", false, true);
      return finish();
    }
    auto &exportFrame = exportFrames[frameIndex];
    for (const auto &state : frame.bone_states) {
      if (state.bone_name.empty()) {
        invalidate({}, "bone_name", true, false);
        continue;
      }
      if (!finiteValues(state.position) || !finiteValues(state.rotation)) {
        invalidate(state.bone_name, "local_channel", false, true);
        continue;
      }
      bakedBoneNames.insert(state.bone_name);
      auto &position = exportFrame.positions[state.bone_name];
      auto &rotation = exportFrame.rotations[state.bone_name];
      for (std::size_t axis = 0; axis < 3; ++axis) {
        position[axis] = export_::AnimationExporter::quantizeValue(
            state.position[axis]);
        rotation[axis] = export_::AnimationExporter::quantizeValue(
            state.rotation[axis]);
      }
    }
    for (const auto &boneName : bone_mapper_.physicsBones()) {
      if (!exportFrame.positions.contains(boneName) ||
          !exportFrame.rotations.contains(boneName)) {
        invalidate(boneName, "export_channel", true, false);
      }
    }
    if (!result.valid) {
      return finish();
    }
    try {
      exportFrame.poses = calculatePoses(
          source_animation_, frame.time, &exportFrame.positions,
          &exportFrame.rotations);
    } catch (const std::exception &) {
      invalidate({}, "pose_reconstruction", false, false);
      return finish();
    }
    auditPose(exportFrame.poses, frame.time, false);
  }

  if (!result.valid || candidate_frames.size() < 2 ||
      (rigid_body_collision_auditor_ == nullptr &&
       rigid_body_joint_auditor_ == nullptr)) {
    return finish();
  }

  const double minimumFeature =
      rigid_body_collision_auditor_ == nullptr
          ? 0.0
          : rigid_body_collision_auditor_->minimumColliderFeature();
  const bool adaptiveCollisionAudit =
      rigid_body_collision_auditor_ != nullptr && minimumFeature > 0.0;
  constexpr int kMinimumMovingSegmentSamples = 8;
  constexpr int kMaximumSegmentSamples = 256;
  constexpr double kMotionTolerance = 1e-12;
  const double allowedTravel =
      adaptiveCollisionAudit ? std::max(1e-9, minimumFeature * 0.5) : 1.0;
  const double maximumRadius =
      adaptiveCollisionAudit
          ? rigid_body_collision_auditor_->maximumColliderRadius()
          : 0.0;
  const auto vectorDistance = [](const std::array<double, 3> &first,
                                 const std::array<double, 3> &second) {
    const double x = second[0] - first[0];
    const double y = second[1] - first[1];
    const double z = second[2] - first[2];
    return std::sqrt(x * x + y * y + z * z);
  };
  const auto interpolateChannels =
      [&](const ChannelMap &first, const ChannelMap &second, double fraction,
          ChannelMap &output) {
        output.clear();
        for (const auto &[boneName, firstValue] : first) {
          const auto next = second.find(boneName);
          if (next == second.end()) {
            invalidate(boneName, "interpolation_channel", true, false);
            return false;
          }
          auto &value = output[boneName];
          for (std::size_t axis = 0; axis < 3; ++axis) {
            value[axis] = firstValue[axis] +
                          (next->second[axis] - firstValue[axis]) * fraction;
          }
        }
        return true;
      };
  const auto dependencies = animationInputDependencyBones();

  for (std::size_t frameIndex = 0; frameIndex + 1 < candidate_frames.size();
       ++frameIndex) {
    checkCancellation();
    const auto &firstFrame = candidate_frames[frameIndex];
    const auto &secondFrame = candidate_frames[frameIndex + 1];
    const auto &firstExport = exportFrames[frameIndex];
    const auto &secondExport = exportFrames[frameIndex + 1];
    const double timeSpan = secondFrame.time - firstFrame.time;
    if (!std::isfinite(timeSpan) || !(timeSpan > 0.0)) {
      invalidate({}, "interpolation_time_span", false, true);
      break;
    }

    double maximumChannelMotion = 0.0;
    bool hasChannelInterpolation = false;
    for (const auto &[boneName, firstPosition] : firstExport.positions) {
      const auto secondPosition = secondExport.positions.find(boneName);
      const auto firstRotation = firstExport.rotations.find(boneName);
      const auto secondRotation = secondExport.rotations.find(boneName);
      if (secondPosition == secondExport.positions.end() ||
          firstRotation == firstExport.rotations.end() ||
          secondRotation == secondExport.rotations.end()) {
        invalidate(boneName, "interpolation_channel", true, false);
        break;
      }
      const double linear = vectorDistance(firstPosition, secondPosition->second);
      const double angular =
          vectorDistance(firstRotation->second, secondRotation->second) *
          std::numbers::pi_v<double> / 180.0;
      hasChannelInterpolation = hasChannelInterpolation ||
                                linear > kMotionTolerance ||
                                angular > kMotionTolerance;
      maximumChannelMotion =
          std::max(maximumChannelMotion, linear + angular * maximumRadius);
    }
    if (!result.valid) {
      break;
    }

    std::set<double> requiredFractions{0.0, 1.0};
    bool authoredReferenceNeedsSampling = false;
    if (source_animation_ != nullptr) {
      const auto appendChannel =
          [&](const loader::Keyframes &channel, bool angular) {
            const auto firstValue = channel.evaluate(firstFrame.time);
            const auto secondValue = channel.evaluate(secondFrame.time);
            const double distance = vectorDistance(firstValue, secondValue);
            hasChannelInterpolation =
                hasChannelInterpolation || distance > kMotionTolerance;
            maximumChannelMotion =
                std::max(maximumChannelMotion,
                         angular ? distance * std::numbers::pi_v<double> /
                                           180.0 * maximumRadius
                                 : distance);
            for (const auto &[keyTime, value] : channel.keyframes) {
              (void)value;
              if (keyTime <= firstFrame.time + 1e-12 ||
                  keyTime >= secondFrame.time - 1e-12) {
                continue;
              }
              authoredReferenceNeedsSampling = true;
              requiredFractions.insert(
                  (keyTime - firstFrame.time) / timeSpan);
              const double beforeDelta =
                  std::min((keyTime - firstFrame.time) * 0.5,
                           std::max(1e-10, timeSpan * 1e-7));
              if (beforeDelta > 1e-12) {
                requiredFractions.insert(
                    (keyTime - beforeDelta - firstFrame.time) / timeSpan);
              }
            }
            for (const auto &[keyTime, value] : channel.pre_keyframes) {
              (void)value;
              if (keyTime > firstFrame.time + 1e-12 &&
                  keyTime < secondFrame.time - 1e-12) {
                authoredReferenceNeedsSampling = true;
              }
            }
            for (const auto &[keyTime, mode] :
                 channel.interpolation_modes) {
              if (mode == loader::Keyframes::InterpolationMode::CatmullRom &&
                  keyTime <= secondFrame.time + 1e-12) {
                authoredReferenceNeedsSampling = true;
              }
            }
          };
      for (const auto &boneName : dependencies) {
        if (bakedBoneNames.contains(boneName)) {
          continue;
        }
        const auto channel = source_animation_->bones.find(boneName);
        if (channel == source_animation_->bones.end()) {
          continue;
        }
        if (channel->second.has_position) {
          appendChannel(channel->second.position, false);
        }
        if (channel->second.has_rotation) {
          appendChannel(channel->second.rotation, true);
        }
      }
    }

    const double endpointTravel =
        adaptiveCollisionAudit
            ? rigid_body_collision_auditor_->maximumVertexTravel(
                  firstExport.poses, secondExport.poses)
            : 0.0;
    const double estimatedRelativeTravel =
        std::max(endpointTravel, maximumChannelMotion * 2.0);
    if (!std::isfinite(estimatedRelativeTravel)) {
      invalidate({}, "estimated_interpolation_travel", false, true);
      break;
    }
    if (estimatedRelativeTravel <= kMotionTolerance &&
        !hasChannelInterpolation && !authoredReferenceNeedsSampling &&
        requiredFractions.size() == 2) {
      continue;
    }
    int subdivisions = kMinimumMovingSegmentSamples;
    if (adaptiveCollisionAudit) {
      subdivisions = std::max(
          subdivisions,
          static_cast<int>(std::ceil(estimatedRelativeTravel / allowedTravel)));
    }
    if (subdivisions > kMaximumSegmentSamples) {
      result.collision_safe = false;
      invalidate({}, "interpolation_sampling_limit", false, false);
      result.interpolation_failure_time = firstFrame.time;
      break;
    }

    bool segmentUnsafe = false;
    while (true) {
      checkCancellation();
      std::set<double> sampleFractions = requiredFractions;
      for (int sample = 1; sample < subdivisions; ++sample) {
        sampleFractions.insert(static_cast<double>(sample) / subdivisions);
      }
      double maximumSampleTravel = 0.0;
      PoseMap previousPose = firstExport.poses;
      for (double fraction : sampleFractions) {
        checkCancellation();
        if (!(fraction > 0.0) || !(fraction < 1.0)) {
          continue;
        }
        const double sampleTime = firstFrame.time + timeSpan * fraction;
        ChannelMap positions;
        ChannelMap rotations;
        if (!interpolateChannels(firstExport.positions,
                                 secondExport.positions, fraction,
                                 positions) ||
            !interpolateChannels(firstExport.rotations,
                                 secondExport.rotations, fraction,
                                 rotations)) {
          if (result.interpolation_failure_time < 0.0) {
            result.interpolation_failure_time = sampleTime;
          }
          segmentUnsafe = true;
          break;
        }
        PoseMap pose;
        try {
          pose = calculatePoses(source_animation_, sampleTime,
                                &positions, &rotations);
        } catch (const std::exception &) {
          invalidate({}, "interpolated_pose_reconstruction", false, false);
          if (result.interpolation_failure_time < 0.0) {
            result.interpolation_failure_time = sampleTime;
          }
          segmentUnsafe = true;
          break;
        }
        ++result.interpolated_sample_count;
        if (adaptiveCollisionAudit) {
          const double travel =
              rigid_body_collision_auditor_->maximumVertexTravel(previousPose,
                                                                  pose);
          if (!std::isfinite(travel)) {
            invalidate({}, "interpolated_vertex_travel", false, true);
            if (result.interpolation_failure_time < 0.0) {
              result.interpolation_failure_time = sampleTime;
            }
            segmentUnsafe = true;
            break;
          }
          maximumSampleTravel = std::max(maximumSampleTravel, travel);
        }
        previousPose = pose;
        auditPose(pose, sampleTime, true);
        if (!result.valid || !result.collision_safe || !result.joint_safe) {
          segmentUnsafe = true;
          break;
        }
      }
      if (adaptiveCollisionAudit && !segmentUnsafe) {
        maximumSampleTravel = std::max(
            maximumSampleTravel,
            rigid_body_collision_auditor_->maximumVertexTravel(
                previousPose, secondExport.poses));
      }
      if (segmentUnsafe || !adaptiveCollisionAudit ||
          maximumSampleTravel <= allowedTravel) {
        break;
      }
      if (subdivisions >= kMaximumSegmentSamples) {
        result.collision_safe = false;
        invalidate({}, "interpolation_sampling_limit", false, false);
        if (result.interpolation_failure_time < 0.0) {
          result.interpolation_failure_time = firstFrame.time;
        }
        break;
      }
      subdivisions = std::min(kMaximumSegmentSamples, subdivisions * 2);
    }
    if (!result.valid || !result.collision_safe || !result.joint_safe) {
      break;
    }
  }
  return finish();
}

std::vector<double>
PhysicsBaker::correctionWindowRatios(double requested_ratio) {
  const double initial =
      std::clamp(std::isfinite(requested_ratio) ? requested_ratio : 0.25,
                 0.0, 0.5);
  std::vector<double> ratios;
  if (initial > 0.0) {
    ratios.push_back(initial);
  }
  if (initial < 0.375) {
    ratios.push_back(0.375);
  }
  if (initial < 0.5) {
    ratios.push_back(0.5);
  }
  return ratios;
}

void PhysicsBaker::unwrapFinalRotations() {
  auto unwrap_scope = profiler_.scope(BakeProfiler::Stage::Unwrap);
  std::map<std::string, std::array<double, 3>> previousByBone;
  for (auto &frame : frames_) {
    checkCancellation();
    for (auto &state : frame.bone_states) {
      auto prev = previousByBone.find(state.bone_name);
      if (prev != previousByBone.end()) {
        state.rotation =
            RotationUtil::unwrapEuler(prev->second, state.rotation);
      }
      previousByBone[state.bone_name] = state.rotation;
    }
  }
}

void PhysicsBaker::blendTransitionFramesToMovingTarget() {
  if (!active_transition_.has_value()) {
    return;
  }


  if (transition_controller_ && frames_.size() >= 2) {
    const double duration = active_transition_->transition_duration;
    for (auto &frame : frames_) {
      checkCancellation();
      const double weight = smootherStep(frame.time / duration);
      if (weight <= 0.0) {
        continue;
      }
      const auto referencePoses = transition_controller_->sample(frame.time);
      for (auto &state : frame.bone_states) {
        auto boneIt = bones_by_name_.find(state.bone_name);
        auto refIt = referencePoses.find(state.bone_name);
        if (boneIt == bones_by_name_.end() || refIt == referencePoses.end()) {
          continue;
        }
        for (int axis = 0; axis < 3; ++axis) {
          state.position[static_cast<std::size_t>(axis)] +=
              weight *
              (refIt->second.animation_position[static_cast<std::size_t>(axis)] -
               state.position[static_cast<std::size_t>(axis)]);
        }
        const std::array<double, 3> physicalTotal{
            boneIt->second.rotation[0] + state.rotation[0],
            boneIt->second.rotation[1] + state.rotation[1],
            boneIt->second.rotation[2] + state.rotation[2]};
        auto fromQ = RotationUtil::quaternionFromBedrockEuler(
            physicalTotal[0], physicalTotal[1], physicalTotal[2]);
        auto toQ = RotationUtil::quaternionFromBedrockEuler(
            refIt->second.total_local_euler[0],
            refIt->second.total_local_euler[1],
            refIt->second.total_local_euler[2]);
        auto delta = RotationUtil::rotationVectorFromQuaternion(
            RotationUtil::quaternionMultiply(
                toQ, RotationUtil::quaternionInverse(fromQ)));
        for (int axis = 0; axis < 3; ++axis) {
          delta[static_cast<std::size_t>(axis)] *= weight;
        }
        const auto blendedQ = RotationUtil::quaternionMultiply(
            RotationUtil::quaternionFromRotationVector(delta), fromQ);
        const auto blendedTotal = RotationUtil::unwrapEuler(
            physicalTotal, RotationUtil::bedrockEulerFromQuaternion(blendedQ));
        for (int axis = 0; axis < 3; ++axis) {
          state.rotation[static_cast<std::size_t>(axis)] =
              blendedTotal[static_cast<std::size_t>(axis)] -
              boneIt->second.rotation[axis];
        }
      }
    }
  }
}

void PhysicsBaker::blendOrdinaryFramesToReferenceEdges() {
  auto blend_scope = profiler_.scope(BakeProfiler::Stage::Blend);
  const double requested = bone_mapper_.config().transition_duration;



  if (source_animation_ == nullptr || hasPeriodicLoop() ||
      !std::isfinite(requested) || requested <= 0.0 || frames_.size() < 2) {
    return;
  }
  const double length = frames_.back().time;
  if (!(length > 0.0)) {
    return;
  }
  const double duration = std::min(requested, length * 0.5);
  for (auto &frame : frames_) {
    checkCancellation();
    const double edgeDistance = std::min(frame.time, length - frame.time);
    if (edgeDistance >= duration) {
      continue;
    }
    const double referenceWeight = 1.0 - smootherStep(edgeDistance / duration);
    const double referenceTime =
        frame.time > length * 0.5
            ? std::max(0.0, source_animation_->animation_length)
            : 0.0;
    const auto referencePoses =
        calculatePoses(source_animation_, referenceTime);
    for (auto &state : frame.bone_states) {
      auto boneIt = bones_by_name_.find(state.bone_name);
      auto refIt = referencePoses.find(state.bone_name);
      if (boneIt == bones_by_name_.end() || refIt == referencePoses.end()) {
        continue;
      }
      for (int axis = 0; axis < 3; ++axis) {
        state.position[static_cast<std::size_t>(axis)] +=
            referenceWeight *
            (refIt->second.animation_position[static_cast<std::size_t>(axis)] -
             state.position[static_cast<std::size_t>(axis)]);
      }
      const std::array<double, 3> physicalTotal{
          boneIt->second.rotation[0] + state.rotation[0],
          boneIt->second.rotation[1] + state.rotation[1],
          boneIt->second.rotation[2] + state.rotation[2]};
      auto fromQ = RotationUtil::quaternionFromBedrockEuler(
          physicalTotal[0], physicalTotal[1], physicalTotal[2]);
      auto toQ = RotationUtil::quaternionFromBedrockEuler(
          refIt->second.total_local_euler[0],
          refIt->second.total_local_euler[1],
          refIt->second.total_local_euler[2]);
      auto delta = RotationUtil::rotationVectorFromQuaternion(
          RotationUtil::quaternionMultiply(
              toQ, RotationUtil::quaternionInverse(fromQ)));
      for (int axis = 0; axis < 3; ++axis) {
        delta[static_cast<std::size_t>(axis)] *= referenceWeight;
      }
      const auto blendedQ = RotationUtil::quaternionMultiply(
          RotationUtil::quaternionFromRotationVector(delta), fromQ);
      const auto blendedTotal = RotationUtil::unwrapEuler(
          physicalTotal, RotationUtil::bedrockEulerFromQuaternion(blendedQ));
      for (int axis = 0; axis < 3; ++axis) {
        state.rotation[static_cast<std::size_t>(axis)] =
            blendedTotal[static_cast<std::size_t>(axis)] -
            boneIt->second.rotation[axis];
      }
      for (int axis = 0; axis < 3; ++axis) {
        state.linear_velocity[static_cast<std::size_t>(axis)] *=
            (1.0 - referenceWeight);
      }
    }
  }
}

void PhysicsBaker::rebuildFinalWorldPositionsAndAudit() {
  checkCancellation();
  auto audit_scope = profiler_.scope(BakeProfiler::Stage::FinalCollisionAudit);



  unsafe_final_collision_count_ = 0;
  unsafe_final_joint_count_ = 0;
  maximum_final_rigid_body_penetration_ = 0.0;
  worst_final_collision_pair_.reset();
  maximum_final_joint_anchor_separation_ = 0.0;
  maximum_final_joint_angular_excess_radians_ = 0.0;
  worst_final_joint_linear_parent_.clear();
  worst_final_joint_linear_child_.clear();
  worst_final_joint_angular_parent_.clear();
  worst_final_joint_angular_child_.clear();
  worst_final_joint_angular_axis_ = -1;
  first_final_joint_euler_singularity_.reset();
  if (frames_.empty()) {
    return;
  }

  const auto recordCollisionAudit = [&](const auto &audit) {
    if (audit.maximum_penetration >
        maximum_final_rigid_body_penetration_) {
      maximum_final_rigid_body_penetration_ = audit.maximum_penetration;
      worst_final_collision_pair_ = audit.worst_pair;
    }
  };
  const auto recordJointAudit = [&](const auto &audit) {
    if (!first_final_joint_euler_singularity_) {
      const auto singular = std::find_if(
          audit.violations.begin(), audit.violations.end(),
          [](const RigidBodyJointAuditor::Violation &violation) {
            return violation.joint_euler_singular;
          });
      if (singular != audit.violations.end()) {
        first_final_joint_euler_singularity_ = *singular;
      }
    }
    if (audit.maximum_linear_anchor_separation >
        maximum_final_joint_anchor_separation_) {
      maximum_final_joint_anchor_separation_ =
          audit.maximum_linear_anchor_separation;
      worst_final_joint_linear_parent_ = audit.worst_linear_parent;
      worst_final_joint_linear_child_ = audit.worst_linear_child;
    }
    if (audit.maximum_angular_limit_excess_radians >
        maximum_final_joint_angular_excess_radians_) {
      maximum_final_joint_angular_excess_radians_ =
          audit.maximum_angular_limit_excess_radians;
      worst_final_joint_angular_parent_ = audit.worst_angular_parent;
      worst_final_joint_angular_child_ = audit.worst_angular_child;
      worst_final_joint_angular_axis_ = audit.worst_angular_axis;
    }
  };

  using ChannelMap = std::map<std::string, std::array<double, 3>>;
  using PoseMap = std::map<std::string, BonePoseCalculator::Pose>;
  struct ExportFrame {
    ChannelMap positions;
    ChannelMap rotations;
    PoseMap poses;
  };

  std::set<std::string> bakedBoneNames;
  for (const auto &frame : frames_) {
    checkCancellation();
    for (const auto &state : frame.bone_states) {
      bakedBoneNames.insert(state.bone_name);
    }
  }
  const auto dependencies = animationInputDependencyBones();
  const loader::Animation *referenceAnimation =
      active_transition_.has_value() ? active_transition_->target_animation
                                     : source_animation_;
  std::vector<ExportFrame> exportFrames(frames_.size());
  std::map<std::string, std::array<double, 3>> previousReferenceRotations;

  for (std::size_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex) {
    checkCancellation();
    auto &frame = frames_[frameIndex];
    auto &exportFrame = exportFrames[frameIndex];

    if (active_transition_.has_value()) {
      const auto referencePoses = sampleOutputReferencePoses(frame.time);
      for (const auto &boneName : dependencies) {
        if (bakedBoneNames.contains(boneName)) {
          continue;
        }
        const auto pose = referencePoses.find(boneName);
        const auto modelBone = bones_by_name_.find(boneName);
        if (pose == referencePoses.end() || modelBone == bones_by_name_.end()) {
          throw std::logic_error(
              "transition export reference is missing dependency bone: " +
              boneName);
        }
        std::array<double, 3> localRotation{
            pose->second.total_local_euler[0] -
                modelBone->second.rotation[0],
            pose->second.total_local_euler[1] -
                modelBone->second.rotation[1],
            pose->second.total_local_euler[2] -
                modelBone->second.rotation[2]};
        const auto previous = previousReferenceRotations.find(boneName);
        if (previous != previousReferenceRotations.end()) {
          localRotation =
              RotationUtil::unwrapEuler(previous->second, localRotation);
        }
        previousReferenceRotations[boneName] = localRotation;
        for (std::size_t axis = 0; axis < 3; ++axis) {
          exportFrame.positions[boneName][axis] =
              export_::AnimationExporter::quantizeValue(
                  pose->second.animation_position[axis]);
          exportFrame.rotations[boneName][axis] =
              export_::AnimationExporter::quantizeValue(localRotation[axis]);
        }
      }
    }

    for (const auto &state : frame.bone_states) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        exportFrame.positions[state.bone_name][axis] =
            export_::AnimationExporter::quantizeValue(state.position[axis]);
        exportFrame.rotations[state.bone_name][axis] =
            export_::AnimationExporter::quantizeValue(state.rotation[axis]);
      }
    }
    exportFrame.poses =
        calculatePoses(referenceAnimation, frame.time, &exportFrame.positions,
                       &exportFrame.rotations);

    if (body_collider_cache_ != nullptr) {
      body_collider_cache_->setAuditPose(exportFrame.poses);
    }
    for (auto &state : frame.bone_states) {
      auto pose = exportFrame.poses.find(state.bone_name);
      if (pose == exportFrame.poses.end()) {
        continue;
      }
      state.world_position = pose->second.world_position;
      state.has_world_position = true;
      if (ground_collision_constraint_ != nullptr &&
          !bone_mapper_.isFixedBone(state.bone_name) &&
          pose->second.world_position[1] <
              ground_collision_constraint_->minimumY() - 1e-9) {
        unsafe_final_collision_count_++;
      }
      if (body_collision_constraint_ != nullptr &&
          body_collider_cache_ != nullptr &&
          !bone_mapper_.isFixedBone(state.bone_name) &&
          body_collider_cache_->containsCurrent(
              pose->second.world_position[0],
              pose->second.world_position[1],
              pose->second.world_position[2],
              body_collision_constraint_->skin())) {
        unsafe_final_collision_count_++;
      }
    }
    if (rigid_body_collision_auditor_ != nullptr) {
      const auto audit =
          rigid_body_collision_auditor_->audit(exportFrame.poses);
      addRigidAuditCounters(profiler_, audit);
      recordCollisionAudit(audit);
      if (audit.unsafe) {
        unsafe_final_collision_count_++;
      }
    }
    if (rigid_body_joint_auditor_ != nullptr) {
      const auto audit = rigid_body_joint_auditor_->audit(exportFrame.poses);
      recordJointAudit(audit);
      if (audit.unsafe) {
        unsafe_final_joint_count_++;
      }
    }
  }

  if (frames_.size() < 2 ||
      (rigid_body_collision_auditor_ == nullptr &&
       rigid_body_joint_auditor_ == nullptr)) {
    return;
  }
  const double minimumFeature =
      rigid_body_collision_auditor_ == nullptr
          ? 0.0
          : rigid_body_collision_auditor_->minimumColliderFeature();
  const bool adaptiveCollisionAudit =
      rigid_body_collision_auditor_ != nullptr && minimumFeature > 0.0;

  constexpr int kMinimumMovingSegmentSamples = 8;
  constexpr int kMaximumSegmentSamples = 256;
  constexpr double kMotionTolerance = 1e-12;
  const double allowedTravel =
      adaptiveCollisionAudit ? std::max(1e-9, minimumFeature * 0.5) : 1.0;
  const double maximumRadius =
      adaptiveCollisionAudit
          ? rigid_body_collision_auditor_->maximumColliderRadius()
          : 0.0;
  const auto vectorDistance = [](const std::array<double, 3> &first,
                                 const std::array<double, 3> &second) {
    const double x = second[0] - first[0];
    const double y = second[1] - first[1];
    const double z = second[2] - first[2];
    return std::sqrt(x * x + y * y + z * z);
  };
  const auto interpolateChannels =
      [](const ChannelMap &first, const ChannelMap &second, double fraction) {
        ChannelMap result;
        for (const auto &[boneName, firstValue] : first) {
          const auto next = second.find(boneName);
          if (next == second.end()) {
            throw std::logic_error(
                "final export frames have inconsistent bone channels");
          }
          auto &value = result[boneName];
          for (std::size_t axis = 0; axis < 3; ++axis) {
            value[axis] = firstValue[axis] +
                          (next->second[axis] - firstValue[axis]) * fraction;
          }
        }
        return result;
      };





  for (std::size_t frameIndex = 0; frameIndex + 1 < frames_.size();
       ++frameIndex) {
    checkCancellation();
    const auto &firstFrame = frames_[frameIndex];
    const auto &secondFrame = frames_[frameIndex + 1];
    const auto &firstExport = exportFrames[frameIndex];
    const auto &secondExport = exportFrames[frameIndex + 1];
    const double timeSpan = secondFrame.time - firstFrame.time;
    if (!std::isfinite(timeSpan) || !(timeSpan > 0.0)) {
      continue;
    }

    double maximumChannelMotion = 0.0;
    bool hasChannelInterpolation = false;
    for (const auto &[boneName, firstPosition] : firstExport.positions) {
      const auto secondPosition = secondExport.positions.find(boneName);
      const auto firstRotation = firstExport.rotations.find(boneName);
      const auto secondRotation = secondExport.rotations.find(boneName);
      if (secondPosition == secondExport.positions.end() ||
          firstRotation == firstExport.rotations.end() ||
          secondRotation == secondExport.rotations.end()) {
        throw std::logic_error(
            "final export frames have inconsistent bone channels");
      }
      const double linear =
          vectorDistance(firstPosition, secondPosition->second);
      const double angular =
          vectorDistance(firstRotation->second, secondRotation->second) *
          std::numbers::pi_v<double> / 180.0;
      hasChannelInterpolation =
          hasChannelInterpolation || linear > kMotionTolerance ||
          angular > kMotionTolerance;
      maximumChannelMotion =
          std::max(maximumChannelMotion, linear + angular * maximumRadius);
    }

    std::set<double> requiredFractions{0.0, 1.0};
    bool authoredReferenceNeedsSampling = false;
    if (!active_transition_.has_value() && referenceAnimation != nullptr) {
      const auto appendChannel =
          [&](const loader::Keyframes &channel, bool angular) {
            const auto firstValue = channel.evaluate(firstFrame.time);
            const auto secondValue = channel.evaluate(secondFrame.time);
            const double distance = vectorDistance(firstValue, secondValue);
            hasChannelInterpolation =
                hasChannelInterpolation || distance > kMotionTolerance;
            maximumChannelMotion =
                std::max(maximumChannelMotion,
                         angular ? distance * std::numbers::pi_v<double> /
                                           180.0 * maximumRadius
                                 : distance);
            for (const auto &[keyTime, value] : channel.keyframes) {
              (void)value;
              if (keyTime <= firstFrame.time + 1e-12 ||
                  keyTime >= secondFrame.time - 1e-12) {
                continue;
              }
              authoredReferenceNeedsSampling = true;
              requiredFractions.insert(
                  (keyTime - firstFrame.time) / timeSpan);
              const double beforeDelta =
                  std::min((keyTime - firstFrame.time) * 0.5,
                           std::max(1e-10, timeSpan * 1e-7));
              if (beforeDelta > 1e-12) {
                requiredFractions.insert(
                    (keyTime - beforeDelta - firstFrame.time) / timeSpan);
              }
            }
            for (const auto &[keyTime, value] : channel.pre_keyframes) {
              (void)value;
              if (keyTime > firstFrame.time + 1e-12 &&
                  keyTime < secondFrame.time - 1e-12) {
                authoredReferenceNeedsSampling = true;
              }
            }
            for (const auto &[keyTime, mode] :
                 channel.interpolation_modes) {
              if (mode == loader::Keyframes::InterpolationMode::CatmullRom &&
                  keyTime <= secondFrame.time + 1e-12) {
                authoredReferenceNeedsSampling = true;
              }
            }
          };
      for (const auto &boneName : dependencies) {
        if (bakedBoneNames.contains(boneName)) {
          continue;
        }
        const auto channel = referenceAnimation->bones.find(boneName);
        if (channel == referenceAnimation->bones.end()) {
          continue;
        }
        if (channel->second.has_position) {
          appendChannel(channel->second.position, false);
        }
        if (channel->second.has_rotation) {
          appendChannel(channel->second.rotation, true);
        }
      }
    }

    const double endpointTravel =
        adaptiveCollisionAudit
            ? rigid_body_collision_auditor_->maximumVertexTravel(
                  firstExport.poses, secondExport.poses)
            : 0.0;
    const double estimatedRelativeTravel =
        std::max(endpointTravel, maximumChannelMotion * 2.0);
    if (estimatedRelativeTravel <= kMotionTolerance &&
        !hasChannelInterpolation && !authoredReferenceNeedsSampling &&
        requiredFractions.size() == 2) {
      continue;
    }
    int subdivisions = kMinimumMovingSegmentSamples;
    if (adaptiveCollisionAudit) {
      subdivisions = std::max(
          subdivisions,
          static_cast<int>(
              std::ceil(estimatedRelativeTravel / allowedTravel)));
    }
    if (subdivisions > kMaximumSegmentSamples) {
      unsafe_final_collision_count_++;
      continue;
    }

    const auto samplePose = [&](double fraction) {
      if (fraction <= 0.0) {
        return firstExport.poses;
      }
      if (fraction >= 1.0) {
        return secondExport.poses;
      }
      auto positions = interpolateChannels(
          firstExport.positions, secondExport.positions, fraction);
      auto rotations = interpolateChannels(
          firstExport.rotations, secondExport.rotations, fraction);
      return calculatePoses(
          referenceAnimation, firstFrame.time + timeSpan * fraction,
          &positions, &rotations);
    };

    bool segmentCollisionUnsafe = false;
    bool segmentJointUnsafe = false;
    while (true) {
      checkCancellation();
      std::set<double> sampleFractions = requiredFractions;
      for (int sample = 1; sample < subdivisions; ++sample) {
        sampleFractions.insert(static_cast<double>(sample) / subdivisions);
      }

      double maximumSampleTravel = 0.0;
      PoseMap previousPose = firstExport.poses;
      for (const double fraction : sampleFractions) {
        checkCancellation();
        if (!(fraction > 0.0) || !(fraction < 1.0)) {
          continue;
        }
        PoseMap pose = samplePose(fraction);
        if (adaptiveCollisionAudit) {
          maximumSampleTravel =
              std::max(maximumSampleTravel,
                       rigid_body_collision_auditor_->maximumVertexTravel(
                           previousPose, pose));
        }
        previousPose = pose;
        if (rigid_body_collision_auditor_ != nullptr) {
          const auto audit = rigid_body_collision_auditor_->audit(pose);
          addRigidAuditCounters(profiler_, audit);
          recordCollisionAudit(audit);
          segmentCollisionUnsafe = audit.unsafe;
        }
        if (rigid_body_joint_auditor_ != nullptr) {
          const auto audit = rigid_body_joint_auditor_->audit(pose);
          recordJointAudit(audit);
          segmentJointUnsafe = audit.unsafe;
        }
        if (segmentCollisionUnsafe || segmentJointUnsafe) {
          break;
        }
      }
      if (adaptiveCollisionAudit) {
        maximumSampleTravel =
            std::max(maximumSampleTravel,
                     rigid_body_collision_auditor_->maximumVertexTravel(
                         previousPose, secondExport.poses));
      }
      if (segmentCollisionUnsafe || segmentJointUnsafe ||
          !adaptiveCollisionAudit || maximumSampleTravel <= allowedTravel) {
        break;
      }
      if (subdivisions >= kMaximumSegmentSamples) {
        segmentCollisionUnsafe = true;
        break;
      }
      subdivisions =
          std::min(kMaximumSegmentSamples, subdivisions * 2);
    }
    if (segmentCollisionUnsafe) {
      unsafe_final_collision_count_++;
    }
    if (segmentJointUnsafe) {
      unsafe_final_joint_count_++;
    }
  }
}

void PhysicsBaker::recomputeFinalLinearVelocities() {
  checkCancellation();
  auto velocity_scope = profiler_.scope(BakeProfiler::Stage::Velocity);
  if (frames_.empty()) {
    return;
  }
  if (frames_.size() == 1) {
    for (auto &state : frames_.front().bone_states) {
      state.linear_velocity = {0.0, 0.0, 0.0};
    }
    return;
  }

  const bool periodic = hasPeriodicLoop() && frames_.size() >= 3;
  const double period =
      periodic ? frames_.back().time - frames_.front().time : 0.0;
  const std::size_t last = frames_.size() - 1;
  for (std::size_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex) {
    checkCancellation();
    for (auto &state : frames_[frameIndex].bone_states) {
      const BoneState *before = nullptr;
      const BoneState *after = nullptr;
      double beforeTime = 0.0;
      double afterTime = 0.0;
      if (periodic && std::isfinite(period) && period > 1e-12 &&
          (frameIndex == 0 || frameIndex == last)) {
        before = frames_[last - 1].getBoneState(state.bone_name);
        after = frames_[1].getBoneState(state.bone_name);
        beforeTime = frames_[last - 1].time - period;
        afterTime = frames_[1].time;
      } else if (frameIndex == 0) {
        before = &state;
        after = frames_[1].getBoneState(state.bone_name);
        beforeTime = frames_[0].time;
        afterTime = frames_[1].time;
      } else if (frameIndex == last) {
        before = frames_[last - 1].getBoneState(state.bone_name);
        after = &state;
        beforeTime = frames_[last - 1].time;
        afterTime = frames_[last].time;
      } else {
        before = frames_[frameIndex - 1].getBoneState(state.bone_name);
        after = frames_[frameIndex + 1].getBoneState(state.bone_name);
        beforeTime = frames_[frameIndex - 1].time;
        afterTime = frames_[frameIndex + 1].time;
      }

      const double span = afterTime - beforeTime;
      if (before == nullptr || after == nullptr || !std::isfinite(span) ||
          !(span > 1e-12)) {
        state.linear_velocity = {0.0, 0.0, 0.0};
        continue;
      }
      for (std::size_t axis = 0; axis < state.linear_velocity.size(); ++axis) {
        const double velocity =
            (after->world_position[axis] - before->world_position[axis]) / span;
        state.linear_velocity[axis] = std::isfinite(velocity) ? velocity : 0.0;
      }
    }
  }
}

void PhysicsBaker::requireSafeForExport() const {
  if (!frames_finalized_ || current_step_ < total_steps_) {
    throw std::logic_error(
        "the bake must be complete and finalized before export");
  }
  if (loop_preview_only_cycle_used_) {
    throw std::logic_error(
        "loop cycle safety gate rejected export");
  }
  const bool bullet_safety_applicable =
      bone_mapper_.config().simulation_mode ==
      BoneMapper::SimulationMode::RigidBody;
  if (bullet_safety_applicable && unsafe_final_collision_count_ > 0) {
    throw std::logic_error("final collision audit rejected the bake");
  }
  if (bullet_safety_applicable && unsafe_final_joint_count_ > 0) {
    throw std::logic_error("final joint audit rejected the bake");
  }
  if (loop_seam_correction_rejected_) {
    if (loop_seam_report_) {
      const auto gate = loop_seam_report_->exportGate(bone_mapper_.config());
      if (!gate.validation_pass) {
        throw std::logic_error("loop candidate validation gate rejected export");
      }
      if (!gate.physics_seam_pass) {
        throw std::logic_error("physics seam gate rejected export");
      }
      if (!gate.driver_seam_pass) {
        throw std::logic_error("driver seam gate rejected export");
      }
      if (!gate.quantization_pass) {
        throw std::logic_error("quantization gate rejected export");
      }
      if (!gate.collision_pass) {
        throw std::logic_error("loop collision gate rejected export");
      }
      if (!gate.joint_pass) {
        throw std::logic_error("loop joint gate rejected export");
      }
    }
    throw std::logic_error("loop export gate rejected the bake");
  }
}

void PhysicsBaker::reset() {
  close();
  frames_.clear();
  loop_cycle_frames_.clear();
  best_loop_preview_cycle_frames_.clear();
  best_loop_safe_export_cycle_frames_.clear();
  current_step_ = 0;
  total_steps_ = 0;
  current_sample_time_ = 0.0;
  frames_finalized_ = false;
  initialized_ = false;
  engine_.clear();
  owned_particles_.clear();
  owned_constraints_.clear();
  animation_targets_.clear();
  collision_particles_.clear();
  ground_collision_constraint_ = nullptr;
  body_collision_constraint_ = nullptr;
  body_collider_cache_.reset();
  rigid_body_session_.reset();
  loop_controller_.reset();
  xpbd_periodic_state_tracker_.clear();
  loop_bake_config_.reset();
  loop_error_report_.reset();
  best_loop_preview_cycle_candidate_.reset();
  best_loop_safe_export_cycle_candidate_.reset();
  loop_cycle_candidates_.clear();
  loop_preview_only_cycle_used_ = false;
  loop_seam_report_.reset();
  best_loop_preview_candidate_report_.reset();
  best_loop_safe_export_candidate_report_.reset();
  loop_seam_window_diagnostics_.clear();
  loop_converged_ = false;
  loop_fallback_used_ = false;
  loop_seam_correction_rejected_ = false;
  completed_loop_cycles_ = 0;
  transition_controller_.reset();
  active_transition_.reset();
  transition_started_ = false;
  transition_pre_roll_steps_ = 0;
  transition_steps_ = 0;
  cycle_dt_ = dt_;
  unsafe_final_collision_count_ = 0;
  unsafe_final_joint_count_ = 0;
  maximum_final_rigid_body_penetration_ = 0.0;
  worst_final_collision_pair_.reset();
  maximum_final_joint_anchor_separation_ = 0.0;
  maximum_final_joint_angular_excess_radians_ = 0.0;
  worst_final_joint_linear_parent_.clear();
  worst_final_joint_linear_child_.clear();
  worst_final_joint_angular_parent_.clear();
  worst_final_joint_angular_child_.clear();
  worst_final_joint_angular_axis_ = -1;
  first_final_joint_euler_singularity_.reset();
}

void PhysicsBaker::close() {
  total_bake_scope_.reset();
  initialized_ = false;
  body_collision_constraint_ = nullptr;
  body_collider_cache_.reset();
  rigid_body_session_.reset();
  rigid_body_output_scratch_.clear();
  rigid_body_collision_auditor_.reset();
  rigid_body_joint_auditor_.reset();
  final_pose_reconstructor_evaluator_.reset();
  final_pose_reconstruction_scratch_.reset();
  xpbd_periodic_state_tracker_.clear();
  engine_.clear();
}

std::set<std::string> PhysicsBaker::animationInputDependencyBones() const {
  return bone_mapper_.animationInputDependencyBones();
}

void PhysicsBaker::validateAnimationInputTransforms(
    const loader::Animation *animation, const std::string &animation_role,
    const std::set<std::string> &dependency_bones) const {
  if (animation == nullptr) {
    return;
  }

  const auto rejectMolang = [&animation_role](const std::string &bone_name,
                                              const std::string &channel,
                                              const std::string &reason) {
    throw std::invalid_argument(animation_role + " bone '" + bone_name + "' " +
                                channel + " " + reason);
  };
  const bool rigid_body_mode =
      bone_mapper_.config().simulation_mode == BoneMapper::SimulationMode::RigidBody;
  const std::set<std::string> rigid_geometry_bones =
      rigid_body_mode ? bone_mapper_.getExpandedCollisionBones()
                      : std::set<std::string>{};
  std::set<std::string> rigid_proxy_scale_bones;

  for (const auto &bone_name : dependency_bones) {
    auto bone_animation = animation->bones.find(bone_name);
    if (bone_animation == animation->bones.end()) {
      continue;
    }
    const auto &channels = bone_animation->second;
    const bool selected_output = bone_mapper_.isPhysicsBone(bone_name);
    const bool rigid_geometry =
        selected_output || rigid_geometry_bones.contains(bone_name);




    (void)validatedScale(animation, bone_name, animation_role,
                         !rigid_body_mode,
                         !rigid_body_mode || rigid_geometry);
    if (rigid_body_mode && !rigid_geometry && channels.has_scale) {
      rigid_proxy_scale_bones.insert(bone_name);
    }
    const bool allow_position_rotation_zero =
        selected_output
            ? bone_mapper_.config().allow_selected_molang_zero_fallback
            : bone_mapper_.config().allow_input_only_molang_zero_fallback;
    if (channels.position.containsMolang() && !allow_position_rotation_zero) {
      rejectMolang(bone_name, "position",
                   "contains Molang and cannot be evaluated by the physics "
                   "pose sampler");
    }
    if (channels.rotation.containsMolang() && !allow_position_rotation_zero) {
      rejectMolang(bone_name, "rotation",
                   "contains Molang and cannot be evaluated by the physics "
                   "pose sampler");
    }
  }

  if (rigid_body_mode) {
    const auto reference_poses = BonePoseCalculator::calculate(
        bone_mapper_.allBones(), animation, 0.0);
    std::map<std::string, const loader::Bone *> bones_by_name;
    for (const auto &bone : bone_mapper_.allBones()) {
      if (!bone.name.empty()) {
        bones_by_name[bone.name] = &bone;
      }
    }
    const auto hasRigidProxyScaleAncestor =
        [&bones_by_name, &rigid_proxy_scale_bones](
            const std::string &bone_name) {
          std::set<std::string> visited;
          auto bone = bones_by_name.find(bone_name);
          while (bone != bones_by_name.end() &&
                 visited.insert(bone->first).second) {
            if (rigid_proxy_scale_bones.contains(bone->first)) {
              return true;
            }
            if (!bone->second->has_parent || bone->second->parent.empty()) {
              break;
            }
            bone = bones_by_name.find(bone->second->parent);
          }
          return false;
        };
    for (const auto &bone_name : dependency_bones) {
      if (!bone_mapper_.isPhysicsBone(bone_name) &&
          !rigid_geometry_bones.contains(bone_name)) {
        continue;
      }
      const auto animated = animation->bones.find(bone_name);
      const auto bone = bones_by_name.find(bone_name);
      if (animated == animation->bones.end() ||
          !animated->second.has_rotation ||
          !channelChanges(animated->second.rotation) ||
          bone == bones_by_name.end() || !bone->second->has_parent) {
        continue;
      }
      const auto parent_pose = reference_poses.find(bone->second->parent);
      if (parent_pose != reference_poses.end() &&
          !hasUniformNonDegenerateBasis(parent_pose->second) &&
          !hasRigidProxyScaleAncestor(bone_name)) {
        throw std::invalid_argument(
            animation_role + " bone '" + bone_name +
            "' rotation changes below a non-uniform or sheared parent "
            "affine basis; this can create time-varying shear between "
            "keyframes and cannot be represented by frozen Bullet box "
            "geometry");
      }
    }

    std::set<double> sample_times{0.0};
    if (std::isfinite(animation->animation_length) &&
        animation->animation_length > 0.0) {
      sample_times.insert(animation->animation_length);
    }
    for (const auto &bone_name : dependency_bones) {
      const auto animated = animation->bones.find(bone_name);
      if (animated == animation->bones.end()) {
        continue;
      }
      const auto add_times = [&sample_times](const loader::Keyframes &channel) {
        for (const auto &[time, value] : channel.keyframes) {
          (void)value;
          sample_times.insert(time);
        }
        for (const auto &[time, value] : channel.pre_keyframes) {
          (void)value;
          sample_times.insert(time);
        }
      };
      add_times(animated->second.rotation);
      add_times(animated->second.scale);
    }
    const std::vector<double> authored_times(sample_times.begin(),
                                             sample_times.end());
    for (std::size_t index = 1; index < authored_times.size(); ++index) {
      const double begin = authored_times[index - 1];
      const double span = authored_times[index] - begin;
      sample_times.insert(begin + span * 0.25);
      sample_times.insert(begin + span * 0.5);
      sample_times.insert(begin + span * 0.75);
    }
    for (double sample_time : sample_times) {
      const auto poses = BonePoseCalculator::calculate(
          bone_mapper_.allBones(), animation, sample_time);
      for (const auto &bone_name : dependency_bones) {
        if (!bone_mapper_.isPhysicsBone(bone_name) &&
            !rigid_geometry_bones.contains(bone_name)) {
          continue;
        }
        const auto pose = poses.find(bone_name);
        const bool frozen_proxy_affine =
            hasRigidProxyScaleAncestor(bone_name);
        if (pose != poses.end() &&
            !hasOrthogonalNonDegenerateBasis(pose->second) &&
            !frozen_proxy_affine) {
          throw std::invalid_argument(
              animation_role + " bone '" + bone_name +
              "' has a sheared or degenerate affine basis at "
              "time " + std::to_string(sample_time) +
              "; Bullet rigid geometry requires a non-degenerate orthogonal "
              "basis");
        }
      }
    }
  }
}

void PhysicsBaker::validateAnimationInputs() const {
  const auto dependencies = animationInputDependencyBones();
  validateAnimationInputTransforms(source_animation_, "source animation",
                                   dependencies);
  if (active_transition_.has_value()) {
    validateAnimationInputTransforms(active_transition_->target_animation,
                                     "transition target animation",
                                     dependencies);
    if (bone_mapper_.config().simulation_mode ==
        BoneMapper::SimulationMode::RigidBody) {
      const auto rigid_geometry_bones =
          bone_mapper_.getExpandedCollisionBones();
      for (const auto &bone_name : dependencies) {
        const bool geometry_owner =
            bone_mapper_.isPhysicsBone(bone_name) ||
            rigid_geometry_bones.contains(bone_name);
        const auto source_scale = validatedScale(
            source_animation_, bone_name, "source animation", false,
            geometry_owner);
        const auto target_scale = validatedScale(
            active_transition_->target_animation, bone_name,
            "transition target animation", false, geometry_owner);
        if (!scaleSignsMatch(source_scale, target_scale)) {
          throw std::invalid_argument(
              "transition target animation bone '" + bone_name +
              "' scale changes sign from the source animation and would "
              "cross zero; dynamic reflection is unsupported");
        }
        if (!geometry_owner) {
          continue;
        }
        if (!scaleNear(source_scale, target_scale)) {
          throw std::invalid_argument(
              "transition target animation bone '" + bone_name +
              "' constant scale differs from source animation; frozen Bullet "
              "collider geometry cannot change across a transition");
        }
      }
    }
  }
}

BakedFrame PhysicsBaker::copyFrameAtTime(const BakedFrame &frame, double time) {
  BakedFrame copy = frame;
  copy.time = time;
  copy.rebuildIndex();
  return copy;
}

std::vector<BakedFrame>
PhysicsBaker::copyFrames(const std::vector<BakedFrame> &source) {
  std::vector<BakedFrame> copy;
  copy.reserve(source.size());
  for (const auto &frame : source) {
    copy.push_back(copyFrameAtTime(frame, frame.time));
  }
  return copy;
}

double PhysicsBaker::smootherStep(double value) {
  const double t = std::max(0.0, std::min(1.0, value));
  return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

models::Vector3 PhysicsBaker::windVector(double speed, double direction_degrees,
                                         double elevation_degrees) {
  const double safeSpeed = std::isfinite(speed) ? std::max(0.0, speed) : 0.0;
  const double azimuth =
      (std::isfinite(direction_degrees) ? direction_degrees : 0.0) *
      std::numbers::pi / 180.0;
  double elevation =
      (std::isfinite(elevation_degrees) ? elevation_degrees : 0.0);
  elevation =
      std::max(-90.0, std::min(90.0, elevation)) * std::numbers::pi / 180.0;
  const double horizontal = std::cos(elevation) * safeSpeed;
  return models::Vector3(std::cos(azimuth) * horizontal,
                         std::sin(elevation) * safeSpeed,
                         std::sin(azimuth) * horizontal);
}

models::Vector3 PhysicsBaker::environmentWindVelocity(
    const BoneMapper::PhysicsGroupConfig &cfg) {
  if (cfg.use_wind_components) {
    return models::Vector3(std::isfinite(cfg.wind_x) ? cfg.wind_x : 0.0,
                           std::isfinite(cfg.wind_y) ? cfg.wind_y : 0.0,
                           std::isfinite(cfg.wind_z) ? cfg.wind_z : 0.0);
  }
  return windVector(cfg.wind_speed, cfg.wind_direction_degrees,
                    cfg.wind_elevation_degrees);
}

models::Vector3
PhysicsBaker::relativeAirVelocity(const BoneMapper::PhysicsGroupConfig &cfg) {
  models::Vector3 environment = environmentWindVelocity(cfg);
  models::Vector3 movement =
      windVector(cfg.movement_speed, cfg.movement_direction_degrees,
                 cfg.movement_elevation_degrees);
  return models::Vector3(environment.x - movement.x, environment.y - movement.y,
                         environment.z - movement.z);
}

}
