#include "xpbd/baker/bone_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace xpbd::baker {

BoneMapper::BoneMapper(std::vector<loader::Bone> bones) {
  replaceModelBones(std::move(bones));
}

double
BoneMapper::animationFollowStrengthToCompliance(double strength) noexcept {
  if (std::isnan(strength) || strength <= 0.0) {
    return 0.0;
  }
  const double canonical_strength =
      std::clamp(strength, kAnimationFollowMinimumEnabledStrength, 1.0);
  const double normalized =
      (canonical_strength - kAnimationFollowMinimumEnabledStrength) /
      (1.0 - kAnimationFollowMinimumEnabledStrength);
  const double log_max = std::log(kAnimationFollowMaximumCompliance);
  const double log_min = std::log(kAnimationFollowMinimumCompliance);
  return std::exp(log_max + normalized * (log_min - log_max));
}

double
BoneMapper::animationFollowComplianceToStrength(double compliance) noexcept {
  if (std::isnan(compliance) || compliance <= 0.0) {
    return 0.0;
  }
  if (!std::isfinite(compliance)) {
    return kAnimationFollowMinimumEnabledStrength;
  }
  const double canonical_compliance =
      std::clamp(compliance, kAnimationFollowMinimumCompliance,
                 kAnimationFollowMaximumCompliance);
  const double log_max = std::log(kAnimationFollowMaximumCompliance);
  const double log_min = std::log(kAnimationFollowMinimumCompliance);
  const double normalized =
      (std::log(canonical_compliance) - log_max) / (log_min - log_max);
  return kAnimationFollowMinimumEnabledStrength +
         (1.0 - kAnimationFollowMinimumEnabledStrength) *
             std::clamp(normalized, 0.0, 1.0);
}

void BoneMapper::replaceModelBones(std::vector<loader::Bone> bones) {
  clearModelState();
  all_bones_ = std::move(bones);
  refreshModelCache();
}

void BoneMapper::addPhysicsBone(const std::string &bone_name) {
  if (bones_by_name_.contains(bone_name) &&
      physics_bones_.insert(bone_name).second) {
    physics_bones_order_.push_back(bone_name);
    canonicalizePhysicsBoneOrder();
  }
}

void BoneMapper::removePhysicsBone(const std::string &bone_name) {
  if (physics_bones_.erase(bone_name) == 0) {
    return;
  }
  physics_bones_order_.erase(std::remove(physics_bones_order_.begin(),
                                         physics_bones_order_.end(), bone_name),
                             physics_bones_order_.end());
}

bool BoneMapper::isPhysicsBone(const std::string &bone_name) const {
  return physics_bones_.contains(bone_name);
}

void BoneMapper::addCollisionRoot(const std::string &bone_name) {
  if (bones_by_name_.contains(bone_name)) {
    collision_roots_.insert(bone_name);
  }
}

void BoneMapper::removeCollisionRoot(const std::string &bone_name) {
  collision_roots_.erase(bone_name);
}

void BoneMapper::clearCollisionRoots() { collision_roots_.clear(); }

bool BoneMapper::isCollisionRoot(const std::string &bone_name) const {
  return collision_roots_.contains(bone_name);
}

std::set<std::string> BoneMapper::getExpandedCollisionBones() const {
  if (collision_roots_.empty()) {
    return {};
  }
  std::map<std::string, std::vector<std::string>> children;
  for (const auto &bone : all_bones_) {
    if (bone.has_parent) {
      children[bone.parent].push_back(bone.name);
    }
  }
  std::set<std::string> reachable;
  std::queue<std::string> pending;
  for (const auto &root : collision_roots_) {
    pending.push(root);
  }
  while (!pending.empty()) {
    const std::string name = pending.front();
    pending.pop();
    if (!reachable.insert(name).second) {
      continue;
    }
    auto it = children.find(name);
    if (it != children.end()) {
      for (const auto &child : it->second) {
        pending.push(child);
      }
    }
  }
  std::set<std::string> result;
  for (const auto &bone : all_bones_) {
    if (reachable.contains(bone.name) && !hasPhysicsAncestorOrSelf(bone.name)) {
      result.insert(bone.name);
    }
  }
  return result;
}

std::set<std::string> BoneMapper::animationInputDependencyBones() const {
  std::set<std::string> dependencies;
  const auto addAncestorClosure = [this,
                                   &dependencies](const std::string &seed) {
    std::set<std::string> path;
    auto bone = bones_by_name_.find(seed);
    while (bone != bones_by_name_.end() && path.insert(bone->first).second) {
      dependencies.insert(bone->first);
      if (!bone->second.has_parent || bone->second.parent.empty()) {
        break;
      }
      bone = bones_by_name_.find(bone->second.parent);
    }
  };
  for (const auto &name : physics_bones_order_) {
    addAncestorClosure(name);
  }
  for (const auto &name : getExpandedCollisionBones()) {
    addAncestorClosure(name);
  }
  return dependencies;
}

bool BoneMapper::hasPhysicsAncestorOrSelf(const std::string &bone_name) const {
  std::set<std::string> visited;
  auto it = bones_by_name_.find(bone_name);
  while (it != bones_by_name_.end() && visited.insert(it->second.name).second) {
    if (physics_bones_.contains(it->second.name)) {
      return true;
    }
    if (!it->second.has_parent) {
      break;
    }
    it = bones_by_name_.find(it->second.parent);
  }
  return false;
}

void BoneMapper::resetModelState() {
  clearModelState();
  refreshModelCache();
}

void BoneMapper::clearModelState() {
  physics_bones_.clear();
  physics_bones_order_.clear();
  collision_roots_.clear();
  bone_to_particle_.clear();
  bone_to_model_index_.clear();
  per_bone_configs_.clear();
}

void BoneMapper::refreshModelCache() {
  bones_by_name_.clear();
  bone_to_model_index_.clear();
  for (std::size_t index = 0; index < all_bones_.size(); ++index) {
    const auto &bone = all_bones_[index];
    if (!bone.name.empty()) {
      bones_by_name_[bone.name] = bone;
      bone_to_model_index_.try_emplace(bone.name, index);
    }
  }
  rest_poses_ = BonePoseCalculator::calculate(all_bones_, nullptr, 0.0);
}

int BoneMapper::hierarchyDepth(const std::string &bone_name) const {
  int depth = 0;
  std::set<std::string> visited;
  auto it = bones_by_name_.find(bone_name);
  while (it != bones_by_name_.end() && it->second.has_parent &&
         visited.insert(it->second.name).second) {
    ++depth;
    it = bones_by_name_.find(it->second.parent);
  }
  return depth;
}

void BoneMapper::canonicalizePhysicsBoneOrder() {
  std::sort(physics_bones_order_.begin(), physics_bones_order_.end(),
            [this](const std::string &a, const std::string &b) {
              const int depth_a = hierarchyDepth(a);
              const int depth_b = hierarchyDepth(b);
              if (depth_a != depth_b) {
                return depth_a < depth_b;
              }
              const std::size_t index_a = getModelBoneIndex(a);
              const std::size_t index_b = getModelBoneIndex(b);
              if (index_a != index_b) {
                return index_a < index_b;
              }
              return a < b;
            });
}

std::array<double, 3>
BoneMapper::getWorldPivot(const std::string &bone_name) const {
  auto it = rest_poses_.find(bone_name);
  if (it == rest_poses_.end()) {
    return {0.0, 0.0, 0.0};
  }
  return it->second.world_position;
}

void BoneMapper::buildParticleMapping() {
  refreshModelCache();
  canonicalizePhysicsBoneOrder();
  bone_to_particle_.clear();
  int idx = 0;
  for (const auto &name : physics_bones_order_) {
    bone_to_particle_[name] = idx++;
  }
}

int BoneMapper::getParticleIndex(const std::string &bone_name) const {
  auto it = bone_to_particle_.find(bone_name);
  return it == bone_to_particle_.end() ? -1 : it->second;
}

std::size_t BoneMapper::getModelBoneIndex(const std::string &bone_name) const {
  auto it = bone_to_model_index_.find(bone_name);
  return it == bone_to_model_index_.end()
             ? std::numeric_limits<std::size_t>::max()
             : it->second;
}

std::vector<BoneMapper::ConstraintDef>
BoneMapper::generateChainConstraints() const {
  std::vector<ConstraintDef> constraints;
  for (const auto &bone_name : physics_bones_order_) {
    auto it = bones_by_name_.find(bone_name);
    if (it == bones_by_name_.end()) {
      continue;
    }
    const auto &bone = it->second;
    if (bone.has_parent && physics_bones_.contains(bone.parent)) {
      const auto posA = getWorldPivot(bone.parent);
      const auto posB = getWorldPivot(bone_name);
      const double dx = posB[0] - posA[0];
      const double dy = posB[1] - posA[1];
      const double dz = posB[2] - posA[2];
      double restLen = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (restLen < 1e-6) {
        restLen = 0.0;
      }
      constraints.push_back(ConstraintDef{
          bone.parent, bone_name, restLen, getEffectiveCompliance(bone_name),
          getEffectiveDampingCompliance(bone_name)});
    }
  }

  std::map<std::string, std::vector<std::string>> childrenByParent;
  for (const auto &bone_name : physics_bones_order_) {
    auto it = bones_by_name_.find(bone_name);
    if (it != bones_by_name_.end() && it->second.has_parent &&
        physics_bones_.contains(it->second.parent)) {
      childrenByParent[it->second.parent].push_back(bone_name);
    }
  }
  for (const auto &parent : physics_bones_order_) {
    auto children_it = childrenByParent.find(parent);
    if (children_it == childrenByParent.end()) {
      continue;
    }
    const auto &children = children_it->second;
    for (std::size_t i = 0; i < children.size(); ++i) {
      for (std::size_t j = i + 1; j < children.size(); ++j) {
        const auto &a = children[i];
        const auto &b = children[j];
        const auto posA = getWorldPivot(a);
        const auto posB = getWorldPivot(b);
        const double dx = posB[0] - posA[0];
        const double dy = posB[1] - posA[1];
        const double dz = posB[2] - posA[2];
        double restLen = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (restLen < 1e-6) {
          restLen = 0.0;
        }
        constraints.push_back(ConstraintDef{
            a, b, restLen,
            std::min(getEffectiveCompliance(a), getEffectiveCompliance(b)),
            std::min(getEffectiveDampingCompliance(a),
                     getEffectiveDampingCompliance(b))});
      }
    }
  }
  return constraints;
}

std::vector<BoneMapper::CrossSpringDef>
BoneMapper::generateCrossSpringConstraints() const {
  std::vector<CrossSpringDef> constraints;
  if (!config_.enable_angle_constraints) {
    return constraints;
  }
  for (const auto &childName : physics_bones_order_) {
    auto childIt = bones_by_name_.find(childName);
    if (childIt == bones_by_name_.end() || !childIt->second.has_parent ||
        !physics_bones_.contains(childIt->second.parent)) {
      continue;
    }
    auto jointIt = bones_by_name_.find(childIt->second.parent);
    if (jointIt == bones_by_name_.end() || !jointIt->second.has_parent ||
        !physics_bones_.contains(jointIt->second.parent)) {
      continue;
    }
    const auto a = getWorldPivot(jointIt->second.parent);
    const auto b = getWorldPivot(jointIt->second.name);
    const auto c = getWorldPivot(childName);
    const double ux = a[0] - b[0], uy = a[1] - b[1], uz = a[2] - b[2];
    const double vx = c[0] - b[0], vy = c[1] - b[1], vz = c[2] - b[2];
    const double lu = std::sqrt(ux * ux + uy * uy + uz * uz);
    const double lv = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (lu < 1e-6 || lv < 1e-6) {
      continue;
    }
    const double cosine = std::max(
        -1.0, std::min(1.0, (ux * vx + uy * vy + uz * vz) / (lu * lv)));
    const double restAngle = std::acos(cosine);
    const double deviation = getEffectiveMaxBendDegrees(jointIt->second.name) *
                             3.14159265358979323846 / 180.0;
    const double minAngle = std::max(0.0, restAngle - deviation);
    const double maxAngle =
        std::min(3.14159265358979323846, restAngle + deviation);
    constraints.push_back(CrossSpringDef{
        jointIt->second.parent, childName, spanForAngle(lu, lv, minAngle),
        spanForAngle(lu, lv, maxAngle),
        getEffectiveBendCompliance(jointIt->second.name), c[0] - a[0],
        c[1] - a[1], c[2] - a[2]});
  }
  return constraints;
}

std::vector<BoneMapper::AngleConstraintDef>
BoneMapper::generateAngleConstraints() const {
  std::vector<AngleConstraintDef> constraints;
  if (!config_.enable_angle_constraints) {
    return constraints;
  }
  for (const auto &childName : physics_bones_order_) {
    auto childIt = bones_by_name_.find(childName);
    if (childIt == bones_by_name_.end() || !childIt->second.has_parent ||
        !physics_bones_.contains(childIt->second.parent)) {
      continue;
    }
    auto jointIt = bones_by_name_.find(childIt->second.parent);
    if (jointIt == bones_by_name_.end() || !jointIt->second.has_parent ||
        !physics_bones_.contains(jointIt->second.parent)) {
      continue;
    }
    const auto a = getWorldPivot(jointIt->second.parent);
    const auto b = getWorldPivot(jointIt->second.name);
    const auto c = getWorldPivot(childName);
    const double ux = a[0] - b[0], uy = a[1] - b[1], uz = a[2] - b[2];
    const double vx = c[0] - b[0], vy = c[1] - b[1], vz = c[2] - b[2];
    const double lu = std::sqrt(ux * ux + uy * uy + uz * uz);
    const double lv = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (lu < 1e-6 || lv < 1e-6) {
      continue;
    }
    const double cosine = std::max(
        -1.0, std::min(1.0, (ux * vx + uy * vy + uz * vz) / (lu * lv)));
    const double restAngle = std::acos(cosine);
    const double deviation = getEffectiveMaxBendDegrees(jointIt->second.name) *
                             3.14159265358979323846 / 180.0;
    const double normalX = uy * vz - uz * vy;
    const double normalY = uz * vx - ux * vz;
    const double normalZ = ux * vy - uy * vx;
    constraints.push_back(AngleConstraintDef{
        jointIt->second.parent, jointIt->second.name, childName,
        std::max(0.0, restAngle - deviation),
        std::min(3.14159265358979323846, restAngle + deviation),
        getEffectiveBendCompliance(jointIt->second.name), normalX, normalY,
        normalZ});
  }
  return constraints;
}

double BoneMapper::spanForAngle(double length_a, double length_b,
                                double angle) {
  const double squared = length_a * length_a + length_b * length_b -
                         2.0 * length_a * length_b * std::cos(angle);
  return std::sqrt(std::max(0.0, squared));
}

BoneMapper::BonePhysicsConfig *
BoneMapper::getBoneConfig(const std::string &bone_name) {
  auto it = per_bone_configs_.find(bone_name);
  return it == per_bone_configs_.end() ? nullptr : &it->second;
}

const BoneMapper::BonePhysicsConfig *
BoneMapper::getBoneConfig(const std::string &bone_name) const {
  auto it = per_bone_configs_.find(bone_name);
  return it == per_bone_configs_.end() ? nullptr : &it->second;
}

void BoneMapper::setBoneConfig(const std::string &bone_name,
                               const BonePhysicsConfig *cfg) {
  if (bone_name.empty() || !bones_by_name_.contains(bone_name)) {
    return;
  }
  if (cfg == nullptr) {
    per_bone_configs_.erase(bone_name);
  } else {
    per_bone_configs_[bone_name] = *cfg;
  }
}

double BoneMapper::getEffectiveMass(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  return effectiveNonNegative(bc ? bc->particle_mass : std::nullopt,
                              config_.particle_mass, 1.0);
}

double BoneMapper::getEffectiveCompliance(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  return effectiveNonNegative(bc ? bc->compliance : std::nullopt,
                              config_.compliance, 0.0);
}

double
BoneMapper::getEffectiveDampingCompliance(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  return effectiveNonNegative(bc ? bc->damping_compliance : std::nullopt,
                              config_.damping_compliance, 0.0);
}

double
BoneMapper::getEffectiveMaxBendDegrees(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  const double value = (bc && bc->max_bend_degrees) ? *bc->max_bend_degrees
                                                    : config_.max_bend_degrees;
  return std::isfinite(value) ? std::max(0.0, std::min(180.0, value)) : 180.0;
}

double
BoneMapper::getEffectiveBendCompliance(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  const double value = (bc && bc->bend_compliance) ? *bc->bend_compliance
                                                   : config_.bend_compliance;
  return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

double
BoneMapper::getEffectiveAnimPullCompliance(const std::string &bone_name) const {
  if (config_.enable_real_gravity_field) {
    return 0.0;
  }
  const auto *bc = getBoneConfig(bone_name);
  return effectiveNonNegative(bc ? bc->animation_pull_compliance : std::nullopt,
                              config_.animation_pull_compliance, 0.0);
}

double
BoneMapper::getEffectiveGravityScale(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  return effectiveNonNegative(bc ? bc->gravity_scale : std::nullopt, 1.0, 1.0);
}

double
BoneMapper::getEffectiveWindInfluence(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  return effectiveNonNegative(bc ? bc->wind_influence : std::nullopt, 1.0, 1.0);
}

double BoneMapper::getEffectiveTurbulenceInfluence(
    const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  return effectiveNonNegative(bc ? bc->turbulence_influence : std::nullopt, 1.0,
                              1.0);
}

std::array<double, 3> BoneMapper::getEffectiveRigidBodyMaxBendDegrees(
    const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  auto angle = [](const std::optional<double> &override_value,
                  double fallback) {
    const double value = override_value ? *override_value : fallback;
    return std::isfinite(value) ? std::max(0.0, std::min(180.0, value)) : 180.0;
  };
  return {angle(bc ? bc->rigid_body_max_bend_x_degrees : std::nullopt,
                config_.rigid_body_max_bend_x_degrees),
          angle(bc ? bc->rigid_body_max_bend_y_degrees : std::nullopt,
                config_.rigid_body_max_bend_y_degrees),
          angle(bc ? bc->rigid_body_max_bend_z_degrees : std::nullopt,
                config_.rigid_body_max_bend_z_degrees)};
}

bool BoneMapper::isFixedBone(const std::string &bone_name) const {
  const auto *bc = getBoneConfig(bone_name);
  if (bc && bc->fixed.has_value()) {
    return *bc->fixed;
  }
  if (config_.enable_real_gravity_field) {
    return false;
  }
  auto it = bones_by_name_.find(bone_name);
  if (it == bones_by_name_.end() || !it->second.has_parent) {
    return true;
  }
  return !physics_bones_.contains(it->second.parent);
}

double
BoneMapper::effectiveNonNegative(const std::optional<double> &override_value,
                                 double fallback, double default_value) {
  if (override_value && std::isfinite(*override_value)) {
    return std::max(0.0, *override_value);
  }
  return std::isfinite(fallback) ? std::max(0.0, fallback) : default_value;
}

}
