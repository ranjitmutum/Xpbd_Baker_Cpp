#include "xpbd/baker/bone_pose_calculator.hpp"

#include "xpbd/baker/bedrock_transform_resolver.hpp"
#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace xpbd::baker {
namespace {

using Linear = std::array<double, 9>;
using Vec3 = std::array<double, 3>;

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

Linear rotationScaleLinear(const RotationUtil::Quat &rotation,
                           const Vec3 &scale) {
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

}

BonePoseCalculator::Evaluator
BonePoseCalculator::compile(const std::vector<loader::Bone> &bones) {
  return Evaluator(bones);
}

BonePoseCalculator::Evaluator::Evaluator(
    const std::vector<loader::Bone> &bones) {
  std::map<std::string, std::size_t> byName;
  for (std::size_t index = 0; index < bones.size(); ++index) {
    const auto &bone = bones[index];
    if (!bone.name.empty()) {



      byName[bone.name] = index;
    }
  }
  std::map<std::string, int> states;
  std::vector<std::size_t> orderedIndices;
  orderedIndices.reserve(bones.size());
  for (std::size_t index = 0; index < bones.size(); ++index) {
    if (!bones[index].name.empty()) {
      appendTopologically(index, bones, byName, states, orderedIndices);
    }
  }

  slots_.reserve(orderedIndices.size());
  std::map<std::string, std::size_t> slotByName;
  for (std::size_t boneIndex : orderedIndices) {
    const loader::Bone &bone = bones[boneIndex];
    Slot slot;
    slot.name = bone.name;
    for (std::size_t axis = 0; axis < slot.pivot.size(); ++axis) {
      slot.pivot[axis] = bone.pivot[axis];
      slot.rotation[axis] = bone.rotation[axis];
    }
    if (bone.has_parent) {
      const auto parent = slotByName.find(bone.parent);
      if (parent != slotByName.end()) {
        slot.parent_slot = parent->second;
      }
    }
    slotByName.emplace(slot.name, slots_.size());
    slots_.push_back(std::move(slot));
  }
}

BonePoseCalculator::Evaluator::EvaluationScratch::EvaluationScratch(
    EvaluationScratch &&other) noexcept
    : dense_poses_(std::move(other.dense_poses_)),
      result_(std::move(other.result_)) {
  other.reset();
}

BonePoseCalculator::Evaluator::EvaluationScratch &
BonePoseCalculator::Evaluator::EvaluationScratch::operator=(
    EvaluationScratch &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  dense_poses_ = std::move(other.dense_poses_);
  result_ = std::move(other.result_);
  result_slots_.clear();
  layout_identity_.reset();
  other.reset();
  return *this;
}

void BonePoseCalculator::Evaluator::EvaluationScratch::reset() {
  dense_poses_.clear();
  result_.clear();
  result_slots_.clear();
  layout_identity_.reset();
}

void BonePoseCalculator::Evaluator::EvaluationScratch::replaceResult(
    PoseMap result) {
  dense_poses_.clear();
  result_ = std::move(result);
  result_slots_.clear();
  layout_identity_.reset();
}

BonePoseCalculator::Evaluator::AnimationBinding
BonePoseCalculator::Evaluator::bind(
    const loader::Animation *animation) const {
  AnimationBinding binding;
  binding.animation_ = animation;
  binding.layout_identity_ = layout_identity_;
  binding.channels_by_slot_.resize(slots_.size(), nullptr);
  if (animation == nullptr) {
    return binding;
  }
  for (std::size_t slotIndex = 0; slotIndex < slots_.size(); ++slotIndex) {
    const auto channel = animation->bones.find(slots_[slotIndex].name);
    if (channel != animation->bones.end()) {
      binding.channels_by_slot_[slotIndex] = &channel->second;
    }
  }
  return binding;
}

void BonePoseCalculator::Evaluator::appendTopologically(
    std::size_t bone_index, const std::vector<loader::Bone> &bones,
    const std::map<std::string, std::size_t> &by_name,
    std::map<std::string, int> &states, std::vector<std::size_t> &ordered) {
  const loader::Bone &bone = bones[bone_index];
  auto stateIt = states.find(bone.name);
  if (stateIt != states.end()) {
    if (stateIt->second == 1) {
      throw std::invalid_argument("Bone hierarchy contains a cycle at " +
                                  bone.name);
    }
    return;
  }
  states[bone.name] = 1;
  if (bone.has_parent) {
    auto parentIt = by_name.find(bone.parent);
    if (parentIt != by_name.end()) {
      appendTopologically(parentIt->second, bones, by_name, states, ordered);
    }
  }
  states[bone.name] = 2;
  ordered.push_back(bone_index);
}

std::map<std::string, BonePoseCalculator::Pose>
BonePoseCalculator::Evaluator::calculate(const loader::Animation *animation,
                                         double time) const {
  return calculate(animation, time, nullptr, nullptr);
}

std::map<std::string, BonePoseCalculator::Pose>
BonePoseCalculator::Evaluator::calculate(
    const loader::Animation *animation, double time,
    const std::map<std::string, std::array<double, 3>> *position_overrides,
    const std::map<std::string, std::array<double, 3>> *rotation_overrides)
    const {
  return calculate(bind(animation), time, position_overrides,
                   rotation_overrides);
}

BonePoseCalculator::Evaluator::PoseMap
BonePoseCalculator::Evaluator::calculate(
    const AnimationBinding &binding, double time,
    const std::map<std::string, std::array<double, 3>> *position_overrides,
    const std::map<std::string, std::array<double, 3>> *rotation_overrides)
    const {
  EvaluationScratch scratch;
  (void)calculateInto(binding, time, scratch, position_overrides,
                      rotation_overrides);
  return std::move(scratch.result_);
}

const BonePoseCalculator::Evaluator::PoseMap &
BonePoseCalculator::Evaluator::calculateInto(
    const AnimationBinding &binding, double time, EvaluationScratch &scratch,
    const std::map<std::string, std::array<double, 3>> *position_overrides,
    const std::map<std::string, std::array<double, 3>> *rotation_overrides)
    const {
  if (binding.layout_identity_ != layout_identity_ ||
      binding.channels_by_slot_.size() != slots_.size()) {
    throw std::invalid_argument(
        "animation binding does not match the pose evaluator layout");
  }

  if (scratch.layout_identity_ != layout_identity_) {
    scratch.dense_poses_.resize(slots_.size());
    scratch.result_.clear();
    scratch.result_slots_.clear();
    scratch.result_slots_.reserve(slots_.size());
    for (const Slot &slot : slots_) {
      auto [result, inserted] = scratch.result_.emplace(slot.name, Pose{});
      if (!inserted) {
        throw std::logic_error(
            "compiled pose layout contains a duplicate bone name");
      }
      scratch.result_slots_.push_back(&result->second);
    }
    scratch.layout_identity_ = layout_identity_;
  }

  for (std::size_t slotIndex = 0; slotIndex < slots_.size(); ++slotIndex) {
    const Slot &slot = slots_[slotIndex];
    const Pose *parentPose =
        slot.parent_slot == kNoParent
            ? nullptr
            : &scratch.dense_poses_[slot.parent_slot];
    scratch.dense_poses_[slotIndex] =
        compose(slot.name, slot.pivot.data(), slot.rotation.data(), parentPose,
                binding.channels_by_slot_[slotIndex], time, position_overrides,
                rotation_overrides);
    *scratch.result_slots_[slotIndex] = scratch.dense_poses_[slotIndex];
  }
  return scratch.result_;
}

std::map<std::string, BonePoseCalculator::Pose>
BonePoseCalculator::calculate(const std::vector<loader::Bone> &bones,
                              const loader::Animation *animation, double time) {
  return calculate(bones, animation, time, nullptr, nullptr);
}

std::map<std::string, BonePoseCalculator::Pose> BonePoseCalculator::calculate(
    const std::vector<loader::Bone> &bones, const loader::Animation *animation,
    double time,
    const std::map<std::string, std::array<double, 3>> *position_overrides,
    const std::map<std::string, std::array<double, 3>> *rotation_overrides) {
  std::map<std::string, loader::Bone> byName;
  for (const auto &bone : bones) {
    if (!bone.name.empty()) {
      byName[bone.name] = bone;
    }
  }
  std::map<std::string, Pose> result;
  std::vector<std::string> visiting;
  for (const auto &bone : bones) {
    if (!bone.name.empty()) {
      resolve(bone, byName, animation, time, position_overrides,
              rotation_overrides, result, visiting);
    }
  }
  return result;
}

BonePoseCalculator::Pose BonePoseCalculator::resolve(
    const loader::Bone &bone,
    const std::map<std::string, loader::Bone> &by_name,
    const loader::Animation *animation, double time,
    const std::map<std::string, std::array<double, 3>> *position_overrides,
    const std::map<std::string, std::array<double, 3>> *rotation_overrides,
    std::map<std::string, Pose> &result, std::vector<std::string> &visiting) {
  auto cached = result.find(bone.name);
  if (cached != result.end()) {
    return cached->second;
  }
  if (std::find(visiting.begin(), visiting.end(), bone.name) !=
      visiting.end()) {
    throw std::invalid_argument("Bone hierarchy contains a cycle at " +
                                bone.name);
  }
  visiting.push_back(bone.name);

  const Pose *parentPose = nullptr;
  Pose parentStorage;
  if (bone.has_parent) {
    auto parentIt = by_name.find(bone.parent);
    if (parentIt != by_name.end()) {
      parentStorage =
          resolve(parentIt->second, by_name, animation, time,
                  position_overrides, rotation_overrides, result, visiting);
      parentPose = &parentStorage;
    }
  }

  const loader::BoneAnimation *channel = nullptr;
  if (animation != nullptr) {
    const auto channelIt = animation->bones.find(bone.name);
    if (channelIt != animation->bones.end()) {
      channel = &channelIt->second;
    }
  }
  Pose pose = compose(bone.name, bone.pivot, bone.rotation, parentPose, channel,
                      time, position_overrides, rotation_overrides);
  result.emplace(bone.name, pose);
  visiting.pop_back();
  return pose;
}

BonePoseCalculator::Pose BonePoseCalculator::compose(
    const std::string &bone_name, const double bone_pivot[3],
    const double bone_rotation[3], const Pose *parent_pose,
    const loader::BoneAnimation *channel, double time,
    const std::map<std::string, std::array<double, 3>> *position_overrides,
    const std::map<std::string, std::array<double, 3>> *rotation_overrides) {
  std::array<double, 3> animPosition{0.0, 0.0, 0.0};
  std::array<double, 3> animRotation{0.0, 0.0, 0.0};
  std::array<double, 3> animScale{1.0, 1.0, 1.0};
  if (channel != nullptr) {
    if (channel->has_position) {
      animPosition = channel->position.evaluate(time);
    }
    if (channel->has_rotation) {
      animRotation = channel->rotation.evaluate(time);
    }
    if (channel->has_scale) {
      animScale = channel->scale.evaluate(time);
    }
  }
  if (position_overrides != nullptr) {
    auto it = position_overrides->find(bone_name);
    if (it != position_overrides->end()) {
      animPosition = it->second;
    }
  }
  if (rotation_overrides != nullptr) {
    auto it = rotation_overrides->find(bone_name);
    if (it != rotation_overrides->end()) {
      animRotation = it->second;
    }
  }

  const std::array<double, 3> totalEuler{bone_rotation[0] + animRotation[0],
                                         bone_rotation[1] + animRotation[1],
                                         bone_rotation[2] + animRotation[2]};
  const auto localRotation = RotationUtil::quaternionFromBedrockEuler(
      totalEuler[0], totalEuler[1], totalEuler[2]);
  const auto localLinear = rotationScaleLinear(localRotation, animScale);
  const auto mappedPivot =
      BedrockTransformResolver::convertBedrockVector(bone_pivot);
  const auto mappedAnimPosition =
      BedrockTransformResolver::convertBedrockVector(animPosition);


  const auto transformedNegativePivot = transformVector(
      localLinear,
      Vec3{-mappedPivot[0], -mappedPivot[1], -mappedPivot[2]});
  const std::array<double, 3> localTranslation{
      mappedAnimPosition[0] + mappedPivot[0] + transformedNegativePivot[0],
      mappedAnimPosition[1] + mappedPivot[1] + transformedNegativePivot[1],
      mappedAnimPosition[2] + mappedPivot[2] + transformedNegativePivot[2]};

  std::array<double, 4> worldRotation = localRotation;
  Linear worldLinear = localLinear;
  std::array<double, 3> worldTranslation = localTranslation;
  if (parent_pose != nullptr) {
    worldRotation = RotationUtil::quaternionMultiply(
        parent_pose->world_rotation, localRotation);
    worldLinear = multiplyLinear(parent_pose->world_linear, localLinear);
    const auto translated =
        transformVector(parent_pose->world_linear, localTranslation);
    worldTranslation = {translated[0] + parent_pose->world_translation[0],
                        translated[1] + parent_pose->world_translation[1],
                        translated[2] + parent_pose->world_translation[2]};
  }

  const auto transformedPivot = transformVector(worldLinear, mappedPivot);
  const std::array<double, 3> worldPosition{
      transformedPivot[0] + worldTranslation[0],
      transformedPivot[1] + worldTranslation[1],
      transformedPivot[2] + worldTranslation[2]};

  Pose pose;
  pose.world_position = worldPosition;
  pose.world_rotation = worldRotation;
  pose.world_linear = worldLinear;
  pose.world_translation = worldTranslation;
  pose.animation_position = animPosition;
  pose.animation_scale = animScale;
  pose.total_local_euler = totalEuler;
  return pose;
}

}
