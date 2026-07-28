#include "xpbd/loader/bedrock_animation_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace xpbd::loader {

namespace {

std::string trimAsciiWhitespace(const std::string &text) {
  std::size_t start = 0;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(start, end - start);
}

double parseCompleteNumberText(const std::string &text,
                               const std::string &context) {
  const std::string trimmed = trimAsciiWhitespace(text);
  if (trimmed.empty()) {
    throw std::invalid_argument(context + " must be numeric");
  }
  std::size_t consumed = 0;
  double value = 0.0;
  try {
    value = std::stod(trimmed, &consumed);
  } catch (const std::invalid_argument &) {
    throw std::invalid_argument(context + " must be numeric");
  }
  if (consumed != trimmed.size()) {
    throw std::invalid_argument(context +
                                " must be a complete numeric value");
  }
  return value;
}

double parseStrictFiniteNumberText(const std::string &text,
                                   const std::string &context) {
  double value = 0.0;
  try {
    value = parseCompleteNumberText(text, context);
  } catch (const std::out_of_range &) {
    throw std::invalid_argument(context + " must be finite");
  }
  if (!std::isfinite(value)) {
    throw std::invalid_argument(context + " must be finite");
  }
  return value;
}

}

void Keyframes::put(double time, const std::array<double, 3> &post) {
  put(time, post, post, InterpolationMode::Linear);
}

void Keyframes::put(double time, const std::array<double, 3> &pre,
                    const std::array<double, 3> &post, InterpolationMode mode) {
  original_authored_json = nullptr;
  original_molang_json = nullptr;

  pre_keyframes[time] = pre;
  keyframes[time] = post;
  interpolation_modes[time] = mode;
}

std::array<double, 3> Keyframes::preValue(double time) const {
  auto preIt = pre_keyframes.find(time);
  if (preIt != pre_keyframes.end()) {
    return preIt->second;
  }
  auto postIt = keyframes.find(time);
  if (postIt != keyframes.end()) {
    return postIt->second;
  }
  return {0.0, 0.0, 0.0};
}

bool Keyframes::hasDistinctPrePost(double time) const {
  auto preIt = pre_keyframes.find(time);
  auto postIt = keyframes.find(time);
  if (preIt == pre_keyframes.end() || postIt == keyframes.end()) {
    return false;
  }
  return preIt->second != postIt->second;
}

Keyframes::InterpolationMode Keyframes::interpolationMode(double time) const {
  auto it = interpolation_modes.find(time);
  return it == interpolation_modes.end() ? InterpolationMode::Linear
                                         : it->second;
}

bool Keyframes::isMolangValue(const nlohmann::json &value) {
  if (value.is_null()) {
    return false;
  }
  if (value.is_array()) {
    for (const auto &item : value) {
      if (isMolangValue(item)) {
        return true;
      }
    }
    return false;
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (it.key() == "time" || it.key() == "lerp_mode") {
        continue;
      }
      if (isMolangValue(it.value())) {
        return true;
      }
    }
    return false;
  }
  if (!value.is_string()) {
    return false;
  }
  const std::string text = value.get<std::string>();
  if (trimAsciiWhitespace(text).empty()) {
    return false;
  }
  try {
    (void)parseCompleteNumberText(text, "animation value");
    return false;
  } catch (const std::out_of_range &) {


    return false;
  } catch (const std::invalid_argument &) {
    return true;
  }
}

double Keyframes::requireTime(double time, const std::string &context) {
  if (!std::isfinite(time) || time < 0.0) {
    throw std::invalid_argument(context +
                                " must be a finite non-negative number");
  }
  return time;
}

double Keyframes::requireFinite(double value, const std::string &label) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(label + " must be finite");
  }
  return value;
}

std::array<double, 3> Keyframes::parseVector(const nlohmann::json &arr,
                                             const std::string &context) {
  if (!arr.is_array() || arr.size() != 3) {
    throw std::invalid_argument(
        context + ": vector must contain exactly three components");
  }
  std::array<double, 3> result{};
  for (int i = 0; i < 3; ++i) {
    const auto &e = arr[static_cast<std::size_t>(i)];
    if (e.is_number()) {
      result[static_cast<std::size_t>(i)] = requireFinite(
          e.get<double>(), context + " component " + std::to_string(i));
    } else if (e.is_string()) {
      if (isMolangValue(e)) {
        result[static_cast<std::size_t>(i)] = 0.0;
      } else {
        result[static_cast<std::size_t>(i)] = parseStrictFiniteNumberText(
            e.get<std::string>(),
            context + " component " + std::to_string(i));
      }
    } else {
      throw std::invalid_argument(context + " component " + std::to_string(i) +
                                  " must be numeric");
    }
  }
  return result;
}

Keyframes::InterpolationMode
Keyframes::parseInterpolationMode(const nlohmann::json &object,
                                  const std::string &context) {
  if (!object.contains("lerp_mode")) {
    return InterpolationMode::Linear;
  }
  const auto &element = object.at("lerp_mode");
  if (!element.is_string()) {
    throw std::invalid_argument(context +
                                ": interpolation mode must be a string");
  }
  const std::string mode = element.get<std::string>();
  if (mode == "linear" || mode == "LINEAR") {
    return InterpolationMode::Linear;
  }

  std::string lower = mode;
  for (char &c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower == "linear") {
    return InterpolationMode::Linear;
  }
  if (lower == "catmullrom") {
    return InterpolationMode::CatmullRom;
  }
  throw std::invalid_argument(context + ": unsupported interpolation mode '" +
                              mode + "'");
}

std::array<double, 3> Keyframes::parseVectorValue(const nlohmann::json &elem,
                                                  const std::string &context) {
  if (elem.is_array()) {
    return parseVector(elem, context);
  }
  const ParsedValue scalar = parseKeyframeValue(elem, context);
  if (scalar.pre != scalar.post) {
    throw std::invalid_argument(
        context + ": nested pre/post keyframes are not supported");
  }
  return scalar.post;
}

Keyframes::ParsedValue
Keyframes::parseKeyframeValue(const nlohmann::json &elem,
                              const std::string &context) {
  if (elem.is_null()) {
    throw std::invalid_argument(context + ": keyframe value is missing");
  }
  if (elem.is_array()) {
    const auto value = parseVector(elem, context);
    return ParsedValue{value, value, InterpolationMode::Linear};
  }
  if (elem.is_object()) {
    const InterpolationMode mode = parseInterpolationMode(elem, context);
    std::optional<std::array<double, 3>> pre;
    std::optional<std::array<double, 3>> post;
    if (elem.contains("pre")) {
      pre = parseVectorValue(elem.at("pre"), context + " pre");
    }
    if (elem.contains("post")) {
      post = parseVectorValue(elem.at("post"), context + " post");
    }
    if (!pre && !post) {
      throw std::invalid_argument(context +
                                  ": object keyframe needs pre or post");
    }
    if (!pre) {
      pre = post;
    }
    if (!post) {
      post = pre;
    }
    return ParsedValue{*pre, *post, mode};
  }
  if (elem.is_number()) {
    const double scalar =
        requireFinite(elem.get<double>(), context + " component");
    const std::array<double, 3> value{scalar, scalar, scalar};
    return ParsedValue{value, value, InterpolationMode::Linear};
  }
  if (elem.is_string()) {
    if (isMolangValue(elem)) {
      const std::array<double, 3> value{0.0, 0.0, 0.0};
      return ParsedValue{value, value, InterpolationMode::Linear};
    }
    const double scalar = parseStrictFiniteNumberText(
        elem.get<std::string>(), context + " component");
    const std::array<double, 3> value{scalar, scalar, scalar};
    return ParsedValue{value, value, InterpolationMode::Linear};
  }
  throw std::invalid_argument(context + ": unsupported keyframe value");
}

Keyframes Keyframes::fromJson(const nlohmann::json &elem,
                              const std::string &context) {
  if (elem.is_null()) {
    throw std::invalid_argument(context + ": keyframe data is missing");
  }
  Keyframes kf;
  kf.contains_molang = isMolangValue(elem);

  if (elem.is_number()) {
    const double v =
        requireFinite(elem.get<double>(), context + " at time 0 component");
    const std::array<double, 3> value{v, v, v};
    kf.put(0.0, value, value, InterpolationMode::Linear);
  } else if (elem.is_string()) {
    if (isMolangValue(elem)) {
      const std::array<double, 3> value{0.0, 0.0, 0.0};
      kf.put(0.0, value, value, InterpolationMode::Linear);
    } else {
      const double scalar = parseStrictFiniteNumberText(
          elem.get<std::string>(), context + " at time 0 component");
      const std::array<double, 3> value{scalar, scalar, scalar};
      kf.put(0.0, value, value, InterpolationMode::Linear);
    }
  } else if (elem.is_array()) {
    if (!elem.empty() && (elem[0].is_number() || elem[0].is_string())) {
      const auto value = parseVector(elem, context + " at time 0");
      kf.put(0.0, value, value, InterpolationMode::Linear);
    } else {
      for (const auto &item : elem) {
        if (!item.is_object()) {
          throw std::invalid_argument(context +
                                      ": timed keyframes must be objects");
        }
        if (!item.contains("time")) {
          throw std::invalid_argument(context + " field 'time' is missing");
        }
        const auto &authoredTime = item.at("time");
        const std::string timeContext = context + " field 'time'";
        double time = 0.0;
        if (authoredTime.is_number()) {
          time = requireTime(authoredTime.get<double>(), timeContext);
        } else if (authoredTime.is_string()) {
          time = requireTime(parseStrictFiniteNumberText(
                                 authoredTime.get<std::string>(), timeContext),
                             timeContext);
        } else {
          throw std::invalid_argument(timeContext + " must be numeric");
        }
        if (!item.contains("data")) {
          throw std::invalid_argument(context + " at time " +
                                      std::to_string(time) +
                                      ": keyframe value is missing");
        }
        const ParsedValue value = parseKeyframeValue(
            item.at("data"), context + " at time " + std::to_string(time));
        kf.put(time, value.pre, value.post, value.mode);
      }
    }
  } else if (elem.is_object()) {
    if (elem.contains("pre") || elem.contains("post") ||
        elem.contains("lerp_mode")) {
      const ParsedValue value =
          parseKeyframeValue(elem, context + " at time 0");
      kf.put(0.0, value.pre, value.post, value.mode);
    } else {
      for (auto it = elem.begin(); it != elem.end(); ++it) {
        const std::string timeContext =
            context + " keyframe time '" + it.key() + "'";
        const double time = requireTime(
            parseStrictFiniteNumberText(it.key(), timeContext), timeContext);
        const ParsedValue value =
            parseKeyframeValue(it.value(), context + " at time " + it.key());
        kf.put(time, value.pre, value.post, value.mode);
      }
    }
  } else {
    throw std::invalid_argument(context + ": unsupported keyframe data");
  }
  kf.original_authored_json = elem;
  if (kf.contains_molang) {
    kf.original_molang_json = elem;
  }
  return kf;
}

std::array<double, 3> Keyframes::evaluate(double time) const {
  if (keyframes.empty()) {
    return {0.0, 0.0, 0.0};
  }
  const double queryTime = std::isfinite(time) ? std::max(0.0, time) : 0.0;

  auto next = keyframes.lower_bound(queryTime);
  if (next != keyframes.end() && std::abs(next->first - queryTime) <= 1e-12) {
    return next->second;
  }


  auto previous = next;
  if (previous == keyframes.begin()) {
    previous = keyframes.end();
  } else {
    --previous;
  }

  if (previous == keyframes.end()) {
    if (next == keyframes.end()) {
      return {0.0, 0.0, 0.0};
    }
    return preValue(next->first);
  }
  if (std::abs(previous->first - queryTime) <= 1e-12) {
    return previous->second;
  }
  if (next == keyframes.end()) {
    return previous->second;
  }

  const double t =
      (queryTime - previous->first) / (next->first - previous->first);
  const auto &from = previous->second;
  const auto to = preValue(next->first);
  std::array<double, 3> result{};

  const bool catmull =
      interpolationMode(previous->first) == InterpolationMode::CatmullRom ||
      interpolationMode(next->first) == InterpolationMode::CatmullRom;
  if (catmull) {
    auto beforePrevious = previous;
    if (beforePrevious == keyframes.begin()) {
      beforePrevious = keyframes.end();
    } else {
      --beforePrevious;
    }
    auto afterNext = next;
    ++afterNext;

    if (looping && keyframes.size() >= 3) {
      if (beforePrevious == keyframes.end()) {
        auto it = keyframes.end();
        --it;
        --it;
        beforePrevious = it;
      }
      if (afterNext == keyframes.end()) {
        afterNext = keyframes.begin();
        ++afterNext;
      }
    }

    const auto outerBefore = beforePrevious != keyframes.end() &&
                                     !hasDistinctPrePost(previous->first)
                                 ? beforePrevious->second
                                 : from;
    const auto outerAfter =
        afterNext != keyframes.end() && !hasDistinctPrePost(next->first)
            ? preValue(afterNext->first)
            : to;

    for (int i = 0; i < 3; ++i) {
      result[static_cast<std::size_t>(i)] = catmullRom(
          t, outerBefore[static_cast<std::size_t>(i)],
          from[static_cast<std::size_t>(i)], to[static_cast<std::size_t>(i)],
          outerAfter[static_cast<std::size_t>(i)]);
    }
  } else {
    for (int i = 0; i < 3; ++i) {
      result[static_cast<std::size_t>(i)] =
          from[static_cast<std::size_t>(i)] +
          (to[static_cast<std::size_t>(i)] -
           from[static_cast<std::size_t>(i)]) *
              t;
    }
  }
  return result;
}

double Keyframes::catmullRom(double t, double p0, double p1, double p2,
                             double p3) {
  const double v0 = (p2 - p0) * 0.5;
  const double v1 = (p3 - p1) * 0.5;
  const double t2 = t * t;
  const double t3 = t2 * t;
  return (2 * p1 - 2 * p2 + v0 + v1) * t3 +
         (-3 * p1 + 3 * p2 - 2 * v0 - v1) * t2 + v0 * t + p1;
}

void BoneAnimation::setLooping(bool looping) {
  if (has_position) {
    position.setLooping(looping);
  }
  if (has_rotation) {
    rotation.setLooping(looping);
  }
  if (has_scale) {
    scale.setLooping(looping);
  }
}

BoneAnimation BoneAnimation::fromJson(const nlohmann::json &json,
                                      const std::string &animation_name,
                                      const std::string &bone_name) {
  BoneAnimation ba;
  auto channelContext = [&](const char *channel) {
    return "animation '" + animation_name + "', bone '" + bone_name +
           "', channel '" + channel + "'";
  };
  if (json.contains("position")) {
    ba.position =
        Keyframes::fromJson(json.at("position"), channelContext("position"));
    ba.has_position = true;
  }
  if (json.contains("rotation")) {
    ba.rotation =
        Keyframes::fromJson(json.at("rotation"), channelContext("rotation"));
    ba.has_rotation = true;
  }
  if (json.contains("scale")) {
    ba.scale = Keyframes::fromJson(json.at("scale"), channelContext("scale"));
    ba.has_scale = true;
  }
  return ba;
}

double Animation::maximumKeyframeTime(const Keyframes &channel) {
  if (channel.keyframes.empty()) {
    return 0.0;
  }
  return channel.keyframes.rbegin()->first;
}

double Animation::maximumKeyframeTime(const Animation &animation) {
  double maximum = 0.0;
  for (const auto &[name, bone] : animation.bones) {
    (void)name;
    if (bone.has_position) {
      maximum = std::max(maximum, maximumKeyframeTime(bone.position));
    }
    if (bone.has_rotation) {
      maximum = std::max(maximum, maximumKeyframeTime(bone.rotation));
    }
    if (bone.has_scale) {
      maximum = std::max(maximum, maximumKeyframeTime(bone.scale));
    }
  }
  return maximum;
}

Animation Animation::fromJson(const nlohmann::json &json,
                              const std::string &animation_name) {
  Animation a;
  a.original_json = json;
  if (json.contains("loop")) {
    const auto &loopElem = json.at("loop");
    if (loopElem.is_boolean()) {
      a.loop = loopElem.get<bool>();
      a.loop_behavior = a.loop ? LoopBehavior::Loop : LoopBehavior::Once;
    } else if (loopElem.is_string() &&
               loopElem.get<std::string>() == "hold_on_last_frame") {
      a.loop = false;
      a.loop_behavior = LoopBehavior::HoldLast;
    } else {
      throw std::invalid_argument(
          "loop must be boolean or 'hold_on_last_frame'");
    }
  }
  bool lengthMissing = !json.contains("animation_length");
  if (!lengthMissing) {
    a.animation_length = json.at("animation_length").get<double>();
  } else {
    a.animation_length = std::numeric_limits<double>::quiet_NaN();
  }
  if (json.contains("override_previous_animation")) {
    const auto &override = json.at("override_previous_animation");
    if (!override.is_boolean()) {
      throw std::invalid_argument(
          "override_previous_animation must be boolean");
    }
    a.override_previous_animation = override.get<bool>();
  }
  if (json.contains("bones") && json.at("bones").is_object()) {
    for (auto it = json.at("bones").begin(); it != json.at("bones").end();
         ++it) {
      BoneAnimation boneAnimation =
          BoneAnimation::fromJson(it.value(), animation_name, it.key());
      boneAnimation.setLooping(a.loop);
      a.bones.emplace(it.key(), std::move(boneAnimation));
    }
  }
  if (std::isnan(a.animation_length)) {
    a.animation_length = maximumKeyframeTime(a);
  } else if (!std::isfinite(a.animation_length) || a.animation_length < 0.0) {
    throw std::invalid_argument(
        "animation_length must be a finite non-negative number");
  }
  return a;
}

AnimationRoot AnimationRoot::fromJson(const nlohmann::json &json) {
  AnimationRoot root;
  if (!json.contains("format_version")) {
    throw std::invalid_argument("missing format_version");
  }
  root.format_version = json.at("format_version").get<std::string>();
  if (!json.contains("animations") || !json.at("animations").is_object()) {
    throw std::invalid_argument("missing animations object");
  }
  for (auto it = json.at("animations").begin();
       it != json.at("animations").end(); ++it) {
    root.animation_order.push_back(it.key());
    root.animations.emplace(it.key(),
                            Animation::fromJson(it.value(), it.key()));
  }
  return root;
}

AnimationRoot
AnimationRoot::fromOrderedJson(const nlohmann::ordered_json &json) {
  AnimationRoot root;
  if (!json.contains("format_version")) {
    throw std::invalid_argument("missing format_version");
  }
  root.format_version = json.at("format_version").get<std::string>();
  if (!json.contains("animations") || !json.at("animations").is_object()) {
    throw std::invalid_argument("missing animations object");
  }
  for (auto it = json.at("animations").begin();
       it != json.at("animations").end(); ++it) {
    root.animation_order.push_back(it.key());
    root.animations.emplace(
        it.key(), Animation::fromJson(nlohmann::json(it.value()), it.key()));
  }
  return root;
}

}
