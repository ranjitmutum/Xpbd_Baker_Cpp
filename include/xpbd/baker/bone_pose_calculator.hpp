#pragma once

#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xpbd::baker {


class BonePoseCalculator {
public:
  struct Pose {
    std::array<double, 3> world_position{};
    std::array<double, 4> world_rotation{0.0, 0.0, 0.0, 1.0};

    std::array<double, 9> world_linear{1.0, 0.0, 0.0, 0.0, 1.0,
                                       0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> world_translation{};
    std::array<double, 3> animation_position{};
    std::array<double, 3> animation_scale{1.0, 1.0, 1.0};
    std::array<double, 3> total_local_euler{};
  };

  class Evaluator {
  public:
    using PoseMap = std::map<std::string, Pose>;





    class AnimationBinding {
    public:
      [[nodiscard]] const loader::Animation *animation() const {
        return animation_;
      }

    private:
      friend class Evaluator;

      const loader::Animation *animation_ = nullptr;
      std::vector<const loader::BoneAnimation *> channels_by_slot_;
      std::shared_ptr<const int> layout_identity_;
    };





    class EvaluationScratch {
    public:
      EvaluationScratch() = default;
      EvaluationScratch(const EvaluationScratch &) = delete;
      EvaluationScratch &operator=(const EvaluationScratch &) = delete;
      EvaluationScratch(EvaluationScratch &&other) noexcept;
      EvaluationScratch &operator=(EvaluationScratch &&other) noexcept;

      [[nodiscard]] const PoseMap &result() const { return result_; }
      void reset();
      void replaceResult(PoseMap result);

    private:
      friend class Evaluator;

      std::vector<Pose> dense_poses_;
      PoseMap result_;
      std::vector<Pose *> result_slots_;
      std::shared_ptr<const int> layout_identity_;
    };

    explicit Evaluator(const std::vector<loader::Bone> &bones);

    [[nodiscard]] AnimationBinding
    bind(const loader::Animation *animation) const;

    [[nodiscard]] PoseMap
    calculate(const loader::Animation *animation, double time) const;

    [[nodiscard]] PoseMap calculate(
        const loader::Animation *animation, double time,
        const std::map<std::string, std::array<double, 3>> *position_overrides,
        const std::map<std::string, std::array<double, 3>> *rotation_overrides)
        const;

    [[nodiscard]] PoseMap calculate(
        const AnimationBinding &binding, double time,
        const std::map<std::string, std::array<double, 3>> *position_overrides =
            nullptr,
        const std::map<std::string, std::array<double, 3>> *rotation_overrides =
            nullptr) const;

    [[nodiscard]] const PoseMap &calculateInto(
        const AnimationBinding &binding, double time,
        EvaluationScratch &scratch,
        const std::map<std::string, std::array<double, 3>> *position_overrides =
            nullptr,
        const std::map<std::string, std::array<double, 3>> *rotation_overrides =
            nullptr) const;

  private:
    static constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);

    struct Slot {
      std::string name;
      std::array<double, 3> pivot{};
      std::array<double, 3> rotation{};
      std::size_t parent_slot = kNoParent;
    };

    std::vector<Slot> slots_;
    std::shared_ptr<const int> layout_identity_ =
        std::make_shared<const int>(0);

    static void appendTopologically(
        std::size_t bone_index, const std::vector<loader::Bone> &bones,
        const std::map<std::string, std::size_t> &by_name,
        std::map<std::string, int> &states, std::vector<std::size_t> &ordered);
  };

  [[nodiscard]] static Evaluator
  compile(const std::vector<loader::Bone> &bones);

  [[nodiscard]] static std::map<std::string, Pose>
  calculate(const std::vector<loader::Bone> &bones,
            const loader::Animation *animation, double time);

  [[nodiscard]] static std::map<std::string, Pose> calculate(
      const std::vector<loader::Bone> &bones,
      const loader::Animation *animation, double time,
      const std::map<std::string, std::array<double, 3>> *position_overrides,
      const std::map<std::string, std::array<double, 3>> *rotation_overrides);

private:
  BonePoseCalculator() = delete;

  static Pose compose(
      const std::string &bone_name, const double bone_pivot[3],
      const double bone_rotation[3], const Pose *parent_pose,
      const loader::BoneAnimation *channel, double time,
      const std::map<std::string, std::array<double, 3>> *position_overrides,
      const std::map<std::string, std::array<double, 3>> *rotation_overrides);

  static Pose resolve(
      const loader::Bone &bone,
      const std::map<std::string, loader::Bone> &by_name,
      const loader::Animation *animation, double time,
      const std::map<std::string, std::array<double, 3>> *position_overrides,
      const std::map<std::string, std::array<double, 3>> *rotation_overrides,
      std::map<std::string, Pose> &result, std::vector<std::string> &visiting);
};

}
