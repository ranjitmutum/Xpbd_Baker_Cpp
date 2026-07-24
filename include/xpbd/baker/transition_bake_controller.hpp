#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/inertialized_target.hpp"
#include "xpbd/baker/transition_bake_request.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xpbd::baker {


class TransitionBakeController {
public:
  TransitionBakeController(const TransitionBakeRequest &request,
                           std::vector<loader::Bone> bones,
                           const BakedFrame *physical_frame = nullptr,
                           const BakedFrame *previous_physical_frame = nullptr,
                           double physical_frame_span = 0.0);

  [[nodiscard]] std::map<std::string, BonePoseCalculator::Pose>
  sample(double elapsed) const;
  [[nodiscard]] const BonePoseCalculator::Evaluator::PoseMap &sampleInto(
      double elapsed,
      BonePoseCalculator::Evaluator::EvaluationScratch &scratch) const;
  [[nodiscard]] double targetSampleTime(double elapsed) const;

private:
  struct OffsetSampleTime {
    double sample_time = 0.0;
    double physical_span = 0.0;
  };

  struct BoneOffset {
    InertializedTarget position;
    InertializedTarget rotation;
  };

  struct SampleInputs {
    double target_time = 0.0;
    std::map<std::string, std::array<double, 3>> position_overrides;
    std::map<std::string, std::array<double, 3>> rotation_overrides;
  };

  TransitionBakeRequest request_;
  std::vector<loader::Bone> bones_;
  BonePoseCalculator::Evaluator pose_evaluator_;
  BonePoseCalculator::Evaluator::AnimationBinding target_animation_binding_;
  std::map<std::string, loader::Bone> bones_by_name_;
  std::map<std::string, BoneState> physical_states_;
  std::map<std::string, BoneState> previous_physical_states_;
  double physical_frame_span_ = 0.0;
  std::map<std::string, BoneOffset> offsets_;

  void initializeOffsets();
  [[nodiscard]] SampleInputs sampleInputs(double elapsed) const;
  [[nodiscard]] std::array<double, 3>
  totalEuler(const std::string &bone_name,
             const std::array<double, 3> &anim) const;
  [[nodiscard]] std::array<double, 3>
  animationEuler(const std::string &bone_name,
                 const std::array<double, 3> &total) const;
  [[nodiscard]] static double
  canonicalSampleTime(const loader::Animation &animation, double time);
  [[nodiscard]] static double
  sourceTransitionSampleTime(const loader::Animation &animation, double time);
  [[nodiscard]] static OffsetSampleTime
  sampleTimeWithOffset(const loader::Animation &animation, double base_time,
                       double offset);
  static std::array<double, 3> channel(const loader::Animation &animation,
                                       const std::string &bone_name,
                                       double time, bool rotation);
  static std::array<double, 3>
  quaternionVelocity(const std::array<double, 4> &from,
                     const std::array<double, 4> &to, double span);
};

}
