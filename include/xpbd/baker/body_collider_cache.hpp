#pragma once

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xpbd::baker {



// 缓存骨骼立方体生成的碰撞体，避免烘焙过程中重复构建。
class BodyColliderCache {
public:
  class Collider {
  public:
    using Aabb = std::array<double, 6>;

    Collider(std::string owner_bone, std::array<double, 24> bind_vertices,
             double epsilon);

    void update(const BonePoseCalculator::Pose &pose, bool keep_history);

    [[nodiscard]] const std::string &ownerBone() const { return owner_bone_; }
    [[nodiscard]] double getBindNormal(int face, int axis) const;
    [[nodiscard]] double getBindConstant(int face) const;
    [[nodiscard]] double getCurrentNormal(int face, int axis) const;
    [[nodiscard]] double signedDistanceCurrent(int face, double x, double y,
                                               double z) const;
    [[nodiscard]] double signedDistancePrevious(int face, double x, double y,
                                                double z) const;
    [[nodiscard]] bool containsCurrent(double x, double y, double z,
                                       double skin) const;
    [[nodiscard]] bool containsPrevious(double x, double y, double z,
                                        double skin) const;
    [[nodiscard]] const Aabb &currentAabb() const { return current_aabb_; }
    [[nodiscard]] const Aabb &previousAabb() const { return previous_aabb_; }
    [[nodiscard]] const Aabb &sweptAabb() const { return swept_aabb_; }

    void toCurrentBind(double x, double y, double z, double result[3]) const;
    void toPreviousBind(double x, double y, double z, double result[3]) const;
    void fromCurrentBind(double x, double y, double z, double result[3]) const;
    void fromPreviousBind(double x, double y, double z, double result[3]) const;

  private:
    void buildBindPlanes(double epsilon);

    std::string owner_bone_;
    std::array<double, 24> bind_vertices_{};
    std::array<double, 18> bind_normals_{};
    std::array<double, 6> bind_constants_{};
    std::array<double, 4> previous_rotation_{0, 0, 0, 1};
    std::array<double, 4> current_rotation_{0, 0, 0, 1};
    std::array<double, 3> previous_translation_{};
    std::array<double, 3> current_translation_{};
    std::array<double, 18> previous_normals_{};
    std::array<double, 18> current_normals_{};
    std::array<double, 6> previous_constants_{};
    std::array<double, 6> current_constants_{};
    Aabb previous_aabb_{};
    Aabb current_aabb_{};
    Aabb swept_aabb_{};
    std::array<double, 24> current_vertices_{};
    double bind_max_radius_ = 0.0;
    bool initialized_ = false;
  };

  BodyColliderCache(const std::vector<loader::Bone> &all_bones,
                    const std::set<std::string> &collision_bones);

  void initialize(const std::map<std::string, BonePoseCalculator::Pose> &poses);
  void advance(const std::map<std::string, BonePoseCalculator::Pose> &poses,
               bool history_continuous);
  void
  setAuditPose(const std::map<std::string, BonePoseCalculator::Pose> &poses);

  [[nodiscard]] const std::vector<Collider> &colliders() const {
    return colliders_;
  }
  [[nodiscard]] int degenerateCubeCount() const {
    return degenerate_cube_count_;
  }
  [[nodiscard]] double epsilon() const { return epsilon_; }
  [[nodiscard]] bool isSweepContinuous() const { return sweep_continuous_; }

  [[nodiscard]] bool containsCurrent(double x, double y, double z,
                                     double skin) const;
  [[nodiscard]] bool containsPrevious(double x, double y, double z,
                                      double skin) const;






  void queryCurrentCandidates(double x, double y, double z, double skin,
                              std::vector<std::size_t> &output) const;
  void queryPreviousCandidates(double x, double y, double z, double skin,
                               std::vector<std::size_t> &output) const;
  void querySweepCandidates(double previous_x, double previous_y,
                            double previous_z, double current_x,
                            double current_y, double current_z, double skin,
                            std::vector<std::size_t> &output) const;

private:
  void update(const std::map<std::string, BonePoseCalculator::Pose> &poses,
              bool history_continuous, bool initialize_history);

  static double estimateModelScale(const std::vector<loader::Bone> &bones);
  void rebuildCandidateIndices();

  std::vector<Collider> colliders_;
  std::vector<std::size_t> current_x_order_;
  std::vector<std::size_t> previous_x_order_;
  std::vector<std::size_t> swept_x_order_;
  double epsilon_ = 1e-9;
  int degenerate_cube_count_ = 0;
  bool sweep_continuous_ = false;
};

}
