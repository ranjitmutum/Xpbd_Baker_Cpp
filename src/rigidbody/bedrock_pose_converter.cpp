#include "xpbd/rigidbody/bedrock_pose_converter.hpp"

#include "xpbd/baker/bedrock_transform_resolver.hpp"
#include "xpbd/baker/rotation_util.hpp"

#include <cmath>
#include <stdexcept>

namespace xpbd::rigidbody {

namespace {
void requireUnitScale(double unit_scale) {
    if (!std::isfinite(unit_scale) || !(unit_scale > 0)) {
        throw std::invalid_argument("unit scale must be finite and greater than zero");
    }
}

double wrapDegrees(double value) {
    double wrapped = std::fmod(value, 360.0);
    if (wrapped > 180.0) {
        wrapped -= 360.0;
    }
    if (wrapped < -180.0) {
        wrapped += 360.0;
    }
    return wrapped;
}
}

Transform BedrockPoseConverter::fromPose(const baker::BonePoseCalculator::Pose& pose,
                                         double unit_scale) {
    requireUnitScale(unit_scale);
    Transform t;
    t.translation = {pose.world_position[0] * unit_scale, pose.world_position[1] * unit_scale,
                     pose.world_position[2] * unit_scale};
    t.rotation = pose.world_rotation;
    t.normalizeRotation();
    return t;
}

LocalChannels BedrockPoseConverter::toLocalChannels(
    const loader::Bone& bone, const Transform& world_pivot_transform,
    const loader::Bone* parent_bone, const Transform* parent_world_pivot_transform,
    double unit_scale) {
    requireUnitScale(unit_scale);
    if ((parent_bone == nullptr) != (parent_world_pivot_transform == nullptr)) {
        throw std::invalid_argument(
            "parent bone and parent world transform must be supplied together");
    }

    std::array<double, 4> parentRotation{0, 0, 0, 1};
    std::array<double, 3> parentModelTranslation{0, 0, 0};
    if (parent_bone != nullptr) {
        parentRotation = parent_world_pivot_transform->rotation;
        const auto parentPivot =
            baker::BedrockTransformResolver::convertBedrockVector(parent_bone->pivot);
        const auto rotatedParentPivot = baker::RotationUtil::rotateVector(
            parentRotation,
            baker::RotationUtil::Vec3{parentPivot[0] * unit_scale, parentPivot[1] * unit_scale,
                                      parentPivot[2] * unit_scale});
        const auto& parentWorld = parent_world_pivot_transform->translation;
        parentModelTranslation = {parentWorld[0] - rotatedParentPivot[0],
                                  parentWorld[1] - rotatedParentPivot[1],
                                  parentWorld[2] - rotatedParentPivot[2]};
    }

    const auto& worldPosition = world_pivot_transform.translation;
    const std::array<double, 3> relativePosition{
        worldPosition[0] - parentModelTranslation[0],
        worldPosition[1] - parentModelTranslation[1],
        worldPosition[2] - parentModelTranslation[2]};
    const auto localPivotPosition = baker::RotationUtil::rotateVector(
        baker::RotationUtil::quaternionInverse(parentRotation), relativePosition);
    const auto bonePivot = baker::BedrockTransformResolver::convertBedrockVector(bone.pivot);
    const std::array<double, 3> mappedAnimationPosition{
        localPivotPosition[0] / unit_scale - bonePivot[0],
        localPivotPosition[1] / unit_scale - bonePivot[1],
        localPivotPosition[2] / unit_scale - bonePivot[2]};
    const auto animationPosition =
        baker::BedrockTransformResolver::convertBedrockVector(mappedAnimationPosition);

    const auto localRotation = baker::RotationUtil::quaternionMultiply(
        baker::RotationUtil::quaternionInverse(parentRotation), world_pivot_transform.rotation);
    const auto totalLocalEuler = baker::RotationUtil::bedrockEulerFromQuaternion(localRotation);
    LocalChannels channels;
    channels.position = animationPosition;
    channels.rotation = {wrapDegrees(totalLocalEuler[0] - bone.rotation[0]),
                         wrapDegrees(totalLocalEuler[1] - bone.rotation[1]),
                         wrapDegrees(totalLocalEuler[2] - bone.rotation[2])};
    return channels;
}

}
