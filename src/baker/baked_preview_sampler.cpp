#include "xpbd/baker/baked_preview_sampler.hpp"

#include "xpbd/baker/output_timeline_resampler.hpp"
#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace xpbd::baker {

namespace {

constexpr double kTimeEpsilon = 1e-12;

std::array<double, 3> lerp(const std::array<double, 3> &from,
                           const std::array<double, 3> &to,
                           double fraction) {
  std::array<double, 3> result{};
  for (std::size_t axis = 0; axis < result.size(); ++axis) {
    result[axis] = from[axis] + (to[axis] - from[axis]) * fraction;
  }
  return result;
}

bool layoutMatches(const BakedFrame &source,
                   const BakedPreviewScratch &scratch) {
  if (source.bone_states.size() != scratch.frame.bone_states.size() ||
      source.bone_states.size() != scratch.bind_rotations.size()) {
    return false;
  }
  for (std::size_t index = 0; index < source.bone_states.size(); ++index) {
    if (source.bone_states[index].bone_name !=
        scratch.frame.bone_states[index].bone_name) {
      return false;
    }
  }
  return true;
}

std::array<double, 3>
findBindRotation(const std::vector<loader::Bone> &model_bones,
                 const std::string &bone_name) {
  const auto bone =
      std::find_if(model_bones.begin(), model_bones.end(),
                   [&](const loader::Bone &candidate) {
                     return candidate.name == bone_name;
                   });
  if (bone == model_bones.end()) {
    return {};
  }
  return {bone->rotation[0], bone->rotation[1], bone->rotation[2]};
}

void prepareLayout(const BakedFrame &source,
                   const std::vector<loader::Bone> &model_bones,
                   BakedPreviewScratch &scratch) {
  if (layoutMatches(source, scratch)) {
    return;
  }

  scratch.frame.bone_states.clear();
  scratch.frame.bone_states.reserve(source.bone_states.size());
  scratch.bind_rotations.clear();
  scratch.bind_rotations.reserve(source.bone_states.size());
  for (const BoneState &state : source.bone_states) {
    BoneState output;
    output.bone_name = state.bone_name;
    scratch.frame.bone_states.push_back(std::move(output));
    scratch.bind_rotations.push_back(
        findBindRotation(model_bones, state.bone_name));
  }
  scratch.frame.rebuildIndex();
}

bool layoutsMatch(const BakedFrame &first, const BakedFrame &second) {
  if (first.bone_states.size() != second.bone_states.size()) {
    return false;
  }
  for (std::size_t index = 0; index < first.bone_states.size(); ++index) {
    if (first.bone_states[index].bone_name !=
        second.bone_states[index].bone_name) {
      return false;
    }
  }
  return true;
}

const BoneState *findState(const BakedFrame &frame,
                           const std::string &bone_name) {
  const auto state =
      std::find_if(frame.bone_states.begin(), frame.bone_states.end(),
                   [&](const BoneState &candidate) {
                     return candidate.bone_name == bone_name;
                   });
  return state == frame.bone_states.end() ? nullptr : &*state;
}

void copyStateValues(const BoneState &source, BoneState &target) {
  target.position = source.position;
  target.rotation = source.rotation;
  target.linear_velocity = source.linear_velocity;
  target.world_position = source.world_position;
  target.has_world_position = source.has_world_position;
}

void interpolateState(const BoneState &from, const BoneState &to,
                      const std::array<double, 3> &bind_rotation,
                      double fraction, BoneState &target) {
  target.position = lerp(from.position, to.position, fraction);
  target.linear_velocity =
      lerp(from.linear_velocity, to.linear_velocity, fraction);





  target.world_position = {};
  target.has_world_position = false;

  const RotationUtil::Vec3 from_total{
      bind_rotation[0] + from.rotation[0],
      bind_rotation[1] + from.rotation[1],
      bind_rotation[2] + from.rotation[2]};
  const RotationUtil::Vec3 to_total{
      bind_rotation[0] + to.rotation[0],
      bind_rotation[1] + to.rotation[1],
      bind_rotation[2] + to.rotation[2]};
  const RotationUtil::Quat from_q = RotationUtil::quaternionFromBedrockEuler(
      from_total[0], from_total[1], from_total[2]);
  const RotationUtil::Quat to_q = RotationUtil::quaternionFromBedrockEuler(
      to_total[0], to_total[1], to_total[2]);
  const RotationUtil::Quat sampled_q =
      OutputTimelineResampler::interpolateQuaternionShortestArc(
          from_q, to_q, fraction);
  const RotationUtil::Vec3 sampled_total = RotationUtil::unwrapEuler(
      from_total, RotationUtil::bedrockEulerFromQuaternion(sampled_q));
  for (std::size_t axis = 0; axis < target.rotation.size(); ++axis) {
    target.rotation[axis] = sampled_total[axis] - bind_rotation[axis];
  }
}

}

void BakedPreviewScratch::reset() {
  frame = {};
  bind_rotations.clear();
}

const BakedFrame &
BakedPreviewSampler::sample(const std::vector<BakedFrame> &frames,
                            const std::vector<loader::Bone> &model_bones,
                            double time, BakedPreviewScratch &scratch,
                            double loop_duration) {
  if (frames.empty()) {
    throw std::invalid_argument(
        "baked preview sampling requires at least one frame");
  }

  const bool periodic =
      std::isfinite(loop_duration) && loop_duration > 0.0;
  if (periodic &&
      (!std::isfinite(frames.front().time) ||
       std::abs(frames.front().time) > kTimeEpsilon ||
       !std::isfinite(frames.back().time) ||
       frames.back().time >= loop_duration)) {
    throw std::invalid_argument(
        "periodic baked preview requires a half-open [0, duration) timeline");
  }

  double sample_time = frames.front().time;
  if (std::isfinite(time)) {
    if (periodic) {
      sample_time = std::fmod(time, loop_duration);
      if (sample_time < 0.0) {
        sample_time += loop_duration;
      }
    } else {
      sample_time =
          std::clamp(time, frames.front().time, frames.back().time);
    }
  }

  const BakedFrame *lower_frame = nullptr;
  const BakedFrame *upper_frame = nullptr;
  double upper_time = sample_time;
  if (periodic && sample_time > frames.back().time) {
    lower_frame = &frames.back();
    upper_frame = &frames.front();
    upper_time = loop_duration;
  } else {
    const auto upper =
        std::upper_bound(frames.begin(), frames.end(), sample_time,
                         [](double value, const BakedFrame &frame) {
                           return value < frame.time;
                         });
    if (upper == frames.begin()) {
      lower_frame = &frames.front();
      upper_frame = lower_frame;
    } else if (upper == frames.end()) {
      lower_frame = &frames.back();
      upper_frame = lower_frame;
    } else {
      upper_frame = &*upper;
      lower_frame = &*std::prev(upper);
      upper_time = upper_frame->time;
    }
  }

  prepareLayout(*lower_frame, model_bones, scratch);
  scratch.frame.time = sample_time;
  const double span = upper_time - lower_frame->time;
  const double fraction =
      span > kTimeEpsilon
          ? std::clamp((sample_time - lower_frame->time) / span, 0.0, 1.0)
          : 0.0;
  const bool stable_layout = layoutsMatch(*lower_frame, *upper_frame);
  for (std::size_t index = 0; index < lower_frame->bone_states.size();
       ++index) {
    const BoneState &from = lower_frame->bone_states[index];
    const BoneState *to =
        stable_layout ? &upper_frame->bone_states[index]
                      : findState(*upper_frame, from.bone_name);
    if (to == nullptr) {
      to = &from;
    }
    BoneState &target = scratch.frame.bone_states[index];
    if (fraction <= kTimeEpsilon) {
      copyStateValues(from, target);
    } else if (fraction >= 1.0 - kTimeEpsilon) {
      copyStateValues(*to, target);
    } else {
      interpolateState(from, *to, scratch.bind_rotations[index], fraction,
                       target);
    }
  }
  return scratch.frame;
}

}
