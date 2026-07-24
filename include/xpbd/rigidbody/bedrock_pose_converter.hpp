#pragma once

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/rigidbody/rigid_body_types.hpp"

#include <array>
#include <optional>

namespace xpbd::rigidbody {

struct LocalChannels {
    std::array<double, 3> position{0, 0, 0};
    std::array<double, 3> rotation{0, 0, 0};
};

class BedrockPoseConverter {
public:


    static Transform fromPose(const baker::BonePoseCalculator::Pose& pose, double unit_scale);




    static LocalChannels toLocalChannels(const loader::Bone& bone,
                                         const Transform& world_pivot_transform,
                                         const loader::Bone* parent_bone,
                                         const Transform* parent_world_pivot_transform,
                                         double unit_scale);
};

}
