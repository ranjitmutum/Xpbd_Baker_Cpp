#include "xpbd/baker/cube_geometry.hpp"

#include "xpbd/baker/bedrock_transform_resolver.hpp"
#include <cmath>
#include <stdexcept>

namespace xpbd::baker {

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
    return {std::min(cube.origin[0], cube.origin[0] + cube.size[0]) - cube.inflate,
            std::min(cube.origin[1], cube.origin[1] + cube.size[1]) - cube.inflate,
            std::min(cube.origin[2], cube.origin[2] + cube.size[2]) - cube.inflate};
}

std::array<double, 3> CubeGeometry::effectiveSize(const loader::Cube& cube) {
    requireCube(cube);
    return {std::abs(cube.size[0]) + cube.inflate * 2.0,
            std::abs(cube.size[1]) + cube.inflate * 2.0,
            std::abs(cube.size[2]) + cube.inflate * 2.0};
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
        if (std::abs(cube.size[axis]) + cube.inflate * 2.0 < 0) {
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
