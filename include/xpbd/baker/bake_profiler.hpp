#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace xpbd::baker {

class BakeProfiler {
public:
  enum class Stage {
    TotalBake,
    Initialize,
    OuterReferencePose,
    LoopController,
    LoopFinalization,
    FinalCollisionAudit,
    Resample,
    Blend,
    Unwrap,
    Solver,
    FrameCapture,
    Reconstruction,
    Velocity,
    Count
  };

  enum class Counter : int {
    SimulationSteps,
    FrameCaptures,
    OutputFrames,
    Particles,
    Constraints,
    BodyColliders,
    LoopCycles,
    LoopCandidates,
    UnsafeFinalCollisions,
    XpbdBodyBroadPhaseQueries,
    XpbdBodyBroadPhasePossible,
    XpbdBodyBroadPhaseCandidates,
    XpbdBodyNarrowPhaseTests,
    RigidAuditPossiblePairs,
    RigidAuditBroadPhaseCandidates,
    RigidAuditSatCalls,
    Count
  };

  static constexpr std::size_t kStageCount =
      static_cast<std::size_t>(Stage::Count);

  struct StageStats {
    std::uint64_t calls = 0;
    std::chrono::nanoseconds elapsed{};
  };

  struct Snapshot {
    bool enabled = false;
    std::array<StageStats, kStageCount> stages{};
    std::map<int, std::int64_t> counters;

    [[nodiscard]] const StageStats &stage(Stage stage) const;
    [[nodiscard]] std::int64_t counter(int id) const;
    [[nodiscard]] std::int64_t counter(Counter id) const {
      return counter(static_cast<int>(id));
    }
  };

  class ScopedStage;

  static BakeProfiler disabled();
  static BakeProfiler enabled();

  [[nodiscard]] bool isEnabled() const { return state_ != nullptr; }
  [[nodiscard]] std::int64_t start(Stage stage) const;
  void stop(Stage stage, std::int64_t token) const;
  [[nodiscard]] ScopedStage scope(Stage stage) const;

  void setCounter(int id, std::int64_t value) const;
  void addCounter(int id, std::int64_t delta) const;
  void setCounter(Counter id, std::int64_t value) const {
    setCounter(static_cast<int>(id), value);
  }
  void addCounter(Counter id, std::int64_t delta) const {
    addCounter(static_cast<int>(id), delta);
  }

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] std::string report() const;
  void reset() const;

  [[nodiscard]] static std::string_view stageName(Stage stage);
  [[nodiscard]] static std::string_view counterName(int id);

private:
  struct State;

  explicit BakeProfiler(bool enabled);

  std::shared_ptr<State> state_;
};

class BakeProfiler::ScopedStage {
public:
  ScopedStage(const ScopedStage &) = delete;
  ScopedStage &operator=(const ScopedStage &) = delete;
  ScopedStage(ScopedStage &&other) noexcept;
  ScopedStage &operator=(ScopedStage &&other) noexcept;
  ~ScopedStage();

  void stop() noexcept;

private:
  friend class BakeProfiler;

  ScopedStage(BakeProfiler profiler, Stage stage, std::int64_t token);

  BakeProfiler profiler_;
  Stage stage_ = Stage::TotalBake;
  std::int64_t token_ = 0;
};

}
