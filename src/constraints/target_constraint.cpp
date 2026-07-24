#include "xpbd/constraints/target_constraint.hpp"

#include <algorithm>
#include <cmath>

namespace xpbd::constraints {

TargetConstraint::TargetConstraint(int particleIndex, double compliance)
    : particle_index_(particleIndex),
      compliance_(std::isfinite(compliance) ? std::max(0.0, compliance) : 0.0) {}

void TargetConstraint::setTarget(double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        throw std::invalid_argument("target position must be finite");
    }
    target_[0] = x;
    target_[1] = y;
    target_[2] = z;
}

void TargetConstraint::solve(std::span<models::Particle* const> particles, double dt) {
    models::Particle& particle = *particles[static_cast<std::size_t>(particle_index_)];
    const double weight = particle.invMass();
    const double alpha = compliance_ / (dt * dt);
    const double denominator = weight + alpha;
    if (!std::isfinite(denominator) || denominator <= 0.0) {
        return;
    }


    const double dx =
        -(particle.position().x - target_[0] + alpha * lambda_[0]) / denominator;
    const double dy =
        -(particle.position().y - target_[1] + alpha * lambda_[1]) / denominator;
    const double dz =
        -(particle.position().z - target_[2] + alpha * lambda_[2]) / denominator;
    lambda_[0] += dx;
    lambda_[1] += dy;
    lambda_[2] += dz;
    particle.position().x += weight * dx;
    particle.position().y += weight * dy;
    particle.position().z += weight * dz;
}

void TargetConstraint::resetLambda() {
    lambda_[0] = 0.0;
    lambda_[1] = 0.0;
    lambda_[2] = 0.0;
}

}
