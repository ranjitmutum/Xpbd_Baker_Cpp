#include "xpbd/constraints/weld_constraint.hpp"

#include <algorithm>
#include <cmath>

namespace xpbd::constraints {

WeldConstraint::WeldConstraint(int idxA, int idxB, double compliance,
                               double dampingCompliance)
    : idx_a_(idxA),
      idx_b_(idxB),
      compliance_(finiteNonNegative(compliance)),
      damping_compliance_(finiteNonNegative(dampingCompliance)) {}

void WeldConstraint::solve(std::span<models::Particle* const> particles, double dt) {
    models::Particle& a = *particles[static_cast<std::size_t>(idx_a_)];
    models::Particle& b = *particles[static_cast<std::size_t>(idx_b_)];
    const double wa = a.invMass();
    const double wb = b.invMass();
    const double weight = wa + wb;
    const double alpha = compliance_ / (dt * dt);
    const double gamma = damping_compliance_ / dt;
    const double denominator = (1.0 + gamma) * weight + alpha;
    if (!std::isfinite(denominator) || denominator <= 0.0) {
        return;
    }

    solveAxis(a, b, 0, b.position().x - a.position().x,
              implicitVelocityX(b, dt) - implicitVelocityX(a, dt), wa, wb, alpha, gamma, dt,
              denominator);
    solveAxis(a, b, 1, b.position().y - a.position().y,
              implicitVelocityY(b, dt) - implicitVelocityY(a, dt), wa, wb, alpha, gamma, dt,
              denominator);
    solveAxis(a, b, 2, b.position().z - a.position().z,
              implicitVelocityZ(b, dt) - implicitVelocityZ(a, dt), wa, wb, alpha, gamma, dt,
              denominator);
}

void WeldConstraint::solveAxis(models::Particle& a, models::Particle& b, int axis, double value,
                               double derivative, double wa, double wb, double alpha,
                               double gamma, double dt, double denominator) {
    const double deltaLambda =
        -(value + alpha * lambda_[axis] + gamma * dt * derivative) / denominator;
    lambda_[axis] += deltaLambda;
    if (axis == 0) {
        a.position().x -= wa * deltaLambda;
        b.position().x += wb * deltaLambda;
    } else if (axis == 1) {
        a.position().y -= wa * deltaLambda;
        b.position().y += wb * deltaLambda;
    } else {
        a.position().z -= wa * deltaLambda;
        b.position().z += wb * deltaLambda;
    }
}

double WeldConstraint::implicitVelocityX(const models::Particle& p, double dt) {
    return (p.position().x - p.prevPosition().x) / dt;
}

double WeldConstraint::implicitVelocityY(const models::Particle& p, double dt) {
    return (p.position().y - p.prevPosition().y) / dt;
}

double WeldConstraint::implicitVelocityZ(const models::Particle& p, double dt) {
    return (p.position().z - p.prevPosition().z) / dt;
}

void WeldConstraint::resetLambda() {
    lambda_[0] = 0.0;
    lambda_[1] = 0.0;
    lambda_[2] = 0.0;
}

double WeldConstraint::finiteNonNegative(double value) {
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

}
