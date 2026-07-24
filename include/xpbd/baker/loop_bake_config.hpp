#pragma once

#include "xpbd/baker/bone_mapper.hpp"

#include <cmath>
#include <stdexcept>

namespace xpbd::baker {

struct LoopBakeConfig {
    int minimum_warmup_cycles = 2;
    int maximum_warmup_cycles = 12;
    int required_stable_cycles = 2;
    double position_tolerance = 0.001;
    double rotation_tolerance_radians = 0.1 * 3.14159265358979323846 / 180.0;
    double linear_velocity_tolerance = 0.01;
    double angular_velocity_tolerance = 0.01;
    double maximum_penetration_tolerance = 0.1;
    bool seam_fallback_enabled = true;

    static LoopBakeConfig from(const BoneMapper::PhysicsGroupConfig& config);
};

}
