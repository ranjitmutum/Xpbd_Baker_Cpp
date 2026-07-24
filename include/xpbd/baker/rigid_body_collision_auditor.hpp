#pragma once

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/rigidbody/rigid_body_types.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xpbd::baker {


class RigidBodyCollisionAuditor {
public:
  struct AuditCounters {

    std::size_t all_possible_pairs = 0;

    std::size_t broad_phase_candidates = 0;

    std::size_t sat_calls = 0;
  };

  struct AuditResult {
    bool unsafe = false;
    double maximum_penetration = 0.0;
    std::optional<std::pair<std::string, std::string>> worst_pair;
    AuditCounters counters{};
  };

  RigidBodyCollisionAuditor(
      const std::vector<loader::Bone> &all_bones,
      const std::map<std::string, rigidbody::BodyDefinition> &physics_bodies,
      const std::set<std::string> &collision_bones, double unit_scale,
      double maximum_safe_penetration, bool ground_collision_enabled);

  [[nodiscard]] AuditResult
  audit(const std::map<std::string, BonePoseCalculator::Pose> &poses) const;

  [[nodiscard]] double minimumColliderFeature() const {
    return minimum_collider_feature_;
  }

  [[nodiscard]] double maximumColliderRadius() const {
    return maximum_collider_radius_;
  }

  [[nodiscard]] double maximumVertexTravel(
      const std::map<std::string, BonePoseCalculator::Pose> &from,
      const std::map<std::string, BonePoseCalculator::Pose> &to) const;

private:
  struct CubeBinding {
    std::string bone_name;
    rigidbody::MotionType motion_type = rigidbody::MotionType::Dynamic;
    std::array<double, 24> bind_vertices{};


    bool use_affine_pose = false;
  };

  struct WorldBox {
    std::string body_name;
    rigidbody::MotionType motion_type = rigidbody::MotionType::Dynamic;

    std::array<double, 24> vertices_soa{};
    std::array<std::array<double, 3>, 3> axes{};
    std::array<double, 3> aabb_min{};
    std::array<double, 3> aabb_max{};
  };

  using CandidatePair = std::pair<std::size_t, std::size_t>;
  using ProjectionOverlapKernel = double (*)(const double *, const double *,
                                             const double *) noexcept;

  std::vector<CubeBinding> physics_cubes_;
  std::vector<CubeBinding> collision_cubes_;
  std::set<std::pair<std::string, std::string>> linked_physics_body_pairs_;
  double unit_scale_ = 1.0 / 16.0;
  double maximum_safe_penetration_ = 0.2;
  double tolerance_ = 1e-9;
  double minimum_collider_feature_ = 0.0;
  double maximum_collider_radius_ = 0.0;
  bool ground_collision_enabled_ = false;
  ProjectionOverlapKernel projection_overlap_kernel_ = nullptr;

  static constexpr double kMinAxisLengthSquared = 1e-20;

  static std::vector<CubeBinding> collectCompiled(
      const std::map<std::string, rigidbody::BodyDefinition> &bodies,
      double unit_scale);
  static std::vector<CubeBinding>
  collect(const std::vector<loader::Bone> &bones,
          const std::set<std::string> &included, double tolerance);
  static std::set<std::pair<std::string, std::string>>
  collectLinkedPhysicsBodyPairs(
      const std::vector<loader::Bone> &bones,
      const std::map<std::string, rigidbody::BodyDefinition> &physics_bodies);
  static std::pair<std::string, std::string>
  canonicalPair(const std::string &first, const std::string &second);
  static std::vector<WorldBox>
  transform(const std::vector<CubeBinding> &bindings,
            const std::map<std::string, BonePoseCalculator::Pose> &poses);
  static std::vector<CandidatePair>
  sweepSelfCandidates(const std::vector<WorldBox> &boxes);
  static std::vector<CandidatePair>
  sweepCrossCandidates(const std::vector<WorldBox> &first,
                       const std::vector<WorldBox> &second);
  static bool aabbOverlaps(const WorldBox &a, const WorldBox &b);
  static std::array<std::array<double, 3>, 3>
  axesFromSoaVertices(const std::array<double, 24> &vertices);
  static std::array<double, 3>
  normalizedSoaEdge(const std::array<double, 24> &vertices, int from, int to);
  static double penetration(const WorldBox &a, const WorldBox &b,
                            double tolerance,
                            ProjectionOverlapKernel projection_overlap);
  static void normalize(std::array<double, 3> &value);
  static double dot(const std::array<double, 3> &a,
                    const std::array<double, 3> &b);
  static double estimateScale(const std::vector<loader::Bone> &bones);
  static double minimumSoaY(const std::array<double, 24> &vertices);
  static double minimumFeature(const std::vector<CubeBinding> &bindings);
  static double maximumRadius(const std::vector<CubeBinding> &bindings);
};

}
