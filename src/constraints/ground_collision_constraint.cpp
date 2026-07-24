#include "xpbd/constraints/ground_collision_constraint.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xpbd::constraints {

GroundCollisionConstraint::GroundCollisionConstraint(std::vector<int> particleIndices,
                                                     int particleCount, double groundY,
                                                     double skin, double restitution)
    : particle_indices_(std::move(particleIndices)), restitution_(restitution) {
    if (particleCount < 0) {
        throw std::invalid_argument("invalid particle count");
    }
    if (!std::isfinite(groundY) || !std::isfinite(skin) || skin < 0.0) {
        throw std::invalid_argument(
            "ground height and collision skin must be finite and non-negative where applicable");
    }
    if (!std::isfinite(restitution) || restitution < 0.0 || restitution > 1.0) {
        throw std::invalid_argument("ground restitution must be between zero and one");
    }
    for (int index : particle_indices_) {
        if (index < 0 || index >= particleCount) {
            throw std::invalid_argument("ground particle index is out of range");
        }
    }
    minimum_y_ = groundY + skin;
    touched_.assign(static_cast<std::size_t>(particleCount), false);
    desired_velocity_y_.assign(static_cast<std::size_t>(particleCount), 0.0);
}

void GroundCollisionConstraint::solve(std::span<models::Particle* const> particles,
                                      double dt) {
    for (int index : particle_indices_) {
        models::Particle& particle = *particles[static_cast<std::size_t>(index)];
        if (particle.isFixed()) {
            continue;
        }
        models::Vector3& position = particle.position();
        models::Vector3& previous = particle.prevPosition();
        if (!std::isfinite(position.y) || !std::isfinite(previous.y) ||
            position.y >= minimum_y_) {
            continue;
        }
        if (!touched_[static_cast<std::size_t>(index)]) {
            const double incomingVelocity = (position.y - previous.y) / dt;
            desired_velocity_y_[static_cast<std::size_t>(index)] =
                incomingVelocity < 0.0 ? -restitution_ * incomingVelocity : incomingVelocity;
            touched_[static_cast<std::size_t>(index)] = true;
        }
        position.y = minimum_y_;
    }
}

void GroundCollisionConstraint::projectInitial(std::span<models::Particle* const> particles) {
    for (int index : particle_indices_) {
        models::Particle& particle = *particles[static_cast<std::size_t>(index)];
        if (particle.isFixed() || particle.position().y >= minimum_y_) {
            continue;
        }
        const double correction = minimum_y_ - particle.position().y;
        particle.position().y += correction;
        particle.prevPosition().y += correction;
        particle.velocity().y = 0.0;
    }
    resetLambda();
}

void GroundCollisionConstraint::postSolveVelocity(
    std::span<models::Particle* const> particles) {
    for (int index : particle_indices_) {
        if (touched_[static_cast<std::size_t>(index)]) {
            particles[static_cast<std::size_t>(index)]->velocity().y =
                desired_velocity_y_[static_cast<std::size_t>(index)];
        }
    }
}

void GroundCollisionConstraint::resetLambda() {
    std::fill(touched_.begin(), touched_.end(), false);
}

}
