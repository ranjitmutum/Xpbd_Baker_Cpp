#include "xpbd/baker/transition_bake_request.hpp"

#include <cmath>
#include <limits>

namespace xpbd::baker {

namespace {
bool normalizeTimelineTime(double &value, double maximum) {
    if (!std::isfinite(value) || !std::isfinite(maximum) || maximum < 0.0) {
        return false;
    }
    const double tolerance =
        std::max(1e-12, 8.0 * std::numeric_limits<float>::epsilon() *
                            std::max(1.0, std::abs(maximum)));
    if (value < -tolerance || value > maximum + tolerance) {
        return false;
    }
    if (std::abs(value) <= tolerance) {
        value = 0.0;
    } else if (std::abs(value - maximum) <= tolerance) {
        value = maximum;
    }
    return true;
}
}

TransitionBakeRequest::TransitionBakeRequest(const loader::Animation& source,
                                             const loader::Animation& target,
                                             double source_exit, double target_entry,
                                             double duration,
                                             std::map<std::string, double> weights)
    : source_animation(&source),
      target_animation(&target),
      source_exit_time(source_exit),
      target_entry_time(target_entry),
      transition_duration(duration),
      per_bone_follow_weight(std::move(weights)) {
    if (!std::isfinite(source.animation_length) || source.animation_length < 0 ||
        !std::isfinite(target.animation_length) || target.animation_length < 0 ||
        !normalizeTimelineTime(source_exit_time, source.animation_length) ||
        !normalizeTimelineTime(target_entry_time, target.animation_length) ||
        !std::isfinite(transition_duration) || !(transition_duration > 0)) {
        throw std::invalid_argument("invalid transition timing");
    }
    for (const auto& [key, value] : per_bone_follow_weight) {
        if (key.empty() || !std::isfinite(value) || value < 0.0 || value > 1.0) {
            throw std::invalid_argument("per-bone transition weights must be in [0, 1]");
        }
    }
}

TransitionBakeRequest TransitionBakeRequest::endingAtClipBoundary(
    const loader::Animation& source, const loader::Animation& target, double duration) {
    return TransitionBakeRequest(source, target, std::max(0.0, source.animation_length), 0.0,
                                 duration, {});
}

double TransitionBakeRequest::followWeight(const std::string& bone_name) const {
    auto it = per_bone_follow_weight.find(bone_name);
    return it == per_bone_follow_weight.end() ? 1.0 : it->second;
}

}
