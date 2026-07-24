#pragma once

#include "xpbd/constraints/constraint.hpp"

namespace xpbd::constraints {





class CrossSpringConstraint final : public Constraint {
public:
    CrossSpringConstraint(int idxA, int idxC, double minDistance, double maxDistance,
                          double compliance, double fallbackX, double fallbackY,
                          double fallbackZ);

    void solve(std::span<models::Particle* const> particles, double dt) override;
    void resetLambda() override;

private:
    static constexpr double kEpsilon = 1e-9;

    int idx_a_;
    int idx_c_;
    double min_distance_;
    double max_distance_;
    double compliance_;
    double fallback_x_;
    double fallback_y_;
    double fallback_z_;
    double lambda_ = 0.0;
    int active_side_ = 0;

    static double finiteNonNegative(double value);
};

}
