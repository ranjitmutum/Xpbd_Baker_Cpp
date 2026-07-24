#pragma once

#include "xpbd/baker/loop_error_report.hpp"

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xpbd::baker {

enum class LoopContactState {
    Separated,
    Touching,
    MeaningfulPenetration,
};

struct LoopContactSignature {
    std::string pair;
    LoopContactState state = LoopContactState::Separated;
    int penetration_bucket = 0;
};

class PeriodicStateAdapter {
public:
    struct BoneState {
        std::array<double, 3> position{};
        bool has_position = true;
        std::array<double, 4> rotation_quaternion{0, 0, 0, 1};
        bool has_rotation = false;
        std::array<double, 3> linear_velocity{};
        bool has_linear_velocity = true;
        std::array<double, 3> angular_velocity{};
        bool has_angular_velocity = false;
        std::array<double, 3> com_position{};
        bool has_com_position = false;
        std::array<double, 4> com_rotation_quaternion{0, 0, 0, 1};
        bool has_com_rotation = false;
        std::array<double, 3> com_linear_velocity{};
        bool has_com_linear_velocity = false;
    };

    struct Snapshot {
        std::map<std::string, BoneState> bones;
        std::set<std::string> expected_bones;
        std::set<std::string> contacts;
        std::map<std::string, double> contact_penetrations;
        std::map<std::string, LoopContactSignature> contact_signatures;
        double enter_contact_threshold = 1e-5;
        double exit_contact_threshold = 5e-6;
        double contact_penetration_bucket_width = 4e-5;
        bool has_contacts = false;
        double maximum_penetration = 0.0;
        bool has_maximum_penetration = false;
        double maximum_penetration_time = -1.0;
        bool has_maximum_penetration_time = false;
        double sample_time = 0.0;
        bool has_sample_time = false;
        PeriodicMetricAvailability required_metrics{};
        int anomaly_count = 0;
    };

    virtual ~PeriodicStateAdapter() = default;
    virtual Snapshot capture() = 0;
    virtual LoopErrorReport compare(const Snapshot& previous, const Snapshot& current);

    static SnapshotValidation validateSnapshot(const Snapshot& snapshot);
    static LoopErrorReport compareSnapshots(const Snapshot& previous, const Snapshot& current);
};

}
