#pragma once

#include <array>
#include <stdexcept>
#include <vector>

namespace xpbd::baker {


class InertializedTarget {
public:

    static constexpr double kOnePercentCriticalFactor = 6.638352067993813;

    InertializedTarget(std::vector<double> initial_offset,
                       std::vector<double> initial_velocity, double duration,
                       double follow_weight);

    [[nodiscard]] std::vector<double> offsetAt(double elapsed) const;

private:
    std::vector<double> initial_offset_;
    std::vector<double> initial_velocity_;
    double duration_ = 0.0;
    double omega_ = 0.0;
};

}
