#pragma once

#include "xpbd/baker/loop_bake_config.hpp"
#include "xpbd/baker/loop_error_report.hpp"
#include "xpbd/baker/periodic_state_adapter.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace xpbd::baker {

class LoopBakeController {
public:
    struct Outcome {
        int completed_cycles = 0;
        int stable_cycles = 0;
        std::optional<LoopErrorReport> report;
        bool converged = false;
        bool finished = false;
        bool use_fallback = false;
        bool valid = false;
        bool collision_safe = false;
        bool within_tolerances = false;
        double normalized_score = 0.0;
    };

    LoopBakeController(LoopBakeConfig config, std::unique_ptr<PeriodicStateAdapter> adapter);

    Outcome completeCycle();

private:
    LoopBakeConfig config_;
    std::unique_ptr<PeriodicStateAdapter> adapter_;
    PeriodicStateAdapter::Snapshot previous_;
    std::optional<LoopErrorReport> latest_report_;
    int completed_cycles_ = 0;
    int stable_cycles_ = 0;
    bool converged_ = false;
};

struct LoopCycleCandidate {
    int cycle_index = 0;
    bool valid = false;
    bool collision_safe = false;
    bool within_tolerances = false;
    double normalized_score = std::numeric_limits<double>::infinity();
    LoopErrorReport report{};
    std::vector<std::string> rejection_reasons;

    [[nodiscard]] int selectionRank() const {
        if (!valid) {
            return 0;
        }
        return collision_safe ? 2 : 1;
    }

    [[nodiscard]] bool isBetterThan(const LoopCycleCandidate* other) const {
        if (other == nullptr) {
            return true;
        }
        if (selectionRank() != other->selectionRank()) {
            return selectionRank() > other->selectionRank();
        }
        if (normalized_score < other->normalized_score) {
            return true;
        }
        if (normalized_score > other->normalized_score) {
            return false;
        }
        return cycle_index < other->cycle_index;
    }

    [[nodiscard]] bool
    isBetterPreviewThan(const LoopCycleCandidate* other) const {
        if (!valid) {
            return false;
        }
        if (other == nullptr || !other->valid) {
            return true;
        }
        if (normalized_score < other->normalized_score) {
            return true;
        }
        if (normalized_score > other->normalized_score) {
            return false;
        }
        return cycle_index < other->cycle_index;
    }

    [[nodiscard]] bool
    isBetterSafeExportThan(const LoopCycleCandidate* other) const {
        if (!valid || !collision_safe) {
            return false;
        }
        if (other == nullptr || !other->valid || !other->collision_safe) {
            return true;
        }
        if (normalized_score < other->normalized_score) {
            return true;
        }
        if (normalized_score > other->normalized_score) {
            return false;
        }
        return cycle_index < other->cycle_index;
    }
};

}
