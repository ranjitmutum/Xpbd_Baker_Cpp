#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace xpbd::baker {


enum class OutputEndpointPolicy { Closed, HalfOpenPeriodic };


class OutputTimelineResampler {
public:







  // 从源动画关键帧时间和动画长度推断 Blockbench 吸附 FPS。
  // 返回 0 表示没有足够的时间信息，或无法匹配 10-500 FPS 整数网格。
  [[nodiscard]] static int
  detectSourceSnappingFps(const loader::Animation &animation);

  // 返回严格吸附时间线最终应包含的关键帧数。Closed 包含末帧，
  // HalfOpenPeriodic 不包含与第 0 帧重复的循环末帧；0 表示长度不在网格上。
  [[nodiscard]] static std::size_t
  snappedFrameCount(double clip_length, int snapping_fps,
                    OutputEndpointPolicy endpoint_policy);

  [[nodiscard]] static double
  inferSourceFrameInterval(const loader::Animation &animation,
                           double fallback_interval);

  // 严格按整数吸附 FPS 重采样，并校验输出关键帧数。
  [[nodiscard]] static std::vector<BakedFrame>
  resampleToSnappingFps(
      const std::vector<BakedFrame> &source,
      const std::map<std::string, loader::Bone> &bones_by_name,
      int snapping_fps, double clip_length,
      OutputEndpointPolicy endpoint_policy);






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
