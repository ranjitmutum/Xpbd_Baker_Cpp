#include "xpbd/baker/xpbd_periodic_state_tracker.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace xpbd::baker {
namespace {

RotationUtil::Quat normalized(const RotationUtil::Quat &value) {
  double length_squared = 0.0;
  for (double component : value) {
    if (!std::isfinite(component)) {
      throw std::invalid_argument("XPBD world rotation must be finite");
    }
    length_squared += component * component;
  }
  if (!(length_squared > 1e-20)) {
    throw std::invalid_argument("XPBD world rotation must be non-zero");
  }
  const double inverse_length = 1.0 / std::sqrt(length_squared);
  return {value[0] * inverse_length, value[1] * inverse_length,
          value[2] * inverse_length, value[3] * inverse_length};
}

std::array<double, 3> angularVelocity(const RotationUtil::Quat &previous,
                                      const RotationUtil::Quat &current,
                                      double dt) {
  const auto delta = RotationUtil::quaternionMultiply(
      current, RotationUtil::quaternionInverse(previous));
  const auto rotation_vector =
      RotationUtil::rotationVectorFromQuaternion(delta);
  return {rotation_vector[0] / dt, rotation_vector[1] / dt,
          rotation_vector[2] / dt};
}

}

void XpbdPeriodicStateTracker::clear() {
  previous_world_rotations_.clear();
  snapshot_ = {};
  initialized_ = false;
}

void XpbdPeriodicStateTracker::initialize(
    const BakedFrame &frame, const WorldRotations &world_rotations) {
  clear();
  record(frame, world_rotations, 0.0, false);
  initialized_ = true;
}

void XpbdPeriodicStateTracker::advance(const BakedFrame &frame,
                                       const WorldRotations &world_rotations,
                                       double dt) {
  if (!initialized_) {
    throw std::logic_error(
        "XPBD periodic state tracker must be initialized first");
  }
  if (!std::isfinite(dt) || !(dt > 0.0)) {
    throw std::invalid_argument(
        "XPBD periodic state dt must be finite and positive");
  }
  record(frame, world_rotations, dt, true);
}

void XpbdPeriodicStateTracker::record(const BakedFrame &frame,
                                      const WorldRotations &world_rotations,
                                      double dt,
                                      bool calculate_angular_velocity) {
  PeriodicStateAdapter::Snapshot next;
  next.required_metrics.position = true;
  next.required_metrics.rotation = true;
  next.required_metrics.linear_velocity = true;
  next.required_metrics.angular_velocity = true;
  next.required_metrics.sample_time = true;
  next.has_sample_time = true;
  next.sample_time = frame.time;
  WorldRotations normalized_rotations;
  std::set<std::string> frame_bones;
  for (const auto &state : frame.bone_states) {
    frame_bones.insert(state.bone_name);
    next.expected_bones.insert(state.bone_name);
    auto rotation = world_rotations.find(state.bone_name);
    if (rotation == world_rotations.end()) {
      next.anomaly_count++;
      continue;
    }

    PeriodicStateAdapter::BoneState bone;
    bone.position =
        state.has_world_position ? state.world_position : state.position;
    bone.linear_velocity = state.linear_velocity;
    bone.rotation_quaternion = normalized(rotation->second);
    bone.has_rotation = true;
    bone.angular_velocity = {0, 0, 0};
    bone.has_angular_velocity = true;

    if (calculate_angular_velocity) {
      auto previous = previous_world_rotations_.find(state.bone_name);
      if (previous == previous_world_rotations_.end()) {
        next.anomaly_count++;
        bone.has_angular_velocity = false;
      } else {
        bone.angular_velocity = angularVelocity(normalized(previous->second),
                                                bone.rotation_quaternion, dt);
      }
    }

    normalized_rotations.emplace(state.bone_name, bone.rotation_quaternion);
    next.bones.emplace(state.bone_name, bone);
  }
  for (const auto &[name, rotation] : world_rotations) {
    (void)rotation;
    if (!frame_bones.contains(name)) {
      next.anomaly_count++;
    }
  }
  previous_world_rotations_ = std::move(normalized_rotations);
  snapshot_ = std::move(next);
}

}
