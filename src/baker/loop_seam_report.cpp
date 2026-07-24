#include "xpbd/baker/loop_seam_report.hpp"

#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xpbd::baker {
namespace {

struct MetricsAccumulator {
    double position = 0;
    double rotation = 0;
    double linear_velocity = 0;
    double angular_velocity = 0;
    double linear_acceleration = 0;
    double angular_acceleration = 0;
    std::string position_bone;
    std::string rotation_bone;
    std::string linear_velocity_bone;
    std::string angular_velocity_bone;
    std::string linear_acceleration_bone;
    std::string angular_acceleration_bone;
    double peak_linear_velocity = 0;
    double peak_angular_velocity = 0;
    double peak_linear_acceleration = 0;
    double peak_angular_acceleration = 0;

    void pos(double value, const std::string& bone) {
        if (value > position) {
            position = value;
            position_bone = bone;
        }
    }
    void rot(double value, const std::string& bone) {
        if (value > rotation) {
            rotation = value;
            rotation_bone = bone;
        }
    }
    void linVel(double value, const std::string& bone) {
        if (value > linear_velocity) {
            linear_velocity = value;
            linear_velocity_bone = bone;
        }
    }
    void angVel(double value, const std::string& bone) {
        if (value > angular_velocity) {
            angular_velocity = value;
            angular_velocity_bone = bone;
        }
    }
    void linAcc(double value, const std::string& bone) {
        if (value > linear_acceleration) {
            linear_acceleration = value;
            linear_acceleration_bone = bone;
        }
    }
    void angAcc(double value, const std::string& bone) {
        if (value > angular_acceleration) {
            angular_acceleration = value;
            angular_acceleration_bone = bone;
        }
    }

    LoopSeamReport::Metrics build() const {
        LoopSeamReport::Metrics m;
        m.maximum_position_error = position;
        m.position_bone = position_bone;
        m.maximum_rotation_error_radians = rotation;
        m.rotation_bone = rotation_bone;
        m.maximum_linear_velocity_jump = linear_velocity;
        m.linear_velocity_bone = linear_velocity_bone;
        m.maximum_angular_velocity_jump = angular_velocity;
        m.angular_velocity_bone = angular_velocity_bone;
        m.maximum_linear_acceleration_jump = linear_acceleration;
        m.linear_acceleration_bone = linear_acceleration_bone;
        m.maximum_angular_acceleration_jump = angular_acceleration;
        m.angular_acceleration_bone = angular_acceleration_bone;
        m.peak_linear_velocity = peak_linear_velocity;
        m.peak_angular_velocity = peak_angular_velocity;
        m.peak_linear_acceleration = peak_linear_acceleration;
        m.peak_angular_acceleration = peak_angular_acceleration;
        return m;
    }
};

}

LoopSeamReport::LoopSeamReport(Metrics local, Metrics final_world, Metrics driver,
                               Metrics physics_relative, Metrics quantized_local,
                               Metrics quantized_final_world, bool correction_applied,
                               double correction_window_duration_seconds,
                               double correction_window_ratio, bool collision_safe,
                               double maximum_penetration, bool joint_safe,
                               bool audit_valid, bool driver_available, Validation validation,
                               std::vector<AnchorCoverage> anchor_coverage)
    : local_(std::move(local)),
      final_world_(std::move(final_world)),
      driver_(std::move(driver)),
      physics_relative_(std::move(physics_relative)),
      quantized_local_(std::move(quantized_local)),
      quantized_final_world_(std::move(quantized_final_world)),
      correction_applied_(correction_applied),
      correction_window_duration_seconds_(correction_window_duration_seconds),
      correction_window_ratio_(correction_window_ratio),
      collision_safe_(collision_safe),
      joint_safe_(joint_safe),
      audit_valid_(audit_valid),
      maximum_penetration_(maximum_penetration),
      driver_available_(driver_available),
      validation_(std::move(validation)),
      anchor_coverage_(std::move(anchor_coverage)) {}

LoopSeamReport LoopSeamReport::measure(const std::vector<BakedFrame>& frames,
                                       const std::vector<loader::Bone>& bones,
                                       const loader::Animation* animation,
                                       const std::set<std::string>& physics_bones,
                                       const std::set<std::string>& fixed_physics_bones,
                                       bool correction_applied, double correction_window_ratio,
                                       bool collision_safe, double maximum_penetration,
                                       bool joint_safe, bool audit_valid,
                                       double correction_window_duration_seconds) {
    Validation validation;
    validation.sample_count_sufficient = frames.size() >= 3;
    validation.verified_continuity_order =
        frames.size() >= 4 ? 2 : frames.size() >= 3 ? 1 : frames.size() >= 2 ? 0 : -1;
    const auto invalidate = [&](const std::string& bone, const std::string& field,
                                const std::string& space, bool missing) {
        validation.valid = false;
        if (missing) {
            ++validation.missing_bone_count;
            if (validation.first_missing_bone.empty()) {
                validation.first_missing_bone = bone;
            }
        } else {
            ++validation.non_finite_value_count;
        }
        if (validation.first_invalid_field.empty()) {
            validation.first_invalid_field = field;
            validation.affected_metric_space = space;
        }
    };
    for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
        const auto& frame = frames[frame_index];
        if (!std::isfinite(frame.time)) {
            invalidate({}, "frame_time", "Timeline", false);
        }
        if (frame_index > 0 &&
            !(frame.time > frames[frame_index - 1].time)) {
            invalidate({}, "distinct_increasing_frame_time", "Timeline", false);
        }
        std::set<std::string> present;
        for (const auto& state : frame.bone_states) {
            present.insert(state.bone_name);
            const bool finite_position = std::all_of(
                state.position.begin(), state.position.end(),
                [](double value) { return std::isfinite(value); });
            const bool finite_rotation = std::all_of(
                state.rotation.begin(), state.rotation.end(),
                [](double value) { return std::isfinite(value); });
            if (!finite_position) {
                invalidate(state.bone_name, "position", "Local", false);
            }
            if (!finite_rotation) {
                invalidate(state.bone_name, "rotation", "Local", false);
            }
        }
        for (const auto& name : physics_bones) {
            if (!present.contains(name)) {
                invalidate(name, "bone_channel", "Local", true);
            }
        }
    }
    if (!std::isfinite(maximum_penetration)) {
        invalidate({}, "maximum_penetration", "Collision", false);
    }
    if (frames.size() < 2) {
        validation.valid = false;
        if (validation.first_invalid_field.empty()) {
            validation.first_invalid_field = "insufficient_samples";
            validation.affected_metric_space = "Timeline";
        }
        return LoopSeamReport(Metrics::empty(), Metrics::empty(), Metrics::empty(),
                              Metrics::empty(), Metrics::empty(), Metrics::empty(),
                              correction_applied, correction_window_duration_seconds,
                              correction_window_ratio, collision_safe,
                              maximum_penetration, joint_safe, audit_valid, false,
                              std::move(validation), {});
    }
    const auto measured = measuredSubtree(bones, physics_bones);
    const auto by_name = indexBones(bones);
    const auto anchors = findAnchors(measured, by_name, physics_bones, fixed_physics_bones);
    auto coverage =
        buildAnchorCoverage(by_name, physics_bones, fixed_physics_bones, anchors);
    validation.physics_relative_available = !coverage.empty() &&
        std::all_of(coverage.begin(), coverage.end(),
                    [](const AnchorCoverage& item) { return item.complete; });
    if (!validation.physics_relative_available) {
        validation.physics_relative_fallback_reason =
            "Physics Relative unavailable — no fixed anchor; using world-space seam gate";
    }
    const auto drivers = driverNames(by_name, physics_bones, fixed_physics_bones);

    const auto local = localTransforms(frames, by_name, false);
    const auto quantized_local = localTransforms(frames, by_name, true);
    const auto world = worldTransforms(frames, bones, animation, false);
    const auto quantized_world = worldTransforms(frames, bones, animation, true);
    const auto driver = driverTransforms(frames, bones, animation);
    const auto relative = relativeTransforms(world, anchors);
    validateTransformSamples(local, physics_bones, "Local", validation);
    validateTransformSamples(world, measured, "FinalWorld", validation);
    validateTransformSamples(driver, drivers, "Driver", validation);
    validateTransformSamples(quantized_local, physics_bones, "QuantizedLocal", validation);
    validateTransformSamples(quantized_world, measured, "QuantizedFinalWorld", validation);
    if (validation.physics_relative_available) {
        validateTransformSamples(relative, relativeNames(anchors), "PhysicsRelative", validation);
    }
    return LoopSeamReport(measureTransforms(local, physics_bones, frames),
                          measureTransforms(world, measured, frames),
                          measureTransforms(driver, drivers, frames),
                          measureTransforms(relative, relativeNames(anchors), frames),
                          measureTransforms(quantized_local, physics_bones, frames),
                          measureTransforms(quantized_world, measured, frames), correction_applied,
                          correction_window_duration_seconds, correction_window_ratio,
                          collision_safe, maximum_penetration,
                          joint_safe, audit_valid, !drivers.empty(), std::move(validation),
                          std::move(coverage));
}

bool LoopSeamReport::passes(const BoneMapper::PhysicsGroupConfig& config) const {
    return exportGate(config).passes();
}

LoopSeamReport::ContinuityGate
LoopSeamReport::physicsSeamGate(const BoneMapper::PhysicsGroupConfig& config) const {
    ContinuityGate gate;
    if (!validation_.valid || !audit_valid_ ||
        validation_.verified_continuity_order <
            (config.loop_seam_match_acceleration ? 2 : 1)) {
        return gate;
    }
    const Metrics& continuity =
        config.loop_seam_strategy == BoneMapper::LoopSeamStrategy::VisualSubtree
                || !validation_.physics_relative_available
            ? final_world_ : physics_relative_;
    const double linear_limit =
        std::max(config.loop_seam_minimum_linear_velocity_tolerance,
                 continuity.peak_linear_velocity * config.loop_seam_relative_velocity_tolerance);
    const double angular_limit =
        std::max(config.loop_seam_minimum_angular_velocity_tolerance,
                 continuity.peak_angular_velocity * config.loop_seam_relative_velocity_tolerance);
    gate.c0_pass = continuity.maximum_position_error <= 1e-4 &&
                   continuity.maximum_rotation_error_radians <= 1e-5;
    gate.c1_pass = continuity.maximum_linear_velocity_jump <= linear_limit &&
                   continuity.maximum_angular_velocity_jump <= angular_limit;
    gate.c2_pass = true;
    if (config.loop_seam_match_acceleration) {
        const double linear_acc_limit = std::max(
            config.loop_seam_minimum_linear_velocity_tolerance,
            continuity.peak_linear_acceleration * config.loop_seam_relative_acceleration_tolerance);
        const double angular_acc_limit = std::max(
            config.loop_seam_minimum_angular_velocity_tolerance,
            continuity.peak_angular_acceleration * config.loop_seam_relative_acceleration_tolerance);
        gate.c2_pass =
            continuity.maximum_linear_acceleration_jump <= linear_acc_limit &&
            continuity.maximum_angular_acceleration_jump <= angular_acc_limit;
    }
    return gate;
}

LoopSeamReport::ContinuityGate
LoopSeamReport::continuityGate(const BoneMapper::PhysicsGroupConfig& config) const {
    return physicsSeamGate(config);
}

LoopSeamReport::QuantizationGate LoopSeamReport::quantizationGate() const {
    QuantizationGate gate;
    if (!validation_.valid || !audit_valid_) {
        return gate;
    }
    gate.local_c0_pass =
        quantized_local_.maximum_position_error == 0.0 &&
        quantized_local_.maximum_rotation_error_radians <= 1e-5;
    gate.final_world_c0_pass =
        quantized_final_world_.maximum_position_error <= 1e-4 &&
        quantized_final_world_.maximum_rotation_error_radians <= 1e-5;
    return gate;
}

LoopSeamReport::CollisionGate LoopSeamReport::collisionGate(
    const BoneMapper::PhysicsGroupConfig& config) const {
    CollisionGate gate;
    gate.candidate_safe = audit_valid_ && collision_safe_;
    gate.penetration_safe =
        std::isfinite(maximum_penetration_) &&
        maximum_penetration_ <= config.rigid_body_maximum_safe_penetration;
    return gate;
}

LoopSeamReport::JointGate LoopSeamReport::jointGate() const {
    JointGate gate;
    gate.candidate_safe = audit_valid_ && joint_safe_;
    return gate;
}

LoopSeamReport::ExportGate LoopSeamReport::exportGate(
    const BoneMapper::PhysicsGroupConfig& config) const {
    ExportGate gate;
    gate.validation_pass =
        validation_.valid && audit_valid_ &&
        validation_.verified_continuity_order >=
            (config.loop_seam_match_acceleration ? 2 : 1);
    gate.physics_seam_pass = physicsSeamGate(config).passes();
    gate.driver_seam_pass = driverGate(config).passes();
    gate.quantization_pass = quantizationGate().passes();
    gate.collision_pass = collisionGate(config).passes();
    gate.joint_pass = jointGate().passes();
    return gate;
}

LoopSeamReport::DriverSeamGate
LoopSeamReport::driverGate(const BoneMapper::PhysicsGroupConfig& config) const {
    DriverSeamGate gate;
    gate.available = driver_available_;
    gate.metrics = driver_;
    if (!gate.available) {
        return gate;
    }
    gate.c0_pass = driver_.maximum_position_error <= 1e-4 &&
                   driver_.maximum_rotation_error_radians <= 1e-5;
    const double linear_limit =
        std::max(config.loop_seam_minimum_linear_velocity_tolerance,
                 driver_.peak_linear_velocity * config.loop_seam_relative_velocity_tolerance);
    const double angular_limit =
        std::max(config.loop_seam_minimum_angular_velocity_tolerance,
                 driver_.peak_angular_velocity * config.loop_seam_relative_velocity_tolerance);
    gate.c1_pass = driver_.maximum_linear_velocity_jump <= linear_limit &&
                   driver_.maximum_angular_velocity_jump <= angular_limit;
    if (config.loop_seam_match_acceleration) {
        const double linear_acc_limit = std::max(
            config.loop_seam_minimum_linear_velocity_tolerance,
            driver_.peak_linear_acceleration * config.loop_seam_relative_acceleration_tolerance);
        const double angular_acc_limit = std::max(
            config.loop_seam_minimum_angular_velocity_tolerance,
            driver_.peak_angular_acceleration * config.loop_seam_relative_acceleration_tolerance);
        gate.c2_pass =
            driver_.maximum_linear_acceleration_jump <= linear_acc_limit &&
            driver_.maximum_angular_acceleration_jump <= angular_acc_limit;
    }
    return gate;
}

double LoopSeamReport::qualityScore(const BoneMapper::PhysicsGroupConfig& config) const {
    if (!joint_safe_ || !collision_safe_) {
        return std::numeric_limits<double>::infinity();
    }
    return previewQualityScore(config);
}

double LoopSeamReport::previewQualityScore(
    const BoneMapper::PhysicsGroupConfig& config) const {
    if (!validation_.valid ||
        validation_.verified_continuity_order <
            (config.loop_seam_match_acceleration ? 2 : 1) ||
        !audit_valid_) {
        return std::numeric_limits<double>::infinity();
    }
    const Metrics& continuity =
        config.loop_seam_strategy == BoneMapper::LoopSeamStrategy::VisualSubtree
                || !validation_.physics_relative_available
            ? final_world_ : physics_relative_;
    const double linear_limit =
        std::max(config.loop_seam_minimum_linear_velocity_tolerance,
                 continuity.peak_linear_velocity * config.loop_seam_relative_velocity_tolerance);
    const double angular_limit =
        std::max(config.loop_seam_minimum_angular_velocity_tolerance,
                 continuity.peak_angular_velocity * config.loop_seam_relative_velocity_tolerance);
    double score = std::max(ratio(quantized_local_.maximum_position_error, 1e-12),
                            ratio(quantized_local_.maximum_rotation_error_radians, 1e-5));
    score = std::max(score, ratio(quantized_final_world_.maximum_position_error, 1e-4));
    score = std::max(score, ratio(quantized_final_world_.maximum_rotation_error_radians, 1e-5));
    score = std::max(score, ratio(continuity.maximum_position_error, 1e-4));
    score = std::max(score,
                     ratio(continuity.maximum_rotation_error_radians, 1e-5));
    score = std::max(score, ratio(continuity.maximum_linear_velocity_jump, linear_limit));
    score = std::max(score, ratio(continuity.maximum_angular_velocity_jump, angular_limit));
    if (config.loop_seam_match_acceleration) {
        score = std::max(
            score, ratio(continuity.maximum_linear_acceleration_jump,
                         std::max(config.loop_seam_minimum_linear_velocity_tolerance,
                                  continuity.peak_linear_acceleration *
                                      config.loop_seam_relative_acceleration_tolerance)));
        score = std::max(
            score, ratio(continuity.maximum_angular_acceleration_jump,
                         std::max(config.loop_seam_minimum_angular_velocity_tolerance,
                                  continuity.peak_angular_acceleration *
                                      config.loop_seam_relative_acceleration_tolerance)));
    }
    const auto driver_gate = driverGate(config);
    if (driver_gate.available) {
        score = std::max(score, ratio(driver_.maximum_position_error, 1e-4));
        score = std::max(score, ratio(driver_.maximum_rotation_error_radians, 1e-5));
        score = std::max(score, ratio(driver_.maximum_linear_velocity_jump,
                                      std::max(config.loop_seam_minimum_linear_velocity_tolerance,
                                               driver_.peak_linear_velocity *
                                                   config.loop_seam_relative_velocity_tolerance)));
        score = std::max(score, ratio(driver_.maximum_angular_velocity_jump,
                                      std::max(config.loop_seam_minimum_angular_velocity_tolerance,
                                               driver_.peak_angular_velocity *
                                                   config.loop_seam_relative_velocity_tolerance)));
    }
    return std::max(score,
                    ratio(maximum_penetration_, config.rigid_body_maximum_safe_penetration));
}

double LoopSeamReport::ratio(double value, double limit) {
    if (limit == std::numeric_limits<double>::infinity()) {
        return 0.0;
    }
    if (!(limit > 0.0)) {
        return value == 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
    }
    return value / limit;
}

LoopSeamReport::Metrics LoopSeamReport::measureTransforms(
    const std::vector<std::map<std::string, Transform>>& samples,
    const std::set<std::string>& names, const std::vector<BakedFrame>& frames) {
    if (samples.size() < 2 || names.empty()) {
        return Metrics::empty();
    }
    const int last = static_cast<int>(samples.size()) - 1;
    MetricsAccumulator result;
    for (const auto& name : names) {
        auto first_it = samples.front().find(name);
        auto end_it = samples.back().find(name);
        if (first_it == samples.front().end() || end_it == samples.back().end()) {
            continue;
        }
        result.pos(distance(first_it->second.position, end_it->second.position), name);
        result.rot(rotationDistance(first_it->second.rotation, end_it->second.rotation), name);

        const auto out_vel = velocity(samples, frames, name, 0, 1);
        const auto in_vel = velocity(samples, frames, name, last - 1, last);
        const auto out_ang = angularVelocity(samples, frames, name, 0, 1);
        const auto in_ang = angularVelocity(samples, frames, name, last - 1, last);
        result.linVel(distance(out_vel, in_vel), name);
        result.angVel(distance(out_ang, in_ang), name);

        if (samples.size() >= 3) {
            const auto out_acc = acceleration(samples, frames, name, 0, 1, 2, false);
            const auto in_acc = acceleration(samples, frames, name, last - 2, last - 1, last, false);
            const auto out_ang_acc = acceleration(samples, frames, name, 0, 1, 2, true);
            const auto in_ang_acc =
                acceleration(samples, frames, name, last - 2, last - 1, last, true);
            result.linAcc(distance(out_acc, in_acc), name);
            result.angAcc(distance(out_ang_acc, in_ang_acc), name);
        }
        for (int i = 1; i < static_cast<int>(samples.size()); ++i) {
            result.peak_linear_velocity =
                std::max(result.peak_linear_velocity, length(velocity(samples, frames, name, i - 1, i)));
            result.peak_angular_velocity = std::max(
                result.peak_angular_velocity, length(angularVelocity(samples, frames, name, i - 1, i)));
        }
        for (int i = 2; i < static_cast<int>(samples.size()); ++i) {
            result.peak_linear_acceleration = std::max(
                result.peak_linear_acceleration,
                length(acceleration(samples, frames, name, i - 2, i - 1, i, false)));
            result.peak_angular_acceleration = std::max(
                result.peak_angular_acceleration,
                length(acceleration(samples, frames, name, i - 2, i - 1, i, true)));
        }
    }
    return result.build();
}

std::vector<std::map<std::string, LoopSeamReport::Transform>> LoopSeamReport::localTransforms(
    const std::vector<BakedFrame>& frames, const std::map<std::string, loader::Bone>& bones,
    bool quantize) {
    std::vector<std::map<std::string, Transform>> result;
    result.reserve(frames.size());
    for (const auto& frame : frames) {
        std::map<std::string, Transform> sample;
        for (const auto& state : frame.bone_states) {
            auto bone_it = bones.find(state.bone_name);
            if (bone_it == bones.end()) {
                continue;
            }
            const auto position = LoopSeamReport::values(state.position, quantize);
            const auto rotation = LoopSeamReport::values(state.rotation, quantize);
            Transform t;
            t.position = position;
            t.rotation = RotationUtil::quaternionFromBedrockEuler(
                bone_it->second.rotation[0] + rotation[0],
                bone_it->second.rotation[1] + rotation[1],
                bone_it->second.rotation[2] + rotation[2]);
            sample.emplace(state.bone_name, t);
        }
        result.push_back(std::move(sample));
    }
    return result;
}

std::vector<std::map<std::string, LoopSeamReport::Transform>> LoopSeamReport::worldTransforms(
    const std::vector<BakedFrame>& frames, const std::vector<loader::Bone>& bones,
    const loader::Animation* animation, bool quantize) {
    std::vector<std::map<std::string, Transform>> result;
    result.reserve(frames.size());
    for (const auto& frame : frames) {
        std::map<std::string, std::array<double, 3>> positions;
        std::map<std::string, std::array<double, 3>> rotations;
        for (const auto& state : frame.bone_states) {
            positions[state.bone_name] = LoopSeamReport::values(state.position, quantize);
            rotations[state.bone_name] = LoopSeamReport::values(state.rotation, quantize);
        }
        result.push_back(transformsFromPoses(
            BonePoseCalculator::calculate(bones, animation, frame.time, &positions, &rotations)));
    }
    return result;
}

std::vector<std::map<std::string, LoopSeamReport::Transform>> LoopSeamReport::driverTransforms(
    const std::vector<BakedFrame>& frames, const std::vector<loader::Bone>& bones,
    const loader::Animation* animation) {
    std::vector<std::map<std::string, Transform>> result;
    result.reserve(frames.size());
    for (const auto& frame : frames) {
        result.push_back(
            transformsFromPoses(BonePoseCalculator::calculate(bones, animation, frame.time)));
    }
    return result;
}

std::map<std::string, LoopSeamReport::Transform> LoopSeamReport::transformsFromPoses(
    const std::map<std::string, BonePoseCalculator::Pose>& poses) {
    std::map<std::string, Transform> result;
    for (const auto& [name, pose] : poses) {
        Transform t;
        t.position = pose.world_position;
        t.rotation = pose.world_rotation;
        result.emplace(name, t);
    }
    return result;
}

std::vector<std::map<std::string, LoopSeamReport::Transform>> LoopSeamReport::relativeTransforms(
    const std::vector<std::map<std::string, Transform>>& world,
    const std::map<std::string, std::string>& anchors) {
    std::vector<std::map<std::string, Transform>> result;
    result.reserve(world.size());
    for (const auto& sample : world) {
        std::map<std::string, Transform> relative;
        for (const auto& [name, anchor_name] : anchors) {
            if (name == anchor_name) {
                continue;
            }
            auto value_it = sample.find(name);
            auto anchor_it = sample.find(anchor_name);
            if (value_it == sample.end() || anchor_it == sample.end()) {
                continue;
            }
            const auto inverse = RotationUtil::quaternionInverse(anchor_it->second.rotation);
            Transform t;
            t.position = RotationUtil::rotateVector(
                inverse, subtract(value_it->second.position, anchor_it->second.position));
            t.rotation =
                RotationUtil::quaternionMultiply(inverse, value_it->second.rotation);
            relative.emplace(name, t);
        }
        result.push_back(std::move(relative));
    }
    return result;
}

std::set<std::string> LoopSeamReport::measuredSubtree(const std::vector<loader::Bone>& bones,
                                                      const std::set<std::string>& physics_bones) {
    std::set<std::string> roots;
    const auto by_name = indexBones(bones);
    for (const auto& name : physics_bones) {
        auto it = by_name.find(name);
        if (it != by_name.end() &&
            (!it->second.has_parent || it->second.parent.empty() ||
             !physics_bones.contains(it->second.parent))) {
            roots.insert(name);
        }
    }
    std::set<std::string> measured;
    bool changed = false;
    do {
        changed = false;
        for (const auto& bone : bones) {
            if (bone.name.empty()) {
                continue;
            }
            if (roots.contains(bone.name) ||
                (bone.has_parent && !bone.parent.empty() && measured.contains(bone.parent))) {
                changed |= measured.insert(bone.name).second;
            }
        }
    } while (changed);
    return measured;
}

std::map<std::string, std::string> LoopSeamReport::findAnchors(
    const std::set<std::string>& names, const std::map<std::string, loader::Bone>& bones,
    const std::set<std::string>& physics_bones, const std::set<std::string>& fixed_physics_bones) {
    std::map<std::string, std::string> result;
    for (const auto& name : names) {
        auto current_it = bones.find(name);
        while (current_it != bones.end()) {
            if (physics_bones.contains(current_it->second.name) &&
                fixed_physics_bones.contains(current_it->second.name)) {
                result[name] = current_it->second.name;
                break;
            }
            if (!current_it->second.has_parent || current_it->second.parent.empty()) {
                break;
            }
            current_it = bones.find(current_it->second.parent);
        }
    }
    return result;
}

std::set<std::string> LoopSeamReport::relativeNames(
    const std::map<std::string, std::string>& anchors) {
    std::set<std::string> names;
    for (const auto& [name, anchor] : anchors) {
        if (name != anchor) {
            names.insert(name);
        }
    }
    return names;
}

std::set<std::string> LoopSeamReport::driverNames(
    const std::map<std::string, loader::Bone>& bones,
    const std::set<std::string>& physics_bones,
    const std::set<std::string>& fixed_physics_bones) {
    std::set<std::string> result = fixed_physics_bones;
    for (const auto& physics_name : physics_bones) {
        auto current = bones.find(physics_name);
        std::set<std::string> visited;
        while (current != bones.end() && current->second.has_parent &&
               !current->second.parent.empty() &&
               visited.insert(current->second.name).second) {
            const auto parent = bones.find(current->second.parent);
            if (parent == bones.end()) {
                break;
            }
            if (!physics_bones.contains(parent->second.name)) {
                result.insert(parent->second.name);
            }
            current = parent;
        }
    }
    return result;
}

std::vector<LoopSeamReport::AnchorCoverage> LoopSeamReport::buildAnchorCoverage(
    const std::map<std::string, loader::Bone>& bones,
    const std::set<std::string>& physics_bones,
    const std::set<std::string>& fixed_physics_bones,
    const std::map<std::string, std::string>& anchors) {
    std::map<std::string, AnchorCoverage> by_root;
    for (const auto& name : physics_bones) {
        if (fixed_physics_bones.contains(name)) {
            continue;
        }
        std::string root = name;
        auto current = bones.find(name);
        std::set<std::string> visited;
        while (current != bones.end() && current->second.has_parent &&
               !current->second.parent.empty() &&
               visited.insert(current->second.name).second) {
            const auto parent = bones.find(current->second.parent);
            if (parent == bones.end()) {
                break;
            }
            if (physics_bones.contains(parent->second.name)) {
                root = parent->second.name;
            }
            current = parent;
        }
        auto& coverage = by_root[root];
        coverage.chain_root = root;
        ++coverage.expected_bone_count;
        const auto anchor = anchors.find(name);
        if (anchor != anchors.end() && !anchor->second.empty()) {
            ++coverage.measured_bone_count;
            if (coverage.fixed_anchor.empty()) {
                coverage.fixed_anchor = anchor->second;
            }
        }
    }
    std::vector<AnchorCoverage> result;
    result.reserve(by_root.size());
    for (auto& [root, coverage] : by_root) {
        (void)root;
        coverage.complete = coverage.expected_bone_count > 0 &&
                            coverage.measured_bone_count == coverage.expected_bone_count &&
                            !coverage.fixed_anchor.empty();
        result.push_back(std::move(coverage));
    }
    return result;
}

void LoopSeamReport::validateTransformSamples(
    const std::vector<std::map<std::string, Transform>>& samples,
    const std::set<std::string>& names, const std::string& metric_space,
    Validation& validation) {
    for (const auto& sample : samples) {
        for (const auto& name : names) {
            const auto found = sample.find(name);
            if (found == sample.end()) {
                validation.valid = false;
                ++validation.missing_bone_count;
                if (validation.first_missing_bone.empty()) {
                    validation.first_missing_bone = name;
                }
                if (validation.first_invalid_field.empty()) {
                    validation.first_invalid_field = "transform";
                    validation.affected_metric_space = metric_space;
                }
                continue;
            }
            const bool finite_position = std::all_of(
                found->second.position.begin(), found->second.position.end(),
                [](double value) { return std::isfinite(value); });
            const bool finite_rotation = std::all_of(
                found->second.rotation.begin(), found->second.rotation.end(),
                [](double value) { return std::isfinite(value); });
            if (!finite_position || !finite_rotation) {
                validation.valid = false;
                ++validation.non_finite_value_count;
                if (validation.first_invalid_field.empty()) {
                    validation.first_invalid_field =
                        finite_position ? "rotation" : "position";
                    validation.affected_metric_space = metric_space;
                }
            }
        }
    }
}

std::map<std::string, loader::Bone> LoopSeamReport::indexBones(
    const std::vector<loader::Bone>& bones) {
    std::map<std::string, loader::Bone> result;
    for (const auto& bone : bones) {
        if (!bone.name.empty()) {
            result[bone.name] = bone;
        }
    }
    return result;
}

std::array<double, 3> LoopSeamReport::velocity(
    const std::vector<std::map<std::string, Transform>>& samples,
    const std::vector<BakedFrame>& frames, const std::string& name, int from, int to) {
    auto a_it = samples[static_cast<std::size_t>(from)].find(name);
    auto b_it = samples[static_cast<std::size_t>(to)].find(name);
    const double dt = timeStep(frames, from, to);
    if (a_it == samples[static_cast<std::size_t>(from)].end() ||
        b_it == samples[static_cast<std::size_t>(to)].end() || !(dt > 0.0)) {
        return {0, 0, 0};
    }
    return scale(subtract(b_it->second.position, a_it->second.position), 1.0 / dt);
}

std::array<double, 3> LoopSeamReport::angularVelocity(
    const std::vector<std::map<std::string, Transform>>& samples,
    const std::vector<BakedFrame>& frames, const std::string& name, int from, int to) {
    auto a_it = samples[static_cast<std::size_t>(from)].find(name);
    auto b_it = samples[static_cast<std::size_t>(to)].find(name);
    const double dt = timeStep(frames, from, to);
    if (a_it == samples[static_cast<std::size_t>(from)].end() ||
        b_it == samples[static_cast<std::size_t>(to)].end() || !(dt > 0.0)) {
        return {0, 0, 0};
    }
    return scale(RotationUtil::rotationVectorFromQuaternion(RotationUtil::quaternionMultiply(
                     RotationUtil::quaternionInverse(a_it->second.rotation),
                     b_it->second.rotation)),
                 1.0 / dt);
}

double LoopSeamReport::timeStep(const std::vector<BakedFrame>& frames, int from, int to) {
    return frames[static_cast<std::size_t>(to)].time - frames[static_cast<std::size_t>(from)].time;
}

std::array<double, 3> LoopSeamReport::acceleration(
    const std::vector<std::map<std::string, Transform>>& samples,
    const std::vector<BakedFrame>& frames, const std::string& name, int a, int b, int c,
    bool angular) {
    const auto first =
        angular ? angularVelocity(samples, frames, name, a, b) : velocity(samples, frames, name, a, b);
    const auto second =
        angular ? angularVelocity(samples, frames, name, b, c) : velocity(samples, frames, name, b, c);
    const double span = 0.5 * timeStep(frames, a, c);
    return span > 0.0 ? scale(subtract(second, first), 1.0 / span) : std::array<double, 3>{0, 0, 0};
}

double LoopSeamReport::rotationDistance(const std::array<double, 4>& a,
                                        const std::array<double, 4>& b) {
    return length(RotationUtil::rotationVectorFromQuaternion(
        RotationUtil::quaternionMultiply(RotationUtil::quaternionInverse(a), b)));
}

std::array<double, 3> LoopSeamReport::values(const std::array<double, 3>& source, bool quantize) {
    std::array<double, 3> result = source;
    if (quantize) {
        for (double& v : result) {
            v = std::round(v * 10000.0) / 10000.0;
        }
    }
    return result;
}

double LoopSeamReport::distance(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return length(subtract(a, b));
}

double LoopSeamReport::length(const std::array<double, 3>& value) {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

std::array<double, 3> LoopSeamReport::subtract(const std::array<double, 3>& a,
                                               const std::array<double, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::array<double, 3> LoopSeamReport::scale(const std::array<double, 3>& value, double factor) {
    return {value[0] * factor, value[1] * factor, value[2] * factor};
}

}
