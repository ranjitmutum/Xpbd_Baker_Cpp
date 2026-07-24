#pragma once

#include "xpbd/baker/body_collider_cache.hpp"
#include "xpbd/constraints/constraint.hpp"

#include <vector>

namespace xpbd::constraints {






class VertexFaceCollisionConstraint final : public Constraint {
public:
  struct Diagnostics {
    int degenerate_cubes = 0;
    long long initial_embedded = 0;
    long long sweep_hits = 0;
    long long overlap_no_exit = 0;
    long long fixed_inside = 0;
    long long invalid_values = 0;
    long long broad_phase_queries = 0;
    long long broad_phase_possible = 0;
    long long broad_phase_candidates = 0;
    long long narrow_phase_tests = 0;
  };

  VertexFaceCollisionConstraint(std::vector<int> particle_indices,
                                int particle_count,
                                baker::BodyColliderCache &cache, double skin,
                                double restitution = 0.0);

  void solve(std::span<models::Particle *const> particles, double dt) override;


  void resetLambda() override;

  void projectInitial(std::span<models::Particle *const> particles);
  void postSolveVelocity(std::span<models::Particle *const> particles);

  [[nodiscard]] double skin() const { return skin_; }
  [[nodiscard]] Diagnostics diagnostics() const;

private:
  bool projectDiscrete(models::Particle &particle, int particle_index,
                       double dt, bool record_velocity);
  bool projectSweep(models::Particle &particle, int particle_index, double dt);
  int countContainingCurrent(double x, double y, double z);
  bool containsCurrent(double x, double y, double z);
  bool containsPrevious(double x, double y, double z);
  void queryCurrentCandidates(double x, double y, double z,
                              std::vector<std::size_t> &output);
  void queryPreviousCandidates(double x, double y, double z,
                               std::vector<std::size_t> &output);
  void querySweepCandidates(const models::Vector3 &previous,
                            const models::Vector3 &current,
                            std::vector<std::size_t> &output);
  void recordBroadPhaseQuery(std::size_t candidate_count);
  void recordContact(models::Particle &particle, int particle_index,
                     const baker::BodyColliderCache::Collider &collider,
                     int face, double before_x, double before_y,
                     double before_z, double dt);

  std::vector<int> particle_indices_;
  baker::BodyColliderCache &cache_;
  double skin_ = 0.0;
  double restitution_ = 0.0;
  double slop_ = 0.0;
  double epsilon_ = 1e-12;

  std::vector<bool> touched_;
  std::vector<bool> fixed_reported_;
  std::vector<double> normal_x_;
  std::vector<double> normal_y_;
  std::vector<double> normal_z_;
  std::vector<double> face_velocity_x_;
  std::vector<double> face_velocity_y_;
  std::vector<double> face_velocity_z_;
  std::vector<double> desired_relative_normal_;
  std::vector<std::size_t> candidate_indices_;
  std::vector<std::size_t> nested_candidate_indices_;

  long long initial_embedded_count_ = 0;
  long long sweep_hit_count_ = 0;
  long long overlap_no_exit_count_ = 0;
  long long fixed_inside_count_ = 0;
  long long invalid_value_count_ = 0;
  long long broad_phase_query_count_ = 0;
  long long broad_phase_possible_count_ = 0;
  long long broad_phase_candidate_count_ = 0;
  long long narrow_phase_test_count_ = 0;

  int chosen_face_ = 0;
  double chosen_x_ = 0;
  double chosen_y_ = 0;
  double chosen_z_ = 0;
  double local_start_[3]{};
  double local_end_[3]{};
  double local_contact_[3]{};
  double previous_world_contact_[3]{};
  double current_world_contact_[3]{};
};

}
