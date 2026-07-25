#include "xpbd/baker/rigid_body_input_compat.hpp"

#include "xpbd/baker/cube_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <utility>

namespace xpbd::baker {
namespace {

constexpr double kMinimumPhysicsScale = 1e-12;
constexpr double kChannelChangeTolerance = 1e-9;
constexpr std::array<double, 3> kIdentityScale{1.0, 1.0, 1.0};

struct BoneHierarchy {
  std::map<std::string, const loader::Bone *> bones_by_name;
  std::map<std::string, std::vector<std::string>> children_by_name;
};

BoneHierarchy buildHierarchy(const BoneMapper &mapper) {
  BoneHierarchy hierarchy;
  for (const auto &bone : mapper.allBones()) {
    if (bone.name.empty()) {
      continue;
    }
    hierarchy.bones_by_name.emplace(bone.name, &bone);
    if (bone.has_parent && !bone.parent.empty()) {
      hierarchy.children_by_name[bone.parent].push_back(bone.name);
    }
  }
  return hierarchy;
}

template <typename T>
void appendUnique(std::vector<T> &destination, const std::vector<T> &values) {
  std::set<T> known(destination.begin(), destination.end());
  for (const auto &value : values) {
    if (known.insert(value).second) {
      destination.push_back(value);
    }
  }
}

bool usableScale(const std::array<double, 3> &value) {
  for (const double component : value) {
    if (!std::isfinite(component) ||
        !(std::abs(component) > kMinimumPhysicsScale)) {
      return false;
    }
  }
  return true;
}

bool vectorsNear(const std::array<double, 3> &left,
                 const std::array<double, 3> &right) {
  for (std::size_t axis = 0; axis < left.size(); ++axis) {
    if (std::abs(left[axis] - right[axis]) > kChannelChangeTolerance) {
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
      } else if (!vectorsNear(baseline, value)) {
        return true;
      }
    }
    return false;
  };
  return inspect(channel.keyframes) || inspect(channel.pre_keyframes);
}

struct ScaleInspection {
  bool authored = false;
  bool contains_molang = false;
  bool contains_degenerate_value = false;
  std::optional<std::array<double, 3>> first_usable_value;
};

ScaleInspection inspectScale(const loader::BoneAnimation *animation) {
  ScaleInspection result;
  if (animation == nullptr) {
    return result;
  }

  const auto &scale = animation->scale;
  result.authored = animation->has_scale || !scale.keyframes.empty() ||
                    !scale.pre_keyframes.empty();
  result.contains_molang = scale.containsMolang();
  if (!result.authored || result.contains_molang) {
    return result;
  }

  const auto inspect_values = [&result](const auto &values) {
    for (const auto &[time, value] : values) {
      (void)time;
      if (usableScale(value)) {
        if (!result.first_usable_value.has_value()) {
          result.first_usable_value = value;
        }
      } else {
        result.contains_degenerate_value = true;
      }
    }
  };
  inspect_values(scale.keyframes);
  inspect_values(scale.pre_keyframes);
  return result;
}

void freezeScale(loader::BoneAnimation &animation,
                 const std::array<double, 3> &baseline) {
  for (auto &[time, value] : animation.scale.keyframes) {
    (void)time;
    value = baseline;
  }
  for (auto &[time, value] : animation.scale.pre_keyframes) {
    (void)time;
    value = baseline;
  }
}

loader::BoneAnimation *findBoneAnimation(loader::Animation &animation,
                                         const std::string &bone_name) {
  const auto found = animation.bones.find(bone_name);
  return found == animation.bones.end() ? nullptr : &found->second;
}

const loader::BoneAnimation *
findBoneAnimation(const loader::Animation *animation,
                  const std::string &bone_name) {
  if (animation == nullptr) {
    return nullptr;
  }
  const auto found = animation->bones.find(bone_name);
  return found == animation->bones.end() ? nullptr : &found->second;
}

bool hasPotentialVolume(const loader::Bone &bone) {
  for (const auto &cube : bone.cubes) {
    const auto size = CubeGeometry::effectiveSize(cube);
    if (std::isfinite(size[0]) && std::isfinite(size[1]) &&
        std::isfinite(size[2]) && size[0] > 0.0 && size[1] > 0.0 &&
        size[2] > 0.0) {
      return true;
    }
  }
  return false;
}

std::set<std::string>
ownedSubtreeBones(const BoneMapper &mapper, const BoneHierarchy &hierarchy,
                  const std::string &owner) {
  std::set<std::string> owned;
  std::queue<std::string> pending;
  pending.push(owner);

  while (!pending.empty()) {
    const std::string current = pending.front();
    pending.pop();
    if (owned.contains(current)) {
      continue;
    }
    if (current != owner && mapper.isPhysicsBone(current)) {
      continue;
    }
    if (!hierarchy.bones_by_name.contains(current)) {
      continue;
    }
    owned.insert(current);
    const auto children = hierarchy.children_by_name.find(current);
    if (children != hierarchy.children_by_name.end()) {
      for (const auto &child : children->second) {
        pending.push(child);
      }
    }
  }
  return owned;
}

std::set<std::string>
compoundDescendantDependencies(const BoneMapper &mapper,
                               const BoneHierarchy &hierarchy) {
  std::set<std::string> dependencies;
  for (const auto &owner : mapper.physicsBones()) {
    const auto owned = ownedSubtreeBones(mapper, hierarchy, owner);
    for (const auto &source_name : owned) {
      if (source_name == owner) {
        continue;
      }
      const auto source = hierarchy.bones_by_name.find(source_name);
      if (source == hierarchy.bones_by_name.end() ||
          source->second->cubes.empty()) {
        continue;
      }

      std::set<std::string> path;
      std::string current = source_name;
      while (current != owner && path.insert(current).second) {
        if (!owned.contains(current)) {
          break;
        }
        dependencies.insert(current);
        const auto bone = hierarchy.bones_by_name.find(current);
        if (bone == hierarchy.bones_by_name.end() ||
            !bone->second->has_parent || bone->second->parent.empty()) {
          break;
        }
        current = bone->second->parent;
      }
    }
  }
  return dependencies;
}

bool hasChangingNumericTransform(const loader::Animation *animation,
                                 const std::string &bone_name) {
  const auto *bone = findBoneAnimation(animation, bone_name);
  if (bone == nullptr) {
    return false;
  }

  const bool changing_position =
      bone->has_position && !bone->position.containsMolang() &&
      channelChanges(bone->position);
  const bool changing_rotation =
      bone->has_rotation && !bone->rotation.containsMolang() &&
      channelChanges(bone->rotation);
  return changing_position || changing_rotation;
}

std::vector<std::string> findAnimatedCompoundDescendants(
    const BoneMapper &mapper, const BoneHierarchy &hierarchy,
    const loader::Animation &source_animation,
    const loader::Animation *target_animation) {
  std::vector<std::string> result;
  for (const auto &bone_name :
       compoundDescendantDependencies(mapper, hierarchy)) {
    if (mapper.isPhysicsBone(bone_name)) {
      continue;
    }
    if (hasChangingNumericTransform(&source_animation, bone_name) ||
        hasChangingNumericTransform(target_animation, bone_name)) {
      result.push_back(bone_name);
    }
  }
  return result;
}

std::vector<std::string>
removeBlockedEmptyDynamicBones(BoneMapper &mapper,
                               const BoneHierarchy &hierarchy) {
  std::vector<std::string> removed;
  if (mapper.config().simulation_mode != BoneMapper::SimulationMode::RigidBody) {
    return removed;
  }

  for (const auto &owner : mapper.physicsBones()) {
    if (mapper.isFixedBone(owner)) {
      continue;
    }

    bool has_owned_geometry = false;
    bool blocked_by_selected_descendant = false;
    std::queue<std::string> pending;
    std::set<std::string> visited;
    pending.push(owner);

    // Search the owner and every descendant branch that is not already owned
    // by another selected physics bone.
    while (!pending.empty() && !has_owned_geometry) {
      const std::string current = pending.front();
      pending.pop();
      if (!visited.insert(current).second) {
        continue;
      }

      if (current != owner && mapper.isPhysicsBone(current)) {
        blocked_by_selected_descendant = true;
        continue;
      }

      const auto bone = hierarchy.bones_by_name.find(current);
      if (bone == hierarchy.bones_by_name.end()) {
        continue;
      }
      if (hasPotentialVolume(*bone->second)) {
        has_owned_geometry = true;
        break;
      }

      const auto children = hierarchy.children_by_name.find(current);
      if (children != hierarchy.children_by_name.end()) {
        for (const auto &child : children->second) {
          pending.push(child);
        }
      }
    }

    if (!has_owned_geometry && blocked_by_selected_descendant) {
      removed.push_back(owner);
    }
  }

  for (const auto &bone_name : removed) {
    mapper.removePhysicsBone(bone_name);
  }
  return removed;
}

std::set<std::string>
scaleDependencyBones(const BoneMapper &mapper,
                     const BoneHierarchy &hierarchy) {
  std::set<std::string> dependencies =
      mapper.animationInputDependencyBones();
  for (const auto &owner : mapper.physicsBones()) {
    const auto owned = ownedSubtreeBones(mapper, hierarchy, owner);
    dependencies.insert(owned.begin(), owned.end());
  }
  return dependencies;
}

void repairDegenerateScaleChannels(
    const BoneMapper &mapper, const BoneHierarchy &hierarchy,
    loader::Animation &source_animation, loader::Animation *target_animation,
    RigidBodyInputCompatibilityReport &report) {
  if (mapper.config().simulation_mode != BoneMapper::SimulationMode::RigidBody) {
    return;
  }

  for (const auto &bone_name : scaleDependencyBones(mapper, hierarchy)) {
    auto *source_bone = findBoneAnimation(source_animation, bone_name);
    auto *target_bone = target_animation == nullptr
                            ? nullptr
                            : findBoneAnimation(*target_animation, bone_name);
    const ScaleInspection source = inspectScale(source_bone);
    const ScaleInspection target =
        inspectScale(findBoneAnimation(target_animation, bone_name));

    if (source_bone != nullptr && !source.contains_molang &&
        source.contains_degenerate_value) {
      const auto baseline = source.first_usable_value.has_value()
                                ? *source.first_usable_value
                                : target.first_usable_value.value_or(
                                      kIdentityScale);
      freezeScale(*source_bone, baseline);
      report.repaired_source_scale_bones.push_back(bone_name);
    }

    if (target_bone != nullptr && !target.contains_molang &&
        target.contains_degenerate_value) {
      const auto baseline = target.first_usable_value.has_value()
                                ? *target.first_usable_value
                                : source.first_usable_value.value_or(
                                      kIdentityScale);
      freezeScale(*target_bone, baseline);
      report.repaired_target_scale_bones.push_back(bone_name);
    }
  }
}

} // namespace

RigidBodyInputCompatibilityReport
prepareRigidBodyInputCompatibility(BoneMapper &mapper,
                                   loader::Animation &source_animation,
                                   loader::Animation *target_animation) {
  RigidBodyInputCompatibilityReport report;
  if (mapper.config().simulation_mode != BoneMapper::SimulationMode::RigidBody) {
    return report;
  }

  const BoneHierarchy hierarchy = buildHierarchy(mapper);
  appendUnique(report.skipped_blocked_dynamic_bones,
               removeBlockedEmptyDynamicBones(mapper, hierarchy));

  // Promoting one animated descendant can expose a deeper compound branch, so
  // iterate until the ownership layout no longer changes.
  const std::size_t maximum_passes = mapper.allBones().size() + 1;
  for (std::size_t pass = 0; pass < maximum_passes; ++pass) {
    const auto promoted = findAnimatedCompoundDescendants(
        mapper, hierarchy, source_animation, target_animation);
    bool changed = false;
    for (const auto &bone_name : promoted) {
      if (!mapper.isPhysicsBone(bone_name)) {
        mapper.addPhysicsBone(bone_name);
        report.promoted_animated_compound_bones.push_back(bone_name);
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
    appendUnique(report.skipped_blocked_dynamic_bones,
                 removeBlockedEmptyDynamicBones(mapper, hierarchy));
  }

  repairDegenerateScaleChannels(mapper, hierarchy, source_animation,
                                target_animation, report);
  return report;
}

} // namespace xpbd::baker
