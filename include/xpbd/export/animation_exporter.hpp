#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/export/baked_channel_write_mask.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xpbd::export_ {

struct TransitionReferenceExport {
  using PoseMap = std::map<std::string, baker::BonePoseCalculator::Pose>;

  std::function<PoseMap(double)> sample_pose;
  std::set<std::string> dependency_bones;
  const std::vector<loader::Bone> *model_bones = nullptr;
};

// 将烘焙帧合并回 Bedrock 动画 JSON，同时保留未受影响的原始通道。
class AnimationExporter {
public:
  [[nodiscard]] static constexpr BakedChannelWriteMask bakedChannelWriteMask() {
    return kBakedChannelWriteMask;
  }
  [[nodiscard]] static double quantizeValue(double value);

  static void exportAnimation(const std::string &anim_id,
                              const loader::Animation *source_animation,
                              const std::vector<baker::BakedFrame> &frames,
                              bool loop,
                              const std::filesystem::path &file_path);

  static void exportAnimation(const std::string &anim_id,
                              const loader::Animation *reference_animation,
                              const std::vector<baker::BakedFrame> &frames,
                              loader::Animation::LoopBehavior loop_behavior,
                              const std::filesystem::path &file_path);

  static void
  exportAnimation(const std::string &anim_id,
                  const loader::Animation *reference_animation,
                  const std::vector<baker::BakedFrame> &frames,
                  loader::Animation::LoopBehavior loop_behavior,
                  const std::filesystem::path &file_path,
                  const TransitionReferenceExport *transition_reference,
                  bool exact_baked_length);

  // 导出全部已导入动画，并用当前烘焙结果替换同名动画。
  static void
  exportAllAnimations(const loader::AnimationRoot &source_root,
                      const std::string &baked_animation_id,
                      const loader::Animation *reference_animation,
                      const std::vector<baker::BakedFrame> &frames,
                      loader::Animation::LoopBehavior loop_behavior,
                      const std::filesystem::path &file_path,
                      const TransitionReferenceExport *transition_reference,
                      bool exact_baked_length);

private:
  static nlohmann::json
  bakedAnimationToJson(const loader::Animation *reference_animation,
                       const std::vector<baker::BakedFrame> &frames,
                       loader::Animation::LoopBehavior loop_behavior,
                       const TransitionReferenceExport *transition_reference,
                       bool exact_baked_length);
  static nlohmann::json
  sourceAnimationToJson(const loader::Animation &animation);
  static nlohmann::json keyframesToJson(const loader::Keyframes &kf);
  static nlohmann::json toArray(const double v[3]);
  static nlohmann::json toArray(const std::array<double, 3> &v);
  static std::string fmtTime(double v);
  static void requireFinite(double value, const char *label);
};

}
