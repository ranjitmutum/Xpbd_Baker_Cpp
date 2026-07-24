#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/periodic_state_adapter.hpp"
#include "xpbd/baker/rotation_util.hpp"

#include <map>
#include <string>

namespace xpbd::baker {








class XpbdPeriodicStateTracker {
public:
  using WorldRotations = std::map<std::string, RotationUtil::Quat>;

  void clear();
  void initialize(const BakedFrame &frame,
                  const WorldRotations &world_rotations);
  void advance(const BakedFrame &frame, const WorldRotations &world_rotations,
               double dt);

  [[nodiscard]] PeriodicStateAdapter::Snapshot capture() const {
    return snapshot_;
  }

private:
  void record(const BakedFrame &frame, const WorldRotations &world_rotations,
              double dt, bool calculate_angular_velocity);

  WorldRotations previous_world_rotations_;
  PeriodicStateAdapter::Snapshot snapshot_;
  bool initialized_ = false;
};

}
