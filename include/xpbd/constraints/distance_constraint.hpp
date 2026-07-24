#pragma once

#include "xpbd/constraints/constraint.hpp"

namespace xpbd::constraints {

class DistanceConstraint final : public Constraint {
public:
    DistanceConstraint(int idxA, int idxB, double restLength, double compliance,
                       double dampingCompliance);

    void solve(std::span<models::Particle* const> particles, double dt) override;
    void resetLambda() override;

private:
    int idx_a_;
    int idx_b_;
    double rest_length_;
    double compliance_;
    double damping_compliance_;
    double lambda_ = 0.0;
    double normal_x_ = 1.0;
    double normal_y_ = 0.0;
    double normal_z_ = 0.0;

    static double finiteNonNegative(double value);
};

}
