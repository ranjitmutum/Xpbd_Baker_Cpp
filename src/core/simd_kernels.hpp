#pragma once

#include "xpbd/core/simd_dispatch.hpp"

#include <cstddef>

namespace xpbd::core::detail {

using DenseScaledAddKernel = void (*)(double *output, const double *base,
                                      const double *delta, double scale,
                                      std::size_t count) noexcept;


using BoxProjectionOverlapKernel = double (*)(const double *first_xyz_soa,
                                              const double *second_xyz_soa,
                                              const double *axis) noexcept;

void denseScaledAddSse2(double *output, const double *base, const double *delta,
                        double scale, std::size_t count) noexcept;
void denseScaledAddAvx2(double *output, const double *base, const double *delta,
                        double scale, std::size_t count) noexcept;
double boxProjectionOverlapScalar(const double *first_xyz_soa,
                                  const double *second_xyz_soa,
                                  const double *axis) noexcept;
double boxProjectionOverlapSse2(const double *first_xyz_soa,
                                const double *second_xyz_soa,
                                const double *axis) noexcept;
double boxProjectionOverlapAvx2(const double *first_xyz_soa,
                                const double *second_xyz_soa,
                                const double *axis) noexcept;

struct SimdKernelTable {
  SimdMode mode = SimdMode::SSE2;
  DenseScaledAddKernel dense_scaled_add = nullptr;
  BoxProjectionOverlapKernel box_projection_overlap = nullptr;
};


[[nodiscard]] const SimdKernelTable &
selectedSimdKernelTable(SimdMode requested = SimdMode::Auto) noexcept;

}
