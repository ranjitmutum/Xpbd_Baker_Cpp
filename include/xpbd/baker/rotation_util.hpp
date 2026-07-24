#pragma once

#include <array>
#include <cmath>
#include <stdexcept>

namespace xpbd::baker {


// 骨骼旋转的四元数、欧拉角与方向向量转换工具。
class RotationUtil {
public:
  using Vec3 = std::array<double, 3>;
  using Quat = std::array<double, 4>;

  static void quaternionMultiply(const double a[4], const double b[4],
                                 double result[4]);
  [[nodiscard]] static Quat quaternionMultiply(const Quat &a, const Quat &b);

  [[nodiscard]] static Quat quaternionInverse(const Quat &q);

  [[nodiscard]] static Vec3
  rotationVectorFromQuaternion(const Quat &quaternion);
  [[nodiscard]] static Quat quaternionFromRotationVector(const Vec3 &vector);

  [[nodiscard]] static Vec3 eulerFromQuaternion(const Quat &q);
  [[nodiscard]] static Vec3 bedrockEulerFromQuaternion(const Quat &q);
  [[nodiscard]] static Vec3 unwrapEuler(const Vec3 &previous,
                                        const Vec3 &current);

  [[nodiscard]] static Quat quaternionFromBedrockEuler(double rx, double ry,
                                                       double rz);
  [[nodiscard]] static Quat quaternionFromEuler(double rx, double ry,
                                                double rz);
  static void quaternionFromEuler(double rx, double ry, double rz,
                                  double result[4]);

  [[nodiscard]] static Vec3 rotateVector(const Quat &q, const Vec3 &v);
  static void rotateVector(const double q[4], double x, double y, double z,
                           double result[3]);








  [[nodiscard]] static Quat
  quaternionFromDirectionPairs(const Vec3 *from, const Vec3 *to, int pairCount);

private:
  RotationUtil() = delete;

  static Quat quaternionFromTwoVectors(const double from[3],
                                       const double to[3]);
  static void crossWithFallback(double fx, double fy, double fz, double out[3]);
};

}
