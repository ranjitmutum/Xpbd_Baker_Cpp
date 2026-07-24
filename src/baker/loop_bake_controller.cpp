#include "xpbd/baker/loop_bake_controller.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace xpbd::baker {

LoopBakeController::LoopBakeController(LoopBakeConfig config,
                                       std::unique_ptr<PeriodicStateAdapter> adapter)
    : config_(config), adapter_(std::move(adapter)) {
    if (!adapter_) {
        throw std::invalid_argument("adapter");
    }
    previous_ = adapter_->capture();
}

LoopBakeController::Outcome LoopBakeController::completeCycle() {
    completed_cycles_++;
    const auto current = adapter_->capture();
    latest_report_ = adapter_->compare(previous_, current);
    const bool valid = latest_report_->valid();
    const bool collisionSafe = latest_report_->collisionSafe(config_);
    const bool withinTolerances = latest_report_->isWithin(config_);
    const bool stable = completed_cycles_ >= config_.minimum_warmup_cycles &&
                         withinTolerances;
    stable_cycles_ = stable ? stable_cycles_ + 1 : 0;
    converged_ = stable_cycles_ >= config_.required_stable_cycles;
    previous_ = current;

    Outcome outcome;
    outcome.completed_cycles = completed_cycles_;
    outcome.stable_cycles = stable_cycles_;
    outcome.report = latest_report_;
    outcome.converged = converged_;
    outcome.finished =
        converged_ || completed_cycles_ >= config_.maximum_warmup_cycles;
    outcome.use_fallback =
        outcome.finished && !converged_ && config_.seam_fallback_enabled &&
        valid && collisionSafe;
    outcome.valid = valid;
    outcome.collision_safe = collisionSafe;
    outcome.within_tolerances = withinTolerances;
    outcome.normalized_score =
        latest_report_ ? latest_report_->normalizedScore(config_)
                       : std::numeric_limits<double>::infinity();
    return outcome;
}

}
