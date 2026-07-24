#include "core/simd_kernels.hpp"

#include <algorithm>
#include <emmintrin.h>
#include <limits>

namespace xpbd::core::detail {

void denseScaledAddSse2(double *output, const double *base, const double *delta,
                        double scale, std::size_t count) noexcept {
  const __m128d scale_vector = _mm_set1_pd(scale);
  std::size_t index = 0;
  for (; index + 2 <= count; index += 2) {
    const __m128d base_vector = _mm_loadu_pd(base + index);
    const __m128d delta_vector = _mm_loadu_pd(delta + index);
    const __m128d result =
        _mm_add_pd(base_vector, _mm_mul_pd(delta_vector, scale_vector));
    _mm_storeu_pd(output + index, result);
  }
  for (; index < count; ++index) {
    output[index] = base[index] + delta[index] * scale;
  }
}

double boxProjectionOverlapSse2(const double *first_xyz_soa,
                                const double *second_xyz_soa,
                                const double *axis) noexcept {
  const __m128d axis_x = _mm_set1_pd(axis[0]);
  const __m128d axis_y = _mm_set1_pd(axis[1]);
  const __m128d axis_z = _mm_set1_pd(axis[2]);
  __m128d first_min = _mm_set1_pd(std::numeric_limits<double>::infinity());
  __m128d first_max = _mm_set1_pd(-std::numeric_limits<double>::infinity());
  __m128d second_min = first_min;
  __m128d second_max = first_max;

  for (std::size_t vertex = 0; vertex < 8; vertex += 2) {
    const auto project = [&](const double *vertices) {
      const __m128d x = _mm_loadu_pd(vertices + vertex);
      const __m128d y = _mm_loadu_pd(vertices + 8 + vertex);
      const __m128d z = _mm_loadu_pd(vertices + 16 + vertex);
      return _mm_add_pd(
          _mm_add_pd(_mm_mul_pd(x, axis_x), _mm_mul_pd(y, axis_y)),
          _mm_mul_pd(z, axis_z));
    };
    const __m128d first_projection = project(first_xyz_soa);
    const __m128d second_projection = project(second_xyz_soa);
    first_min = _mm_min_pd(first_min, first_projection);
    first_max = _mm_max_pd(first_max, first_projection);
    second_min = _mm_min_pd(second_min, second_projection);
    second_max = _mm_max_pd(second_max, second_projection);
  }

  alignas(16) double first_min_lanes[2];
  alignas(16) double first_max_lanes[2];
  alignas(16) double second_min_lanes[2];
  alignas(16) double second_max_lanes[2];
  _mm_store_pd(first_min_lanes, first_min);
  _mm_store_pd(first_max_lanes, first_max);
  _mm_store_pd(second_min_lanes, second_min);
  _mm_store_pd(second_max_lanes, second_max);
  const double minimum_first = std::min(first_min_lanes[0], first_min_lanes[1]);
  const double maximum_first = std::max(first_max_lanes[0], first_max_lanes[1]);
  const double minimum_second =
      std::min(second_min_lanes[0], second_min_lanes[1]);
  const double maximum_second =
      std::max(second_max_lanes[0], second_max_lanes[1]);
  return std::min(maximum_first - minimum_second,
                  maximum_second - minimum_first);
}

}
