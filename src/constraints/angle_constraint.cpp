#include "xpbd/constraints/angle_constraint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace xpbd::constraints {
namespace {

using Vec3 = std::array<double, 3>;

double dot(const Vec3 &a, const Vec3 &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3 &a, const Vec3 &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

double length(const Vec3 &value) { return std::sqrt(dot(value, value)); }

Vec3 scaled(const Vec3 &value, double scale) {
  return {value[0] * scale, value[1] * scale, value[2] * scale};
}

Vec3 add(const Vec3 &a, const Vec3 &b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 normalizedFallback(const Vec3 &requested, const Vec3 &direction) {



  Vec3 projected =
      add(requested, scaled(direction, -dot(requested, direction)));
  double projectedLength = length(projected);
  if (std::isfinite(projectedLength) && projectedLength > 1e-10) {
    return scaled(projected, 1.0 / projectedLength);
  }
  Vec3 axis = std::abs(direction[0]) <= std::abs(direction[1]) &&
                      std::abs(direction[0]) <= std::abs(direction[2])
                  ? Vec3{1.0, 0.0, 0.0}
                  : (std::abs(direction[1]) <= std::abs(direction[2])
                         ? Vec3{0.0, 1.0, 0.0}
                         : Vec3{0.0, 0.0, 1.0});
  projected = cross(direction, axis);
  projectedLength = length(projected);
  return projectedLength > 1e-10 ? scaled(projected, 1.0 / projectedLength)
                                 : Vec3{0.0, 0.0, 1.0};
}

}

AngleConstraint::AngleConstraint(int idxA, int idxB, int idxC,
                                 double minAngleRadians, double maxAngleRadians,
                                 double compliance, double fallbackNormalX,
                                 double fallbackNormalY, double fallbackNormalZ)
    : idx_a_(idxA), idx_b_(idxB), idx_c_(idxC),
      compliance_(finiteNonNegative(compliance)) {
  const auto angle = [](double value, double fallback) {
    return std::isfinite(value) ? std::clamp(value, 0.0, std::numbers::pi)
                                : fallback;
  };
  const double safeMin = angle(minAngleRadians, 0.0);
  const double safeMax = angle(maxAngleRadians, std::numbers::pi);
  min_angle_ = std::min(safeMin, safeMax);
  max_angle_ = std::max(safeMin, safeMax);

  const Vec3 requested{fallbackNormalX, fallbackNormalY, fallbackNormalZ};
  const double requestedLength = length(requested);
  if (std::isfinite(requestedLength) && requestedLength > 1e-10) {
    fallback_normal_x_ = requested[0] / requestedLength;
    fallback_normal_y_ = requested[1] / requestedLength;
    fallback_normal_z_ = requested[2] / requestedLength;
  }
}

void AngleConstraint::solve(std::span<models::Particle *const> particles,
                            double dt) {
  if (!std::isfinite(dt) || !(dt > 0.0)) {
    return;
  }
  models::Particle &a = *particles[static_cast<std::size_t>(idx_a_)];
  models::Particle &b = *particles[static_cast<std::size_t>(idx_b_)];
  models::Particle &c = *particles[static_cast<std::size_t>(idx_c_)];
  const Vec3 u{a.position().x - b.position().x, a.position().y - b.position().y,
               a.position().z - b.position().z};
  const Vec3 v{c.position().x - b.position().x, c.position().y - b.position().y,
               c.position().z - b.position().z};
  const double uLength = length(u);
  const double vLength = length(v);
  if (!std::isfinite(uLength) || !std::isfinite(vLength) || uLength < 1e-10 ||
      vLength < 1e-10) {
    return;
  }
  const Vec3 uDirection = scaled(u, 1.0 / uLength);
  const Vec3 vDirection = scaled(v, 1.0 / vLength);
  const Vec3 crossValue = cross(uDirection, vDirection);
  const double crossLength = length(crossValue);
  const double cosine = std::clamp(dot(uDirection, vDirection), -1.0, 1.0);
  const double currentAngle = std::atan2(crossLength, cosine);

  int side = 0;
  double boundary = 0.0;
  if (currentAngle < min_angle_) {
    side = -1;
    boundary = min_angle_;
  } else if (currentAngle > max_angle_) {
    side = 1;
    boundary = max_angle_;
  } else {
    lambda_ = 0.0;
    active_side_ = 0;
    return;
  }
  if (side != active_side_) {
    lambda_ = 0.0;
  }
  active_side_ = side;

  const Vec3 normal =
      crossLength > 1e-10
          ? scaled(crossValue, 1.0 / crossLength)
          : normalizedFallback(
                {fallback_normal_x_, fallback_normal_y_, fallback_normal_z_},
                uDirection);
  const Vec3 gradientA = scaled(cross(uDirection, normal), 1.0 / uLength);
  const Vec3 gradientC = scaled(cross(normal, vDirection), 1.0 / vLength);
  const Vec3 gradientB = scaled(add(gradientA, gradientC), -1.0);
  const double weightA = a.invMass();
  const double weightB = b.invMass();
  const double weightC = c.invMass();
  const double weightedGradient = weightA * dot(gradientA, gradientA) +
                                  weightB * dot(gradientB, gradientB) +
                                  weightC * dot(gradientC, gradientC);
  const double alpha = compliance_ / (dt * dt);
  const double denominator = weightedGradient + alpha;
  if (!std::isfinite(denominator) || denominator <= 0.0) {
    return;
  }
  const double constraintValue = currentAngle - boundary;
  const double deltaLambda = -(constraintValue + alpha * lambda_) / denominator;
  lambda_ += deltaLambda;

  a.position().x += weightA * gradientA[0] * deltaLambda;
  a.position().y += weightA * gradientA[1] * deltaLambda;
  a.position().z += weightA * gradientA[2] * deltaLambda;
  b.position().x += weightB * gradientB[0] * deltaLambda;
  b.position().y += weightB * gradientB[1] * deltaLambda;
  b.position().z += weightB * gradientB[2] * deltaLambda;
  c.position().x += weightC * gradientC[0] * deltaLambda;
  c.position().y += weightC * gradientC[1] * deltaLambda;
  c.position().z += weightC * gradientC[2] * deltaLambda;
}

void AngleConstraint::resetLambda() {
  lambda_ = 0.0;
  active_side_ = 0;
}

double AngleConstraint::finiteNonNegative(double value) {
  return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

}
