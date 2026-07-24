#include "xpbd/rigidbody/rigid_body_bake_session.hpp"

#include "xpbd/baker/bedrock_transform_resolver.hpp"
#include "xpbd/baker/final_pose_reconstructor.hpp"
#include "xpbd/baker/physics_baker.hpp"
#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/rigidbody/bedrock_pose_converter.hpp"
#include "xpbd/rigidbody/bedrock_rigid_body_compiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numbers>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace xpbd::rigidbody {

namespace {
Transform multiplyTransforms(const Transform &left, const Transform &right) {
  Transform result;
  result.rotation =
      baker::RotationUtil::quaternionMultiply(left.rotation, right.rotation);
  const auto translated =
      baker::RotationUtil::rotateVector(left.rotation, right.translation);
  result.translation = {left.translation[0] + translated[0],
                        left.translation[1] + translated[1],
                        left.translation[2] + translated[2]};
  result.normalizeRotation();
  return result;
}

Transform inverseTransform(const Transform &value) {
  Transform result;
  result.rotation = baker::RotationUtil::quaternionInverse(value.rotation);
  const std::array<double, 3> negative{
      -value.translation[0], -value.translation[1], -value.translation[2]};
  result.translation =
      baker::RotationUtil::rotateVector(result.rotation, negative);
  result.normalizeRotation();
  return result;
}

std::string canonicalContactKey(std::string first, std::string second) {
  if (second < first) {
    std::swap(first, second);
  }


  return std::to_string(first.size()) + ":" + first + second;
}

std::uint32_t javaStringHashCode(std::string_view value) {
  std::uint32_t hash = 0;
  std::size_t offset = 0;
  const auto continuation = [&](std::size_t index) {
    return index < value.size() &&
           (static_cast<unsigned char>(value[index]) & 0xc0u) == 0x80u;
  };
  const auto appendCodeUnit = [&](std::uint16_t code_unit) {
    hash = hash * 31u + code_unit;
  };
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t codePoint = 0xfffdu;
    std::size_t width = 1;
    if (first < 0x80u) {
      codePoint = first;
    } else if (first >= 0xc2u && first <= 0xdfu &&
               continuation(offset + 1)) {
      codePoint = ((first & 0x1fu) << 6u) |
                  (static_cast<unsigned char>(value[offset + 1]) & 0x3fu);
      width = 2;
    } else if (first >= 0xe0u && first <= 0xefu &&
               continuation(offset + 1) && continuation(offset + 2)) {
      const auto second = static_cast<unsigned char>(value[offset + 1]);
      if ((first != 0xe0u || second >= 0xa0u) &&
          (first != 0xedu || second <= 0x9fu)) {
        codePoint = ((first & 0x0fu) << 12u) | ((second & 0x3fu) << 6u) |
                    (static_cast<unsigned char>(value[offset + 2]) & 0x3fu);
        width = 3;
      }
    } else if (first >= 0xf0u && first <= 0xf4u &&
               continuation(offset + 1) && continuation(offset + 2) &&
               continuation(offset + 3)) {
      const auto second = static_cast<unsigned char>(value[offset + 1]);
      if ((first != 0xf0u || second >= 0x90u) &&
          (first != 0xf4u || second <= 0x8fu)) {
        codePoint = ((first & 0x07u) << 18u) | ((second & 0x3fu) << 12u) |
                    ((static_cast<unsigned char>(value[offset + 2]) & 0x3fu)
                     << 6u) |
                    (static_cast<unsigned char>(value[offset + 3]) & 0x3fu);
        width = 4;
      }
    }
    offset += width;
    if (codePoint <= 0xffffu) {
      appendCodeUnit(static_cast<std::uint16_t>(codePoint));
    } else {
      codePoint -= 0x10000u;
      appendCodeUnit(static_cast<std::uint16_t>(0xd800u + (codePoint >> 10u)));
      appendCodeUnit(
          static_cast<std::uint16_t>(0xdc00u + (codePoint & 0x3ffu)));
    }
  }
  return hash;
}

double intervalSampleTime(double start_sample_time, double end_sample_time,
                          bool continuous_history, double forcing_period,
                          double fraction) {
  if (!continuous_history) {
    return end_sample_time;
  }
  if (end_sample_time < start_sample_time && forcing_period > 0.0) {
    double time =
        start_sample_time +
        (end_sample_time + forcing_period - start_sample_time) * fraction;
    if (time >= forcing_period) {
      time -= forcing_period;
    }
    return time;
  }
  if (end_sample_time >= start_sample_time) {
    return start_sample_time + (end_sample_time - start_sample_time) * fraction;
  }
  return end_sample_time;
}

void requireFinite(const char *name, double value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void requireNonNegative(const char *name, double value) {
  requireFinite(name, value);
  if (value < 0.0) {
    throw std::invalid_argument(std::string(name) + " must be >= 0");
  }
}

void requirePositive(const char *name, double value) {
  requireFinite(name, value);
  if (!(value > 0.0)) {
    throw std::invalid_argument(std::string(name) + " must be > 0");
  }
}

void requireRange(const char *name, double value, double minimum,
                  double maximum) {
  requireFinite(name, value);
  if (value < minimum || value > maximum) {
    throw std::invalid_argument(std::string(name) + " must be in [" +
                                std::to_string(minimum) + ", " +
                                std::to_string(maximum) + "]");
  }
}

void validateOptionalNonNegative(const char *name,
                                 const std::optional<double> &value) {
  if (value.has_value()) {
    requireNonNegative(name, *value);
  }
}

void validateOptionalPositive(const char *name,
                              const std::optional<double> &value) {
  if (value.has_value()) {
    requirePositive(name, *value);
  }
}

void validateOptionalBend(const char *name,
                          const std::optional<double> &value) {
  if (value.has_value()) {
    requireRange(name, *value, 0.0, 180.0);
  }
}

void validateConfig(const baker::BoneMapper &bone_mapper) {
  const auto &config = bone_mapper.config();
  requirePositive("particleMass", config.particle_mass);
  requireFinite("gravity", config.gravity_y);
  requireNonNegative("airDrag", config.air_drag);
  requireNonNegative("turbulence", config.turbulence);
  requireNonNegative("animationPullCompliance",
                     config.animation_pull_compliance);
  requirePositive("rigidBodyUnitScale", config.rigid_body_unit_scale);
  requireRange("rigidBodyLinearDamping", config.rigid_body_linear_damping, 0.0,
               1.0);
  requireRange("rigidBodyAngularDamping", config.rigid_body_angular_damping,
               0.0, 1.0);
  requireNonNegative("rigidBodyJointStiffness",
                     config.rigid_body_joint_stiffness);
  requireNonNegative("rigidBodyJointDamping", config.rigid_body_joint_damping);
  requireRange("rigidBodyMaxBendX", config.rigid_body_max_bend_x_degrees, 0.0,
               180.0);
  requireRange("rigidBodyMaxBendY", config.rigid_body_max_bend_y_degrees, 0.0,
               180.0);
  requireRange("rigidBodyMaxBendZ", config.rigid_body_max_bend_z_degrees, 0.0,
               180.0);
  requireRange("rigidBodyFriction", config.rigid_body_friction, 0.0, 10.0);
  requireRange("rigidBodyRestitution", config.rigid_body_restitution, 0.0, 1.0);
  requireNonNegative("rigidBodyMaximumSafePenetration",
                     config.rigid_body_maximum_safe_penetration);
  requireNonNegative("windSpeed", config.wind_speed);
  requireFinite("windDirection", config.wind_direction_degrees);
  requireFinite("windElevation", config.wind_elevation_degrees);
  requireFinite("windX", config.wind_x);
  requireFinite("windY", config.wind_y);
  requireFinite("windZ", config.wind_z);
  requireNonNegative("movementSpeed", config.movement_speed);
  requireFinite("movementDirection", config.movement_direction_degrees);
  requireFinite("movementElevation", config.movement_elevation_degrees);

  if (config.rigid_body_substeps < 1 || config.rigid_body_substeps > 16) {
    throw std::invalid_argument("rigidBodySubsteps must be in [1, 16]");
  }
  if (config.rigid_body_step_trace_capacity < 1 ||
      config.rigid_body_step_trace_capacity > 4096) {
    throw std::invalid_argument(
        "rigidBodyStepTraceCapacity must be in [1, 4096]");
  }
  switch (config.rigid_body_snapshot_level) {
  case SnapshotLevel::None:
  case SnapshotLevel::ContactsOnly:
  case SnapshotLevel::FullDiagnostics:
    break;
  default:
    throw std::invalid_argument("rigidBodySnapshotLevel is invalid");
  }
  if (config.solver_iterations <= 0) {
    throw std::invalid_argument("solverIterations must be > 0");
  }

  for (const auto &bone_name : bone_mapper.physicsBones()) {
    const auto *bone_config = bone_mapper.getBoneConfig(bone_name);
    if (bone_config == nullptr) {
      continue;
    }
    validateOptionalPositive("bone particleMass", bone_config->particle_mass);
    validateOptionalBend("bone rigidBodyMaxBendX",
                         bone_config->rigid_body_max_bend_x_degrees);
    validateOptionalBend("bone rigidBodyMaxBendY",
                         bone_config->rigid_body_max_bend_y_degrees);
    validateOptionalBend("bone rigidBodyMaxBendZ",
                         bone_config->rigid_body_max_bend_z_degrees);
    validateOptionalNonNegative("bone animationPullCompliance",
                                bone_config->animation_pull_compliance);
    validateOptionalNonNegative("bone gravityScale",
                                bone_config->gravity_scale);
    validateOptionalNonNegative("bone windInfluence",
                                bone_config->wind_influence);
    validateOptionalNonNegative("bone turbulenceInfluence",
                                bone_config->turbulence_influence);
  }
}

struct BodyMotionGeometry {
  double minimum_half_extent = std::numeric_limits<double>::infinity();
  double maximum_radius = 0.0;
};

BodyMotionGeometry motionGeometry(const BodyDefinition &definition) {
  BodyMotionGeometry geometry;
  for (const auto &box : definition.boxes) {
    geometry.minimum_half_extent =
        std::min(geometry.minimum_half_extent,
                 std::min(box.half_extents[0],
                          std::min(box.half_extents[1], box.half_extents[2])));
    const auto &center = box.local_transform.translation;
    const double centerRadius =
        std::sqrt(center[0] * center[0] + center[1] * center[1] +
                  center[2] * center[2]);
    const double halfDiagonal =
        std::sqrt(box.half_extents[0] * box.half_extents[0] +
                  box.half_extents[1] * box.half_extents[1] +
                  box.half_extents[2] * box.half_extents[2]);
    geometry.maximum_radius =
        std::max(geometry.maximum_radius, centerRadius + halfDiagonal);
  }
  return geometry;
}

double vectorLength(const std::array<double, 3> &value) {
  return std::sqrt(value[0] * value[0] + value[1] * value[1] +
                   value[2] * value[2]);
}

int requiredSubstepsForTravel(double travel, double allowed_travel) {
  if (!std::isfinite(travel) || travel < 0.0 ||
      !std::isfinite(allowed_travel) || !(allowed_travel > 0.0)) {
    return std::numeric_limits<int>::max();
  }
  const double required = std::ceil(travel / allowed_travel);
  if (!std::isfinite(required) ||
      required >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return std::max(1, static_cast<int>(required));
}

bool channelChanges(const loader::Keyframes &channel) {
  std::array<double, 3> baseline{};
  bool hasBaseline = false;
  auto inspect = [&](const auto &values) {
    for (const auto &[time, value] : values) {
      (void)time;
      if (!hasBaseline) {
        baseline = value;
        hasBaseline = true;
        continue;
      }
      for (std::size_t axis = 0; axis < baseline.size(); ++axis) {
        if (std::abs(value[axis] - baseline[axis]) > 1e-9) {
          return true;
        }
      }
    }
    return false;
  };
  return inspect(channel.keyframes) || inspect(channel.pre_keyframes);
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

std::array<double, 3> validateNonDegenerateScale(
    const loader::Animation *animation, const std::string &animation_role,
    const std::string &bone_name, bool require_constant = true) {
  const std::array<double, 3> identity{1.0, 1.0, 1.0};
  if (animation == nullptr) {
    return identity;
  }
  const auto animated = animation->bones.find(bone_name);
  if (animated == animation->bones.end()) {
    return identity;
  }
  const auto &scale = animated->second.scale;
  if (scale.containsMolang()) {
    throw std::invalid_argument(
        animation_role + " bone '" + bone_name +
        "' scale channel contains Molang; a constant non-zero scale cannot "
        "be proven for Bullet rigid geometry");
  }
  const bool authored = animated->second.has_scale ||
                        !scale.keyframes.empty() ||
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
      } else if (require_constant && !scaleNear(baseline, value)) {
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

bool hasUniformNonDegenerateBasis(
    const baker::BonePoseCalculator::Pose &pose) {
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

void validateChangingRotationUnderInheritedAffine(
    const loader::Animation *animation, const std::string &animation_role,
    const std::vector<loader::Bone> &bones,
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    const std::set<std::string> &dependency_bones) {
  if (animation == nullptr) {
    return;
  }
  std::map<std::string, const loader::Bone *> bones_by_name;
  for (const auto &bone : bones) {
    if (!bone.name.empty()) {
      bones_by_name[bone.name] = &bone;
    }
  }
  for (const auto &bone_name : dependency_bones) {
    const auto animated = animation->bones.find(bone_name);
    const auto bone = bones_by_name.find(bone_name);
    if (animated == animation->bones.end() ||
        !animated->second.has_rotation ||
        !channelChanges(animated->second.rotation) ||
        bone == bones_by_name.end() || !bone->second->has_parent) {
      continue;
    }
    const auto parent_pose = poses.find(bone->second->parent);
    if (parent_pose != poses.end() &&
        !hasUniformNonDegenerateBasis(parent_pose->second)) {
      throw std::invalid_argument(
          animation_role + " bone '" + bone_name +
          "' rotation changes below a non-uniform or sheared parent affine "
          "basis; this can create time-varying shear between keyframes and "
          "cannot be represented by frozen Bullet box geometry");
    }
  }
}

double physicalRotationDeltaDegrees(const loader::Keyframes &channel,
                                    double time) {
  const auto pre = channel.preValue(time);
  const auto post = channel.evaluate(time);
  const auto preQuaternion =
      baker::RotationUtil::quaternionFromBedrockEuler(pre[0], pre[1], pre[2]);
  const auto postQuaternion = baker::RotationUtil::quaternionFromBedrockEuler(
      post[0], post[1], post[2]);
  double dot = 0.0;
  for (std::size_t component = 0; component < 4; ++component) {
    dot += preQuaternion[component] * postQuaternion[component];
  }
  return 2.0 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0)) *
         180.0 / std::numbers::pi_v<double>;
}

std::array<double, 9> relativeLinearToRigidBody(
    const baker::BonePoseCalculator::Pose &body,
    const baker::BonePoseCalculator::Pose &source) {
  const auto inverse =
      baker::RotationUtil::quaternionInverse(body.world_rotation);
  std::array<double, 9> result{};
  for (std::size_t column = 0; column < 3; ++column) {
    const std::array<double, 3> world_axis{
        source.world_linear[column], source.world_linear[3 + column],
        source.world_linear[6 + column]};
    const auto local_axis =
        baker::RotationUtil::rotateVector(inverse, world_axis);
    for (std::size_t row = 0; row < 3; ++row) {
      result[row * 3 + column] = local_axis[row];
    }
  }
  return result;
}

std::map<std::string, baker::BonePoseCalculator::Pose>
buildFrozenRigidProxyPoses(
    const baker::BoneMapper &bone_mapper,
    const loader::Animation *animation, double sample_time,
    const std::map<std::string, baker::BonePoseCalculator::Pose>
        &authored_poses) {
  if (animation == nullptr) {
    return authored_poses;
  }

  loader::Animation proxy_animation = *animation;
  const auto collision_bones = bone_mapper.getExpandedCollisionBones();
  bool removed_input_only_scale = false;
  for (const auto &bone_name : bone_mapper.animationInputDependencyBones()) {
    const bool geometry_owner = bone_mapper.isPhysicsBone(bone_name) ||
                                collision_bones.contains(bone_name);
    auto channel = proxy_animation.bones.find(bone_name);
    if (!geometry_owner && channel != proxy_animation.bones.end() &&
        channel->second.has_scale) {
      channel->second.has_scale = false;
      removed_input_only_scale = true;
    }
  }
  if (!removed_input_only_scale) {
    return authored_poses;
  }

  auto proxy_poses = baker::BonePoseCalculator::calculate(
      bone_mapper.allBones(), &proxy_animation, sample_time);
  for (const auto &bone : bone_mapper.allBones()) {
    auto proxy = proxy_poses.find(bone.name);
    const auto authored = authored_poses.find(bone.name);
    if (proxy == proxy_poses.end() || authored == authored_poses.end()) {
      continue;
    }




    proxy->second.world_position = authored->second.world_position;
    proxy->second.world_rotation = authored->second.world_rotation;
    const auto mapped_pivot =
        baker::BedrockTransformResolver::convertBedrockVector(bone.pivot);
    std::array<double, 3> transformed_pivot{};
    for (std::size_t row = 0; row < 3; ++row) {
      for (std::size_t column = 0; column < 3; ++column) {
        transformed_pivot[row] +=
            proxy->second.world_linear[row * 3 + column] *
            mapped_pivot[column];
      }
      proxy->second.world_translation[row] =
          authored->second.world_position[row] - transformed_pivot[row];
    }
  }
  return proxy_poses;
}
}

std::unique_ptr<RigidBodyBakeSession> RigidBodyBakeSession::create(
    baker::BoneMapper &bone_mapper, const loader::Animation *source_animation,
    const std::map<std::string, baker::BonePoseCalculator::Pose>
        &initial_poses) {
  validateConfig(bone_mapper);
  const double g = bone_mapper.config().gravity_y *
                   bone_mapper.config().rigid_body_unit_scale;
  auto backend = createBulletBackend(0.0, g, 0.0);
  return std::unique_ptr<RigidBodyBakeSession>(new RigidBodyBakeSession(
      bone_mapper, source_animation, initial_poses, std::move(backend)));
}

RigidBodyBakeSession::RigidBodyBakeSession(
    baker::BoneMapper &bone_mapper, const loader::Animation *source_animation,
    const std::map<std::string, baker::BonePoseCalculator::Pose> &initial_poses,
    std::unique_ptr<RigidBodyBackend> backend)
    : bone_mapper_(bone_mapper), config_(bone_mapper.config()),
      snapshot_level_(resolveSnapshotLevel(
          config_.rigid_body_snapshot_level,
          config_.rigid_body_step_trace_enabled)),
      backend_(std::move(backend)),
      final_pose_reconstructor_evaluator_(
          baker::FinalPoseReconstructor::compile(bone_mapper.allBones())),
      unit_scale_(config_.rigid_body_unit_scale),
      substeps_(config_.rigid_body_substeps),
      source_animation_(source_animation) {
  step_trace_.snapshot_level = snapshot_level_;
  step_trace_.enabled = snapshot_level_ == SnapshotLevel::FullDiagnostics;
  step_trace_.capacity = config_.rigid_body_step_trace_capacity;
  collider_preflight_diagnostics_.unit_scale = unit_scale_;
  joint_spring_diagnostics_.requested_stiffness =
      config_.rigid_body_joint_stiffness;
  joint_spring_diagnostics_.requested_damping =
      config_.rigid_body_joint_damping;
  joint_spring_diagnostics_.solver_iterations = config_.solver_iterations;
  joint_spring_diagnostics_.configured_fixed_substeps = substeps_;
  joint_spring_diagnostics_.configured_minimum_substeps = substeps_;
  fixed_substep_stats_.configured_minimum = substeps_;
  fixed_substep_stats_.last_effective = substeps_;
  fixed_substep_stats_.maximum_effective = substeps_;
  fixed_substep_stats_.maximum_recommended = substeps_;
  backend_->setSolverIterations(config_.solver_iterations);
  backend_->setSnapshotLevel(snapshot_level_);
  pose_sampler_ = [this, source_animation](double time) {
    return baker::BonePoseCalculator::calculate(bone_mapper_.allBones(),
                                                source_animation, time);
  };
  const auto collision_bones = bone_mapper_.getExpandedCollisionBones();
  auto rigid_geometry_bones = collision_bones;
  for (const auto &bone_name : bone_mapper_.animationInputDependencyBones()) {
    const bool geometry_owner = bone_mapper_.isPhysicsBone(bone_name) ||
                                collision_bones.contains(bone_name);
    if (geometry_owner) {
      rigid_geometry_bones.insert(bone_name);
    }
    (void)validateNonDegenerateScale(source_animation, "source animation",
                                     bone_name, geometry_owner);
  }
  const auto proxy_initial_poses = buildFrozenRigidProxyPoses(
      bone_mapper_, source_animation, 0.0, initial_poses);
  validateChangingRotationUnderInheritedAffine(
      source_animation, "source animation", bone_mapper_.allBones(),
      proxy_initial_poses, rigid_geometry_bones);
  initialize(proxy_initial_poses);
  validateCompoundDescendantAnimation(source_animation, "source animation");
}

RigidBodyBakeSession::~RigidBodyBakeSession() {
  step_trace_.samples.clear();
  closed_ = true;
}

void RigidBodyBakeSession::setPoseSampler(PoseSampler sampler) {
  pose_sampler_ = std::move(sampler);
  custom_pose_sampler_ = true;
  motion_event_fraction_provider_ = {};
}

void RigidBodyBakeSession::setPoseSampler(
    PoseSampler sampler, MotionEventFractionProvider event_fraction_provider) {
  pose_sampler_ = std::move(sampler);
  custom_pose_sampler_ = true;
  motion_event_fraction_provider_ = std::move(event_fraction_provider);
}

void RigidBodyBakeSession::initialize(
    const std::map<std::string, baker::BonePoseCalculator::Pose>
        &initial_poses) {
  for (const auto &bone : bone_mapper_.allBones()) {
    if (bone.name.empty()) {
      continue;
    }
    bones_by_name_[bone.name] = bone;
    if (bone.has_parent) {
      children_by_bone_[bone.parent].push_back(bone.name);
    }
  }
  ordered_physics_bones_ = bone_mapper_.physicsBones();
  std::sort(ordered_physics_bones_.begin(), ordered_physics_bones_.end(),
            [this](const std::string &a, const std::string &b) {
              const int depth_a = hierarchyDepth(a);
              const int depth_b = hierarchyDepth(b);
              if (depth_a != depth_b) {
                return depth_a < depth_b;
              }
              const std::size_t model_index_a =
                  bone_mapper_.getModelBoneIndex(a);
              const std::size_t model_index_b =
                  bone_mapper_.getModelBoneIndex(b);
              if (model_index_a != model_index_b) {
                return model_index_a < model_index_b;
              }
              return a < b;
            });
  if (ordered_physics_bones_.empty()) {
    throw std::invalid_argument(
        "rigid-body mode requires at least one physics bone");
  }

  validateJointBridgeAnimation(source_animation_, "source animation");
  validateJointConfiguration();

  const auto collisionBones = bone_mapper_.getExpandedCollisionBones();

  for (const auto &boneName : ordered_physics_bones_) {
    const auto &bone = requireBone(boneName);
    const auto pose = requirePose(initial_poses, boneName);
    const bool fixed = bone_mapper_.isFixedBone(boneName);
    const MotionType motionType =
        fixed ? MotionType::Kinematic : MotionType::Dynamic;
    const double mass = fixed ? 0.0 : bone_mapper_.getEffectiveMass(boneName);
    if (!fixed && !(mass > 0)) {
      throw std::invalid_argument(
          "dynamic rigid-body bone requires positive mass: " + boneName);
    }





    std::vector<CubeSource> sources;
    std::vector<std::string> blockingPhysicsDescendants;
    std::queue<std::string> pending;
    pending.push(bone.name);
    bool hasDescendantGeometry = false;
    while (!pending.empty()) {
      const std::string sourceName = pending.front();
      pending.pop();
      if (sourceName != bone.name && bone_mapper_.isPhysicsBone(sourceName)) {
        blockingPhysicsDescendants.push_back(sourceName);
        continue;
      }
      const auto &sourceBone = requireBone(sourceName);
      (void)validateNonDegenerateScale(source_animation_, "source animation",
                                       sourceName);
      sources.push_back(
          CubeSource{&sourceBone, requirePose(initial_poses, sourceName)});
      hasDescendantGeometry =
          hasDescendantGeometry ||
          (sourceName != bone.name && !sourceBone.cubes.empty());
      auto it = children_by_bone_.find(sourceName);
      if (it != children_by_bone_.end()) {
        for (const auto &child : it->second) {
          pending.push(child);
        }
      }
    }

    for (const auto &source : sources) {
      if (source.bone != nullptr && !source.bone->cubes.empty()) {
        geometry_source_reference_linear_[bone.name][source.bone->name] =
            relativeLinearToRigidBody(pose, source.pose);
      }
    }

    if (hasDescendantGeometry) {



      for (const auto &source : sources) {
        if (source.bone == nullptr || source.bone->name == bone.name ||
            source.bone->cubes.empty()) {
          continue;
        }
        auto dependency = bones_by_name_.find(source.bone->name);
        while (dependency != bones_by_name_.end() &&
               dependency->second.name != bone.name) {
          compound_descendant_dependencies_[bone.name].insert(
              dependency->second.name);
          if (!dependency->second.has_parent) {
            break;
          }
          dependency = bones_by_name_.find(dependency->second.parent);
        }
      }
    }

    Compilation compilation =
        hasDescendantGeometry
            ? BedrockRigidBodyCompiler::compileCompound(
                  bone, pose, sources, motionType, mass, unit_scale_,
                  config_.rigid_body_friction, config_.rigid_body_restitution,
                  config_.rigid_body_ccd)
            : BedrockRigidBodyCompiler::compile(
                  bone, pose, motionType, mass, unit_scale_,
                  config_.rigid_body_friction, config_.rigid_body_restitution,
                  config_.rigid_body_ccd);
    source_cube_count_ += compilation.source_cube_count;
    skipped_degenerate_cube_count_ += compilation.skipped_degenerate_cube_count;
    appendColliderDiagnostics(compilation.collider_diagnostics);

    BodyDefinition definition;
    if (compilation.body.has_value()) {
      definition = *compilation.body;
    } else {
      if (!fixed) {
        std::ostringstream message;
        message << "dynamic rigid-body physics bone has no usable cube: "
                << boneName;
        if (!blockingPhysicsDescendants.empty()) {
          message << "; selected descendant physics bone";
          if (blockingPhysicsDescendants.size() != 1) {
            message << "s";
          }
          message << " block compound geometry: ";
          for (std::size_t index = 0;
               index < blockingPhysicsDescendants.size(); ++index) {
            if (index != 0) {
              message << ", ";
            }
            message << blockingPhysicsDescendants[index];
          }
          message << "; unselect those descendants to let " << boneName
                  << " own their cubes, or unselect " << boneName;
        }
        throw std::invalid_argument(message.str());
      }
      definition.name = boneName;
      definition.motion_type = MotionType::Kinematic;
      definition.mass = 0;
      definition.friction = config_.rigid_body_friction;
      definition.restitution = config_.rigid_body_restitution;
      definition.initial_bone_transform =
          BedrockPoseConverter::fromPose(pose, unit_scale_);
    }

    definition.linear_damping = config_.rigid_body_linear_damping;
    definition.angular_damping = config_.rigid_body_angular_damping;
    BodyHandle handle = backend_->createBody(definition);
    physics_bodies_[boneName] = handle;
    physics_body_definitions_[boneName] = definition;
    const BodyState initialState = backend_->getBodyState(handle);
    bone_to_com_transforms_[boneName] =
        multiplyTransforms(inverseTransform(initialState.bone_transform),
                           initialState.com_transform);
    if (fixed) {
      kinematic_bodies_[boneName] = handle;
      kinematic_body_definitions_[boneName] = definition;
    }
  }

  for (const auto &[owner, dependencies] : compound_descendant_dependencies_) {
    const Transform ownerTransform = BedrockPoseConverter::fromPose(
        requirePose(initial_poses, owner), unit_scale_);
    const Transform inverseOwner = inverseTransform(ownerTransform);
    for (const auto &dependency : dependencies) {
      compound_descendant_reference_transforms_[owner][dependency] =
          multiplyTransforms(
              inverseOwner,
              BedrockPoseConverter::fromPose(
                  requirePose(initial_poses, dependency), unit_scale_));
    }
  }

  for (const auto &boneName : collisionBones) {
    const auto &bone = requireBone(boneName);
    (void)validateNonDegenerateScale(source_animation_, "source animation",
                                     boneName);
    const auto pose = requirePose(initial_poses, boneName);
    Compilation compilation = BedrockRigidBodyCompiler::compile(
        bone, pose, MotionType::Kinematic, 0.0, unit_scale_,
        config_.rigid_body_friction, config_.rigid_body_restitution, false);
    source_cube_count_ += compilation.source_cube_count;
    skipped_degenerate_cube_count_ += compilation.skipped_degenerate_cube_count;
    appendColliderDiagnostics(compilation.collider_diagnostics);
    if (!compilation.body.has_value()) {
      skipped_body_bone_count_++;
      continue;
    }
    compilation.body->linear_damping = config_.rigid_body_linear_damping;
    compilation.body->angular_damping = config_.rigid_body_angular_damping;
    BodyHandle handle = backend_->createBody(*compilation.body);
    kinematic_bodies_[boneName] = handle;
    kinematic_body_definitions_[boneName] = *compilation.body;
    const BodyState initialState = backend_->getBodyState(handle);
    bone_to_com_transforms_[boneName] =
        multiplyTransforms(inverseTransform(initialState.bone_transform),
                           initialState.com_transform);
    collision_body_count_++;
  }

  if (config_.enable_ground_collision) {
    backend_->createGroundPlane(kGroundBodyName, 0.0,
                                config_.rigid_body_friction,
                                config_.rigid_body_restitution);
  }
  compileRuntimeSlots();
  buildJoints(initial_poses);
  initial_collision_ = backend_->detectContactsWithoutAdvancing();
  initial_collision_.failure_threshold =
      config_.rigid_body_maximum_safe_penetration;
  initial_collision_.warning_threshold =
      config_.rigid_body_maximum_safe_penetration * 0.5;
  initial_collision_.warning =
      initial_collision_.maximum_penetration >
          initial_collision_.warning_threshold &&
      initial_collision_.maximum_penetration <=
          initial_collision_.failure_threshold;
  initial_collision_.unsafe =
      initial_collision_.maximum_penetration >
      initial_collision_.failure_threshold;
  current_contact_count_ = initial_collision_.contact_count;
  maximum_contact_count_ = initial_collision_.contact_count;
  current_step_maximum_penetration_ = initial_collision_.maximum_penetration;
  periodic_interval_maximum_penetration_ =
      initial_collision_.maximum_penetration;
  periodic_interval_maximum_penetration_time_ =
      initial_collision_.maximum_penetration > 0.0 ? 0.0 : -1.0;
  maximum_penetration_ = initial_collision_.maximum_penetration;
  if (initial_collision_.unsafe) {
    std::ostringstream message;
    message << "initial Bullet collision exceeds Maximum Safe Penetration: "
            << initial_collision_.maximum_penetration << " > "
            << config_.rigid_body_maximum_safe_penetration;
    if (initial_collision_.worst_pair) {
      message << " (" << initial_collision_.worst_pair->first.name << " / "
              << initial_collision_.worst_pair->second.name << ")";
    }
    throw std::runtime_error(message.str());
  }
}

void RigidBodyBakeSession::compileRuntimeSlots() {
  const auto modelAir = baker::PhysicsBaker::relativeAirVelocity(config_);
  air_velocity_ = {modelAir.x * unit_scale_, modelAir.y * unit_scale_,
                   modelAir.z * unit_scale_};
  base_gravity_acceleration_ = config_.gravity_y * unit_scale_;

  capture_physics_targets_.clear();
  capture_runtime_slots_.clear();
  capture_runtime_slots_.reserve(ordered_physics_bones_.size());
  capture_body_query_slot_indices_.clear();
  capture_body_query_slot_indices_.reserve(ordered_physics_bones_.size());
  capture_body_states_.resize(ordered_physics_bones_.size());
  std::map<std::string, std::size_t> captureSlotByName;
  for (const auto &boneName : ordered_physics_bones_) {
    const auto handle = physics_bodies_.find(boneName);
    if (handle == physics_bodies_.end()) {
      throw std::logic_error("capture runtime slot is missing body: " +
                             boneName);
    }
    auto [target, inserted] =
        capture_physics_targets_.emplace(
            boneName, baker::FinalPoseReconstructor::WorldTarget{});
    if (!inserted) {
      throw std::logic_error(
          "capture runtime layout contains a duplicate bone name: " +
          boneName);
    }
    CaptureRuntimeSlot slot;
    slot.bone_name = boneName;
    slot.handle = handle->second;
    slot.physics_target = &target->second;
    captureSlotByName.emplace(boneName, capture_runtime_slots_.size());
    capture_runtime_slots_.push_back(std::move(slot));
  }
  for (const auto &[boneName, handle] : physics_bodies_) {
    (void)handle;
    const auto slot = captureSlotByName.find(boneName);
    if (slot == captureSlotByName.end()) {
      throw std::logic_error("capture query layout is missing body: " +
                             boneName);
    }
    capture_body_query_slot_indices_.push_back(slot->second);
  }

  dynamic_runtime_slots_.clear();
  dynamic_runtime_slots_.reserve(ordered_physics_bones_.size());
  std::map<std::string, std::size_t> dynamicSlotByName;
  for (const auto &boneName : ordered_physics_bones_) {
    if (bone_mapper_.isFixedBone(boneName)) {
      continue;
    }
    const auto handle = physics_bodies_.find(boneName);
    const auto definition = physics_body_definitions_.find(boneName);
    if (handle == physics_bodies_.end() ||
        definition == physics_body_definitions_.end()) {
      throw std::logic_error("dynamic runtime slot is missing body: " +
                             boneName);
    }

    DynamicRuntimeSlot slot;
    slot.bone_name = boneName;
    slot.handle = handle->second;
    slot.mass = bone_mapper_.getEffectiveMass(boneName);
    slot.gravity_scale = bone_mapper_.getEffectiveGravityScale(boneName);
    slot.wind_influence =
        bone_mapper_.getEffectiveWindInfluence(boneName);
    slot.turbulence_amplitude =
        config_.turbulence * unit_scale_ *
        bone_mapper_.getEffectiveTurbulenceInfluence(boneName) * slot.mass;
    slot.turbulence_phase =
        static_cast<double>(javaStringHashCode(boneName) & 0xffffu) * 0.001;
    slot.pull_compliance =
        bone_mapper_.getEffectiveAnimPullCompliance(boneName);
    if (slot.pull_compliance > 0.0) {
      slot.pull_stiffness =
          std::min(200.0, 1.0 / slot.pull_compliance);
      slot.pull_damping =
          std::min(50.0, 2.0 * std::sqrt(slot.pull_stiffness));
    }
    if (const auto offset = bone_to_com_transforms_.find(boneName);
        offset != bone_to_com_transforms_.end()) {
      slot.bone_to_com = offset->second;
      slot.has_bone_to_com = true;
    }
    if (!definition->second.boxes.empty()) {
      const BodyMotionGeometry geometry = motionGeometry(definition->second);
      slot.minimum_half_extent = geometry.minimum_half_extent;
      slot.maximum_radius = geometry.maximum_radius;
      slot.has_motion_geometry = true;
    }
    dynamicSlotByName.emplace(slot.bone_name,
                              dynamic_runtime_slots_.size());
    dynamic_runtime_slots_.push_back(std::move(slot));
  }

  dynamic_risk_slot_indices_.clear();
  dynamic_risk_slot_indices_.reserve(dynamic_runtime_slots_.size());
  for (const auto &[boneName, definition] : physics_body_definitions_) {
    if (definition.motion_type != MotionType::Dynamic) {
      continue;
    }
    if (const auto slot = dynamicSlotByName.find(boneName);
        slot != dynamicSlotByName.end()) {
      dynamic_risk_slot_indices_.push_back(slot->second);
    }
  }

  kinematic_runtime_slots_.clear();
  kinematic_runtime_slots_.reserve(kinematic_bodies_.size());
  for (const auto &[boneName, handle] : kinematic_bodies_) {
    KinematicRuntimeSlot slot;
    slot.bone_name = boneName;
    slot.handle = handle;
    if (const auto offset = bone_to_com_transforms_.find(boneName);
        offset != bone_to_com_transforms_.end()) {
      slot.bone_to_com = offset->second;
      slot.has_bone_to_com = true;
    }
    if (const auto definition =
            kinematic_body_definitions_.find(boneName);
        definition != kinematic_body_definitions_.end()) {
      slot.previous_transform = definition->second.initial_bone_transform;
      slot.has_previous_transform = true;
      if (!definition->second.boxes.empty()) {
        const BodyMotionGeometry geometry =
            motionGeometry(definition->second);
        slot.minimum_half_extent = geometry.minimum_half_extent;
        slot.maximum_radius = geometry.maximum_radius;
        slot.has_motion_geometry = true;
      }
    }
    kinematic_runtime_slots_.push_back(std::move(slot));
  }

  trace_body_slots_.clear();
  step_trace_.samples.clear();
  step_trace_write_index_ = 0;
  trace_sample_scratch_ = {};
  if (step_trace_.enabled) {
    std::map<std::string, std::pair<BodyHandle, bool>> handles;
    for (const auto &[name, handle] : physics_bodies_) {
      handles[name] = {handle, true};
    }
    for (const auto &[name, handle] : kinematic_bodies_) {
      handles.try_emplace(name, handle, false);
    }
    trace_body_slots_.reserve(handles.size());
    for (const auto &[name, handle] : handles) {
      TraceBodySlot slot;
      slot.name = name;
      slot.handle = handle.first;
      slot.physics_body = handle.second;
      if (const auto definition = physics_body_definitions_.find(name);
          definition != physics_body_definitions_.end()) {
        slot.motion_type = definition->second.motion_type;
      } else if (const auto definition =
                     kinematic_body_definitions_.find(name);
                 definition != kinematic_body_definitions_.end()) {
        slot.motion_type = definition->second.motion_type;
      }
      trace_body_slots_.push_back(std::move(slot));
    }
    step_trace_.samples.reserve(
        static_cast<std::size_t>(step_trace_.capacity));
  }
}

void RigidBodyBakeSession::appendColliderDiagnostics(
    const std::vector<ColliderDiagnostic> &diagnostics) {
  std::vector<const ColliderDiagnostic *> errors;
  for (const auto &diagnostic : diagnostics) {
    const bool first = collider_preflight_diagnostics_.colliders.empty();
    if (first) {
      collider_preflight_diagnostics_.observed_minimum_half_extent =
          diagnostic.minimum_half_extent;
    } else {
      collider_preflight_diagnostics_.observed_minimum_half_extent = std::min(
          collider_preflight_diagnostics_.observed_minimum_half_extent,
          diagnostic.minimum_half_extent);
    }
    collider_preflight_diagnostics_.observed_maximum_half_extent = std::max(
        collider_preflight_diagnostics_.observed_maximum_half_extent,
        diagnostic.maximum_half_extent);
    collider_preflight_diagnostics_.observed_maximum_aspect_ratio = std::max(
        collider_preflight_diagnostics_.observed_maximum_aspect_ratio,
        diagnostic.aspect_ratio);
    if (diagnostic.risk == ColliderRiskLevel::Warning) {
      ++collider_preflight_diagnostics_.warning_count;
    } else if (diagnostic.risk == ColliderRiskLevel::Error) {
      ++collider_preflight_diagnostics_.error_count;
      errors.push_back(&diagnostic);
    }
    collider_preflight_diagnostics_.colliders.push_back(diagnostic);
  }

  if (errors.empty()) {
    return;
  }
  std::ostringstream message;
  message << "Bullet collider preflight rejected " << errors.size()
          << " unsafe collider(s): ";
  for (std::size_t error_index = 0; error_index < errors.size();
       ++error_index) {
    if (error_index != 0) {
      message << "; ";
    }
    const auto &error = *errors[error_index];
    message << "Body '" << error.body_name << "' Box " << error.box_index
            << ", Bullet half extents [" << error.bullet_half_extents[0]
            << ", " << error.bullet_half_extents[1] << ", "
            << error.bullet_half_extents[2] << "], Minimum Half Extent "
            << error.minimum_half_extent << ", Maximum Half Extent "
            << error.maximum_half_extent << ", Aspect Ratio "
            << error.aspect_ratio
            << ", Collision Margin / Minimum Half Extent "
            << error.margin_to_minimum_half_extent << ", CCD Radius "
            << error.ccd_radius << ", Unit Scale " << error.unit_scale
            << ": ";
    for (std::size_t issue_index = 0; issue_index < error.issues.size();
         ++issue_index) {
      if (issue_index != 0) {
        message << ", ";
      }
      message << error.issues[issue_index];
    }
  }
  throw std::invalid_argument(message.str());
}

void RigidBodyBakeSession::validateJointBridgeAnimation(
    const loader::Animation *animation,
    const std::string &animation_role) const {
  struct BridgePath {
    std::string parent;
    std::string child;
    std::vector<std::string> bridges;
    std::vector<std::string> affected_channels;
  };

  std::vector<BridgePath> paths;
  std::set<std::string> allBridgeBones;
  for (const auto &childName : ordered_physics_bones_) {
    const std::string parentName = nearestPhysicsAncestor(childName);
    if (parentName.empty() ||
        (bone_mapper_.isFixedBone(parentName) &&
         bone_mapper_.isFixedBone(childName))) {
      continue;
    }

    BridgePath path{parentName, childName, {}, {}};
    auto current = bones_by_name_.find(childName);
    while (current != bones_by_name_.end() && current->second.has_parent) {
      const std::string bridgeName = current->second.parent;
      if (bridgeName == parentName) {
        break;
      }
      const auto bridge = bones_by_name_.find(bridgeName);
      if (bridge == bones_by_name_.end() ||
          bone_mapper_.isPhysicsBone(bridgeName)) {
        break;
      }
      path.bridges.push_back(bridgeName);
      allBridgeBones.insert(bridgeName);
      current = bridge;
    }
    if (path.bridges.empty()) {
      continue;
    }
    std::reverse(path.bridges.begin(), path.bridges.end());
    if (animation != nullptr) {
      for (const auto &bridgeName : path.bridges) {
        const auto channels = animation->bones.find(bridgeName);
        if (channels == animation->bones.end()) {
          continue;
        }
        if (channels->second.has_position) {
          path.affected_channels.push_back(bridgeName + ".position");
        }
        if (channels->second.has_rotation) {
          path.affected_channels.push_back(bridgeName + ".rotation");
        }
      }
    }
    paths.push_back(std::move(path));
  }

  if (paths.empty()) {
    return;
  }

  std::map<std::string, std::array<double, 3>> zeroPhysicsPosition;
  std::map<std::string, std::array<double, 3>> zeroPhysicsRotation;
  for (const auto &boneName : ordered_physics_bones_) {
    zeroPhysicsPosition[boneName] = {0.0, 0.0, 0.0};
    zeroPhysicsRotation[boneName] = {0.0, 0.0, 0.0};
  }

  const auto evaluator =
      baker::BonePoseCalculator::compile(bone_mapper_.allBones());
  const auto calculateFilteredPoses =
      [&](double time, bool usePreValues) {
        auto positionOverrides = zeroPhysicsPosition;
        auto rotationOverrides = zeroPhysicsRotation;
        if (usePreValues && animation != nullptr) {
          for (const auto &bridgeName : allBridgeBones) {
            const auto channels = animation->bones.find(bridgeName);
            if (channels == animation->bones.end()) {
              continue;
            }
            if (channels->second.has_position &&
                channels->second.position.hasDistinctPrePost(time)) {
              positionOverrides[bridgeName] =
                  channels->second.position.preValue(time);
            }
            if (channels->second.has_rotation &&
                channels->second.rotation.hasDistinctPrePost(time)) {
              rotationOverrides[bridgeName] =
                  channels->second.rotation.preValue(time);
            }
          }
        }
        return evaluator.calculate(animation, time, &positionOverrides,
                                   &rotationOverrides);
      };
  const auto relativeFrame = [&](const BridgePath &path,
                                 const auto &poses) {
    const Transform parent = BedrockPoseConverter::fromPose(
        requirePose(poses, path.parent), 1.0);
    const Transform child = BedrockPoseConverter::fromPose(
        requirePose(poses, path.child), 1.0);
    return multiplyTransforms(inverseTransform(parent), child);
  };

  const auto referencePoses = calculateFilteredPoses(0.0, false);
  for (const auto &path : paths) {
    joint_bridge_reference_transforms_.try_emplace(
        path.child, relativeFrame(path, referencePoses));
  }

  const bool hasAuthoredBridgeChannels =
      std::any_of(paths.begin(), paths.end(), [](const BridgePath &path) {
        return !path.affected_channels.empty();
      });
  std::set<double> eventTimes{0.0};
  double maximumTime = hasAuthoredBridgeChannels && animation != nullptr &&
                               std::isfinite(animation->animation_length)
                           ? std::max(0.0, animation->animation_length)
                           : 0.0;
  if (hasAuthoredBridgeChannels && animation != nullptr) {
    for (const auto &bridgeName : allBridgeBones) {
      const auto channels = animation->bones.find(bridgeName);
      if (channels == animation->bones.end()) {
        continue;
      }
      const auto appendChannelTimes = [&](const loader::Keyframes &channel) {
        for (const auto &[time, value] : channel.keyframes) {
          (void)value;
          if (std::isfinite(time) && time >= 0.0) {
            eventTimes.insert(time);
            maximumTime = std::max(maximumTime, time);
          }
        }
        for (const auto &[time, value] : channel.pre_keyframes) {
          (void)value;
          if (std::isfinite(time) && time >= 0.0) {
            eventTimes.insert(time);
            maximumTime = std::max(maximumTime, time);
          }
        }
      };
      if (channels->second.has_position) {
        appendChannelTimes(channels->second.position);
      }
      if (channels->second.has_rotation) {
        appendChannelTimes(channels->second.rotation);
      }
    }
  }
  eventTimes.insert(maximumTime);

  std::set<double> sampleTimes = eventTimes;
  constexpr std::array<double, 7> kInteriorProbeFractions{
      0.125, 0.21132486540518713, 0.25, 0.5,
      0.75,  0.7886751345948129,  0.875};
  if (eventTimes.size() >= 2) {
    auto previous = eventTimes.begin();
    for (auto next = std::next(previous); next != eventTimes.end();
         ++previous, ++next) {
      const double span = *next - *previous;
      if (!(span > 0.0)) {
        continue;
      }
      for (double fraction : kInteriorProbeFractions) {
        sampleTimes.insert(*previous + span * fraction);
      }
    }
  }

  struct Issue {
    const BridgePath *path = nullptr;
    double first_failure_time = 0.0;
    const char *first_failure_sample_kind = "post";
    double translation_error = 0.0;
    double rotation_error = 0.0;
  };
  std::map<std::string, Issue> issues;
  constexpr double kTranslationTolerance = 1e-8;
  constexpr double kQuaternionDotTolerance = 1e-10;
  const auto inspectPoses = [&](double time, const char *sampleKind,
                                const auto &poses) {
    for (const auto &path : paths) {
      const auto expected =
          joint_bridge_reference_transforms_.find(path.child);
      if (expected == joint_bridge_reference_transforms_.end()) {
        continue;
      }
      const Transform actual = relativeFrame(path, poses);
      double translationError = 0.0;
      for (std::size_t axis = 0; axis < 3; ++axis) {
        translationError = std::max(
            translationError,
            std::abs(actual.translation[axis] -
                     expected->second.translation[axis]));
      }
      double quaternionDot = 0.0;
      for (std::size_t component = 0; component < 4; ++component) {
        quaternionDot += actual.rotation[component] *
                         expected->second.rotation[component];
      }
      const double rotationError =
          1.0 - std::abs(std::clamp(quaternionDot, -1.0, 1.0));
      if (translationError > kTranslationTolerance ||
          rotationError > kQuaternionDotTolerance) {
        auto [issue, inserted] = issues.try_emplace(path.child);
        if (inserted) {
          issue->second = Issue{&path, time, sampleKind, translationError,
                                rotationError};
        } else {
          issue->second.translation_error =
              std::max(issue->second.translation_error, translationError);
          issue->second.rotation_error =
              std::max(issue->second.rotation_error, rotationError);
        }
      }
    }
  };

  for (double time : sampleTimes) {
    inspectPoses(time, "post", calculateFilteredPoses(time, false));
    bool hasDistinctPrePost = false;
    if (animation != nullptr && eventTimes.contains(time)) {
      for (const auto &bridgeName : allBridgeBones) {
        const auto channels = animation->bones.find(bridgeName);
        if (channels == animation->bones.end()) {
          continue;
        }
        hasDistinctPrePost =
            hasDistinctPrePost ||
            (channels->second.has_position &&
             channels->second.position.hasDistinctPrePost(time)) ||
            (channels->second.has_rotation &&
             channels->second.rotation.hasDistinctPrePost(time));
      }
    }
    if (hasDistinctPrePost) {
      inspectPoses(time, "pre", calculateFilteredPoses(time, true));
    }
  }

  if (!issues.empty()) {
    std::ostringstream message;
    message << "Bullet relative Joint bridge preflight rejected "
            << issues.size() << " changing frame(s) in " << animation_role
            << ": ";
    std::size_t issueIndex = 0;
    for (const auto &[child, issue] : issues) {
      (void)child;
      if (issueIndex++ != 0) {
        message << "; ";
      }
      message << "Physics Parent '" << issue.path->parent
              << "', Physics Child '" << issue.path->child
              << "', Bridge Path [";
      for (std::size_t bridgeIndex = 0;
           bridgeIndex < issue.path->bridges.size(); ++bridgeIndex) {
        if (bridgeIndex != 0) {
          message << " -> ";
        }
        message << issue.path->bridges[bridgeIndex];
      }
      message << "], affected channels [";
      for (std::size_t channelIndex = 0;
           channelIndex < issue.path->affected_channels.size();
           ++channelIndex) {
        if (channelIndex != 0) {
          message << ", ";
        }
        message << issue.path->affected_channels[channelIndex];
      }
      message << "], maximum translation delta " << issue.translation_error
              << ", quaternion distance " << issue.rotation_error
              << ", first failure at " << issue.first_failure_sample_kind
              << " time " << issue.first_failure_time;
    }
    message << "; dynamic relative Joint frames are not implemented";
    throw std::invalid_argument(message.str());
  }
}

void RigidBodyBakeSession::validateJointConfiguration() {
  joint_preflight_diagnostics_ = {};
  std::vector<std::string> errors;
  for (const auto &childName : ordered_physics_bones_) {
    const std::string parentName = nearestPhysicsAncestor(childName);
    if (parentName.empty() ||
        (bone_mapper_.isFixedBone(parentName) &&
         bone_mapper_.isFixedBone(childName))) {
      continue;
    }

    const auto limits =
        bone_mapper_.getEffectiveRigidBodyMaxBendDegrees(childName);
    const std::array<bool, 3> limited{
        config_.enable_angle_constraints && limits[0] < 180.0,
        config_.enable_angle_constraints && limits[1] < 180.0,
        config_.enable_angle_constraints && limits[2] < 180.0};

    if (limited[1] &&
        limits[1] >
            joint_preflight_diagnostics_.maximum_safe_y_limit_degrees) {
      std::ostringstream message;
      message << "Parent " << parentName << ", Child " << childName
              << ": Y Bend " << limits[1] << " degrees exceeds the safe "
              << joint_preflight_diagnostics_.maximum_safe_y_limit_degrees
              << " degree limit for Rotation Order XYZ";
      errors.push_back(message.str());
    } else if (!limited[1] && (limited[0] || limited[2])) {
      std::ostringstream message;
      message << "Parent " << parentName << ", Child " << childName
              << ": free Y can cross +/-90 degrees while "
              << (limited[0] && limited[2]
                      ? "X and Z remain limited/sprung"
                      : limited[0] ? "X remains limited/sprung"
                                   : "Z remains limited/sprung")
              << " for Rotation Order XYZ";
      errors.push_back(message.str());
    }

    for (const std::size_t axis : {std::size_t{0}, std::size_t{2}}) {
      if (limited[axis] &&
          limits[axis] >=
              joint_preflight_diagnostics_.near_half_turn_warning_degrees) {
        joint_preflight_diagnostics_.warnings.push_back(
            {parentName, childName, axis == 0 ? "X" : "Z", limits[axis]});
      }
    }
  }

  if (!errors.empty()) {
    std::ostringstream message;
    message << "Bullet XYZ joint preflight rejected " << errors.size()
            << " unsafe joint configuration(s): ";
    for (std::size_t index = 0; index < errors.size(); ++index) {
      if (index != 0) {
        message << "; ";
      }
      message << errors[index];
    }
    throw std::invalid_argument(message.str());
  }
}

void RigidBodyBakeSession::validateCompoundDescendantAnimation(
    const loader::Animation *animation,
    const std::string &animation_role) const {
  if (animation == nullptr) {
    return;
  }
  for (const auto &[owner, dependencies] : compound_descendant_dependencies_) {
    for (const auto &dependency : dependencies) {
      const auto animated = animation->bones.find(dependency);
      if (animated == animation->bones.end()) {
        continue;
      }
      const auto reject = [&](const char *channel_name,
                              const loader::Keyframes &channel) {
        const std::string reason =
            channel.containsMolang() ? "contains Molang" : "changes over time";
        throw std::invalid_argument(
            animation_role + " " + channel_name + " channel " + reason +
            " on compound descendant " + dependency + " (rigid body " + owner +
            "); animated compound deformation is unsupported");
      };
      if (animated->second.has_position &&
          (animated->second.position.containsMolang() ||
           channelChanges(animated->second.position))) {
        reject("position", animated->second.position);
      }
      if (animated->second.has_rotation &&
          (animated->second.rotation.containsMolang() ||
           channelChanges(animated->second.rotation))) {
        reject("rotation", animated->second.rotation);
      }
      if (animated->second.has_scale) {
        (void)validateNonDegenerateScale(animation, animation_role,
                                         dependency);
      }
    }
  }
}

void RigidBodyBakeSession::validateCompoundDescendantTransition(
    const loader::Animation *target_animation, double target_sample_time) {
  transition_target_animation_ = target_animation;
  transition_target_entry_time_ = target_sample_time;
  if (target_animation == nullptr) {
    return;
  }
  validateJointBridgeAnimation(target_animation,
                               "transition target animation");
  const auto collision_bones = bone_mapper_.getExpandedCollisionBones();
  auto rigid_geometry_bones = collision_bones;
  for (const auto &bone_name : bone_mapper_.animationInputDependencyBones()) {
    const bool geometry_owner = bone_mapper_.isPhysicsBone(bone_name) ||
                                collision_bones.contains(bone_name);
    if (geometry_owner) {
      rigid_geometry_bones.insert(bone_name);
    }
    const auto source_scale = validateNonDegenerateScale(
        source_animation_, "source animation", bone_name, geometry_owner);
    const auto target_scale = validateNonDegenerateScale(
        target_animation, "transition target animation", bone_name,
        geometry_owner);
    if (!scaleSignsMatch(source_scale, target_scale)) {
      throw std::invalid_argument(
          "transition target animation bone '" + bone_name +
          "' scale changes sign from the source animation and would cross "
          "zero; dynamic reflection is unsupported");
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
  const auto targetAuthoredPoses = baker::BonePoseCalculator::calculate(
      bone_mapper_.allBones(), target_animation, target_sample_time);
  const auto targetPoses = buildFrozenRigidProxyPoses(
      bone_mapper_, target_animation, target_sample_time, targetAuthoredPoses);
  validateChangingRotationUnderInheritedAffine(
      target_animation, "transition target animation",
      bone_mapper_.allBones(), targetPoses,
      rigid_geometry_bones);
  if (compound_descendant_reference_transforms_.empty() &&
      geometry_source_reference_linear_.empty()) {
    return;
  }
  constexpr double kTranslationTolerance = 1e-8;
  constexpr double kQuaternionDotTolerance = 1e-10;
  constexpr double kLinearTolerance = 1e-8;
  for (const auto &[owner, dependencies] :
       compound_descendant_reference_transforms_) {
    const Transform targetOwner = BedrockPoseConverter::fromPose(
        requirePose(targetPoses, owner), unit_scale_);
    const Transform inverseTargetOwner = inverseTransform(targetOwner);
    for (const auto &[dependency, expectedRelative] : dependencies) {
      const Transform targetRelative = multiplyTransforms(
          inverseTargetOwner,
          BedrockPoseConverter::fromPose(requirePose(targetPoses, dependency),
                                         unit_scale_));
      double translationError = 0.0;
      for (std::size_t axis = 0; axis < 3; ++axis) {
        translationError = std::max(
            translationError, std::abs(targetRelative.translation[axis] -
                                       expectedRelative.translation[axis]));
      }
      double quaternionDot = 0.0;
      for (std::size_t component = 0; component < 4; ++component) {
        quaternionDot += targetRelative.rotation[component] *
                         expectedRelative.rotation[component];
      }
      if (translationError > kTranslationTolerance ||
          1.0 - std::abs(quaternionDot) > kQuaternionDotTolerance) {
        throw std::invalid_argument(
            "transition target animation changes the static relative "
            "transform of compound descendant " +
            dependency + " (rigid body " + owner +
            "); dynamic compound deformation is unsupported");
      }
    }
  }
  for (const auto &[owner, sources] : geometry_source_reference_linear_) {
    const auto &target_owner = requirePose(targetPoses, owner);
    for (const auto &[source, expected] : sources) {
      const auto actual = relativeLinearToRigidBody(
          target_owner, requirePose(targetPoses, source));
      double maximum_error = 0.0;
      for (std::size_t component = 0; component < expected.size();
           ++component) {
        maximum_error =
            std::max(maximum_error,
                     std::abs(actual[component] - expected[component]));
      }
      if (maximum_error > kLinearTolerance) {
        throw std::invalid_argument(
            "transition target animation changes the frozen affine geometry "
            "of source " +
            source + " (rigid body " + owner +
            "); scale/shear changes across a transition are unsupported");
      }
    }
  }
}

void RigidBodyBakeSession::buildJoints(
    const std::map<std::string, baker::BonePoseCalculator::Pose>
        &initial_poses) {
  for (const auto &childName : ordered_physics_bones_) {
    const std::string parentName = nearestPhysicsAncestor(childName);
    if (parentName.empty()) {
      continue;
    }
    auto parentIt = physics_bodies_.find(parentName);
    auto childIt = physics_bodies_.find(childName);
    if (parentIt == physics_bodies_.end() || childIt == physics_bodies_.end()) {
      continue;
    }
    if (bone_mapper_.isFixedBone(parentName) &&
        bone_mapper_.isFixedBone(childName)) {
      continue;
    }
    const auto bendDegrees =
        bone_mapper_.getEffectiveRigidBodyMaxBendDegrees(childName);
    JointSettings settings;
    settings.stiffness = config_.rigid_body_joint_stiffness;
    settings.damping = config_.rigid_body_joint_damping;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const bool freeAxis =
          !config_.enable_angle_constraints || bendDegrees[axis] >= 180.0;
      if (freeAxis) {

        settings.angular_lower_limit[axis] = 1.0;
        settings.angular_upper_limit[axis] = -1.0;
        settings.angular_spring_enabled[axis] = false;
      } else {
        const double limit = bendDegrees[axis] * 3.14159265358979323846 / 180.0;
        settings.angular_lower_limit[axis] = -limit;
        settings.angular_upper_limit[axis] = limit;
        settings.angular_spring_enabled[axis] = settings.stiffness > 0.0;
      }
    }
    const Transform worldAnchor = BedrockPoseConverter::fromPose(
        requirePose(initial_poses, childName), unit_scale_);
    joint_construction_snapshots_.push_back(
        {parentName, childName, worldAnchor, settings});
    ++joint_spring_diagnostics_.constructed_joint_count;
    const int activeSpringAxes = static_cast<int>(std::count(
        settings.angular_spring_enabled.begin(),
        settings.angular_spring_enabled.end(), true));
    joint_spring_diagnostics_.active_spring_axis_count += activeSpringAxes;
    if (activeSpringAxes > 0) {
      ++joint_spring_diagnostics_.active_spring_joint_count;
    }
    backend_->addSpringJoint(parentIt->second, childIt->second, worldAnchor,
                             settings);
  }
}

std::set<std::string> RigidBodyBakeSession::kinematicPoseDrivers() const {
  std::set<std::string> drivers;
  for (const auto &[boneName, handle] : kinematic_bodies_) {
    (void)handle;
    auto current = bones_by_name_.find(boneName);
    while (current != bones_by_name_.end() &&
           drivers.insert(current->first).second) {
      if (!current->second.has_parent) {
        break;
      }
      current = bones_by_name_.find(current->second.parent);
    }
  }
  return drivers;
}

void RigidBodyBakeSession::appendAnimationEventFractions(
    std::set<double> &fractions, const loader::Animation &animation,
    double sample_origin, double start_parameter, double end_parameter) const {
  const double parameterSpan = end_parameter - start_parameter;
  if (!(parameterSpan > 0.0)) {
    return;
  }
  const double sampleStart = sample_origin + start_parameter;
  const double sampleEnd = sample_origin + end_parameter;
  const double period = animation.loop && animation.animation_length > 0.0
                            ? animation.animation_length
                            : 0.0;
  constexpr double kFractionTolerance = 1e-12;

  const auto appendEventTime = [&](double eventTime) {
    bool appended = false;
    const auto appendOccurrence = [&](double occurrence) {
      const double fraction = (occurrence - sampleStart) / parameterSpan;
      if (fraction >= -kFractionTolerance &&
          fraction <= 1.0 + kFractionTolerance) {
        fractions.insert(std::clamp(fraction, 0.0, 1.0));
        appended = true;
      }
    };
    if (period > 0.0) {
      const auto firstCycle = static_cast<long long>(
          std::ceil((sampleStart - eventTime) / period - kFractionTolerance));
      const auto lastCycle = static_cast<long long>(
          std::floor((sampleEnd - eventTime) / period + kFractionTolerance));
      for (long long cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        appendOccurrence(eventTime + static_cast<double>(cycle) * period);
      }
    } else if (eventTime >= sampleStart - kFractionTolerance &&
               eventTime <= sampleEnd + kFractionTolerance) {
      appendOccurrence(eventTime);
    }
    return appended;
  };

  const auto drivers = kinematicPoseDrivers();
  for (const auto &boneName : drivers) {
    const auto channelsIt = animation.bones.find(boneName);
    if (channelsIt == animation.bones.end()) {
      continue;
    }
    const auto &channels = channelsIt->second;
    const bool zeroFallbackAllowed =
        bone_mapper_.isPhysicsBone(boneName)
            ? config_.allow_selected_molang_zero_fallback
            : config_.allow_input_only_molang_zero_fallback;
    const auto appendChannel = [&](const loader::Keyframes &channel,
                                   const char *channelName, bool angular) {
      if (channel.containsMolang() && !zeroFallbackAllowed) {
        throw std::runtime_error(
            "kinematic " + std::string(channelName) +
            " contains Molang without an explicit zero-fallback contract: " +
            boneName);
      }

      std::set<double> eventTimes;
      for (const auto &[time, value] : channel.keyframes) {
        (void)value;
        eventTimes.insert(time);
      }
      for (const auto &[time, value] : channel.pre_keyframes) {
        (void)value;
        eventTimes.insert(time);
      }

      if (channel.keyframes.size() >= 2) {
        auto previous = channel.keyframes.begin();
        for (auto next = std::next(previous); next != channel.keyframes.end();
             ++previous, ++next) {
          const bool catmull =
              channel.interpolationMode(previous->first) ==
                  loader::Keyframes::InterpolationMode::CatmullRom ||
              channel.interpolationMode(next->first) ==
                  loader::Keyframes::InterpolationMode::CatmullRom;
          const int subdivisions = angular ? 8 : (catmull ? 4 : 1);
          for (int part = 1; part < subdivisions; ++part) {
            eventTimes.insert(previous->first +
                              (next->first - previous->first) *
                                  (static_cast<double>(part) / subdivisions));
          }
        }
      }

      for (const double eventTime : eventTimes) {
        const bool occursInInterval = appendEventTime(eventTime);
        constexpr double kBenignRotationDiscontinuityDegrees = 5.0;
        const bool meaningfulDiscontinuity =
            !angular ||
            physicalRotationDeltaDegrees(channel, eventTime) >
                kBenignRotationDiscontinuityDegrees;
        if (occursInInterval && channel.hasDistinctPrePost(eventTime) &&
            meaningfulDiscontinuity) {
          throw std::runtime_error(
              "kinematic " + std::string(channelName) +
              " has a discontinuous pre/post teleport that cannot be "
              "collision-safe or preserve dynamic Bullet state without "
              "sweep support: " +
              boneName);
        }
      }
    };

    if (channels.has_position) {
      appendChannel(channels.position, "position", false);
    }
    if (channels.has_rotation) {
      appendChannel(channels.rotation, "rotation", true);
    }
  }
}

std::vector<double> RigidBodyBakeSession::motionProbeFractions(
    double start_sample_time, double end_sample_time, bool continuous_history,
    double forcing_period) const {
  std::set<double> fractions{0.0, 1.0};
  if (!continuous_history) {
    return {0.0, 1.0};
  }


  constexpr int kMinimumAngularProbeSegments = 8;
  const int uniformSegments = std::max(substeps_, kMinimumAngularProbeSegments);
  for (int segment = 1; segment < uniformSegments; ++segment) {
    fractions.insert(static_cast<double>(segment) / uniformSegments);
  }

  double sourceEndParameter = end_sample_time;
  if (sourceEndParameter < start_sample_time && forcing_period > 0.0) {
    sourceEndParameter += forcing_period;
  }
  if (source_animation_ != nullptr) {
    appendAnimationEventFractions(fractions, *source_animation_, 0.0,
                                  start_sample_time, sourceEndParameter);
  }
  if (transition_target_animation_ != nullptr &&
      end_sample_time >= start_sample_time) {
    appendAnimationEventFractions(fractions, *transition_target_animation_,
                                  transition_target_entry_time_,
                                  start_sample_time, end_sample_time);
  }

  if (motion_event_fraction_provider_) {
    for (const double fraction :
         motion_event_fraction_provider_(start_sample_time, end_sample_time,
                                         continuous_history, forcing_period)) {
      if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        throw std::runtime_error(
            "custom rigid pose sampler returned an invalid motion-event "
            "fraction");
      }
      fractions.insert(fraction);
    }
  } else if (custom_pose_sampler_ && source_animation_ == nullptr &&
             transition_target_animation_ == nullptr &&
             !kinematic_bodies_.empty()) {
    throw std::runtime_error(
        "custom rigid pose sampler requires explicit motion-event fractions");
  }

  return {fractions.begin(), fractions.end()};
}

void RigidBodyBakeSession::advance(
    double start_sample_time, double end_sample_time, double output_dt,
    bool continuous_history, double forcing_period,
    const std::map<std::string, baker::BonePoseCalculator::Pose>
        *endpoint_poses) {
  if (closed_) {
    throw std::logic_error("session closed");
  }
  if (!std::isfinite(start_sample_time) || !std::isfinite(end_sample_time) ||
      !std::isfinite(output_dt) || !(output_dt > 0)) {
    throw std::invalid_argument("rigid-body step times must be finite");
  }
  if (continuous_history && forcing_period <= 0.0 &&
      end_sample_time < start_sample_time) {
    ++kinematic_history_stats_.detected_discontinuities;
    ++kinematic_history_stats_.rejected_discontinuities;
    kinematic_history_stats_.last_reason =
        KinematicDiscontinuityReason::SampleTimeRegression;
    std::ostringstream message;
    message << "kinematic discontinuity detected: non-periodic sample time "
               "regressed from "
            << start_sample_time << " to " << end_sample_time
            << "; preserving the dynamic Bullet session is unsafe";
    throw std::runtime_error(message.str());
  }

  std::map<double, PoseMap> poseCache;
  const PoseAtFraction poseAtFraction =
      [&](double fraction) -> const PoseMap & {
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
      throw std::logic_error("invalid rigid pose-cache fraction");
    }
    if (fraction == 1.0 && endpoint_poses != nullptr) {
      return *endpoint_poses;
    }
    auto found = poseCache.find(fraction);
    if (found != poseCache.end()) {
      return found->second;
    }
    const double sampleTime =
        intervalSampleTime(start_sample_time, end_sample_time,
                           continuous_history, forcing_period, fraction);
    return poseCache.emplace(fraction, pose_sampler_(sampleTime)).first->second;
  };

  analyzeFixedStepRisk(start_sample_time, end_sample_time, output_dt,
                       continuous_history, forcing_period, poseAtFraction);
  last_effective_substeps_ = substeps_;
  fixed_substep_stats_.last_effective = substeps_;
  fixed_substep_stats_.maximum_effective = substeps_;
  ++fixed_substep_stats_.output_step_count;
  executed_substep_sum_ += static_cast<std::uint64_t>(substeps_);
  fixed_substep_stats_.average_effective =
      static_cast<double>(executed_substep_sum_) /
      static_cast<double>(fixed_substep_stats_.output_step_count);
  const double fixedDt = output_dt / substeps_;
  last_output_dt_ = output_dt;
  last_substep_dt_ = fixedDt;
  current_step_maximum_penetration_ = 0.0;
  double previousSubstepSampleTime = start_sample_time;
  for (int substep = 0; substep < substeps_; ++substep) {
    const bool substepContinuous = continuous_history || substep > 0;
    const double sampleTime = intervalSampleTime(
        start_sample_time, end_sample_time, continuous_history, forcing_period,
        (substep + 1.0) / substeps_);
    const bool crossedPeriodicBoundary =
        continuous_history && forcing_period > 0.0 &&
        sampleTime < previousSubstepSampleTime;
    const KinematicHistoryMode historyMode =
        !substepContinuous
            ? KinematicHistoryMode::TeleportResetVelocity
            : crossedPeriodicBoundary
                  ? KinematicHistoryMode::PeriodicWrapResetVelocity
                  : KinematicHistoryMode::Continuous;
    const PoseMap &poses =
        poseAtFraction((substep + 1.0) / substeps_);
    driveKinematicBodies(poses, fixedDt, historyMode);
    applyDynamicForces(poses, sampleTime, forcing_period);
    backend_->step(fixedDt);
    current_contact_count_ = backend_->getContactCount();
    maximum_contact_count_ =
        std::max(maximum_contact_count_, current_contact_count_);
    const double stepPenetration = backend_->getMaximumPenetration();
    current_step_maximum_penetration_ =
        std::max(current_step_maximum_penetration_, stepPenetration);
    if (stepPenetration > periodic_interval_maximum_penetration_) {
      periodic_interval_maximum_penetration_ = stepPenetration;
      periodic_interval_maximum_penetration_time_ = sampleTime;
    }
    maximum_penetration_ = std::max(maximum_penetration_, stepPenetration);
    if (step_trace_.enabled) {
      captureStepTraceSample(poses,
                             fixed_substep_stats_.output_step_count - 1,
                             substep, sampleTime, output_dt, fixedDt);
    }
    previousSubstepSampleTime = sampleTime;
  }
  periodic_sample_time_ = end_sample_time;
}

RigidBodyRuntimeFingerprint RigidBodyBakeSession::runtimeFingerprint() const {
  RigidBodyRuntimeFingerprint fingerprint;
  fingerprint.bake_fps = last_output_dt_ > 0.0 ? 1.0 / last_output_dt_ : 0.0;
  fingerprint.fixed_substeps = substeps_;
  fingerprint.physics_dt = last_output_dt_;
  fingerprint.substep_dt = last_substep_dt_;
  fingerprint.snapshot_level = snapshot_level_;
  fingerprint.bullet_version = backend_->getNativeBulletVersion();
  fingerprint.solver_iterations = config_.solver_iterations;
#if defined(__FAST_MATH__)
  fingerprint.fast_math_enabled = true;
  fingerprint.floating_point_mode = "fast-math-enabled";
#endif
  fingerprint.unit_scale = config_.rigid_body_unit_scale;
  fingerprint.linear_damping = config_.rigid_body_linear_damping;
  fingerprint.angular_damping = config_.rigid_body_angular_damping;
  fingerprint.gravity_y = config_.gravity_y;
  fingerprint.real_gravity_field = config_.enable_real_gravity_field;
  fingerprint.ground_collision_enabled = config_.enable_ground_collision;
  fingerprint.wind_speed = config_.wind_speed;
  fingerprint.wind_direction_degrees = config_.wind_direction_degrees;
  fingerprint.wind_elevation_degrees = config_.wind_elevation_degrees;
  fingerprint.use_wind_components = config_.use_wind_components;
  fingerprint.wind_components = {config_.wind_x, config_.wind_y,
                                 config_.wind_z};
  fingerprint.movement_speed = config_.movement_speed;
  fingerprint.movement_direction_degrees =
      config_.movement_direction_degrees;
  fingerprint.movement_elevation_degrees =
      config_.movement_elevation_degrees;
  fingerprint.air_drag = config_.air_drag;
  fingerprint.turbulence = config_.turbulence;

  std::map<std::string, std::pair<const BodyDefinition *, bool>> definitions;
  for (const auto &[name, definition] : physics_body_definitions_) {
    definitions[name] = {&definition, true};
  }
  for (const auto &[name, definition] : kinematic_body_definitions_) {
    definitions.try_emplace(name, &definition, false);
  }
  fingerprint.shape_topology.reserve(definitions.size());
  for (const auto &[name, definition] : definitions) {
    RuntimeBodyTopology topology;
    topology.name = name;
    topology.motion_type = definition.first->motion_type;
    topology.physics_body = definition.second;
    topology.compound_shape = definition.first->boxes.size() > 1;
    topology.boxes = definition.first->boxes;
    fingerprint.shape_topology.push_back(std::move(topology));
  }
  return fingerprint;
}

const RigidBodyStepTrace &RigidBodyBakeSession::stepTrace() const {
  normalizeStepTraceOrder();
  return step_trace_;
}

void RigidBodyBakeSession::normalizeStepTraceOrder() const {
  if (step_trace_write_index_ == 0 || step_trace_.samples.empty()) {
    return;
  }
  std::rotate(step_trace_.samples.begin(),
              step_trace_.samples.begin() +
                  static_cast<std::ptrdiff_t>(step_trace_write_index_),
              step_trace_.samples.end());
  step_trace_write_index_ = 0;
}

void RigidBodyBakeSession::captureStepTraceSample(
    const PoseMap &poses, std::uint64_t output_step_index, int substep_index,
    double sample_time, double physics_dt, double substep_dt) {
  if (!step_trace_.enabled) {
    return;
  }
  RigidBodyStepTraceSample sample = std::move(trace_sample_scratch_);
  sample.output_step_index = output_step_index;
  sample.substep_index = substep_index;
  sample.sample_time = sample_time;
  sample.physics_dt = physics_dt;
  sample.substep_dt = substep_dt;
  sample.bodies.clear();
  sample.contact_summary = {};
  sample.contacts.clear();
  sample.contacts_truncated = false;
  sample.joint_errors.clear();

  sample.bodies.reserve(trace_body_slots_.size());
  for (const TraceBodySlot &slot : trace_body_slots_) {
    RigidBodyTraceBody body;
    body.name = slot.name;
    body.physics_body = slot.physics_body;
    body.motion_type = slot.motion_type;
    if (const auto pose = poses.find(slot.name); pose != poses.end()) {
      body.driver_bone_transform =
          BedrockPoseConverter::fromPose(pose->second, unit_scale_);
    }
    body.simulated = backend_->getBodyState(slot.handle);
    sample.bodies.push_back(std::move(body));
  }

  sample.contacts = backend_->getContactSnapshots();
  for (ContactSnapshot &contact : sample.contacts) {
    if (contact.body_b.name < contact.body_a.name) {
      std::swap(contact.body_a, contact.body_b);
    }
    ++sample.contact_summary.contact_count;
    if (contact.body_a.name == kGroundBodyName ||
        contact.body_b.name == kGroundBodyName) {
      ++sample.contact_summary.ground_contacts;
    } else {
      ++sample.contact_summary.self_contacts;
    }
    if (contact.penetration > sample.contact_summary.maximum_penetration) {
      sample.contact_summary.maximum_penetration = contact.penetration;
      sample.contact_summary.worst_pair =
          std::make_pair(contact.body_a, contact.body_b);
    }
  }
  std::sort(sample.contacts.begin(), sample.contacts.end(),
            [](const ContactSnapshot &first, const ContactSnapshot &second) {
              if (first.body_a.name != second.body_a.name) {
                return first.body_a.name < second.body_a.name;
              }
              if (first.body_b.name != second.body_b.name) {
                return first.body_b.name < second.body_b.name;
              }
              return first.penetration < second.penetration;
            });
  constexpr std::size_t kMaximumTraceContactsPerSample = 64;
  if (sample.contacts.size() > kMaximumTraceContactsPerSample) {
    sample.contacts.resize(kMaximumTraceContactsPerSample);
    sample.contacts_truncated = true;
  }
  sample.contact_summary.warning_threshold =
      config_.rigid_body_maximum_safe_penetration * 0.5;
  sample.contact_summary.failure_threshold =
      config_.rigid_body_maximum_safe_penetration;
  sample.contact_summary.warning =
      sample.contact_summary.maximum_penetration >
          sample.contact_summary.warning_threshold &&
      sample.contact_summary.maximum_penetration <=
          sample.contact_summary.failure_threshold;
  sample.contact_summary.unsafe =
      sample.contact_summary.maximum_penetration >
      sample.contact_summary.failure_threshold;

  sample.joint_errors = backend_->getJointErrorSnapshots();
  std::sort(sample.joint_errors.begin(), sample.joint_errors.end(),
            [](const JointErrorSnapshot &first,
               const JointErrorSnapshot &second) {
              return std::tie(first.parent_body, first.child_body) <
                     std::tie(second.parent_body, second.child_body);
            });

  ++step_trace_.captured_sample_count;
  const std::size_t capacity =
      static_cast<std::size_t>(step_trace_.capacity);
  if (step_trace_.samples.size() < capacity) {
    step_trace_.samples.push_back(std::move(sample));
  } else {
    std::swap(step_trace_.samples[step_trace_write_index_], sample);
    trace_sample_scratch_ = std::move(sample);
    step_trace_write_index_ =
        (step_trace_write_index_ + 1U) % capacity;
    ++step_trace_.dropped_sample_count;
  }
}

void RigidBodyBakeSession::analyzeFixedStepRisk(
    double start_sample_time, double end_sample_time, double output_dt,
    bool continuous_history, double forcing_period,
    const PoseAtFraction &pose_at_fraction) {
  const std::vector<double> probeFractions = motionProbeFractions(
      start_sample_time, end_sample_time, continuous_history, forcing_period);
  const PoseMap &endpointPoses = pose_at_fraction(1.0);

  int recommended = substeps_;
  fixed_substep_stats_.last_kinematic_required = substeps_;
  fixed_substep_stats_.last_dynamic_required = substeps_;
  for (const auto &slot : kinematic_runtime_slots_) {
    const std::string &boneName = slot.bone_name;
    auto poseIt = endpointPoses.find(boneName);
    if (poseIt == endpointPoses.end() || !slot.has_motion_geometry) {
      continue;
    }
    const Transform endpoint =
        BedrockPoseConverter::fromPose(poseIt->second, unit_scale_);
    Transform previous =
        slot.has_previous_transform ? slot.previous_transform : endpoint;
    double maximumEquivalentLinearTravel = 0.0;
    double maximumEquivalentAngularTravel = 0.0;
    bool completePath = true;
    double previousFraction = 0.0;
    for (const double fraction : probeFractions) {
      if (!(fraction > previousFraction)) {
        continue;
      }
      const PoseMap &poses = pose_at_fraction(fraction);
      const auto sampleIt = poses.find(boneName);
      if (sampleIt == poses.end()) {
        completePath = false;
        break;
      }
      const Transform next =
          BedrockPoseConverter::fromPose(sampleIt->second, unit_scale_);
      const double dx = next.translation[0] - previous.translation[0];
      const double dy = next.translation[1] - previous.translation[1];
      const double dz = next.translation[2] - previous.translation[2];
      const double fractionSpan = fraction - previousFraction;
      maximumEquivalentLinearTravel =
          std::max(maximumEquivalentLinearTravel,
                   std::sqrt(dx * dx + dy * dy + dz * dz) / fractionSpan);
      double quaternionDot = 0.0;
      for (int component = 0; component < 4; ++component) {
        quaternionDot +=
            previous.rotation[static_cast<std::size_t>(component)] *
            next.rotation[static_cast<std::size_t>(component)];
      }
      maximumEquivalentAngularTravel = std::max(
          maximumEquivalentAngularTravel,
          2.0 * std::acos(std::clamp(std::abs(quaternionDot), 0.0, 1.0)) /
              fractionSpan);
      previous = next;
      previousFraction = fraction;
    }
    if (!completePath) {
      continue;
    }

    const double minimumHalfExtent = slot.minimum_half_extent;
    const double maximumRadius = slot.maximum_radius;
    if (!std::isfinite(minimumHalfExtent) || !(minimumHalfExtent > 0.0)) {
      continue;
    }






    const double travelBound = maximumEquivalentLinearTravel +
                               maximumEquivalentAngularTravel * maximumRadius;
    const double allowedTravel = std::max(1e-9, minimumHalfExtent * 0.5);
    const int bodyRequired =
        requiredSubstepsForTravel(travelBound, allowedTravel);
    fixed_substep_stats_.last_kinematic_required =
        std::max(fixed_substep_stats_.last_kinematic_required,
                 bodyRequired);
    if (bodyRequired >= fixed_substep_stats_.maximum_recommended &&
        bodyRequired >= recommended) {
      fixed_substep_stats_.worst_body = boneName;
      fixed_substep_stats_.worst_motion_source = "Kinematic";
      fixed_substep_stats_.worst_frame =
          static_cast<int>(fixed_substep_stats_.output_step_count);
      fixed_substep_stats_.worst_minimum_half_extent = minimumHalfExtent;
      fixed_substep_stats_.worst_maximum_radius = maximumRadius;
      fixed_substep_stats_.worst_equivalent_linear_travel =
          maximumEquivalentLinearTravel;
      fixed_substep_stats_.worst_equivalent_angular_travel =
          maximumEquivalentAngularTravel;
      fixed_substep_stats_.worst_acceleration = 0.0;
    }
    recommended = std::max(recommended, bodyRequired);
  }

  for (const std::size_t slotIndex : dynamic_risk_slot_indices_) {
    const DynamicRuntimeSlot &slot = dynamic_runtime_slots_[slotIndex];
    const std::string &boneName = slot.bone_name;
    if (!slot.has_motion_geometry ||
        !std::isfinite(slot.minimum_half_extent) ||
        !(slot.minimum_half_extent > 0.0)) {
      continue;
    }

    const BodyState state = backend_->getBodyState(slot.handle);
    const double linearSpeed = vectorLength(state.com_linear_velocity);
    const double angularSpeed = vectorLength(state.angular_velocity);
    const bool accelerationDependsOnPose = slot.pull_compliance > 0.0;
    double maximumAcceleration = 0.0;
    for (const double fraction : probeFractions) {
      const PoseMap &poses =
          accelerationDependsOnPose ? pose_at_fraction(fraction)
                                    : endpointPoses;
      const double sampleTime = intervalSampleTime(
          start_sample_time, end_sample_time, continuous_history,
          forcing_period, fraction);
      auto force = evaluateAdditionalDynamicForce(
          slot, state, poses, sampleTime, forcing_period);
      for (double &component : force) {
        component /= slot.mass;
      }


      force[1] += base_gravity_acceleration_;
      maximumAcceleration =
          std::max(maximumAcceleration, vectorLength(force));
    }

    const double equivalentLinearTravel =
        linearSpeed * output_dt +
        0.5 * maximumAcceleration * output_dt * output_dt;
    const double equivalentAngularTravel = angularSpeed * output_dt;
    const double travelBound = equivalentLinearTravel +
                               equivalentAngularTravel *
                                   slot.maximum_radius;
    const double allowedTravel =
        std::max(1e-9, slot.minimum_half_extent * 0.5);
    const int bodyRequired =
        requiredSubstepsForTravel(travelBound, allowedTravel);
    fixed_substep_stats_.last_dynamic_required =
        std::max(fixed_substep_stats_.last_dynamic_required, bodyRequired);
    fixed_substep_stats_.maximum_dynamic_required =
        std::max(fixed_substep_stats_.maximum_dynamic_required,
                 bodyRequired);
    if (bodyRequired >= fixed_substep_stats_.maximum_recommended &&
        bodyRequired >= recommended) {
      fixed_substep_stats_.worst_body = boneName;
      fixed_substep_stats_.worst_motion_source = "Dynamic";
      fixed_substep_stats_.worst_frame =
          static_cast<int>(fixed_substep_stats_.output_step_count);
      fixed_substep_stats_.worst_minimum_half_extent =
          slot.minimum_half_extent;
      fixed_substep_stats_.worst_maximum_radius = slot.maximum_radius;
      fixed_substep_stats_.worst_equivalent_linear_travel =
          equivalentLinearTravel;
      fixed_substep_stats_.worst_equivalent_angular_travel =
          equivalentAngularTravel;
      fixed_substep_stats_.worst_acceleration = maximumAcceleration;
    }
    recommended = std::max(recommended, bodyRequired);
  }
  fixed_substep_stats_.maximum_recommended =
      std::max(fixed_substep_stats_.maximum_recommended, recommended);
  if (recommended > substeps_) {
    ++fixed_substep_stats_.insufficient_step_risk_count;
  }
}

void RigidBodyBakeSession::driveKinematicBodies(
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    double fixed_dt, KinematicHistoryMode history_mode) {
  for (auto &slot : kinematic_runtime_slots_) {
    const std::string &boneName = slot.bone_name;
    Transform next = BedrockPoseConverter::fromPose(
        requirePose(poses, boneName), unit_scale_);
    KinematicHistoryMode bodyHistoryMode = history_mode;
    if (history_mode == KinematicHistoryMode::Continuous &&
        slot.has_previous_transform && slot.has_motion_geometry) {
      const Transform &previous = slot.previous_transform;
      const double pivotDx = next.translation[0] - previous.translation[0];
      const double pivotDy = next.translation[1] - previous.translation[1];
      const double pivotDz = next.translation[2] - previous.translation[2];
      const double pivotDelta = std::sqrt(
          pivotDx * pivotDx + pivotDy * pivotDy + pivotDz * pivotDz);

      Transform previousCom = previous;
      Transform nextCom = next;
      if (slot.has_bone_to_com) {
        previousCom = multiplyTransforms(previous, slot.bone_to_com);
        nextCom = multiplyTransforms(next, slot.bone_to_com);
      }
      const double comDx =
          nextCom.translation[0] - previousCom.translation[0];
      const double comDy =
          nextCom.translation[1] - previousCom.translation[1];
      const double comDz =
          nextCom.translation[2] - previousCom.translation[2];
      const double comDelta =
          std::sqrt(comDx * comDx + comDy * comDy + comDz * comDz);

      double quaternionDot = 0.0;
      for (std::size_t component = 0; component < 4; ++component) {
        quaternionDot +=
            previous.rotation[component] * next.rotation[component];
      }
      const double angularDelta =
          2.0 * std::acos(std::clamp(std::abs(quaternionDot), 0.0, 1.0));

      if (std::isfinite(slot.minimum_half_extent) &&
          slot.minimum_half_extent > 0.0) {
        const double equivalentTravel =
            std::max(pivotDelta, comDelta) +
            angularDelta * slot.maximum_radius;



        const double detectionThreshold =
            std::max(1e-9, slot.minimum_half_extent * 0.5);
        if (equivalentTravel > detectionThreshold) {
          ++kinematic_history_stats_.insufficient_step_risks;
          kinematic_history_stats_.last_step_risk_body = boneName;
          kinematic_history_stats_.last_pivot_delta = pivotDelta;
          kinematic_history_stats_.last_com_delta = comDelta;
          kinematic_history_stats_.last_angular_delta_radians = angularDelta;
          kinematic_history_stats_.last_equivalent_travel = equivalentTravel;
          kinematic_history_stats_.last_detection_threshold =
              detectionThreshold;
          kinematic_history_stats_.last_fixed_dt = fixed_dt;
        }
      }
    }

    backend_->setKinematicTransform(slot.handle, next, fixed_dt,
                                    bodyHistoryMode);
    switch (bodyHistoryMode) {
    case KinematicHistoryMode::Continuous:
      ++kinematic_history_stats_.continuous_updates;
      break;
    case KinematicHistoryMode::PeriodicWrapResetVelocity:
      ++kinematic_history_stats_.periodic_updates;
      kinematic_history_stats_.last_reason =
          KinematicDiscontinuityReason::PeriodicWrap;
      break;
    case KinematicHistoryMode::TeleportResetVelocity:
      ++kinematic_history_stats_.teleport_resets;
      kinematic_history_stats_.last_teleport_body = boneName;
      kinematic_history_stats_.last_reason =
          KinematicDiscontinuityReason::ExplicitHistoryReset;
      kinematic_history_stats_.last_fixed_dt = fixed_dt;
      break;
    }
    slot.previous_transform = next;
    slot.has_previous_transform = true;
  }
}

std::array<double, 3> RigidBodyBakeSession::evaluateAdditionalDynamicForce(
    const DynamicRuntimeSlot &slot, const BodyState &state,
    const PoseMap &poses, double sample_time, double forcing_period) const {
  const auto &velocity = state.com_linear_velocity;
  const double gravityForce =
      slot.mass * config_.gravity_y * unit_scale_ *
      (slot.gravity_scale - 1.0);
  std::array<double, 3> force{
      (air_velocity_[0] - velocity[0]) * config_.air_drag *
          slot.wind_influence * slot.mass,
      (air_velocity_[1] - velocity[1]) * config_.air_drag *
              slot.wind_influence * slot.mass +
          gravityForce,
      (air_velocity_[2] - velocity[2]) * config_.air_drag *
          slot.wind_influence * slot.mass};

  if (slot.turbulence_amplitude > 0.0) {
    double basePhase = sample_time;
    if (forcing_period > 0.0) {
      double wrapped = std::fmod(sample_time, forcing_period);
      if (wrapped < 0.0) {
        wrapped += forcing_period;
      }
      basePhase = 2.0 * 3.14159265358979323846 *
                  (wrapped / forcing_period);
    }
    force[0] +=
        std::sin(basePhase + slot.turbulence_phase) *
        slot.turbulence_amplitude;
    force[1] +=
        std::sin(basePhase * 2.0 + slot.turbulence_phase * 1.7) *
        slot.turbulence_amplitude;
    force[2] +=
        std::sin(basePhase * 3.0 + slot.turbulence_phase * 2.3) *
        slot.turbulence_amplitude;
  }

  const auto reference = poses.find(slot.bone_name);
  if (slot.pull_compliance > 0.0 && reference != poses.end()) {
    const Transform targetBone =
        BedrockPoseConverter::fromPose(reference->second, unit_scale_);
    const Transform targetCom =
        slot.has_bone_to_com
            ? multiplyTransforms(targetBone, slot.bone_to_com)
            : targetBone;
    const auto &target = targetCom.translation;
    const auto &current = state.com_transform.translation;
    for (int axis = 0; axis < 3; ++axis) {
      force[static_cast<std::size_t>(axis)] +=
          slot.mass *
          (slot.pull_stiffness *
               (target[static_cast<std::size_t>(axis)] -
                current[static_cast<std::size_t>(axis)]) -
           slot.pull_damping * velocity[static_cast<std::size_t>(axis)]);
    }
  }
  return force;
}

void RigidBodyBakeSession::applyDynamicForces(
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    double sample_time, double forcing_period) {
  for (const auto &slot : dynamic_runtime_slots_) {
    const BodyState state = backend_->getBodyState(slot.handle);
    const auto force = evaluateAdditionalDynamicForce(
        slot, state, poses, sample_time, forcing_period);
    backend_->applyCentralForce(slot.handle, force);
  }
}

std::vector<RigidBodyBakeSession::BoneOutput>
RigidBodyBakeSession::captureBoneOutputs(
    const std::map<std::string, baker::BonePoseCalculator::Pose>
        &reference_poses) const {
  std::vector<BoneOutput> outputs;
  captureBoneOutputsInto(reference_poses, outputs);
  return outputs;
}

void RigidBodyBakeSession::captureBoneOutputsInto(
    const std::map<std::string, baker::BonePoseCalculator::Pose>
        &reference_poses,
    std::vector<BoneOutput> &outputs) const {
  if (capture_body_states_.size() != capture_runtime_slots_.size() ||
      capture_body_query_slot_indices_.size() !=
          capture_runtime_slots_.size()) {
    throw std::logic_error("capture runtime state layout is invalid");
  }
  for (const std::size_t slotIndex : capture_body_query_slot_indices_) {
    const CaptureRuntimeSlot &slot = capture_runtime_slots_[slotIndex];
    BodyState &state = capture_body_states_[slotIndex];
    state = backend_->getBodyState(slot.handle);
    if (slot.physics_target == nullptr) {
      throw std::logic_error("capture runtime target is missing: " +
                             slot.bone_name);
    }
    slot.physics_target->position = {
        state.bone_transform.translation[0] / unit_scale_,
        state.bone_transform.translation[1] / unit_scale_,
        state.bone_transform.translation[2] / unit_scale_};
    slot.physics_target->rotation = state.bone_transform.rotation;
  }




  const auto &reconstructed =
      final_pose_reconstructor_evaluator_.reconstructInto(
          reference_poses, capture_physics_targets_,
          final_pose_reconstruction_scratch_);

  outputs.resize(capture_runtime_slots_.size());
  for (std::size_t slotIndex = 0; slotIndex < capture_runtime_slots_.size();
       ++slotIndex) {
    const CaptureRuntimeSlot &slot = capture_runtime_slots_[slotIndex];
    if (slot.local_channels == nullptr || slot.world_pose == nullptr) {
      const auto channels =
          reconstructed.local_channels.find(slot.bone_name);
      const auto worldPose =
          reconstructed.world_poses.find(slot.bone_name);
      if (channels == reconstructed.local_channels.end() ||
          worldPose == reconstructed.world_poses.end()) {
        throw std::logic_error(
            "final pose reconstruction omitted physics bone: " +
            slot.bone_name);
      }
      slot.local_channels = &channels->second;
      slot.world_pose = &worldPose->second;
    }
    const BodyState &state = capture_body_states_[slotIndex];
    BoneOutput &out = outputs[slotIndex];
    if (out.bone_name != slot.bone_name) {
      out.bone_name = slot.bone_name;
    }
    out.position = slot.local_channels->position;
    out.rotation = slot.local_channels->rotation;
    out.linear_velocity = {state.bone_linear_velocity[0] / unit_scale_,
                           state.bone_linear_velocity[1] / unit_scale_,
                           state.bone_linear_velocity[2] / unit_scale_};
    out.world_position = slot.world_pose->world_position;
  }
}

int RigidBodyBakeSession::getUnsafeCollisionCount() const {
  return current_step_maximum_penetration_ >
                 config_.rigid_body_maximum_safe_penetration
             ? 1
             : 0;
}

baker::PeriodicStateAdapter::Snapshot
RigidBodyBakeSession::capturePeriodicSnapshot() {
  baker::PeriodicStateAdapter::Snapshot snap;
  snap.required_metrics.position = true;
  snap.required_metrics.rotation = true;
  snap.required_metrics.linear_velocity = true;
  snap.required_metrics.angular_velocity = true;
  snap.required_metrics.contacts = true;
  snap.required_metrics.maximum_penetration = true;
  snap.required_metrics.sample_time = true;
  snap.has_contacts = true;
  snap.has_maximum_penetration = true;
  snap.has_maximum_penetration_time = true;
  snap.has_sample_time = true;
  snap.sample_time = periodic_sample_time_;
  snap.enter_contact_threshold = std::max(
      1e-8, config_.rigid_body_maximum_safe_penetration * 0.01);
  snap.exit_contact_threshold = snap.enter_contact_threshold * 0.5;
  snap.contact_penetration_bucket_width =
      snap.enter_contact_threshold * 4.0;
  for (const auto &[name, handle] : physics_bodies_) {
    snap.expected_bones.insert(name);
    const BodyState state = backend_->getBodyState(handle);
    baker::PeriodicStateAdapter::BoneState bone;
    bone.position = {state.bone_transform.translation[0] / unit_scale_,
                     state.bone_transform.translation[1] / unit_scale_,
                     state.bone_transform.translation[2] / unit_scale_};
    bone.rotation_quaternion = state.bone_transform.rotation;
    bone.has_rotation = true;
    bone.linear_velocity = {state.bone_linear_velocity[0] / unit_scale_,
                            state.bone_linear_velocity[1] / unit_scale_,
                            state.bone_linear_velocity[2] / unit_scale_};
    bone.angular_velocity = state.angular_velocity;
    bone.has_angular_velocity = true;
    bone.com_position = {state.com_transform.translation[0] / unit_scale_,
                         state.com_transform.translation[1] / unit_scale_,
                         state.com_transform.translation[2] / unit_scale_};
    bone.has_com_position = true;
    bone.com_rotation_quaternion = state.com_transform.rotation;
    bone.has_com_rotation = true;
    bone.com_linear_velocity = {
        state.com_linear_velocity[0] / unit_scale_,
        state.com_linear_velocity[1] / unit_scale_,
        state.com_linear_velocity[2] / unit_scale_};
    bone.has_com_linear_velocity = true;
    snap.bones.emplace(name, bone);
  }
  for (const auto &contact : backend_->getContactSnapshots()) {
    const std::string key =
        canonicalContactKey(contact.body_a.name, contact.body_b.name);
    snap.contacts.insert(key);
    const auto existing = snap.contact_penetrations.find(key);
    if (existing == snap.contact_penetrations.end()) {
      snap.contact_penetrations.emplace(key, contact.penetration);
    } else if (!std::isfinite(existing->second) ||
               !std::isfinite(contact.penetration)) {
      existing->second = std::numeric_limits<double>::quiet_NaN();
    } else {
      existing->second = std::max(existing->second, contact.penetration);
    }
  }
  for (const auto &[pair, penetration] : snap.contact_penetrations) {
    baker::LoopContactSignature signature;
    signature.pair = pair;
    signature.state =
        std::isfinite(penetration) &&
                std::max(0.0, penetration) >= snap.enter_contact_threshold
            ? baker::LoopContactState::MeaningfulPenetration
            : baker::LoopContactState::Touching;
    if (std::isfinite(penetration) && penetration > 0.0) {
      signature.penetration_bucket = static_cast<int>(std::min(
          static_cast<double>(std::numeric_limits<int>::max()),
          std::floor(penetration /
                     snap.contact_penetration_bucket_width)));
    }
    snap.contact_signatures.emplace(pair, std::move(signature));
  }


  snap.maximum_penetration = periodic_interval_maximum_penetration_;
  snap.maximum_penetration_time =
      periodic_interval_maximum_penetration_time_;
  periodic_interval_maximum_penetration_ = 0.0;
  periodic_interval_maximum_penetration_time_ = -1.0;
  return snap;
}

int RigidBodyBakeSession::getNativeBulletVersion() const {
  return backend_->getNativeBulletVersion();
}

int RigidBodyBakeSession::hierarchyDepth(const std::string &bone_name) const {
  int depth = 0;
  auto it = bones_by_name_.find(bone_name);
  while (it != bones_by_name_.end() && it->second.has_parent) {
    depth++;
    it = bones_by_name_.find(it->second.parent);
  }
  return depth;
}

std::string RigidBodyBakeSession::nearestPhysicsAncestor(
    const std::string &bone_name) const {
  auto it = bones_by_name_.find(bone_name);
  std::set<std::string> visited;
  while (it != bones_by_name_.end() && it->second.has_parent &&
         visited.insert(it->second.name).second) {
    const std::string &parent = it->second.parent;
    if (bone_mapper_.isPhysicsBone(parent)) {
      return parent;
    }
    it = bones_by_name_.find(parent);
  }
  return {};
}

const loader::Bone &
RigidBodyBakeSession::requireBone(const std::string &name) const {
  auto it = bones_by_name_.find(name);
  if (it == bones_by_name_.end()) {
    throw std::invalid_argument("missing bone: " + name);
  }
  return it->second;
}

baker::BonePoseCalculator::Pose RigidBodyBakeSession::requirePose(
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    const std::string &name) const {
  auto it = poses.find(name);
  if (it == poses.end()) {
    throw std::invalid_argument("missing pose: " + name);
  }
  return it->second;
}

}
