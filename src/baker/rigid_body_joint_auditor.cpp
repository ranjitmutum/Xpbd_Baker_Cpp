#include "xpbd/baker/rigid_body_joint_auditor.hpp"

#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/export/animation_exporter.hpp"

#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint.h>
#include <LinearMath/btMatrix3x3.h>
#include <LinearMath/btQuaternion.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>

namespace xpbd::baker {
namespace {

using Pose = BonePoseCalculator::Pose;
using PoseMap = RigidBodyJointAuditor::PoseMap;
using Quat = RotationUtil::Quat;
using Vec3 = RotationUtil::Vec3;

const Pose &requirePose(const PoseMap &poses, const std::string &bone_name,
                        const char *role) {
  const auto found = poses.find(bone_name);
  if (found == poses.end()) {
    throw std::invalid_argument(std::string("missing ") + role +
                                " joint audit pose for bone: " + bone_name);
  }
  return found->second;
}

void requireFiniteVector(const Vec3 &value, const std::string &label) {
  if (!std::all_of(value.begin(), value.end(),
                   [](double component) { return std::isfinite(component); })) {
    throw std::invalid_argument(label + " must be finite");
  }
}

Quat normalizedQuaternion(const Quat &value, const std::string &label) {
  double normSquared = 0.0;
  for (const double component : value) {
    if (!std::isfinite(component)) {
      throw std::invalid_argument(label + " must be finite");
    }
    normSquared += component * component;
  }
  if (!std::isfinite(normSquared) || normSquared <= 1e-24) {
    throw std::invalid_argument(label + " must have non-zero length");
  }
  const double inverseNorm = 1.0 / std::sqrt(normSquared);
  return {value[0] * inverseNorm, value[1] * inverseNorm,
          value[2] * inverseNorm, value[3] * inverseNorm};
}

Vec3 subtract(const Vec3 &first, const Vec3 &second) {
  return {first[0] - second[0], first[1] - second[1], first[2] - second[2]};
}

Vec3 add(const Vec3 &first, const Vec3 &second) {
  return {first[0] + second[0], first[1] + second[1], first[2] + second[2]};
}

double length(const Vec3 &value) {
  return std::sqrt(value[0] * value[0] + value[1] * value[1] +
                   value[2] * value[2]);
}

std::string
nearestPhysicsAncestor(const std::string &bone_name,
                       const std::map<std::string, loader::Bone> &bones_by_name,
                       const std::set<std::string> &physics_bones) {
  auto bone = bones_by_name.find(bone_name);
  std::set<std::string> visited;
  while (bone != bones_by_name.end() && bone->second.has_parent &&
         visited.insert(bone->second.name).second) {
    const std::string &parent = bone->second.parent;
    if (physics_bones.contains(parent)) {
      return parent;
    }
    bone = bones_by_name.find(parent);
  }
  return {};
}

struct BulletEulerXyzResult {
  std::array<double, 3> angles{};
  bool unique = true;
};

BulletEulerXyzResult bulletEulerXyz(const Quat &relative_rotation) {
  const Quat normalized =
      normalizedQuaternion(relative_rotation, "relative joint rotation");
  const btQuaternion quaternion(static_cast<btScalar>(normalized[0]),
                                static_cast<btScalar>(normalized[1]),
                                static_cast<btScalar>(normalized[2]),
                                static_cast<btScalar>(normalized[3]));
  const btMatrix3x3 matrix(quaternion);
  btVector3 angles;
  const bool unique =
      btGeneric6DofSpring2Constraint::matrixToEulerXYZ(matrix, angles);
  BulletEulerXyzResult result{{static_cast<double>(angles.x()),
                               static_cast<double>(angles.y()),
                               static_cast<double>(angles.z())},
                              unique};
  if (!std::all_of(result.angles.begin(), result.angles.end(),
                   [](double component) { return std::isfinite(component); })) {
    throw std::invalid_argument("relative joint Euler angles must be finite");
  }
  return result;
}

}

RigidBodyJointAuditor::RigidBodyJointAuditor(const BoneMapper &bone_mapper,
                                             const PoseMap &initial_poses)
    : RigidBodyJointAuditor(bone_mapper, initial_poses, Tolerances{}) {}

RigidBodyJointAuditor::RigidBodyJointAuditor(const BoneMapper &bone_mapper,
                                             const PoseMap &initial_poses,
                                             Tolerances tolerances)
    : pose_evaluator_(BonePoseCalculator::compile(bone_mapper.allBones())),
      tolerances_(tolerances),
      unit_scale_(bone_mapper.config().rigid_body_unit_scale) {
  if (!std::isfinite(unit_scale_) || !(unit_scale_ > 0.0)) {
    throw std::invalid_argument(
        "joint audit unit scale must be finite and greater than zero");
  }
  if (!std::isfinite(tolerances_.linear_anchor_separation) ||
      tolerances_.linear_anchor_separation < 0.0) {
    throw std::invalid_argument(
        "joint audit linear tolerance must be finite and non-negative");
  }
  if (!std::isfinite(tolerances_.angular_limit_excess_radians) ||
      tolerances_.angular_limit_excess_radians < 0.0) {
    throw std::invalid_argument(
        "joint audit angular tolerance must be finite and non-negative");
  }

  std::map<std::string, loader::Bone> bonesByName;
  for (const auto &bone : bone_mapper.allBones()) {
    if (!bone.name.empty()) {
      bonesByName.emplace(bone.name, bone);
    }
  }
  const std::set<std::string> physicsBones(bone_mapper.physicsBones().begin(),
                                           bone_mapper.physicsBones().end());

  for (const std::string &childName : bone_mapper.physicsBones()) {
    const std::string parentName =
        nearestPhysicsAncestor(childName, bonesByName, physicsBones);
    if (parentName.empty() || (bone_mapper.isFixedBone(parentName) &&
                               bone_mapper.isFixedBone(childName))) {
      continue;
    }

    const Pose &initialParent =
        requirePose(initial_poses, parentName, "initial parent");
    const Pose &initialChild =
        requirePose(initial_poses, childName, "initial child");
    requireFiniteVector(initialParent.world_position,
                        "initial parent joint position");
    requireFiniteVector(initialChild.world_position,
                        "initial child joint position");
    const Quat parentRotation = normalizedQuaternion(
        initialParent.world_rotation, "initial parent joint rotation");
    const Quat childRotation = normalizedQuaternion(
        initialChild.world_rotation, "initial child joint rotation");

    JointDefinition joint;
    joint.parent_bone = parentName;
    joint.child_bone = childName;
    joint.parent_anchor_offset = RotationUtil::rotateVector(
        RotationUtil::quaternionInverse(parentRotation),
        subtract(initialChild.world_position, initialParent.world_position));
    joint.parent_anchor_rotation = RotationUtil::quaternionMultiply(
        RotationUtil::quaternionInverse(parentRotation), childRotation);

    const auto bendDegrees =
        bone_mapper.getEffectiveRigidBodyMaxBendDegrees(childName);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double bend = bendDegrees[axis];
      if (!std::isfinite(bend) || bend < 0.0 || bend > 180.0) {
        throw std::invalid_argument(
            "joint audit angular limit must be finite and in [0, 180]");
      }
      const bool limited =
          bone_mapper.config().enable_angle_constraints && bend < 180.0;
      joint.angular_axis_limited[axis] = limited;
      joint.angular_limit_radians[axis] =
          limited ? bend * std::numbers::pi_v<double> / 180.0 : 0.0;
    }
    joints_.push_back(std::move(joint));
  }
}

RigidBodyJointAuditor::AuditResult
RigidBodyJointAuditor::audit(const PoseMap &poses) const {
  AuditResult result;
  result.audited_joint_count = joints_.size();

  for (const JointDefinition &joint : joints_) {
    const Pose &parent = requirePose(poses, joint.parent_bone, "final parent");
    const Pose &child = requirePose(poses, joint.child_bone, "final child");
    requireFiniteVector(parent.world_position, "final parent joint position");
    requireFiniteVector(child.world_position, "final child joint position");
    const Quat parentRotation = normalizedQuaternion(
        parent.world_rotation, "final parent joint rotation");
    const Quat childRotation = normalizedQuaternion(
        child.world_rotation, "final child joint rotation");

    const Vec3 parentAnchorPosition = add(
        parent.world_position,
        RotationUtil::rotateVector(parentRotation, joint.parent_anchor_offset));
    const Quat parentAnchorRotation = RotationUtil::quaternionMultiply(
        parentRotation, joint.parent_anchor_rotation);
    const double separation =
        length(subtract(child.world_position, parentAnchorPosition)) *
        unit_scale_;
    const double linearExcess =
        std::max(0.0, separation - tolerances_.linear_anchor_separation);
    if (separation > result.maximum_linear_anchor_separation) {
      result.maximum_linear_anchor_separation = separation;
      result.worst_linear_parent = joint.parent_bone;
      result.worst_linear_child = joint.child_bone;
    }
    result.maximum_linear_anchor_excess =
        std::max(result.maximum_linear_anchor_excess, linearExcess);

    const Quat relativeRotation = RotationUtil::quaternionMultiply(
        RotationUtil::quaternionInverse(normalizedQuaternion(
            parentAnchorRotation, "final parent anchor rotation")),
        childRotation);
    const Quat normalizedRelativeRotation = normalizedQuaternion(
        relativeRotation, "final relative joint rotation");
    const auto euler = bulletEulerXyz(normalizedRelativeRotation);

    Violation violation;
    violation.parent_bone = joint.parent_bone;
    violation.child_bone = joint.child_bone;
    violation.relative_rotation_xyzw = normalizedRelativeRotation;
    violation.linear_anchor_separation = separation;
    violation.linear_anchor_excess = linearExcess;
    violation.linear_unsafe = linearExcess > 0.0;
    violation.angular_coordinates_radians = euler.angles;
    violation.joint_euler_singular = !euler.unique;
    bool jointUnsafe =
        violation.linear_unsafe || violation.joint_euler_singular;
    if (violation.joint_euler_singular) {
      ++result.euler_singular_joint_count;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
      if (!joint.angular_axis_limited[axis]) {
        continue;
      }
      const double limitExcess = std::max(
          0.0,
          std::abs(euler.angles[axis]) - joint.angular_limit_radians[axis]);
      violation.angular_limit_excess_radians[axis] = limitExcess;
      violation.angular_unsafe[axis] =
          limitExcess > tolerances_.angular_limit_excess_radians;
      jointUnsafe = jointUnsafe || violation.angular_unsafe[axis];
      if (limitExcess > result.maximum_angular_limit_excess_radians) {
        result.maximum_angular_limit_excess_radians = limitExcess;
        result.worst_angular_parent = joint.parent_bone;
        result.worst_angular_child = joint.child_bone;
        result.worst_angular_axis = static_cast<int>(axis);
      }
    }

    if (jointUnsafe) {
      result.unsafe = true;
      ++result.unsafe_joint_count;
      result.violations.push_back(std::move(violation));
    }
  }
  return result;
}

RigidBodyJointAuditor::AuditResult RigidBodyJointAuditor::auditQuantizedFrame(
    const BakedFrame &frame, const loader::Animation *reference_animation,
    double reference_time) const {
  if (!std::isfinite(reference_time)) {
    throw std::invalid_argument("joint audit reference time must be finite");
  }
  std::map<std::string, std::array<double, 3>> positions;
  std::map<std::string, std::array<double, 3>> rotations;
  for (const BoneState &state : frame.bone_states) {
    if (state.bone_name.empty()) {
      throw std::invalid_argument("joint audit bone name must not be blank");
    }
    std::array<double, 3> position{};
    std::array<double, 3> rotation{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      position[axis] = quantizeExportValue(state.position[axis]);
      rotation[axis] = quantizeExportValue(state.rotation[axis]);
    }
    positions[state.bone_name] = position;
    rotations[state.bone_name] = rotation;
  }
  for (const JointDefinition &joint : joints_) {
    for (const std::string *boneName :
         {&joint.parent_bone, &joint.child_bone}) {
      if (!positions.contains(*boneName) || !rotations.contains(*boneName)) {
        throw std::invalid_argument(
            "final joint audit frame is missing baked physics bone: " +
            *boneName);
      }
    }
  }
  return audit(pose_evaluator_.calculate(reference_animation, reference_time,
                                         &positions, &rotations));
}

double RigidBodyJointAuditor::quantizeExportValue(double value) {
  return export_::AnimationExporter::quantizeValue(value);
}

}
