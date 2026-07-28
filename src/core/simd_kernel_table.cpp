#include "simd_kernels.hpp"

#include <algorithm>
#include <limits>

namespace xpbd::core::detail {

double boxProjectionOverlapScalar(const double *first_xyz_soa,
                                  const double *second_xyz_soa,
                                  const double *axis) noexcept {
  double first_min = std::numeric_limits<double>::infinity();
  double first_max = -std::numeric_limits<double>::infinity();
  double second_min = std::numeric_limits<double>::infinity();
  double second_max = -std::numeric_limits<double>::infinity();
  for (std::size_t vertex = 0; vertex < 8; ++vertex) {
    const double first_projection = first_xyz_soa[vertex] * axis[0] +
                                    first_xyz_soa[8 + vertex] * axis[1] +
                                    first_xyz_soa[16 + vertex] * axis[2];
    const double second_projection = second_xyz_soa[vertex] * axis[0] +
                                     second_xyz_soa[8 + vertex] * axis[1] +
                                     second_xyz_soa[16 + vertex] * axis[2];
    first_min = std::min(first_min, first_projection);
    first_max = std::max(first_max, first_projection);
    second_min = std::min(second_min, second_projection);
    second_max = std::max(second_max, second_projection);
  }
  return std::min(first_max - second_min, second_max - first_min);
}

void targetPositionScalar(double *position, double *lambda,
                          const double *target, double alpha, double weight,
                          double denominator) noexcept {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const double delta =
        -(position[axis] - target[axis] + alpha * lambda[axis]) / denominator;
    lambda[axis] += delta;
    position[axis] += weight * delta;
  }
}

void affineTransform8Scalar(double *output_xyz_aos,
                            const double *input_xyz_aos,
                            const double *linear_3x3,
                            const double *translation) noexcept {
  for (std::size_t vertex = 0; vertex < 8; ++vertex) {
    const std::size_t offset = vertex * 3;
    const double x = input_xyz_aos[offset];
    const double y = input_xyz_aos[offset + 1];
    const double z = input_xyz_aos[offset + 2];
    output_xyz_aos[offset] = linear_3x3[0] * x + linear_3x3[1] * y +
                             linear_3x3[2] * z + translation[0];
    output_xyz_aos[offset + 1] =
        linear_3x3[3] * x + linear_3x3[4] * y + linear_3x3[5] * z +
        translation[1];
    output_xyz_aos[offset + 2] =
        linear_3x3[6] * x + linear_3x3[7] * y + linear_3x3[8] * z +
        translation[2];
  }
}

const SimdKernelTable &selectedSimdKernelTable(SimdMode requested) noexcept {
  static constexpr SimdKernelTable sse2{
      SimdMode::SSE2, boxProjectionOverlapSse2, targetPositionSse2,
      affineTransform8Sse2};


  // 刻意使用 SSE2 版 box projection：8 顶点的小规模负载下 AVX2 实测为负提升。
  static constexpr SimdKernelTable avx2{
      SimdMode::AVX2, boxProjectionOverlapSse2, targetPositionAvx2,
      affineTransform8Avx2};
  return selectedSimdMode(requested) == SimdMode::AVX2 ? avx2 : sse2;
}

}
