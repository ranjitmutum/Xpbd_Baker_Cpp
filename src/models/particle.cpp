#include "xpbd/models/particle.hpp"

#include <algorithm>
#include <cmath>

namespace xpbd::models {

Particle::Particle(double mass) {
    if (!std::isfinite(mass) || mass < 0.0) {
        throw std::invalid_argument("mass must be zero or a finite positive value");
    }
    const double inverse = mass > 0.0 ? 1.0 / mass : 0.0;
    if (!std::isfinite(inverse)) {
        throw std::invalid_argument("mass is too small to produce a finite inverse mass");
    }
    inv_mass_ = inverse;
}

void Particle::setKinematicPosition(const Vector3& pos, double dt) {
    setKinematicPosition(pos.x, pos.y, pos.z, dt);
}

void Particle::setKinematicPosition(double x, double y, double z, double dt) {
    if (!isFixed()) {
        throw std::logic_error("kinematic positioning requires a fixed particle");
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        throw std::invalid_argument("kinematic position must be finite");
    }
    if (!std::isfinite(dt) || dt <= 0.0) {
        throw std::invalid_argument("dt must be a finite value greater than 0");
    }
    const double old_x = position_.x;
    const double old_y = position_.y;
    const double old_z = position_.z;
    prev_position_.set(old_x, old_y, old_z);
    position_.set(x, y, z);
    velocity_.set((x - old_x) / dt, (y - old_y) / dt, (z - old_z) / dt);
}

void Particle::synchronizeKinematicPosition(double x, double y, double z) {
    synchronizeKinematicPosition(x, y, z, 0.0, 0.0, 0.0, 1.0);
}

void Particle::synchronizeKinematicPosition(double x, double y, double z, double vx,
                                            double vy, double vz, double dt) {
    if (!isFixed()) {
        throw std::logic_error("kinematic positioning requires a fixed particle");
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(vx) ||
        !std::isfinite(vy) || !std::isfinite(vz) || !std::isfinite(dt) || !(dt > 0.0)) {
        throw std::invalid_argument("kinematic position must be finite");
    }
    position_.set(x, y, z);
    prev_position_.set(x - vx * dt, y - vy * dt, z - vz * dt);
    velocity_.set(vx, vy, vz);
}

void Particle::setForceMultipliers(double gravityScale, double windInfluence,
                                   double turbulenceInfluence) {
    gravity_scale_ = finiteNonNegative(gravityScale);
    wind_influence_ = finiteNonNegative(windInfluence);
    turbulence_influence_ = finiteNonNegative(turbulenceInfluence);
}

double Particle::finiteNonNegative(double value) {
    return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

}
