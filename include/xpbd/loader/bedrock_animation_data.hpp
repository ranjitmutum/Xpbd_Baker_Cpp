#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace xpbd::loader {


struct Keyframes {
  enum class InterpolationMode { Linear, CatmullRom };


  std::map<double, std::array<double, 3>> keyframes;
  std::map<double, std::array<double, 3>> pre_keyframes;
  std::map<double, InterpolationMode> interpolation_modes;
  bool looping = false;
  bool contains_molang = false;

  nlohmann::json original_authored_json = nullptr;

  nlohmann::json original_molang_json = nullptr;

  void put(double time, const std::array<double, 3> &post);
  void put(double time, const std::array<double, 3> &pre,
           const std::array<double, 3> &post,
           InterpolationMode mode = InterpolationMode::Linear);

  void setLooping(bool value) { looping = value; }

  [[nodiscard]] std::array<double, 3> evaluate(double time) const;
  [[nodiscard]] std::array<double, 3> preValue(double time) const;
  [[nodiscard]] bool hasDistinctPrePost(double time) const;
  [[nodiscard]] InterpolationMode interpolationMode(double time) const;
  [[nodiscard]] bool containsMolang() const { return contains_molang; }
  [[nodiscard]] bool hasOriginalAuthoredJson() const {
    return !original_authored_json.is_null();
  }
  [[nodiscard]] bool hasOriginalMolang() const {
    return !original_molang_json.is_null();
  }

  [[nodiscard]] static bool isMolangValue(const nlohmann::json &value);
  [[nodiscard]] static Keyframes
  fromJson(const nlohmann::json &elem,
           const std::string &context = "animation keyframe");

private:
  static double catmullRom(double t, double p0, double p1, double p2,
                           double p3);
  static double requireTime(double time, const std::string &context);
  static double requireFinite(double value, const std::string &label);
  static std::array<double, 3> parseVector(const nlohmann::json &arr,
                                           const std::string &context);
  struct ParsedValue {
    std::array<double, 3> pre{};
    std::array<double, 3> post{};
    InterpolationMode mode = InterpolationMode::Linear;
  };
  static ParsedValue parseKeyframeValue(const nlohmann::json &elem,
                                        const std::string &context);
  static InterpolationMode parseInterpolationMode(const nlohmann::json &object,
                                                  const std::string &context);
  static std::array<double, 3> parseVectorValue(const nlohmann::json &elem,
                                                const std::string &context);
};

struct BoneAnimation {
  Keyframes position;
  Keyframes rotation;
  Keyframes scale;
  bool has_position = false;
  bool has_rotation = false;
  bool has_scale = false;

  void setLooping(bool looping);
  [[nodiscard]] static BoneAnimation
  fromJson(const nlohmann::json &json,
           const std::string &animation_name = "<animation>",
           const std::string &bone_name = "<bone>");
};

struct Animation {
  enum class LoopBehavior { Once, Loop, HoldLast };

  bool loop = false;
  LoopBehavior loop_behavior = LoopBehavior::Once;
  double animation_length = 0.0;
  std::optional<bool> override_previous_animation;
  std::map<std::string, BoneAnimation> bones;

  [[nodiscard]] static Animation
  fromJson(const nlohmann::json &json,
           const std::string &animation_name = "<animation>");

private:
  static double maximumKeyframeTime(const Animation &animation);
  static double maximumKeyframeTime(const Keyframes &channel);
};

struct AnimationRoot {
  std::string format_version;
  std::map<std::string, Animation> animations;

  std::vector<std::string> animation_order;

  [[nodiscard]] static AnimationRoot fromJson(const nlohmann::json &json);
  [[nodiscard]] static AnimationRoot
  fromOrderedJson(const nlohmann::ordered_json &json);
};

}
