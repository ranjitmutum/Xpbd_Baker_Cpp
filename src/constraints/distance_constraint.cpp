#include "xpbd/constraints/distance_constraint.hpp"

#include <algorithm>
#include <cmath>

namespace xpbd::constraints {

DistanceConstraint::DistanceConstraint(int idxA, int idxB, double restLength,
                                       double compliance, double dampingCompliance)
    : idx_a_(idxA),
      idx_b_(idxB),
      rest_length_(finiteNonNegative(restLength)),
      compliance_(finiteNonNegative(compliance)),
      damping_compliance_(finiteNonNegative(dampingCompliance)) {}

void DistanceConstraint::solve(std::span<models::Particle* const> particles, double dt) {
    models::Particle& pA = *particles[static_cast<std::size_t>(idx_a_)];
    models::Particle& pB = *particles[static_cast<std::size_t>(idx_b_)];
    const double invMassA = pA.invMass();
    const double invMassB = pB.invMass();
    if (invMassA == 0.0 && invMassB == 0.0) {
        return;
    }

    const double dx = pB.position().x - pA.position().x;
    const double dy = pB.position().y - pA.position().y;
    const double dz = pB.position().z - pA.position().z;
    const double distSquared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distSquared)) {
        return;
    }
    const double dist = std::sqrt(distSquared);
    double nx = 0.0;
    double ny = 0.0;
    double nz = 0.0;
    if (dist > 1e-8) {
        nx = dx / dist;
        ny = dy / dist;
        nz = dz / dist;
        normal_x_ = nx;
        normal_y_ = ny;
        normal_z_ = nz;
    } else if (rest_length_ > 0.0) {
        nx = normal_x_;
        ny = normal_y_;
        nz = normal_z_;
    } else {
        return;
    }

    const double C = dist - rest_length_;


    const double velocityX =
        ((pB.position().x - pB.prevPosition().x) - (pA.position().x - pA.prevPosition().x)) / dt;
    const double velocityY =
        ((pB.position().y - pB.prevPosition().y) - (pA.position().y - pA.prevPosition().y)) / dt;
    const double velocityZ =
        ((pB.position().z - pB.prevPosition().z) - (pA.position().z - pA.prevPosition().z)) / dt;
    const double Cdot = nx * velocityX + ny * velocityY + nz * velocityZ;

    const double alphaTilde = compliance_ / (dt * dt);
    const double gamma = damping_compliance_ / dt;
    const double invMassSum = invMassA + invMassB;
    const double denominator = (1.0 + gamma) * invMassSum + alphaTilde;
    if (!std::isfinite(denominator) || denominator <= 0.0) {
        return;
    }

    const double deltaLambda = -(C + alphaTilde * lambda_ + gamma * dt * Cdot) / denominator;

    pA.position().x -= nx * deltaLambda * invMassA;
    pA.position().y -= ny * deltaLambda * invMassA;
    pA.position().z -= nz * deltaLambda * invMassA;
    pB.position().x += nx * deltaLambda * invMassB;
    pB.position().y += ny * deltaLambda * invMassB;
    pB.position().z += nz * deltaLambda * invMassB;

    lambda_ += deltaLambda;
}

void DistanceConstraint::resetLambda() { lambda_ = 0.0; }

double DistanceConstraint::finiteNonNegative(double value) {
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

}
