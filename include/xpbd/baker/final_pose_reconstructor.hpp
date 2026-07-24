#pragma once

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xpbd::baker {










class FinalPoseReconstructor {
public:
  struct WorldTarget {
    std::array<double, 3> position{};
    std::optional<RotationUtil::Quat> rotation;
  };

  struct LocalChannels {
    std::array<double, 3> position{};
    std::array<double, 3> rotation{};
  };

  struct Result {
    std::map<std::string, LocalChannels> local_channels;
    std::map<std::string, BonePoseCalculator::Pose> world_poses;
  };


  class Evaluator {
  public:

    class ReconstructionScratch {
    public:
      ReconstructionScratch() = default;
      ReconstructionScratch(const ReconstructionScratch &) = delete;
      ReconstructionScratch &operator=(const ReconstructionScratch &) =
          delete;
      ReconstructionScratch(ReconstructionScratch &&other) noexcept;
      ReconstructionScratch &
      operator=(ReconstructionScratch &&other) noexcept;

      [[nodiscard]] const Result &result() const { return result_; }
      void reset();

    private:
      friend class Evaluator;

      std::vector<BonePoseCalculator::Pose> final_world_poses_;
      std::vector<const BonePoseCalculator::Pose *> reference_pose_slots_;
      Result result_;
      std::vector<LocalChannels *> local_channel_slots_;
      std::vector<BonePoseCalculator::Pose *> world_pose_slots_;
      std::shared_ptr<const int> layout_identity_;
    };

    explicit Evaluator(const std::vector<loader::Bone> &bones);

    [[nodiscard]] Result reconstruct(
        const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
        const std::map<std::string, WorldTarget> &physics_targets) const;

    [[nodiscard]] const Result &reconstructInto(
        const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
        const std::map<std::string, WorldTarget> &physics_targets,
        ReconstructionScratch &scratch) const;

    [[nodiscard]] std::size_t boneCount() const { return slots_.size(); }

  private:
    static constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);

    struct Slot {
      loader::Bone bone;
      std::size_t parent_slot = kNoParent;
    };

    std::vector<Slot> slots_;
    std::shared_ptr<const int> layout_identity_ =
        std::make_shared<const int>(0);
  };

  [[nodiscard]] static Evaluator
  compile(const std::vector<loader::Bone> &bones);

  [[nodiscard]] static Result reconstruct(
      const std::vector<loader::Bone> &bones,
      const std::map<std::string, BonePoseCalculator::Pose> &reference_poses,
      const std::map<std::string, WorldTarget> &physics_targets);

private:
  FinalPoseReconstructor() = delete;
};

}
