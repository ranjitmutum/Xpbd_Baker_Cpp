#pragma once

#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"

#include <string>
#include <vector>

namespace xpbd::baker {

// Compatibility changes are applied only to a per-bake mapper/animation copy.
// The user's loaded model and authored animation remain untouched.
struct RigidBodyInputCompatibilityReport {
  std::vector<std::string> skipped_blocked_dynamic_bones;
  std::vector<std::string> promoted_animated_compound_bones;
  std::vector<std::string> repaired_source_scale_bones;
  std::vector<std::string> repaired_target_scale_bones;

  [[nodiscard]] bool changed() const noexcept {
    return !skipped_blocked_dynamic_bones.empty() ||
           !promoted_animated_compound_bones.empty() ||
           !repaired_source_scale_bones.empty() ||
           !repaired_target_scale_bones.empty();
  }
};

// Makes a rigid-body bake copy tolerant of common authoring patterns:
// 1. an empty dynamic parent is selected together with a descendant that owns
//    all of the available cubes;
// 2. a compound descendant has a numeric position/rotation channel that
//    changes over time, so it must become an independent rigid body;
// 3. a numeric scale channel contains a zero/non-finite keyframe.
//
// Strict validation remains enabled after this preflight. Unsupported shear,
// dynamic collider scaling, Molang transforms, and genuinely empty unblocked
// bodies are still rejected by the normal Bullet validation path.
[[nodiscard]] RigidBodyInputCompatibilityReport
prepareRigidBodyInputCompatibility(BoneMapper &mapper,
                                   loader::Animation &source_animation,
                                   loader::Animation *target_animation = nullptr);

} // namespace xpbd::baker
