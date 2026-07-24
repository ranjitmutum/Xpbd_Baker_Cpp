#include "xpbd/loader/molang_keyframe_detector.hpp"

#include <set>

namespace xpbd::loader {

std::vector<MolangOverwrite> MolangKeyframeDetector::findOverwrittenChannels(
    const Animation *animation,
    const std::vector<std::string> &selected_bone_ids,
    export_::BakedChannelWriteMask write_mask) {
  std::vector<MolangOverwrite> result;
  if (animation == nullptr || selected_bone_ids.empty()) {
    return result;
  }
  for (const auto &boneId : selected_bone_ids) {
    if (boneId.empty()) {
      continue;
    }
    auto it = animation->bones.find(boneId);
    if (it == animation->bones.end()) {
      continue;
    }
    const BoneAnimation &bone = it->second;
    if (write_mask.position && bone.has_position &&
        bone.position.containsMolang()) {
      result.push_back({boneId, MolangChannel::Position});
    }
    if (write_mask.rotation && bone.has_rotation &&
        bone.rotation.containsMolang()) {
      result.push_back({boneId, MolangChannel::Rotation});
    }
    if (write_mask.scale && bone.has_scale && bone.scale.containsMolang()) {
      result.push_back({boneId, MolangChannel::Scale});
    }
  }
  return result;
}

std::vector<MolangBakeWarning> MolangKeyframeDetector::findBakeWarnings(
    const Animation *animation, MolangAnimationRole role,
    const std::string &animation_name,
    const std::set<std::string> &input_dependency_ids,
    const std::vector<std::string> &selected_bone_ids,
    export_::BakedChannelWriteMask write_mask) {
  std::vector<MolangBakeWarning> result;
  if (animation == nullptr || input_dependency_ids.empty()) {
    return result;
  }
  const std::set<std::string> selected(selected_bone_ids.begin(),
                                       selected_bone_ids.end());
  for (const auto &boneId : input_dependency_ids) {
    const auto animated = animation->bones.find(boneId);
    if (animated == animation->bones.end()) {
      continue;
    }
    const bool outputBone = selected.contains(boneId);
    const auto append = [&](MolangChannel channel, bool contains_molang,
                            bool channel_is_written) {
      if (!contains_molang) {
        return;
      }
      const MolangBakeAction action =
          outputBone && channel_is_written
              ? MolangBakeAction::OverwriteWithBakedKeys
              : MolangBakeAction::SampleAsZeroPreserve;
      result.push_back({role, action, animation_name, boneId, channel});
    };
    const BoneAnimation &channels = animated->second;
    append(MolangChannel::Position,
           channels.has_position && channels.position.containsMolang(),
           write_mask.position);
    append(MolangChannel::Rotation,
           channels.has_rotation && channels.rotation.containsMolang(),
           write_mask.rotation);
  }
  return result;
}

bool MolangKeyframeDetector::hasMolangKeyframes(
    const Animation *animation,
    const std::vector<std::string> &selected_bone_ids) {
  return !findOverwrittenChannels(animation, selected_bone_ids).empty();
}

const char *MolangKeyframeDetector::channelName(MolangChannel channel) {
  switch (channel) {
  case MolangChannel::Position:
    return "position";
  case MolangChannel::Rotation:
    return "rotation";
  case MolangChannel::Scale:
    return "scale";
  }
  return "unknown";
}

}
