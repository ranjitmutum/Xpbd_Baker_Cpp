#include "xpbd/baker/final_pose_reconstructor.hpp"

#include "xpbd/baker/bedrock_transform_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace xpbd::baker {
namespace {

using Pose = BonePoseCalculator::Pose;
using Quat = RotationUtil::Quat;
using Vec3 = RotationUtil::Vec3;
using Linear = std::array<double, 9>;
using LocalChannels = FinalPoseReconstructor::LocalChannels;
using WorldTarget = FinalPoseReconstructor::WorldTarget;

Vec3 add(const Vec3 &a, const Vec3 &b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 subtract(const Vec3 &a, const Vec3 &b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Quat normalized(const Quat &value);

Vec3 transformVector(const Linear &matrix, const Vec3 &value) {
  return {matrix[0] * value[0] + matrix[1] * value[1] +
              matrix[2] * value[2],
          matrix[3] * value[0] + matrix[4] * value[1] +
              matrix[5] * value[2],
          matrix[6] * value[0] + matrix[7] * value[1] +
              matrix[8] * value[2]};
}

Linear multiplyLinear(const Linear &left, const Linear &right) {
  Linear result{};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      for (std::size_t inner = 0; inner < 3; ++inner) {
        result[row * 3 + column] +=
            left[row * 3 + inner] * right[inner * 3 + column];
      }
    }
  }
  return result;
}

Linear inverseLinear(const Linear &value) {
  const double determinant =
      value[0] * (value[4] * value[8] - value[5] * value[7]) -
      value[1] * (value[3] * value[8] - value[5] * value[6]) +
      value[2] * (value[3] * value[7] - value[4] * value[6]);
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-15) {
    throw std::invalid_argument(
        "parent affine transform must be finite and invertible");
  }
  const double inverse = 1.0 / determinant;
  return {(value[4] * value[8] - value[5] * value[7]) * inverse,
          (value[2] * value[7] - value[1] * value[8]) * inverse,
          (value[1] * value[5] - value[2] * value[4]) * inverse,
          (value[5] * value[6] - value[3] * value[8]) * inverse,
          (value[0] * value[8] - value[2] * value[6]) * inverse,
          (value[2] * value[3] - value[0] * value[5]) * inverse,
          (value[3] * value[7] - value[4] * value[6]) * inverse,
          (value[1] * value[6] - value[0] * value[7]) * inverse,
          (value[0] * value[4] - value[1] * value[3]) * inverse};
}

Linear rotationScaleLinear(const Quat &rotation, const Vec3 &scale) {
  const std::array<Vec3, 3> axes{
      RotationUtil::rotateVector(rotation, Vec3{1.0, 0.0, 0.0}),
      RotationUtil::rotateVector(rotation, Vec3{0.0, 1.0, 0.0}),
      RotationUtil::rotateVector(rotation, Vec3{0.0, 0.0, 1.0})};
  Linear result{};
  for (std::size_t column = 0; column < 3; ++column) {
    for (std::size_t row = 0; row < 3; ++row) {
      result[row * 3 + column] = axes[column][row] * scale[column];
    }
  }
  return result;
}

Pose composePose(const loader::Bone &bone, const LocalChannels &channels,
                 const Vec3 &local_scale, const Pose *final_parent,
                 const Quat &local_rotation, const Quat &world_rotation) {
  const Vec3 mapped_pivot =
      BedrockTransformResolver::convertBedrockVector(bone.pivot);
  const Vec3 mapped_animation =
      BedrockTransformResolver::convertBedrockVector(channels.position);
  const Linear local_linear =
      rotationScaleLinear(local_rotation, local_scale);
  const Vec3 transformed_negative_pivot = transformVector(
      local_linear, {-mapped_pivot[0], -mapped_pivot[1], -mapped_pivot[2]});
  const Vec3 local_translation{
      mapped_animation[0] + mapped_pivot[0] +
          transformed_negative_pivot[0],
      mapped_animation[1] + mapped_pivot[1] +
          transformed_negative_pivot[1],
      mapped_animation[2] + mapped_pivot[2] +
          transformed_negative_pivot[2]};

  Pose result;
  result.world_rotation = normalized(world_rotation);
  result.world_linear = local_linear;
  result.world_translation = local_translation;
  if (final_parent != nullptr) {
    result.world_linear =
        multiplyLinear(final_parent->world_linear, local_linear);
    result.world_translation =
        add(final_parent->world_translation,
            transformVector(final_parent->world_linear, local_translation));
  }
  result.world_position =
      add(result.world_translation,
          transformVector(result.world_linear, mapped_pivot));
  result.animation_position = channels.position;
  result.animation_scale = local_scale;
  result.total_local_euler = {bone.rotation[0] + channels.rotation[0],
                              bone.rotation[1] + channels.rotation[1],
                              bone.rotation[2] + channels.rotation[2]};
  return result;
}

Quat normalized(const Quat &value) {
  double length_squared = 0.0;
  for (double component : value) {
    if (!std::isfinite(component)) {
      throw std::invalid_argument("world target rotation must be finite");
    }
    length_squared += component * component;
  }
  if (!(length_squared > 1e-20)) {
    throw std::invalid_argument("world target rotation must be non-zero");
  }
  const double inverse_length = 1.0 / std::sqrt(length_squared);
  return {value[0] * inverse_length, value[1] * inverse_length,
          value[2] * inverse_length, value[3] * inverse_length};
}

void appendTopologically(const loader::Bone &bone,
                         const std::map<std::string, loader::Bone> &by_name,
                         std::map<std::string, int> &states,
                         std::vector<loader::Bone> &ordered) {
  auto state = states.find(bone.name);
  if (state != states.end()) {
    if (state->second == 1) {
      throw std::invalid_argument("Bone hierarchy contains a cycle at " +
                                  bone.name);
    }
    return;
  }
  states[bone.name] = 1;
  if (bone.has_parent) {
    auto parent = by_name.find(bone.parent);
    if (parent != by_name.end()) {
      appendTopologically(parent->second, by_name, states, ordered);
    }
  }
  states[bone.name] = 2;
  ordered.push_back(bone);
}

LocalChannels referenceChannels(const loader::Bone &bone,
                                const Pose &reference) {
  return {reference.animation_position,
          {reference.total_local_euler[0] - bone.rotation[0],
           reference.total_local_euler[1] - bone.rotation[1],
           reference.total_local_euler[2] - bone.rotation[2]}};
}

Quat referenceLocalRotation(const Pose &reference,
                            const Pose *reference_parent) {
  if (reference_parent == nullptr) {
    return reference.world_rotation;
  }
  return RotationUtil::quaternionMultiply(
      RotationUtil::quaternionInverse(reference_parent->world_rotation),
      reference.world_rotation);
}

Pose inheritReferenceLocal(const loader::Bone &bone, const Pose &reference,
                           const Pose *reference_parent,
                           const Pose *final_parent,
                           const LocalChannels &channels) {
  const Quat local_rotation =
      referenceLocalRotation(reference, reference_parent);
  const Quat world_rotation =
      final_parent == nullptr
          ? local_rotation
          : RotationUtil::quaternionMultiply(final_parent->world_rotation,
                                             local_rotation);
  return composePose(bone, channels, reference.animation_scale, final_parent,
                     local_rotation, world_rotation);
}

Quat desiredRotation(const Pose &reference, const Pose *reference_parent,
                     const Pose *final_parent, const WorldTarget &target) {
  if (target.rotation.has_value()) {
    return normalized(*target.rotation);
  }
  const Quat local_rotation =
      referenceLocalRotation(reference, reference_parent);
  return final_parent == nullptr
             ? normalized(local_rotation)
             : normalized(RotationUtil::quaternionMultiply(
                   final_parent->world_rotation, local_rotation));
}

LocalChannels solveLocalChannels(const loader::Bone &bone,
                                 const Pose &reference,
                                 const Pose *final_parent,
                                 const WorldTarget &target,
                                 const Quat &world_rotation) {
  const Quat parent_rotation =
      final_parent == nullptr ? Quat{0, 0, 0, 1} : final_parent->world_rotation;
  const Vec3 parent_translation =
      final_parent == nullptr ? Vec3{0, 0, 0} : final_parent->world_translation;
  const Quat local_rotation = RotationUtil::quaternionMultiply(
      RotationUtil::quaternionInverse(parent_rotation), world_rotation);
  const Vec3 total_local_euler = RotationUtil::unwrapEuler(
      reference.total_local_euler,
      RotationUtil::bedrockEulerFromQuaternion(local_rotation));

  const Vec3 local_pivot =
      final_parent == nullptr
          ? subtract(target.position, parent_translation)
          : transformVector(inverseLinear(final_parent->world_linear),
                            subtract(target.position, parent_translation));
  const Vec3 mapped_pivot =
      BedrockTransformResolver::convertBedrockVector(bone.pivot);
  const Vec3 mapped_animation_position = subtract(local_pivot, mapped_pivot);
  return {
      BedrockTransformResolver::convertBedrockVector(mapped_animation_position),
      {total_local_euler[0] - bone.rotation[0],
       total_local_euler[1] - bone.rotation[1],
       total_local_euler[2] - bone.rotation[2]}};
}

Pose targetPose(const loader::Bone &bone, const Pose &reference,
                const Pose *final_parent, const LocalChannels &channels,
                const WorldTarget &target, const Quat &world_rotation) {
  const Quat parent_rotation =
      final_parent == nullptr ? Quat{0, 0, 0, 1} : final_parent->world_rotation;
  const Quat local_rotation = RotationUtil::quaternionMultiply(
      RotationUtil::quaternionInverse(parent_rotation), world_rotation);
  Pose result = composePose(bone, channels, reference.animation_scale,
                            final_parent, local_rotation, world_rotation);
  result.world_position = target.position;
  return result;
}

}

FinalPoseReconstructor::Evaluator::Evaluator(
    const std::vector<loader::Bone> &bones) {
  std::map<std::string, loader::Bone> by_name;
  for (const auto &bone : bones) {
    if (bone.name.empty()) {
      continue;
    }



    by_name.emplace(bone.name, bone);
  }

  std::vector<loader::Bone> ordered;
  ordered.reserve(bones.size());
  std::map<std::string, int> states;
  for (const auto &bone : bones) {
    if (!bone.name.empty()) {
      appendTopologically(by_name.at(bone.name), by_name, states, ordered);
    }
  }

  slots_.reserve(ordered.size());
  std::map<std::string, std::size_t> slot_by_name;
  for (auto &bone : ordered) {
    std::size_t parent_slot = kNoParent;
    if (bone.has_parent) {
      const auto parent = slot_by_name.find(bone.parent);
      if (parent != slot_by_name.end()) {
        parent_slot = parent->second;
      }
    }
    const std::size_t slot = slots_.size();
    slot_by_name.emplace(bone.name, slot);
    slots_.push_back({std::move(bone), parent_slot});
  }
}

FinalPoseReconstructor::Evaluator::ReconstructionScratch::
    ReconstructionScratch(ReconstructionScratch &&other) noexcept
    : final_world_poses_(std::move(other.final_world_poses_)),
      reference_pose_slots_(std::move(other.reference_pose_slots_)),
      result_(std::move(other.result_)) {
  other.reset();
}

FinalPoseReconstructor::Evaluator::ReconstructionScratch &
FinalPoseReconstructor::Evaluator::ReconstructionScratch::operator=(
    ReconstructionScratch &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  final_world_poses_ = std::move(other.final_world_poses_);
  reference_pose_slots_ = std::move(other.reference_pose_slots_);
  result_ = std::move(other.result_);
  local_channel_slots_.clear();
  world_pose_slots_.clear();
  layout_identity_.reset();
  other.reset();
  return *this;
}

void FinalPoseReconstructor::Evaluator::ReconstructionScratch::reset() {
  final_world_poses_.clear();
  reference_pose_slots_.clear();
  result_ = {};
  local_channel_slots_.clear();
  world_pose_slots_.clear();
  layout_identity_.reset();
}

FinalPoseReconstructor::Evaluator
FinalPoseReconstructor::compile(const std::vector<loader::Bone> &bones) {
  return Evaluator(bones);
}

FinalPoseReconstructor::Result FinalPoseReconstructor::Evaluator::reconstruct(
    const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
    const std::map<std::string, WorldTarget> &physics_targets) const {
  ReconstructionScratch scratch;
  (void)reconstructInto(reference_poses, physics_targets, scratch);
  return std::move(scratch.result_);
}

const FinalPoseReconstructor::Result &
FinalPoseReconstructor::Evaluator::reconstructInto(
    const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
    const std::map<std::string, WorldTarget> &physics_targets,
    ReconstructionScratch &scratch) const {
  if (scratch.layout_identity_ != layout_identity_) {
    scratch.final_world_poses_.resize(slots_.size());
    scratch.reference_pose_slots_.resize(slots_.size(), nullptr);
    scratch.result_ = {};
    scratch.local_channel_slots_.clear();
    scratch.local_channel_slots_.reserve(slots_.size());
    scratch.world_pose_slots_.clear();
    scratch.world_pose_slots_.reserve(slots_.size());
    for (const Slot &slot : slots_) {
      auto [local, local_inserted] =
          scratch.result_.local_channels.emplace(slot.bone.name,
                                                 LocalChannels{});
      auto [world, world_inserted] =
          scratch.result_.world_poses.emplace(slot.bone.name, Pose{});
      if (!local_inserted || !world_inserted) {
        throw std::logic_error(
            "compiled reconstruction layout contains a duplicate bone name");
      }
      scratch.local_channel_slots_.push_back(&local->second);
      scratch.world_pose_slots_.push_back(&world->second);
    }
    scratch.layout_identity_ = layout_identity_;
  }

  for (std::size_t slot_index = 0; slot_index < slots_.size(); ++slot_index) {
    const Slot &slot = slots_[slot_index];
    const loader::Bone &bone = slot.bone;
    auto reference_it = reference_poses.find(bone.name);
    if (reference_it == reference_poses.end()) {
      throw std::invalid_argument("missing reference pose: " + bone.name);
    }
    const Pose &reference = reference_it->second;
    scratch.reference_pose_slots_[slot_index] = &reference;

    const Pose *reference_parent = nullptr;
    const Pose *final_parent = nullptr;
    if (slot.parent_slot != kNoParent) {
      reference_parent = scratch.reference_pose_slots_[slot.parent_slot];
      final_parent = &scratch.final_world_poses_[slot.parent_slot];
    } else if (bone.has_parent) {



      auto reference_parent_it = reference_poses.find(bone.parent);
      if (reference_parent_it != reference_poses.end()) {
        reference_parent = &reference_parent_it->second;
      }
    }

    auto target = physics_targets.find(bone.name);
    LocalChannels channels;
    Pose final_pose;
    if (target == physics_targets.end()) {
      channels = referenceChannels(bone, reference);
      final_pose = inheritReferenceLocal(bone, reference, reference_parent,
                                         final_parent, channels);
    } else {
      for (double component : target->second.position) {
        if (!std::isfinite(component)) {
          throw std::invalid_argument("world target position must be finite: " +
                                      bone.name);
        }
      }
      const Quat world_rotation = desiredRotation(reference, reference_parent,
                                                  final_parent, target->second);
      channels = solveLocalChannels(bone, reference, final_parent,
                                    target->second, world_rotation);
      final_pose = targetPose(bone, reference, final_parent, channels,
                              target->second, world_rotation);
    }

    scratch.final_world_poses_[slot_index] = final_pose;
    *scratch.local_channel_slots_[slot_index] = channels;
    *scratch.world_pose_slots_[slot_index] = std::move(final_pose);
  }
  return scratch.result_;
}

FinalPoseReconstructor::Result FinalPoseReconstructor::reconstruct(
    const std::vector<loader::Bone> &bones,
    const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
    const std::map<std::string, WorldTarget> &physics_targets) {
  return compile(bones).reconstruct(reference_poses, physics_targets);
}

}
