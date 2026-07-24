#pragma once

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/rigidbody/rigid_body_types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace xpbd::rigidbody {

struct Compilation {
    std::optional<BodyDefinition> body;
    std::vector<ColliderDiagnostic> collider_diagnostics;
    int source_cube_count = 0;
    int skipped_degenerate_cube_count = 0;
    std::string diagnostic;
};

struct CubeSource {
    const loader::Bone* bone = nullptr;
    baker::BonePoseCalculator::Pose pose{};
};

class BedrockRigidBodyCompiler {
public:
    static std::vector<ColliderDiagnostic> diagnose(
        const BodyDefinition& definition, double unit_scale);

    static Compilation compile(const loader::Bone& bone,
                               const baker::BonePoseCalculator::Pose& initial_pose,
                               MotionType motion_type, double mass, double unit_scale,
                               double friction, double restitution, bool enable_ccd);

    static Compilation compileCompound(const loader::Bone& body_bone,
                                       const baker::BonePoseCalculator::Pose& body_pose,
                                       const std::vector<CubeSource>& cube_sources,
                                       MotionType motion_type, double mass, double unit_scale,
                                       double friction, double restitution, bool enable_ccd);
};

}
