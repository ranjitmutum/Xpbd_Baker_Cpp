#include "xpbd/baker/cube_geometry.hpp"

#include "xpbd/baker/bedrock_transform_resolver.hpp"
#if defined(XPBD_HAS_X86_SIMD)
#include "../core/simd_kernels.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xpbd::baker {

namespace {

double effectiveAxisOrigin(const loader::Cube& cube, int axis) {
    if (cube.size[axis] == 0.0 && cube.inflate < 0.0) {
        return cube.origin[axis];
    }
    return std::min(cube.origin[axis],
                    cube.origin[axis] + cube.size[axis]) - cube.inflate;
}

double effectiveAxisSize(const loader::Cube& cube, int axis) {
    return std::max(0.0,
                    std::abs(cube.size[axis]) + cube.inflate * 2.0);
}

} // namespace

std::array<double, 3> CubeGeometry::effectivePivot(const loader::Cube& cube) {
    requireCube(cube);
    if (cube.has_pivot) {
        return {cube.pivot[0], cube.pivot[1], cube.pivot[2]};
    }
    return {cube.origin[0] + cube.size[0] * 0.5, cube.origin[1] + cube.size[1] * 0.5,
            cube.origin[2] + cube.size[2] * 0.5};
}

std::array<double, 3> CubeGeometry::effectiveOrigin(const loader::Cube& cube) {
    requireCube(cube);
    return {effectiveAxisOrigin(cube, 0), effectiveAxisOrigin(cube, 1),
            effectiveAxisOrigin(cube, 2)};
}

std::array<double, 3> CubeGeometry::effectiveSize(const loader::Cube& cube) {
    requireCube(cube);
    return {effectiveAxisSize(cube, 0), effectiveAxisSize(cube, 1),
            effectiveAxisSize(cube, 2)};
}

std::array<double, 24> CubeGeometry::bindVertices(const loader::Cube& cube) {
    requireCube(cube);
    std::array<double, 24> result{};
    const auto size = effectiveSize(cube);
    const auto matrix = BedrockTransformResolver::resolveCubeLocalMatrix(cube);
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                const int index = x + y * 2 + z * 4;
                const auto transformed = matrix.transformPoint(
                    (x - 0.5) * size[0], (y - 0.5) * size[1], (z - 0.5) * size[2]);
                result[static_cast<std::size_t>(index * 3)] = transformed[0];
                result[static_cast<std::size_t>(index * 3 + 1)] = transformed[1];
                result[static_cast<std::size_t>(index * 3 + 2)] = transformed[2];
            }
        }
    }
    return result;
}

std::array<double, 24> CubeGeometry::transformPoints8(
    const BonePoseCalculator::Pose& pose,
    const std::array<double, 24>& points) {
    return transformPoints8(pose, points, core::SimdMode::Auto);
}

std::array<double, 24> CubeGeometry::transformPoints8(
    const BonePoseCalculator::Pose& pose,
    const std::array<double, 24>& points,
    core::SimdMode mode) {
    std::array<double, 24> result{};
#if defined(XPBD_HAS_X86_SIMD)
    static const auto sse2_kernel =
        core::detail::selectedSimdKernelTable(core::SimdMode::SSE2)
            .affine_transform_8;
    static const auto avx2_kernel =
        core::detail::selectedSimdKernelTable(core::SimdMode::AVX2)
            .affine_transform_8;
    static const auto auto_kernel =
        core::detail::selectedSimdKernelTable(core::SimdMode::Auto)
            .affine_transform_8;
    const auto kernel = mode == core::SimdMode::SSE2
                            ? sse2_kernel
                            : (mode == core::SimdMode::AVX2 ? avx2_kernel
                                                           : auto_kernel);
    kernel(result.data(), points.data(), pose.world_linear.data(),
           pose.world_translation.data());
#else
    for (std::size_t vertex = 0; vertex < 8; ++vertex) {
        const std::size_t offset = vertex * 3;
        const double x = points[offset];
        const double y = points[offset + 1];
        const double z = points[offset + 2];
        result[offset] = pose.world_linear[0] * x + pose.world_linear[1] * y +
                         pose.world_linear[2] * z + pose.world_translation[0];
        result[offset + 1] =
            pose.world_linear[3] * x + pose.world_linear[4] * y +
            pose.world_linear[5] * z + pose.world_translation[1];
        result[offset + 2] =
            pose.world_linear[6] * x + pose.world_linear[7] * y +
            pose.world_linear[8] * z + pose.world_translation[2];
    }
#endif
    if (!std::all_of(result.begin(), result.end(),
                     [](double value) { return std::isfinite(value); })) {
        throw std::invalid_argument("transformed model point must be finite");
    }
    return result;
}

core::SimdMode
CubeGeometry::recommendedTransformSimdMode(std::size_t cube_count) noexcept {
    if (!core::detectSimdCapabilities().avx2Usable()) {
        return core::SimdMode::SSE2;
    }
    // On the supported 8-point AoS kernel, AVX2 wins while the per-frame
    // working set is moderate. Around ten thousand cubes the wider masked
    // stores and AVX frequency cost lose to SSE2, so large models switch back.
    constexpr std::size_t kAvx2MaximumCubeCount = 6144;
    return cube_count <= kAvx2MaximumCubeCount ? core::SimdMode::AVX2
                                               : core::SimdMode::SSE2;
}

void CubeGeometry::transformPoint(const BonePoseCalculator::Pose& pose, double x, double y,
                                  double z, double result[3]) {
    transformPoint(pose, x, y, z, result, 0);
}

void CubeGeometry::transformPoint(const BonePoseCalculator::Pose& pose, double x, double y,
                                  double z, double* result, int offset) {
    if (result == nullptr) {
        throw std::invalid_argument("pose and three-component result are required");
    }
    if (offset < 0) {
        throw std::invalid_argument("transformed point result is too small");
    }
    const auto& linear = pose.world_linear;
    result[offset] = linear[0] * x + linear[1] * y + linear[2] * z +
                     pose.world_translation[0];
    result[offset + 1] = linear[3] * x + linear[4] * y + linear[5] * z +
                         pose.world_translation[1];
    result[offset + 2] = linear[6] * x + linear[7] * y + linear[8] * z +
                         pose.world_translation[2];
    if (!std::isfinite(result[offset]) || !std::isfinite(result[offset + 1]) ||
        !std::isfinite(result[offset + 2])) {
        throw std::invalid_argument("transformed model point must be finite");
    }
}

void CubeGeometry::requireCube(const loader::Cube& cube) {
    requireFinite(cube.origin, "cube origin");
    requireFinite(cube.size, "cube size");
    if (!std::isfinite(cube.inflate)) {
        throw std::invalid_argument("cube inflate must be finite");
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (cube.size[axis] != 0.0 &&
            std::abs(cube.size[axis]) + cube.inflate * 2.0 < 0) {
            throw std::invalid_argument("cube inflate shrinks an effective size below zero");
        }
    }
    if (cube.has_pivot) {
        requireFinite(cube.pivot, "cube pivot");
    }
    if (cube.has_rotation) {
        requireFinite(cube.rotation, "cube rotation");
    }
}

void CubeGeometry::requireFinite(const double value[3], const char* label) {
    if (!std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2])) {
        throw std::invalid_argument(std::string(label) + " must contain three finite values");
    }
}

}
