#include "xpbd/baker/loop_seam_corrector.hpp"

#include "xpbd/baker/bedrock_transform_resolver.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace xpbd::baker {
namespace {

std::vector<BakedFrame> deepCopy(const std::vector<BakedFrame>& source) {
    std::vector<BakedFrame> result;
    result.reserve(source.size());
    for (const auto& frame : source) {
        BakedFrame copy = frame;
        copy.rebuildIndex();
        result.push_back(std::move(copy));
    }
    return result;
}

struct CompiledFrameBoneSlot {
    std::string name;
    std::optional<std::size_t> stable_index;
};

CompiledFrameBoneSlot compileFrameBoneSlot(
    const std::string& name, const StableFrameLayout* stable_layout) {
    CompiledFrameBoneSlot slot;
    slot.name = name;
    if (stable_layout != nullptr) {
        const auto index = stable_layout->index_by_name.find(name);
        if (index != stable_layout->index_by_name.end()) {
            slot.stable_index = index->second;
        }
    }
    return slot;
}

const BoneState* frameState(const BakedFrame& frame,
                            const CompiledFrameBoneSlot& slot) {
    if (slot.stable_index.has_value()) {
        const std::size_t index = *slot.stable_index;
        if (index < frame.bone_states.size() &&
            frame.bone_states[index].bone_name == slot.name) {
            return &frame.bone_states[index];
        }
        return nullptr;
    }
    return frame.getBoneState(slot.name);
}

BoneState* frameState(BakedFrame& frame,
                      const CompiledFrameBoneSlot& slot) {
    if (slot.stable_index.has_value()) {
        const std::size_t index = *slot.stable_index;
        if (index < frame.bone_states.size() &&
            frame.bone_states[index].bone_name == slot.name) {
            return &frame.bone_states[index];
        }
        return nullptr;
    }
    return frame.getBoneState(slot.name);
}

double cube(double v) { return v * v * v; }
double fourth(double v) { return cube(v) * v; }
double fifth(double v) { return fourth(v) * v; }

double evaluate(const double coefficients[3], double u) {
    const double u2 = u * u;
    const double u3 = u2 * u;
    return coefficients[0] * u3 + coefficients[1] * u3 * u + coefficients[2] * u3 * u2;
}

double normalizedTime(const std::vector<BakedFrame>& frames, int start, int index,
                      double duration) {
    return std::max(0.0, std::min(1.0, (frames[static_cast<std::size_t>(index)].time -
                                        frames[static_cast<std::size_t>(start)].time) /
                                           duration));
}

std::array<double, 3> solve3(double matrix[3][4]) {
    // 退化回退：消元会改写 matrix[0][3]，必须在开始前捕获 u=1 处的端点值。
    // {0,0,endpoint} 使 evaluate(coeff,1)=endpoint，保住 C0 接缝并放弃高阶约束。
    const double endpoint_value = matrix[0][3];
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(matrix[pivot][column]) < 1e-12) {
            return {0.0, 0.0, endpoint_value};
        }
        if (pivot != column) {
            for (int j = 0; j < 4; ++j) {
                std::swap(matrix[column][j], matrix[pivot][j]);
            }
        }
        const double scale = matrix[column][column];
        for (int j = column; j < 4; ++j) {
            matrix[column][j] /= scale;
        }
        for (int row = 0; row < 3; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = matrix[row][column];
            for (int j = column; j < 4; ++j) {
                matrix[row][j] -= factor * matrix[column][j];
            }
        }
    }
    return {matrix[0][3], matrix[1][3], matrix[2][3]};
}

std::array<double, 3> sampledValueCoefficients(double atLast, double atPenultimate,
                                               double atPrevious, double penultimateU,
                                               double previousU) {
    double matrix[3][4] = {
        {1, 1, 1, atLast},
        {cube(penultimateU), fourth(penultimateU), fifth(penultimateU), atPenultimate},
        {cube(previousU), fourth(previousU), fifth(previousU), atPrevious},
    };
    return solve3(matrix);
}

std::array<double, 3> sampledCoefficients(double dx, double dv, double da, double lastStep,
                                          double previousStep, double penultimateU,
                                          double previousU) {
    const double accelerationSpan = 0.5 * (lastStep + previousStep);
    const double atLast = dx;
    const double atPenultimate = atLast - dv * lastStep;
    const double atPrevious = atPenultimate - previousStep * (dv - da * accelerationSpan);
    return sampledValueCoefficients(atLast, atPenultimate, atPrevious, penultimateU, previousU);
}

std::array<double, 3> scale3(const std::array<double, 3>& v, double f) {
    return {v[0] * f, v[1] * f, v[2] * f};
}
std::array<double, 3> sub3(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

RotationUtil::Quat totalQuaternion(const loader::Bone& bone,
                                   const std::array<double, 3>& animationRotation) {
    return RotationUtil::quaternionFromBedrockEuler(
        bone.rotation[0] + animationRotation[0], bone.rotation[1] + animationRotation[1],
        bone.rotation[2] + animationRotation[2]);
}

std::array<double, 3> correctionVector(const RotationUtil::Quat& from,
                                       const RotationUtil::Quat& to) {
    return RotationUtil::rotationVectorFromQuaternion(
        RotationUtil::quaternionMultiply(RotationUtil::quaternionInverse(from), to));
}

std::array<double, 3> angularVelocityQ(const RotationUtil::Quat& from,
                                       const RotationUtil::Quat& to, double dt) {
    if (!(dt > 0)) {
        return {0, 0, 0};
    }
    auto value = correctionVector(from, to);
    return {value[0] / dt, value[1] / dt, value[2] / dt};
}

std::array<double, 3> linearVelocity(const std::vector<BakedFrame>& frames,
                                     const CompiledFrameBoneSlot& slot,
                                     int from, int to) {
    const double dt = frames[static_cast<std::size_t>(to)].time -
                      frames[static_cast<std::size_t>(from)].time;
    if (!(dt > 0)) {
        return {0, 0, 0};
    }
    const auto* a =
        frameState(frames[static_cast<std::size_t>(from)], slot);
    const auto* b =
        frameState(frames[static_cast<std::size_t>(to)], slot);
    if (a == nullptr || b == nullptr) {
        return {0, 0, 0};
    }
    return {(b->position[0] - a->position[0]) / dt, (b->position[1] - a->position[1]) / dt,
            (b->position[2] - a->position[2]) / dt};
}

std::array<double, 3> linearAcceleration(const std::vector<BakedFrame>& frames,
                                         const CompiledFrameBoneSlot& slot,
                                         int a, int b, int c) {
    const auto first = linearVelocity(frames, slot, a, b);
    const auto second = linearVelocity(frames, slot, b, c);
    const double span = 0.5 * (frames[static_cast<std::size_t>(c)].time -
                               frames[static_cast<std::size_t>(a)].time);
    if (!(span > 0)) {
        return {0, 0, 0};
    }
    return {(second[0] - first[0]) / span, (second[1] - first[1]) / span,
            (second[2] - first[2]) / span};
}

struct EndpointCanonicalizationStats {
    int canonicalized_bone_count = 0;
    int preserved_driver_bone_count = 0;
    int driver_endpoint_conflict_count = 0;
};

bool endpointDiffers(const BoneState& first, const BoneState& last) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (first.position[axis] != last.position[axis] ||
            first.rotation[axis] != last.rotation[axis]) {
            return true;
        }
    }
    return false;
}

EndpointCanonicalizationStats canonicalizeEndpoint(
    std::vector<BakedFrame>& frames, const std::set<std::string>& corrected_bones,
    const std::set<std::string>& preserved_driver_bones,
    const StableFrameLayout* stable_layout) {
    EndpointCanonicalizationStats stats;
    if (frames.size() < 2) {
        return stats;
    }
    auto& first = frames.front();
    auto& last = frames.back();
    for (std::size_t state_index = 0;
         state_index < last.bone_states.size(); ++state_index) {
        auto& state = last.bone_states[state_index];
        const auto* target =
            stable_layout != nullptr
                ? &first.bone_states[state_index]
                : first.getBoneState(state.bone_name);
        if (target == nullptr) {
            continue;
        }
        if (!corrected_bones.contains(state.bone_name)) {
            if (preserved_driver_bones.contains(state.bone_name)) {
                ++stats.preserved_driver_bone_count;
                if (endpointDiffers(*target, state)) {
                    ++stats.driver_endpoint_conflict_count;
                }
            }
            continue;
        }
        state.position = target->position;
        state.rotation = target->rotation;
        ++stats.canonicalized_bone_count;
    }
    return stats;
}

void correctPosition(std::vector<BakedFrame>& frames,
                     const CompiledFrameBoneSlot& slot, int start,
                     double duration, bool matchAcceleration) {
    const int last = static_cast<int>(frames.size()) - 1;
    const auto* firstState = frameState(frames[0], slot);
    const auto* lastState =
        frameState(frames[static_cast<std::size_t>(last)], slot);
    if (firstState == nullptr || lastState == nullptr) {
        return;
    }

    const auto firstVelocity = linearVelocity(frames, slot, 0, 1);
    const auto lastVelocity = linearVelocity(frames, slot, last - 1, last);
    const auto firstAcceleration =
        matchAcceleration ? linearAcceleration(frames, slot, 0, 1, 2)
                          : std::array<double, 3>{0, 0, 0};
    const auto lastAcceleration =
        matchAcceleration ? linearAcceleration(frames, slot, last - 2, last - 1, last)
                          : std::array<double, 3>{0, 0, 0};

    const double lastStep = frames[static_cast<std::size_t>(last)].time -
                            frames[static_cast<std::size_t>(last - 1)].time;
    const double previousStep =
        last >= 2 ? frames[static_cast<std::size_t>(last - 1)].time -
                        frames[static_cast<std::size_t>(last - 2)].time
                  : lastStep;
    const double penultimateU =
        (frames[static_cast<std::size_t>(last - 1)].time - frames[static_cast<std::size_t>(start)].time) /
        duration;
    const double previousU =
        last >= 2
            ? (frames[static_cast<std::size_t>(last - 2)].time -
               frames[static_cast<std::size_t>(start)].time) /
                  duration
            : 0.0;

    std::array<double, 3> coefficients[3];
    for (int axis = 0; axis < 3; ++axis) {
        const double dx = firstState->position[static_cast<std::size_t>(axis)] -
                          lastState->position[static_cast<std::size_t>(axis)];
        const double dv = firstVelocity[static_cast<std::size_t>(axis)] -
                          lastVelocity[static_cast<std::size_t>(axis)];
        const double da = firstAcceleration[static_cast<std::size_t>(axis)] -
                          lastAcceleration[static_cast<std::size_t>(axis)];
        coefficients[axis] =
            sampledCoefficients(dx, dv, da, lastStep, previousStep, penultimateU, previousU);
    }
    for (int index = start; index <= last; ++index) {
        auto* state =
            frameState(frames[static_cast<std::size_t>(index)], slot);
        if (state == nullptr) {
            continue;
        }
        const double u = normalizedTime(frames, start, index, duration);
        for (int axis = 0; axis < 3; ++axis) {
            state->position[static_cast<std::size_t>(axis)] +=
                evaluate(coefficients[axis].data(), u);
        }
    }
}

void correctRotation(std::vector<BakedFrame>& frames, const loader::Bone& bone,
                     const CompiledFrameBoneSlot& slot, int start,
                     double duration, bool matchAcceleration) {
    const int last = static_cast<int>(frames.size()) - 1;
    std::vector<RotationUtil::Quat> quaternions(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto* state = frameState(frames[i], slot);
        if (state == nullptr) {
            return;
        }
        quaternions[i] = totalQuaternion(bone, state->rotation);
        if (i > 0) {
            const double dot = quaternions[i - 1][0] * quaternions[i][0] +
                               quaternions[i - 1][1] * quaternions[i][1] +
                               quaternions[i - 1][2] * quaternions[i][2] +
                               quaternions[i - 1][3] * quaternions[i][3];
            if (dot < 0) {
                for (int k = 0; k < 4; ++k) {
                    quaternions[i][static_cast<std::size_t>(k)] =
                        -quaternions[i][static_cast<std::size_t>(k)];
                }
            }
        }
    }

    const auto firstVelocity = angularVelocityQ(
        quaternions[0], quaternions[1], frames[1].time - frames[0].time);
    std::array<double, 3> firstAcceleration{0, 0, 0};
    if (matchAcceleration && frames.size() >= 3) {
        const auto nextVelocity = angularVelocityQ(quaternions[1], quaternions[2],
                                                   frames[2].time - frames[1].time);
        const double firstSpan = 0.5 * (frames[2].time - frames[0].time);
        for (int axis = 0; axis < 3; ++axis) {
            firstAcceleration[static_cast<std::size_t>(axis)] =
                (nextVelocity[static_cast<std::size_t>(axis)] -
                 firstVelocity[static_cast<std::size_t>(axis)]) /
                firstSpan;
        }
    }

    const double lastStep = frames[static_cast<std::size_t>(last)].time -
                            frames[static_cast<std::size_t>(last - 1)].time;
    const double previousStep = frames[static_cast<std::size_t>(last - 1)].time -
                                frames[static_cast<std::size_t>(last - 2)].time;
    const double accelerationSpan = 0.5 * (lastStep + previousStep);
    const auto desiredLast = quaternions[0];
    const auto desiredPenultimate = RotationUtil::quaternionMultiply(
        desiredLast,
        RotationUtil::quaternionFromRotationVector(scale3(firstVelocity, -lastStep)));
    std::array<double, 3> desiredPreviousVelocity{};
    for (int axis = 0; axis < 3; ++axis) {
        desiredPreviousVelocity[static_cast<std::size_t>(axis)] =
            firstVelocity[static_cast<std::size_t>(axis)] -
            firstAcceleration[static_cast<std::size_t>(axis)] * accelerationSpan;
    }
    const auto desiredPrevious = RotationUtil::quaternionMultiply(
        desiredPenultimate,
        RotationUtil::quaternionFromRotationVector(scale3(desiredPreviousVelocity, -previousStep)));

    const auto atLast = correctionVector(quaternions[static_cast<std::size_t>(last)], desiredLast);
    const auto atPenultimate =
        correctionVector(quaternions[static_cast<std::size_t>(last - 1)], desiredPenultimate);
    const auto atPrevious =
        correctionVector(quaternions[static_cast<std::size_t>(last - 2)], desiredPrevious);
    const double penultimateU =
        (frames[static_cast<std::size_t>(last - 1)].time -
         frames[static_cast<std::size_t>(start)].time) /
        duration;
    const double previousU =
        (frames[static_cast<std::size_t>(last - 2)].time -
         frames[static_cast<std::size_t>(start)].time) /
        duration;

    std::array<double, 3> coefficients[3];
    for (int axis = 0; axis < 3; ++axis) {
        coefficients[axis] = sampledValueCoefficients(
            atLast[static_cast<std::size_t>(axis)], atPenultimate[static_cast<std::size_t>(axis)],
            atPrevious[static_cast<std::size_t>(axis)], penultimateU, previousU);
    }

    std::optional<std::array<double, 3>> previousTotalEuler;
    for (int index = start; index <= last; ++index) {
        const double u = normalizedTime(frames, start, index, duration);
        std::array<double, 3> correction{};
        for (int axis = 0; axis < 3; ++axis) {
            correction[static_cast<std::size_t>(axis)] =
                evaluate(coefficients[axis].data(), u);
        }
        const auto corrected = RotationUtil::quaternionMultiply(
            quaternions[static_cast<std::size_t>(index)],
            RotationUtil::quaternionFromRotationVector(correction));
        auto totalEuler = RotationUtil::bedrockEulerFromQuaternion(corrected);
        auto* state =
            frameState(frames[static_cast<std::size_t>(index)], slot);
        if (state == nullptr) {
            continue;
        }
        const std::array<double, 3> originalTotal{bone.rotation[0] + state->rotation[0],
                                                  bone.rotation[1] + state->rotation[1],
                                                  bone.rotation[2] + state->rotation[2]};
        const auto* firstState = frameState(frames[0], slot);
        const std::array<double, 3> targetTotal{
            bone.rotation[0] + firstState->rotation[0],
            bone.rotation[1] + firstState->rotation[1],
            bone.rotation[2] + firstState->rotation[2]};
        const double guideWeight = u * u * (3 - 2 * u);
        std::array<double, 3> guide{};
        for (int axis = 0; axis < 3; ++axis) {
            guide[static_cast<std::size_t>(axis)] =
                originalTotal[static_cast<std::size_t>(axis)] * (1 - guideWeight) +
                targetTotal[static_cast<std::size_t>(axis)] * guideWeight;
        }
        totalEuler = RotationUtil::unwrapEuler(
            previousTotalEuler ? *previousTotalEuler : guide, totalEuler);
        for (int axis = 0; axis < 3; ++axis) {
            state->rotation[static_cast<std::size_t>(axis)] =
                totalEuler[static_cast<std::size_t>(axis)] - bone.rotation[axis];
        }
        previousTotalEuler = totalEuler;
    }
}

struct CorrectionWindow {
    int start_index = 0;
    double duration_seconds = 0.0;
    double ratio = 0.0;
};

CorrectionWindow selectCorrectionWindow(const std::vector<BakedFrame>& frames,
                                        double requested_ratio) {
    if (!std::isfinite(requested_ratio) || !(requested_ratio > 0.0) ||
        requested_ratio > 1.0) {
        throw std::invalid_argument("window ratio must be finite and in (0, 1]");
    }
    const int last = static_cast<int>(frames.size()) - 1;
    const double first_time = frames.front().time;
    const double last_time = frames.back().time;
    const double total_duration = last_time - first_time;
    if (!(total_duration > 0.0) || !std::isfinite(total_duration)) {
        throw std::invalid_argument("correction window must have positive duration");
    }

    const double requested_start_time =
        last_time - requested_ratio * total_duration;
    int time_start = 0;
    while (time_start + 1 < last &&
           frames[static_cast<std::size_t>(time_start + 1)].time <=
               requested_start_time) {
        ++time_start;
    }
    const int minimum_intervals = std::min(8, last);
    const int start = std::min(time_start, last - minimum_intervals);
    const double duration =
        last_time - frames[static_cast<std::size_t>(start)].time;
    if (!(duration > 0.0) || !std::isfinite(duration)) {
        throw std::invalid_argument("correction window must have positive duration");
    }
    return {start, duration, duration / total_duration};
}

}

LoopSeamCorrector::Result LoopSeamCorrector::correctCopy(
    const std::vector<BakedFrame>& source,
    const std::map<std::string, loader::Bone>& bones_by_name,
    const std::set<std::string>& corrected_bones,
    double window_ratio, bool match_acceleration) {
    // 3 帧窗口下 previousU==0，三行约束系统奇异，solve3 退化为仅 C0 端点修正；
    // 完整的 C1/C2 需要窗口内至少 3 个间隔（≥4 帧）。
    const std::size_t minimum_samples = match_acceleration ? 4 : 3;
    if (source.size() < minimum_samples) {
        throw std::invalid_argument(match_acceleration
                                        ? "at least four distinct loop frames are required for C2"
                                        : "at least three distinct loop frames are required for C1");
    }
    auto frames = deepCopy(source);
    const auto stable_layout = StableFrameLayout::tryCreate(frames);
    const StableFrameLayout* stable_layout_ptr =
        stable_layout.has_value() ? &*stable_layout : nullptr;
    std::vector<CompiledFrameBoneSlot> corrected_slots;
    corrected_slots.reserve(corrected_bones.size());
    for (const auto& boneName : corrected_bones) {
        if (!bones_by_name.contains(boneName)) {
            throw std::invalid_argument(
                "loop correction is missing required model bone: " + boneName);
        }
        corrected_slots.push_back(
            compileFrameBoneSlot(boneName, stable_layout_ptr));
    }
    for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        if (!std::isfinite(frames[frameIndex].time) ||
            (frameIndex > 0 &&
             !(frames[frameIndex].time > frames[frameIndex - 1].time))) {
            throw std::invalid_argument(
                "loop correction requires finite, strictly increasing sample times");
        }
        for (const auto& slot : corrected_slots) {
            const auto* state = frameState(frames[frameIndex], slot);
            if (state == nullptr) {
                throw std::invalid_argument(
                    "loop correction is missing required physics bone: " +
                    slot.name);
            }
            const auto finite = [](double value) { return std::isfinite(value); };
            if (!std::all_of(state->position.begin(), state->position.end(), finite) ||
                !std::all_of(state->rotation.begin(), state->rotation.end(), finite)) {
                throw std::invalid_argument(
                    "loop correction has non-finite channel data for physics bone: " +
                    slot.name);
            }
        }
    }
    const auto window = selectCorrectionWindow(frames, window_ratio);
    const int start = window.start_index;
    const double duration = window.duration_seconds;

    for (const auto& slot : corrected_slots) {
        const auto it = bones_by_name.find(slot.name);
        correctPosition(frames, slot, start, duration, match_acceleration);
        correctRotation(frames, it->second, slot, start, duration,
                        match_acceleration);
    }
    const auto endpoint_stats =
        canonicalizeEndpoint(frames, corrected_bones, {}, stable_layout_ptr);
    return Result{std::move(frames), start, duration, window.ratio,
                  endpoint_stats.canonicalized_bone_count,
                  endpoint_stats.preserved_driver_bone_count,
                  endpoint_stats.driver_endpoint_conflict_count};
}

namespace {

struct DesiredTransform {
    std::array<double, 3> position{};
    RotationUtil::Quat rotation{0, 0, 0, 1};
};

struct ChannelTarget {
    std::array<double, 3> position{};
    std::array<double, 3> rotation{};
};

std::array<double, 3> add3(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

std::map<std::string, std::array<double, 3>> overrides(const BakedFrame& frame, bool position) {
    std::map<std::string, std::array<double, 3>> result;
    for (const auto& state : frame.bone_states) {
        result[state.bone_name] = position ? state.position : state.rotation;
    }
    return result;
}

int hierarchyDepth(const std::string& name, const std::map<std::string, loader::Bone>& bones) {
    int depth = 0;
    auto it = bones.find(name);
    std::set<std::string> visited;
    while (it != bones.end() && it->second.has_parent && visited.insert(it->second.name).second) {
        depth++;
        it = bones.find(it->second.parent);
    }
    return depth;
}

std::string fixedAnchor(const std::string& name, const std::map<std::string, loader::Bone>& bones,
                        const std::set<std::string>& physics_bones,
                        const std::set<std::string>& fixed_bones) {
    auto it = bones.find(name);
    while (it != bones.end()) {
        if (physics_bones.contains(it->second.name) && fixed_bones.contains(it->second.name)) {
            return it->second.name;
        }
        if (!it->second.has_parent) {
            break;
        }
        it = bones.find(it->second.parent);
    }
    return {};
}

DesiredTransform transformOf(const std::map<std::string, BonePoseCalculator::Pose>& poses,
                             const std::string& name, const std::string& anchor) {
    const auto& pose = poses.at(name);
    if (anchor.empty()) {
        return DesiredTransform{pose.world_position, pose.world_rotation};
    }
    const auto& root = poses.at(anchor);
    const auto inverse = RotationUtil::quaternionInverse(root.world_rotation);
    return DesiredTransform{
        RotationUtil::rotateVector(inverse, sub3(pose.world_position, root.world_position)),
        RotationUtil::quaternionMultiply(inverse, pose.world_rotation)};
}

DesiredTransform composeAnchor(const BonePoseCalculator::Pose& anchor,
                               const DesiredTransform& relative) {
    const auto offset = RotationUtil::rotateVector(anchor.world_rotation, relative.position);
    return DesiredTransform{add3(anchor.world_position, offset),
                            RotationUtil::quaternionMultiply(anchor.world_rotation,
                                                             relative.rotation)};
}

ChannelTarget solveLocalChannel(const loader::Bone& bone, const BonePoseCalculator::Pose* parent,
                                const DesiredTransform& desired,
                                const std::array<double, 3>& original_rotation) {
    const RotationUtil::Quat parent_q =
        parent == nullptr ? RotationUtil::Quat{0, 0, 0, 1} : parent->world_rotation;
    const std::array<double, 3> parent_translation =
        parent == nullptr ? std::array<double, 3>{0, 0, 0} : parent->world_translation;
    const auto local_q =
        RotationUtil::quaternionMultiply(RotationUtil::quaternionInverse(parent_q), desired.rotation);
    auto total_euler = RotationUtil::unwrapEuler(
        {bone.rotation[0] + original_rotation[0], bone.rotation[1] + original_rotation[1],
         bone.rotation[2] + original_rotation[2]},
        RotationUtil::bedrockEulerFromQuaternion(local_q));
    const auto local_pivot = RotationUtil::rotateVector(
        RotationUtil::quaternionInverse(parent_q), sub3(desired.position, parent_translation));
    const auto mapped_pivot = BedrockTransformResolver::convertBedrockVector(bone.pivot);
    const auto mapped_animation = sub3(local_pivot, mapped_pivot);
    const auto position = BedrockTransformResolver::convertBedrockVector(mapped_animation);
    return ChannelTarget{position,
                         {total_euler[0] - bone.rotation[0], total_euler[1] - bone.rotation[1],
                          total_euler[2] - bone.rotation[2]}};
}

std::array<double, 3> angularVelocityQuat(const RotationUtil::Quat& from,
                                          const RotationUtil::Quat& to, double dt) {
    if (!(dt > 0)) {
        return {0, 0, 0};
    }
    auto value = correctionVector(from, to);
    return {value[0] / dt, value[1] / dt, value[2] / dt};
}

}

LoopSeamCorrector::Result LoopSeamCorrector::correctHierarchyCopy(
    const std::vector<BakedFrame>& source, const std::vector<loader::Bone>& bones,
    const loader::Animation* animation, const std::set<std::string>& physics_bones,
    const std::set<std::string>& fixed_physics_bones, BoneMapper::LoopSeamStrategy strategy,
    double window_ratio, bool match_acceleration) {
    const std::size_t minimum_samples = match_acceleration ? 4 : 3;
    if (source.size() < minimum_samples) {
        throw std::invalid_argument(match_acceleration
                                        ? "at least four distinct loop frames are required for C2"
                                        : "at least three distinct loop frames are required for C1");
    }
    auto frames = deepCopy(source);
    const int last = static_cast<int>(frames.size()) - 1;

    std::map<std::string, loader::Bone> by_name;
    for (const auto& bone : bones) {
        if (!bone.name.empty()) {
            by_name[bone.name] = bone;
        }
    }
    std::vector<std::string> ordered;
    for (const auto& name : physics_bones) {
        if (strategy == BoneMapper::LoopSeamStrategy::VisualSubtree ||
            !fixed_physics_bones.contains(name)) {
            ordered.push_back(name);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [&](const std::string& a, const std::string& b) {
        return hierarchyDepth(a, by_name) < hierarchyDepth(b, by_name);
    });
    const auto stable_layout = StableFrameLayout::tryCreate(frames);
    const StableFrameLayout* stable_layout_ptr =
        stable_layout.has_value() ? &*stable_layout : nullptr;
    std::vector<CompiledFrameBoneSlot> ordered_slots;
    ordered_slots.reserve(ordered.size());
    for (const auto& name : ordered) {
        ordered_slots.push_back(
            compileFrameBoneSlot(name, stable_layout_ptr));
    }
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (!std::isfinite(frames[index].time) ||
            (index > 0 && !(frames[index].time > frames[index - 1].time))) {
            throw std::invalid_argument(
                "loop correction requires finite, strictly increasing sample times");
        }
        for (const auto& slot : ordered_slots) {
            if (!by_name.contains(slot.name)) {
                throw std::invalid_argument(
                    "loop correction is missing required model bone: " +
                    slot.name);
            }
            const auto* state = frameState(frames[index], slot);
            if (state == nullptr) {
                throw std::invalid_argument(
                    "loop correction is missing required physics bone: " +
                    slot.name);
            }
            const auto finite = [](double value) { return std::isfinite(value); };
            if (!std::all_of(state->position.begin(), state->position.end(), finite) ||
                !std::all_of(state->rotation.begin(), state->rotation.end(), finite)) {
                throw std::invalid_argument(
                    "loop correction has non-finite channel data for physics bone: " +
                    slot.name);
            }
        }
    }

    const auto window = selectCorrectionWindow(frames, window_ratio);
    const int start = window.start_index;
    const double duration = window.duration_seconds;


    std::vector<std::map<std::string, BonePoseCalculator::Pose>> poses;
    poses.reserve(frames.size());
    for (const auto& frame : frames) {
        auto pos_ov = overrides(frame, true);
        auto rot_ov = overrides(frame, false);
        poses.push_back(BonePoseCalculator::calculate(bones, animation, frame.time, &pos_ov, &rot_ov));
    }

    const double last_step = frames[static_cast<std::size_t>(last)].time -
                             frames[static_cast<std::size_t>(last - 1)].time;
    const double previous_step = frames[static_cast<std::size_t>(last - 1)].time -
                                 frames[static_cast<std::size_t>(last - 2)].time;
    const double first_step = frames[1].time - frames[0].time;
    const double next_step = frames[2].time - frames[1].time;

    std::map<std::string, std::array<DesiredTransform, 3>> desired;
    for (const auto& compiled_slot : ordered_slots) {
        const auto& name = compiled_slot.name;
        const std::string anchor =
            strategy == BoneMapper::LoopSeamStrategy::PhysicsRelative
                ? fixedAnchor(name, by_name, physics_bones, fixed_physics_bones)
                : std::string{};
        DesiredTransform first = transformOf(poses[0], name, anchor);
        DesiredTransform second = transformOf(poses[1], name, anchor);
        DesiredTransform third = transformOf(poses[2], name, anchor);
        auto velocity = scale3(sub3(second.position, first.position), 1.0 / first_step);
        auto next_velocity = scale3(sub3(third.position, second.position), 1.0 / next_step);
        auto acceleration =
            match_acceleration
                ? scale3(sub3(next_velocity, velocity), 1.0 / (0.5 * (first_step + next_step)))
                : std::array<double, 3>{0, 0, 0};
        auto angular = angularVelocityQuat(first.rotation, second.rotation, first_step);
        auto next_angular = angularVelocityQuat(second.rotation, third.rotation, next_step);
        auto angular_acceleration =
            match_acceleration
                ? scale3(sub3(next_angular, angular), 1.0 / (0.5 * (first_step + next_step)))
                : std::array<double, 3>{0, 0, 0};
        const double span = 0.5 * (last_step + previous_step);
        DesiredTransform end = first;
        DesiredTransform penultimate{
            sub3(end.position, scale3(velocity, last_step)),
            RotationUtil::quaternionMultiply(
                end.rotation,
                RotationUtil::quaternionFromRotationVector(scale3(angular, -last_step)))};
        auto previous_velocity = sub3(velocity, scale3(acceleration, span));
        auto previous_angular = sub3(angular, scale3(angular_acceleration, span));
        DesiredTransform previous{
            sub3(penultimate.position, scale3(previous_velocity, previous_step)),
            RotationUtil::quaternionMultiply(
                penultimate.rotation,
                RotationUtil::quaternionFromRotationVector(scale3(previous_angular, -previous_step)))};
        std::array<DesiredTransform, 3> values{previous, penultimate, end};
        if (!anchor.empty()) {
            for (int slot = 0; slot < 3; ++slot) {
                const auto& anchor_pose = poses[static_cast<std::size_t>(last - 2 + slot)].at(anchor);
                values[static_cast<std::size_t>(slot)] =
                    composeAnchor(anchor_pose, values[static_cast<std::size_t>(slot)]);
            }
        }
        desired[name] = values;
    }

    const int tail_indices[3] = {last - 2, last - 1, last};
    std::array<std::map<std::string, ChannelTarget>, 3> targets;
    for (int slot = 0; slot < 3; ++slot) {
        const int frame_index = tail_indices[slot];
        auto position_overrides = overrides(frames[static_cast<std::size_t>(frame_index)], true);
        auto rotation_overrides = overrides(frames[static_cast<std::size_t>(frame_index)], false);
        for (const auto& compiled_slot : ordered_slots) {
            const auto& name = compiled_slot.name;
            const auto& bone = by_name.at(name);
            const auto& target = desired.at(name)[static_cast<std::size_t>(slot)];
            const auto slot_poses = BonePoseCalculator::calculate(
                bones, animation, frames[static_cast<std::size_t>(frame_index)].time,
                &position_overrides, &rotation_overrides);
            const BonePoseCalculator::Pose* parent =
                bone.has_parent ? &slot_poses.at(bone.parent) : nullptr;
            const auto* original = frameState(
                frames[static_cast<std::size_t>(frame_index)],
                compiled_slot);
            ChannelTarget channel =
                solveLocalChannel(bone, parent, target, original != nullptr ? original->rotation
                                                                            : std::array<double, 3>{});
            position_overrides[name] = channel.position;
            rotation_overrides[name] = channel.rotation;
            targets[static_cast<std::size_t>(slot)][name] = channel;
        }
    }

    const double previous_u =
        (frames[static_cast<std::size_t>(last - 2)].time - frames[static_cast<std::size_t>(start)].time) /
        duration;
    const double penultimate_u =
        (frames[static_cast<std::size_t>(last - 1)].time - frames[static_cast<std::size_t>(start)].time) /
        duration;

    for (const auto& compiled_slot : ordered_slots) {
        const auto& name = compiled_slot.name;
        const auto& bone = by_name.at(name);
        std::array<double, 3> position_coefficients[3];
        std::array<double, 3> rotation_coefficients[3];
        for (int axis = 0; axis < 3; ++axis) {
            const double p2 = targets[0].at(name).position[static_cast<std::size_t>(axis)] -
                              frameState(
                                  frames[static_cast<std::size_t>(last - 2)],
                                  compiled_slot)
                                  ->position[static_cast<std::size_t>(axis)];
            const double p1 = targets[1].at(name).position[static_cast<std::size_t>(axis)] -
                              frameState(
                                  frames[static_cast<std::size_t>(last - 1)],
                                  compiled_slot)
                                  ->position[static_cast<std::size_t>(axis)];
            const double p0 = targets[2].at(name).position[static_cast<std::size_t>(axis)] -
                              frameState(
                                  frames[static_cast<std::size_t>(last)],
                                  compiled_slot)
                                  ->position[static_cast<std::size_t>(axis)];
            position_coefficients[axis] =
                sampledValueCoefficients(p0, p1, p2, penultimate_u, previous_u);
        }
        std::array<std::array<double, 3>, 3> correction_vectors{};
        for (int slot = 0; slot < 3; ++slot) {
            const auto original_q = totalQuaternion(
                bone,
                frameState(
                    frames[static_cast<std::size_t>(tail_indices[slot])],
                    compiled_slot)
                    ->rotation);
            const auto target_q = totalQuaternion(bone, targets[static_cast<std::size_t>(slot)].at(name).rotation);
            correction_vectors[static_cast<std::size_t>(slot)] =
                correctionVector(original_q, target_q);
        }
        for (int axis = 0; axis < 3; ++axis) {
            rotation_coefficients[axis] = sampledValueCoefficients(
                correction_vectors[2][static_cast<std::size_t>(axis)],
                correction_vectors[1][static_cast<std::size_t>(axis)],
                correction_vectors[0][static_cast<std::size_t>(axis)], penultimate_u, previous_u);
        }

        std::optional<std::array<double, 3>> previous_euler;
        for (int index = start; index <= last; ++index) {
            const double u = normalizedTime(frames, start, index, duration);
            auto* state = frameState(
                frames[static_cast<std::size_t>(index)], compiled_slot);
            if (state == nullptr) {
                continue;
            }
            for (int axis = 0; axis < 3; ++axis) {
                state->position[static_cast<std::size_t>(axis)] +=
                    evaluate(position_coefficients[axis].data(), u);
            }
            const auto original_q = totalQuaternion(bone, state->rotation);
            std::array<double, 3> correction{};
            for (int axis = 0; axis < 3; ++axis) {
                correction[static_cast<std::size_t>(axis)] =
                    evaluate(rotation_coefficients[axis].data(), u);
            }
            auto total_euler = RotationUtil::bedrockEulerFromQuaternion(RotationUtil::quaternionMultiply(
                original_q, RotationUtil::quaternionFromRotationVector(correction)));
            const std::array<double, 3> guide =
                previous_euler ? *previous_euler
                               : std::array<double, 3>{bone.rotation[0] + state->rotation[0],
                                                       bone.rotation[1] + state->rotation[1],
                                                       bone.rotation[2] + state->rotation[2]};
            total_euler = RotationUtil::unwrapEuler(guide, total_euler);
            for (int axis = 0; axis < 3; ++axis) {
                state->rotation[static_cast<std::size_t>(axis)] =
                    total_euler[static_cast<std::size_t>(axis)] - bone.rotation[axis];
            }
            previous_euler = total_euler;
        }
    }

    std::set<std::string> preserved_drivers = fixed_physics_bones;
    for (const auto& name : physics_bones) {
        auto current = by_name.find(name);
        std::set<std::string> visited;
        while (current != by_name.end() && current->second.has_parent &&
               !current->second.parent.empty() &&
               visited.insert(current->second.name).second) {
            const auto parent = by_name.find(current->second.parent);
            if (parent == by_name.end()) {
                break;
            }
            if (!physics_bones.contains(parent->second.name)) {
                preserved_drivers.insert(parent->second.name);
            }
            current = parent;
        }
    }
    const std::set<std::string> corrected(ordered.begin(), ordered.end());
    const auto endpoint_stats =
        canonicalizeEndpoint(frames, corrected, preserved_drivers,
                             stable_layout_ptr);
    return Result{std::move(frames), start, duration, window.ratio,
                  endpoint_stats.canonicalized_bone_count,
                  endpoint_stats.preserved_driver_bone_count,
                  endpoint_stats.driver_endpoint_conflict_count};
}

}
