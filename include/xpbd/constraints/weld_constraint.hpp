#pragma once

#include "xpbd/constraints/constraint.hpp"

namespace xpbd::constraints {


class WeldConstraint final : public Constraint {
public:
    WeldConstraint(int idxA, int idxB, double compliance, double dampingCompliance);

    void solve(std::span<models::Particle* const> particles, double dt) override;
    void resetLambda() override;

private:
    int idx_a_;
    int idx_b_;
    double compliance_;
    double damping_compliance_;
    double lambda_[3] = {0.0, 0.0, 0.0};

    static double finiteNonNegative(double value);
    static double implicitVelocityX(const models::Particle& p, double dt);
    static double implicitVelocityY(const models::Particle& p, double dt);
    static double implicitVelocityZ(const models::Particle& p, double dt);

    void solveAxis(models::Particle& a, models::Particle& b, int axis, double value,
                   double derivative, double wa, double wb, double alpha, double gamma,
                   double dt, double denominator);
};

}
