#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xpbd::baker {


class LoopSeamReport {
public:
    struct Metrics {
        double maximum_position_error = 0.0;
        std::string position_bone;
        double maximum_rotation_error_radians = 0.0;
        std::string rotation_bone;
        double maximum_linear_velocity_jump = 0.0;
        std::string linear_velocity_bone;
        double maximum_angular_velocity_jump = 0.0;
        std::string angular_velocity_bone;
        double maximum_linear_acceleration_jump = 0.0;
        std::string linear_acceleration_bone;
        double maximum_angular_acceleration_jump = 0.0;
        std::string angular_acceleration_bone;
        double peak_linear_velocity = 0.0;
        double peak_angular_velocity = 0.0;
        double peak_linear_acceleration = 0.0;
        double peak_angular_acceleration = 0.0;

        static Metrics empty() { return {}; }
    };

    struct AnchorCoverage {
        std::string chain_root;
        std::string fixed_anchor;
        std::size_t expected_bone_count = 0;
        std::size_t measured_bone_count = 0;
        bool complete = false;
    };

    struct Validation {
        bool valid = true;
        bool sample_count_sufficient = false;
        int verified_continuity_order = -1;
        int missing_bone_count = 0;
        int non_finite_value_count = 0;
        std::string first_missing_bone;
        std::string first_invalid_field;
        std::string affected_metric_space;
        bool physics_relative_available = false;
        std::string physics_relative_fallback_reason;
    };

    struct DriverSeamGate {
        bool available = false;
        bool c0_pass = true;
        bool c1_pass = true;
        bool c2_pass = true;
        Metrics metrics{};

        [[nodiscard]] bool passes() const {
            return !available || (c0_pass && c1_pass && c2_pass);
        }
    };

    struct ContinuityGate {
        bool c0_pass = false;
        bool c1_pass = false;
        bool c2_pass = false;

        [[nodiscard]] bool passes() const {
            return c0_pass && c1_pass && c2_pass;
        }
    };

    struct QuantizationGate {
        bool local_c0_pass = false;
        bool final_world_c0_pass = false;

        [[nodiscard]] bool passes() const {
            return local_c0_pass && final_world_c0_pass;
        }
    };

    struct CollisionGate {
        bool candidate_safe = false;
        bool penetration_safe = false;

        [[nodiscard]] bool passes() const {
            return candidate_safe && penetration_safe;
        }
    };

    struct JointGate {
        bool candidate_safe = false;

        [[nodiscard]] bool passes() const { return candidate_safe; }
    };

    struct ExportGate {
        bool validation_pass = false;
        bool physics_seam_pass = false;
        bool driver_seam_pass = false;
        bool quantization_pass = false;
        bool collision_pass = false;
        bool joint_pass = false;

        [[nodiscard]] bool passes() const {
            return validation_pass && physics_seam_pass && driver_seam_pass &&
                   quantization_pass && collision_pass && joint_pass;
        }
    };

    static LoopSeamReport measure(const std::vector<BakedFrame>& frames,
                                  const std::vector<loader::Bone>& bones,
                                  const loader::Animation* animation,
                                  const std::set<std::string>& physics_bones,
                                  const std::set<std::string>& fixed_physics_bones,
                                  bool correction_applied, double correction_window_ratio,
                                  bool collision_safe, double maximum_penetration,
                                  bool joint_safe = true, bool audit_valid = true,
                                  double correction_window_duration_seconds = 0.0);

    [[nodiscard]] bool passes(const BoneMapper::PhysicsGroupConfig& config) const;
    [[nodiscard]] double
    previewQualityScore(const BoneMapper::PhysicsGroupConfig& config) const;
    [[nodiscard]] double qualityScore(const BoneMapper::PhysicsGroupConfig& config) const;
    [[nodiscard]] DriverSeamGate
    driverGate(const BoneMapper::PhysicsGroupConfig& config) const;
    [[nodiscard]] ContinuityGate
    physicsSeamGate(const BoneMapper::PhysicsGroupConfig& config) const;

    [[nodiscard]] ContinuityGate
    continuityGate(const BoneMapper::PhysicsGroupConfig& config) const;
    [[nodiscard]] QuantizationGate quantizationGate() const;
    [[nodiscard]] CollisionGate
    collisionGate(const BoneMapper::PhysicsGroupConfig& config) const;
    [[nodiscard]] JointGate jointGate() const;
    [[nodiscard]] ExportGate
    exportGate(const BoneMapper::PhysicsGroupConfig& config) const;

    [[nodiscard]] const Metrics& local() const { return local_; }
    [[nodiscard]] const Metrics& finalWorld() const { return final_world_; }
    [[nodiscard]] const Metrics& driver() const { return driver_; }
    [[nodiscard]] const Metrics& physicsRelative() const { return physics_relative_; }
    [[nodiscard]] const Metrics& quantizedLocal() const { return quantized_local_; }
    [[nodiscard]] const Metrics& quantizedFinalWorld() const { return quantized_final_world_; }
    [[nodiscard]] bool correctionApplied() const { return correction_applied_; }
    [[nodiscard]] double correctionWindowDurationSeconds() const {
        return correction_window_duration_seconds_;
    }
    [[nodiscard]] double correctionWindowRatio() const { return correction_window_ratio_; }
    [[nodiscard]] bool collisionSafe() const { return collision_safe_; }
    [[nodiscard]] bool jointSafe() const { return joint_safe_; }
    [[nodiscard]] bool auditValid() const { return audit_valid_; }
    [[nodiscard]] double maximumPenetration() const { return maximum_penetration_; }
    [[nodiscard]] const Validation& validation() const { return validation_; }
    [[nodiscard]] const std::vector<AnchorCoverage>& anchorCoverage() const {
        return anchor_coverage_;
    }
    [[nodiscard]] bool usesPhysicsRelativeFallback(
        const BoneMapper::PhysicsGroupConfig& config) const {
        return config.loop_seam_strategy == BoneMapper::LoopSeamStrategy::PhysicsRelative &&
               !validation_.physics_relative_available;
    }

private:
    struct Transform {
        std::array<double, 3> position{0, 0, 0};
        std::array<double, 4> rotation{0, 0, 0, 1};
    };

    Metrics local_{};
    Metrics final_world_{};
    Metrics driver_{};
    Metrics physics_relative_{};
    Metrics quantized_local_{};
    Metrics quantized_final_world_{};
    bool correction_applied_ = false;
    double correction_window_duration_seconds_ = 0.0;
    double correction_window_ratio_ = 0.0;
    bool collision_safe_ = true;
    bool joint_safe_ = true;
    bool audit_valid_ = true;
    double maximum_penetration_ = 0.0;
    bool driver_available_ = false;
    Validation validation_{};
    std::vector<AnchorCoverage> anchor_coverage_;

    LoopSeamReport(Metrics local, Metrics final_world, Metrics driver, Metrics physics_relative,
                   Metrics quantized_local, Metrics quantized_final_world,
                   bool correction_applied, double correction_window_duration_seconds,
                   double correction_window_ratio, bool collision_safe,
                   double maximum_penetration, bool joint_safe, bool audit_valid,
                   bool driver_available,
                   Validation validation, std::vector<AnchorCoverage> anchor_coverage);

    static Metrics measureTransforms(
        const std::vector<std::map<std::string, Transform>>& samples,
        const std::set<std::string>& names, const std::vector<BakedFrame>& frames);
    static std::vector<std::map<std::string, Transform>> localTransforms(
        const std::vector<BakedFrame>& frames, const std::map<std::string, loader::Bone>& bones,
        bool quantize);
    static std::vector<std::map<std::string, Transform>> worldTransforms(
        const std::vector<BakedFrame>& frames, const std::vector<loader::Bone>& bones,
        const loader::Animation* animation, bool quantize);
    static std::vector<std::map<std::string, Transform>> driverTransforms(
        const std::vector<BakedFrame>& frames, const std::vector<loader::Bone>& bones,
        const loader::Animation* animation);
    static std::map<std::string, Transform> transformsFromPoses(
        const std::map<std::string, BonePoseCalculator::Pose>& poses);
    static std::vector<std::map<std::string, Transform>> relativeTransforms(
        const std::vector<std::map<std::string, Transform>>& world,
        const std::map<std::string, std::string>& anchors);
    static std::set<std::string> measuredSubtree(const std::vector<loader::Bone>& bones,
                                                 const std::set<std::string>& physics_bones);
    static std::map<std::string, std::string> findAnchors(
        const std::set<std::string>& names, const std::map<std::string, loader::Bone>& bones,
        const std::set<std::string>& physics_bones, const std::set<std::string>& fixed_physics_bones);
    static std::set<std::string> relativeNames(const std::map<std::string, std::string>& anchors);
    static std::set<std::string> driverNames(
        const std::map<std::string, loader::Bone>& bones,
        const std::set<std::string>& physics_bones,
        const std::set<std::string>& fixed_physics_bones);
    static std::vector<AnchorCoverage> buildAnchorCoverage(
        const std::map<std::string, loader::Bone>& bones,
        const std::set<std::string>& physics_bones,
        const std::set<std::string>& fixed_physics_bones,
        const std::map<std::string, std::string>& anchors);
    static void validateTransformSamples(
        const std::vector<std::map<std::string, Transform>>& samples,
        const std::set<std::string>& names, const std::string& metric_space,
        Validation& validation);
    static std::map<std::string, loader::Bone> indexBones(const std::vector<loader::Bone>& bones);
    static std::array<double, 3> velocity(const std::vector<std::map<std::string, Transform>>& samples,
                                          const std::vector<BakedFrame>& frames,
                                          const std::string& name, int from, int to);
    static std::array<double, 3> angularVelocity(
        const std::vector<std::map<std::string, Transform>>& samples,
        const std::vector<BakedFrame>& frames, const std::string& name, int from, int to);
    static double timeStep(const std::vector<BakedFrame>& frames, int from, int to);
    static std::array<double, 3> acceleration(
        const std::vector<std::map<std::string, Transform>>& samples,
        const std::vector<BakedFrame>& frames, const std::string& name, int a, int b, int c,
        bool angular);
    static double rotationDistance(const std::array<double, 4>& a, const std::array<double, 4>& b);
    static std::array<double, 3> values(const std::array<double, 3>& source, bool quantize);
    static double distance(const std::array<double, 3>& a, const std::array<double, 3>& b);
    static double length(const std::array<double, 3>& value);
    static std::array<double, 3> subtract(const std::array<double, 3>& a,
                                          const std::array<double, 3>& b);
    static std::array<double, 3> scale(const std::array<double, 3>& value, double factor);
    static double ratio(double value, double limit);
};

}
