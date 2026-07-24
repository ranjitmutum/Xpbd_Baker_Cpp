#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <map>
#include <string>
#include <vector>

namespace xpbd::baker {


enum class OutputEndpointPolicy { Closed, HalfOpenPeriodic };


class OutputTimelineResampler {
public:







  [[nodiscard]] static double
  inferSourceFrameInterval(const loader::Animation &animation,
                           double fallback_interval);






  [[nodiscard]] static std::vector<BakedFrame>
  resample(const std::vector<BakedFrame> &source,
           const std::map<std::string, loader::Bone> &bones_by_name,
           double target_interval, double clip_length,
           OutputEndpointPolicy endpoint_policy);


  [[nodiscard]] static RotationUtil::Quat
  interpolateQuaternionShortestArc(const RotationUtil::Quat &from,
                                   const RotationUtil::Quat &to,
                                   double fraction);

private:
  [[nodiscard]] static BakedFrame
  interpolateFrame(const std::vector<BakedFrame> &source,
                   const std::map<std::string, loader::Bone> &bones_by_name,
                   std::size_t suggested_lower_index, double time,
                   const StableFrameLayout *stable_layout,
                   const std::vector<std::array<double, 3>>
                       *bind_rotations_by_index);
};

}
