#include "xpbd/baker/bedrock_transform_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace xpbd::baker {

BedrockTransformResolver::Matrix4::Matrix4(std::array<double, 16> values)
    : values_(values) {}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::Matrix4::identity() {
    return Matrix4(std::array<double, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::Matrix4::translation(double x,
                                                                                 double y,
                                                                                 double z) {
    Matrix4 result = identity();
    result.values_[3] = x;
    result.values_[7] = y;
    result.values_[11] = z;
    return result;
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::Matrix4::rotation(
    const RotationUtil::Quat& quaternion) {
    double x = quaternion[0], y = quaternion[1], z = quaternion[2], w = quaternion[3];
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(length > 1e-20) || !std::isfinite(length)) {
        throw std::invalid_argument("finite non-zero quaternion is required");
    }
    x /= length;
    y /= length;
    z /= length;
    w /= length;
    return Matrix4(std::array<double, 16>{
        1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0,
        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0,
        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0, 0, 0, 0, 1});
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::Matrix4::multiply(
    const Matrix4& right) const {
    std::array<double, 16> result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int inner = 0; inner < 4; ++inner) {
                result[static_cast<std::size_t>(row * 4 + column)] +=
                    values_[static_cast<std::size_t>(row * 4 + inner)] *
                    right.values_[static_cast<std::size_t>(inner * 4 + column)];
            }
        }
    }
    return Matrix4(result);
}

std::array<double, 3> BedrockTransformResolver::Matrix4::transformPoint(double x, double y,
                                                                        double z) const {
    return {values_[0] * x + values_[1] * y + values_[2] * z + values_[3],
            values_[4] * x + values_[5] * y + values_[6] * z + values_[7],
            values_[8] * x + values_[9] * y + values_[10] * z + values_[11]};
}

double BedrockTransformResolver::Matrix4::get(int row, int column) const {
    if (row < 0 || row > 3 || column < 0 || column > 3) {
        throw std::out_of_range("matrix index must be in [0, 3]");
    }
    return values_[static_cast<std::size_t>(row * 4 + column)];
}

std::array<double, 3> BedrockTransformResolver::convertBedrockVector(const double vector[3]) {
    requireVector(vector, "Bedrock vector");
    return {-vector[0], vector[1], vector[2]};
}

std::array<double, 3> BedrockTransformResolver::convertBedrockVector(
    const std::array<double, 3>& vector) {
    return convertBedrockVector(vector.data());
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::resolveBoneLocalMatrix(
    const loader::Bone& bone) {
    const double zero[3] = {0.0, 0.0, 0.0};
    return resolveBoneLocalMatrix(bone, zero, bone.rotation);
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::resolveBoneLocalMatrix(
    const loader::Bone& bone, const double translation[3], const double rotation[3]) {
    requireBone(bone);
    requireVector(translation, "bone translation");
    requireVector(rotation, "bone rotation");
    const auto mappedTranslation = convertBedrockVector(translation);
    const Matrix4 pivotRotation = rotationAroundPivot(bone.pivot, rotation);
    return Matrix4::translation(mappedTranslation[0], mappedTranslation[1], mappedTranslation[2])
        .multiply(pivotRotation);
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::resolveBoneWorldMatrix(
    const loader::Bone& bone, const Matrix4* parentWorld) {
    const Matrix4 local = resolveBoneLocalMatrix(bone);
    return parentWorld == nullptr ? local : parentWorld->multiply(local);
}

std::map<std::string, BedrockTransformResolver::Matrix4>
BedrockTransformResolver::resolveBoneWorldMatrices(const std::vector<loader::Bone>& bones) {
    std::map<std::string, loader::Bone> byName;
    for (const auto& bone : bones) {
        requireBone(bone);
        byName[bone.name] = bone;
    }
    std::map<std::string, Matrix4> result;
    std::vector<std::string> visiting;
    for (const auto& bone : bones) {
        resolveBoneWorldMatrixRecursive(bone, byName, result, visiting);
    }
    return result;
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::resolveCubeLocalMatrix(
    const loader::Cube& cube) {
    requireCube(cube);
    const double rawCenter[3] = {cube.origin[0] + cube.size[0] * 0.5,
                                 cube.origin[1] + cube.size[1] * 0.5,
                                 cube.origin[2] + cube.size[2] * 0.5};
    const auto center = convertBedrockVector(rawCenter);
    const double* pivot = cube.has_pivot ? cube.pivot : rawCenter;
    const double zeroRot[3] = {0.0, 0.0, 0.0};
    const double* rotation = cube.has_rotation ? cube.rotation : zeroRot;
    return rotationAroundPivot(pivot, rotation)
        .multiply(Matrix4::translation(center[0], center[1], center[2]));
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::resolveBoneWorldMatrixRecursive(
    const loader::Bone& bone, const std::map<std::string, loader::Bone>& byName,
    std::map<std::string, Matrix4>& cache, std::vector<std::string>& visiting) {
    auto cached = cache.find(bone.name);
    if (cached != cache.end()) {
        return cached->second;
    }
    if (std::find(visiting.begin(), visiting.end(), bone.name) != visiting.end()) {
        throw std::invalid_argument("Bone hierarchy contains a cycle at " + bone.name);
    }
    visiting.push_back(bone.name);
    const Matrix4* parentWorldPtr = nullptr;
    Matrix4 parentWorldStorage = Matrix4::identity();
    if (bone.has_parent) {
        auto parentIt = byName.find(bone.parent);
        if (parentIt == byName.end()) {
            throw std::invalid_argument("Missing parent '" + bone.parent + "' for bone " +
                                        bone.name);
        }
        parentWorldStorage =
            resolveBoneWorldMatrixRecursive(parentIt->second, byName, cache, visiting);
        parentWorldPtr = &parentWorldStorage;
    }
    Matrix4 world = resolveBoneWorldMatrix(bone, parentWorldPtr);
    cache.emplace(bone.name, world);
    visiting.pop_back();
    return world;
}

BedrockTransformResolver::Matrix4 BedrockTransformResolver::rotationAroundPivot(
    const double pivot[3], const double rotation[3]) {
    const auto mappedPivot = convertBedrockVector(pivot);
    const auto quaternion =
        RotationUtil::quaternionFromBedrockEuler(rotation[0], rotation[1], rotation[2]);
    return Matrix4::translation(mappedPivot[0], mappedPivot[1], mappedPivot[2])
        .multiply(Matrix4::rotation(quaternion))
        .multiply(Matrix4::translation(-mappedPivot[0], -mappedPivot[1], -mappedPivot[2]));
}

void BedrockTransformResolver::requireBone(const loader::Bone& bone) {
    if (bone.name.empty()) {
        throw std::invalid_argument("named bone is required");
    }
    requireVector(bone.pivot, "bone pivot");
    requireVector(bone.rotation, "bone rotation");
}

void BedrockTransformResolver::requireCube(const loader::Cube& cube) {
    requireVector(cube.origin, "cube origin");
    requireVector(cube.size, "cube size");
    if (cube.has_pivot) {
        requireVector(cube.pivot, "cube pivot");
    }
    if (cube.has_rotation) {
        requireVector(cube.rotation, "cube rotation");
    }
}

void BedrockTransformResolver::requireVector(const double vector[3], const char* label) {
    if (vector == nullptr || !std::isfinite(vector[0]) || !std::isfinite(vector[1]) ||
        !std::isfinite(vector[2])) {
        throw std::invalid_argument(std::string(label) + " must contain three finite values");
    }
}

}
