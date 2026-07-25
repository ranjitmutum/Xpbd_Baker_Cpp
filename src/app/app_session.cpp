#include "xpbd/app/app_session.hpp"

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/rigid_body_input_compat.hpp"
#include "xpbd/export/animation_exporter.hpp"
#include "xpbd/export/velocity_cache_exporter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif


#include <windows.h>
#include <commdlg.h>

#endif

namespace xpbd::app {
namespace {

void advanceGeneration(std::uint64_t &generation) noexcept {
  if (generation < (std::numeric_limits<std::uint64_t>::max)()) {
    ++generation;
  }
}

bool sameTextureResource(const gfx::TextureImage &lhs,
                         const gfx::TextureImage &rhs) noexcept {
  return lhs.width == rhs.width && lhs.height == rhs.height &&
         lhs.rgba == rhs.rgba;
}

bool hasTextureResourceState(const gfx::TextureImage &texture) noexcept {
  return texture.width != 0 || texture.height != 0 || !texture.rgba.empty();
}


std::vector<std::uint8_t> decodeBase64(std::string_view in) {
  static int kDec[256];
  static bool ready = false;
  if (!ready) {
    for (int &d : kDec) {
      d = -1;
    }
    const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; alphabet[i]; ++i) {
      kDec[static_cast<unsigned char>(alphabet[i])] = i;
    }
    ready = true;
  }
  std::vector<std::uint8_t> out;
  out.reserve(in.size() * 3 / 4);
  int val = 0, valb = -8;
  for (unsigned char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      continue;
    }
    const int d = kDec[c];
    if (d < 0) {
      return {};
    }
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}



std::optional<double> parseBbNumberStrict(const nlohmann::json &v) {
  if (v.is_number()) {
    return v.get<double>();
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();

    try {
      std::size_t idx = 0;
      const double d = std::stod(s, &idx);

      while (idx < s.size() &&
             std::isspace(static_cast<unsigned char>(s[idx]))) {
        ++idx;
      }
      if (idx == s.size()) {
        return d;
      }
    } catch (...) {
    }
    return std::nullopt;
  }
  return 0.0;
}





double evalSimpleMolang(std::string_view expr, double anim_time) {
  struct Parser {
    std::string_view s;
    std::size_t i = 0;
    double anim_time = 0.0;

    void skip() {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
      }
    }
    bool match(std::string_view lit) {
      skip();
      if (s.substr(i, lit.size()) == lit) {
        i += lit.size();
        return true;
      }
      return false;
    }
    double parseExpr() { return parseAdd(); }
    double parseAdd() {
      double v = parseMul();
      for (;;) {
        if (match("+")) {
          v += parseMul();
        } else if (match("-")) {
          v -= parseMul();
        } else {
          break;
        }
      }
      return v;
    }
    double parseMul() {
      double v = parseUnary();
      for (;;) {
        if (match("*")) {
          v *= parseUnary();
        } else if (match("/")) {
          const double d = parseUnary();
          v = (d != 0.0) ? v / d : 0.0;
        } else {
          break;
        }
      }
      return v;
    }
    double parseUnary() {
      if (match("+")) {
        return parseUnary();
      }
      if (match("-")) {
        return -parseUnary();
      }
      return parsePrimary();
    }
    double parsePrimary() {
      skip();
      if (match("(")) {
        const double v = parseExpr();
        match(")");
        return v;
      }

      if (match("query.anim_time") || match("q.anim_time")) {
        return anim_time;
      }

      const std::size_t start = i;
      while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) ||
                              s[i] == '.' || s[i] == '_')) {
        ++i;
      }
      const std::string_view name = s.substr(start, i - start);
      skip();
      if (!name.empty() && i < s.size() && s[i] == '(') {
        match("(");
        const double arg = parseExpr();
        match(")");
        std::string lower(name);
        for (char &c : lower) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        constexpr double kDeg2Rad = 0.017453292519943295;
        if (lower == "math.sin" || lower == "sin") {
          return std::sin(arg * kDeg2Rad);
        }
        if (lower == "math.cos" || lower == "cos") {
          return std::cos(arg * kDeg2Rad);
        }
        if (lower == "math.abs" || lower == "abs") {
          return std::abs(arg);
        }
        return 0.0;
      }

      i = start;
      skip();
      if (i < s.size() &&
          (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
        std::size_t idx = 0;
        try {
          const double d = std::stod(std::string(s.substr(i)), &idx);
          i += idx;
          return d;
        } catch (...) {
          return 0.0;
        }
      }

      while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) ||
                              s[i] == '.' || s[i] == '_')) {
        ++i;
      }
      return 0.0;
    }
  };
  Parser p;
  p.s = expr;
  p.anim_time = anim_time;
  try {
    return p.parseExpr();
  } catch (...) {
    return 0.0;
  }
}

std::string jsonAxisToString(const nlohmann::json &v) {
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_number()) {
    return std::to_string(v.get<double>());
  }
  return "0";
}


struct BbAxis {
  bool is_molang = false;
  double value = 0.0;
  std::string expr;
};

BbAxis parseBbAxis(const nlohmann::json &v) {
  BbAxis a;
  if (auto n = parseBbNumberStrict(v)) {
    a.value = *n;
    return a;
  }
  a.is_molang = true;
  a.expr = jsonAxisToString(v);
  return a;
}


void bbToBedrockChannelAxes(const std::string &channel, BbAxis &x, BbAxis &y,
                            BbAxis &z) {
  if (channel == "position") {

    if (x.is_molang) {
      x.expr = "-(" + x.expr + ")";
    } else {
      x.value = -x.value;
    }
    (void)y;
    (void)z;
  } else if (channel == "rotation") {

    if (x.is_molang) {
      x.expr = "-(" + x.expr + ")";
    } else {
      x.value = -x.value;
    }
    if (y.is_molang) {
      y.expr = "-(" + y.expr + ")";
    } else {
      y.value = -y.value;
    }
    (void)z;
  }

}

double evalAxis(const BbAxis &a, double t) {
  if (a.is_molang) {
    return evalSimpleMolang(a.expr, t);
  }
  return a.value;
}


loader::AnimationRoot convertBbAnimations(const nlohmann::json &root) {
  loader::AnimationRoot out;
  out.format_version = "1.8.0";
  if (!root.contains("animations") || !root.at("animations").is_array()) {
    return out;
  }
  nlohmann::json bedrock = nlohmann::json::object();
  bedrock["format_version"] = "1.8.0";
  bedrock["animations"] = nlohmann::json::object();

  for (const auto &anim : root.at("animations")) {
    if (!anim.is_object() || !anim.contains("name")) {
      continue;
    }
    const std::string name = anim.at("name").get<std::string>();
    nlohmann::json a = nlohmann::json::object();

    bool is_loop = false;
    if (anim.contains("loop")) {
      const auto &lp = anim.at("loop");
      if (lp.is_boolean()) {
        is_loop = lp.get<bool>();
        a["loop"] = is_loop;
      } else if (lp.is_string()) {
        const std::string s = lp.get<std::string>();
        if (s == "loop" || s == "true") {
          is_loop = true;
          a["loop"] = true;
        } else if (s == "hold" || s == "hold_on_last_frame") {
          a["loop"] = "hold_on_last_frame";
        } else {
          a["loop"] = false;
        }
      }
    }
    double length = 0.0;
    if (anim.contains("length") && anim.at("length").is_number()) {
      length = anim.at("length").get<double>();
    }
    a["bones"] = nlohmann::json::object();


    using AxisKf = std::map<double, std::array<BbAxis, 3>>;
    std::map<std::string, std::map<std::string, AxisKf>> bone_channels;
    double max_kf_time = 0.0;
    bool any_molang = false;

    if (anim.contains("animators") && anim.at("animators").is_object()) {
      for (auto it = anim.at("animators").begin();
           it != anim.at("animators").end(); ++it) {
        const auto &an = it.value();
        if (!an.is_object() || !an.contains("name") ||
            !an.contains("keyframes")) {
          continue;
        }
        if (an.contains("type") && an.at("type").is_string() &&
            an.at("type").get<std::string>() != "bone") {
          continue;
        }
        const std::string bone = an.at("name").get<std::string>();
        for (const auto &kf : an.at("keyframes")) {
          if (!kf.is_object() || !kf.contains("channel") ||
              !kf.contains("time") || !kf.contains("data_points") ||
              !kf.at("data_points").is_array() ||
              kf.at("data_points").empty()) {
            continue;
          }
          const std::string ch = kf.at("channel").get<std::string>();
          if (ch != "position" && ch != "rotation" && ch != "scale") {
            continue;
          }
          const double t =
              kf.at("time").is_number()
                  ? kf.at("time").get<double>()
                  : parseBbNumberStrict(kf.at("time")).value_or(0.0);
          max_kf_time = std::max(max_kf_time, t);
          const auto &dp = kf.at("data_points").at(0);
          std::array<BbAxis, 3> axes{};
          if (dp.is_object()) {
            axes[0] =
                parseBbAxis(dp.contains("x") ? dp.at("x") : nlohmann::json(0));
            axes[1] =
                parseBbAxis(dp.contains("y") ? dp.at("y") : nlohmann::json(0));
            axes[2] =
                parseBbAxis(dp.contains("z") ? dp.at("z") : nlohmann::json(0));
          }
          bbToBedrockChannelAxes(ch, axes[0], axes[1], axes[2]);
          for (const auto &ax : axes) {
            if (ax.is_molang) {
              any_molang = true;
            }
          }
          bone_channels[bone][ch][t] = axes;
        }
      }
    }



    if (!(length > 1e-9)) {
      length = std::max(1.0, max_kf_time > 1e-9 ? max_kf_time : 1.0);
      if (is_loop && any_molang) {
        length = std::max(length, 1.0);
      }
    }
    a["animation_length"] = length;

    for (const auto &[bone, channels] : bone_channels) {
      nlohmann::json bone_json = nlohmann::json::object();
      for (const auto &[ch, times] : channels) {
        bool channel_molang = false;
        for (const auto &[t, axes] : times) {
          (void)t;
          for (const auto &ax : axes) {
            if (ax.is_molang) {
              channel_molang = true;
            }
          }
        }
        auto emitVec = [](double x, double y, double z) {
          return nlohmann::json::array({x, y, z});
        };
        if (channel_molang) {


          const int samples = std::max(
              2, std::min(64, static_cast<int>(std::ceil(length * 20.0)) + 1));
          nlohmann::json timeline = nlohmann::json::object();
          for (int i = 0; i < samples; ++i) {
            const double t = length * (static_cast<double>(i) /
                                       static_cast<double>(samples - 1));

            auto it = times.upper_bound(t);
            if (it != times.begin()) {
              --it;
            }
            const auto &axes = it->second;
            const double x = evalAxis(axes[0], t);
            const double y = evalAxis(axes[1], t);
            const double z = evalAxis(axes[2], t);
            char key[32];
            std::snprintf(key, sizeof(key), "%.4f", t);
            timeline[key] = emitVec(x, y, z);
          }
          bone_json[ch] = timeline;
        } else if (times.size() == 1) {
          const auto &axes = times.begin()->second;
          bone_json[ch] = emitVec(axes[0].value, axes[1].value, axes[2].value);
        } else {
          nlohmann::json timeline = nlohmann::json::object();
          for (const auto &[t, axes] : times) {
            char key[32];
            std::snprintf(key, sizeof(key), "%.4f", t);
            timeline[key] =
                emitVec(axes[0].value, axes[1].value, axes[2].value);
          }
          bone_json[ch] = timeline;
        }
      }
      if (!bone_json.empty()) {
        a["bones"][bone] = bone_json;
      }
    }
    bedrock["animations"][name] = a;
  }
  try {
    out = loader::AnimationRoot::fromJson(bedrock);
  } catch (...) {
    out = {};
  }
  return out;
}

bool loadTextureFromBbEntry(const nlohmann::json &tex,
                            const std::filesystem::path &bb_path,
                            gfx::TextureImage &out, std::string *err) {

  if (tex.contains("source") && tex.at("source").is_string()) {
    const std::string src = tex.at("source").get<std::string>();
    const std::string prefix = "data:image/";
    if (src.rfind(prefix, 0) == 0) {
      const auto pos = src.find("base64,");
      if (pos != std::string::npos) {
        const auto bytes = decodeBase64(std::string_view(src).substr(pos + 7));
        if (!bytes.empty()) {
          return gfx::loadTextureImageFromMemory(
              bytes.data(), static_cast<int>(bytes.size()), out, err);
        }
      }
    }
  }

  if (tex.contains("relative_path") && tex.at("relative_path").is_string()) {
    const auto rel =
        bb_path.parent_path() / tex.at("relative_path").get<std::string>();
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(rel, ec);
    if (!ec && std::filesystem::exists(canon)) {
      return gfx::loadTextureImage(canon, out, err);
    }
    if (std::filesystem::exists(rel)) {
      return gfx::loadTextureImage(rel, out, err);
    }
  }
  if (tex.contains("path") && tex.at("path").is_string()) {
    const std::filesystem::path p = tex.at("path").get<std::string>();
    if (std::filesystem::exists(p)) {
      return gfx::loadTextureImage(p, out, err);
    }
  }
  if (err) {
    *err = "no usable texture source in bbmodel";
  }
  return false;
}

}

struct BakeExecutionState {
  explicit BakeExecutionState(BakeJobInput job);

  [[nodiscard]] const loader::Animation *outputReferenceAnimation() const;
  [[nodiscard]] std::string compatibilityStatusSuffix() const;

  BakeJobInput input;
  loader::Animation physics_source_animation;
  std::optional<loader::Animation> physics_target_animation;
  baker::RigidBodyInputCompatibilityReport compatibility;
  std::optional<baker::TransitionBakeRequest> transition_request;
  std::unique_ptr<baker::PhysicsBaker> baker;
  bool initialized = false;
};

struct BakeWorkerMailbox {
  std::atomic<int> current{0};
  std::atomic<int> total{0};
  std::atomic<WorkerPhase> phase{WorkerPhase::Preparing};
  std::atomic<bool> finished{false};
  std::mutex mutex;
  std::optional<BakeJobResult> result;
  std::unique_ptr<BakeExecutionState> completed_execution;
};

namespace {

class FingerprintBuilder {
public:
  void addString(std::string_view value) {
    addUnsigned(static_cast<std::uint64_t>(value.size()));
    for (const unsigned char byte : value) {
      hash_ ^= byte;
      hash_ *= 1099511628211ull;
    }
  }

  void addBool(bool value) { addUnsigned(value ? 1u : 0u); }

  template <typename T> void addInteger(T value) {
    addUnsigned(static_cast<std::uint64_t>(value));
  }

  void addDouble(double value) {
    addUnsigned(std::bit_cast<std::uint64_t>(value));
  }

  [[nodiscard]] SessionFingerprint finish() const { return {hash_}; }

private:
  void addUnsigned(std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      hash_ ^= static_cast<unsigned char>((value >> (byte * 8)) & 0xffu);
      hash_ *= 1099511628211ull;
    }
  }

  std::uint64_t hash_ = 1469598103934665603ull;
};

void hashKeyframes(FingerprintBuilder &hash,
                   const loader::Keyframes &channel) {
  hash.addBool(channel.looping);
  hash.addBool(channel.contains_molang);
  const auto hashValues = [&](const auto &values) {
    hash.addInteger(values.size());
    for (const auto &[time, value] : values) {
      hash.addDouble(time);
      for (const double component : value) {
        hash.addDouble(component);
      }
    }
  };
  hashValues(channel.keyframes);
  hashValues(channel.pre_keyframes);
  hash.addInteger(channel.interpolation_modes.size());
  for (const auto &[time, mode] : channel.interpolation_modes) {
    hash.addDouble(time);
    hash.addInteger(mode);
  }
  hash.addString(channel.original_authored_json.dump());
  hash.addString(channel.original_molang_json.dump());
}

void hashAnimation(FingerprintBuilder &hash,
                   const loader::Animation &animation) {
  hash.addBool(animation.loop);
  hash.addInteger(animation.loop_behavior);
  hash.addDouble(animation.animation_length);
  hash.addBool(animation.override_previous_animation.has_value());
  if (animation.override_previous_animation) {
    hash.addBool(*animation.override_previous_animation);
  }
  hash.addInteger(animation.bones.size());
  for (const auto &[name, channels] : animation.bones) {
    hash.addString(name);
    hash.addBool(channels.has_position);
    hash.addBool(channels.has_rotation);
    hash.addBool(channels.has_scale);
    if (channels.has_position) {
      hashKeyframes(hash, channels.position);
    }
    if (channels.has_rotation) {
      hashKeyframes(hash, channels.rotation);
    }
    if (channels.has_scale) {
      hashKeyframes(hash, channels.scale);
    }
  }
}

void hashConfig(FingerprintBuilder &hash,
                const baker::BoneMapper::PhysicsGroupConfig &config) {
  hash.addInteger(config.simulation_mode);
  hash.addDouble(config.particle_mass);
  hash.addDouble(config.compliance);
  hash.addDouble(config.damping_compliance);
  hash.addBool(config.enable_angle_constraints);
  hash.addDouble(config.max_bend_degrees);
  hash.addDouble(config.bend_compliance);
  hash.addDouble(config.gravity_y);
  hash.addBool(config.enable_real_gravity_field);
  hash.addBool(config.enable_ground_collision);
  hash.addInteger(config.solver_iterations);
  hash.addInteger(config.simd_mode);
  hash.addDouble(config.animation_pull_compliance);
  hash.addBool(config.allow_input_only_molang_zero_fallback);
  hash.addBool(config.allow_selected_molang_zero_fallback);
  hash.addDouble(config.collision_skin);
  hash.addDouble(config.xpbd_collision_restitution);
  hash.addDouble(config.wind_speed);
  hash.addDouble(config.wind_direction_degrees);
  hash.addDouble(config.wind_elevation_degrees);
  hash.addBool(config.use_wind_components);
  hash.addDouble(config.wind_x);
  hash.addDouble(config.wind_y);
  hash.addDouble(config.wind_z);
  hash.addDouble(config.movement_speed);
  hash.addDouble(config.movement_direction_degrees);
  hash.addDouble(config.movement_elevation_degrees);
  hash.addDouble(config.air_drag);
  hash.addDouble(config.turbulence);
  hash.addDouble(config.transition_duration);
  hash.addInteger(config.output_timeline_mode);
  hash.addInteger(config.loop_mode);
  hash.addInteger(config.minimum_warmup_cycles);
  hash.addInteger(config.maximum_warmup_cycles);
  hash.addInteger(config.required_stable_cycles);
  hash.addDouble(config.loop_position_tolerance);
  hash.addDouble(config.loop_rotation_tolerance_degrees);
  hash.addDouble(config.loop_linear_velocity_tolerance);
  hash.addDouble(config.loop_angular_velocity_tolerance);
  hash.addBool(config.loop_seam_fallback_enabled);
  hash.addInteger(config.loop_seam_strategy);
  hash.addDouble(config.loop_seam_window_ratio);
  hash.addBool(config.loop_seam_match_acceleration);
  hash.addDouble(config.loop_seam_relative_velocity_tolerance);
  hash.addDouble(config.loop_seam_minimum_linear_velocity_tolerance);
  hash.addDouble(config.loop_seam_minimum_angular_velocity_tolerance);
  hash.addDouble(config.loop_seam_relative_acceleration_tolerance);
  hash.addInteger(config.rigid_body_substeps);
  hash.addDouble(config.rigid_body_unit_scale);
  hash.addDouble(config.rigid_body_linear_damping);
  hash.addDouble(config.rigid_body_angular_damping);
  hash.addDouble(config.rigid_body_joint_stiffness);
  hash.addDouble(config.rigid_body_joint_damping);
  hash.addDouble(config.rigid_body_max_bend_x_degrees);
  hash.addDouble(config.rigid_body_max_bend_y_degrees);
  hash.addDouble(config.rigid_body_max_bend_z_degrees);
  hash.addDouble(config.rigid_body_friction);
  hash.addDouble(config.rigid_body_restitution);
  hash.addBool(config.rigid_body_ccd);
  hash.addDouble(config.rigid_body_maximum_safe_penetration);
  hash.addInteger(config.rigid_body_snapshot_level);
  hash.addBool(config.rigid_body_step_trace_enabled);
  hash.addInteger(config.rigid_body_step_trace_capacity);
}

void hashOptionalDouble(FingerprintBuilder &hash,
                        const std::optional<double> &value) {
  hash.addBool(value.has_value());
  if (value) {
    hash.addDouble(*value);
  }
}

SessionFingerprint fingerprintFor(const BakeJobInput &input) {
  FingerprintBuilder hash;
  hash.addString("xpbd-app-session-v2");
  hash.addString("stable-sine-v1");
  hash.addString("java-utf16-string-hashcode-v1");
  hash.addInteger(input.model_generation);
  hash.addInteger(input.animation_generation);
  hash.addInteger(input.timing.output_fps);
  hash.addString(input.source_animation_name);
  hashAnimation(hash, input.source_animation);
  hash.addBool(input.transition_target_animation.has_value());
  if (input.transition_target_animation) {
    hashAnimation(hash, *input.transition_target_animation);
  }
  hash.addString(input.transition_target_animation_name);
  hash.addInteger(input.transition_mode);
  hash.addDouble(input.transition_duration);
  hash.addDouble(input.transition_source_exit);
  hash.addDouble(input.transition_target_entry);
  for (const auto &[bone, weight] : input.transition_follow_weights) {
    hash.addString(bone);
    hash.addDouble(weight);
  }
  hash.addBool(input.allow_input_molang_zero);
  hash.addBool(input.allow_selected_molang_zero);

  const auto &mapper = input.mapper;
  hashConfig(hash, mapper.config());
  hash.addInteger(mapper.allBones().size());
  for (const auto &bone : mapper.allBones()) {
    hash.addString(bone.name);
    hash.addString(bone.parent);
    hash.addBool(bone.has_parent);
    for (int axis = 0; axis < 3; ++axis) {
      hash.addDouble(bone.pivot[axis]);
      hash.addDouble(bone.rotation[axis]);
    }
    hash.addInteger(bone.cubes.size());
    for (const auto &cube : bone.cubes) {
      for (int axis = 0; axis < 3; ++axis) {
        hash.addDouble(cube.origin[axis]);
        hash.addDouble(cube.size[axis]);
        hash.addDouble(cube.pivot[axis]);
        hash.addDouble(cube.rotation[axis]);
      }
      hash.addBool(cube.has_pivot);
      hash.addBool(cube.has_rotation);
      hash.addDouble(cube.inflate);
    }
    if (const auto *config = mapper.getBoneConfig(bone.name)) {
      hash.addBool(true);
      hashOptionalDouble(hash, config->particle_mass);
      hashOptionalDouble(hash, config->compliance);
      hashOptionalDouble(hash, config->damping_compliance);
      hashOptionalDouble(hash, config->max_bend_degrees);
      hashOptionalDouble(hash, config->bend_compliance);
      hashOptionalDouble(hash, config->rigid_body_max_bend_x_degrees);
      hashOptionalDouble(hash, config->rigid_body_max_bend_y_degrees);
      hashOptionalDouble(hash, config->rigid_body_max_bend_z_degrees);
      hashOptionalDouble(hash, config->animation_pull_compliance);
      hashOptionalDouble(hash, config->gravity_scale);
      hashOptionalDouble(hash, config->wind_influence);
      hashOptionalDouble(hash, config->turbulence_influence);
      hash.addBool(config->fixed.has_value());
      if (config->fixed) {
        hash.addBool(*config->fixed);
      }
    } else {
      hash.addBool(false);
    }
  }
  for (const auto &bone : mapper.physicsBones()) {
    hash.addString(bone);
  }
  for (const auto &bone : mapper.collisionRoots()) {
    hash.addString(bone);
  }
  return hash.finish();
}

SessionFingerprint timingFingerprintFor(const BakeJobInput &input) {
  FingerprintBuilder hash;
  hash.addString("xpbd-timing-v1");
  hash.addInteger(input.timing.output_fps);
  hash.addDouble(input.source_animation.animation_length);
  hash.addInteger(input.mapper.config().loop_mode);
  hash.addInteger(input.transition_mode);
  hash.addDouble(input.transition_duration);
  hash.addDouble(input.transition_source_exit);
  hash.addDouble(input.transition_target_entry);
  if (input.transition_target_animation) {
    hash.addDouble(input.transition_target_animation->animation_length);
    hash.addInteger(input.transition_target_animation->loop_behavior);
  }
  return hash.finish();
}

SessionFingerprint configFingerprintFor(const BakeJobInput &input) {
  FingerprintBuilder hash;
  hash.addString("xpbd-config-v1");
  hashConfig(hash, input.mapper.config());
  for (const auto &bone : input.mapper.physicsBones()) {
    hash.addString(bone);
    if (const auto *config = input.mapper.getBoneConfig(bone)) {
      hash.addBool(true);
      hashOptionalDouble(hash, config->particle_mass);
      hashOptionalDouble(hash, config->compliance);
      hashOptionalDouble(hash, config->damping_compliance);
      hashOptionalDouble(hash, config->max_bend_degrees);
      hashOptionalDouble(hash, config->bend_compliance);
      hashOptionalDouble(hash, config->rigid_body_max_bend_x_degrees);
      hashOptionalDouble(hash, config->rigid_body_max_bend_y_degrees);
      hashOptionalDouble(hash, config->rigid_body_max_bend_z_degrees);
      hashOptionalDouble(hash, config->animation_pull_compliance);
      hashOptionalDouble(hash, config->gravity_scale);
      hashOptionalDouble(hash, config->wind_influence);
      hashOptionalDouble(hash, config->turbulence_influence);
      hash.addBool(config->fixed.has_value());
      if (config->fixed) {
        hash.addBool(*config->fixed);
      }
    } else {
      hash.addBool(false);
    }
  }
  for (const auto &bone : input.mapper.collisionRoots()) {
    hash.addString(bone);
  }
  for (const auto &[bone, weight] : input.transition_follow_weights) {
    hash.addString(bone);
    hash.addDouble(weight);
  }
  hash.addBool(input.allow_input_molang_zero);
  hash.addBool(input.allow_selected_molang_zero);
  return hash.finish();
}

SessionFingerprint contentFingerprintFor(const BakeJobInput &input) {
  FingerprintBuilder hash;
  hash.addString("xpbd-content-v1");
  hash.addInteger(input.model_generation);
  hash.addInteger(input.animation_generation);
  hash.addString(input.source_animation_name);
  hashAnimation(hash, input.source_animation);
  hash.addString(input.transition_target_animation_name);
  hash.addBool(input.transition_target_animation.has_value());
  if (input.transition_target_animation) {
    hashAnimation(hash, *input.transition_target_animation);
  }
  for (const auto &bone : input.mapper.allBones()) {
    hash.addString(bone.name);
    hash.addString(bone.parent);
    hash.addBool(bone.has_parent);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      hash.addDouble(bone.pivot[axis]);
      hash.addDouble(bone.rotation[axis]);
    }
    for (const auto &cube : bone.cubes) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        hash.addDouble(cube.origin[axis]);
        hash.addDouble(cube.size[axis]);
        hash.addDouble(cube.pivot[axis]);
        hash.addDouble(cube.rotation[axis]);
      }
      hash.addBool(cube.has_pivot);
      hash.addBool(cube.has_rotation);
      hash.addDouble(cube.inflate);
    }
  }
  return hash.finish();
}

std::string valueText(double value) {
  std::ostringstream stream;
  stream << std::setprecision(12) << value;
  return stream.str();
}

std::string valueText(bool value) { return value ? "true" : "false"; }

EffectiveConfigSnapshot effectiveConfigFor(const BakeJobInput &input) {
  EffectiveConfigSnapshot snapshot;
  const auto &config = input.mapper.config();
  const bool bullet = config.simulation_mode ==
                      baker::BoneMapper::SimulationMode::RigidBody;
  const auto snapshot_level = rigidbody::resolveSnapshotLevel(
      config.rigid_body_snapshot_level,
      config.rigid_body_step_trace_enabled);
  const auto add = [&](std::string name, const std::string &committed,
                       const std::string &effective, std::string source,
                       std::string reason = {}) {
    snapshot.global.push_back({std::move(name), committed, committed, effective,
                               std::move(source), std::move(reason)});
  };
  const auto addModeValue = [&](std::string name, const std::string &value,
                                bool active, const char *active_mode) {
    add(std::move(name), value, active ? value : "N/A",
        active ? "Global" : "Unsupported",
        active ? "" : std::string("Used only in ") + active_mode + " mode");
  };
  add("Simulation Mode", bullet ? "Bullet" : "XPBD",
      bullet ? "Bullet" : "XPBD", "Global");
  add("Output FPS", std::to_string(input.timing.output_fps),
      std::to_string(input.timing.output_fps), "Global");
  add("Output Timeline",
      config.output_timeline_mode ==
              baker::BoneMapper::OutputTimelineMode::SourceKeyframeGrid
          ? "Source Keyframe Grid"
          : "Bake FPS",
      config.output_timeline_mode ==
              baker::BoneMapper::OutputTimelineMode::SourceKeyframeGrid
          ? "Source Keyframe Grid"
          : "Bake FPS",
      "Global");
  add("Nominal Output dt", valueText(input.timing.nominalOutputDt()),
      valueText(input.timing.nominalOutputDt()), "Derived",
      "Exactly 1 / Output FPS before endpoint fitting");
  add("Physics Bone Count", std::to_string(input.mapper.physicsBones().size()),
      std::to_string(input.mapper.physicsBones().size()), "Selection");
  add("Collision Root Count",
      std::to_string(input.mapper.collisionRoots().size()),
      std::to_string(input.mapper.collisionRoots().size()), "Selection");
  add("Loop Mode",
      config.loop_mode == baker::BoneMapper::LoopMode::ForceLoop
          ? "Force Loop"
          : config.loop_mode == baker::BoneMapper::LoopMode::ForceOnce
                ? "Force Once"
                : "Auto",
      config.loop_mode == baker::BoneMapper::LoopMode::ForceLoop
          ? "Force Loop"
          : config.loop_mode == baker::BoneMapper::LoopMode::ForceOnce
                ? "Force Once"
                : "Auto",
      "Global");
  add("Transition Mode", std::to_string(input.transition_mode),
      input.transition_mode == 0
          ? "Off"
          : input.transition_mode == 1 ? "Simple" : "Custom",
      "Global");
  add("Transition Duration", valueText(input.transition_duration),
      valueText(input.transition_duration), "Global");
  add("Transition Source Exit", valueText(input.transition_source_exit),
      valueText(input.transition_source_exit), "Global");
  add("Transition Target Entry", valueText(input.transition_target_entry),
      valueText(input.transition_target_entry), "Global");
  add("Transition Reference",
      input.transition_target_animation_name.empty()
          ? input.source_animation_name
          : input.transition_target_animation_name,
      input.transition_target_animation_name.empty()
          ? input.source_animation_name
          : input.transition_target_animation_name,
      "Global");
  add("Fixed Substeps", std::to_string(config.rigid_body_substeps),
      bullet ? std::to_string(config.rigid_body_substeps) : "N/A",
      bullet ? "Global" : "Unsupported",
      bullet ? "Exact Bullet steps per output interval"
             : "Used only in Bullet mode");
  add("Snapshot Level",
      rigidbody::snapshotLevelName(config.rigid_body_snapshot_level),
      bullet ? rigidbody::snapshotLevelName(snapshot_level) : "N/A",
      !bullet ? "Unsupported"
              : config.rigid_body_step_trace_enabled ? "Legacy Alias"
                                                      : "Global",
      !bullet ? "Used only in Bullet mode"
              : config.rigid_body_step_trace_enabled
                    ? "Legacy Step Trace enables FullDiagnostics"
                    : "Controls diagnostic materialization only");
  add("Step Trace",
      config.rigid_body_step_trace_enabled ? "Enabled" : "Disabled",
      bullet ? (snapshot_level == rigidbody::SnapshotLevel::FullDiagnostics
                    ? "Enabled"
                    : "Disabled")
             : "N/A",
      bullet ? "Diagnostics" : "Unsupported",
      bullet ? "Bounded observational capture; does not change stepping"
             : "Used only in Bullet mode");
  add("Step Trace Capacity",
      std::to_string(config.rigid_body_step_trace_capacity),
      bullet && snapshot_level == rigidbody::SnapshotLevel::FullDiagnostics
          ? std::to_string(config.rigid_body_step_trace_capacity)
          : "N/A",
      bullet && snapshot_level == rigidbody::SnapshotLevel::FullDiagnostics
          ? "Diagnostics"
          : "Mode Forced",
      snapshot_level == rigidbody::SnapshotLevel::FullDiagnostics
          ? "Latest-N substep samples retained"
          : "Trace disabled");
  addModeValue("Unit Scale", valueText(config.rigid_body_unit_scale), bullet,
               "Bullet");
  addModeValue("Linear Body Damping",
               valueText(config.rigid_body_linear_damping), bullet, "Bullet");
  addModeValue("Angular Body Damping",
               valueText(config.rigid_body_angular_damping), bullet, "Bullet");
  add("Solver Iterations", std::to_string(config.solver_iterations),
      std::to_string(config.solver_iterations), "Global");
  add("Requested Joint Stiffness",
      valueText(config.rigid_body_joint_stiffness),
      bullet && config.enable_angle_constraints
          ? valueText(config.rigid_body_joint_stiffness)
          : "N/A",
      bullet && config.enable_angle_constraints ? "Requested" : "Mode Forced",
      !bullet ? "Used only in Bullet mode"
              : config.enable_angle_constraints
                    ? "Passed to Bullet; effective solver behavior may be "
                      "stability-limited"
                    : "Angle constraints disabled");
  const bool angular_spring =
      bullet && config.enable_angle_constraints &&
      config.rigid_body_joint_stiffness > 0.0 &&
      (config.rigid_body_max_bend_x_degrees < 180.0 ||
       config.rigid_body_max_bend_y_degrees < 180.0 ||
       config.rigid_body_max_bend_z_degrees < 180.0);
  add("Requested Joint Damping", valueText(config.rigid_body_joint_damping),
      angular_spring ? valueText(config.rigid_body_joint_damping) : "N/A",
      angular_spring ? "Requested" : "Mode Forced",
      angular_spring
          ? "Passed to Bullet; effective solver behavior may be "
            "stability-limited"
          : "No limited angular spring is active");
  add("Effective Joint Spring Behavior", "Solver-dependent",
      bullet ? "Resolved after joint construction" : "N/A",
      bullet ? "Bullet Runtime" : "Unsupported",
      bullet ? "Not a calibrated physical stiffness/damping value"
             : "Used only in Bullet mode");
  addModeValue("Maximum Bend X",
               valueText(config.rigid_body_max_bend_x_degrees), bullet,
               "Bullet");
  addModeValue("Maximum Bend Y",
               valueText(config.rigid_body_max_bend_y_degrees), bullet,
               "Bullet");
  addModeValue("Maximum Bend Z",
               valueText(config.rigid_body_max_bend_z_degrees), bullet,
               "Bullet");
  add("Mass", valueText(config.particle_mass), valueText(config.particle_mass),
      "Global");
  addModeValue("XPBD Compliance", valueText(config.compliance), !bullet,
               "XPBD");
  addModeValue("XPBD Damping Compliance",
               valueText(config.damping_compliance), !bullet, "XPBD");
  addModeValue("XPBD Maximum Bend", valueText(config.max_bend_degrees),
               !bullet && config.enable_angle_constraints, "XPBD");
  addModeValue("XPBD Bend Compliance", valueText(config.bend_compliance),
               !bullet && config.enable_angle_constraints, "XPBD");
  addModeValue("XPBD Collision Skin", valueText(config.collision_skin),
               !bullet, "XPBD");
  addModeValue("XPBD Collision Restitution",
               valueText(config.xpbd_collision_restitution), !bullet,
               "XPBD");
  add("Gravity", valueText(config.gravity_y), valueText(config.gravity_y),
      "Global");
  add("Air Drag", valueText(config.air_drag), valueText(config.air_drag),
      "Global");
  add("Turbulence", valueText(config.turbulence), valueText(config.turbulence),
      "Global");
  add("Wind Representation", config.use_wind_components ? "XYZ" : "Polar",
      config.use_wind_components ? "XYZ" : "Polar", "Global");
  add("Wind X", valueText(config.wind_x), valueText(config.wind_x), "Global");
  add("Wind Y", valueText(config.wind_y), valueText(config.wind_y), "Global");
  add("Wind Z", valueText(config.wind_z), valueText(config.wind_z), "Global");
  add("Wind Speed", valueText(config.wind_speed), valueText(config.wind_speed),
      "Global");
  add("Wind Direction", valueText(config.wind_direction_degrees),
      valueText(config.wind_direction_degrees), "Global");
  add("Wind Elevation", valueText(config.wind_elevation_degrees),
      valueText(config.wind_elevation_degrees), "Global");
  add("Movement Speed", valueText(config.movement_speed),
      valueText(config.movement_speed), "Global");
  add("Movement Direction", valueText(config.movement_direction_degrees),
      valueText(config.movement_direction_degrees), "Global");
  add("Movement Elevation", valueText(config.movement_elevation_degrees),
      valueText(config.movement_elevation_degrees), "Global");
  add("Animation Follow", valueText(config.animation_pull_compliance),
      config.enable_real_gravity_field
          ? "0"
          : valueText(config.animation_pull_compliance),
      config.enable_real_gravity_field ? "Mode Forced" : "Global",
      config.enable_real_gravity_field ? "Real Gravity enabled" : "");
  addModeValue("Friction", valueText(config.rigid_body_friction), bullet,
               "Bullet");
  addModeValue("Restitution", valueText(config.rigid_body_restitution), bullet,
               "Bullet");
  addModeValue("CCD", valueText(config.rigid_body_ccd), bullet, "Bullet");
  add("Ground Collision", valueText(config.enable_ground_collision),
      valueText(config.enable_ground_collision), "Global");
  add("Real Gravity", valueText(config.enable_real_gravity_field),
      valueText(config.enable_real_gravity_field), "Global");
  add("Maximum Safe Penetration (Loop / Seam)",
      valueText(config.rigid_body_maximum_safe_penetration),
      bullet ? valueText(config.rigid_body_maximum_safe_penetration) : "N/A",
      bullet ? "Global" : "Unsupported",
       bullet ? "Loop-cycle/seam/export gate; does not decide source looping or change contact response"
              : "Used only in Bullet mode");
  add("Seam Strategy",
      config.loop_seam_strategy ==
              baker::BoneMapper::LoopSeamStrategy::PhysicsRelative
          ? "Physics Relative"
          : "Visual Subtree",
      config.loop_seam_strategy ==
              baker::BoneMapper::LoopSeamStrategy::PhysicsRelative
          ? "Physics Relative"
          : "Visual Subtree",
      "Global");
  add("Loop Seam Window (ratio)",
      valueText(config.loop_seam_window_ratio),
      valueText(config.loop_seam_window_ratio), "Global");
  add("Warmup Cycle Range",
      std::to_string(config.minimum_warmup_cycles) + ".." +
          std::to_string(config.maximum_warmup_cycles),
      std::to_string(config.minimum_warmup_cycles) + ".." +
          std::to_string(config.maximum_warmup_cycles),
      "Global");
  add("Molang Input Zero Approval",
      valueText(input.allow_input_molang_zero),
      valueText(input.allow_input_molang_zero), "One-Shot Approval");
  add("Molang Selected Zero Approval",
      valueText(input.allow_selected_molang_zero),
      valueText(input.allow_selected_molang_zero), "One-Shot Approval");

  for (const auto &bone : input.mapper.physicsBones()) {
    const auto *override_config = input.mapper.getBoneConfig(bone);
    const auto sourceFor = [&](bool overridden) {
      return overridden ? std::string("Per-Bone Override")
                        : std::string("Global");
    };
    auto &values = snapshot.per_bone[bone];
    const auto addBone = [&](std::string name, const std::string &committed,
                             const std::string &effective, bool overridden,
                             std::string reason = {}) {
      values.push_back({std::move(name), committed, committed, effective,
                        sourceFor(overridden), std::move(reason)});
    };
    addBone("Mass",
            valueText(override_config && override_config->particle_mass
                          ? *override_config->particle_mass
                          : config.particle_mass),
            valueText(input.mapper.getEffectiveMass(bone)),
            override_config && override_config->particle_mass.has_value());
    addBone("Compliance",
            valueText(override_config && override_config->compliance
                          ? *override_config->compliance
                          : config.compliance),
            bullet ? "N/A"
                   : valueText(input.mapper.getEffectiveCompliance(bone)),
            override_config && override_config->compliance.has_value(),
            bullet ? "Used only in XPBD mode" : "");
    addBone("Damping Compliance",
            valueText(override_config && override_config->damping_compliance
                          ? *override_config->damping_compliance
                          : config.damping_compliance),
            bullet ? "N/A"
                   : valueText(
                         input.mapper.getEffectiveDampingCompliance(bone)),
            override_config &&
                override_config->damping_compliance.has_value(),
            bullet ? "Used only in XPBD mode" : "");
    const bool fixedOverride =
        override_config && override_config->fixed.has_value();
    values.push_back(
        {"Fixed", fixedOverride ? valueText(*override_config->fixed) : "Auto",
         fixedOverride ? valueText(*override_config->fixed) : "Auto",
         valueText(input.mapper.isFixedBone(bone)),
         fixedOverride ? "Per-Bone Override"
                       : config.enable_real_gravity_field ? "Mode Forced"
                                                          : "Topology Default",
         !fixedOverride && config.enable_real_gravity_field
             ? "Real Gravity disables automatic root fixing"
             : ""});
    values.push_back(
        {"Animation Follow",
         valueText(override_config && override_config->animation_pull_compliance
                       ? *override_config->animation_pull_compliance
                       : config.animation_pull_compliance),
         valueText(override_config && override_config->animation_pull_compliance
                       ? *override_config->animation_pull_compliance
                       : config.animation_pull_compliance),
         valueText(input.mapper.getEffectiveAnimPullCompliance(bone)),
         config.enable_real_gravity_field
             ? "Mode Forced"
             : sourceFor(override_config &&
                         override_config->animation_pull_compliance.has_value()),
         config.enable_real_gravity_field ? "Real Gravity enabled" : ""});
    addBone("Gravity Scale",
            valueText(override_config && override_config->gravity_scale
                          ? *override_config->gravity_scale
                          : 1.0),
            valueText(input.mapper.getEffectiveGravityScale(bone)),
            override_config && override_config->gravity_scale.has_value());
    addBone("Wind Influence",
            valueText(override_config && override_config->wind_influence
                          ? *override_config->wind_influence
                          : 1.0),
            valueText(input.mapper.getEffectiveWindInfluence(bone)),
            override_config && override_config->wind_influence.has_value());
    addBone("Turbulence Influence",
            valueText(override_config && override_config->turbulence_influence
                          ? *override_config->turbulence_influence
                          : 1.0),
            valueText(input.mapper.getEffectiveTurbulenceInfluence(bone)),
            override_config &&
                override_config->turbulence_influence.has_value());
    const auto bend =
        input.mapper.getEffectiveRigidBodyMaxBendDegrees(bone);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const bool overridden =
          override_config &&
          (axis == 0
               ? override_config->rigid_body_max_bend_x_degrees.has_value()
               : axis == 1
                     ? override_config->rigid_body_max_bend_y_degrees
                           .has_value()
                     : override_config->rigid_body_max_bend_z_degrees
                           .has_value());
      const double committed =
          overridden
              ? axis == 0   ? *override_config->rigid_body_max_bend_x_degrees
                : axis == 1 ? *override_config->rigid_body_max_bend_y_degrees
                            : *override_config->rigid_body_max_bend_z_degrees
              : axis == 0   ? config.rigid_body_max_bend_x_degrees
                : axis == 1 ? config.rigid_body_max_bend_y_degrees
                            : config.rigid_body_max_bend_z_degrees;
      addBone(std::string("Maximum Bend ") + "XYZ"[axis],
              valueText(committed), bullet ? valueText(bend[axis]) : "N/A",
              overridden, bullet ? "" : "Used only in Bullet mode");
    }
    const auto follow = input.transition_follow_weights.find(bone);
    addBone("Transition Follow",
            valueText(follow != input.transition_follow_weights.end()
                          ? follow->second
                          : 1.0),
            valueText(follow != input.transition_follow_weights.end()
                          ? follow->second
                          : 1.0),
            follow != input.transition_follow_weights.end());
  }
  return snapshot;
}

bool framesAreFinite(const std::vector<baker::BakedFrame> &frames) {
  for (const auto &frame : frames) {
    if (!std::isfinite(frame.time)) {
      return false;
    }
    for (const auto &state : frame.bone_states) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(state.position[axis]) ||
            !std::isfinite(state.rotation[axis]) ||
            !std::isfinite(state.linear_velocity[axis]) ||
            (state.has_world_position &&
             !std::isfinite(state.world_position[axis]))) {
          return false;
        }
      }
    }
  }
  return true;
}

std::string loopPolicy(const baker::PhysicsBaker &baker,
                       baker::BoneMapper::LoopMode mode) {
  if (mode == baker::BoneMapper::LoopMode::ForceLoop) {
    return "Forced Loop";
  }
  if (mode == baker::BoneMapper::LoopMode::ForceOnce) {
    return "Forced Once";
  }
  switch (baker.getOutputLoopBehavior()) {
  case loader::Animation::LoopBehavior::Loop:
    return "Loop";
  case loader::Animation::LoopBehavior::HoldLast:
    return "Hold Last";
  case loader::Animation::LoopBehavior::Once:
    return "Once";
  }
  return "Once";
}

std::string loopValidationState(baker::LoopValidationState state) {
  switch (state) {
  case baker::LoopValidationState::Valid:
    return "Valid";
  case baker::LoopValidationState::MissingBone:
    return "Missing Bone";
  case baker::LoopValidationState::MissingMetric:
    return "Missing Metric";
  case baker::LoopValidationState::NonFiniteValue:
    return "Non-Finite Value";
  case baker::LoopValidationState::InvalidQuaternion:
    return "Invalid Quaternion";
  case baker::LoopValidationState::InvalidContact:
    return "Invalid Contact";
  case baker::LoopValidationState::InvalidSampleTime:
    return "Invalid Sample Time";
  case baker::LoopValidationState::ProducerAnomaly:
    return "Producer Anomaly";
  }
  return "Invalid";
}

std::string joinLoopRejectionReasons(const std::vector<std::string> &reasons) {
  std::ostringstream text;
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    if (index != 0) {
      text << ';';
    }
    text << reasons[index];
  }
  return text.str();
}

LoopBoundaryDiagnostics
loopBoundaryDiagnostics(const baker::LoopBoundaryState &boundary) {
  LoopBoundaryDiagnostics result;
  result.sample_time = boundary.sample_time;
  result.has_sample_time = boundary.has_sample_time;
  result.maximum_penetration = boundary.maximum_penetration;
  result.has_maximum_penetration = boundary.has_maximum_penetration;
  result.maximum_penetration_time = boundary.maximum_penetration_time;
  result.has_maximum_penetration_time =
      boundary.has_maximum_penetration_time;
  result.bodies.reserve(boundary.bodies.size());
  for (const auto &body : boundary.bodies) {
    result.bodies.push_back(
        {body.bone_name,
         body.pivot_position,
         body.has_pivot_position,
         body.pivot_rotation_xyzw,
         body.has_pivot_rotation,
         body.pivot_linear_velocity,
         body.has_pivot_linear_velocity,
         body.com_position,
         body.has_com_position,
         body.com_rotation_xyzw,
         body.has_com_rotation,
         body.com_linear_velocity,
         body.has_com_linear_velocity,
         body.angular_velocity,
         body.has_angular_velocity});
  }
  result.contacts.reserve(boundary.contacts.size());
  for (const auto &contact : boundary.contacts) {
    result.contacts.push_back(
        {contact.pair, contact.meaningful_penetration, contact.penetration,
         contact.penetration_bucket});
  }
  return result;
}

nlohmann::json loopBoundaryJson(const LoopBoundaryDiagnostics &boundary) {
  nlohmann::json result = {
      {"sample_time", boundary.has_sample_time
                          ? nlohmann::json(boundary.sample_time)
                          : nlohmann::json(nullptr)},
      {"maximum_penetration",
       boundary.has_maximum_penetration
           ? nlohmann::json(boundary.maximum_penetration)
           : nlohmann::json(nullptr)},
      {"maximum_penetration_time",
       boundary.has_maximum_penetration_time &&
               boundary.maximum_penetration_time >= 0.0
           ? nlohmann::json(boundary.maximum_penetration_time)
           : nlohmann::json(nullptr)},
      {"bodies", nlohmann::json::array()},
      {"contacts", nlohmann::json::array()}};
  for (const auto &body : boundary.bodies) {
    nlohmann::json pivot = {
        {"position", body.has_pivot_position
                         ? nlohmann::json(body.pivot_position)
                         : nlohmann::json(nullptr)},
        {"rotation_xyzw", body.has_pivot_rotation
                              ? nlohmann::json(body.pivot_rotation_xyzw)
                              : nlohmann::json(nullptr)},
        {"linear_velocity", body.has_pivot_linear_velocity
                                ? nlohmann::json(body.pivot_linear_velocity)
                                : nlohmann::json(nullptr)},
        {"angular_velocity", body.has_angular_velocity
                                 ? nlohmann::json(body.angular_velocity)
                                 : nlohmann::json(nullptr)}};
    nlohmann::json com = nullptr;
    if (body.has_com_position || body.has_com_rotation ||
        body.has_com_linear_velocity) {
      com = {{"position", body.has_com_position
                             ? nlohmann::json(body.com_position)
                             : nlohmann::json(nullptr)},
             {"rotation_xyzw", body.has_com_rotation
                                   ? nlohmann::json(body.com_rotation_xyzw)
                                   : nlohmann::json(nullptr)},
             {"linear_velocity", body.has_com_linear_velocity
                                     ? nlohmann::json(body.com_linear_velocity)
                                     : nlohmann::json(nullptr)}};
    }
    result["bodies"].push_back(
        {{"bone", body.bone_name}, {"pivot", std::move(pivot)},
         {"com", std::move(com)}});
  }
  for (const auto &contact : boundary.contacts) {
    result["contacts"].push_back(
        {{"pair", contact.pair},
         {"state", contact.meaningful_penetration ? "meaningful_penetration"
                                                    : "touching"},
         {"penetration", contact.penetration},
         {"penetration_bucket", contact.penetration_bucket}});
  }
  return result;
}

ExportPreflight preflightFor(const BakeExecutionState &execution) {
  ExportPreflight preflight;
  const auto &baker = *execution.baker;
  const bool bullet_safety_applicable =
      execution.input.mapper.config().simulation_mode ==
      baker::BoneMapper::SimulationMode::RigidBody;
  preflight.finalized = baker.isFramesFinalized() && !baker.frames().empty();
  const auto block = [&](ExportBlockCode code, std::string detail) {
    preflight.block_reasons.push_back({code, std::move(detail)});
  };
  if (!preflight.finalized) {
    block(ExportBlockCode::NotFinalized, "Bake has not been finalized");
  }
  if (baker.isLoopSeamCorrectionRejected()) {
    bool specific_gate_recorded = false;
    if (const auto *report = baker.getLoopSeamReport()) {
      const auto gate = report->exportGate(execution.input.mapper.config());
      const auto block_gate = [&](bool passed, ExportBlockCode code,
                                  const char *detail) {
        if (!passed) {
          block(code, detail);
          specific_gate_recorded = true;
        }
      };
      block_gate(gate.validation_pass, ExportBlockCode::LoopValidationFailed,
                 "Loop candidate validation gate is unsafe");
      block_gate(gate.physics_seam_pass, ExportBlockCode::PhysicsSeamUnsafe,
                 "Physics seam gate is unsafe");
      block_gate(gate.driver_seam_pass, ExportBlockCode::DriverSeamUnsafe,
                 "Driver seam gate is unsafe");
      block_gate(gate.quantization_pass, ExportBlockCode::QuantizationUnsafe,
                 "Loop quantization gate is unsafe");
      block_gate(gate.collision_pass, ExportBlockCode::LoopCollisionUnsafe,
                 "Loop collision gate is unsafe");
      block_gate(gate.joint_pass, ExportBlockCode::LoopJointUnsafe,
                 "Loop joint gate is unsafe");
    }
    if (!specific_gate_recorded) {
      block(ExportBlockCode::SeamRejected,
            "Loop seam correction was rejected");
    }
  }
  if (bullet_safety_applicable &&
      baker.getUnsafeFinalCollisionCount() > 0) {
    block(ExportBlockCode::CollisionUnsafe,
          "Final Bullet collision audit is unsafe");
  }
  if (bullet_safety_applicable && baker.getUnsafeFinalJointCount() > 0) {
    block(ExportBlockCode::JointUnsafe,
          "Final Bullet joint audit is unsafe");
  }
  if (!framesAreFinite(baker.frames())) {
    block(ExportBlockCode::NumericalFailure,
          "Final frames contain NaN or Infinity");
  }
  if (baker.isTransitionBake()) {
    try {
      baker.requireTransitionReferenceExportable();
    } catch (const std::exception &error) {
      block(ExportBlockCode::TransitionReference, error.what());
    }
  }
  preflight.animation_allowed = preflight.block_reasons.empty();
  preflight.velocity_allowed = preflight.block_reasons.empty();
  return preflight;
}

bool isForceExportableBlock(ExportBlockCode code) {
  switch (code) {
  case ExportBlockCode::SeamRejected:
  case ExportBlockCode::CollisionUnsafe:
  case ExportBlockCode::JointUnsafe:
  case ExportBlockCode::LoopValidationFailed:
  case ExportBlockCode::PhysicsSeamUnsafe:
  case ExportBlockCode::DriverSeamUnsafe:
  case ExportBlockCode::QuantizationUnsafe:
  case ExportBlockCode::LoopCollisionUnsafe:
  case ExportBlockCode::LoopJointUnsafe:
    return true;
  default:
    return false;
  }
}

BakeDiagnostics diagnosticsFor(const BakeExecutionState &execution,
                               WorkerPhase terminal_phase,
                               int resumed_from_step = 0) {
  const auto &baker = *execution.baker;
  BakeDiagnostics diagnostics;
  diagnostics.bullet_safety_applicable =
      execution.input.mapper.config().simulation_mode ==
      baker::BoneMapper::SimulationMode::RigidBody;
  diagnostics.fingerprint = execution.input.fingerprint;
  diagnostics.timing.output_fps = execution.input.timing.output_fps;
  diagnostics.timing.nominal_output_dt =
      execution.input.timing.nominalOutputDt();
  diagnostics.timing.effective_output_dt = baker.outputFrameInterval();
  diagnostics.timing.resumed_from_step = resumed_from_step;
  diagnostics.timing.total_steps = baker.totalSteps();
  diagnostics.timing.completed_steps = baker.currentStep();
  if (diagnostics.bullet_safety_applicable) {
    diagnostics.initial_collision = baker.getInitialCollisionSnapshot();
    diagnostics.runtime_collision.current_contact_count =
        baker.getCurrentRigidBodyContactCount();
    diagnostics.runtime_collision.maximum_contact_count =
        baker.getMaximumRigidBodyContactCount();
    diagnostics.runtime_collision.maximum_penetration =
        baker.getMaximumRuntimeRigidBodyPenetration();
  }
  std::map<std::string, std::array<double, 3>> previousVelocities;
  for (std::size_t frameIndex = 0; frameIndex < baker.frames().size();
       ++frameIndex) {
    for (const auto &state : baker.frames()[frameIndex].bone_states) {
      const double speed = std::sqrt(
          state.linear_velocity[0] * state.linear_velocity[0] +
          state.linear_velocity[1] * state.linear_velocity[1] +
          state.linear_velocity[2] * state.linear_velocity[2]);
      diagnostics.velocity.available = true;
      if (speed > diagnostics.velocity.maximum_linear_speed) {
        diagnostics.velocity.maximum_linear_speed = speed;
        diagnostics.velocity.maximum_speed_bone = state.bone_name;
        diagnostics.velocity.maximum_speed_frame =
            static_cast<int>(frameIndex);
      }
      const auto previous = previousVelocities.find(state.bone_name);
      if (previous != previousVelocities.end()) {
        const double dx = state.linear_velocity[0] - previous->second[0];
        const double dy = state.linear_velocity[1] - previous->second[1];
        const double dz = state.linear_velocity[2] - previous->second[2];
        const double jump = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (jump > diagnostics.velocity.maximum_frame_velocity_jump) {
          diagnostics.velocity.maximum_frame_velocity_jump = jump;
          diagnostics.velocity.maximum_jump_bone = state.bone_name;
          diagnostics.velocity.maximum_jump_frame =
              static_cast<int>(frameIndex);
        }
      }
      previousVelocities[state.bone_name] = state.linear_velocity;
    }
  }
  diagnostics.substeps = baker.getFixedSubstepStats();
  diagnostics.kinematic_history = baker.getKinematicHistoryStats();
  diagnostics.joint_preflight = baker.getJointPreflightDiagnostics();
  diagnostics.collider_preflight = baker.getColliderPreflightDiagnostics();
  diagnostics.joint_spring = baker.getJointSpringDiagnostics();
  diagnostics.runtime_fingerprint = baker.getRigidBodyRuntimeFingerprint();
  diagnostics.step_trace = baker.getRigidBodyStepTrace();
  if (diagnostics.bullet_safety_applicable) {
    diagnostics.joints.unsafe_final_count = baker.getUnsafeFinalJointCount();
    diagnostics.joints.maximum_anchor_separation =
        baker.getMaximumFinalJointAnchorSeparation();
    diagnostics.joints.maximum_angular_excess_radians =
        baker.getMaximumFinalJointAngularExcessRadians();
    diagnostics.joints.worst_linear_parent =
        baker.getWorstFinalJointLinearParent();
    diagnostics.joints.worst_linear_child =
        baker.getWorstFinalJointLinearChild();
    diagnostics.joints.worst_angular_parent =
        baker.getWorstFinalJointAngularParent();
    diagnostics.joints.worst_angular_child =
        baker.getWorstFinalJointAngularChild();
    diagnostics.joints.worst_angular_axis =
        baker.getWorstFinalJointAngularAxis();
    if (const auto &singularity = baker.getFirstFinalJointEulerSingularity()) {
      diagnostics.joints.euler_singularity = JointEulerSingularityStats{
          singularity->parent_bone, singularity->child_bone,
          singularity->relative_rotation_xyzw, singularity->rotation_order};
    }
  }
  const auto &loopConfig = execution.input.mapper.config();
  double loopBoundaryTime =
      baker.frames().empty() ? -1.0 : baker.frames().back().time;
  if (baker.isLooping() &&
      std::isfinite(execution.input.source_animation.animation_length) &&
      execution.input.source_animation.animation_length > 0.0) {


    loopBoundaryTime = execution.input.source_animation.animation_length;
  }
  diagnostics.loop.source_policy = loopPolicy(baker, loopConfig.loop_mode);
  diagnostics.loop.converged = baker.isLoopConverged();
  diagnostics.loop.fallback_used = baker.isLoopFallbackUsed();
  diagnostics.loop.seam_correction_rejected =
      baker.isLoopSeamCorrectionRejected();
  diagnostics.loop.completed_cycles = baker.getCompletedLoopCycles();
  diagnostics.loop.selected_cycle = baker.getBestLoopCycleIndex();
  diagnostics.loop.best_cycle_score = baker.getBestLoopCycleScore();
  diagnostics.loop.seam_strategy =
      execution.input.mapper.config().loop_seam_strategy ==
              baker::BoneMapper::LoopSeamStrategy::PhysicsRelative
          ? "Physics Relative"
          : "Visual Subtree";
  diagnostics.loop.configured_seam_window_ratio =
      execution.input.mapper.config().loop_seam_window_ratio;
  diagnostics.loop.configured_seam_window_seconds =
      std::isfinite(execution.input.source_animation.animation_length) &&
              execution.input.source_animation.animation_length > 0.0
          ? diagnostics.loop.configured_seam_window_ratio *
                execution.input.source_animation.animation_length
          : 0.0;
  diagnostics.loop.configured_loop_seam_penetration_limit =
      loopConfig.rigid_body_maximum_safe_penetration;
  if (const auto *report = baker.getLoopErrorReport()) {
    diagnostics.loop.maximum_position_error = report->maximum_position_error;
    diagnostics.loop.position_bone = report->position_bone;
    diagnostics.loop.maximum_rotation_error_radians =
        report->maximum_rotation_error_radians;
    diagnostics.loop.rotation_bone = report->rotation_bone;
    diagnostics.loop.maximum_linear_velocity_error =
        report->maximum_linear_velocity_error;
    diagnostics.loop.linear_velocity_bone = report->linear_velocity_bone;
    diagnostics.loop.maximum_angular_velocity_error =
        report->maximum_angular_velocity_error;
    diagnostics.loop.angular_velocity_bone = report->angular_velocity_bone;
    if (diagnostics.bullet_safety_applicable) {
      diagnostics.loop.contact_set_changed = report->contact_set_changed;
      diagnostics.loop.contact_difference_count =
          report->contact_difference_count;
      diagnostics.loop.contact_pair_added_count =
          report->contact_pair_added_count;
      diagnostics.loop.contact_pair_removed_count =
          report->contact_pair_removed_count;
      diagnostics.loop.contact_state_changed_count =
          report->contact_state_changed_count;
      diagnostics.loop.meaningful_penetration_changed_count =
          report->meaningful_penetration_changed_count;
    }
    diagnostics.loop.cycle_valid = report->valid();
    diagnostics.loop.cycle_validation_state =
        loopValidationState(report->validation.state);
    diagnostics.loop.invalid_numeric_bone =
        report->validation.first_invalid_bone;
    diagnostics.loop.invalid_numeric_field =
        report->validation.first_invalid_field;
    if (diagnostics.bullet_safety_applicable) {
      diagnostics.loop.cycle_maximum_penetration =
          report->maximum_penetration;
      diagnostics.loop.cycle_maximum_penetration_time =
          report->maximum_penetration_time;
    }
    diagnostics.loop.start_boundary =
        loopBoundaryDiagnostics(report->start_boundary);
    diagnostics.loop.end_boundary =
        loopBoundaryDiagnostics(report->end_boundary);
    if (!report->valid()) {
      diagnostics.loop.danger_markers.push_back(
          {"Invalid Numeric", loopBoundaryTime,
           report->validation.first_invalid_bone.empty()
               ? report->validation.first_invalid_field
               : report->validation.first_invalid_bone + ":" +
                     report->validation.first_invalid_field});
    }
  }
  for (const auto &candidate : baker.getLoopCycleCandidates()) {
    LoopCycleDiagnostics value;
    value.cycle = candidate.cycle_index;
    value.valid = candidate.valid;
    value.collision_safe = diagnostics.bullet_safety_applicable
                               ? candidate.collision_safe
                               : false;
    value.within_tolerances = candidate.within_tolerances;
    value.score = candidate.normalized_score;
    value.pose_error = std::max(
        candidate.report.maximum_position_error,
        candidate.report.maximum_rotation_error_radians);
    value.velocity_error = std::max(
        candidate.report.maximum_linear_velocity_error,
        candidate.report.maximum_angular_velocity_error);
    if (diagnostics.bullet_safety_applicable) {
      value.contact_difference_count =
          candidate.report.contact_difference_count;
      value.maximum_penetration = candidate.report.maximum_penetration;
      value.maximum_penetration_time =
          candidate.report.maximum_penetration_time;
    }
    value.selected = diagnostics.loop.selected_cycle &&
                     *diagnostics.loop.selected_cycle == candidate.cycle_index;
    value.rejection_reasons = candidate.rejection_reasons;
    value.invalid_reason =
        candidate.valid
            ? joinLoopRejectionReasons(value.rejection_reasons)
            : loopValidationState(candidate.report.validation.state);
    value.start_boundary =
        loopBoundaryDiagnostics(candidate.report.start_boundary);
    value.end_boundary = loopBoundaryDiagnostics(candidate.report.end_boundary);
    diagnostics.loop.cycle_candidates.push_back(std::move(value));
  }
  if (const auto *report = baker.getLoopSeamReport()) {
    const auto physics_seam = report->physicsSeamGate(loopConfig);
    const auto quantization = report->quantizationGate();
    const auto collision = report->collisionGate(loopConfig);
    const auto joint = report->jointGate();
    const auto export_gate = report->exportGate(loopConfig);
    const auto &metrics = report->quantizedFinalWorld();
    diagnostics.loop.seam_corrected = report->correctionApplied();
    diagnostics.loop.effective_seam_window_seconds =
        report->correctionWindowDurationSeconds();
    diagnostics.loop.effective_seam_window_ratio =
        report->correctionWindowRatio();
    diagnostics.loop.seam_position_error = metrics.maximum_position_error;
    diagnostics.loop.seam_rotation_error_radians =
        metrics.maximum_rotation_error_radians;
    diagnostics.loop.seam_linear_velocity_jump =
        metrics.maximum_linear_velocity_jump;
    diagnostics.loop.seam_angular_velocity_jump =
        metrics.maximum_angular_velocity_jump;
    diagnostics.loop.seam_linear_acceleration_jump =
        metrics.maximum_linear_acceleration_jump;
    diagnostics.loop.seam_angular_acceleration_jump =
        metrics.maximum_angular_acceleration_jump;
    diagnostics.loop.seam_valid =
        report->validation().valid && report->auditValid();
    diagnostics.loop.seam_sample_count_sufficient =
        report->validation().verified_continuity_order >=
        (loopConfig.loop_seam_match_acceleration ? 2 : 1);
    diagnostics.loop.seam_verified_continuity_order =
        report->validation().verified_continuity_order;
    diagnostics.loop.physics_relative_available =
        report->validation().physics_relative_available;
    diagnostics.loop.physics_relative_fallback_reason =
        report->validation().physics_relative_fallback_reason;
    diagnostics.loop.missing_bone =
        report->validation().first_missing_bone;
    diagnostics.loop.affected_metric_space =
        report->validation().affected_metric_space;
    const auto driver = report->driverGate(loopConfig);
    diagnostics.loop.driver_available = driver.available;
    diagnostics.loop.driver_safe = driver.passes();
    diagnostics.loop.seam_physics_safe = physics_seam.passes();
    diagnostics.loop.seam_quantization_safe = quantization.passes();
    diagnostics.loop.seam_export_safe = export_gate.passes();
    diagnostics.loop.driver_position_error =
        driver.metrics.maximum_position_error;
    diagnostics.loop.driver_rotation_error_radians =
        driver.metrics.maximum_rotation_error_radians;
    diagnostics.loop.driver_linear_velocity_jump =
        driver.metrics.maximum_linear_velocity_jump;
    diagnostics.loop.driver_angular_velocity_jump =
        driver.metrics.maximum_angular_velocity_jump;
    const std::array<std::pair<double, std::string>, 4> driverItems{{
        {driver.metrics.maximum_position_error, driver.metrics.position_bone},
        {driver.metrics.maximum_rotation_error_radians,
         driver.metrics.rotation_bone},
        {driver.metrics.maximum_linear_velocity_jump,
         driver.metrics.linear_velocity_bone},
        {driver.metrics.maximum_angular_velocity_jump,
         driver.metrics.angular_velocity_bone},
    }};
    double worstDriverValue = -1.0;
    for (const auto &[value, bone] : driverItems) {
      if (!bone.empty() && value > worstDriverValue) {
        diagnostics.loop.worst_driver_bone = bone;
        worstDriverValue = value;
      }
    }
    if (driver.available && !driver.passes()) {
      diagnostics.loop.danger_markers.push_back(
          {"Driver Seam Jump", loopBoundaryTime,
           diagnostics.loop.worst_driver_bone});
    }
    if (diagnostics.bullet_safety_applicable) {
      diagnostics.loop.seam_collision_safe = collision.passes();
      diagnostics.loop.seam_joint_safe = joint.passes();
      diagnostics.loop.seam_maximum_penetration =
          report->maximumPenetration();
    }
    for (const auto &coverage : report->anchorCoverage()) {
      diagnostics.loop.anchor_coverage.push_back(
          {coverage.chain_root, coverage.fixed_anchor,
           coverage.expected_bone_count, coverage.measured_bone_count,
           coverage.complete});
    }
    if (!diagnostics.loop.missing_bone.empty()) {
      diagnostics.loop.danger_markers.push_back(
          {"Missing Bone", loopBoundaryTime, diagnostics.loop.missing_bone});
    }
  }
  for (const auto &window : baker.getLoopSeamWindowDiagnostics()) {
    LoopSeamWindowDiagnostics value;
    value.window_duration_seconds = window.window_duration_seconds;
    value.window_ratio = window.window_ratio;
    value.window_start_time = window.window_start_time;
    value.corrected = window.corrected;
    value.valid = window.valid;
    value.c0_pass = window.c0_pass;
    value.c1_pass = window.c1_pass;
    value.c2_pass = window.c2_pass;
    value.driver_pass = window.driver_pass;
    value.driver_c0_pass = window.driver_c0_pass;
    value.driver_c1_pass = window.driver_c1_pass;
    value.driver_c2_pass = window.driver_c2_pass;
    value.validation_pass = window.validation_pass;
    value.physics_seam_pass = window.physics_seam_pass;
    value.driver_seam_pass = window.driver_seam_pass;
    value.quantization_pass = window.quantization_pass;
    value.collision_pass = window.collision_pass;
    value.joint_pass = window.joint_pass;
    value.export_pass = window.export_pass;
    value.collision_safe = diagnostics.bullet_safety_applicable
                               ? window.collision_safe
                               : false;
    value.joint_safe = diagnostics.bullet_safety_applicable
                           ? window.joint_safe
                           : false;
    value.accepted = window.accepted;
    value.best_preview = window.best_preview;
    value.best_safe_export = window.best_safe_export;
    value.selected_for_output = window.selected_for_output;
    value.score = window.score;
    value.rejection_reasons = window.rejection_reasons;
    if (diagnostics.bullet_safety_applicable) {
      value.maximum_penetration = window.maximum_penetration;
      value.maximum_penetration_time = window.maximum_penetration_time;
      value.joint_failure_time = window.joint_failure_time;
      value.interpolation_failure_time =
          window.interpolation_failure_time;
      value.interpolated_sample_count = window.interpolated_sample_count;
    }
    value.canonicalized_bone_count = window.canonicalized_bone_count;
    value.preserved_driver_bone_count =
        window.preserved_driver_bone_count;
    value.driver_endpoint_conflict_count =
        window.driver_endpoint_conflict_count;
    value.invalid_item = window.first_invalid_bone.empty()
                             ? window.first_invalid_field
                             : window.first_invalid_bone + ":" +
                                   window.first_invalid_field;
    diagnostics.loop.seam_windows.push_back(std::move(value));
    if (window.corrected && window.selected_for_output) {
      diagnostics.loop.danger_markers.push_back(
          {"Seam Window Start", window.window_start_time, {}});
    }
    if (diagnostics.bullet_safety_applicable &&
        window.maximum_penetration_time >= 0.0) {
      diagnostics.loop.danger_markers.push_back(
          {"Maximum Penetration", window.maximum_penetration_time, {}});
    }
    if (diagnostics.bullet_safety_applicable &&
        window.interpolation_failure_time >= 0.0) {
      diagnostics.loop.danger_markers.push_back(
          {"Interpolation Audit Failure",
           window.interpolation_failure_time,
           window.first_invalid_bone.empty()
               ? window.first_invalid_field
               : window.first_invalid_bone});
    }
    if (diagnostics.bullet_safety_applicable &&
        window.joint_failure_time >= 0.0) {
      diagnostics.loop.danger_markers.push_back(
          {"Joint Audit Failure", window.joint_failure_time, {}});
    }
  }
  if (!baker.isLooping()) {
    diagnostics.loop.physical_state = "Not Applicable";
    diagnostics.loop.seam_state = "Not Applicable";
    diagnostics.loop.physics_seam_state = "Not Applicable";
    diagnostics.loop.quantization_state = "Not Applicable";
    diagnostics.loop.joint_state = "Not Applicable";
  } else {
    diagnostics.loop.physical_state =
        diagnostics.loop.cycle_valid
            ? (diagnostics.loop.converged ? "Converged" : "Not Converged")
            : "Invalid";
    diagnostics.loop.seam_state =
        diagnostics.loop.seam_correction_rejected
            ? "Rejected"
            : diagnostics.loop.seam_corrected ? "Corrected" : "No Correction";
    diagnostics.loop.physics_seam_state =
        diagnostics.loop.seam_physics_safe ? "Safe" : "Unsafe";
    diagnostics.loop.quantization_state =
        diagnostics.loop.seam_quantization_safe ? "Safe" : "Unsafe";
    diagnostics.loop.joint_state =
        !diagnostics.bullet_safety_applicable
            ? "Not Applicable"
            : diagnostics.loop.seam_joint_safe ? "Safe" : "Unsafe";
  }
  diagnostics.loop.driver_state =
      !diagnostics.loop.driver_available
          ? "Not Applicable"
          : diagnostics.loop.driver_safe ? "Safe" : "Unsafe";
  if (diagnostics.bullet_safety_applicable) {
    diagnostics.final_audit.unsafe_collision_count =
        baker.getUnsafeFinalCollisionCount();
    diagnostics.final_audit.collision_safe =
        diagnostics.final_audit.unsafe_collision_count == 0;
    diagnostics.final_audit.maximum_penetration =
        baker.getMaximumFinalRigidBodyPenetration();
    diagnostics.final_audit.worst_collision_pair =
        baker.getWorstFinalCollisionPair();
    diagnostics.final_audit.unsafe_joint_count =
        baker.getUnsafeFinalJointCount();
    diagnostics.final_audit.joint_safe =
        diagnostics.final_audit.unsafe_joint_count == 0;
  }
  diagnostics.final_audit.numerical_safe = framesAreFinite(baker.frames());
  diagnostics.loop.collision_state =
      !diagnostics.bullet_safety_applicable
          ? "Not Applicable"
          : diagnostics.loop.seam_collision_safe &&
                    diagnostics.final_audit.collision_safe
                ? "Safe"
                : "Unsafe";
  diagnostics.loop.export_state =
      preflightFor(execution).animation_allowed ? "Allowed" : "Blocked";
  diagnostics.determinism.bullet_version = baker.getNativeBulletVersion();
  diagnostics.determinism.simulation_mode =
      execution.input.mapper.config().simulation_mode ==
              baker::BoneMapper::SimulationMode::RigidBody
          ? "Bullet"
          : "XPBD";
  diagnostics.determinism.timing_fingerprint =
      timingFingerprintFor(execution.input);
  diagnostics.determinism.config_fingerprint =
      configFingerprintFor(execution.input);
  diagnostics.determinism.content_fingerprint =
      contentFingerprintFor(execution.input);
  diagnostics.effective_config = effectiveConfigFor(execution.input);
  for (auto &value : diagnostics.effective_config.global) {
    if (value.name == "Nominal Output dt") {
      value.effective_value = valueText(baker.outputFrameInterval());
      value.source = "Endpoint-Fitted Timing";
      value.reason = std::abs(baker.outputFrameInterval() -
                              execution.input.timing.nominalOutputDt()) > 1e-12
                         ? "Duration divided by ceil(duration / nominal dt)"
                         : "Nominal timing is already endpoint exact";
    } else if (value.name == "Fixed Substeps" && diagnostics.substeps) {
      value.effective_value =
          std::to_string(diagnostics.substeps->configured_minimum);
      value.source = "Fixed UI Contract";
      value.reason = "Runtime motion analysis can warn but cannot change the "
                     "executed Bullet step count";
    } else if (value.name == "Effective Joint Spring Behavior" &&
               diagnostics.joint_spring) {
      value.effective_value =
          diagnostics.joint_spring->active_spring_axis_count > 0
              ? "Bullet stability-limited, solver-dependent"
              : "Inactive";
      value.source = "Bullet Runtime";
      value.reason = diagnostics.joint_spring->effective_behavior;
    }
  }
  diagnostics.terminal_phase = terminal_phase;
  const bool unsafe =
      !diagnostics.final_audit.numerical_safe ||
      (diagnostics.bullet_safety_applicable &&
       (!diagnostics.final_audit.collision_safe ||
        !diagnostics.final_audit.joint_safe)) ||
                      (baker.isLooping() &&
                       (!diagnostics.loop.cycle_valid ||
                        !diagnostics.loop.seam_valid ||
                        !diagnostics.loop.driver_safe ||
                        (diagnostics.bullet_safety_applicable &&
                         (!diagnostics.loop.seam_collision_safe ||
                          !diagnostics.loop.seam_joint_safe)) ||
                        diagnostics.loop.seam_correction_rejected));
  bool warning = false;
  if (diagnostics.substeps) {
    warning = diagnostics.substeps->insufficient_step_risk_count > 0;
  }
  if (diagnostics.joint_preflight &&
      !diagnostics.joint_preflight->warnings.empty()) {
    warning = true;
  }
  if (diagnostics.collider_preflight &&
      diagnostics.collider_preflight->warning_count > 0) {
    warning = true;
  }
  diagnostics.chain_stability = unsafe ? "Unsafe" : warning ? "Warning" : "Safe";
  return diagnostics;
}

void runBakeWorker(std::stop_token stop,
                   std::unique_ptr<BakeExecutionState> execution,
                   const std::shared_ptr<BakeWorkerMailbox> &mailbox) {
  BakeJobResult result;
  result.generation = execution->input.generation;
  result.fingerprint = execution->input.fingerprint;
  try {
    execution->baker->setCancellationCheck(
        [stop]() { return stop.stop_requested(); });
    const std::weak_ptr<BakeWorkerMailbox> weak_mailbox = mailbox;
    execution->baker->setAuditPhaseCallback([weak_mailbox]() {
      if (const auto locked = weak_mailbox.lock()) {
        locked->phase = WorkerPhase::Auditing;
      }
    });
    mailbox->phase = WorkerPhase::Preparing;
    if (!execution->initialized) {
      execution->baker->initialize();
      execution->initialized = true;
    }
    mailbox->total = execution->baker->totalSteps();
    mailbox->current = execution->baker->currentStep();
    const int resumed_from_step = execution->baker->currentStep();
    mailbox->phase = WorkerPhase::Simulating;
    while (execution->baker->currentStep() <
           execution->baker->totalSteps()) {
      execution->baker->step();
      mailbox->current = execution->baker->currentStep();
    }
    mailbox->phase = WorkerPhase::Finalizing;
    execution->baker->finalizeFrames();
    if (stop.stop_requested()) {
      throw baker::BakeCancelled{};
    }
    mailbox->current = execution->baker->currentStep();
    mailbox->phase = WorkerPhase::Committing;
    auto frame_copy = std::make_shared<std::vector<baker::BakedFrame>>();
    frame_copy->reserve(execution->baker->frames().size());
    for (const auto &frame : execution->baker->frames()) {
      if (stop.stop_requested()) {
        throw baker::BakeCancelled{};
      }
      frame_copy->push_back(frame);
    }
    result.frames = std::move(frame_copy);
    if (stop.stop_requested()) {
      throw baker::BakeCancelled{};
    }
    result.export_preflight = preflightFor(*execution);
    if (stop.stop_requested()) {
      throw baker::BakeCancelled{};
    }
    result.diagnostics =
        diagnosticsFor(*execution, WorkerPhase::Finished, resumed_from_step);
    result.terminal_state = BakeState::Completed;
    {
      std::lock_guard lock(mailbox->mutex);
      mailbox->completed_execution = std::move(execution);
      mailbox->result = std::move(result);
    }
  } catch (const baker::BakeCancelled &) {
    result.terminal_state = BakeState::Cancelled;
    result.error = "Bake cancelled";
    result.export_preflight.block_reasons.push_back(
        {ExportBlockCode::Cancelled, result.error});
    std::lock_guard lock(mailbox->mutex);
    mailbox->result = std::move(result);
  } catch (const std::exception &error) {
    result.terminal_state = BakeState::Failed;
    result.error = error.what();
    result.export_preflight.block_reasons.push_back(
        {ExportBlockCode::Failed, result.error});
    std::lock_guard lock(mailbox->mutex);
    mailbox->result = std::move(result);
  }
  mailbox->phase = WorkerPhase::Finished;
  mailbox->finished = true;
}

}

BakeExecutionState::BakeExecutionState(BakeJobInput job)
    : input(std::move(job)), physics_source_animation(input.source_animation),
      physics_target_animation(input.transition_target_animation) {
  auto &config = input.mapper.config();
  config.allow_input_only_molang_zero_fallback =
      input.allow_input_molang_zero;
  config.allow_selected_molang_zero_fallback =
      input.allow_selected_molang_zero;

  compatibility = baker::prepareRigidBodyInputCompatibility(
      input.mapper, physics_source_animation,
      physics_target_animation ? &*physics_target_animation : nullptr);
  if (input.mapper.physicsBones().empty()) {
    throw std::invalid_argument(
        "rigid-body compatibility preflight removed every selected physics "
        "bone; select at least one bone that owns a usable cube");
  }

  baker = std::make_unique<baker::PhysicsBaker>(input.mapper);
  baker->setSourceAnimation(&physics_source_animation);
  const loader::Animation *target =
      physics_target_animation ? &*physics_target_animation
                               : &physics_source_animation;
  if (input.transition_mode == 0) {
    baker->setTransitionRequest(nullptr);
    baker->setTransitionAnimation(nullptr);
  } else if (input.transition_mode == 1) {
    baker->setTransitionRequest(nullptr);
    baker->setTransitionAnimation(target);
  } else {
    transition_request.emplace(
        physics_source_animation, *target,
        std::max(0.0, input.transition_source_exit),
        std::max(0.0, input.transition_target_entry),
        std::max(0.0, input.transition_duration),
        input.transition_follow_weights);
    baker->setTransitionAnimation(target);
    baker->setTransitionRequest(&*transition_request);
  }
  baker->setDt(input.timing.nominalOutputDt());
}

const loader::Animation *
BakeExecutionState::outputReferenceAnimation() const {
  if (!baker) {
    return nullptr;
  }
  const loader::Animation *reference = baker->getOutputReferenceAnimation();
  if (reference == &physics_source_animation) {
    return &input.source_animation;
  }
  if (physics_target_animation && reference == &*physics_target_animation) {
    return input.transition_target_animation
               ? &*input.transition_target_animation
               : &input.source_animation;
  }
  return reference;
}

std::string BakeExecutionState::compatibilityStatusSuffix() const {
  const std::size_t skipped =
      compatibility.skipped_blocked_dynamic_bones.size();
  const std::size_t promoted =
      compatibility.promoted_animated_compound_bones.size();
  const std::size_t repaired =
      compatibility.repaired_source_scale_bones.size() +
      compatibility.repaired_target_scale_bones.size();
  if (skipped == 0 && promoted == 0 && repaired == 0) {
    return {};
  }

  std::ostringstream message;
  message << " — compatibility: ";
  bool needs_separator = false;
  if (skipped != 0) {
    message << "skipped " << skipped << " blocked empty rigid "
            << (skipped == 1 ? "body" : "bodies");
    needs_separator = true;
  }
  if (promoted != 0) {
    if (needs_separator) {
      message << ", ";
    }
    message << "promoted " << promoted << " animated compound "
            << (promoted == 1 ? "bone" : "bones");
    needs_separator = true;
  }
  if (repaired != 0) {
    if (needs_separator) {
      message << ", ";
    }
    message << "repaired " << repaired << " degenerate scale channel";
    if (repaired != 1) {
      message << "s";
    }
  }
  return message.str();
}

std::string SessionFingerprint::hex() const {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

double BakeTimingConfig::nominalOutputDt() const {
  if (output_fps <= 0 || output_fps > 1000) {
    throw std::invalid_argument("output FPS must be in [1, 1000]");
  }
  return 1.0 / static_cast<double>(output_fps);
}

AppSession &AppSession::instance() {
  static AppSession session;
  return session;
}

AppSession::AppSession() {
  pull = animationFollowStrengthForUi(
      bone_mapper.config().animation_pull_compliance);
  skeleton_view.setBoneMapper(&bone_mapper);
  skeleton_view.setHiddenBones(&hidden_bone_names);
}

AppSession::~AppSession() { shutdownBakeWorker(); }

void AppSession::applyUiToConfig() {
  auto &cfg = bone_mapper.config();



  cfg.allow_input_only_molang_zero_fallback = false;
  cfg.allow_selected_molang_zero_fallback = false;
  cfg.simulation_mode = solver_mode == 0
                            ? baker::BoneMapper::SimulationMode::Xpbd
                            : baker::BoneMapper::SimulationMode::RigidBody;
  cfg.gravity_y = gravity_y;
  cfg.particle_mass = std::max(0.0, static_cast<double>(particle_mass));
  cfg.compliance = compliance;
  cfg.damping_compliance = damping;
  cfg.bend_compliance = bend_compliance;
  cfg.xpbd_collision_restitution =
      std::max(0.0, static_cast<double>(xpbd_restitution));
  cfg.air_drag = air_drag;
  cfg.turbulence = turbulence;
  cfg.wind_speed = wind_speed;
  cfg.wind_direction_degrees = wind_dir;
  cfg.wind_elevation_degrees = wind_elev;
  cfg.use_wind_components = use_wind_components;
  cfg.wind_x = wind_x;
  cfg.wind_y = wind_y;
  cfg.wind_z = wind_z;
  cfg.movement_speed = movement_speed;
  cfg.movement_direction_degrees = movement_dir;
  cfg.movement_elevation_degrees = movement_elev;
  cfg.animation_pull_compliance = animationFollowComplianceFromUi(pull);
  cfg.solver_iterations = std::max(1, solver_iters);
  cfg.enable_ground_collision = enable_ground;
  cfg.enable_angle_constraints = enable_angle;
  cfg.enable_real_gravity_field = enable_real_gravity;
  cfg.max_bend_degrees = max_bend;
  cfg.collision_skin = std::max(0.0, static_cast<double>(collision_skin));
  cfg.output_timeline_mode =
      output_timeline_mode == 1
          ? baker::BoneMapper::OutputTimelineMode::SourceKeyframeGrid
          : baker::BoneMapper::OutputTimelineMode::BakeFps;
  cfg.loop_seam_strategy =
      loop_seam_strategy == 1
          ? baker::BoneMapper::LoopSeamStrategy::VisualSubtree
          : baker::BoneMapper::LoopSeamStrategy::PhysicsRelative;
  cfg.loop_seam_window_ratio =
      std::clamp(static_cast<double>(loop_seam_window_ratio), 0.0, 0.5);
  cfg.rigid_body_substeps = std::clamp(rigid_substeps, 1, 16);
  cfg.rigid_body_unit_scale = std::max(0.0001, static_cast<double>(unit_scale));
  cfg.rigid_body_linear_damping =
      std::clamp(static_cast<double>(rb_linear_damping), 0.0, 1.0);
  cfg.rigid_body_angular_damping =
      std::clamp(static_cast<double>(rb_angular_damping), 0.0, 1.0);
  cfg.rigid_body_joint_stiffness = rb_joint_stiffness;
  cfg.rigid_body_joint_damping = rb_joint_damping;
  cfg.rigid_body_friction = rb_friction;
  cfg.rigid_body_restitution = rb_restitution;
  cfg.rigid_body_ccd = rb_ccd;
  cfg.rigid_body_maximum_safe_penetration = rb_max_safe_pen;
  cfg.rigid_body_max_bend_x_degrees = rb_max_bend_x;
  cfg.rigid_body_max_bend_y_degrees = rb_max_bend_y;
  cfg.rigid_body_max_bend_z_degrees = rb_max_bend_z;
  cfg.transition_duration =
      std::max(0.0, static_cast<double>(transition_duration));
  switch (loop_mode) {
  case 1:
    cfg.loop_mode = baker::BoneMapper::LoopMode::ForceLoop;
    break;
  case 2:
    cfg.loop_mode = baker::BoneMapper::LoopMode::ForceOnce;
    break;
  default:
    cfg.loop_mode = baker::BoneMapper::LoopMode::Auto;
    break;
  }
}

void AppSession::setSolverMode(int mode) {
  const int next = mode <= 0 ? 0 : 1;
  if (solver_mode == next) {
    return;
  }
  solver_mode = next;
  applyUiToConfig();
  const char *label = solver_mode == 0 ? "XPBD" : "Bullet";
  invalidatePhysicsArtifacts(InvalidationReason::Solver,
                             std::string("Solver → ") + label +
                                 " — play to rebake");
}

void AppSession::setLoopMode(int mode) {
  const int next = std::clamp(mode, 0, 2);
  if (loop_mode == next) {
    return;
  }
  loop_mode = next;
  applyUiToConfig();
  invalidatePhysicsArtifacts(InvalidationReason::LoopOrSeam,
                             "Loop mode changed — play to rebake");
}

void AppSession::loadModel(const std::filesystem::path &path) {
  try {
    auto loaded_geometry = loader::ModelLoader::load(path);
    geometry = std::move(loaded_geometry);
    model_path = path.string();
    bone_mapper.replaceModelBones(geometry.bones);
    for (auto it = transition_follow_weights.begin();
         it != transition_follow_weights.end();) {
      const bool exists =
          std::any_of(geometry.bones.begin(), geometry.bones.end(),
                      [&](const auto &bone) { return bone.name == it->first; });
      if (exists) {
        ++it;
      } else {
        it = transition_follow_weights.erase(it);
      }
    }
    selected_bone_name.clear();
    hidden_bone_names.clear();
    closeBoneContext();
    skeleton_view.setGeometry(&geometry);
    skeleton_view.setBoneMapper(&bone_mapper);
    skeleton_view.setSelectedBone({});
    skeleton_view.setHiddenBones(&hidden_bone_names);
    camera_needs_fit = true;
    last_error.clear();

    std::string extra;
    const std::string ext = path.extension().string();
    std::string ext_l = ext;
    std::transform(
        ext_l.begin(), ext_l.end(), ext_l.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });


    if (ext_l == ".bbmodel") {
      std::ifstream in(path, std::ios::binary);
      if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        nlohmann::json root = nlohmann::json::parse(ss.str());
        int tex_n = 0;
        if (root.contains("textures") && root.at("textures").is_array() &&
            !root.at("textures").empty()) {

          std::string err;
          bool ok = false;
          for (const auto &t : root.at("textures")) {
            if (!t.is_object()) {
              continue;
            }
            gfx::TextureImage loaded_texture;
            if (loadTextureFromBbEntry(t, path, loaded_texture, &err)) {
              const bool texture_changed =
                  !sameTextureResource(model_texture, loaded_texture);
              model_texture = std::move(loaded_texture);
              texture_path = path.string() + "#embedded";
              if (t.contains("name") && t.at("name").is_string()) {
                texture_path = t.at("name").get<std::string>();
              }
              if (texture_changed) {
                advanceGeneration(texture_generation_);
              }
              ok = true;
              ++tex_n;
              break;
            }
          }
          if (!ok) {
            last_error = err.empty() ? "bbmodel texture extract failed" : err;
          }
        }



        int anim_n = 0;
        try {
          auto ar = convertBbAnimations(root);
          if (!ar.animations.empty()) {
            animation_root = std::move(ar);
            animation_path = path.string();
            clearAnimationSelection();
            anim_n = static_cast<int>(animation_root.animations.size());
          }
        } catch (...) {

        }

        if (anim_n == 0 && root.contains("animations") &&
            root.at("animations").is_array() &&
            !root.at("animations").empty()) {
          const auto &a0 = root.at("animations").at(0);
          if (a0.is_object() && a0.contains("path") &&
              a0.at("path").is_string()) {
            const auto ap =
                path.parent_path() / a0.at("path").get<std::string>();
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(ap, ec);
            if (!ec && std::filesystem::exists(canon)) {
              try {

                animation_root = loader::AnimationLoader::load(canon);
                animation_path = canon.string();
                clearAnimationSelection();
                anim_n = static_cast<int>(animation_root.animations.size());
              } catch (...) {
              }
            }
          }
        }
        extra = " | tex " + std::to_string(tex_n) + " | anim " +
                std::to_string(anim_n);
      }
    }

    invalidatePhysicsArtifacts(
        InvalidationReason::Model,
        "Model loaded — select physics bones and play to bake");
    status = "Model: " + path.filename().string() + " (" +
             std::to_string(geometry.bones.size()) + " bones)" + extra;
    if (geometry.description.has_texture_size) {
      status += " UV " + std::to_string(geometry.description.texture_width) +
                "x" + std::to_string(geometry.description.texture_height);
    }
    if (model_texture.valid()) {
      status += " tex " + std::to_string(model_texture.width) + "x" +
                std::to_string(model_texture.height);
    }
    advanceGeneration(model_generation_);
  } catch (const std::exception &e) {
    last_error = e.what();
    status = "Model load failed";
  }
}

bool AppSession::loadTexture(const std::filesystem::path &path) {
  std::string err;
  gfx::TextureImage loaded_texture;
  if (!gfx::loadTextureImage(path, loaded_texture, &err)) {
    last_error = err.empty() ? "Texture load failed" : err;
    status = "Texture load failed";
    return false;
  }
  const bool texture_changed =
      !sameTextureResource(model_texture, loaded_texture);
  model_texture = std::move(loaded_texture);
  texture_path = path.string();
  if (texture_changed) {
    advanceGeneration(texture_generation_);
  }
  last_error.clear();
  status = "Texture: " + path.filename().string() + " (" +
           std::to_string(model_texture.width) + "x" +
           std::to_string(model_texture.height) + ")";


  if (geometry.description.has_texture_size &&
      (model_texture.width != geometry.description.texture_width ||
       model_texture.height != geometry.description.texture_height)) {
    status += "  [!] model UV " +
              std::to_string(geometry.description.texture_width) + "x" +
              std::to_string(geometry.description.texture_height) +
              " — prefer matching PNG or load .bbmodel for face UVs";
  }
  return true;
}

void AppSession::clearTexture() {
  const bool texture_changed = hasTextureResourceState(model_texture);
  model_texture.clear();
  texture_path.clear();
  if (texture_changed) {
    advanceGeneration(texture_generation_);
  }
  status = "Texture cleared";
}

void AppSession::loadAnimation(const std::filesystem::path &path) {
  try {
    auto loaded = loader::AnimationLoader::load(path);
    if (loaded.animations.empty()) {
      throw std::runtime_error("No animations found in this file");
    }
    animation_root = std::move(loaded);
    animation_path = path.string();
    advanceGeneration(animation_generation_);
    selected_animation_name.clear();
    selected_animation = nullptr;
    const std::string &first_name =
        animation_root.animation_order.empty()
            ? animation_root.animations.begin()->first
            : animation_root.animation_order.front();
    selectAnimation(first_name);
    last_error.clear();
    status = "Animations: " + path.filename().string() + " (" +
             std::to_string(animation_root.animations.size()) + ")";
  } catch (const std::exception &e) {
    last_error = e.what();
    status = "Animation load failed";
  }
}

void AppSession::selectAnimation(const std::string &name) {
  auto it = animation_root.animations.find(name);
  if (it == animation_root.animations.end()) {
    return;
  }
  selected_animation_name = name;
  selected_animation = &it->second;
  preview_time = 0.0;
  playback_state = PlaybackState::Paused;
  invalidatePhysicsArtifacts(InvalidationReason::Animation,
                             "Anim: " + name + " — play to bake");
}

void AppSession::clearAnimationSelection() {
  selected_animation_name.clear();
  selected_animation = nullptr;
  preview_time = 0.0;
  playback_state = PlaybackState::Paused;
  invalidatePhysicsArtifacts(
      InvalidationReason::Animation,
      "Rest pose — select an animation to bake");
  status = "Rest pose (no animation selected)";
}

void AppSession::updateCameraFollowPreview() {
  if (!camera_follow_preview || geometry.bones.empty()) {
    return;
  }


  skeleton_view.setGeometry(&geometry);
  float radius = 10.0f;

  const loader::Animation *anim = currentPreviewReferenceAnimation();
  double animation_time = currentPreviewReferenceTime();
  std::map<std::string, baker::BonePoseCalculator::Pose> poses;
  if (presentation_mode != PresentationMode::SourcePreview) {
    if (const auto *frame = currentPreviewFrame()) {
      std::map<std::string, std::array<double, 3>> position_overrides;
      std::map<std::string, std::array<double, 3>> rotation_overrides;
      for (const auto &bs : frame->bone_states) {
        position_overrides[bs.bone_name] = bs.position;
        rotation_overrides[bs.bone_name] = bs.rotation;
      }
      poses = baker::BonePoseCalculator::calculate(
          geometry.bones, anim, animation_time, &position_overrides,
          &rotation_overrides);
      for (const auto &bs : frame->bone_states) {
        if (bs.has_world_position) {
          auto it = poses.find(bs.bone_name);
          if (it != poses.end()) {
            it->second.world_position = bs.world_position;
          }
        }
      }
    }
  } else {
    poses = baker::BonePoseCalculator::calculate(geometry.bones, anim,
                                                 animation_time);
  }
  if (poses.empty()) {
    return;
  }
  float min_x = 1e9f, min_y = 1e9f, min_z = 1e9f;
  float max_x = -1e9f, max_y = -1e9f, max_z = -1e9f;
  bool any = false;
  for (const auto &[_, p] : poses) {
    const float x = static_cast<float>(p.world_position[0]);
    const float y = static_cast<float>(p.world_position[1]);
    const float z = static_cast<float>(p.world_position[2]);
    min_x = std::min(min_x, x);
    max_x = std::max(max_x, x);
    min_y = std::min(min_y, y);
    max_y = std::max(max_y, y);
    min_z = std::min(min_z, z);
    max_z = std::max(max_z, z);
    any = true;
  }
  if (!any) {
    return;
  }

  const float tx = 0.5f * (min_x + max_x);
  const float ty = 0.5f * (min_y + max_y);
  const float tz = 0.5f * (min_z + max_z);
  constexpr float kFollow = 0.18f;
  camera.pan_x += (tx - camera.pan_x) * kFollow;
  camera.pan_y += (ty - camera.pan_y) * kFollow;
  camera.pan_z += (tz - camera.pan_z) * kFollow;
  (void)radius;
}

void AppSession::togglePhysicsBone(const std::string &name, bool enabled) {
  if (bake_busy.load()) {
    return;
  }
  if (enabled) {
    bone_mapper.addPhysicsBone(name);
  } else {
    bone_mapper.removePhysicsBone(name);
  }
  invalidatePhysicsArtifacts(InvalidationReason::PhysicsBones,
                             "Physics bones changed — play to rebake");
}

void AppSession::setCollisionRoot(const std::string &name, bool enabled) {
  if (bake_busy.load()) {
    return;
  }
  const bool was_enabled = bone_mapper.isCollisionRoot(name);
  if (was_enabled == enabled) {
    return;
  }
  if (enabled) {
    bone_mapper.addCollisionRoot(name);
  } else {
    bone_mapper.removeCollisionRoot(name);
  }
  invalidatePhysicsArtifacts(InvalidationReason::CollisionRoots,
                             "Collision roots changed — play to rebake");
}

void AppSession::clearCollisionRoots() {
  if (bake_busy.load()) {
    return;
  }
  if (bone_mapper.collisionRoots().empty()) {
    return;
  }
  bone_mapper.clearCollisionRoots();
  invalidatePhysicsArtifacts(InvalidationReason::CollisionRoots,
                             "Collision roots cleared — play to rebake");
}

void AppSession::selectBone(const std::string &name) {
  if (name != selected_bone_name && per_bone_draft_dirty &&
      !applySelectedBoneConfig()) {
    status = "Cannot change bone: fix or discard the unapplied bone draft";
    return;
  }
  if (bone_context_open && bone_context_bone_name != name) {
    closeBoneContext();
  }
  selected_bone_name = name;
  skeleton_view.setSelectedBone(name);
  loadSelectedBoneEditors();
}

void AppSession::setBoneVisible(const std::string &name, bool visible) {
  const bool known = std::any_of(
      geometry.bones.begin(), geometry.bones.end(),
      [&](const loader::Bone &bone) { return bone.name == name; });
  if (!known) {
    return;
  }
  std::vector<std::string> pending{name};
  std::set<std::string> visited;
  std::size_t changed_count = 0;
  while (!pending.empty()) {
    std::string current = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    if (visible) {
      changed_count += hidden_bone_names.erase(current);
    } else {
      changed_count += hidden_bone_names.insert(current).second ? 1u : 0u;
    }
    for (const auto &bone : geometry.bones) {
      if (bone.has_parent && bone.parent == current && bone.name != current) {
        pending.push_back(bone.name);
      }
    }
  }
  if (changed_count == 0) {
    return;
  }
  status = std::string("Bone subtree ") + (visible ? "shown: " : "hidden: ") +
           name + " (" + std::to_string(changed_count) +
           (visible ? " newly shown)" : " newly hidden)");
}

std::string AppSession::pickBoneAt(float viewport_x, float viewport_y,
                                   float view_w, float view_h) {
  if (geometry.bones.empty() || viewport_x < 0.0f || viewport_y < 0.0f ||
      viewport_x > view_w || viewport_y > view_h) {
    return {};
  }
  return render::pickBone(buildViewportDrawList(view_w, view_h), viewport_x,
                          viewport_y);
}

bool AppSession::openBoneContext(const std::string &name) {
  const bool known = std::any_of(
      geometry.bones.begin(), geometry.bones.end(),
      [&](const loader::Bone &bone) { return bone.name == name; });
  if (!known) {
    return false;
  }
  selectBone(name);
  if (selected_bone_name != name) {
    return false;
  }
  bone_context_bone_name = name;
  bone_context_open = true;
  return true;
}

void AppSession::closeBoneContext() {
  bone_context_open = false;
  bone_context_bone_name.clear();
}

void AppSession::loadSelectedBoneEditors() {
  per_bone_draft_dirty = false;
  bone_ov_mass = bone_ov_compliance = bone_ov_damping = bone_ov_max_bend =
      false;
  bone_ov_bend_compliance = bone_ov_rb_bend_x = bone_ov_rb_bend_y =
      bone_ov_rb_bend_z = false;
  bone_ov_pull = bone_ov_gravity = bone_ov_wind = bone_ov_turbulence =
      bone_ov_fixed = false;
  bone_ov_transition_follow = false;
  bone_fixed = false;
  bone_mass = static_cast<float>(bone_mapper.config().particle_mass);
  bone_compliance = static_cast<float>(bone_mapper.config().compliance);
  bone_damping = static_cast<float>(bone_mapper.config().damping_compliance);
  bone_max_bend = static_cast<float>(bone_mapper.config().max_bend_degrees);
  bone_bend_compliance =
      static_cast<float>(bone_mapper.config().bend_compliance);
  bone_rb_bend_x =
      static_cast<float>(bone_mapper.config().rigid_body_max_bend_x_degrees);
  bone_rb_bend_y =
      static_cast<float>(bone_mapper.config().rigid_body_max_bend_y_degrees);
  bone_rb_bend_z =
      static_cast<float>(bone_mapper.config().rigid_body_max_bend_z_degrees);
  bone_pull = animationFollowStrengthForUi(
      bone_mapper.config().animation_pull_compliance);
  bone_gravity_scale = 1.0f;
  bone_wind = 1.0f;
  bone_turbulence = 1.0f;
  if (selected_bone_name.empty()) {
    return;
  }
  if (const auto weight = transition_follow_weights.find(selected_bone_name);
      weight != transition_follow_weights.end()) {
    bone_ov_transition_follow = true;
    bone_transition_follow = static_cast<float>(weight->second);
  } else {
    bone_transition_follow = 1.0f;
  }
  const auto *cfg = bone_mapper.getBoneConfig(selected_bone_name);
  if (cfg == nullptr) {
    return;
  }
  if (cfg->particle_mass) {
    bone_ov_mass = true;
    bone_mass = static_cast<float>(*cfg->particle_mass);
  }
  if (cfg->compliance) {
    bone_ov_compliance = true;
    bone_compliance = static_cast<float>(*cfg->compliance);
  }
  if (cfg->damping_compliance) {
    bone_ov_damping = true;
    bone_damping = static_cast<float>(*cfg->damping_compliance);
  }
  if (cfg->max_bend_degrees) {
    bone_ov_max_bend = true;
    bone_max_bend = static_cast<float>(*cfg->max_bend_degrees);
  }
  if (cfg->bend_compliance) {
    bone_ov_bend_compliance = true;
    bone_bend_compliance = static_cast<float>(*cfg->bend_compliance);
  }
  if (cfg->rigid_body_max_bend_x_degrees) {
    bone_ov_rb_bend_x = true;
    bone_rb_bend_x = static_cast<float>(*cfg->rigid_body_max_bend_x_degrees);
  }
  if (cfg->rigid_body_max_bend_y_degrees) {
    bone_ov_rb_bend_y = true;
    bone_rb_bend_y = static_cast<float>(*cfg->rigid_body_max_bend_y_degrees);
  }
  if (cfg->rigid_body_max_bend_z_degrees) {
    bone_ov_rb_bend_z = true;
    bone_rb_bend_z = static_cast<float>(*cfg->rigid_body_max_bend_z_degrees);
  }
  if (cfg->animation_pull_compliance) {
    bone_ov_pull = true;
    bone_pull = animationFollowStrengthForUi(*cfg->animation_pull_compliance);
  }
  if (cfg->gravity_scale) {
    bone_ov_gravity = true;
    bone_gravity_scale = static_cast<float>(*cfg->gravity_scale);
  }
  if (cfg->wind_influence) {
    bone_ov_wind = true;
    bone_wind = static_cast<float>(*cfg->wind_influence);
  }
  if (cfg->turbulence_influence) {
    bone_ov_turbulence = true;
    bone_turbulence = static_cast<float>(*cfg->turbulence_influence);
  }
  if (cfg->fixed) {
    bone_ov_fixed = true;
    bone_fixed = *cfg->fixed;
  }
}

bool AppSession::applySelectedBoneConfig() {
  if (selected_bone_name.empty()) {
    status = "Select a bone first";
    return false;
  }
  const auto requireFiniteRange = [&](bool enabled, float value, float minimum,
                                      float maximum, const char *label) {
    if (!enabled) {
      return true;
    }
    if (!std::isfinite(value) || value < minimum || value > maximum) {
      status = std::string("Invalid per-bone ") + label;
      return false;
    }
    return true;
  };
  if (!requireFiniteRange(bone_ov_mass, bone_mass, 0.000001f, 1000000.0f,
                          "mass") ||
      !requireFiniteRange(bone_ov_compliance, bone_compliance, 0.0f,
                          1000000.0f, "compliance") ||
      !requireFiniteRange(bone_ov_damping, bone_damping, 0.0f, 1000000.0f,
                          "damping") ||
      !requireFiniteRange(bone_ov_max_bend, bone_max_bend, 0.0f, 180.0f,
                          "maximum bend") ||
      !requireFiniteRange(bone_ov_bend_compliance, bone_bend_compliance, 0.0f,
                          1000000.0f, "bend compliance") ||
      !requireFiniteRange(bone_ov_rb_bend_x, bone_rb_bend_x, 0.0f, 180.0f,
                          "Bullet bend X") ||
      !requireFiniteRange(bone_ov_rb_bend_y, bone_rb_bend_y, 0.0f, 180.0f,
                          "Bullet bend Y") ||
      !requireFiniteRange(bone_ov_rb_bend_z, bone_rb_bend_z, 0.0f, 180.0f,
                          "Bullet bend Z") ||
      !requireFiniteRange(bone_ov_pull, bone_pull, 0.0f, 1.0f,
                          "animation follow") ||
      !requireFiniteRange(bone_ov_transition_follow, bone_transition_follow,
                          0.0f, 1.0f, "transition follow") ||
      !requireFiniteRange(bone_ov_gravity, bone_gravity_scale, 0.0f,
                          1000000.0f, "gravity scale") ||
      !requireFiniteRange(bone_ov_wind, bone_wind, 0.0f, 1000000.0f,
                          "wind influence") ||
      !requireFiniteRange(bone_ov_turbulence, bone_turbulence, 0.0f,
                          1000000.0f, "turbulence influence")) {
    return false;
  }
  baker::BoneMapper::BonePhysicsConfig cfg;
  if (bone_ov_mass) {
    cfg.particle_mass = bone_mass;
  }
  if (bone_ov_compliance) {
    cfg.compliance = bone_compliance;
  }
  if (bone_ov_damping) {
    cfg.damping_compliance = bone_damping;
  }
  if (bone_ov_max_bend) {
    cfg.max_bend_degrees = bone_max_bend;
  }
  if (bone_ov_bend_compliance) {
    cfg.bend_compliance = bone_bend_compliance;
  }
  if (bone_ov_rb_bend_x) {
    cfg.rigid_body_max_bend_x_degrees = bone_rb_bend_x;
  }
  if (bone_ov_rb_bend_y) {
    cfg.rigid_body_max_bend_y_degrees = bone_rb_bend_y;
  }
  if (bone_ov_rb_bend_z) {
    cfg.rigid_body_max_bend_z_degrees = bone_rb_bend_z;
  }
  if (bone_ov_pull) {
    cfg.animation_pull_compliance = animationFollowComplianceFromUi(bone_pull);
  }
  if (bone_ov_gravity) {
    cfg.gravity_scale = bone_gravity_scale;
  }
  if (bone_ov_wind) {
    cfg.wind_influence = bone_wind;
  }
  if (bone_ov_turbulence) {
    cfg.turbulence_influence = bone_turbulence;
  }
  if (bone_ov_fixed) {
    cfg.fixed = bone_fixed;
  }
  const bool any_mapper_override =
      bone_ov_mass || bone_ov_compliance || bone_ov_damping ||
      bone_ov_max_bend || bone_ov_bend_compliance || bone_ov_rb_bend_x ||
      bone_ov_rb_bend_y || bone_ov_rb_bend_z || bone_ov_pull ||
      bone_ov_gravity || bone_ov_wind || bone_ov_turbulence || bone_ov_fixed;
  if (bone_ov_transition_follow) {
    transition_follow_weights[selected_bone_name] = bone_transition_follow;
  } else {
    transition_follow_weights.erase(selected_bone_name);
  }
  if (!any_mapper_override) {
    bone_mapper.setBoneConfig(selected_bone_name, nullptr);
  } else {
    bone_mapper.setBoneConfig(selected_bone_name, &cfg);
  }
  per_bone_draft_dirty = false;
  invalidatePhysicsArtifacts(InvalidationReason::PerBoneApplied,
                             "Applied per-bone config: " +
                                 selected_bone_name);
  return true;
}

void AppSession::clearSelectedBoneConfig() {
  if (selected_bone_name.empty()) {
    return;
  }
  bone_mapper.setBoneConfig(selected_bone_name, nullptr);
  transition_follow_weights.erase(selected_bone_name);
  loadSelectedBoneEditors();
  invalidatePhysicsArtifacts(InvalidationReason::PerBoneApplied,
                             "Cleared per-bone overrides for " +
                                 selected_bone_name);
}

void AppSession::markSelectedBoneDraftDirty() {
  per_bone_draft_dirty = true;
  advanceGeneration(per_bone_edit_revision);
  invalidatePhysicsArtifacts(InvalidationReason::PerBoneDraft,
                             "Unapplied per-bone parameters");
}

void AppSession::discardSelectedBoneDraft() { loadSelectedBoneEditors(); }

bool AppSession::applyPendingPerBoneDraft() {
  return !per_bone_draft_dirty || applySelectedBoneConfig();
}

const loader::Animation *AppSession::resolvedTransitionTargetAnimation() const {
  if (selected_animation == nullptr) {
    return nullptr;
  }
  if (!transition_target_animation_name.empty()) {
    const auto target =
        animation_root.animations.find(transition_target_animation_name);
    if (target != animation_root.animations.end()) {
      return &target->second;
    }
  }
  return selected_animation;
}

std::vector<loader::MolangBakeWarning>
AppSession::collectPendingMolangWarnings() const {
  std::vector<loader::MolangBakeWarning> warnings;
  if (selected_animation == nullptr) {
    return warnings;
  }
  const auto dependencies = bone_mapper.animationInputDependencyBones();
  const auto append = [&](const loader::Animation *animation,
                          loader::MolangAnimationRole role,
                          const std::string &animation_name) {
    auto found = loader::MolangKeyframeDetector::findBakeWarnings(
        animation, role, animation_name, dependencies,
        bone_mapper.physicsBones());
    warnings.insert(warnings.end(), found.begin(), found.end());
  };
  append(selected_animation, loader::MolangAnimationRole::Source,
         selected_animation_name);

  const loader::Animation *target = resolvedTransitionTargetAnimation();
  const bool target_is_active =
      target != nullptr && target != selected_animation &&
      (transition_mode == 2 ||
       (transition_mode == 1 && transition_duration > 0.0f));
  if (target_is_active) {
    append(target, loader::MolangAnimationRole::TransitionTarget,
           transition_target_animation_name);
  }
  return warnings;
}

void AppSession::clearCommittedPhysicsArtifacts(bool reset_to_source_start) {
  live_execution_.reset();
  final_execution_.reset();
  live_frame_.reset();
  final_result_.reset();
  preview_sample_scratch_.reset();
  empty_export_preflight_ = {};
  force_export_confirmation_pending = false;
  pending_export_animation_path_.reset();
  presentation_mode = PresentationMode::SourcePreview;
  playback_state = PlaybackState::Paused;
  preview_frame_index = 0;
  if (reset_to_source_start) {
    preview_time = 0.0;
  }
}

void AppSession::invalidatePhysicsArtifacts(InvalidationReason reason,
                                            const std::string &message,
                                            bool reset_to_source_start) {
  explicit_cancel_requested_ = false;
  advanceGeneration(physics_generation_);
  active_job_fingerprint_ = {};
  if (bake_thread && bake_thread->joinable()) {
    bake_thread->request_stop();
  }
  const bool cancelling = bake_busy.load();
  clearCommittedPhysicsArtifacts(reset_to_source_start);
  bake_current = 0;
  bake_total = 0;
  bake_message = message;
  status = message;
  bake_state = cancelling ? BakeState::Cancelling
                          : reason == InvalidationReason::Reset
                                ? BakeState::Idle
                                : BakeState::Invalid;
  worker_phase = cancelling ? worker_phase : WorkerPhase::Finished;
  molang_confirmation_pending = false;
  pending_molang_warnings.clear();
  molang_approved_once_ = false;
}

BakeJobInput AppSession::makeBakeJobInput(
    bool allow_input_molang_zero,
    bool allow_selected_molang_zero) const {
  if (selected_animation == nullptr) {
    throw std::logic_error("no source animation is selected");
  }
  BakeJobInput input;
  input.mapper = bone_mapper;
  input.model_generation = model_generation_;
  input.animation_generation = animation_generation_;
  input.source_animation = *selected_animation;
  input.source_animation_name = selected_animation_name;
  if (const auto *target = resolvedTransitionTargetAnimation();
      target != nullptr && transition_mode != 0) {
    input.transition_target_animation = *target;
    input.transition_target_animation_name =
        transition_target_animation_name.empty()
            ? selected_animation_name
            : transition_target_animation_name;
  }
  input.transition_mode = transition_mode;
  input.transition_duration =
      std::max(0.0, static_cast<double>(transition_duration));
  input.transition_source_exit =
      std::max(0.0, static_cast<double>(transition_source_exit));
  input.transition_target_entry =
      std::max(0.0, static_cast<double>(transition_target_entry));
  input.transition_follow_weights = transition_follow_weights;
  input.timing.output_fps = output_fps;
  (void)input.timing.nominalOutputDt();
  input.allow_input_molang_zero = allow_input_molang_zero;
  input.allow_selected_molang_zero = allow_selected_molang_zero;
  input.mapper.config().allow_input_only_molang_zero_fallback =
      allow_input_molang_zero;
  input.mapper.config().allow_selected_molang_zero_fallback =
      allow_selected_molang_zero;
  input.fingerprint = fingerprintFor(input);
  return input;
}

SessionFingerprint AppSession::currentSessionFingerprint() const {
  if (selected_animation == nullptr) {
    return {};
  }
  return makeBakeJobInput(false, false).fingerprint;
}

void AppSession::startBake() {
  if (bake_busy) {
    return;
  }
  if (bone_mapper.physicsBones().empty()) {
    status = "Select at least one physics bone";
    return;
  }
  if (selected_animation == nullptr) {
    status = "Load and select an animation";
    return;
  }
  if (!applyPendingPerBoneDraft()) {
    status = "Bake blocked: invalid unapplied per-bone parameters";
    return;
  }
  applyUiToConfig();

  const auto warnings = collectPendingMolangWarnings();
  const bool approved =
      molang_approved_once_ && warnings == pending_molang_warnings;

  molang_approved_once_ = false;
  if (!warnings.empty() && !approved) {
    pending_molang_warnings = warnings;
    molang_confirmation_pending = true;
    status = "Molang bake confirmation required";
    return;
  }
  molang_confirmation_pending = false;
  pending_molang_warnings.clear();
  const bool allow_input_molang_zero =
      approved &&
      std::any_of(warnings.begin(), warnings.end(), [](const auto &warning) {
        return warning.action == loader::MolangBakeAction::SampleAsZeroPreserve;
      });
  const bool allow_selected_molang_zero =
      approved &&
      std::any_of(warnings.begin(), warnings.end(), [](const auto &warning) {
        return warning.action ==
               loader::MolangBakeAction::OverwriteWithBakedKeys;
      });
  try {
    BakeJobInput job =
        makeBakeJobInput(allow_input_molang_zero, allow_selected_molang_zero);
    advanceGeneration(physics_generation_);
    job.generation = physics_generation_;
    active_job_fingerprint_ = job.fingerprint;

    std::unique_ptr<BakeExecutionState> execution;
    const bool continue_live =
        live_execution_ != nullptr &&
        live_execution_->input.fingerprint == job.fingerprint;
    if (continue_live) {
      execution = std::move(live_execution_);
      execution->input.generation = job.generation;
    } else {
      execution = std::make_unique<BakeExecutionState>(std::move(job));
      live_execution_.reset();
      live_frame_.reset();
      presentation_mode = PresentationMode::SourcePreview;
    }
    final_execution_.reset();
    final_result_.reset();
    explicit_cancel_requested_ = false;
    bake_busy = true;
    bake_state = BakeState::Running;
    worker_phase = WorkerPhase::Preparing;
    bake_message = "Baking…";
    status = "Baking…";
    startWorker(std::move(execution));
  } catch (const std::exception &error) {
    last_error = error.what();
    worker_mailbox_.reset();
    bake_thread.reset();
    bake_busy = false;
    clearCommittedPhysicsArtifacts(true);
    bake_state = BakeState::Failed;
    worker_phase = WorkerPhase::Finished;
    status = std::string("Bake failed: ") + error.what();
    bake_message = status;
  }
}

void AppSession::startWorker(
    std::unique_ptr<BakeExecutionState> execution) {
  worker_mailbox_ = std::make_shared<BakeWorkerMailbox>();
  worker_mailbox_->current = execution->baker->currentStep();
  worker_mailbox_->total = execution->baker->totalSteps();
  const auto mailbox = worker_mailbox_;
  bake_thread.emplace(
      [execution = std::move(execution), mailbox](std::stop_token stop) mutable {
        runBakeWorker(stop, std::move(execution), mailbox);
      });
}

void AppSession::confirmMolangBake(bool proceed) {
  if (!molang_confirmation_pending) {
    return;
  }
  if (!proceed) {
    molang_confirmation_pending = false;
    pending_molang_warnings.clear();
    molang_approved_once_ = false;
    status = "Bake cancelled: Molang handling was not approved";
    return;
  }
  molang_confirmation_pending = false;
  molang_approved_once_ = true;
  startBake();
}

void AppSession::cancelBake() {
  if (!bake_busy.load()) {
    pollBakeProgress();
    return;
  }
  explicit_cancel_requested_ = true;
  advanceGeneration(physics_generation_);
  active_job_fingerprint_ = {};
  if (bake_thread && bake_thread->joinable()) {
    bake_thread->request_stop();
  }
  bake_state = BakeState::Cancelling;
  status = "Cancelling…";
  bake_message = status;
}

void AppSession::shutdownBakeWorker() {
  if (bake_thread && bake_thread->joinable()) {
    bake_thread->request_stop();
    bake_thread->join();
  }
  bake_thread.reset();
  worker_mailbox_.reset();
  bake_busy = false;
  worker_phase = WorkerPhase::Finished;
}

void AppSession::stepBake() {
  if (bake_busy) {
    return;
  }
  try {
    if (!applyPendingPerBoneDraft()) {
      status = "Step blocked: invalid unapplied per-bone parameters";
      return;
    }
    applyUiToConfig();
    if (selected_animation == nullptr || bone_mapper.physicsBones().empty()) {
      status = "Need model physics bones + animation";
      return;
    }
    BakeJobInput job = makeBakeJobInput(false, false);
    const bool matches_live =
        live_execution_ != nullptr &&
        live_execution_->input.fingerprint == job.fingerprint;
    if (!matches_live) {
      advanceGeneration(physics_generation_);
      job.generation = physics_generation_;
      live_execution_ =
          std::make_unique<BakeExecutionState>(std::move(job));
      live_execution_->baker->initialize();
      live_execution_->initialized = true;
    }
    auto &execution = *live_execution_;
    if (execution.baker->currentStep() < execution.baker->totalSteps()) {
      execution.baker->step();
    }
    bake_current = execution.baker->currentStep();
    bake_total = execution.baker->totalSteps();
    LiveSimulationFrame live;
    live.fingerprint = execution.input.fingerprint;
    live.current_step = execution.baker->currentStep();
    live.total_steps = execution.baker->totalSteps();
    live.sample_time = execution.baker->currentSampleTime();
    live.output_frame_interval = execution.baker->outputFrameInterval();
    live.frame = execution.baker->captureCurrentFrameForPreview();
    if (const auto *reference =
            execution.outputReferenceAnimation()) {
      live.reference_animation =
          std::make_shared<const loader::Animation>(*reference);
    }
    live.reference_time =
        execution.baker->getOutputReferenceTime(live.frame.time);
    live.collision.current_contact_count =
        execution.baker->getCurrentRigidBodyContactCount();
    live.collision.maximum_contact_count =
        execution.baker->getMaximumRigidBodyContactCount();
    live.collision.maximum_penetration =
        execution.baker->getMaximumRuntimeRigidBodyPenetration();
    live.substeps = execution.baker->getFixedSubstepStats();
    live_frame_ = std::move(live);
    final_execution_.reset();
    final_result_.reset();
    presentation_mode = PresentationMode::LiveSimulation;
    playback_state = PlaybackState::Paused;
    bake_state = execution.baker->currentStep() >=
                         execution.baker->totalSteps()
                     ? BakeState::AwaitingFinalize
                     : BakeState::Initialized;
    if (bake_state == BakeState::AwaitingFinalize) {
      status = "Steps complete — press Bake (or finalize via full bake) for "
               "export audit";
    } else {
      status = "Stepped " + std::to_string(bake_current.load()) + " / " +
               std::to_string(bake_total.load()) +
               execution.compatibilityStatusSuffix();
    }
  } catch (const std::exception &e) {
    last_error = e.what();
    status = std::string("Step failed: ") + e.what();
    live_execution_.reset();
    live_frame_.reset();
    presentation_mode = PresentationMode::SourcePreview;
    bake_state = BakeState::Failed;
  }
}

void AppSession::resetBake() {
  invalidatePhysicsArtifacts(InvalidationReason::Reset, "Reset");
}

bool AppSession::hasCompleteBake() const {
  return final_execution_ != nullptr && final_result_.has_value() &&
         final_result_->terminal_state == BakeState::Completed &&
         final_result_->frames != nullptr && !final_result_->frames->empty();
}

bool AppSession::exportAnimation(const std::filesystem::path &path) {
  return exportAnimationInternal(path, false);
}

void AppSession::requestAnimationExport(const std::filesystem::path &path) {
  if (canExportAnimation()) {
    exportAnimation(path);
    return;
  }
  if (!canForceExportAnimation()) {
    exportAnimation(path);
    return;
  }
  pending_export_animation_path_ = path;
  force_export_confirmation_pending = true;
  status = "Animation export requires safety confirmation";
}

void AppSession::confirmAnimationExport(bool proceed) {
  if (!force_export_confirmation_pending ||
      !pending_export_animation_path_) {
    return;
  }
  const auto path = std::move(*pending_export_animation_path_);
  pending_export_animation_path_.reset();
  force_export_confirmation_pending = false;
  if (!proceed) {
    status = "Force animation export cancelled";
    return;
  }
  exportAnimationInternal(path, true);
}

bool AppSession::exportAnimationInternal(const std::filesystem::path &path,
                                         bool force_unsafe) {
  if (!canExportAnimation() &&
      (!force_unsafe || !canForceExportAnimation())) {
    status = exportPreflight().block_reasons.empty()
                 ? "Animation export blocked"
                 : "Animation export blocked: " +
                       exportPreflight().block_reasons.front().detail;
    return false;
  }
  try {
    auto &execution = *final_execution_;
    if (!force_unsafe) {
      execution.baker->requireSafeForExport();
    }
    const loader::Animation *reference =
        execution.outputReferenceAnimation();
    if (reference == nullptr) {
      status = "No reference animation for export";
      return false;
    }
    const std::string id = execution.input.source_animation_name.empty()
                               ? "animation.xpbd.baked"
                               : execution.input.source_animation_name +
                                     ".baked";
    std::optional<export_::TransitionReferenceExport> transition_reference;
    if (execution.baker->isTransitionBake()) {
      execution.baker->requireTransitionReferenceExportable();
      transition_reference.emplace();
      transition_reference->sample_pose = [&execution](double t) {
        return execution.baker->sampleOutputReferencePoses(t);
      };
      transition_reference->dependency_bones =
          execution.baker->getOutputReferenceDependencyBones();
      transition_reference->model_bones = &execution.input.mapper.allBones();
    }
    export_::AnimationExporter::exportAnimation(
        id, reference, *final_result_->frames,
        execution.baker->getOutputLoopBehavior(), path,
        transition_reference ? &*transition_reference : nullptr,
        execution.baker->isTransitionBake());
    status = force_unsafe
                 ? "Warning: force-exported " + path.filename().string() +
                       " despite safety preflight"
                 : "Exported " + path.filename().string();
    return true;
  } catch (const std::exception &e) {
    last_error = e.what();
    status = std::string("Export failed: ") + e.what();
    return false;
  }
}

bool AppSession::exportVelocity(const std::filesystem::path &path) {
  if (!canExportVelocity()) {
    status = exportPreflight().block_reasons.empty()
                 ? "Velocity export blocked"
                 : "Velocity export blocked: " +
                       exportPreflight().block_reasons.front().detail;
    return false;
  }
  try {
    auto &execution = *final_execution_;
    execution.baker->requireSafeForExport();
    const std::string id = execution.input.source_animation_name.empty()
                               ? "animation.xpbd.baked"
                               : execution.input.source_animation_name;
    const double dt = execution.baker->outputFrameInterval();
    export_::VelocityCacheExporter::exportCache(id, *final_result_->frames, dt,
                                                path,
                                                execution.input.mapper.config()
                                                            .simulation_mode ==
                                                        baker::BoneMapper::
                                                            SimulationMode::
                                                                RigidBody
                                                    ? "Bullet"
                                                    : "XPBD");
    status = "Exported velocity " + path.filename().string();
    return true;
  } catch (const std::exception &e) {
    last_error = e.what();
    status = std::string("Velocity export failed: ") + e.what();
    return false;
  }
}

bool AppSession::exportDiagnostics(const std::filesystem::path &path) {
  if (!final_result_) {
    status = "No committed bake diagnostics to export";
    return false;
  }
  try {
    const auto &d = final_result_->diagnostics;
    const auto &p = final_result_->export_preflight;
    nlohmann::json json;
    const auto transformJson = [](const rigidbody::Transform &transform) {
      return nlohmann::json{
          {"translation", transform.translation},
          {"rotation_xyzw", transform.rotation}};
    };
    json["generation"] = final_result_->generation;
    json["bullet_safety_applicable"] = d.bullet_safety_applicable;
    json["session_fingerprint"] = d.fingerprint.hex();
    json["worker_terminal_state"] =
        static_cast<int>(final_result_->terminal_state);
    json["timing"] = {{"output_fps", d.timing.output_fps},
                      {"nominal_output_dt", d.timing.nominal_output_dt},
                      {"effective_output_dt", d.timing.effective_output_dt},
                      {"resumed_from_step", d.timing.resumed_from_step},
                      {"total_steps", d.timing.total_steps},
                      {"completed_steps", d.timing.completed_steps}};
    if (d.runtime_fingerprint) {
      const auto &runtime = *d.runtime_fingerprint;
      json["runtime_fingerprint"] = {
          {"schema_version", runtime.schema_version},
          {"stable_name_hash_version", runtime.stable_name_hash_version},
          {"bake_fps", runtime.bake_fps},
          {"fixed_substeps", runtime.fixed_substeps},
          {"physics_dt", runtime.physics_dt},
          {"substep_dt", runtime.substep_dt},
          {"snapshot_level",
           rigidbody::snapshotLevelName(runtime.snapshot_level)},
          {"bullet_version", runtime.bullet_version},
          {"solver_thread_count", runtime.solver_thread_count},
          {"solver_iterations", runtime.solver_iterations},
          {"fast_math_enabled", runtime.fast_math_enabled},
          {"floating_point_mode", runtime.floating_point_mode},
          {"unit_scale", runtime.unit_scale},
          {"linear_damping", runtime.linear_damping},
          {"angular_damping", runtime.angular_damping},
          {"gravity_y", runtime.gravity_y},
          {"real_gravity_field", runtime.real_gravity_field},
          {"ground_collision_enabled", runtime.ground_collision_enabled},
          {"wind_speed", runtime.wind_speed},
          {"wind_direction_degrees", runtime.wind_direction_degrees},
          {"wind_elevation_degrees", runtime.wind_elevation_degrees},
          {"use_wind_components", runtime.use_wind_components},
          {"wind_components", runtime.wind_components},
          {"movement_speed", runtime.movement_speed},
          {"movement_direction_degrees",
           runtime.movement_direction_degrees},
          {"movement_elevation_degrees",
           runtime.movement_elevation_degrees},
          {"air_drag", runtime.air_drag},
          {"turbulence", runtime.turbulence},
          {"shape_topology", nlohmann::json::array()}};
      for (const auto &body : runtime.shape_topology) {
        nlohmann::json value = {
            {"name", body.name},
            {"motion_type", rigidbody::motionTypeName(body.motion_type)},
            {"physics_body", body.physics_body},
            {"compound_shape", body.compound_shape},
            {"boxes", nlohmann::json::array()}};
        for (const auto &box : body.boxes) {
          value["boxes"].push_back(
              {{"half_extents", box.half_extents},
               {"local_transform", transformJson(box.local_transform)}});
        }
        json["runtime_fingerprint"]["shape_topology"].push_back(
            std::move(value));
      }
    } else {
      json["runtime_fingerprint"] = nullptr;
    }
    if (d.step_trace) {
      const auto &trace = *d.step_trace;
      json["step_trace"] = {
          {"snapshot_level",
           rigidbody::snapshotLevelName(trace.snapshot_level)},
          {"enabled", trace.enabled},
          {"capacity", trace.capacity},
          {"captured_sample_count", trace.captured_sample_count},
          {"dropped_sample_count", trace.dropped_sample_count},
          {"samples", nlohmann::json::array()}};
      for (const auto &sample : trace.samples) {
        nlohmann::json value = {
            {"output_step_index", sample.output_step_index},
            {"substep_index", sample.substep_index},
            {"sample_time", sample.sample_time},
            {"physics_dt", sample.physics_dt},
            {"substep_dt", sample.substep_dt},
            {"bodies", nlohmann::json::array()},
            {"contacts", nlohmann::json::array()},
            {"contacts_truncated", sample.contacts_truncated},
            {"joint_errors", nlohmann::json::array()},
            {"contact_summary",
             {{"contact_count", sample.contact_summary.contact_count},
              {"maximum_penetration",
               sample.contact_summary.maximum_penetration},
              {"ground_contacts", sample.contact_summary.ground_contacts},
              {"self_contacts", sample.contact_summary.self_contacts},
              {"warning_threshold",
               sample.contact_summary.warning_threshold},
              {"failure_threshold",
               sample.contact_summary.failure_threshold},
              {"warning", sample.contact_summary.warning},
              {"unsafe", sample.contact_summary.unsafe}}}};
        if (sample.contact_summary.worst_pair) {
          value["contact_summary"]["worst_pair"] = {
              sample.contact_summary.worst_pair->first.name,
              sample.contact_summary.worst_pair->second.name};
        } else {
          value["contact_summary"]["worst_pair"] = nullptr;
        }
        for (const auto &body : sample.bodies) {
          value["bodies"].push_back(
              {{"name", body.name},
               {"motion_type", rigidbody::motionTypeName(body.motion_type)},
               {"physics_body", body.physics_body},
               {"driver_pivot", transformJson(body.driver_bone_transform)},
               {"simulated_pivot",
                transformJson(body.simulated.bone_transform)},
               {"simulated_com", transformJson(body.simulated.com_transform)},
               {"pivot_linear_velocity",
                body.simulated.bone_linear_velocity},
               {"com_linear_velocity", body.simulated.com_linear_velocity},
               {"angular_velocity", body.simulated.angular_velocity}});
        }
        for (const auto &contact : sample.contacts) {
          value["contacts"].push_back(
              {{"body_a", contact.body_a.name},
               {"body_b", contact.body_b.name},
               {"penetration", contact.penetration},
               {"combined_friction", contact.combined_friction},
               {"combined_restitution", contact.combined_restitution}});
        }
        for (const auto &joint : sample.joint_errors) {
          value["joint_errors"].push_back(
              {{"parent", joint.parent_body},
               {"child", joint.child_body},
               {"anchor_separation", joint.anchor_separation},
               {"relative_angles", joint.relative_angles},
               {"angular_excess", joint.angular_excess},
               {"maximum_angular_excess", joint.maximum_angular_excess}});
        }
        json["step_trace"]["samples"].push_back(std::move(value));
      }
    } else {
      json["step_trace"] = nullptr;
    }
    if (d.bullet_safety_applicable && d.initial_collision) {
      json["initial_collision"] =
          {{"contact_count", d.initial_collision->contact_count},
           {"maximum_penetration",
            d.initial_collision->maximum_penetration},
           {"ground_contacts", d.initial_collision->ground_contacts},
           {"self_contacts", d.initial_collision->self_contacts},
           {"warning_threshold", d.initial_collision->warning_threshold},
           {"failure_threshold", d.initial_collision->failure_threshold},
           {"warning", d.initial_collision->warning},
           {"unsafe", d.initial_collision->unsafe}};
      if (d.initial_collision->worst_pair) {
        json["initial_collision"]["worst_pair"] =
            {d.initial_collision->worst_pair->first.name,
             d.initial_collision->worst_pair->second.name};
      }
    } else {
      json["initial_collision"] = nullptr;
    }
    if (d.bullet_safety_applicable) {
      json["runtime_collision"] =
          {{"current_contact_count", d.runtime_collision.current_contact_count},
           {"maximum_contact_count", d.runtime_collision.maximum_contact_count},
           {"maximum_penetration", d.runtime_collision.maximum_penetration}};
    } else {
      json["runtime_collision"] = nullptr;
    }
    json["velocity"] =
        {{"available", d.velocity.available},
         {"maximum_linear_speed", d.velocity.maximum_linear_speed},
         {"maximum_speed_bone", d.velocity.maximum_speed_bone},
         {"maximum_speed_frame", d.velocity.maximum_speed_frame},
         {"maximum_frame_velocity_jump",
          d.velocity.maximum_frame_velocity_jump},
         {"maximum_jump_bone", d.velocity.maximum_jump_bone},
         {"maximum_jump_frame", d.velocity.maximum_jump_frame}};
    if (d.substeps) {
      nlohmann::json fixedSubsteps =
          {{"configured_steps", d.substeps->configured_minimum},
           {"last_executed", d.substeps->last_effective},
           {"maximum_executed", d.substeps->maximum_effective},
           {"average_executed", d.substeps->average_effective},
           {"insufficient_step_risk_count",
            d.substeps->insufficient_step_risk_count},
           {"maximum_recommended", d.substeps->maximum_recommended},
           {"last_kinematic_recommended",
            d.substeps->last_kinematic_required},
           {"last_dynamic_recommended",
            d.substeps->last_dynamic_required},
           {"maximum_dynamic_recommended",
            d.substeps->maximum_dynamic_required},
           {"configured_minimum", d.substeps->configured_minimum},
           {"last_effective", d.substeps->last_effective},
           {"maximum_effective", d.substeps->maximum_effective},
           {"average_effective", d.substeps->average_effective},
           {"output_step_count", d.substeps->output_step_count},
           {"raised_step_count", d.substeps->raised_step_count},
           {"worst_body", d.substeps->worst_body},
           {"worst_motion_source", d.substeps->worst_motion_source},
           {"last_kinematic_required",
            d.substeps->last_kinematic_required},
           {"last_dynamic_required", d.substeps->last_dynamic_required},
           {"maximum_dynamic_required",
            d.substeps->maximum_dynamic_required},
           {"worst_minimum_half_extent",
            d.substeps->worst_minimum_half_extent},
           {"worst_maximum_radius", d.substeps->worst_maximum_radius},
           {"worst_equivalent_linear_travel",
            d.substeps->worst_equivalent_linear_travel},
           {"worst_equivalent_angular_travel",
            d.substeps->worst_equivalent_angular_travel},
           {"worst_acceleration", d.substeps->worst_acceleration},
           {"required_before_failure",
            d.substeps->required_before_failure},
           {"hard_limit", d.substeps->hard_limit}};
      if (d.substeps->worst_frame) {
        fixedSubsteps["worst_frame"] = *d.substeps->worst_frame;
      }
      json["fixed_substeps"] = fixedSubsteps;


      json["adaptive_substeps"] = std::move(fixedSubsteps);
    } else {
      json["fixed_substeps"] = nullptr;
      json["adaptive_substeps"] = nullptr;
    }
    if (d.collider_preflight) {
      const auto &colliders = *d.collider_preflight;
      json["collider_preflight"] = {
          {"unit_scale", colliders.unit_scale},
          {"warning_count", colliders.warning_count},
          {"error_count", colliders.error_count},
          {"observed_minimum_half_extent",
           colliders.observed_minimum_half_extent},
          {"observed_maximum_half_extent",
           colliders.observed_maximum_half_extent},
          {"observed_maximum_aspect_ratio",
           colliders.observed_maximum_aspect_ratio},
          {"thresholds",
           {{"minimum_half_extent_warning",
             colliders.minimum_half_extent_warning},
            {"minimum_half_extent_error",
             colliders.minimum_half_extent_error},
            {"aspect_ratio_warning", colliders.aspect_ratio_warning},
            {"aspect_ratio_error", colliders.aspect_ratio_error},
            {"margin_ratio_warning_minimum",
             colliders.margin_ratio_warning_minimum},
            {"margin_ratio_warning_maximum",
             colliders.margin_ratio_warning_maximum}}},
          {"colliders", nlohmann::json::array()}};
      for (const auto &collider : colliders.colliders) {
        json["collider_preflight"]["colliders"].push_back(
            {{"body", collider.body_name},
             {"box_index", collider.box_index},
             {"bullet_half_extents", collider.bullet_half_extents},
             {"unit_scale", collider.unit_scale},
             {"minimum_half_extent", collider.minimum_half_extent},
             {"maximum_half_extent", collider.maximum_half_extent},
             {"aspect_ratio", collider.aspect_ratio},
             {"collision_margin", collider.collision_margin},
             {"margin_to_minimum_half_extent",
              collider.margin_to_minimum_half_extent},
             {"ccd_enabled", collider.ccd_enabled},
             {"ccd_radius", collider.ccd_radius},
             {"risk", rigidbody::colliderRiskLevelName(collider.risk)},
             {"issues", collider.issues}});
      }
    } else {
      json["collider_preflight"] = nullptr;
    }
    if (d.kinematic_history) {
      json["kinematic_history"] = {
          {"continuous_updates", d.kinematic_history->continuous_updates},
          {"periodic_updates", d.kinematic_history->periodic_updates},
          {"teleport_resets", d.kinematic_history->teleport_resets},
          {"detected_discontinuities",
           d.kinematic_history->detected_discontinuities},
          {"rejected_discontinuities",
           d.kinematic_history->rejected_discontinuities},
          {"insufficient_step_risks",
           d.kinematic_history->insufficient_step_risks},
          {"last_teleport_body", d.kinematic_history->last_teleport_body},
          {"last_step_risk_body",
           d.kinematic_history->last_step_risk_body},
          {"last_reason", rigidbody::kinematicDiscontinuityReasonName(
                              d.kinematic_history->last_reason)},
          {"last_pivot_delta", d.kinematic_history->last_pivot_delta},
          {"last_com_delta", d.kinematic_history->last_com_delta},
          {"last_angular_delta_radians",
           d.kinematic_history->last_angular_delta_radians},
          {"last_equivalent_travel",
           d.kinematic_history->last_equivalent_travel},
          {"last_detection_threshold",
           d.kinematic_history->last_detection_threshold},
          {"last_fixed_dt", d.kinematic_history->last_fixed_dt}};
    } else {
      json["kinematic_history"] = nullptr;
    }
    if (d.joint_preflight) {
      json["joint_preflight"] = {
          {"rotation_order", d.joint_preflight->rotation_order},
          {"maximum_safe_y_limit_degrees",
           d.joint_preflight->maximum_safe_y_limit_degrees},
          {"near_half_turn_warning_degrees",
           d.joint_preflight->near_half_turn_warning_degrees},
          {"warnings", nlohmann::json::array()}};
      for (const auto &warning : d.joint_preflight->warnings) {
        json["joint_preflight"]["warnings"].push_back(
            {{"parent", warning.parent_body},
             {"child", warning.child_body},
             {"axis", warning.axis},
             {"limit_degrees", warning.limit_degrees}});
      }
    } else {
      json["joint_preflight"] = nullptr;
    }
    if (d.joint_spring) {
      json["joint_spring"] = {
          {"requested_stiffness", d.joint_spring->requested_stiffness},
          {"requested_damping", d.joint_spring->requested_damping},
          {"constructed_joint_count",
           d.joint_spring->constructed_joint_count},
          {"active_spring_joint_count",
           d.joint_spring->active_spring_joint_count},
          {"active_spring_axis_count",
           d.joint_spring->active_spring_axis_count},
          {"solver_iterations", d.joint_spring->solver_iterations},
          {"configured_fixed_substeps",
           d.joint_spring->configured_fixed_substeps},
          {"configured_minimum_substeps",
           d.joint_spring->configured_minimum_substeps},
          {"bullet_stability_limiting_enabled",
           d.joint_spring->bullet_stability_limiting_enabled},
          {"solver_constraint", d.joint_spring->solver_constraint},
          {"effective_solver_behavior",
           d.joint_spring->effective_behavior}};
    } else {
      json["joint_spring"] = nullptr;
    }
    if (d.bullet_safety_applicable) {
      json["joints"] = {
          {"unsafe_final_count", d.joints.unsafe_final_count},
          {"maximum_anchor_separation", d.joints.maximum_anchor_separation},
          {"maximum_angular_excess_radians",
           d.joints.maximum_angular_excess_radians},
          {"worst_linear_parent", d.joints.worst_linear_parent},
          {"worst_linear_child", d.joints.worst_linear_child},
          {"worst_angular_parent", d.joints.worst_angular_parent},
          {"worst_angular_child", d.joints.worst_angular_child},
          {"worst_angular_axis", d.joints.worst_angular_axis}};
      if (d.joints.euler_singularity) {
        json["joints"]["euler_singularity"] = {
            {"joint_euler_singular", true},
            {"parent", d.joints.euler_singularity->parent_bone},
            {"child", d.joints.euler_singularity->child_bone},
            {"relative_rotation_xyzw",
             d.joints.euler_singularity->relative_rotation_xyzw},
            {"rotation_order", d.joints.euler_singularity->rotation_order}};
      } else {
        json["joints"]["euler_singularity"] = nullptr;
      }
    } else {
      json["joints"] = nullptr;
    }
    json["loop"] =
        {{"source_policy", d.loop.source_policy},
         {"converged", d.loop.converged},
         {"fallback_used", d.loop.fallback_used},
         {"completed_cycles", d.loop.completed_cycles},
         {"selected_cycle", d.loop.selected_cycle
                                ? nlohmann::json(*d.loop.selected_cycle)
                                : nlohmann::json(nullptr)},
         {"best_cycle_score", d.loop.best_cycle_score
                                  ? nlohmann::json(*d.loop.best_cycle_score)
                                  : nlohmann::json(nullptr)},
         {"seam_strategy", d.loop.seam_strategy},
         {"seam_correction_rejected", d.loop.seam_correction_rejected},
         {"configured_seam_window_ratio",
          d.loop.configured_seam_window_ratio},
         {"configured_seam_window_seconds",
          d.loop.configured_seam_window_seconds},
         {"effective_seam_window_seconds",
          d.loop.effective_seam_window_seconds},
         {"effective_seam_window_ratio",
          d.loop.effective_seam_window_ratio},
         {"maximum_position_error", d.loop.maximum_position_error},
         {"position_bone", d.loop.position_bone},
         {"maximum_rotation_error_radians",
          d.loop.maximum_rotation_error_radians},
         {"rotation_bone", d.loop.rotation_bone},
         {"maximum_linear_velocity_error",
          d.loop.maximum_linear_velocity_error},
         {"linear_velocity_bone", d.loop.linear_velocity_bone},
         {"maximum_angular_velocity_error",
          d.loop.maximum_angular_velocity_error},
         {"angular_velocity_bone", d.loop.angular_velocity_bone},
         {"seam_position_error", d.loop.seam_position_error},
         {"seam_rotation_error_radians",
          d.loop.seam_rotation_error_radians},
         {"seam_linear_velocity_jump",
          d.loop.seam_linear_velocity_jump},
         {"seam_angular_velocity_jump",
          d.loop.seam_angular_velocity_jump},
         {"seam_linear_acceleration_jump",
          d.loop.seam_linear_acceleration_jump},
         {"seam_angular_acceleration_jump",
          d.loop.seam_angular_acceleration_jump}};
    json["loop"]["physical_state"] = d.loop.physical_state;
    json["loop"]["seam_state"] = d.loop.seam_state;
    json["loop"]["physics_seam_state"] = d.loop.physics_seam_state;
    json["loop"]["driver_state"] = d.loop.driver_state;
    json["loop"]["quantization_state"] = d.loop.quantization_state;
    json["loop"]["collision_state"] = d.loop.collision_state;
    json["loop"]["joint_state"] = d.loop.joint_state;
    json["loop"]["bullet_safety_applicable"] =
        d.bullet_safety_applicable;
    json["loop"]["export_state"] = d.loop.export_state;
    json["loop"]["cycle_valid"] = d.loop.cycle_valid;
    json["loop"]["periodic_boundary"] =
        {{"start", loopBoundaryJson(d.loop.start_boundary)},
         {"end", loopBoundaryJson(d.loop.end_boundary)}};
    json["loop"]["cycle_validation_state"] =
        d.loop.cycle_validation_state;
    json["loop"]["invalid_numeric_bone"] = d.loop.invalid_numeric_bone;
    json["loop"]["invalid_numeric_field"] = d.loop.invalid_numeric_field;
    json["loop"]["seam_valid"] = d.loop.seam_valid;
    json["loop"]["seam_sample_count_sufficient"] =
        d.loop.seam_sample_count_sufficient;
    json["loop"]["seam_verified_continuity_order"] =
        d.loop.seam_verified_continuity_order;
    json["loop"]["physics_relative_available"] =
        d.loop.physics_relative_available;
    json["loop"]["physics_relative_fallback_reason"] =
        d.loop.physics_relative_fallback_reason;
    json["loop"]["missing_bone"] = d.loop.missing_bone;
    json["loop"]["affected_metric_space"] =
        d.loop.affected_metric_space;
    json["loop"]["driver_available"] = d.loop.driver_available;
    json["loop"]["driver_safe"] = d.loop.driver_safe;
    json["loop"]["seam_physics_safe"] = d.loop.seam_physics_safe;
    json["loop"]["seam_quantization_safe"] =
        d.loop.seam_quantization_safe;
    json["loop"]["seam_export_safe"] = d.loop.seam_export_safe;
    json["loop"]["worst_driver_bone"] = d.loop.worst_driver_bone;
    json["loop"]["driver_position_error"] =
        d.loop.driver_position_error;
    json["loop"]["driver_rotation_error_radians"] =
        d.loop.driver_rotation_error_radians;
    json["loop"]["driver_linear_velocity_jump"] =
        d.loop.driver_linear_velocity_jump;
    json["loop"]["driver_angular_velocity_jump"] =
        d.loop.driver_angular_velocity_jump;
    if (d.bullet_safety_applicable) {
      json["loop"]["contact_set_changed"] = d.loop.contact_set_changed;
      json["loop"]["contact_difference_count"] =
          d.loop.contact_difference_count;
      json["loop"]["contact_pair_added_count"] =
          d.loop.contact_pair_added_count;
      json["loop"]["contact_pair_removed_count"] =
          d.loop.contact_pair_removed_count;
      json["loop"]["contact_state_changed_count"] =
          d.loop.contact_state_changed_count;
      json["loop"]["meaningful_penetration_changed_count"] =
          d.loop.meaningful_penetration_changed_count;
      json["loop"]["seam_collision_safe"] =
          d.loop.seam_collision_safe;
      json["loop"]["seam_joint_safe"] = d.loop.seam_joint_safe;
      json["loop"]["cycle_maximum_penetration"] =
          d.loop.cycle_maximum_penetration;
      json["loop"]["cycle_maximum_penetration_time"] =
          d.loop.cycle_maximum_penetration_time >= 0.0
              ? nlohmann::json(d.loop.cycle_maximum_penetration_time)
              : nlohmann::json(nullptr);
      json["loop"]["seam_maximum_penetration"] =
          d.loop.seam_maximum_penetration;
      json["loop"]["configured_loop_seam_penetration_limit"] =
          d.loop.configured_loop_seam_penetration_limit;
    }
    json["loop"]["cycle_candidates"] = nlohmann::json::array();
    for (const auto &candidate : d.loop.cycle_candidates) {
      nlohmann::json value =
          {{"cycle", candidate.cycle},
           {"valid", candidate.valid},
           {"within_tolerances", candidate.within_tolerances},
           {"score", std::isfinite(candidate.score)
                         ? nlohmann::json(candidate.score)
                         : nlohmann::json(nullptr)},
           {"pose_error", candidate.pose_error},
           {"velocity_error", candidate.velocity_error},
           {"selected", candidate.selected},
           {"invalid_reason", candidate.invalid_reason},
           {"rejection_reasons", candidate.rejection_reasons},
           {"periodic_boundary",
            {{"start", loopBoundaryJson(candidate.start_boundary)},
             {"end", loopBoundaryJson(candidate.end_boundary)}}}};
      if (d.bullet_safety_applicable) {
        value["collision_safe"] = candidate.collision_safe;
        value["contact_difference_count"] =
            candidate.contact_difference_count;
        value["maximum_penetration"] = candidate.maximum_penetration;
        value["maximum_penetration_time"] =
            candidate.maximum_penetration_time >= 0.0
                ? nlohmann::json(candidate.maximum_penetration_time)
                : nlohmann::json(nullptr);
      }
      json["loop"]["cycle_candidates"].push_back(std::move(value));
    }
    json["loop"]["seam_windows"] = nlohmann::json::array();
    for (const auto &window : d.loop.seam_windows) {
      nlohmann::json value =
          {{"window_duration_seconds", window.window_duration_seconds},
           {"window_ratio", window.window_ratio},
           {"window_start_time", window.window_start_time},
           {"corrected", window.corrected},
           {"valid", window.valid},
           {"c0", window.c0_pass},
           {"c1", window.c1_pass},
           {"c2", window.c2_pass},
           {"driver", window.driver_pass},
           {"driver_c0", window.driver_c0_pass},
           {"driver_c1", window.driver_c1_pass},
           {"driver_c2", window.driver_c2_pass},
           {"validation_gate", window.validation_pass},
           {"physics_seam_gate", window.physics_seam_pass},
           {"driver_seam_gate", window.driver_seam_pass},
           {"quantization_gate", window.quantization_pass},
           {"collision_gate", window.collision_pass},
           {"joint_gate", window.joint_pass},
           {"export_gate", window.export_pass},
           {"score", std::isfinite(window.score)
                         ? nlohmann::json(window.score)
                         : nlohmann::json(nullptr)},
           {"accepted", window.accepted},
           {"best_preview", window.best_preview},
           {"best_safe_export", window.best_safe_export},
           {"selected_for_output", window.selected_for_output},
           {"canonicalized_bone_count",
            window.canonicalized_bone_count},
           {"preserved_driver_bone_count",
            window.preserved_driver_bone_count},
           {"driver_endpoint_conflict_count",
            window.driver_endpoint_conflict_count},
           {"invalid_item", window.invalid_item},
           {"rejection_reasons", window.rejection_reasons}};
      if (d.bullet_safety_applicable) {
        value["collision"] = window.collision_safe;
        value["joint"] = window.joint_safe;
        value["maximum_penetration"] = window.maximum_penetration;
        value["maximum_penetration_time"] =
            window.maximum_penetration_time;
        value["joint_failure_time"] = window.joint_failure_time;
        value["interpolation_failure_time"] =
            window.interpolation_failure_time;
        value["interpolated_sample_count"] =
            window.interpolated_sample_count;
      }
      json["loop"]["seam_windows"].push_back(std::move(value));
    }
    json["loop"]["anchor_coverage"] = nlohmann::json::array();
    for (const auto &coverage : d.loop.anchor_coverage) {
      json["loop"]["anchor_coverage"].push_back(
          {{"chain_root", coverage.chain_root},
           {"fixed_anchor", coverage.fixed_anchor.empty()
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(coverage.fixed_anchor)},
           {"expected_bone_count", coverage.expected_bone_count},
           {"measured_bone_count", coverage.measured_bone_count},
           {"complete", coverage.complete}});
    }
    json["loop"]["danger_markers"] = nlohmann::json::array();
    for (const auto &marker : d.loop.danger_markers) {
      json["loop"]["danger_markers"].push_back(
          {{"kind", marker.kind},
           {"time", marker.time >= 0.0 ? nlohmann::json(marker.time)
                                        : nlohmann::json(nullptr)},
           {"item", marker.item}});
    }
    json["final_audit"] =
        {{"bullet_safety_applicable", d.bullet_safety_applicable},
         {"numerical_safe", d.final_audit.numerical_safe},
         {"chain_stability", d.chain_stability}};
    if (d.bullet_safety_applicable) {
      json["final_audit"]["collision_safe"] =
          d.final_audit.collision_safe;
      json["final_audit"]["unsafe_collision_count"] =
          d.final_audit.unsafe_collision_count;
      json["final_audit"]["maximum_penetration"] =
          d.final_audit.maximum_penetration;
      json["final_audit"]["joint_safe"] = d.final_audit.joint_safe;
      json["final_audit"]["unsafe_joint_count"] =
          d.final_audit.unsafe_joint_count;
      if (d.final_audit.worst_collision_pair) {
        json["final_audit"]["worst_collision_pair"] =
            {d.final_audit.worst_collision_pair->first,
             d.final_audit.worst_collision_pair->second};
      } else {
        json["final_audit"]["worst_collision_pair"] = nullptr;
      }
    }
    json["determinism"] =
        {{"forcing_algorithm_version",
          d.determinism.forcing_algorithm_version},
         {"stable_name_hash_version",
          d.determinism.stable_name_hash_version},
         {"bullet_solver_seed", d.determinism.bullet_solver_seed},
         {"bullet_version", d.determinism.bullet_version},
         {"simulation_mode", d.determinism.simulation_mode},
         {"timing_fingerprint",
          d.determinism.timing_fingerprint.hex()},
         {"config_fingerprint",
          d.determinism.config_fingerprint.hex()},
         {"content_fingerprint",
          d.determinism.content_fingerprint.hex()}};
    json["export_preflight"]["finalized"] = p.finalized;
    json["export_preflight"]["animation_allowed"] = p.animation_allowed;
    json["export_preflight"]["velocity_allowed"] = p.velocity_allowed;
    json["export_preflight"]["block_reasons"] = nlohmann::json::array();
    for (const auto &reason : p.block_reasons) {
      json["export_preflight"]["block_reasons"].push_back(
          {{"code", static_cast<int>(reason.code)},
           {"detail", reason.detail}});
    }
    json["effective_config"]["global"] = nlohmann::json::array();
    for (const auto &value : d.effective_config.global) {
      json["effective_config"]["global"].push_back(
          {{"name", value.name},
           {"ui", value.ui_value},
           {"committed", value.committed_value},
           {"effective", value.effective_value},
           {"source", value.source},
           {"reason", value.reason}});
    }
    for (const auto &[bone, values] : d.effective_config.per_bone) {
      auto &array = json["effective_config"]["per_bone"][bone];
      array = nlohmann::json::array();
      for (const auto &value : values) {
        array.push_back({{"name", value.name},
                         {"effective", value.effective_value},
                         {"source", value.source},
                         {"reason", value.reason}});
      }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot open diagnostics output");
    }
    stream << json.dump(2) << '\n';
    if (!stream) {
      throw std::runtime_error("failed to write diagnostics output");
    }
    status = "Exported diagnostics " + path.filename().string();
    return true;
  } catch (const std::exception &error) {
    last_error = error.what();
    status = std::string("Diagnostics export failed: ") + error.what();
    return false;
  }
}

bool AppSession::canExportAnimation() const {
  return hasCompleteBake() && !bake_busy.load() &&
         exportPreflight().animation_allowed;
}

bool AppSession::canForceExportAnimation() const {
  if (!hasCompleteBake() || bake_busy.load()) {
    return false;
  }
  const auto &preflight = exportPreflight();
  if (!preflight.finalized || preflight.block_reasons.empty()) {
    return false;
  }
  return std::all_of(
      preflight.block_reasons.begin(), preflight.block_reasons.end(),
      [](const ExportBlockReason &reason) {
        return isForceExportableBlock(reason.code);
      });
}

bool AppSession::canExportVelocity() const {
  return hasCompleteBake() && !bake_busy.load() &&
         exportPreflight().velocity_allowed;
}

const ExportPreflight &AppSession::exportPreflight() const {
  return final_result_ ? final_result_->export_preflight
                       : empty_export_preflight_;
}

const BakeDiagnostics *AppSession::diagnostics() const {
  return final_result_ ? &final_result_->diagnostics : nullptr;
}

const BakeJobResult *AppSession::finalResult() const {
  return final_result_ ? &*final_result_ : nullptr;
}

const LiveSimulationFrame *AppSession::liveSimulationFrame() const {
  return live_frame_ ? &*live_frame_ : nullptr;
}

void AppSession::pollBakeProgress() {
  if (!worker_mailbox_) {
    return;
  }
  const auto mailbox = worker_mailbox_;
  const bool explicit_cancel = explicit_cancel_requested_;
  bake_current = mailbox->current.load();
  bake_total = mailbox->total.load();
  worker_phase = mailbox->phase.load();
  if (!mailbox->finished.load()) {
    return;
  }
  if (bake_thread && bake_thread->joinable()) {
    bake_thread->join();
  }
  bake_thread.reset();

  std::optional<BakeJobResult> result;
  std::unique_ptr<BakeExecutionState> execution;
  {
    std::lock_guard lock(mailbox->mutex);
    result = std::move(mailbox->result);
    execution = std::move(mailbox->completed_execution);
  }
  worker_mailbox_.reset();
  bake_busy = false;
  explicit_cancel_requested_ = false;
  worker_phase = WorkerPhase::Finished;
  if (!result) {
    bake_state = BakeState::Failed;
    status = "Bake failed: worker returned no result";
    return;
  }

  const bool current =
      result->generation == physics_generation_ &&
      result->fingerprint == active_job_fingerprint_;
  if (!current) {
    clearCommittedPhysicsArtifacts(true);
    bake_state = explicit_cancel ? BakeState::Cancelled : BakeState::Invalid;
    status = explicit_cancel ? "Bake cancelled"
                             : "Discarded stale bake result";
    bake_message = status;
    return;
  }

  if (result->terminal_state == BakeState::Completed && execution) {
    final_execution_ = std::move(execution);
    final_result_ = std::move(result);
    preview_sample_scratch_.reset();
    live_execution_.reset();
    live_frame_.reset();
    bake_state = BakeState::Completed;
    presentation_mode = PresentationMode::FinalBakedPreview;
    playback_state = PlaybackState::Playing;
    preview_time = 0.0;
    preview_frame_index = 0;
    status = (exportPreflight().animation_allowed
                  ? "Bake complete"
                  : "Bake complete — export blocked") +
             final_execution_->compatibilityStatusSuffix();
    bake_message = status;
    return;
  }

  last_error = result->error;
  clearCommittedPhysicsArtifacts(true);
  bake_state = result->terminal_state;
  status = result->terminal_state == BakeState::Cancelled
               ? "Bake cancelled"
               : "Bake failed: " + result->error;
  bake_message = status;
}

void AppSession::fitCameraToModel() {
  skeleton_view.setGeometry(geometry.bones.empty() ? nullptr : &geometry);
  skeleton_view.setHiddenBones(&hidden_bone_names);
  skeleton_view.setShowBones(show_bones);
  skeleton_view.setMcbeCoords(use_mcbe_coords);
  std::array<float, 3> center{};
  float radius = 10.0f;
  if (skeleton_view.computeBounds(center, radius)) {
    camera.fit(center, radius);
  } else {
    camera.reset();
  }
  camera_needs_fit = false;
}

void AppSession::setPreviewFrameIndex(int index) {
  if (presentation_mode == PresentationMode::LiveSimulation) {
    preview_frame_index = 0;
    if (live_frame_) {
      preview_time = live_frame_->frame.time;
    }
    return;
  }
  if (presentation_mode != PresentationMode::FinalBakedPreview ||
      !hasCompleteBake()) {
    preview_frame_index = 0;
    return;
  }
  const auto &frames = *final_result_->frames;
  const int n = static_cast<int>(frames.size());
  preview_frame_index = std::clamp(index, 0, n - 1);
  preview_time = frames[static_cast<std::size_t>(preview_frame_index)].time;
}

const baker::BakedFrame *AppSession::currentPreviewFrame() const {
  if (presentation_mode == PresentationMode::LiveSimulation) {
    return live_frame_ ? &live_frame_->frame : nullptr;
  }
  if (presentation_mode != PresentationMode::FinalBakedPreview ||
      !hasCompleteBake()) {
    return nullptr;
  }
  const auto &frames = *final_result_->frames;
  if (frames.empty()) {
    return nullptr;
  }
  double loop_duration = 0.0;
  if (final_execution_->baker->isLooping()) {
    const double source_duration =
        final_execution_->input.source_animation.animation_length;
    if (std::isfinite(source_duration) && source_duration > 0.0) {
      loop_duration = source_duration;
    }
  }
  return &baker::BakedPreviewSampler::sample(
      frames, geometry.bones, preview_time, preview_sample_scratch_,
      loop_duration);
}

std::size_t AppSession::previewFrameCount() const {
  if (presentation_mode == PresentationMode::LiveSimulation) {
    return live_frame_ ? 1U : 0U;
  }
  if (presentation_mode == PresentationMode::FinalBakedPreview &&
      hasCompleteBake()) {
    return final_result_->frames->size();
  }
  return 0U;
}

const loader::Animation *
AppSession::currentPreviewReferenceAnimation() const {
  if (presentation_mode == PresentationMode::LiveSimulation && live_frame_) {
    return live_frame_->reference_animation
               ? live_frame_->reference_animation.get()
               : selected_animation;
  }
  if (presentation_mode == PresentationMode::FinalBakedPreview &&
      hasCompleteBake()) {
    if (const auto *reference =
            final_execution_->outputReferenceAnimation()) {
      return reference;
    }
  }
  return selected_animation;
}

double AppSession::currentPreviewReferenceTime() const {
  if (presentation_mode == PresentationMode::LiveSimulation && live_frame_) {
    return live_frame_->reference_time;
  }
  if (presentation_mode == PresentationMode::FinalBakedPreview &&
      hasCompleteBake()) {
    return final_execution_->baker->getOutputReferenceTime(preview_time);
  }
  return preview_time;
}

void AppSession::setPresentationMode(PresentationMode mode) {
  if (mode == PresentationMode::LiveSimulation && !live_frame_) {
    return;
  }
  if (mode == PresentationMode::FinalBakedPreview && !hasCompleteBake()) {
    return;
  }
  presentation_mode = mode;
  preview_frame_index = 0;
  playback_state = PlaybackState::Paused;
  if (mode == PresentationMode::LiveSimulation) {
    preview_time = live_frame_->frame.time;
  } else if (mode == PresentationMode::FinalBakedPreview) {
    preview_time = final_result_->frames->front().time;
  } else {
    preview_time = 0.0;
  }
}

bool AppSession::canPreview() const {
  switch (presentation_mode) {
  case PresentationMode::LiveSimulation:
    return live_frame_.has_value() && !geometry.bones.empty();
  case PresentationMode::FinalBakedPreview:
    return hasCompleteBake() && !geometry.bones.empty();
  case PresentationMode::SourcePreview:
    return selected_animation != nullptr && !geometry.bones.empty();
  }
  return false;
}

double AppSession::previewLength() const {
  if (presentation_mode == PresentationMode::FinalBakedPreview &&
      hasCompleteBake()) {
    if (final_execution_->baker->isLooping()) {
      const double source_duration =
          final_execution_->input.source_animation.animation_length;
      if (std::isfinite(source_duration) && source_duration > 0.0) {
        return source_duration;
      }
    }
    const auto &frames = *final_result_->frames;
    return std::max(1e-6,
                    frames.back().time - frames.front().time);
  }
  if (presentation_mode == PresentationMode::LiveSimulation && live_frame_) {
    return std::max(1e-6, live_frame_->frame.time);
  }
  if (selected_animation != nullptr) {
    return std::max(1e-6, selected_animation->animation_length);
  }
  return 1.0;
}

void AppSession::togglePreviewPlayback() {
  if (!canPreview()) {
    status = "Load model + animation (or complete a bake) to preview";
    return;
  }
  if (presentation_mode == PresentationMode::LiveSimulation) {
    playback_state = PlaybackState::Paused;
    status = "Live simulation is a stepped snapshot";
    return;
  }
  playback_state = playback_state == PlaybackState::Playing
                       ? PlaybackState::Paused
                       : PlaybackState::Playing;
  if (playback_state == PlaybackState::Playing) {

    const double len = previewLength();
    if (preview_time >= len - 1e-6) {
      preview_time = 0.0;
      preview_frame_index = 0;
    }
    status = presentation_mode == PresentationMode::FinalBakedPreview
                 ? "Playing baked preview"
                 : "Playing source animation";
  } else {
    status = "Preview paused";
  }
}

void AppSession::advancePreview(float dt_seconds) {
  if (playback_state != PlaybackState::Playing || dt_seconds <= 0.0f ||
      !canPreview()) {
    return;
  }

  if (presentation_mode == PresentationMode::LiveSimulation) {
    playback_state = PlaybackState::Paused;
    return;
  }


  if (presentation_mode == PresentationMode::FinalBakedPreview &&
      hasCompleteBake() && final_result_->frames->size() >= 2) {
    const auto &frames = *final_result_->frames;
    const bool looping = final_execution_->baker->isLooping();
    const double end_t = looping ? previewLength() : frames.back().time;
    preview_time += static_cast<double>(dt_seconds);
    if (looping) {
      const double span = end_t;
      while (preview_time >= end_t) {
        preview_time -= span;
      }
      while (preview_time < 0.0) {
        preview_time += span;
      }
    } else if (preview_time >= end_t) {
      preview_time = end_t;
      playback_state = PlaybackState::Paused;
      status = "Preview finished";
    }

    int best = 0;
    double best_d = std::abs(frames[0].time - preview_time);
    for (int i = 1; i < static_cast<int>(frames.size()); ++i) {
      const double d =
          std::abs(frames[static_cast<std::size_t>(i)].time - preview_time);
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    preview_frame_index = best;
    return;
  }


  if (selected_animation == nullptr) {
    playback_state = PlaybackState::Paused;
    return;
  }
  const double length = std::max(1e-6, selected_animation->animation_length);
  preview_time += static_cast<double>(dt_seconds);
  const bool source_loops =
      selected_animation->loop ||
      selected_animation->loop_behavior ==
          loader::Animation::LoopBehavior::Loop;
  const bool looping = loop_mode == 1 || (loop_mode == 0 && source_loops);
  if (looping) {
    while (preview_time >= length) {
      preview_time -= length;
    }
    while (preview_time < 0.0) {
      preview_time += length;
    }
  } else if (preview_time >= length) {
    preview_time = length;
    playback_state = PlaybackState::Paused;
    status = "Source preview finished";
  }
}

render::SkeletonDrawList AppSession::buildViewportDrawList(float view_w,
                                                           float view_h) {
  skeleton_view.setGeometry(geometry.bones.empty() ? nullptr : &geometry);
  skeleton_view.setBoneMapper(&bone_mapper);
  skeleton_view.setSelectedBone(selected_bone_name);
  skeleton_view.setHiddenBones(&hidden_bone_names);
  skeleton_view.setShowBones(show_bones);
  skeleton_view.setMcbeCoords(use_mcbe_coords);

  if (camera_needs_fit && !geometry.bones.empty()) {
    fitCameraToModel();
  }

  if (presentation_mode != PresentationMode::SourcePreview) {
    if (const auto *frame = currentPreviewFrame()) {
      return skeleton_view.buildBaked(
          *frame, currentPreviewReferenceAnimation(),
          currentPreviewReferenceTime(), camera, view_w, view_h);
    }
  }

  if (selected_animation != nullptr) {
    return skeleton_view.buildAnimation(selected_animation, preview_time,
                                        camera, view_w, view_h);
  }

  return skeleton_view.buildRest(camera, view_w, view_h);
}

#if defined(_WIN32)
std::optional<std::filesystem::path> openFileDialog(const wchar_t *title,
                                                    const wchar_t *filter) {
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = filter;
  ofn.nFilterIndex = 1;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&ofn) == TRUE) {
    return std::filesystem::path(file);
  }
  return std::nullopt;
}

std::optional<std::filesystem::path>
saveFileDialog(const wchar_t *title, const wchar_t *filter,
               const wchar_t *default_name) {
  wchar_t file[MAX_PATH] = {};
  if (default_name != nullptr) {
    wcsncpy_s(file, default_name, _TRUNCATE);
  }
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = filter;
  ofn.nFilterIndex = 1;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
  if (GetSaveFileNameW(&ofn) == TRUE) {
    return std::filesystem::path(file);
  }
  return std::nullopt;
}
#else
std::optional<std::filesystem::path> openFileDialog(const wchar_t *,
                                                    const wchar_t *) {
  return std::nullopt;
}
std::optional<std::filesystem::path>
saveFileDialog(const wchar_t *, const wchar_t *, const wchar_t *) {
  return std::nullopt;
}
#endif

}
