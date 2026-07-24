#include "xpbd/baker/periodic_state_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace xpbd::baker {
namespace {

template <std::size_t Size>
bool finiteArray(const std::array<double, Size>& values) {
    return std::all_of(values.begin(), values.end(),
                       [](double value) { return std::isfinite(value); });
}

void recordIssue(SnapshotValidation& validation, LoopValidationState state,
                 const std::string& bone, const std::string& field) {
    validation.complete = false;
    if (validation.state == LoopValidationState::Valid) {
        validation.state = state;
        validation.first_invalid_bone = bone;
        validation.first_invalid_field = field;
    }
}

void recordMissingMetric(SnapshotValidation& validation, const std::string& bone,
                         const std::string& field) {
    ++validation.missing_metric_count;
    recordIssue(validation, LoopValidationState::MissingMetric, bone, field);
}

void recordNonFinite(SnapshotValidation& validation, const std::string& bone,
                     const std::string& field) {
    ++validation.non_finite_value_count;
    recordIssue(validation, LoopValidationState::NonFiniteValue, bone, field);
}

void mergeValidation(SnapshotValidation& target, const SnapshotValidation& source) {
    if (!source.complete && target.state == LoopValidationState::Valid) {
        target.state = source.state;
        target.first_invalid_bone = source.first_invalid_bone;
        target.first_invalid_field = source.first_invalid_field;
    }
    target.complete = target.complete && source.complete;
    target.non_finite_value_count += source.non_finite_value_count;
    target.missing_bone_count += source.missing_bone_count;
    target.missing_metric_count += source.missing_metric_count;
    target.invalid_quaternion_count += source.invalid_quaternion_count;
    target.invalid_contact_count += source.invalid_contact_count;
}

double vectorDistance(const std::array<double, 3>& first,
                      const std::array<double, 3>& second) {
    const double x = first[0] - second[0];
    const double y = first[1] - second[1];
    const double z = first[2] - second[2];
    return std::sqrt(x * x + y * y + z * z);
}

double quaternionLength(const std::array<double, 4>& value) {
    double lengthSquared = 0.0;
    for (double component : value) {
        lengthSquared += component * component;
    }
    return std::sqrt(lengthSquared);
}

double quaternionAngle(const std::array<double, 4>& first,
                       const std::array<double, 4>& second) {
    const double firstLength = quaternionLength(first);
    const double secondLength = quaternionLength(second);
    if (!(firstLength > 1e-10) || !(secondLength > 1e-10)) {
        return std::numeric_limits<double>::infinity();
    }
    double dot = 0.0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        dot += (first[index] / firstLength) * (second[index] / secondLength);
    }
    return 2.0 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0));
}

int validationIssueCount(const SnapshotValidation& validation) {
    return validation.non_finite_value_count + validation.missing_bone_count +
           validation.missing_metric_count + validation.invalid_quaternion_count +
           validation.invalid_contact_count;
}

bool contactPresent(const PeriodicStateAdapter::Snapshot& snapshot,
                    const std::string& pair) {
    return snapshot.contacts.contains(pair) ||
           snapshot.contact_penetrations.contains(pair);
}

double contactPenetration(const PeriodicStateAdapter::Snapshot& snapshot,
                          const std::string& pair) {
    const auto found = snapshot.contact_penetrations.find(pair);
    return found == snapshot.contact_penetrations.end()
               ? 0.0
               : std::max(0.0, found->second);
}

LoopContactState rawContactState(const PeriodicStateAdapter::Snapshot& snapshot,
                                 const std::string& pair) {
    if (!contactPresent(snapshot, pair)) {
        return LoopContactState::Separated;
    }
    return contactPenetration(snapshot, pair) >= snapshot.enter_contact_threshold
               ? LoopContactState::MeaningfulPenetration
               : LoopContactState::Touching;
}

int contactBucket(double penetration, double width) {
    if (!(width > 0.0) || !std::isfinite(width) || !(penetration > 0.0)) {
        return 0;
    }
    const double bucket = std::floor(penetration / width);
    if (bucket >= static_cast<double>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(bucket);
}

LoopBoundaryState boundaryState(const PeriodicStateAdapter::Snapshot& snapshot) {
    LoopBoundaryState boundary;
    boundary.sample_time = snapshot.sample_time;
    boundary.has_sample_time = snapshot.has_sample_time;
    boundary.maximum_penetration = snapshot.maximum_penetration;
    boundary.has_maximum_penetration = snapshot.has_maximum_penetration;
    boundary.maximum_penetration_time = snapshot.maximum_penetration_time;
    boundary.has_maximum_penetration_time =
        snapshot.has_maximum_penetration_time;
    boundary.bodies.reserve(snapshot.bones.size());
    for (const auto& [name, state] : snapshot.bones) {
        LoopBoundaryBodyState body;
        body.bone_name = name;
        body.pivot_position = state.position;
        body.has_pivot_position = state.has_position;
        body.pivot_rotation_xyzw = state.rotation_quaternion;
        body.has_pivot_rotation = state.has_rotation;
        body.pivot_linear_velocity = state.linear_velocity;
        body.has_pivot_linear_velocity = state.has_linear_velocity;
        body.com_position = state.com_position;
        body.has_com_position = state.has_com_position;
        body.com_rotation_xyzw = state.com_rotation_quaternion;
        body.has_com_rotation = state.has_com_rotation;
        body.com_linear_velocity = state.com_linear_velocity;
        body.has_com_linear_velocity = state.has_com_linear_velocity;
        body.angular_velocity = state.angular_velocity;
        body.has_angular_velocity = state.has_angular_velocity;
        boundary.bodies.push_back(std::move(body));
    }

    std::set<std::string> pairs = snapshot.contacts;
    for (const auto& [pair, penetration] : snapshot.contact_penetrations) {
        (void)penetration;
        pairs.insert(pair);
    }
    boundary.contacts.reserve(pairs.size());
    for (const auto& pair : pairs) {
        LoopBoundaryContactState contact;
        contact.pair = pair;
        contact.penetration = contactPenetration(snapshot, pair);
        auto state = rawContactState(snapshot, pair);
        const auto signature = snapshot.contact_signatures.find(pair);
        if (signature != snapshot.contact_signatures.end()) {
            state = signature->second.state;
            contact.penetration_bucket = signature->second.penetration_bucket;
        } else if (state == LoopContactState::MeaningfulPenetration) {
            contact.penetration_bucket = contactBucket(
                contact.penetration, snapshot.contact_penetration_bucket_width);
        }
        contact.meaningful_penetration =
            state == LoopContactState::MeaningfulPenetration;
        boundary.contacts.push_back(std::move(contact));
    }
    return boundary;
}

}

LoopErrorReport PeriodicStateAdapter::compare(const Snapshot& previous,
                                              const Snapshot& current) {
    return compareSnapshots(previous, current);
}

SnapshotValidation PeriodicStateAdapter::validateSnapshot(const Snapshot& snapshot) {
    SnapshotValidation validation;

    for (const auto& name : snapshot.expected_bones) {
        if (!snapshot.bones.contains(name)) {
            ++validation.missing_bone_count;
            recordIssue(validation, LoopValidationState::MissingBone, name, "bone");
        }
    }

    for (const auto& [name, bone] : snapshot.bones) {
        const auto validateVector = [&](bool present, bool required,
                                        const std::array<double, 3>& value,
                                        const char* field) {
            if (required && !present) {
                recordMissingMetric(validation, name, field);
            } else if (present && !finiteArray(value)) {
                recordNonFinite(validation, name, field);
            }
        };
        validateVector(bone.has_position, snapshot.required_metrics.position,
                       bone.position, "position");
        validateVector(bone.has_linear_velocity,
                       snapshot.required_metrics.linear_velocity,
                       bone.linear_velocity, "linear_velocity");
        validateVector(bone.has_angular_velocity,
                       snapshot.required_metrics.angular_velocity,
                       bone.angular_velocity, "angular_velocity");

        if (snapshot.required_metrics.rotation && !bone.has_rotation) {
            recordMissingMetric(validation, name, "rotation");
        } else if (bone.has_rotation) {
            if (!finiteArray(bone.rotation_quaternion)) {
                recordNonFinite(validation, name, "rotation");
            } else if (!(quaternionLength(bone.rotation_quaternion) > 1e-10)) {
                ++validation.invalid_quaternion_count;
                recordIssue(validation, LoopValidationState::InvalidQuaternion,
                            name, "rotation");
            }
        }
    }

    if (snapshot.required_metrics.sample_time && !snapshot.has_sample_time) {
        recordMissingMetric(validation, {}, "sample_time");
    } else if (snapshot.has_sample_time && !std::isfinite(snapshot.sample_time)) {
        ++validation.non_finite_value_count;
        recordIssue(validation, LoopValidationState::InvalidSampleTime, {},
                    "sample_time");
    }

    if (snapshot.required_metrics.maximum_penetration &&
        !snapshot.has_maximum_penetration) {
        recordMissingMetric(validation, {}, "maximum_penetration");
    } else if (snapshot.has_maximum_penetration &&
               !std::isfinite(snapshot.maximum_penetration)) {
        recordNonFinite(validation, {}, "maximum_penetration");
    }

    if (snapshot.required_metrics.contacts && !snapshot.has_contacts) {
        recordMissingMetric(validation, {}, "contacts");
    }
    if (snapshot.has_contacts) {
        if (!std::isfinite(snapshot.enter_contact_threshold) ||
            !std::isfinite(snapshot.exit_contact_threshold) ||
            !std::isfinite(snapshot.contact_penetration_bucket_width) ||
            !(snapshot.enter_contact_threshold > 0.0) ||
            snapshot.exit_contact_threshold < 0.0 ||
            snapshot.exit_contact_threshold > snapshot.enter_contact_threshold ||
            !(snapshot.contact_penetration_bucket_width > 0.0)) {
            recordNonFinite(validation, {}, "contact_thresholds");
        }
        for (const auto& identifier : snapshot.contacts) {
            if (identifier.empty()) {
                ++validation.invalid_contact_count;
                recordIssue(validation, LoopValidationState::InvalidContact, {},
                            "contact_identifier");
            }
            if (snapshot.required_metrics.contacts &&
                !snapshot.contact_penetrations.contains(identifier)) {
                recordMissingMetric(validation, identifier,
                                    "contact_penetration");
            }
        }
        for (const auto& [identifier, penetration] : snapshot.contact_penetrations) {
            if (identifier.empty()) {
                ++validation.invalid_contact_count;
                recordIssue(validation, LoopValidationState::InvalidContact, {},
                            "contact_identifier");
            }
            if (!std::isfinite(penetration)) {
                recordNonFinite(validation, identifier, "contact_penetration");
            }
        }
        for (const auto& [identifier, signature] : snapshot.contact_signatures) {
            if (identifier.empty() || signature.pair != identifier) {
                ++validation.invalid_contact_count;
                recordIssue(validation, LoopValidationState::InvalidContact,
                            identifier, "contact_signature");
            }
        }
    }

    if (snapshot.anomaly_count > 0) {
        recordIssue(validation, LoopValidationState::ProducerAnomaly, {},
                    "producer_anomaly");
    }
    return validation;
}

LoopErrorReport PeriodicStateAdapter::compareSnapshots(const Snapshot& previous,
                                                       const Snapshot& current) {
    LoopErrorReport report;
    report.start_boundary = boundaryState(previous);
    report.end_boundary = boundaryState(current);
    const SnapshotValidation previousValidation = validateSnapshot(previous);
    const SnapshotValidation currentValidation = validateSnapshot(current);
    mergeValidation(report.validation, previousValidation);
    mergeValidation(report.validation, currentValidation);
    report.anomaly_count = previous.anomaly_count + current.anomaly_count +
                           validationIssueCount(previousValidation) +
                           validationIssueCount(currentValidation);

    for (const auto& [name, state] : previous.bones) {
        (void)state;
        if (!current.bones.contains(name) &&
            !current.expected_bones.contains(name)) {
            ++report.validation.missing_bone_count;
            ++report.anomaly_count;
            recordIssue(report.validation, LoopValidationState::MissingBone,
                        name, "bone");
        }
    }
    for (const auto& [name, state] : current.bones) {
        (void)state;
        if (!previous.bones.contains(name) &&
            !previous.expected_bones.contains(name)) {
            ++report.validation.missing_bone_count;
            ++report.anomaly_count;
            recordIssue(report.validation, LoopValidationState::MissingBone,
                        name, "bone");
        }
    }

    report.availability.maximum_penetration =
        previous.has_maximum_penetration && current.has_maximum_penetration;
    report.availability.maximum_penetration_time =
        previous.has_maximum_penetration_time &&
        current.has_maximum_penetration_time;
    report.availability.contacts = previous.has_contacts && current.has_contacts;
    report.availability.sample_time =
        previous.has_sample_time && current.has_sample_time;
    if (report.availability.maximum_penetration) {
        report.maximum_penetration = current.maximum_penetration;
    }
    if (report.availability.maximum_penetration_time) {
        report.maximum_penetration_time = current.maximum_penetration_time;
    }

    bool comparedPosition = false;
    bool comparedRotation = false;
    bool comparedLinear = false;
    bool comparedAngular = false;
    for (const auto& [name, before] : previous.bones) {
        const auto afterIt = current.bones.find(name);
        if (afterIt == current.bones.end()) {
            continue;
        }
        const auto& after = afterIt->second;
        if (before.has_position && after.has_position && finiteArray(before.position) &&
            finiteArray(after.position)) {
            comparedPosition = true;
            const double error = vectorDistance(before.position, after.position);
            if (error >= report.maximum_position_error) {
                report.maximum_position_error = error;
                report.position_bone = name;
            }
        }
        if (before.has_linear_velocity && after.has_linear_velocity &&
            finiteArray(before.linear_velocity) && finiteArray(after.linear_velocity)) {
            comparedLinear = true;
            const double error =
                vectorDistance(before.linear_velocity, after.linear_velocity);
            if (error >= report.maximum_linear_velocity_error) {
                report.maximum_linear_velocity_error = error;
                report.linear_velocity_bone = name;
            }
        }
        if (before.has_rotation && after.has_rotation &&
            finiteArray(before.rotation_quaternion) &&
            finiteArray(after.rotation_quaternion)) {
            const double error =
                quaternionAngle(before.rotation_quaternion, after.rotation_quaternion);
            if (std::isfinite(error)) {
                comparedRotation = true;
                if (error >= report.maximum_rotation_error_radians) {
                    report.maximum_rotation_error_radians = error;
                    report.rotation_bone = name;
                }
            }
        }
        if (before.has_angular_velocity && after.has_angular_velocity &&
            finiteArray(before.angular_velocity) && finiteArray(after.angular_velocity)) {
            comparedAngular = true;
            const double error =
                vectorDistance(before.angular_velocity, after.angular_velocity);
            if (error >= report.maximum_angular_velocity_error) {
                report.maximum_angular_velocity_error = error;
                report.angular_velocity_bone = name;
            }
        }
    }
    report.availability.position = comparedPosition;
    report.availability.rotation = comparedRotation;
    report.availability.linear_velocity = comparedLinear;
    report.availability.angular_velocity = comparedAngular;

    if (report.availability.contacts) {
        std::set<std::string> pairs = previous.contacts;
        pairs.insert(current.contacts.begin(), current.contacts.end());
        for (const auto& [pair, penetration] : previous.contact_penetrations) {
            (void)penetration;
            pairs.insert(pair);
        }
        for (const auto& [pair, penetration] : current.contact_penetrations) {
            (void)penetration;
            pairs.insert(pair);
        }
        for (const auto& pair : pairs) {
            const bool beforePresent = contactPresent(previous, pair);
            const bool afterPresent = contactPresent(current, pair);
            if (!beforePresent && afterPresent) {
                ++report.contact_pair_added_count;
            }
            if (beforePresent && !afterPresent) {
                ++report.contact_pair_removed_count;
            }

            LoopContactState beforeState = rawContactState(previous, pair);
            const auto priorSignature = previous.contact_signatures.find(pair);
            if (priorSignature != previous.contact_signatures.end()) {
                beforeState = priorSignature->second.state;
            }
            LoopContactState afterState = LoopContactState::Separated;
            if (afterPresent) {
                const double threshold =
                    beforeState == LoopContactState::MeaningfulPenetration
                        ? current.exit_contact_threshold
                        : current.enter_contact_threshold;
                afterState = contactPenetration(current, pair) >= threshold
                                 ? LoopContactState::MeaningfulPenetration
                                 : LoopContactState::Touching;
            }
            if (beforeState != afterState) {
                ++report.contact_state_changed_count;
            }

            const int beforeBucket =
                beforeState == LoopContactState::MeaningfulPenetration
                    ? contactBucket(contactPenetration(previous, pair),
                                    previous.contact_penetration_bucket_width)
                    : 0;
            const int afterBucket =
                afterState == LoopContactState::MeaningfulPenetration
                    ? contactBucket(contactPenetration(current, pair),
                                    current.contact_penetration_bucket_width)
                    : 0;
            const bool meaningfulChanged =
                (beforeState == LoopContactState::MeaningfulPenetration) !=
                    (afterState == LoopContactState::MeaningfulPenetration) ||
                (beforeState == LoopContactState::MeaningfulPenetration &&
                 afterState == LoopContactState::MeaningfulPenetration &&
                 beforeBucket != afterBucket);
            if (meaningfulChanged) {
                ++report.meaningful_penetration_changed_count;
            }
        }
        report.contact_difference_count =
            report.contact_pair_added_count + report.contact_pair_removed_count +
            report.contact_state_changed_count +
            report.meaningful_penetration_changed_count;
        report.contact_set_changed =
            report.meaningful_penetration_changed_count > 0;
    }
    return report;
}

}
