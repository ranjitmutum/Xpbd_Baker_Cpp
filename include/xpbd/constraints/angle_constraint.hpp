#pragma once

#include "xpbd/constraints/constraint.hpp"

namespace xpbd::constraints {








class AngleConstraint final : public Constraint {
public:
  AngleConstraint(int idxA, int idxB, int idxC, double minAngleRadians,
                  double maxAngleRadians, double compliance,
                  double fallbackNormalX, double fallbackNormalY,
                  double fallbackNormalZ);

  void solve(std::span<models::Particle *const> particles, double dt) override;
  void resetLambda() override;

private:
  int idx_a_;
  int idx_b_;
  int idx_c_;
  double min_angle_ = 0.0;
  double max_angle_ = 3.14159265358979323846;
  double compliance_ = 0.0;
  double lambda_ = 0.0;
  int active_side_ = 0;
  double fallback_normal_x_ = 0.0;
  double fallback_normal_y_ = 0.0;
  double fallback_normal_z_ = 1.0;

  static double finiteNonNegative(double value);
};

}
