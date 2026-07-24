#include "xpbd/baker/bake_profiler.hpp"

#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <utility>

namespace xpbd::baker {

struct BakeProfiler::State {
  struct ActiveTimer {
    Stage stage = Stage::TotalBake;
    std::chrono::steady_clock::time_point started;
  };

  mutable std::mutex mutex;
  std::array<StageStats, kStageCount> stages{};
  std::map<int, std::int64_t> counters;
  std::map<std::int64_t, ActiveTimer> active_timers;
  std::int64_t next_token = 1;
};

namespace {

bool isValidStage(BakeProfiler::Stage stage) {
  return static_cast<std::size_t>(stage) < BakeProfiler::kStageCount;
}

}

BakeProfiler::BakeProfiler(bool enabled)
    : state_(enabled ? std::make_shared<State>() : nullptr) {}

BakeProfiler BakeProfiler::disabled() { return BakeProfiler(false); }

BakeProfiler BakeProfiler::enabled() { return BakeProfiler(true); }

std::int64_t BakeProfiler::start(Stage stage) const {
  if (state_ == nullptr || !isValidStage(stage)) {
    return 0;
  }
  std::lock_guard lock(state_->mutex);
  std::int64_t token = 0;
  do {
    token = state_->next_token;
    state_->next_token =
        token == std::numeric_limits<std::int64_t>::max() ? 1 : token + 1;
  } while (state_->active_timers.contains(token));
  state_->active_timers.emplace(
      token, State::ActiveTimer{stage, std::chrono::steady_clock::now()});
  return token;
}

void BakeProfiler::stop(Stage stage, std::int64_t token) const {
  if (state_ == nullptr || token == 0 || !isValidStage(stage)) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  const auto timer = state_->active_timers.find(token);
  if (timer == state_->active_timers.end()) {
    return;
  }
  const State::ActiveTimer active = timer->second;
  state_->active_timers.erase(timer);
  if (active.stage != stage) {
    return;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - active.started);
  StageStats &stats = state_->stages[static_cast<std::size_t>(stage)];
  stats.calls++;
  if (elapsed.count() > 0) {
    stats.elapsed += elapsed;
  }
}

BakeProfiler::ScopedStage BakeProfiler::scope(Stage stage) const {
  return ScopedStage(*this, stage, start(stage));
}

void BakeProfiler::setCounter(int id, std::int64_t value) const {
  if (state_ == nullptr) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->counters[id] = value;
}

void BakeProfiler::addCounter(int id, std::int64_t delta) const {
  if (state_ == nullptr) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->counters[id] += delta;
}

BakeProfiler::Snapshot BakeProfiler::snapshot() const {
  Snapshot result;
  result.enabled = state_ != nullptr;
  if (state_ == nullptr) {
    return result;
  }
  std::lock_guard lock(state_->mutex);
  result.stages = state_->stages;
  result.counters = state_->counters;
  return result;
}

std::string BakeProfiler::report() const {
  const Snapshot values = snapshot();
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "enabled=" << (values.enabled ? 1 : 0) << '\n';
  for (std::size_t index = 0; index < values.stages.size(); ++index) {
    const auto stage = static_cast<Stage>(index);
    const StageStats &stats = values.stages[index];
    output << "stage." << stageName(stage) << ".calls=" << stats.calls << '\n';
    output << "stage." << stageName(stage)
           << ".elapsed_ns=" << stats.elapsed.count() << '\n';
  }
  for (const auto &[id, value] : values.counters) {
    output << "counter.";
    const std::string_view name = counterName(id);
    if (name.empty()) {
      output << id;
    } else {
      output << name;
    }
    output << '=' << value << '\n';
  }
  return output.str();
}

void BakeProfiler::reset() const {
  if (state_ == nullptr) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  state_->stages = {};
  state_->counters.clear();
  state_->active_timers.clear();
}

std::string_view BakeProfiler::stageName(Stage stage) {
  switch (stage) {
  case Stage::TotalBake:
    return "total_bake";
  case Stage::Initialize:
    return "initialize";
  case Stage::OuterReferencePose:
    return "outer_reference_pose";
  case Stage::LoopController:
    return "loop_controller";
  case Stage::LoopFinalization:
    return "loop_finalization";
  case Stage::FinalCollisionAudit:
    return "final_collision_audit";
  case Stage::Resample:
    return "resample";
  case Stage::Blend:
    return "blend";
  case Stage::Unwrap:
    return "unwrap";
  case Stage::Solver:
    return "solver";
  case Stage::FrameCapture:
    return "frame_capture";
  case Stage::Reconstruction:
    return "reconstruction";
  case Stage::Velocity:
    return "velocity";
  case Stage::Count:
    break;
  }
  return "unknown";
}

std::string_view BakeProfiler::counterName(int id) {
  switch (static_cast<Counter>(id)) {
  case Counter::SimulationSteps:
    return "simulation_steps";
  case Counter::FrameCaptures:
    return "frame_captures";
  case Counter::OutputFrames:
    return "output_frames";
  case Counter::Particles:
    return "particles";
  case Counter::Constraints:
    return "constraints";
  case Counter::BodyColliders:
    return "body_colliders";
  case Counter::LoopCycles:
    return "loop_cycles";
  case Counter::LoopCandidates:
    return "loop_candidates";
  case Counter::UnsafeFinalCollisions:
    return "unsafe_final_collisions";
  case Counter::XpbdBodyBroadPhaseQueries:
    return "xpbd_body_broad_phase_queries";
  case Counter::XpbdBodyBroadPhasePossible:
    return "xpbd_body_broad_phase_possible";
  case Counter::XpbdBodyBroadPhaseCandidates:
    return "xpbd_body_broad_phase_candidates";
  case Counter::XpbdBodyNarrowPhaseTests:
    return "xpbd_body_narrow_phase_tests";
  case Counter::RigidAuditPossiblePairs:
    return "rigid_audit_possible_pairs";
  case Counter::RigidAuditBroadPhaseCandidates:
    return "rigid_audit_broad_phase_candidates";
  case Counter::RigidAuditSatCalls:
    return "rigid_audit_sat_calls";
  case Counter::Count:
    break;
  }
  return {};
}

const BakeProfiler::StageStats &
BakeProfiler::Snapshot::stage(Stage stage_value) const {
  return stages.at(static_cast<std::size_t>(stage_value));
}

std::int64_t BakeProfiler::Snapshot::counter(int id) const {
  const auto value = counters.find(id);
  return value == counters.end() ? 0 : value->second;
}

BakeProfiler::ScopedStage::ScopedStage(BakeProfiler profiler, Stage stage,
                                       std::int64_t token)
    : profiler_(std::move(profiler)), stage_(stage), token_(token) {}

BakeProfiler::ScopedStage::ScopedStage(ScopedStage &&other) noexcept
    : profiler_(std::move(other.profiler_)), stage_(other.stage_),
      token_(std::exchange(other.token_, 0)) {}

BakeProfiler::ScopedStage &
BakeProfiler::ScopedStage::operator=(ScopedStage &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  stop();
  profiler_ = std::move(other.profiler_);
  stage_ = other.stage_;
  token_ = std::exchange(other.token_, 0);
  return *this;
}

BakeProfiler::ScopedStage::~ScopedStage() { stop(); }

void BakeProfiler::ScopedStage::stop() noexcept {
  const std::int64_t token = std::exchange(token_, 0);
  if (token == 0) {
    return;
  }
  try {
    profiler_.stop(stage_, token);
  } catch (...) {

  }
}

}
