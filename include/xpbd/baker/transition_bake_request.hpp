#pragma once

#include "xpbd/loader/bedrock_animation_data.hpp"

#include <map>
#include <stdexcept>
#include <string>

namespace xpbd::baker {

struct TransitionBakeRequest {
    const loader::Animation* source_animation = nullptr;
    const loader::Animation* target_animation = nullptr;
    double source_exit_time = 0.0;
    double target_entry_time = 0.0;
    double transition_duration = 0.0;
    std::map<std::string, double> per_bone_follow_weight;

    TransitionBakeRequest(const loader::Animation& source, const loader::Animation& target,
                          double source_exit, double target_entry, double duration,
                          std::map<std::string, double> weights = {});

    static TransitionBakeRequest endingAtClipBoundary(const loader::Animation& source,
                                                      const loader::Animation& target,
                                                      double duration);

    [[nodiscard]] double followWeight(const std::string& bone_name) const;
};

}
