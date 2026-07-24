#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace xpbd::baker {

class LoopSeamCorrector {
public:
    struct Result {
        std::vector<BakedFrame> frames;
        int window_start_index = 0;
        double window_duration_seconds = 0.0;
        double window_ratio = 0.0;
        int canonicalized_bone_count = 0;
        int preserved_driver_bone_count = 0;
        int driver_endpoint_conflict_count = 0;
    };

    static Result correctCopy(const std::vector<BakedFrame>& source,
                              const std::map<std::string, loader::Bone>& bones_by_name,
                              const std::set<std::string>& corrected_bones,
                              double window_ratio,
                              bool match_acceleration);

    static Result correctHierarchyCopy(
        const std::vector<BakedFrame>& source, const std::vector<loader::Bone>& bones,
        const loader::Animation* animation, const std::set<std::string>& physics_bones,
        const std::set<std::string>& fixed_physics_bones,
        BoneMapper::LoopSeamStrategy strategy, double window_ratio,
        bool match_acceleration);
};

}
