#pragma once

#include "xpbd/baker/loop_bake_config.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace xpbd::baker {

enum class LoopValidationState {
    Valid,
    MissingBone,
    MissingMetric,
    NonFiniteValue,
    InvalidQuaternion,
    InvalidContact,
    InvalidSampleTime,
    ProducerAnomaly,
};

struct PeriodicMetricAvailability {
    bool position = false;
    bool rotation = false;
    bool linear_velocity = false;
    bool angular_velocity = false;
    bool contacts = false;
    bool maximum_penetration = false;
    bool maximum_penetration_time = false;
    bool sample_time = false;
};

struct LoopBoundaryBodyState {
    std::string bone_name;
    std::array<double, 3> pivot_position{};
    bool has_pivot_position = false;
    std::array<double, 4> pivot_rotation_xyzw{0, 0, 0, 1};
    bool has_pivot_rotation = false;
    std::array<double, 3> pivot_linear_velocity{};
    bool has_pivot_linear_velocity = false;
    std::array<double, 3> com_position{};
    bool has_com_position = false;
    std::array<double, 4> com_rotation_xyzw{0, 0, 0, 1};
    bool has_com_rotation = false;
    std::array<double, 3> com_linear_velocity{};
    bool has_com_linear_velocity = false;
    std::array<double, 3> angular_velocity{};
    bool has_angular_velocity = false;
};

struct LoopBoundaryContactState {
    std::string pair;
    bool meaningful_penetration = false;
    double penetration = 0.0;
    int penetration_bucket = 0;
};

struct LoopBoundaryState {

    double sample_time = 0.0;
    bool has_sample_time = false;
    double maximum_penetration = 0.0;
    bool has_maximum_penetration = false;
    double maximum_penetration_time = -1.0;
    bool has_maximum_penetration_time = false;
    std::vector<LoopBoundaryBodyState> bodies;
    std::vector<LoopBoundaryContactState> contacts;
};

struct SnapshotValidation {
    bool complete = true;
    LoopValidationState state = LoopValidationState::Valid;
    int non_finite_value_count = 0;
    int missing_bone_count = 0;
    int missing_metric_count = 0;
    int invalid_quaternion_count = 0;
    int invalid_contact_count = 0;
    std::string first_invalid_bone;
    std::string first_invalid_field;
};

struct LoopErrorReport {

    LoopBoundaryState start_boundary{};
    LoopBoundaryState end_boundary{};
    double maximum_position_error = 0.0;
    std::string position_bone;
    double maximum_rotation_error_radians = 0.0;
    std::string rotation_bone;
    double maximum_linear_velocity_error = 0.0;
    std::string linear_velocity_bone;
    double maximum_angular_velocity_error = 0.0;
    std::string angular_velocity_bone;
    bool contact_set_changed = false;
    int contact_difference_count = 0;
    int contact_pair_added_count = 0;
    int contact_pair_removed_count = 0;
    int contact_state_changed_count = 0;
    int meaningful_penetration_changed_count = 0;
    double maximum_penetration = 0.0;
    double maximum_penetration_time = -1.0;
    int anomaly_count = 0;
    PeriodicMetricAvailability availability{};
    SnapshotValidation validation{};

    [[nodiscard]] bool isWithin(const LoopBakeConfig& config) const;
    [[nodiscard]] bool collisionSafe(const LoopBakeConfig& config) const;
    [[nodiscard]] double normalizedScore(const LoopBakeConfig& config) const;
    [[nodiscard]] std::vector<std::string>
    rejectionReasons(const LoopBakeConfig& config) const;
    [[nodiscard]] bool valid() const { return validation.complete && anomaly_count == 0; }
};

}
