#include "xpbd/baker/loop_error_report.hpp"

namespace xpbd::baker {

namespace {
bool within(double value, bool available, double tolerance) {
    return !available || (std::isfinite(value) && value <= tolerance);
}

double normalized(double value, double tolerance) {
    if (!std::isfinite(value)) {
        return std::numeric_limits<double>::infinity();
    }
    if (tolerance == std::numeric_limits<double>::infinity()) {
        return 0.0;
    }
    if (tolerance == 0.0) {
        return value == 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
    }
    return value / tolerance;
}
}

bool LoopErrorReport::isWithin(const LoopBakeConfig& config) const {
    return valid() &&
           within(maximum_position_error, availability.position, config.position_tolerance) &&
           within(maximum_rotation_error_radians, availability.rotation,
                  config.rotation_tolerance_radians) &&
           within(maximum_linear_velocity_error, availability.linear_velocity,
                  config.linear_velocity_tolerance) &&
           within(maximum_angular_velocity_error, availability.angular_velocity,
                  config.angular_velocity_tolerance) &&
           (!availability.contacts || !contact_set_changed) && collisionSafe(config);
}

bool LoopErrorReport::collisionSafe(const LoopBakeConfig& config) const {
    return !availability.maximum_penetration ||
           (std::isfinite(maximum_penetration) &&
            maximum_penetration <= config.maximum_penetration_tolerance);
}

double LoopErrorReport::normalizedScore(const LoopBakeConfig& config) const {
    if (!valid()) {
        return std::numeric_limits<double>::infinity();
    }
    double maximum = 0.0;
    if (availability.position) {
        maximum = std::max(maximum,
                           normalized(maximum_position_error, config.position_tolerance));
    }
    if (availability.rotation) {
        maximum = std::max(maximum, normalized(maximum_rotation_error_radians,
                                               config.rotation_tolerance_radians));
    }
    if (availability.linear_velocity) {
        maximum = std::max(maximum, normalized(maximum_linear_velocity_error,
                                               config.linear_velocity_tolerance));
    }
    if (availability.angular_velocity) {
        maximum = std::max(maximum, normalized(maximum_angular_velocity_error,
                                               config.angular_velocity_tolerance));
    }
    if (availability.maximum_penetration) {
        maximum = std::max(maximum, normalized(maximum_penetration,
                                               config.maximum_penetration_tolerance));
    }
    const double contactPenalty =
        availability.contacts && contact_set_changed
            ? static_cast<double>(std::max(1, contact_difference_count))
            : 0.0;
    return maximum + contactPenalty;
}

std::vector<std::string>
LoopErrorReport::rejectionReasons(const LoopBakeConfig& config) const {
    if (!valid()) {
        switch (validation.state) {
        case LoopValidationState::Valid:
            return {"producer_anomaly"};
        case LoopValidationState::MissingBone:
            return {"missing_bone"};
        case LoopValidationState::MissingMetric:
            return {"missing_metric"};
        case LoopValidationState::NonFiniteValue:
            return {"non_finite_value"};
        case LoopValidationState::InvalidQuaternion:
            return {"invalid_quaternion"};
        case LoopValidationState::InvalidContact:
            return {"invalid_contact"};
        case LoopValidationState::InvalidSampleTime:
            return {"invalid_sample_time"};
        case LoopValidationState::ProducerAnomaly:
            return {"producer_anomaly"};
        }
    }
    std::vector<std::string> reasons;
    if (availability.position &&
        !within(maximum_position_error, true, config.position_tolerance)) {
        reasons.emplace_back("pivot_position_tolerance");
    }
    if (availability.rotation &&
        !within(maximum_rotation_error_radians, true,
                config.rotation_tolerance_radians)) {
        reasons.emplace_back("pivot_rotation_tolerance");
    }
    if (availability.linear_velocity &&
        !within(maximum_linear_velocity_error, true,
                config.linear_velocity_tolerance)) {
        reasons.emplace_back("pivot_linear_velocity_tolerance");
    }
    if (availability.angular_velocity &&
        !within(maximum_angular_velocity_error, true,
                config.angular_velocity_tolerance)) {
        reasons.emplace_back("angular_velocity_tolerance");
    }
    if (availability.contacts && contact_set_changed) {
        reasons.emplace_back("boundary_contact_change");
    }
    if (availability.maximum_penetration && !collisionSafe(config)) {
        reasons.emplace_back("maximum_penetration");
    }
    return reasons;
}

}
