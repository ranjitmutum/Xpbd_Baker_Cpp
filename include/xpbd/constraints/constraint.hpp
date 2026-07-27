#pragma once

#include "xpbd/models/particle.hpp"

#include <cstdint>
#include <span>

namespace xpbd::core {
enum class SimdMode : std::uint8_t;
}

namespace xpbd::constraints {





// 所有 XPBD 约束的统一接口；每次子步由求解器调用 solve。
class Constraint {
public:
    virtual ~Constraint() = default;
    virtual void solve(std::span<models::Particle* const> particles, double dt) = 0;
    virtual void resetLambda() = 0;
    virtual void setSimdMode(core::SimdMode) noexcept {}
};

}
