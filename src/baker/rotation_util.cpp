#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace xpbd::baker {

namespace {

using Direction = RotationUtil::Vec3;
using Quaternion = RotationUtil::Quat;
using Matrix4 = std::array<std::array<double, 4>, 4>;

constexpr double kDirectionLengthEpsilon = 1e-10;
constexpr double kCollinearEpsilon = 1e-8;
constexpr double kEigenEpsilon = 1e-15;

struct DirectionPair {
  Direction from{};
  Direction to{};
};

bool normalizeFiniteDirection(const Direction &input, Direction &output) {
  const double length_squared =
      input[0] * input[0] + input[1] * input[1] + input[2] * input[2];
  if (!std::isfinite(length_squared) ||
      !(length_squared > kDirectionLengthEpsilon * kDirectionLengthEpsilon)) {
    return false;
  }
  const double inverse_length = 1.0 / std::sqrt(length_squared);
  for (int axis = 0; axis < 3; ++axis) {
    output[static_cast<std::size_t>(axis)] =
        input[static_cast<std::size_t>(axis)] * inverse_length;
  }
  return true;
}

double directionDot(const Direction &first, const Direction &second) {
  return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
}

Direction directionCross(const Direction &first, const Direction &second) {
  return Direction{first[1] * second[2] - first[2] * second[1],
                   first[2] * second[0] - first[0] * second[2],
                   first[0] * second[1] - first[1] * second[0]};
}

bool pairLess(const DirectionPair &first, const DirectionPair &second) {
  for (int axis = 0; axis < 3; ++axis) {
    const auto index = static_cast<std::size_t>(axis);
    if (first.from[index] != second.from[index]) {
      return first.from[index] < second.from[index];
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    const auto index = static_cast<std::size_t>(axis);
    if (first.to[index] != second.to[index]) {
      return first.to[index] < second.to[index];
    }
  }
  return false;
}

bool hasIndependentDirections(const std::vector<DirectionPair> &pairs,
                              bool use_from) {
  const Direction &first = use_from ? pairs.front().from : pairs.front().to;
  for (std::size_t index = 1; index < pairs.size(); ++index) {
    const Direction &candidate = use_from ? pairs[index].from : pairs[index].to;
    const Direction cross = directionCross(first, candidate);
    if (directionDot(cross, cross) > kCollinearEpsilon * kCollinearEpsilon) {
      return true;
    }
  }
  return false;
}

Direction canonicalAxis(Direction axis) {
  for (double component : axis) {
    if (std::abs(component) <= kDirectionLengthEpsilon) {
      continue;
    }
    if (component < 0.0) {
      for (double &value : axis) {
        value = -value;
      }
    }
    break;
  }
  return axis;
}

Quaternion canonicalizeQuaternion(Quaternion quaternion) {
  double length_squared = 0.0;
  for (double component : quaternion) {
    length_squared += component * component;
  }
  if (!std::isfinite(length_squared) || !(length_squared > 1e-24)) {
    return Quaternion{0.0, 0.0, 0.0, 1.0};
  }
  const double inverse_length = 1.0 / std::sqrt(length_squared);
  for (double &component : quaternion) {
    component *= inverse_length;
  }



  constexpr std::array<int, 4> kSignOrder{3, 0, 1, 2};
  for (int raw_index : kSignOrder) {
    const auto index = static_cast<std::size_t>(raw_index);
    if (std::abs(quaternion[index]) <= 1e-12) {
      continue;
    }
    if (quaternion[index] < 0.0) {
      for (double &component : quaternion) {
        component = -component;
      }
    }
    break;
  }
  for (double &component : quaternion) {
    if (component == 0.0) {
      component = 0.0;
    }
  }
  return quaternion;
}

Quaternion largestEigenQuaternion(Matrix4 matrix) {
  Matrix4 eigenvectors{};
  for (int index = 0; index < 4; ++index) {
    eigenvectors[static_cast<std::size_t>(index)]
                [static_cast<std::size_t>(index)] = 1.0;
  }



  for (int sweep = 0; sweep < 32; ++sweep) {
    bool changed = false;
    for (int p = 0; p < 3; ++p) {
      for (int q = p + 1; q < 4; ++q) {
        const auto pi = static_cast<std::size_t>(p);
        const auto qi = static_cast<std::size_t>(q);
        const double off_diagonal = matrix[pi][qi];
        const double scale =
            1.0 + std::abs(matrix[pi][pi]) + std::abs(matrix[qi][qi]);
        if (std::abs(off_diagonal) <= kEigenEpsilon * scale) {
          continue;
        }
        changed = true;
        const double tau =
            (matrix[qi][qi] - matrix[pi][pi]) / (2.0 * off_diagonal);
        const double tangent = std::copysign(1.0, tau) /
                               (std::abs(tau) + std::sqrt(1.0 + tau * tau));
        const double cosine = 1.0 / std::sqrt(1.0 + tangent * tangent);
        const double sine = tangent * cosine;

        const double pp = matrix[pi][pi];
        const double qq = matrix[qi][qi];
        matrix[pi][pi] = pp - tangent * off_diagonal;
        matrix[qi][qi] = qq + tangent * off_diagonal;
        matrix[pi][qi] = 0.0;
        matrix[qi][pi] = 0.0;
        for (int k = 0; k < 4; ++k) {
          if (k == p || k == q) {
            continue;
          }
          const auto ki = static_cast<std::size_t>(k);
          const double kp = matrix[ki][pi];
          const double kq = matrix[ki][qi];
          matrix[ki][pi] = cosine * kp - sine * kq;
          matrix[pi][ki] = matrix[ki][pi];
          matrix[ki][qi] = sine * kp + cosine * kq;
          matrix[qi][ki] = matrix[ki][qi];
        }
        for (int k = 0; k < 4; ++k) {
          const auto ki = static_cast<std::size_t>(k);
          const double kp = eigenvectors[ki][pi];
          const double kq = eigenvectors[ki][qi];
          eigenvectors[ki][pi] = cosine * kp - sine * kq;
          eigenvectors[ki][qi] = sine * kp + cosine * kq;
        }
      }
    }
    if (!changed) {
      break;
    }
  }

  int largest = 0;
  for (int index = 1; index < 4; ++index) {
    if (matrix[static_cast<std::size_t>(index)]
              [static_cast<std::size_t>(index)] >
        matrix[static_cast<std::size_t>(largest)]
              [static_cast<std::size_t>(largest)]) {
      largest = index;
    }
  }
  Quaternion result{};
  for (int component = 0; component < 4; ++component) {
    result[static_cast<std::size_t>(component)] =
        eigenvectors[static_cast<std::size_t>(component)]
                    [static_cast<std::size_t>(largest)];
  }
  return canonicalizeQuaternion(result);
}

}

void RotationUtil::quaternionMultiply(const double a[4], const double b[4],
                                      double result[4]) {
  const double ax = a[0], ay = a[1], az = a[2], aw = a[3];
  const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
  result[0] = aw * bx + ax * bw + ay * bz - az * by;
  result[1] = aw * by - ax * bz + ay * bw + az * bx;
  result[2] = aw * bz + ax * by - ay * bx + az * bw;
  result[3] = aw * bw - ax * bx - ay * by - az * bz;
}

RotationUtil::Quat RotationUtil::quaternionMultiply(const Quat &a,
                                                    const Quat &b) {
  Quat result{};
  quaternionMultiply(a.data(), b.data(), result.data());
  return result;
}

RotationUtil::Quat RotationUtil::quaternionInverse(const Quat &q) {
  return Quat{-q[0], -q[1], -q[2], q[3]};
}

RotationUtil::Vec3
RotationUtil::rotationVectorFromQuaternion(const Quat &quaternion) {
  const double length =
      std::sqrt(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
                quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
  if (!(length > 1e-20)) {
    return Vec3{0.0, 0.0, 0.0};
  }
  const double sign = quaternion[3] < 0.0 ? -1.0 : 1.0;
  const double x = quaternion[0] * sign / length;
  const double y = quaternion[1] * sign / length;
  const double z = quaternion[2] * sign / length;
  const double w = std::max(-1.0, std::min(1.0, quaternion[3] * sign / length));
  const double sine = std::sqrt(x * x + y * y + z * z);
  if (sine < 1e-12) {
    return Vec3{0.0, 0.0, 0.0};
  }
  const double angle = 2.0 * std::atan2(sine, w);
  const double scale = angle / sine;
  return Vec3{x * scale, y * scale, z * scale};
}

RotationUtil::Quat
RotationUtil::quaternionFromRotationVector(const Vec3 &vector) {
  const double angle = std::sqrt(vector[0] * vector[0] + vector[1] * vector[1] +
                                 vector[2] * vector[2]);
  if (angle < 1e-12) {
    return Quat{0.0, 0.0, 0.0, 1.0};
  }
  const double half = angle * 0.5;
  const double scale = std::sin(half) / angle;
  return Quat{vector[0] * scale, vector[1] * scale, vector[2] * scale,
              std::cos(half)};
}

RotationUtil::Vec3 RotationUtil::eulerFromQuaternion(const Quat &q) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];

  const double sinr_cosp = 2.0 * (w * x + y * z);
  const double cosr_cosp = 1.0 - 2.0 * (x * x + y * y);
  const double rx = std::atan2(sinr_cosp, cosr_cosp);

  const double sinp = 2.0 * (w * y - z * x);
  double ry = 0.0;
  if (std::abs(sinp) >= 1.0) {
    ry = std::copysign(std::numbers::pi_v<double> / 2.0, sinp);
  } else {
    ry = std::asin(sinp);
  }

  const double siny_cosp = 2.0 * (w * z + x * y);
  const double cosy_cosp = 1.0 - 2.0 * (y * y + z * z);
  const double rz = std::atan2(siny_cosp, cosy_cosp);

  constexpr double kRadToDeg = 180.0 / std::numbers::pi_v<double>;
  return Vec3{rx * kRadToDeg, ry * kRadToDeg, rz * kRadToDeg};
}

RotationUtil::Vec3 RotationUtil::bedrockEulerFromQuaternion(const Quat &q) {
  const Vec3 internal = eulerFromQuaternion(q);
  return Vec3{-internal[0], -internal[1], internal[2]};
}

RotationUtil::Vec3 RotationUtil::unwrapEuler(const Vec3 &previous,
                                             const Vec3 &current) {
  Vec3 result{};
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(previous[static_cast<std::size_t>(axis)]) ||
        !std::isfinite(current[static_cast<std::size_t>(axis)])) {
      throw std::invalid_argument("Euler values must be finite");
    }
    result[static_cast<std::size_t>(axis)] =
        current[static_cast<std::size_t>(axis)] +
        360.0 * std::rint((previous[static_cast<std::size_t>(axis)] -
                           current[static_cast<std::size_t>(axis)]) /
                          360.0);
  }
  return result;
}

RotationUtil::Quat
RotationUtil::quaternionFromBedrockEuler(double rx, double ry, double rz) {
  return quaternionFromEuler(-rx, -ry, rz);
}

RotationUtil::Quat RotationUtil::quaternionFromEuler(double rx, double ry,
                                                     double rz) {
  Quat result{};
  quaternionFromEuler(rx, ry, rz, result.data());
  return result;
}

void RotationUtil::quaternionFromEuler(double rx, double ry, double rz,
                                       double result[4]) {
  constexpr double kDegToRad = std::numbers::pi_v<double> / 180.0;
  rx *= kDegToRad;
  ry *= kDegToRad;
  rz *= kDegToRad;

  const double cx = std::cos(rx * 0.5), sx = std::sin(rx * 0.5);
  const double cy = std::cos(ry * 0.5), sy = std::sin(ry * 0.5);
  const double cz = std::cos(rz * 0.5), sz = std::sin(rz * 0.5);

  result[0] = sx * cy * cz - cx * sy * sz;
  result[1] = cx * sy * cz + sx * cy * sz;
  result[2] = cx * cy * sz - sx * sy * cz;
  result[3] = cx * cy * cz + sx * sy * sz;
}

RotationUtil::Vec3 RotationUtil::rotateVector(const Quat &q, const Vec3 &v) {
  Vec3 result{};
  rotateVector(q.data(), v[0], v[1], v[2], result.data());
  return result;
}

void RotationUtil::rotateVector(const double q[4], double x, double y, double z,
                                double result[3]) {
  const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
  const double vx = x, vy = y, vz = z;

  const double tx = 2.0 * (qy * vz - qz * vy);
  const double ty = 2.0 * (qz * vx - qx * vz);
  const double tz = 2.0 * (qx * vy - qy * vx);

  result[0] = vx + qw * tx + (qy * tz - qz * ty);
  result[1] = vy + qw * ty + (qz * tx - qx * tz);
  result[2] = vz + qw * tz + (qx * ty - qy * tx);
}

RotationUtil::Quat RotationUtil::quaternionFromDirectionPairs(const Vec3 *from,
                                                              const Vec3 *to,
                                                              int pairCount) {
  if (from == nullptr || to == nullptr || pairCount <= 0) {
    return Quat{0.0, 0.0, 0.0, 1.0};
  }
  std::vector<DirectionPair> pairs;
  pairs.reserve(static_cast<std::size_t>(pairCount));
  for (int index = 0; index < pairCount; ++index) {
    DirectionPair pair;
    if (normalizeFiniteDirection(from[static_cast<std::size_t>(index)],
                                 pair.from) &&
        normalizeFiniteDirection(to[static_cast<std::size_t>(index)],
                                 pair.to)) {
      pairs.push_back(pair);
    }
  }
  if (pairs.empty()) {
    return Quat{0.0, 0.0, 0.0, 1.0};
  }
  std::sort(pairs.begin(), pairs.end(), pairLess);

  const bool independent_from = hasIndependentDirections(pairs, true);
  const bool independent_to = hasIndependentDirections(pairs, false);
  if (!independent_from || !independent_to) {
    Direction aggregate_from{};
    Direction aggregate_to{};
    if (!independent_from) {
      const Direction from_axis = canonicalAxis(pairs.front().from);
      for (const auto &pair : pairs) {
        const double coefficient = directionDot(pair.from, from_axis);
        for (int axis = 0; axis < 3; ++axis) {
          aggregate_to[static_cast<std::size_t>(axis)] +=
              coefficient * pair.to[static_cast<std::size_t>(axis)];
        }
      }
      Direction to_axis{};
      if (!normalizeFiniteDirection(aggregate_to, to_axis)) {
        return Quat{0.0, 0.0, 0.0, 1.0};
      }
      return canonicalizeQuaternion(
          quaternionFromTwoVectors(from_axis.data(), to_axis.data()));
    }

    const Direction to_axis = canonicalAxis(pairs.front().to);
    for (const auto &pair : pairs) {
      const double coefficient = directionDot(pair.to, to_axis);
      for (int axis = 0; axis < 3; ++axis) {
        aggregate_from[static_cast<std::size_t>(axis)] +=
            coefficient * pair.from[static_cast<std::size_t>(axis)];
      }
    }
    Direction from_axis{};
    if (!normalizeFiniteDirection(aggregate_from, from_axis)) {
      return Quat{0.0, 0.0, 0.0, 1.0};
    }
    return canonicalizeQuaternion(
        quaternionFromTwoVectors(from_axis.data(), to_axis.data()));
  }

  Matrix4 davenport{};
  for (const auto &pair : pairs) {
    const double alignment = directionDot(pair.from, pair.to);
    for (int row = 0; row < 3; ++row) {
      const auto ri = static_cast<std::size_t>(row);
      for (int column = 0; column < 3; ++column) {
        const auto ci = static_cast<std::size_t>(column);
        davenport[ri][ci] +=
            pair.from[ri] * pair.to[ci] + pair.to[ri] * pair.from[ci];
      }
      davenport[ri][ri] -= alignment;
    }
    const Direction cross = directionCross(pair.from, pair.to);
    for (int axis = 0; axis < 3; ++axis) {
      const auto index = static_cast<std::size_t>(axis);
      davenport[index][3] += cross[index];
      davenport[3][index] += cross[index];
    }
    davenport[3][3] += alignment;
  }
  return largestEigenQuaternion(davenport);
}

RotationUtil::Quat RotationUtil::quaternionFromTwoVectors(const double from[3],
                                                          const double to[3]) {
  const double fx = from[0], fy = from[1], fz = from[2];
  const double tx = to[0], ty = to[1], tz = to[2];
  const double dot = fx * tx + fy * ty + fz * tz;

  if (dot > 0.999999) {
    return Quat{0.0, 0.0, 0.0, 1.0};
  }
  if (dot < -0.999999) {
    double axis[3];
    crossWithFallback(fx, fy, fz, axis);
    const double len =
        std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    axis[0] /= len;
    axis[1] /= len;
    axis[2] /= len;
    return Quat{axis[0], axis[1], axis[2], 0.0};
  }

  const double cx = fy * tz - fz * ty;
  const double cy = fz * tx - fx * tz;
  const double cz = fx * ty - fy * tx;
  const double w = 1.0 + dot;
  const double len = std::sqrt(cx * cx + cy * cy + cz * cz + w * w);
  return Quat{cx / len, cy / len, cz / len, w / len};
}

void RotationUtil::crossWithFallback(double fx, double fy, double fz,
                                     double out[3]) {
  if (std::abs(fx) < 0.9) {
    out[0] = 0.0;
    out[1] = -fz;
    out[2] = fy;
  } else {
    out[0] = fz;
    out[1] = 0.0;
    out[2] = -fx;
  }
}

}
