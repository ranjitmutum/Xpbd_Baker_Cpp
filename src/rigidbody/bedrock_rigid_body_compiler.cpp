#include "xpbd/rigidbody/bedrock_rigid_body_compiler.hpp"

#include "xpbd/baker/cube_geometry.hpp"
#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace xpbd::rigidbody {
namespace {

using Vec3 = baker::RotationUtil::Vec3;

Vec3 subtract(const Vec3 &left, const Vec3 &right) {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

double dot(const Vec3 &left, const Vec3 &right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

Vec3 cross(const Vec3 &left, const Vec3 &right) {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

double length(const Vec3 &value) {
    return std::sqrt(dot(value, value));
}

Vec3 normalized(const Vec3 &value, const std::string& context) {
    const double magnitude = length(value);
    if (!std::isfinite(magnitude) || !(magnitude > 0.0)) {
        throw std::invalid_argument(context + " has a non-finite or zero edge");
    }
    return {value[0] / magnitude, value[1] / magnitude, value[2] / magnitude};
}

}

std::vector<ColliderDiagnostic> BedrockRigidBodyCompiler::diagnose(
    const BodyDefinition& definition, double unit_scale) {
    std::vector<ColliderDiagnostic> diagnostics;
    diagnostics.reserve(definition.boxes.size());
    for (std::size_t index = 0; index < definition.boxes.size(); ++index) {
        const auto& box = definition.boxes[index];
        ColliderDiagnostic diagnostic;
        diagnostic.body_name = definition.name;
        diagnostic.box_index = static_cast<int>(index);
        diagnostic.bullet_half_extents = box.half_extents;
        diagnostic.unit_scale = unit_scale;
        diagnostic.minimum_half_extent = std::min(
            box.half_extents[0], std::min(box.half_extents[1], box.half_extents[2]));
        diagnostic.maximum_half_extent = std::max(
            box.half_extents[0], std::max(box.half_extents[1], box.half_extents[2]));
        diagnostic.aspect_ratio = diagnostic.maximum_half_extent /
                                  diagnostic.minimum_half_extent;
        diagnostic.collision_margin =
            bulletBoxCollisionMargin(diagnostic.minimum_half_extent);
        diagnostic.margin_to_minimum_half_extent =
            diagnostic.collision_margin / diagnostic.minimum_half_extent;
        diagnostic.ccd_enabled = definition.ccd.enabled;
        diagnostic.ccd_radius = definition.ccd.swept_sphere_radius;

        const auto addIssue = [&](ColliderRiskLevel risk,
                                  const std::string& issue) {
            if (risk == ColliderRiskLevel::Error ||
                (risk == ColliderRiskLevel::Warning &&
                 diagnostic.risk == ColliderRiskLevel::Safe)) {
                diagnostic.risk = risk;
            }
            diagnostic.issues.push_back(issue);
        };
        if (diagnostic.minimum_half_extent <=
            kBulletSafetyThresholds.minimum_half_extent_error) {
            std::ostringstream issue;
            issue << "minimum half extent " << diagnostic.minimum_half_extent
                  << " after Unit Scale " << unit_scale
                  << " is at or below the error threshold "
                  << kBulletSafetyThresholds.minimum_half_extent_error
                  << " Bullet units";
            addIssue(ColliderRiskLevel::Error, issue.str());
        } else if (diagnostic.minimum_half_extent <=
                   kBulletSafetyThresholds.minimum_half_extent_warning) {
            std::ostringstream issue;
            issue << "minimum half extent " << diagnostic.minimum_half_extent
                  << " after Unit Scale " << unit_scale
                  << " is at or below the warning threshold "
                  << kBulletSafetyThresholds.minimum_half_extent_warning
                  << " Bullet units";
            addIssue(ColliderRiskLevel::Warning, issue.str());
        }
        if (diagnostic.aspect_ratio >=
            kBulletSafetyThresholds.aspect_ratio_error) {
            std::ostringstream issue;
            issue << "aspect ratio " << diagnostic.aspect_ratio
                  << " is at or above the error threshold "
                  << kBulletSafetyThresholds.aspect_ratio_error;
            addIssue(ColliderRiskLevel::Error, issue.str());
        } else if (diagnostic.aspect_ratio >=
                   kBulletSafetyThresholds.aspect_ratio_warning) {
            std::ostringstream issue;
            issue << "aspect ratio " << diagnostic.aspect_ratio
                  << " is at or above the warning threshold "
                  << kBulletSafetyThresholds.aspect_ratio_warning;
            addIssue(ColliderRiskLevel::Warning, issue.str());
        }
        if (diagnostic.margin_to_minimum_half_extent <
                kBulletSafetyThresholds
                    .margin_to_minimum_extent_warning_minimum ||
            diagnostic.margin_to_minimum_half_extent >
                kBulletSafetyThresholds
                    .margin_to_minimum_extent_warning_maximum) {
            std::ostringstream issue;
            issue << "collision margin / minimum half extent ratio "
                  << diagnostic.margin_to_minimum_half_extent
                  << " is outside the warning interval ["
                  << kBulletSafetyThresholds
                         .margin_to_minimum_extent_warning_minimum
                  << ", "
                  << kBulletSafetyThresholds
                         .margin_to_minimum_extent_warning_maximum
                  << "]";
            addIssue(ColliderRiskLevel::Warning, issue.str());
        }
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

Compilation BedrockRigidBodyCompiler::compile(
    const loader::Bone& bone, const baker::BonePoseCalculator::Pose& initial_pose,
    MotionType motion_type, double mass, double unit_scale, double friction, double restitution,
    bool enable_ccd) {
    return compileCompound(bone, initial_pose, {CubeSource{&bone, initial_pose}}, motion_type,
                           mass, unit_scale, friction, restitution, enable_ccd);
}

Compilation BedrockRigidBodyCompiler::compileCompound(
    const loader::Bone& body_bone, const baker::BonePoseCalculator::Pose& body_pose,
    const std::vector<CubeSource>& cube_sources, MotionType motion_type, double mass,
    double unit_scale, double friction, double restitution, bool enable_ccd) {
    if (body_bone.name.empty()) {
        throw std::invalid_argument("bone name is required");
    }
    if (!std::isfinite(unit_scale) || !(unit_scale > 0)) {
        throw std::invalid_argument("unit scale must be finite and greater than zero");
    }

    Compilation result;
    std::vector<BoxShape> boxes;
    double smallestHalfExtent = std::numeric_limits<double>::infinity();
    const auto inverseBodyRotation =
        baker::RotationUtil::quaternionInverse(body_pose.world_rotation);

    for (const auto& source : cube_sources) {
        if (source.bone == nullptr) {
            continue;
        }
        result.source_cube_count += static_cast<int>(source.bone->cubes.size());
        for (const auto& cube : source.bone->cubes) {
            const auto bindVertices = baker::CubeGeometry::bindVertices(cube);
            std::array<Vec3, 8> worldVertices{};
            Vec3 worldCenter{0.0, 0.0, 0.0};
            for (int vertex = 0; vertex < 8; ++vertex) {
                const int offset = vertex * 3;
                baker::CubeGeometry::transformPoint(
                    source.pose,
                    bindVertices[static_cast<std::size_t>(offset)],
                    bindVertices[static_cast<std::size_t>(offset + 1)],
                    bindVertices[static_cast<std::size_t>(offset + 2)],
                    worldVertices[static_cast<std::size_t>(vertex)].data());
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    worldCenter[axis] +=
                        worldVertices[static_cast<std::size_t>(vertex)][axis] /
                        8.0;
                }
            }

            const std::array<Vec3, 3> edges{
                subtract(worldVertices[1], worldVertices[0]),
                subtract(worldVertices[2], worldVertices[0]),
                subtract(worldVertices[4], worldVertices[0])};
            const std::string geometryContext =
                "cube on bone '" + source.bone->name + "' owned by rigid body '" +
                body_bone.name + "'";
            std::array<double, 3> edgeLengths{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                edgeLengths[axis] = length(edges[axis]);
            }

            const std::array<double, 3> halfExtents{
                edgeLengths[0] * unit_scale * 0.5,
                edgeLengths[1] * unit_scale * 0.5,
                edgeLengths[2] * unit_scale * 0.5};
            const bool hasSafeExtents = std::all_of(
                halfExtents.begin(), halfExtents.end(), [](double halfExtent) {
                    return std::isfinite(halfExtent) &&
                           halfExtent >
                               kBulletSafetyThresholds.minimum_half_extent_error;
                });
            if (!hasSafeExtents) {
                ++result.skipped_degenerate_cube_count;
                continue;
            }
            std::array<Vec3, 3> axes{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                axes[axis] = normalized(edges[axis], geometryContext);
            }
            constexpr double kOrthogonalityTolerance = 1e-8;
            if (std::abs(dot(axes[0], axes[1])) > kOrthogonalityTolerance ||
                std::abs(dot(axes[0], axes[2])) > kOrthogonalityTolerance ||
                std::abs(dot(axes[1], axes[2])) > kOrthogonalityTolerance) {
                throw std::invalid_argument(
                    geometryContext +
                    " becomes sheared after inherited non-uniform scale; "
                    "Bullet box geometry requires orthogonal edges");
            }
            const double handedness = dot(cross(axes[0], axes[1]), axes[2]);
            if (!std::isfinite(handedness) ||
                std::abs(handedness) < 1.0 - 1e-8) {
                throw std::invalid_argument(
                    geometryContext +
                    " has an invalid orthogonal basis");
            }
            if (handedness < 0.0) {



                for (double& component : axes[2]) {
                    component = -component;
                }
            }

            const std::array<double, 3> centerDelta{
                worldCenter[0] - body_pose.world_position[0],
                worldCenter[1] - body_pose.world_position[1],
                worldCenter[2] - body_pose.world_position[2]};
            auto center = baker::RotationUtil::rotateVector(inverseBodyRotation, centerDelta);
            for (int axis = 0; axis < 3; ++axis) {
                center[static_cast<std::size_t>(axis)] *= unit_scale;
            }

            const std::array<Vec3, 3> standardAxes{
                Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0},
                Vec3{0.0, 0.0, 1.0}};
            const auto sourceWorldRotation =
                baker::RotationUtil::quaternionFromDirectionPairs(
                    standardAxes.data(), axes.data(), 3);
            const auto localRotation =
                baker::RotationUtil::quaternionMultiply(inverseBodyRotation, sourceWorldRotation);
            BoxShape box;
            box.half_extents = halfExtents;
            box.local_transform.translation = center;
            box.local_transform.rotation = localRotation;
            boxes.push_back(box);
            for (double half : halfExtents) {
                smallestHalfExtent = std::min(smallestHalfExtent, half);
            }
        }
    }

    if (boxes.empty()) {
        result.diagnostic =
            "bone has no non-degenerate cube in its owned groups: " + body_bone.name;
        return result;
    }

    BodyDefinition definition;
    definition.name = body_bone.name;
    definition.motion_type = motion_type;
    definition.boxes = std::move(boxes);
    definition.mass = motion_type == MotionType::Dynamic ? mass : 0.0;
    definition.friction = friction;
    definition.restitution = restitution;
    definition.initial_bone_transform.translation = {
        body_pose.world_position[0] * unit_scale, body_pose.world_position[1] * unit_scale,
        body_pose.world_position[2] * unit_scale};
    definition.initial_bone_transform.rotation = body_pose.world_rotation;
    if (motion_type == MotionType::Dynamic && enable_ccd) {
        definition.ccd = CcdSettings{true, smallestHalfExtent * 0.5, smallestHalfExtent * 0.8};
    }
    if (result.skipped_degenerate_cube_count > 0) {
        result.diagnostic =
            "skipped " + std::to_string(result.skipped_degenerate_cube_count) +
            " unsafe or degenerate cube(s) owned by " + body_bone.name;
    }
    result.body = std::move(definition);
    result.collider_diagnostics = diagnose(*result.body, unit_scale);
    return result;
}

}
