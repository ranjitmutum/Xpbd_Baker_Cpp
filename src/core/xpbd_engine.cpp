#include "xpbd/core/xpbd_engine.hpp"

#if defined(XPBD_HAS_X86_SIMD)
#include "simd_kernels.hpp"
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>

namespace xpbd::core {

namespace {

std::uint64_t appendFnv1a(std::uint64_t hash, double value) {
  const double canonical = !std::isfinite(value) || value == 0.0 ? 0.0 : value;
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(canonical);
  for (unsigned shift = 0; shift < 64; shift += 8) {
    hash ^= (bits >> shift) & 0xffu;
    hash *= 1099511628211ull;
  }
  return hash;
}

double stableTurbulencePhaseKey(const models::Particle &particle) {



  std::uint64_t hash = 14695981039346656037ull;
  hash = appendFnv1a(hash, particle.position().x);
  hash = appendFnv1a(hash, particle.position().y);
  hash = appendFnv1a(hash, particle.position().z);
  return static_cast<double>(hash & 0xffffu) * 0.001;
}

#if !defined(XPBD_HAS_X86_SIMD)
void denseScaledAddScalar(double *output, const double *base,
                          const double *delta, double scale,
                          std::size_t count) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    output[index] = base[index] + delta[index] * scale;
  }
}
#endif

}

XpbdEngine::XpbdEngine() noexcept { setSimdMode(SimdMode::Auto); }

void XpbdEngine::setSimdMode(SimdMode mode) noexcept {
  simd_diagnostics_ = simdSelectionDiagnostics(mode);
#if defined(XPBD_HAS_X86_SIMD)
  dense_scaled_add_kernel_ = simd_diagnostics_.selected == SimdMode::AVX2
                                 ? detail::denseScaledAddAvx2
                                 : detail::denseScaledAddSse2;
#else
  dense_scaled_add_kernel_ = denseScaledAddScalar;
#endif
  for (constraints::Constraint *constraint : constraints_) {
    constraint->setSimdMode(simd_diagnostics_.selected);
  }
}

void XpbdEngine::addParticle(models::Particle *particle) {
  if (particle == nullptr) {
    throw std::invalid_argument("particle");
  }
  particles_.push_back(particle);
  turbulence_phase_keys_.push_back(stableTurbulencePhaseKey(*particle));
  particle_cache_dirty_ = true;
}

models::Particle *XpbdEngine::getParticle(std::size_t index) {
  return particles_.at(index);
}

void XpbdEngine::addConstraint(constraints::Constraint *constraint) {
  if (constraint == nullptr) {
    throw std::invalid_argument("constraint");
  }
  constraints_.push_back(constraint);
  constraint->setSimdMode(simd_diagnostics_.selected);
}

void XpbdEngine::setGravity(const models::Vector3 &g) {
  gravity_ = finiteVector(g);
}

void XpbdEngine::setSolverIterations(int iterations) {
  solver_iterations_ = std::max(1, iterations);
}

void XpbdEngine::setAerodynamics(const models::Vector3 &velocity, double drag,
                                 double turbulenceAcceleration) {
  wind_velocity_ = finiteVector(velocity);
  air_drag_ = std::isfinite(drag) ? std::max(0.0, drag) : 0.0;
  turbulence_ = std::isfinite(turbulenceAcceleration)
                    ? std::max(0.0, turbulenceAcceleration)
                    : 0.0;
}

void XpbdEngine::clear() {
  particles_.clear();
  constraints_.clear();
  particle_cache_.clear();
  dynamic_particle_indices_.clear();
  turbulence_phase_keys_.clear();
  prediction_positions_.clear();
  prediction_velocities_.clear();
  particle_cache_dirty_ = true;
  dense_prediction_eligible_ = false;
  elapsed_time_ = 0.0;
}

void XpbdEngine::step(double dt) { step(dt, elapsed_time_ + dt, 0.0); }

void XpbdEngine::step(double dt, double forcingTime, double forcingPeriod) {
  if (!std::isfinite(dt) || dt <= 0.0) {
    throw std::invalid_argument("dt must be a finite value greater than 0");
  }
  if (!std::isfinite(forcingTime) || !std::isfinite(forcingPeriod) ||
      forcingPeriod < 0.0) {
    throw std::invalid_argument("forcing time/period must be finite");
  }

  rebuildParticleCache();
  auto &pArray = particle_cache_;

  for (constraints::Constraint *c : constraints_) {
    c->resetLambda();
  }

  auto updateVelocity = [&](models::Particle *p,
                            std::size_t particle_index) -> models::Vector3 & {
    models::Vector3 &velocity = p->velocity();
    const double gravityFactor = dt * p->gravityScale();
    velocity.x += gravity_.x * gravityFactor;
    velocity.y += gravity_.y * gravityFactor;
    velocity.z += gravity_.z * gravityFactor;

    if (air_drag_ > 0.0 && p->windInfluence() > 0.0) {

      const double response =
          1.0 - std::exp(-air_drag_ * p->windInfluence() * dt);
      velocity.x += (wind_velocity_.x - velocity.x) * response;
      velocity.y += (wind_velocity_.y - velocity.y) * response;
      velocity.z += (wind_velocity_.z - velocity.z) * response;
    }

    if (turbulence_ > 0.0 && p->turbulenceInfluence() > 0.0) {

      const double basePhase =
          forcingPeriod > 0.0 ? 2.0 * 3.14159265358979323846 *
                                    wrappedPhase(forcingTime, forcingPeriod)
                              : forcingTime * 3.7;
      const double stablePhase = turbulence_phase_keys_[particle_index];
      const double gust = turbulence_ * p->turbulenceInfluence();
      velocity.x +=
          std::sin(basePhase + stablePhase * 1.61803398875) * gust * dt;
      velocity.y += std::sin(basePhase * 2.0 + stablePhase * 0.73 + 1.7) *
                    gust * 0.15 * dt;
      velocity.z +=
          std::cos(basePhase * 3.0 + stablePhase * 1.13 + 0.4) * gust * dt;
    }
    return velocity;
  };








  // Auto 模式同样走 dense 预测路径：内核已在 setSimdMode 中按 CPU 能力
  // 选定（AVX2 优先，回退 SSE2），与显式指定模式共用同一条代码路径。
  if (dense_prediction_eligible_) {
    for (std::size_t dense_index = 0;
         dense_index < dynamic_particle_indices_.size(); ++dense_index) {
      const std::size_t particle_index = dynamic_particle_indices_[dense_index];
      models::Particle *p = pArray[particle_index];
      p->prevPosition().set(p->position());
      const models::Vector3 &velocity = updateVelocity(p, particle_index);
      const std::size_t value_index = dense_index * 3;
      prediction_positions_[value_index] = p->position().x;
      prediction_positions_[value_index + 1] = p->position().y;
      prediction_positions_[value_index + 2] = p->position().z;
      prediction_velocities_[value_index] = velocity.x;
      prediction_velocities_[value_index + 1] = velocity.y;
      prediction_velocities_[value_index + 2] = velocity.z;
    }
    if (!prediction_positions_.empty()) {
      dense_scaled_add_kernel_(
          prediction_positions_.data(), prediction_positions_.data(),
          prediction_velocities_.data(), dt, prediction_positions_.size());
    }
    for (std::size_t dense_index = 0;
         dense_index < dynamic_particle_indices_.size(); ++dense_index) {
      models::Particle *p = pArray[dynamic_particle_indices_[dense_index]];
      const std::size_t value_index = dense_index * 3;
      p->position().set(prediction_positions_[value_index],
                        prediction_positions_[value_index + 1],
                        prediction_positions_[value_index + 2]);
    }
  } else {
    for (std::size_t particle_index = 0; particle_index < pArray.size();
         ++particle_index) {
      models::Particle *p = pArray[particle_index];
      if (p->isFixed()) {
        continue;
      }
      p->prevPosition().set(p->position());
      const models::Vector3 &velocity = updateVelocity(p, particle_index);
      p->position().x += velocity.x * dt;
      p->position().y += velocity.y * dt;
      p->position().z += velocity.z * dt;
    }
  }


  for (int iter = 0; iter < solver_iterations_; ++iter) {
    for (constraints::Constraint *c : constraints_) {
      c->solve(pArray, dt);
    }
  }


  for (models::Particle *p : pArray) {
    if (p->isFixed()) {
      continue;
    }
    const models::Vector3 &newPos = p->position();
    const models::Vector3 &oldPos = p->prevPosition();
    p->velocity().set((newPos.x - oldPos.x) / dt, (newPos.y - oldPos.y) / dt,
                      (newPos.z - oldPos.z) / dt);
  }
  elapsed_time_ += dt;
}

double XpbdEngine::wrappedPhase(double time, double period) {
  double wrapped = std::fmod(time, period);
  if (wrapped < 0.0) {
    wrapped += period;
  }
  return wrapped / period;
}

models::Vector3 XpbdEngine::finiteVector(const models::Vector3 &value) {
  return models::Vector3(std::isfinite(value.x) ? value.x : 0.0,
                         std::isfinite(value.y) ? value.y : 0.0,
                         std::isfinite(value.z) ? value.z : 0.0);
}

void XpbdEngine::rebuildParticleCache() {
  if (!particle_cache_dirty_) {
    return;
  }
  particle_cache_ = particles_;
  dynamic_particle_indices_.clear();
  dynamic_particle_indices_.reserve(particle_cache_.size());
  std::unordered_set<models::Particle *> unique_dynamic_particles;
  bool unique_dynamic_registration = true;
  for (std::size_t index = 0; index < particle_cache_.size(); ++index) {
    if (!particle_cache_[index]->isFixed()) {
      dynamic_particle_indices_.push_back(index);
      unique_dynamic_registration &=
          unique_dynamic_particles.insert(particle_cache_[index]).second;
    }
  }
  const std::size_t dense_value_count = dynamic_particle_indices_.size() * 3;
  prediction_positions_.resize(dense_value_count);
  prediction_velocities_.resize(dense_value_count);
#if defined(XPBD_HAS_X86_SIMD)
  dense_prediction_eligible_ = unique_dynamic_registration;
#else
  dense_prediction_eligible_ = false;
#endif
  particle_cache_dirty_ = false;
}

}
