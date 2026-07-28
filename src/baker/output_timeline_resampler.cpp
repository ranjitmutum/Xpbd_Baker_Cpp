#include "xpbd/baker/output_timeline_resampler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace xpbd::baker {

namespace {

constexpr double kTimeEpsilon = 1e-12;
constexpr int kMinimumSourceSnappingFps = 10;
constexpr int kMaximumSourceSnappingFps = 500;
// Blockbench 导出的时间码通常会进行十进制裁剪。这里同时保留旧实现
// 的 0.015 帧容差，并允许约 4 位小数的时间码舍入误差。
constexpr double kSourceTimeRoundingTolerance = 5.1e-5;

double snappingFrameTolerance(int fps) {
  return std::max(0.015,
                  static_cast<double>(fps) * kSourceTimeRoundingTolerance);
}

bool timeMatchesSnapping(double time, int fps) {
  const double frame = time * static_cast<double>(fps);
  return std::abs(frame - std::nearbyint(frame)) <=
         snappingFrameTolerance(fps);
}

int declaredSnappingFps(const nlohmann::json &animation_json) {
  if (!animation_json.is_object() ||
      !animation_json.contains("snapping") ||
      !animation_json.at("snapping").is_number()) {
    return 0;
  }
  const double value = animation_json.at("snapping").get<double>();
  const double rounded = std::nearbyint(value);
  if (!std::isfinite(value) ||
      std::abs(value - rounded) > 1e-9 ||
      rounded < static_cast<double>(kMinimumSourceSnappingFps) ||
      rounded > static_cast<double>(kMaximumSourceSnappingFps)) {
    return 0;
  }
  return static_cast<int>(rounded);
}

RotationUtil::Quat normalized(RotationUtil::Quat value) {
  double length_squared = 0.0;
  for (double component : value) {
    length_squared += component * component;
  }
  if (!std::isfinite(length_squared) || !(length_squared > 1e-24)) {
    return {0.0, 0.0, 0.0, 1.0};
  }
  const double inverse_length = 1.0 / std::sqrt(length_squared);
  for (double &component : value) {
    component *= inverse_length;
  }
  return value;
}

std::array<double, 3> lerp(const std::array<double, 3> &from,
                           const std::array<double, 3> &to,
                           double fraction) {
  std::array<double, 3> result{};
  for (std::size_t axis = 0; axis < result.size(); ++axis) {
    result[axis] = from[axis] + (to[axis] - from[axis]) * fraction;
  }
  return result;
}

void collectKeyTimes(std::set<double> &target,
                     const loader::Keyframes &channel) {
  for (const auto &[time, unused] : channel.keyframes) {
    (void)unused;
    if (std::isfinite(time) && time >= 0.0) {
      target.insert(time);
    }
  }
}

void collectEffectKeyTimes(std::set<double> &target,
                           const nlohmann::json &animation_json,
                           const char *field_name) {
  if (!animation_json.is_object() || !animation_json.contains(field_name) ||
      !animation_json.at(field_name).is_object()) {
    return;
  }
  for (auto it = animation_json.at(field_name).begin();
       it != animation_json.at(field_name).end(); ++it) {
    try {
      std::size_t consumed = 0;
      const double time = std::stod(it.key(), &consumed);
      if (consumed == it.key().size() && std::isfinite(time) && time >= 0.0) {
        target.insert(time);
      }
    } catch (const std::exception &) {
      // 非时间键不是有效的 Bedrock 效果关键帧，检测时忽略即可。
    }
  }
}

}

int OutputTimelineResampler::detectSourceSnappingFps(
    const loader::Animation &animation) {
  // Blockbench 动画对象已经保存了 snapping；第二模式优先直接读取，
  // 不再用稀疏关键帧反推并误判源吸附。仅在旧文件没有该字段时回退推断。
  if (const int declared = declaredSnappingFps(animation.original_json);
      declared > 0) {
    return declared;
  }

  std::set<double> key_times;
  for (const auto &[unused_name, bone] : animation.bones) {
    (void)unused_name;
    collectKeyTimes(key_times, bone.position);
    collectKeyTimes(key_times, bone.rotation);
    collectKeyTimes(key_times, bone.scale);
  }
  collectEffectKeyTimes(key_times, animation.original_json, "timeline");
  collectEffectKeyTimes(key_times, animation.original_json, "sound_effects");
  collectEffectKeyTimes(key_times, animation.original_json,
                        "particle_effects");
  if (std::isfinite(animation.animation_length) &&
      animation.animation_length >= 0.0) {
    key_times.insert(animation.animation_length);
  }
  if (key_times.size() < 2) {
    return 0;
  }

  // Blockbench 的动画吸附是整数 FPS。返回能容纳全部源时间码的
  // 最小网格，避免把 20 FPS 源动画误判成同样兼容的 40/60 FPS。
  for (int fps = kMinimumSourceSnappingFps;
       fps <= kMaximumSourceSnappingFps; ++fps) {
    const bool aligned = std::all_of(
        key_times.begin(), key_times.end(),
        [fps](double time) { return timeMatchesSnapping(time, fps); });
    if (aligned) {
      return fps;
    }
  }
  return 0;
}

std::size_t OutputTimelineResampler::snappedFrameCount(
    double clip_length, int snapping_fps,
    OutputEndpointPolicy endpoint_policy) {
  if (!std::isfinite(clip_length) || clip_length < 0.0 || snapping_fps <= 0) {
    return 0;
  }
  const double frame_position =
      clip_length * static_cast<double>(snapping_fps);
  const double rounded_position = std::nearbyint(frame_position);
  if (std::abs(frame_position - rounded_position) >
          snappingFrameTolerance(snapping_fps) ||
      rounded_position < 0.0 ||
      rounded_position >
          static_cast<double>(std::numeric_limits<std::size_t>::max() - 1U)) {
    return 0;
  }
  const auto steps = static_cast<std::size_t>(rounded_position);
  if (endpoint_policy == OutputEndpointPolicy::HalfOpenPeriodic) {
    return steps;
  }
  return steps + 1U;
}

double OutputTimelineResampler::inferSourceFrameInterval(
    const loader::Animation &animation, double fallback_interval) {
  const int source_snapping_fps = detectSourceSnappingFps(animation);
  return source_snapping_fps > 0
             ? 1.0 / static_cast<double>(source_snapping_fps)
             : fallback_interval;
}

std::vector<BakedFrame> OutputTimelineResampler::resampleToSnappingFps(
    const std::vector<BakedFrame> &source,
    const std::map<std::string, loader::Bone> &bones_by_name,
    int snapping_fps, double clip_length,
    OutputEndpointPolicy endpoint_policy) {
  const std::size_t expected_count =
      snappedFrameCount(clip_length, snapping_fps, endpoint_policy);
  if (expected_count == 0) {
    throw std::invalid_argument(
        "source animation length is not aligned to the declared snapping grid");
  }

  const std::size_t snapped_steps =
      endpoint_policy == OutputEndpointPolicy::HalfOpenPeriodic
          ? expected_count
          : expected_count - 1U;
  const double snapped_length =
      static_cast<double>(snapped_steps) /
      static_cast<double>(snapping_fps);

  // animation_length 常被 JSON 十进制格式化成 1.6667 之类的值。
  // 若直接按该浮点长度 floor，会在网格上方舍入时多生成一帧。
  // 严格模式应由 snapping 和整数步数构造时间线，而不是再次推算帧数。
  const std::vector<BakedFrame> *sampling_source = &source;
  std::vector<BakedFrame> extended_source;
  if (!source.empty() && source.back().time < snapped_length) {
    const double maximum_rounding_gap =
        snappingFrameTolerance(snapping_fps) /
            static_cast<double>(snapping_fps) +
        kTimeEpsilon;
    if (source.back().time < clip_length - kTimeEpsilon ||
        snapped_length - source.back().time > maximum_rounding_gap) {
      throw std::invalid_argument(
          "output resample source does not cover the declared snapping grid");
    }
    extended_source = source;
    BakedFrame endpoint = extended_source.back();
    endpoint.time = snapped_length;
    extended_source.push_back(std::move(endpoint));
    sampling_source = &extended_source;
  }

  auto result = resample(*sampling_source, bones_by_name,
                         1.0 / static_cast<double>(snapping_fps),
                         snapped_length, endpoint_policy);
  if (result.size() != expected_count) {
    throw std::logic_error(
        "strict source snapping resample produced an unexpected frame count");
  }
  return result;
}

RotationUtil::Quat OutputTimelineResampler::interpolateQuaternionShortestArc(
    const RotationUtil::Quat &from, const RotationUtil::Quat &to,
    double fraction) {
  const double t = std::clamp(fraction, 0.0, 1.0);
  RotationUtil::Quat first = normalized(from);
  RotationUtil::Quat second = normalized(to);
  double dot = 0.0;
  for (std::size_t index = 0; index < first.size(); ++index) {
    dot += first[index] * second[index];
  }
  if (dot < 0.0) {
    dot = -dot;
    for (double &component : second) {
      component = -component;
    }
  }
  dot = std::clamp(dot, -1.0, 1.0);

  RotationUtil::Quat result{};
  if (dot > 0.9995) {
    for (std::size_t index = 0; index < result.size(); ++index) {
      result[index] = first[index] + (second[index] - first[index]) * t;
    }
    return normalized(result);
  }

  const double angle = std::acos(dot);
  const double sine = std::sin(angle);
  if (!(std::abs(sine) > std::numeric_limits<double>::epsilon())) {
    return first;
  }
  const double first_weight = std::sin((1.0 - t) * angle) / sine;
  const double second_weight = std::sin(t * angle) / sine;
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = first[index] * first_weight + second[index] * second_weight;
  }
  return normalized(result);
}

BakedFrame OutputTimelineResampler::interpolateFrame(
    const std::vector<BakedFrame> &source,
    const std::map<std::string, loader::Bone> &bones_by_name,
    std::size_t suggested_lower_index, double time,
    const StableFrameLayout *stable_layout,
    const std::vector<std::array<double, 3>>
        *bind_rotations_by_index) {
  std::size_t lower_index =
      std::min(suggested_lower_index, source.size() - 1);
  while (lower_index + 1 < source.size() &&
         source[lower_index + 1].time < time - kTimeEpsilon) {
    ++lower_index;
  }
  const BakedFrame &lower = source[lower_index];
  const BakedFrame &upper = source[std::min(lower_index + 1, source.size() - 1)];
  const double span = upper.time - lower.time;
  const double fraction = span > kTimeEpsilon
                              ? std::clamp((time - lower.time) / span, 0.0, 1.0)
                              : 0.0;
  if (fraction <= kTimeEpsilon) {
    BakedFrame result = lower;
    result.time = time;
    return result;
  }
  if (fraction >= 1.0 - kTimeEpsilon) {
    BakedFrame result = upper;
    result.time = time;
    return result;
  }

  BakedFrame result;
  result.time = time;
  result.bone_states.reserve(lower.bone_states.size());
  for (std::size_t state_index = 0;
       state_index < lower.bone_states.size(); ++state_index) {
    const BoneState &from = lower.bone_states[state_index];
    const BoneState *to =
        stable_layout != nullptr
            ? &upper.bone_states[state_index]
            : upper.getBoneState(from.bone_name);
    to = to != nullptr ? to : &from;
    BoneState state;
    state.bone_name = from.bone_name;
    state.position = lerp(from.position, to->position, fraction);
    state.linear_velocity =
        lerp(from.linear_velocity, to->linear_velocity, fraction);
    state.world_position = lerp(from.world_position, to->world_position, fraction);
    state.has_world_position = from.has_world_position && to->has_world_position;

    std::array<double, 3> bind_rotation =
        bind_rotations_by_index != nullptr
            ? (*bind_rotations_by_index)[state_index]
            : std::array<double, 3>{};
    if (bind_rotations_by_index == nullptr) {
      if (const auto bone = bones_by_name.find(from.bone_name);
          bone != bones_by_name.end()) {
        std::copy(std::begin(bone->second.rotation),
                  std::end(bone->second.rotation), bind_rotation.begin());
      }
    }
    const RotationUtil::Vec3 from_total{
        bind_rotation[0] + from.rotation[0],
        bind_rotation[1] + from.rotation[1],
        bind_rotation[2] + from.rotation[2]};
    const RotationUtil::Vec3 to_total{
        bind_rotation[0] + to->rotation[0],
        bind_rotation[1] + to->rotation[1],
        bind_rotation[2] + to->rotation[2]};
    const RotationUtil::Quat from_q = RotationUtil::quaternionFromBedrockEuler(
        from_total[0], from_total[1], from_total[2]);
    const RotationUtil::Quat to_q = RotationUtil::quaternionFromBedrockEuler(
        to_total[0], to_total[1], to_total[2]);
    const RotationUtil::Quat interpolated_q =
        interpolateQuaternionShortestArc(from_q, to_q, fraction);
    const RotationUtil::Vec3 interpolated_total = RotationUtil::unwrapEuler(
        from_total, RotationUtil::bedrockEulerFromQuaternion(interpolated_q));
    for (std::size_t axis = 0; axis < state.rotation.size(); ++axis) {
      state.rotation[axis] = interpolated_total[axis] - bind_rotation[axis];
    }
    result.bone_states.push_back(std::move(state));
  }
  result.rebuildIndex();
  return result;
}

std::vector<BakedFrame> OutputTimelineResampler::resample(
    const std::vector<BakedFrame> &source,
    const std::map<std::string, loader::Bone> &bones_by_name,
    double target_interval, double clip_length,
    OutputEndpointPolicy endpoint_policy) {
  if (source.size() < 2) {
    return source;
  }
  if (!std::isfinite(target_interval) || !(target_interval > 0.0) ||
      !std::isfinite(clip_length) || !(clip_length > 0.0)) {
    throw std::invalid_argument(
        "output resample interval and clip length must be finite and positive");
  }
  for (std::size_t index = 0; index < source.size(); ++index) {
    if (!std::isfinite(source[index].time) ||
        (index > 0 && !(source[index].time > source[index - 1].time))) {
      throw std::invalid_argument(
          "output resample source times must be finite and strictly increasing");
    }
  }
  if (source.front().time > kTimeEpsilon ||
      source.back().time < clip_length - kTimeEpsilon) {
    throw std::invalid_argument(
        "output resample source must cover the requested [0,L] interval");
  }

  const auto stable_layout = StableFrameLayout::tryCreate(source);
  std::vector<std::array<double, 3>> bind_rotations_by_index;
  if (stable_layout.has_value()) {
    bind_rotations_by_index.resize(stable_layout->bone_names.size());
    for (std::size_t index = 0; index < stable_layout->bone_names.size();
         ++index) {
      if (const auto bone =
              bones_by_name.find(stable_layout->bone_names[index]);
          bone != bones_by_name.end()) {
        std::copy(std::begin(bone->second.rotation),
                  std::end(bone->second.rotation),
                  bind_rotations_by_index[index].begin());
      }
    }
  }
  const StableFrameLayout *stable_layout_ptr =
      stable_layout.has_value() ? &*stable_layout : nullptr;
  const std::vector<std::array<double, 3>> *bind_rotations_ptr =
      stable_layout.has_value() ? &bind_rotations_by_index : nullptr;

  const auto whole_steps = static_cast<std::size_t>(
      std::floor(clip_length / target_interval + 1e-9));
  std::vector<BakedFrame> result;
  result.reserve(whole_steps + 2);
  std::size_t lower_index = 0;
  for (std::size_t step = 0; step <= whole_steps; ++step) {
    const double time = static_cast<double>(step) * target_interval;
    // step*interval 与 clip_length 网格对齐时约有一半概率向下舍入 1 ulp，
    // 不加 epsilon 会在半开区间末尾多发出一帧 t≈L 的重复帧（等于第 0 帧），
    // Closed 模式下则产生相距 1 ulp 的两帧，毒化接缝速度测量。
    if (time >= clip_length - kTimeEpsilon) {
      break;
    }
    while (lower_index + 1 < source.size() &&
           source[lower_index + 1].time < time - kTimeEpsilon) {
      ++lower_index;
    }
    result.push_back(interpolateFrame(
        source, bones_by_name, lower_index, time, stable_layout_ptr,
        bind_rotations_ptr));
  }
  if (endpoint_policy == OutputEndpointPolicy::Closed) {
    result.push_back(interpolateFrame(source, bones_by_name,
                                      source.size() > 1 ? source.size() - 2 : 0,
                                      clip_length, stable_layout_ptr,
                                      bind_rotations_ptr));
  }
  if (result.empty()) {
    result.push_back(interpolateFrame(source, bones_by_name, 0, 0.0,
                                      stable_layout_ptr,
                                      bind_rotations_ptr));
  }
  return result;
}

}
