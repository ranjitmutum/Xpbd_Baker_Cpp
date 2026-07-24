#include "xpbd/baker/inertialized_target.hpp"

#include <algorithm>
#include <cmath>

namespace xpbd::baker {

InertializedTarget::InertializedTarget(std::vector<double> initial_offset,
                                       std::vector<double> initial_velocity, double duration,
                                       double follow_weight)
    : initial_offset_(std::move(initial_offset)),
      initial_velocity_(std::move(initial_velocity)),
      duration_(duration) {
    if (initial_offset_.size() != initial_velocity_.size() || !std::isfinite(duration) ||
        !(duration > 0.0) || !std::isfinite(follow_weight) || follow_weight < 0.0 ||
        follow_weight > 1.0) {
        throw std::invalid_argument("invalid inertialized target settings");
    }
    for (double c : initial_offset_) {
        if (!std::isfinite(c)) {
            throw std::invalid_argument("initial offset must be finite");
        }
    }
    for (double c : initial_velocity_) {
        if (!std::isfinite(c)) {
            throw std::invalid_argument("initial velocity must be finite");
        }
    }
    omega_ = follow_weight == 0.0 ? 0.0 : kOnePercentCriticalFactor * follow_weight / duration_;
}

std::vector<double> InertializedTarget::offsetAt(double elapsed) const {
    const double time = std::isfinite(elapsed) ? std::max(0.0, elapsed) : 0.0;
    std::vector<double> result(initial_offset_.size(), 0.0);
    if (omega_ == 0.0) {
        return initial_offset_;
    }
    if (time >= duration_) {
        return result;
    }

    const double decay = std::exp(-omega_ * time);
    const double endDecay = std::exp(-omega_ * duration_);
    const double u = time / duration_;
    const double u2 = u * u;
    const double u3 = u2 * u;

    const double endValueWeight = -2 * u3 + 3 * u2;
    const double endVelocityWeight = u3 - u2;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const double b = initial_velocity_[i] + omega_ * initial_offset_[i];
        const double raw = (initial_offset_[i] + b * time) * decay;
        const double rawEnd = (initial_offset_[i] + b * duration_) * endDecay;
        const double rawEndVelocity = (initial_velocity_[i] - omega_ * b * duration_) * endDecay;
        result[i] = raw - endValueWeight * rawEnd - endVelocityWeight * duration_ * rawEndVelocity;
    }
    return result;
}

}
