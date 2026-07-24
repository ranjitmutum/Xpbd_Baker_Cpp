#pragma once

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>

namespace xpbd::baker {


class CubeGeometry {
public:
    [[nodiscard]] static std::array<double, 3> effectivePivot(const loader::Cube& cube);
    [[nodiscard]] static std::array<double, 3> effectiveOrigin(const loader::Cube& cube);
    [[nodiscard]] static std::array<double, 3> effectiveSize(const loader::Cube& cube);


    [[nodiscard]] static std::array<double, 24> bindVertices(const loader::Cube& cube);

    static void transformPoint(const BonePoseCalculator::Pose& pose, double x, double y,
                               double z, double result[3]);
    static void transformPoint(const BonePoseCalculator::Pose& pose, double x, double y,
                               double z, double* result, int offset);

private:
    CubeGeometry() = delete;
    static void requireCube(const loader::Cube& cube);
    static void requireFinite(const double value[3], const char* label);
};

}
