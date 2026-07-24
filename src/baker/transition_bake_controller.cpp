#include "xpbd/baker/transition_bake_controller.hpp"

#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <cmath>

namespace xpbd::baker {

TransitionBakeController::TransitionBakeController(
    const TransitionBakeRequest &request, std::vector<loader::Bone> bones,
    const BakedFrame *physical_frame, const BakedFrame *previous_physical_frame,
    double physical_frame_span)
    : request_(request), bones_(std::move(bones)),
      pose_evaluator_(BonePoseCalculator::compile(bones_)),
      target_animation_binding_(
          pose_evaluator_.bind(request_.target_animation)),
      physical_frame_span_(std::isfinite(physical_frame_span) &&
                                   physical_frame_span > 0
                               ? physical_frame_span
                               : 0.0) {
  for (const auto &bone : bones_) {
    if (!bone.name.empty()) {
      bones_by_name_[bone.name] = bone;
    }
  }
  auto index = [](const BakedFrame *frame,
                  std::map<std::string, BoneState> &out) {
    if (frame == nullptr) {
      return;
    }
    for (const auto &state : frame->bone_states) {
      out[state.bone_name] = state;
    }
  };
  index(physical_frame, physical_states_);
  index(previous_physical_frame, previous_physical_states_);
  initializeOffsets();
}

double TransitionBakeController::targetSampleTime(double elapsed) const {
  const auto *target = request_.target_animation;
  return canonicalSampleTime(
      *target, request_.target_entry_time - request_.transition_duration +
                   elapsed);
}

std::map<std::string, BonePoseCalculator::Pose>
TransitionBakeController::sample(double elapsed) const {
  SampleInputs inputs = sampleInputs(elapsed);
  return pose_evaluator_.calculate(
      target_animation_binding_, inputs.target_time,
      &inputs.position_overrides, &inputs.rotation_overrides);
}

const BonePoseCalculator::Evaluator::PoseMap &
TransitionBakeController::sampleInto(
    double elapsed,
    BonePoseCalculator::Evaluator::EvaluationScratch &scratch) const {
  SampleInputs inputs = sampleInputs(elapsed);
  return pose_evaluator_.calculateInto(
      target_animation_binding_, inputs.target_time, scratch,
      &inputs.position_overrides, &inputs.rotation_overrides);
}

TransitionBakeController::SampleInputs
TransitionBakeController::sampleInputs(double elapsed) const {
  const double safeElapsed =
      std::max(0.0, std::min(request_.transition_duration, elapsed));
  const double targetTime = targetSampleTime(safeElapsed);
  SampleInputs inputs;
  inputs.target_time = targetTime;
  for (const auto &[name, offset] : offsets_) {
    auto targetPosition =
        channel(*request_.target_animation, name, targetTime, false);
    const auto positionOffset = offset.position.offsetAt(safeElapsed);
    for (int axis = 0; axis < 3; ++axis) {
      targetPosition[static_cast<std::size_t>(axis)] +=
          positionOffset[static_cast<std::size_t>(axis)];
    }
    inputs.position_overrides[name] = targetPosition;

    auto targetEuler =
        channel(*request_.target_animation, name, targetTime, true);
    const auto targetTotalEuler = totalEuler(name, targetEuler);
    const auto targetQ = RotationUtil::quaternionFromBedrockEuler(
        targetTotalEuler[0], targetTotalEuler[1], targetTotalEuler[2]);
    const auto rotationVector = offset.rotation.offsetAt(safeElapsed);
    RotationUtil::Vec3 rotVec{rotationVector[0], rotationVector[1],
                              rotationVector[2]};
    const auto adjustedQ = RotationUtil::quaternionMultiply(
        RotationUtil::quaternionFromRotationVector(rotVec), targetQ);
    const auto adjustedTotalEuler = RotationUtil::unwrapEuler(
        targetTotalEuler, RotationUtil::bedrockEulerFromQuaternion(adjustedQ));
    inputs.rotation_overrides[name] =
        animationEuler(name, adjustedTotalEuler);
  }
  return inputs;
}

void TransitionBakeController::initializeOffsets() {
  const double epsilon =
      std::min(1.0 / 120.0, request_.transition_duration * 0.25);
  const double sourceBaseTime = sourceTransitionSampleTime(
      *request_.source_animation, request_.source_exit_time);
  const double targetBaseTime = targetSampleTime(0.0);
  const auto sourcePreviousSample = sampleTimeWithOffset(
      *request_.source_animation, sourceBaseTime, -epsilon);
  const OffsetSampleTime targetNextSample{
      targetSampleTime(epsilon), std::max(1e-9, epsilon)};
  for (const auto &bone : bones_) {
    if (bone.name.empty()) {
      continue;
    }
    const std::string &name = bone.name;
    const BoneState *physical =
        physical_states_.contains(name) ? &physical_states_.at(name) : nullptr;
    auto sourcePosition =
        physical != nullptr
            ? physical->position
            : channel(*request_.source_animation, name, sourceBaseTime, false);
    auto targetPosition =
        channel(*request_.target_animation, name, targetBaseTime, false);
    std::vector<double> positionOffset = {sourcePosition[0] - targetPosition[0],
                                          sourcePosition[1] - targetPosition[1],
                                          sourcePosition[2] -
                                              targetPosition[2]};

    auto sourceChannelPosition =
        channel(*request_.source_animation, name, sourceBaseTime, false);
    auto sourcePrevious = channel(*request_.source_animation, name,
                                  sourcePreviousSample.sample_time, false);
    auto targetNext = channel(*request_.target_animation, name,
                              targetNextSample.sample_time, false);
    const double sourceSpan = sourcePreviousSample.physical_span;
    const double targetSpan = targetNextSample.physical_span;

    const BoneState *previousPhysical =
        previous_physical_states_.contains(name)
            ? &previous_physical_states_.at(name)
            : nullptr;
    std::vector<double> velocityOffset(3);
    for (int axis = 0; axis < 3; ++axis) {
      double sourceVelocity = 0.0;
      if (physical != nullptr && previousPhysical != nullptr &&
          physical_frame_span_ > 0.0) {
        sourceVelocity =
            (physical->position[static_cast<std::size_t>(axis)] -
             previousPhysical->position[static_cast<std::size_t>(axis)]) /
            physical_frame_span_;
      } else {
        sourceVelocity =
            (sourceChannelPosition[static_cast<std::size_t>(axis)] -
             sourcePrevious[static_cast<std::size_t>(axis)]) /
            sourceSpan;
      }
      velocityOffset[static_cast<std::size_t>(axis)] =
          sourceVelocity - (targetNext[static_cast<std::size_t>(axis)] -
                            targetPosition[static_cast<std::size_t>(axis)]) /
                               targetSpan;
    }

    auto sourceEuler =
        physical != nullptr
            ? physical->rotation
            : channel(*request_.source_animation, name, sourceBaseTime, true);
    auto targetEuler =
        channel(*request_.target_animation, name, targetBaseTime, true);
    const auto sourceTotalEuler = totalEuler(name, sourceEuler);
    const auto targetTotalEuler = totalEuler(name, targetEuler);
    const auto sourceQ = RotationUtil::quaternionFromBedrockEuler(
        sourceTotalEuler[0], sourceTotalEuler[1], sourceTotalEuler[2]);
    const auto targetQ = RotationUtil::quaternionFromBedrockEuler(
        targetTotalEuler[0], targetTotalEuler[1], targetTotalEuler[2]);
    const auto rotationOffsetVec = RotationUtil::rotationVectorFromQuaternion(
        RotationUtil::quaternionMultiply(
            sourceQ, RotationUtil::quaternionInverse(targetQ)));
    std::vector<double> rotationOffset = {
        rotationOffsetVec[0], rotationOffsetVec[1], rotationOffsetVec[2]};

    auto sourcePreviousEuler = channel(*request_.source_animation, name,
                                       sourcePreviousSample.sample_time, true);
    auto targetNextEuler = channel(*request_.target_animation, name,
                                   targetNextSample.sample_time, true);
    auto sourceChannelEuler =
        channel(*request_.source_animation, name, sourceBaseTime, true);
    const auto sourcePreviousTotal = totalEuler(name, sourcePreviousEuler);
    const auto targetNextTotal = totalEuler(name, targetNextEuler);
    const auto sourceChannelTotal = totalEuler(name, sourceChannelEuler);

    std::array<double, 3> sourceAngular{};
    if (physical != nullptr && previousPhysical != nullptr &&
        physical_frame_span_ > 0.0) {
      const auto prevQ = RotationUtil::quaternionFromBedrockEuler(
          totalEuler(name, previousPhysical->rotation)[0],
          totalEuler(name, previousPhysical->rotation)[1],
          totalEuler(name, previousPhysical->rotation)[2]);
      const auto curQ = RotationUtil::quaternionFromBedrockEuler(
          totalEuler(name, physical->rotation)[0],
          totalEuler(name, physical->rotation)[1],
          totalEuler(name, physical->rotation)[2]);
      sourceAngular = quaternionVelocity(prevQ, curQ, physical_frame_span_);
    } else {
      sourceAngular = quaternionVelocity(
          RotationUtil::quaternionFromBedrockEuler(sourcePreviousTotal[0],
                                                   sourcePreviousTotal[1],
                                                   sourcePreviousTotal[2]),
          RotationUtil::quaternionFromBedrockEuler(sourceChannelTotal[0],
                                                   sourceChannelTotal[1],
                                                   sourceChannelTotal[2]),
          sourceSpan);
    }
    const auto targetAngular = quaternionVelocity(
        targetQ,
        RotationUtil::quaternionFromBedrockEuler(
            targetNextTotal[0], targetNextTotal[1], targetNextTotal[2]),
        targetSpan);
    std::vector<double> angularVelocityOffset = {
        sourceAngular[0] - targetAngular[0],
        sourceAngular[1] - targetAngular[1],
        sourceAngular[2] - targetAngular[2]};

    const double weight = request_.followWeight(name);
    offsets_.emplace(
        name,
        BoneOffset{InertializedTarget(std::move(positionOffset),
                                      std::move(velocityOffset),
                                      request_.transition_duration, weight),
                   InertializedTarget(std::move(rotationOffset),
                                      std::move(angularVelocityOffset),
                                      request_.transition_duration, weight)});
  }
}

std::array<double, 3>
TransitionBakeController::totalEuler(const std::string &bone_name,
                                     const std::array<double, 3> &anim) const {
  auto it = bones_by_name_.find(bone_name);
  if (it == bones_by_name_.end()) {
    return anim;
  }
  return {it->second.rotation[0] + anim[0], it->second.rotation[1] + anim[1],
          it->second.rotation[2] + anim[2]};
}

std::array<double, 3> TransitionBakeController::animationEuler(
    const std::string &bone_name, const std::array<double, 3> &total) const {
  auto it = bones_by_name_.find(bone_name);
  if (it == bones_by_name_.end()) {
    return total;
  }
  return {total[0] - it->second.rotation[0], total[1] - it->second.rotation[1],
          total[2] - it->second.rotation[2]};
}

double TransitionBakeController::canonicalSampleTime(
    const loader::Animation &animation, double time) {
  const double length = animation.animation_length;
  if (animation.loop && length > 0.0) {
    double wrapped = std::fmod(time, length);
    if (wrapped < 0.0) {
      wrapped += length;
    }
    return wrapped;
  }
  return std::max(0.0, std::min(length, time));
}

double TransitionBakeController::sourceTransitionSampleTime(
    const loader::Animation &animation, double time) {
  const double length = animation.animation_length;
  if (animation.loop && length > 0.0 && time >= 0.0 && time <= length) {
    return time;
  }
  return canonicalSampleTime(animation, time);
}

TransitionBakeController::OffsetSampleTime
TransitionBakeController::sampleTimeWithOffset(
    const loader::Animation &animation, double base_time, double offset) {
  constexpr double kMinimumPhysicalSpan = 1e-9;
  const double base = base_time;
  const double sample = canonicalSampleTime(animation, base + offset);
  const double requestedSpan = std::abs(offset);
  const double physicalSpan = animation.loop && animation.animation_length > 0.0
                                  ? requestedSpan
                                  : std::abs(sample - base);
  return {sample, std::max(kMinimumPhysicalSpan, physicalSpan)};
}

std::array<double, 3>
TransitionBakeController::channel(const loader::Animation &animation,
                                  const std::string &bone_name, double time,
                                  bool rotation) {
  auto it = animation.bones.find(bone_name);
  if (it == animation.bones.end()) {
    return {0, 0, 0};
  }
  if (rotation) {
    return it->second.has_rotation ? it->second.rotation.evaluate(time)
                                   : std::array<double, 3>{0, 0, 0};
  }
  return it->second.has_position ? it->second.position.evaluate(time)
                                 : std::array<double, 3>{0, 0, 0};
}

std::array<double, 3>
TransitionBakeController::quaternionVelocity(const std::array<double, 4> &from,
                                             const std::array<double, 4> &to,
                                             double span) {
  const auto delta = RotationUtil::quaternionMultiply(
      to, RotationUtil::quaternionInverse(from));
  auto vector = RotationUtil::rotationVectorFromQuaternion(delta);
  const double inverseSpan = 1.0 / std::max(1e-9, span);
  return {vector[0] * inverseSpan, vector[1] * inverseSpan,
          vector[2] * inverseSpan};
}

}
