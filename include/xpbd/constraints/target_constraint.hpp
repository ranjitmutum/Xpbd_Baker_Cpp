#pragma once

#include "xpbd/constraints/constraint.hpp"

#include <stdexcept>

namespace xpbd::constraints {


class TargetConstraint final : public Constraint {
public:
    TargetConstraint(int particleIndex, double compliance);

    void setTarget(double x, double y, double z);

    void solve(std::span<models::Particle* const> particles, double dt) override;
    void resetLambda() override;
    void setSimdMode(core::SimdMode mode) noexcept override;

private:
    using SolvePositionKernel = void (*)(double*, double*, const double*, double,
                                         double, double) noexcept;

    int particle_index_;
    double compliance_;
    double target_[3] = {0.0, 0.0, 0.0};
    double lambda_[3] = {0.0, 0.0, 0.0};
    SolvePositionKernel solve_position_kernel_ = nullptr;
};

}
