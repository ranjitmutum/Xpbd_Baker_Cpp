#include "xpbd/baker/loop_bake_config.hpp"

namespace xpbd::baker {

namespace {
void requireTolerance(double value, const char* label) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument(std::string(label) +
                                    " must be finite and non-negative");
    }
}
}

LoopBakeConfig LoopBakeConfig::from(const BoneMapper::PhysicsGroupConfig& config) {
    LoopBakeConfig c;
    c.minimum_warmup_cycles = config.minimum_warmup_cycles;
    c.maximum_warmup_cycles = config.maximum_warmup_cycles;
    c.required_stable_cycles = config.required_stable_cycles;
    c.position_tolerance = config.loop_position_tolerance;
    c.rotation_tolerance_radians =
        config.loop_rotation_tolerance_degrees * 3.14159265358979323846 / 180.0;
    c.linear_velocity_tolerance = config.loop_linear_velocity_tolerance;
    c.angular_velocity_tolerance = config.loop_angular_velocity_tolerance;
    c.maximum_penetration_tolerance =
        config.simulation_mode == BoneMapper::SimulationMode::RigidBody
            ? config.rigid_body_maximum_safe_penetration
            : config.collision_skin;
    c.seam_fallback_enabled = config.loop_seam_fallback_enabled;

    if (c.minimum_warmup_cycles < 1 || c.maximum_warmup_cycles < c.minimum_warmup_cycles ||
        c.required_stable_cycles < 1) {
        throw std::invalid_argument("invalid loop cycle limits");
    }
    requireTolerance(c.position_tolerance, "position tolerance");
    requireTolerance(c.rotation_tolerance_radians, "rotation tolerance");
    requireTolerance(c.linear_velocity_tolerance, "linear velocity tolerance");
    requireTolerance(c.angular_velocity_tolerance, "angular velocity tolerance");
    if (std::isnan(c.maximum_penetration_tolerance) || c.maximum_penetration_tolerance < 0) {
        throw std::invalid_argument("maximum penetration tolerance must be non-negative");
    }
    return c;
}

}
