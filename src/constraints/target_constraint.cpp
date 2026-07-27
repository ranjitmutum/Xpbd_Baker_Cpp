#include "xpbd/constraints/target_constraint.hpp"
#include "xpbd/core/simd_dispatch.hpp"

#if defined(XPBD_HAS_X86_SIMD)
#include "../core/simd_kernels.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace xpbd::constraints {
namespace {

#if !defined(XPBD_HAS_X86_SIMD)
void solveTargetPositionScalar(double *position, double *lambda,
                               const double *target, double alpha,
                               double weight, double denominator) noexcept {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const double delta =
        -(position[axis] - target[axis] + alpha * lambda[axis]) / denominator;
    lambda[axis] += delta;
    position[axis] += weight * delta;
  }
}
#endif

}

TargetConstraint::TargetConstraint(int particleIndex, double compliance)
    : particle_index_(particleIndex),
      compliance_(std::isfinite(compliance) ? std::max(0.0, compliance)
                                            : 0.0) {
  setSimdMode(core::SimdMode::Auto);
}

void TargetConstraint::setTarget(double x, double y, double z) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    throw std::invalid_argument("target position must be finite");
  }
  target_[0] = x;
  target_[1] = y;
  target_[2] = z;
}

void TargetConstraint::solve(
    std::span<models::Particle *const> particles, double dt) {
  models::Particle &particle =
      *particles[static_cast<std::size_t>(particle_index_)];
  const double weight = particle.invMass();
  const double alpha = compliance_ / (dt * dt);
  const double denominator = weight + alpha;
  if (!std::isfinite(denominator) || denominator <= 0.0) {
    return;
  }

  static_assert(offsetof(models::Vector3, y) == sizeof(double));
  static_assert(offsetof(models::Vector3, z) == sizeof(double) * 2);
  solve_position_kernel_(&particle.position().x, lambda_, target_, alpha,
                         weight, denominator);
}

void TargetConstraint::resetLambda() {
  lambda_[0] = 0.0;
  lambda_[1] = 0.0;
  lambda_[2] = 0.0;
}

void TargetConstraint::setSimdMode(core::SimdMode mode) noexcept {
#if defined(XPBD_HAS_X86_SIMD)
  solve_position_kernel_ =
      core::detail::selectedSimdKernelTable(mode).target_position;
#else
  (void)mode;
  solve_position_kernel_ = solveTargetPositionScalar;
#endif
}

}
