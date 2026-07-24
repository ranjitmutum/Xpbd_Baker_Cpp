#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace xpbd::baker {


class StabilityCalculator {
public:
    struct Result {
        double current_load = 0.0;
        double force_budget = 0.0;
        double max_compliance = 0.0;
        double max_gravity_magnitude = 0.0;
        double max_wind_speed = 0.0;
        double max_turbulence = 0.0;
        double max_uniform_mass_scale = 0.0;
        bool safe = true;
    };

    [[nodiscard]] static Result calculate(double permitted_extension, double compliance,
                                          double gravity_coefficient, double wind_coefficient,
                                          double turbulence_coefficient, double gravity_magnitude,
                                          double wind_speed, double turbulence) {
        const double extension = finiteNonNegative(permitted_extension);
        const double current_compliance = finiteNonNegative(compliance);
        const double g_coeff = finiteNonNegative(gravity_coefficient);
        const double w_coeff = finiteNonNegative(wind_coefficient);
        const double t_coeff = finiteNonNegative(turbulence_coefficient);
        const double gravity = finiteNonNegative(gravity_magnitude);
        const double wind = finiteNonNegative(wind_speed);
        const double gust = finiteNonNegative(turbulence);

        const double gravity_load = g_coeff * gravity;
        const double wind_load = w_coeff * wind;
        const double turbulence_load = t_coeff * gust;
        const double current_load = gravity_load + wind_load + turbulence_load;
        const double force_budget =
            current_compliance > 0.0 ? extension / current_compliance
                                     : std::numeric_limits<double>::infinity();
        const double max_compliance =
            current_load > 0.0 ? extension / current_load
                               : std::numeric_limits<double>::infinity();

        Result r;
        r.current_load = current_load;
        r.force_budget = force_budget;
        r.max_compliance = max_compliance;
        r.max_gravity_magnitude = maxParameter(force_budget - wind_load - turbulence_load, g_coeff);
        r.max_wind_speed = maxParameter(force_budget - gravity_load - turbulence_load, w_coeff);
        r.max_turbulence = maxParameter(force_budget - gravity_load - wind_load, t_coeff);
        r.max_uniform_mass_scale =
            current_load > 0.0 ? std::max(0.0, force_budget / current_load)
                               : std::numeric_limits<double>::infinity();
        r.safe = current_compliance <= max_compliance;
        return r;
    }

private:
    StabilityCalculator() = delete;

    static double finiteNonNegative(double value) {
        return std::isfinite(value) ? std::max(0.0, value) : 0.0;
    }

    static double maxParameter(double remaining_budget, double coefficient) {
        if (std::isinf(remaining_budget)) {
            return std::numeric_limits<double>::infinity();
        }
        if (coefficient <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        return std::max(0.0, remaining_budget / coefficient);
    }
};

}
