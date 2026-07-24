#pragma once

#include "xpbd/constraints/constraint.hpp"
#include "xpbd/core/simd_dispatch.hpp"
#include "xpbd/models/particle.hpp"
#include "xpbd/models/vector3.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace xpbd::core {





// XPBD 求解器：施加外力、投影约束，并重建粒子速度。
class XpbdEngine {
public:
  XpbdEngine() noexcept;

  void addParticle(models::Particle *particle);
  [[nodiscard]] models::Particle *getParticle(std::size_t index);
  [[nodiscard]] std::size_t particleCount() const { return particles_.size(); }

  void addConstraint(constraints::Constraint *constraint);
  [[nodiscard]] std::size_t constraintCount() const {
    return constraints_.size();
  }

  void setGravity(const models::Vector3 &g);
  void setSolverIterations(int iterations);






  void setSimdMode(SimdMode mode) noexcept;
  [[nodiscard]] const SimdSelectionDiagnostics &
  simdDiagnostics() const noexcept {
    return simd_diagnostics_;
  }






  void setAerodynamics(const models::Vector3 &velocity, double drag,
                       double turbulenceAcceleration);

  void clear();

  void step(double dt);
  void step(double dt, double forcingTime, double forcingPeriod);

  [[nodiscard]] double elapsedTime() const { return elapsed_time_; }

private:
  using DenseScaledAddKernel = void (*)(double *output, const double *base,
                                        const double *delta, double scale,
                                        std::size_t count) noexcept;

  std::vector<models::Particle *> particles_;
  std::vector<constraints::Constraint *> constraints_;
  std::vector<models::Particle *> particle_cache_;
  std::vector<std::size_t> dynamic_particle_indices_;
  std::vector<double> turbulence_phase_keys_;
  std::vector<double> prediction_positions_;
  std::vector<double> prediction_velocities_;
  bool particle_cache_dirty_ = true;
  bool dense_prediction_eligible_ = false;

  SimdSelectionDiagnostics simd_diagnostics_{};
  DenseScaledAddKernel dense_scaled_add_kernel_ = nullptr;

  models::Vector3 gravity_{0.0, -9.8, 0.0};
  models::Vector3 wind_velocity_;
  double air_drag_ = 0.0;
  double turbulence_ = 0.0;
  double elapsed_time_ = 0.0;
  int solver_iterations_ = 4;

  static double wrappedPhase(double time, double period);
  static models::Vector3 finiteVector(const models::Vector3 &value);
  void rebuildParticleCache();
};

}
