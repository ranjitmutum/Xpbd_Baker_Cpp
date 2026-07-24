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

const SimdKernelTable &selectedSimdKernelTable(SimdMode requested) noexcept {
  static constexpr SimdKernelTable sse2{SimdMode::SSE2, denseScaledAddSse2,
                                        boxProjectionOverlapSse2};


  static constexpr SimdKernelTable avx2{SimdMode::AVX2, denseScaledAddAvx2,
                                        boxProjectionOverlapSse2};
  return selectedSimdMode(requested) == SimdMode::AVX2 ? avx2 : sse2;
}

}
