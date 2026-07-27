#include "xpbd/constraints/vertex_face_collision_constraint.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace xpbd::constraints {
namespace {

bool finiteVec(const models::Vector3 &v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}

VertexFaceCollisionConstraint::VertexFaceCollisionConstraint(
    std::vector<int> particle_indices, int particle_count,
    baker::BodyColliderCache &cache, double skin, double restitution)
    : particle_indices_(std::move(particle_indices)), cache_(cache),
      skin_(skin), restitution_(restitution) {
  if (!std::isfinite(skin) || skin < 0.0) {
    throw std::invalid_argument(
        "collision skin must be finite and non-negative");
  }
  if (!std::isfinite(restitution) || restitution < 0.0 || restitution > 1.0) {
    throw std::invalid_argument(
        "collision restitution must be between zero and one");
  }
  if (particle_count < 0) {
    throw std::invalid_argument("invalid particle count");
  }
  for (int index : particle_indices_) {
    if (index < 0 || index >= particle_count) {
      throw std::invalid_argument("collision particle index is out of range");
    }
  }
  epsilon_ = std::max(cache_.epsilon(), 1e-12);
  slop_ = std::max(epsilon_ * 8.0, std::max(1e-7, skin_ * 1e-5));
  const auto n = static_cast<std::size_t>(particle_count);
  touched_.assign(n, false);
  fixed_reported_.assign(n, false);
  normal_x_.assign(n, 0.0);
  normal_y_.assign(n, 0.0);
  normal_z_.assign(n, 0.0);
  face_velocity_x_.assign(n, 0.0);
  face_velocity_y_.assign(n, 0.0);
  face_velocity_z_.assign(n, 0.0);
  desired_relative_normal_.assign(n, 0.0);
  candidate_indices_.reserve(cache_.colliders().size());
  nested_candidate_indices_.reserve(cache_.colliders().size());
}

void VertexFaceCollisionConstraint::solve(
    std::span<models::Particle *const> particles, double dt) {
  if (cache_.colliders().empty()) {
    return;
  }
  for (int particle_index : particle_indices_) {
    models::Particle &particle =
        *particles[static_cast<std::size_t>(particle_index)];
    models::Vector3 &position = particle.position();
    models::Vector3 &previous = particle.prevPosition();
    if (!finiteVec(position) || !finiteVec(previous)) {
      invalid_value_count_++;
      continue;
    }
    if (particle.isFixed()) {
      if (!fixed_reported_[static_cast<std::size_t>(particle_index)] &&
          containsCurrent(position.x, position.y, position.z)) {
        fixed_inside_count_++;
        fixed_reported_[static_cast<std::size_t>(particle_index)] = true;
      }
      continue;
    }
    const bool sweep_eligible =
        cache_.isSweepContinuous() &&
        !containsPrevious(previous.x, previous.y, previous.z);
    if (sweep_eligible && projectSweep(particle, particle_index, dt)) {
      continue;
    }
    projectDiscrete(particle, particle_index, dt, true);
  }
}

void VertexFaceCollisionConstraint::projectInitial(
    std::span<models::Particle *const> particles) {
  if (cache_.colliders().empty()) {
    return;
  }
  const int maximum_passes =
      std::max(4, static_cast<int>(cache_.colliders().size()) * 2);
  for (int particle_index : particle_indices_) {
    models::Particle &particle =
        *particles[static_cast<std::size_t>(particle_index)];
    models::Vector3 &position = particle.position();
    if (!finiteVec(position)) {
      invalid_value_count_++;
      continue;
    }
    if (!containsCurrent(position.x, position.y, position.z)) {
      continue;
    }
    if (particle.isFixed()) {
      fixed_inside_count_++;
      continue;
    }
    initial_embedded_count_++;
    for (int pass = 0; pass < maximum_passes; ++pass) {
      const double before_x = position.x;
      const double before_y = position.y;
      const double before_z = position.z;
      if (!projectDiscrete(particle, particle_index, 1.0, false)) {
        break;
      }
      particle.prevPosition().x += position.x - before_x;
      particle.prevPosition().y += position.y - before_y;
      particle.prevPosition().z += position.z - before_z;
      if (!containsCurrent(position.x, position.y, position.z)) {
        break;
      }
    }
    particle.velocity().set(0, 0, 0);
  }
  resetLambda();
}

bool VertexFaceCollisionConstraint::projectDiscrete(models::Particle &particle,
                                                    int particle_index,
                                                    double dt,
                                                    bool record_velocity) {
  models::Vector3 &position = particle.position();
  const baker::BodyColliderCache::Collider *chosen_collider = nullptr;
  double chosen_distance_sq = std::numeric_limits<double>::infinity();
  int chosen_remaining_inside = std::numeric_limits<int>::max();
  int containing_count = 0;

  queryCurrentCandidates(position.x, position.y, position.z,
                         candidate_indices_);
  for (const std::size_t collider_index : candidate_indices_) {
    const auto &collider = cache_.colliders()[collider_index];
    narrow_phase_test_count_++;
    if (!collider.containsCurrent(position.x, position.y, position.z, skin_)) {
      continue;
    }
    containing_count++;
    for (int face = 0; face < 6; ++face) {
      const double signed_distance = collider.signedDistanceCurrent(
          face, position.x, position.y, position.z);
      const double correction = skin_ + slop_ - signed_distance;
      if (!std::isfinite(correction) || correction <= 0.0) {
        continue;
      }
      const double nx = collider.getCurrentNormal(face, 0);
      const double ny = collider.getCurrentNormal(face, 1);
      const double nz = collider.getCurrentNormal(face, 2);
      const double target_x = position.x + nx * correction;
      const double target_y = position.y + ny * correction;
      const double target_z = position.z + nz * correction;
      const int remaining_inside =
          countContainingCurrent(target_x, target_y, target_z);
      const double distance_sq = correction * correction;
      if (remaining_inside < chosen_remaining_inside ||
          (remaining_inside == chosen_remaining_inside &&
           distance_sq < chosen_distance_sq)) {
        chosen_collider = &collider;
        chosen_face_ = face;
        chosen_x_ = target_x;
        chosen_y_ = target_y;
        chosen_z_ = target_z;
        chosen_distance_sq = distance_sq;
        chosen_remaining_inside = remaining_inside;
      }
    }
  }
  if (chosen_collider == nullptr) {
    return false;
  }
  if (containing_count > 1 && chosen_remaining_inside > 0) {
    overlap_no_exit_count_++;
  }
  const double before_x = position.x;
  const double before_y = position.y;
  const double before_z = position.z;
  position.set(chosen_x_, chosen_y_, chosen_z_);
  if (record_velocity) {
    recordContact(particle, particle_index, *chosen_collider, chosen_face_,
                  before_x, before_y, before_z, dt);
  }
  return true;
}

int VertexFaceCollisionConstraint::countContainingCurrent(double x, double y,
                                                          double z) {
  int count = 0;
  queryCurrentCandidates(x, y, z, nested_candidate_indices_);
  for (const std::size_t collider_index : nested_candidate_indices_) {
    const auto &c = cache_.colliders()[collider_index];
    narrow_phase_test_count_++;
    if (c.containsCurrent(x, y, z, skin_)) {
      count++;
    }
  }
  return count;
}

bool VertexFaceCollisionConstraint::containsCurrent(double x, double y,
                                                    double z) {
  queryCurrentCandidates(x, y, z, candidate_indices_);
  for (const std::size_t collider_index : candidate_indices_) {
    narrow_phase_test_count_++;
    if (cache_.colliders()[collider_index].containsCurrent(x, y, z, skin_)) {
      return true;
    }
  }
  return false;
}

bool VertexFaceCollisionConstraint::containsPrevious(double x, double y,
                                                     double z) {
  queryPreviousCandidates(x, y, z, candidate_indices_);
  for (const std::size_t collider_index : candidate_indices_) {
    narrow_phase_test_count_++;
    if (cache_.colliders()[collider_index].containsPrevious(x, y, z, skin_)) {
      return true;
    }
  }
  return false;
}

void VertexFaceCollisionConstraint::queryCurrentCandidates(
    double x, double y, double z, std::vector<std::size_t> &output) {
  cache_.queryCurrentCandidates(x, y, z, skin_, output);
  recordBroadPhaseQuery(output.size());
}

void VertexFaceCollisionConstraint::queryPreviousCandidates(
    double x, double y, double z, std::vector<std::size_t> &output) {
  cache_.queryPreviousCandidates(x, y, z, skin_, output);
  recordBroadPhaseQuery(output.size());
}

void VertexFaceCollisionConstraint::querySweepCandidates(
    const models::Vector3 &previous, const models::Vector3 &current,
    std::vector<std::size_t> &output) {
  cache_.querySweepCandidates(previous.x, previous.y, previous.z, current.x,
                              current.y, current.z, skin_, output);
  recordBroadPhaseQuery(output.size());
}

void VertexFaceCollisionConstraint::recordBroadPhaseQuery(
    std::size_t candidate_count) {
  broad_phase_query_count_++;
  broad_phase_possible_count_ +=
      static_cast<long long>(cache_.colliders().size());
  broad_phase_candidate_count_ += static_cast<long long>(candidate_count);
}

bool VertexFaceCollisionConstraint::projectSweep(models::Particle &particle,
                                                 int particle_index,
                                                 double dt) {
  models::Vector3 &position = particle.position();
  models::Vector3 &previous = particle.prevPosition();
  const baker::BodyColliderCache::Collider *earliest_collider = nullptr;
  int earliest_face = -1;
  double earliest_time = std::numeric_limits<double>::infinity();

  querySweepCandidates(previous, position, candidate_indices_);
  for (const std::size_t collider_index : candidate_indices_) {
    const auto &collider = cache_.colliders()[collider_index];
    narrow_phase_test_count_++;
    collider.toPreviousBind(previous.x, previous.y, previous.z, local_start_);
    collider.toCurrentBind(position.x, position.y, position.z, local_end_);
    double enter = 0.0;
    double exit = 1.0;
    int enter_face = -1;
    bool valid = true;
    for (int face = 0; face < 6; ++face) {
      const double nx = collider.getBindNormal(face, 0);
      const double ny = collider.getBindNormal(face, 1);
      const double nz = collider.getBindNormal(face, 2);
      const double constant = collider.getBindConstant(face) + skin_;
      const double start_value = nx * local_start_[0] + ny * local_start_[1] +
                                 nz * local_start_[2] - constant;
      const double end_value = nx * local_end_[0] + ny * local_end_[1] +
                               nz * local_end_[2] - constant;
      if (start_value <= 0.0 && end_value <= 0.0) {
        continue;
      }
      const double delta = end_value - start_value;
      if (std::abs(delta) <= epsilon_) {
        if (start_value > 0.0) {
          valid = false;
        }
        if (!valid) {
          break;
        }
        continue;
      }
      const double time = -start_value / delta;
      if (delta < 0.0) {
        if (time > enter) {
          enter = time;
          enter_face = face;
        }
      } else {
        exit = std::min(exit, time);
      }
      if (enter - exit > epsilon_) {
        valid = false;
        break;
      }
    }
    if (valid && enter_face >= 0 && enter >= -epsilon_ &&
        enter <= 1.0 + epsilon_ && enter <= exit + epsilon_ &&
        enter < earliest_time) {
      earliest_time = std::max(0.0, enter);
      earliest_face = enter_face;
      earliest_collider = &collider;
    }
  }
  if (earliest_collider == nullptr) {
    return false;
  }
  const double signed_d = earliest_collider->signedDistanceCurrent(
      earliest_face, position.x, position.y, position.z);
  const double correction = skin_ + slop_ - signed_d;
  if (!std::isfinite(correction) || correction <= 0.0) {
    return false;
  }
  const double before_x = position.x;
  const double before_y = position.y;
  const double before_z = position.z;
  position.x +=
      earliest_collider->getCurrentNormal(earliest_face, 0) * correction;
  position.y +=
      earliest_collider->getCurrentNormal(earliest_face, 1) * correction;
  position.z +=
      earliest_collider->getCurrentNormal(earliest_face, 2) * correction;
  recordContact(particle, particle_index, *earliest_collider, earliest_face,
                before_x, before_y, before_z, dt);
  sweep_hit_count_++;
  return true;
}

void VertexFaceCollisionConstraint::recordContact(
    models::Particle &particle, int particle_index,
    const baker::BodyColliderCache::Collider &collider, int face,
    double before_x, double before_y, double before_z, double dt) {
  const auto idx = static_cast<std::size_t>(particle_index);
  // 与地面约束一致的首次接触锁存：后续求解迭代中的再接触是被前一次
  // 投影削减过的位移，用它重算入射速度会把恢复系数系统性冲淡为 0。
  if (touched_[idx]) {
    return;
  }
  const double nx = collider.getCurrentNormal(face, 0);
  const double ny = collider.getCurrentNormal(face, 1);
  const double nz = collider.getCurrentNormal(face, 2);
  collider.toCurrentBind(particle.position().x, particle.position().y,
                         particle.position().z, local_contact_);
  collider.fromPreviousBind(local_contact_[0], local_contact_[1],
                            local_contact_[2], previous_world_contact_);
  collider.fromCurrentBind(local_contact_[0], local_contact_[1],
                           local_contact_[2], current_world_contact_);
  const double face_x =
      (current_world_contact_[0] - previous_world_contact_[0]) / dt;
  const double face_y =
      (current_world_contact_[1] - previous_world_contact_[1]) / dt;
  const double face_z =
      (current_world_contact_[2] - previous_world_contact_[2]) / dt;
  const models::Vector3 &previous = particle.prevPosition();
  const double velocity_x = (before_x - previous.x) / dt;
  const double velocity_y = (before_y - previous.y) / dt;
  const double velocity_z = (before_z - previous.z) / dt;
  const double relative = (velocity_x - face_x) * nx +
                          (velocity_y - face_y) * ny +
                          (velocity_z - face_z) * nz;
  if (!std::isfinite(relative) || !std::isfinite(face_x) ||
      !std::isfinite(face_y) || !std::isfinite(face_z)) {
    invalid_value_count_++;
    return;
  }
  touched_[idx] = true;
  normal_x_[idx] = nx;
  normal_y_[idx] = ny;
  normal_z_[idx] = nz;
  face_velocity_x_[idx] = face_x;
  face_velocity_y_[idx] = face_y;
  face_velocity_z_[idx] = face_z;
  desired_relative_normal_[idx] =
      relative < 0.0 ? -restitution_ * relative : relative;
}

void VertexFaceCollisionConstraint::postSolveVelocity(
    std::span<models::Particle *const> particles) {
  for (int index : particle_indices_) {
    const auto idx = static_cast<std::size_t>(index);
    if (!touched_[idx]) {
      continue;
    }
    models::Particle &particle = *particles[idx];
    models::Vector3 &velocity = particle.velocity();
    const double current_relative =
        (velocity.x - face_velocity_x_[idx]) * normal_x_[idx] +
        (velocity.y - face_velocity_y_[idx]) * normal_y_[idx] +
        (velocity.z - face_velocity_z_[idx]) * normal_z_[idx];
    const double correction = desired_relative_normal_[idx] - current_relative;
    if (!std::isfinite(correction)) {
      invalid_value_count_++;
      continue;
    }
    velocity.x += normal_x_[idx] * correction;
    velocity.y += normal_y_[idx] * correction;
    velocity.z += normal_z_[idx] * correction;
  }
}

void VertexFaceCollisionConstraint::resetLambda() {
  std::fill(touched_.begin(), touched_.end(), false);
  std::fill(fixed_reported_.begin(), fixed_reported_.end(), false);
}

VertexFaceCollisionConstraint::Diagnostics
VertexFaceCollisionConstraint::diagnostics() const {
  return Diagnostics{cache_.degenerateCubeCount(),
                     initial_embedded_count_,
                     sweep_hit_count_,
                     overlap_no_exit_count_,
                     fixed_inside_count_,
                     invalid_value_count_,
                     broad_phase_query_count_,
                     broad_phase_possible_count_,
                     broad_phase_candidate_count_,
                     narrow_phase_test_count_};
}

}
