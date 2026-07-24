#include "xpbd/constraints/cross_spring_constraint.hpp"

#include <algorithm>
#include <cmath>

namespace xpbd::constraints {

CrossSpringConstraint::CrossSpringConstraint(int idxA, int idxC, double minDistance,
                                             double maxDistance, double compliance,
                                             double fallbackX, double fallbackY,
                                             double fallbackZ)
    : idx_a_(idxA), idx_c_(idxC), compliance_(finiteNonNegative(compliance)) {
    const double safeMin = finiteNonNegative(minDistance);
    const double safeMax = finiteNonNegative(maxDistance);
    min_distance_ = std::min(safeMin, safeMax);
    max_distance_ = std::max(safeMin, safeMax);
    const double length =
        std::sqrt(fallbackX * fallbackX + fallbackY * fallbackY + fallbackZ * fallbackZ);
    if (std::isfinite(length) && length > kEpsilon) {
        fallback_x_ = fallbackX / length;
        fallback_y_ = fallbackY / length;
        fallback_z_ = fallbackZ / length;
    } else {
        fallback_x_ = 1.0;
        fallback_y_ = 0.0;
        fallback_z_ = 0.0;
    }
}

void CrossSpringConstraint::solve(std::span<models::Particle* const> particles, double dt) {
    (void)dt;
    models::Particle& a = *particles[static_cast<std::size_t>(idx_a_)];
    models::Particle& c = *particles[static_cast<std::size_t>(idx_c_)];
    const double dx = c.position().x - a.position().x;
    const double dy = c.position().y - a.position().y;
    const double dz = c.position().z - a.position().z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    int side = 0;
    double boundary = 0.0;
    if (distance < min_distance_) {
        side = -1;
        boundary = min_distance_;
    } else if (distance > max_distance_) {
        side = 1;
        boundary = max_distance_;
    } else {
        lambda_ = 0.0;
        active_side_ = 0;
        return;
    }
    if (side != active_side_) {
        lambda_ = 0.0;
    }
    active_side_ = side;

    double nx = 0.0;
    double ny = 0.0;
    double nz = 0.0;
    if (distance > kEpsilon) {
        nx = dx / distance;
        ny = dy / distance;
        nz = dz / distance;
    } else {
        nx = fallback_x_;
        ny = fallback_y_;
        nz = fallback_z_;
    }
    const double wa = a.invMass();
    const double wc = c.invMass();
    const double alpha = compliance_ / (dt * dt);
    const double denominator = wa + wc + alpha;
    if (!std::isfinite(denominator) || denominator <= 0.0) {
        return;
    }
    const double value = distance - boundary;
    const double deltaLambda = -(value + alpha * lambda_) / denominator;
    lambda_ += deltaLambda;
    a.position().x -= nx * deltaLambda * wa;
    a.position().y -= ny * deltaLambda * wa;
    a.position().z -= nz * deltaLambda * wa;
    c.position().x += nx * deltaLambda * wc;
    c.position().y += ny * deltaLambda * wc;
    c.position().z += nz * deltaLambda * wc;
}

void CrossSpringConstraint::resetLambda() {
    lambda_ = 0.0;
    active_side_ = 0;
}

double CrossSpringConstraint::finiteNonNegative(double value) {
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

}
