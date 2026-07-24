#pragma once

#include "xpbd/baker/rotation_util.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <map>
#include <string>
#include <vector>

namespace xpbd::baker {





class BedrockTransformResolver {
public:

    class Matrix4 {
    public:
        [[nodiscard]] static Matrix4 identity();
        [[nodiscard]] static Matrix4 translation(double x, double y, double z);
        [[nodiscard]] static Matrix4 rotation(const RotationUtil::Quat& quaternion);

        [[nodiscard]] Matrix4 multiply(const Matrix4& right) const;
        [[nodiscard]] std::array<double, 3> transformPoint(double x, double y, double z) const;
        [[nodiscard]] double get(int row, int column) const;

    private:
        explicit Matrix4(std::array<double, 16> values);
        std::array<double, 16> values_{};
    };

    [[nodiscard]] static std::array<double, 3> convertBedrockVector(
        const double vector[3]);
    [[nodiscard]] static std::array<double, 3> convertBedrockVector(
        const std::array<double, 3>& vector);

    [[nodiscard]] static Matrix4 resolveBoneLocalMatrix(const loader::Bone& bone);
    [[nodiscard]] static Matrix4 resolveBoneLocalMatrix(const loader::Bone& bone,
                                                        const double translation[3],
                                                        const double rotation[3]);

    [[nodiscard]] static std::map<std::string, Matrix4> resolveBoneWorldMatrices(
        const std::vector<loader::Bone>& bones);

    [[nodiscard]] static Matrix4 resolveCubeLocalMatrix(const loader::Cube& cube);

private:
    BedrockTransformResolver() = delete;

    static Matrix4 resolveBoneWorldMatrix(const loader::Bone& bone, const Matrix4* parentWorld);
    static Matrix4 resolveBoneWorldMatrixRecursive(
        const loader::Bone& bone, const std::map<std::string, loader::Bone>& byName,
        std::map<std::string, Matrix4>& cache, std::vector<std::string>& visiting);

    static Matrix4 rotationAroundPivot(const double pivot[3], const double rotation[3]);
    static void requireBone(const loader::Bone& bone);
    static void requireCube(const loader::Cube& cube);
    static void requireVector(const double vector[3], const char* label);
};

}
