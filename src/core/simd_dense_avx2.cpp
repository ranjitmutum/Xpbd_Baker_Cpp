#include "core/simd_kernels.hpp"

#include <algorithm>
#include <immintrin.h>
#include <limits>

namespace xpbd::core::detail {

void denseScaledAddAvx2(double *output, const double *base, const double *delta,
                        double scale, std::size_t count) noexcept {
  const __m256d scale_vector = _mm256_set1_pd(scale);
  std::size_t index = 0;
  for (; index + 4 <= count; index += 4) {
    const __m256d base_vector = _mm256_loadu_pd(base + index);
    const __m256d delta_vector = _mm256_loadu_pd(delta + index);
    const __m256d result =
        _mm256_add_pd(base_vector, _mm256_mul_pd(delta_vector, scale_vector));
    _mm256_storeu_pd(output + index, result);
  }
  for (; index < count; ++index) {
    output[index] = base[index] + delta[index] * scale;
  }
}

double boxProjectionOverlapAvx2(const double *first_xyz_soa,
                                const double *second_xyz_soa,
                                const double *axis) noexcept {
  const __m256d axis_x = _mm256_set1_pd(axis[0]);
  const __m256d axis_y = _mm256_set1_pd(axis[1]);
  const __m256d axis_z = _mm256_set1_pd(axis[2]);
  __m256d first_min = _mm256_set1_pd(std::numeric_limits<double>::infinity());
  __m256d first_max = _mm256_set1_pd(-std::numeric_limits<double>::infinity());
  __m256d second_min = first_min;
  __m256d second_max = first_max;

  for (std::size_t vertex = 0; vertex < 8; vertex += 4) {
    const auto project = [&](const double *vertices) {
      const __m256d x = _mm256_loadu_pd(vertices + vertex);
      const __m256d y = _mm256_loadu_pd(vertices + 8 + vertex);
      const __m256d z = _mm256_loadu_pd(vertices + 16 + vertex);
      return _mm256_add_pd(
          _mm256_add_pd(_mm256_mul_pd(x, axis_x), _mm256_mul_pd(y, axis_y)),
          _mm256_mul_pd(z, axis_z));
    };
    const __m256d first_projection = project(first_xyz_soa);
    const __m256d second_projection = project(second_xyz_soa);
    first_min = _mm256_min_pd(first_min, first_projection);
    first_max = _mm256_max_pd(first_max, first_projection);
    second_min = _mm256_min_pd(second_min, second_projection);
    second_max = _mm256_max_pd(second_max, second_projection);
  }

  alignas(32) double first_min_lanes[4];
  alignas(32) double first_max_lanes[4];
  alignas(32) double second_min_lanes[4];
  alignas(32) double second_max_lanes[4];
  _mm256_store_pd(first_min_lanes, first_min);
  _mm256_store_pd(first_max_lanes, first_max);
  _mm256_store_pd(second_min_lanes, second_min);
  _mm256_store_pd(second_max_lanes, second_max);
  const auto reduce_min = [](const double *lanes) {
    return std::min(std::min(lanes[0], lanes[1]), std::min(lanes[2], lanes[3]));
  };
  const auto reduce_max = [](const double *lanes) {
    return std::max(std::max(lanes[0], lanes[1]), std::max(lanes[2], lanes[3]));
  };
  const double minimum_first = reduce_min(first_min_lanes);
  const double maximum_first = reduce_max(first_max_lanes);
  const double minimum_second = reduce_min(second_min_lanes);
  const double maximum_second = reduce_max(second_max_lanes);
  return std::min(maximum_first - minimum_second,
                  maximum_second - minimum_first);
}

}
