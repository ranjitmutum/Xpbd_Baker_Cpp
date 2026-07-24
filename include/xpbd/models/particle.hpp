#pragma once

#include "xpbd/models/vector3.hpp"

#include <stdexcept>

namespace xpbd::models {


class Particle {
public:
    explicit Particle(double mass);

    [[nodiscard]] Vector3& position() { return position_; }
    [[nodiscard]] const Vector3& position() const { return position_; }
    void setPosition(const Vector3& pos) { position_.set(pos); }

    [[nodiscard]] Vector3& prevPosition() { return prev_position_; }
    [[nodiscard]] const Vector3& prevPosition() const { return prev_position_; }
    void setPrevPosition(const Vector3& pos) { prev_position_.set(pos); }

    [[nodiscard]] Vector3& velocity() { return velocity_; }
    [[nodiscard]] const Vector3& velocity() const { return velocity_; }
    void setVelocity(const Vector3& vel) { velocity_.set(vel); }





    void setKinematicPosition(const Vector3& pos, double dt);
    void setKinematicPosition(double x, double y, double z, double dt);


    void synchronizeKinematicPosition(double x, double y, double z);


    void synchronizeKinematicPosition(double x, double y, double z,
                                      double vx, double vy, double vz, double dt);

    [[nodiscard]] double invMass() const { return inv_mass_; }
    [[nodiscard]] bool isFixed() const { return inv_mass_ == 0.0; }

    [[nodiscard]] double gravityScale() const { return gravity_scale_; }
    [[nodiscard]] double windInfluence() const { return wind_influence_; }
    [[nodiscard]] double turbulenceInfluence() const { return turbulence_influence_; }

    void setForceMultipliers(double gravityScale, double windInfluence,
                             double turbulenceInfluence);

private:
    Vector3 position_;
    Vector3 prev_position_;
    Vector3 velocity_;
    double inv_mass_;
    double gravity_scale_ = 1.0;
    double wind_influence_ = 1.0;
    double turbulence_influence_ = 1.0;

    static double finiteNonNegative(double value);
};

}
