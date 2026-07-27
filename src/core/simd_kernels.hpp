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

using TargetPositionKernel = void (*)(double *position, double *lambda,
                                      const double *target, double alpha,
                                      double weight,
                                      double denominator) noexcept;

using AffineTransform8Kernel = void (*)(double *output_xyz_aos,
                                       const double *input_xyz_aos,
                                       const double *linear_3x3,
                                       const double *translation) noexcept;

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
void targetPositionScalar(double *position, double *lambda,
                          const double *target, double alpha, double weight,
                          double denominator) noexcept;
void targetPositionSse2(double *position, double *lambda, const double *target,
                        double alpha, double weight,
                        double denominator) noexcept;
void targetPositionAvx2(double *position, double *lambda, const double *target,
                        double alpha, double weight,
                        double denominator) noexcept;
void affineTransform8Scalar(double *output_xyz_aos,
                            const double *input_xyz_aos,
                            const double *linear_3x3,
                            const double *translation) noexcept;
void affineTransform8Sse2(double *output_xyz_aos,
                          const double *input_xyz_aos,
                          const double *linear_3x3,
                          const double *translation) noexcept;
void affineTransform8Avx2(double *output_xyz_aos,
                          const double *input_xyz_aos,
                          const double *linear_3x3,
                          const double *translation) noexcept;

struct SimdKernelTable {
  SimdMode mode = SimdMode::SSE2;
  DenseScaledAddKernel dense_scaled_add = nullptr;
  BoxProjectionOverlapKernel box_projection_overlap = nullptr;
  TargetPositionKernel target_position = nullptr;
  AffineTransform8Kernel affine_transform_8 = nullptr;
};


[[nodiscard]] const SimdKernelTable &
selectedSimdKernelTable(SimdMode requested = SimdMode::Auto) noexcept;

}
