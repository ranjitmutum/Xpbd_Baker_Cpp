#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace xpbd::baker {









class RigidBodyJointAuditor {
public:
  using PoseMap = std::map<std::string, BonePoseCalculator::Pose>;

  struct Tolerances {

    double linear_anchor_separation = 1e-3;
    double angular_limit_excess_radians = 1e-3;
  };

  struct Violation {
    std::string parent_bone;
    std::string child_bone;
    std::array<double, 4> relative_rotation_xyzw{0.0, 0.0, 0.0, 1.0};
    std::string rotation_order = "XYZ";
    double linear_anchor_separation = 0.0;
    double linear_anchor_excess = 0.0;
    std::array<double, 3> angular_coordinates_radians{};
    std::array<double, 3> angular_limit_excess_radians{};
    bool linear_unsafe = false;
    std::array<bool, 3> angular_unsafe{};
    bool joint_euler_singular = false;
  };

  struct AuditResult {
    bool unsafe = false;
    std::size_t audited_joint_count = 0;
    std::size_t unsafe_joint_count = 0;
    std::size_t euler_singular_joint_count = 0;
    double maximum_linear_anchor_separation = 0.0;
    double maximum_linear_anchor_excess = 0.0;
    double maximum_angular_limit_excess_radians = 0.0;
    std::string worst_linear_parent;
    std::string worst_linear_child;
    std::string worst_angular_parent;
    std::string worst_angular_child;
    int worst_angular_axis = -1;
    std::vector<Violation> violations;
  };

  RigidBodyJointAuditor(const BoneMapper &bone_mapper,
                        const PoseMap &initial_poses);
  RigidBodyJointAuditor(const BoneMapper &bone_mapper,
                        const PoseMap &initial_poses, Tolerances tolerances);


  [[nodiscard]] AuditResult audit(const PoseMap &poses) const;








  [[nodiscard]] AuditResult
  auditQuantizedFrame(const BakedFrame &frame,
                      const loader::Animation *reference_animation,
                      double reference_time) const;

  [[nodiscard]] std::size_t jointCount() const { return joints_.size(); }
  [[nodiscard]] const Tolerances &tolerances() const { return tolerances_; }


  [[nodiscard]] static double quantizeExportValue(double value);

private:
  struct JointDefinition {
    std::string parent_bone;
    std::string child_bone;
    std::array<double, 3> parent_anchor_offset{};
    std::array<double, 4> parent_anchor_rotation{0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> angular_limit_radians{};
    std::array<bool, 3> angular_axis_limited{};
  };

  BonePoseCalculator::Evaluator pose_evaluator_;
  std::vector<JointDefinition> joints_;
  Tolerances tolerances_{};
  double unit_scale_ = 1.0;
};

}
