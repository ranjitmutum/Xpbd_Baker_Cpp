#pragma once

#include "xpbd/constraints/constraint.hpp"

#include <vector>

namespace xpbd::constraints {


class GroundCollisionConstraint final : public Constraint {
public:
    GroundCollisionConstraint(std::vector<int> particleIndices, int particleCount,
                              double groundY, double skin, double restitution);

    void solve(std::span<models::Particle* const> particles, double dt) override;
    void resetLambda() override;


    void projectInitial(std::span<models::Particle* const> particles);


    void postSolveVelocity(std::span<models::Particle* const> particles);

    [[nodiscard]] double minimumY() const { return minimum_y_; }

private:
    std::vector<int> particle_indices_;
    double minimum_y_;
    double restitution_;
    std::vector<bool> touched_;
    std::vector<double> desired_velocity_y_;
};

}
