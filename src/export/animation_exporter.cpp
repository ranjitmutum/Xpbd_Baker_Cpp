#include "xpbd/export/animation_exporter.hpp"

#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/export/atomic_file_writer.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace xpbd::export_ {

void AnimationExporter::exportAnimation(
    const std::string &anim_id, const loader::Animation *source_animation,
    const std::vector<baker::BakedFrame> &frames, bool loop,
    const std::filesystem::path &file_path) {
  exportAnimation(anim_id, source_animation, frames,
                  loop ? loader::Animation::LoopBehavior::Loop
                       : loader::Animation::LoopBehavior::Once,
                  file_path, nullptr, false);
}

void AnimationExporter::exportAnimation(
    const std::string &anim_id, const loader::Animation *reference_animation,
    const std::vector<baker::BakedFrame> &frames,
    loader::Animation::LoopBehavior loop_behavior,
    const std::filesystem::path &file_path) {
  exportAnimation(anim_id, reference_animation, frames, loop_behavior,
                  file_path, nullptr, false);
}

void AnimationExporter::exportAnimation(
    const std::string &anim_id, const loader::Animation *reference_animation,
    const std::vector<baker::BakedFrame> &frames,
    loader::Animation::LoopBehavior loop_behavior,
    const std::filesystem::path &file_path,
    const TransitionReferenceExport *transition_reference,
    bool exact_baked_length) {
  if (anim_id.empty()) {
    throw std::invalid_argument("animation ID must not be blank");
  }

  nlohmann::json root = nlohmann::json::object();
  root["format_version"] = "1.8.0";
  root["animations"] = nlohmann::json::object();
  root["animations"][anim_id] = bakedAnimationToJson(
      reference_animation, frames, loop_behavior, transition_reference,
      exact_baked_length);
  AtomicFileWriter::writeUtf8(file_path, root.dump(2));
}

void AnimationExporter::exportAllAnimations(
    const loader::AnimationRoot &source_root,
    const std::string &baked_animation_id,
    const loader::Animation *reference_animation,
    const std::vector<baker::BakedFrame> &frames,
    loader::Animation::LoopBehavior loop_behavior,
    const std::filesystem::path &file_path,
    const TransitionReferenceExport *transition_reference,
    bool exact_baked_length) {
  if (baked_animation_id.empty()) {
    throw std::invalid_argument("baked animation ID must not be blank");
  }
  if (!source_root.animations.contains(baked_animation_id)) {
    throw std::invalid_argument("baked animation is missing from animation root");
  }

  // 全量导出必须保留导入文件中的动画名称和顺序，仅替换当前烘焙动画的内容。
  nlohmann::ordered_json root = nlohmann::ordered_json::object();
  root["format_version"] = source_root.format_version.empty()
                               ? "1.8.0"
                               : source_root.format_version;
  nlohmann::ordered_json animations = nlohmann::ordered_json::object();
  std::unordered_set<std::string> written_names;

  const auto write_animation = [&](const std::string &name) {
    const auto found = source_root.animations.find(name);
    if (found == source_root.animations.end() ||
        !written_names.insert(name).second) {
      return;
    }
    if (name == baked_animation_id) {
      animations[name] = bakedAnimationToJson(
          reference_animation, frames, loop_behavior, transition_reference,
          exact_baked_length);
    } else {
      animations[name] = sourceAnimationToJson(found->second);
    }
  };

  for (const auto &name : source_root.animation_order) {
    write_animation(name);
  }
  for (const auto &[name, animation] : source_root.animations) {
    (void)animation;
    write_animation(name);
  }

  root["animations"] = std::move(animations);
  AtomicFileWriter::writeUtf8(file_path, root.dump(2));
}

nlohmann::json AnimationExporter::bakedAnimationToJson(
    const loader::Animation *reference_animation,
    const std::vector<baker::BakedFrame> &frames,
    loader::Animation::LoopBehavior loop_behavior,
    const TransitionReferenceExport *transition_reference,
    bool exact_baked_length) {
  if (transition_reference != nullptr &&
      (!transition_reference->sample_pose ||
       transition_reference->model_bones == nullptr)) {
    throw std::invalid_argument(
        "transition reference export requires a pose sampler and model bones");
  }

  nlohmann::json anim = nlohmann::json::object();
  if (loop_behavior == loader::Animation::LoopBehavior::HoldLast) {
    anim["loop"] = "hold_on_last_frame";
  } else {
    anim["loop"] = loop_behavior == loader::Animation::LoopBehavior::Loop;
  }
  if (reference_animation != nullptr &&
      reference_animation->override_previous_animation.has_value()) {
    anim["override_previous_animation"] =
        *reference_animation->override_previous_animation;
  }

  double animLen = frames.empty() ? 0.0 : frames.back().time;
  if (!exact_baked_length && reference_animation != nullptr &&
      reference_animation->animation_length > animLen) {
    animLen = reference_animation->animation_length;
  }
  requireFinite(animLen, "animation length");
  anim["animation_length"] = animLen;
  const bool halfOpenPeriodic =
      loop_behavior == loader::Animation::LoopBehavior::Loop &&
      reference_animation != nullptr &&
      std::isfinite(reference_animation->animation_length) &&
      reference_animation->animation_length > 0.0;
  const double periodicLength =
      halfOpenPeriodic ? reference_animation->animation_length : 0.0;
  const auto serializeFrame = [&](const baker::BakedFrame &frame) {
    return !halfOpenPeriodic || frame.time < periodicLength;
  };
  const auto stableFrameLayout =
      baker::StableFrameLayout::tryCreate(frames);


  std::vector<std::string> allBoneNames;
  std::unordered_set<std::string> seen;
  if (reference_animation != nullptr) {
    for (const auto &[name, channel] : reference_animation->bones) {
      (void)channel;
      if (seen.insert(name).second) {
        allBoneNames.push_back(name);
      }
    }
  }
  std::map<std::string, std::array<double, 3>> modelBoneRotations;
  if (transition_reference != nullptr) {
    for (const auto &bone : *transition_reference->model_bones) {
      if (bone.name.empty()) {
        continue;
      }
      modelBoneRotations[bone.name] = {bone.rotation[0], bone.rotation[1],
                                       bone.rotation[2]};
      if (transition_reference->dependency_bones.contains(bone.name) &&
          seen.insert(bone.name).second) {
        allBoneNames.push_back(bone.name);
      }
    }
  }
  std::unordered_set<std::string> bakedBoneNames;
  if (stableFrameLayout.has_value()) {
    for (const auto &boneName : stableFrameLayout->bone_names) {
      bakedBoneNames.insert(boneName);
      if (seen.insert(boneName).second) {
        allBoneNames.push_back(boneName);
      }
    }
  } else {
    for (const auto &frame : frames) {
      for (const auto &state : frame.bone_states) {
        bakedBoneNames.insert(state.bone_name);
        if (seen.insert(state.bone_name).second) {
          allBoneNames.push_back(state.bone_name);
        }
      }
    }
  }

  std::set<std::string> flattenedDependencies;
  std::vector<TransitionReferenceExport::PoseMap> transitionSamples;
  if (transition_reference != nullptr) {
    for (const auto &boneName : transition_reference->dependency_bones) {
      if (bakedBoneNames.contains(boneName)) {
        continue;
      }
      if (!modelBoneRotations.contains(boneName)) {
        throw std::invalid_argument("transition reference dependency bone '" +
                                    boneName + "' is missing from the model");
      }
      const loader::BoneAnimation *channel = nullptr;
      if (reference_animation != nullptr) {
        const auto found = reference_animation->bones.find(boneName);
        if (found != reference_animation->bones.end()) {
          channel = &found->second;
        }
      }
      if (channel != nullptr && channel->has_position &&
          channel->position.containsMolang()) {
        throw std::invalid_argument(
            "transition reference dependency bone '" + boneName +
            "' position contains Molang and cannot be flattened for standalone "
            "export");
      }
      if (channel != nullptr && channel->has_rotation &&
          channel->rotation.containsMolang()) {
        throw std::invalid_argument(
            "transition reference dependency bone '" + boneName +
            "' rotation contains Molang and cannot be flattened for standalone "
            "export");
      }
      flattenedDependencies.insert(boneName);
    }
    transitionSamples.reserve(frames.size());
    for (const auto &frame : frames) {
      auto sample = transition_reference->sample_pose(frame.time);
      for (const auto &boneName : flattenedDependencies) {
        if (!sample.contains(boneName)) {
          throw std::invalid_argument(
              "transition reference pose is missing dependency bone '" +
              boneName + "'");
        }
      }
      transitionSamples.push_back(std::move(sample));
    }
  }





  nlohmann::json bones = nlohmann::json::object();
  for (const auto &boneName : allBoneNames) {
    nlohmann::json boneObj = nlohmann::json::object();
    std::optional<std::size_t> stableBakedIndex;
    if (stableFrameLayout.has_value()) {
      const auto index = stableFrameLayout->index_by_name.find(boneName);
      if (index != stableFrameLayout->index_by_name.end()) {
        stableBakedIndex = index->second;
      }
    }
    const bool hasBaked = stableFrameLayout.has_value()
                              ? stableBakedIndex.has_value()
                              : bakedBoneNames.contains(boneName);
    const loader::BoneAnimation *srcBone = nullptr;
    if (reference_animation != nullptr) {
      auto it = reference_animation->bones.find(boneName);
      if (it != reference_animation->bones.end()) {
        srcBone = &it->second;
      }
    }

    if (hasBaked) {
      nlohmann::json rotObj = nlohmann::json::object();
      nlohmann::json posObj = nlohmann::json::object();
      for (const auto &frame : frames) {
        if (!serializeFrame(frame)) {
          continue;
        }
        const baker::BoneState *bs =
            stableBakedIndex.has_value()
                ? &frame.bone_states[*stableBakedIndex]
                : frame.getBoneState(boneName);
        if (bs == nullptr && !stableFrameLayout.has_value()) {
          for (const auto &state : frame.bone_states) {
            if (state.bone_name == boneName) {
              bs = &state;
              break;
            }
          }
        }
        if (bs == nullptr) {
          continue;
        }
        rotObj[fmtTime(frame.time)] = toArray(bs->rotation);
        posObj[fmtTime(frame.time)] = toArray(bs->position);
      }
      boneObj["position"] = posObj;
      boneObj["rotation"] = rotObj;
    } else if (flattenedDependencies.contains(boneName)) {
      nlohmann::json rotObj = nlohmann::json::object();
      nlohmann::json posObj = nlohmann::json::object();
      const auto &baseRotation = modelBoneRotations.at(boneName);
      std::array<double, 3> previousRotation{};
      bool hasPreviousRotation = false;
      for (std::size_t frameIndex = 0; frameIndex < frames.size();
           ++frameIndex) {
        const auto &frame = frames[frameIndex];
        const auto &pose = transitionSamples[frameIndex].at(boneName);
        posObj[fmtTime(frame.time)] = toArray(pose.animation_position);
        std::array<double, 3> rotation{
            pose.total_local_euler[0] - baseRotation[0],
            pose.total_local_euler[1] - baseRotation[1],
            pose.total_local_euler[2] - baseRotation[2]};
        if (hasPreviousRotation) {
          rotation =
              baker::RotationUtil::unwrapEuler(previousRotation, rotation);
        }
        rotObj[fmtTime(frame.time)] = toArray(rotation);
        previousRotation = rotation;
        hasPreviousRotation = true;
      }
      boneObj["position"] = std::move(posObj);
      boneObj["rotation"] = std::move(rotObj);
    } else if (srcBone != nullptr) {
      if (srcBone->has_position) {
        boneObj["position"] = keyframesToJson(srcBone->position);
      }
      if (srcBone->has_rotation) {
        boneObj["rotation"] = keyframesToJson(srcBone->rotation);
      }
    }

    if (srcBone != nullptr && srcBone->has_scale) {
      boneObj["scale"] = keyframesToJson(srcBone->scale);
    }
    bones[boneName] = boneObj;
  }

  anim["bones"] = bones;
  return anim;
}

nlohmann::json AnimationExporter::sourceAnimationToJson(
    const loader::Animation &animation) {
  nlohmann::json result = nlohmann::json::object();
  if (animation.loop_behavior == loader::Animation::LoopBehavior::HoldLast) {
    result["loop"] = "hold_on_last_frame";
  } else {
    result["loop"] =
        animation.loop_behavior == loader::Animation::LoopBehavior::Loop;
  }
  result["animation_length"] = animation.animation_length;
  if (animation.override_previous_animation.has_value()) {
    result["override_previous_animation"] =
        *animation.override_previous_animation;
  }

  nlohmann::json bones = nlohmann::json::object();
  for (const auto &[bone_name, bone] : animation.bones) {
    nlohmann::json bone_json = nlohmann::json::object();
    if (bone.has_position) {
      bone_json["position"] = keyframesToJson(bone.position);
    }
    if (bone.has_rotation) {
      bone_json["rotation"] = keyframesToJson(bone.rotation);
    }
    if (bone.has_scale) {
      bone_json["scale"] = keyframesToJson(bone.scale);
    }
    bones[bone_name] = std::move(bone_json);
  }
  result["bones"] = std::move(bones);
  return result;
}

nlohmann::json AnimationExporter::keyframesToJson(const loader::Keyframes &kf) {
  if (kf.hasOriginalAuthoredJson()) {
    return kf.original_authored_json;
  }
  if (kf.hasOriginalMolang()) {
    return kf.original_molang_json;
  }
  nlohmann::json obj = nlohmann::json::object();
  for (const auto &[time, post] : kf.keyframes) {
    const auto mode = kf.interpolationMode(time);
    if (kf.hasDistinctPrePost(time) ||
        mode != loader::Keyframes::InterpolationMode::Linear) {
      nlohmann::json split = nlohmann::json::object();
      if (kf.hasDistinctPrePost(time)) {
        split["pre"] = toArray(kf.preValue(time));
      }
      split["post"] = toArray(post);
      if (mode != loader::Keyframes::InterpolationMode::Linear) {
        split["lerp_mode"] = "catmullrom";
      }
      obj[fmtTime(time)] = split;
    } else {
      obj[fmtTime(time)] = toArray(post);
    }
  }
  return obj;
}

nlohmann::json AnimationExporter::toArray(const double v[3]) {
  if (v == nullptr) {
    throw std::invalid_argument(
        "animation vectors must contain three components");
  }
  return nlohmann::json::array(
      {quantizeValue(v[0]), quantizeValue(v[1]), quantizeValue(v[2])});
}

nlohmann::json AnimationExporter::toArray(const std::array<double, 3> &v) {
  return toArray(v.data());
}

std::string AnimationExporter::fmtTime(double v) {
  requireFinite(v, "keyframe time");

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(12) << v;
  std::string s = oss.str();

  const auto dot = s.find('.');
  if (dot != std::string::npos) {
    while (!s.empty() && s.back() == '0') {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
      s.pop_back();
    }


    const auto newDot = s.find('.');
    if (newDot == std::string::npos) {
      s += ".0000";
    } else {
      const auto decimals = s.size() - newDot - 1;
      if (decimals < 4) {
        s.append(4 - decimals, '0');
      }
    }
  } else {
    s += ".0000";
  }
  return s;
}

double AnimationExporter::quantizeValue(double v) {
  requireFinite(v, "animation value");
  return std::round(v * 10000.0) / 10000.0;
}

void AnimationExporter::requireFinite(double value, const char *label) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(label) + " must be finite");
  }
}

}
