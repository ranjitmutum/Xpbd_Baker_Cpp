#include "xpbd/baker/body_collider_cache.hpp"

#include "xpbd/baker/cube_geometry.hpp"
#include "xpbd/baker/rotation_util.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace xpbd::baker {
namespace {


constexpr int kFaceVertices[6][4] = {
    {0, 2, 6, 4},
    {1, 5, 7, 3},
    {0, 4, 5, 1},
    {2, 3, 7, 6},
    {0, 1, 3, 2},
    {4, 6, 7, 5},
};

bool pointInAabb(const std::array<double, 6> &aabb, double x, double y,
                 double z, double margin) {
  return x >= aabb[0] - margin && x <= aabb[1] + margin &&
         y >= aabb[2] - margin && y <= aabb[3] + margin &&
         z >= aabb[4] - margin && z <= aabb[5] + margin;
}

using Collider = BodyColliderCache::Collider;
using AabbGetter = const Collider::Aabb &(Collider::*)() const;

void queryAabbCandidates(const std::vector<Collider> &colliders,
                         const std::vector<std::size_t> &x_order,
                         const Collider::Aabb &query, AabbGetter getter,
                         std::vector<std::size_t> &output) {
  output.clear();
  for (const std::size_t index : x_order) {
    const Collider::Aabb &bounds = (colliders[index].*getter)();
    if (bounds[0] > query[1]) {
      break;
    }
    if (bounds[1] < query[0] || bounds[3] < query[2] || bounds[2] > query[3] ||
        bounds[5] < query[4] || bounds[4] > query[5]) {
      continue;
    }
    output.push_back(index);
  }
  std::sort(output.begin(), output.end());
}

void rotateVectorExplicit(double qx, double qy, double qz, double qw, double x,
                          double y, double z, double result[3]) {
  const double tx = 2.0 * (qy * z - qz * y);
  const double ty = 2.0 * (qz * x - qx * z);
  const double tz = 2.0 * (qx * y - qy * x);
  result[0] = x + qw * tx + (qy * tz - qz * ty);
  result[1] = y + qw * ty + (qz * tx - qx * tz);
  result[2] = z + qw * tz + (qx * ty - qy * tx);
}

void normalizeQuaternion(const std::array<double, 4> &quaternion,
                         double result[4]) {
  double norm_sq = 0.0;
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(quaternion[static_cast<std::size_t>(i)])) {
      throw std::invalid_argument("collider rotation must be finite");
    }
    norm_sq += quaternion[static_cast<std::size_t>(i)] *
               quaternion[static_cast<std::size_t>(i)];
  }
  if (!std::isfinite(norm_sq) || norm_sq <= 1e-20) {
    throw std::invalid_argument("collider rotation is not invertible");
  }
  const double inv = 1.0 / std::sqrt(norm_sq);
  for (int i = 0; i < 4; ++i) {
    result[i] = quaternion[static_cast<std::size_t>(i)] * inv;
  }
}

void requireFinite3(const std::array<double, 3> &value, const char *label) {
  if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
      !std::isfinite(value[2])) {
    throw std::invalid_argument(std::string(label) + " must be finite");
  }
}

}

BodyColliderCache::Collider::Collider(std::string owner_bone,
                                      std::array<double, 24> bind_vertices,
                                      double epsilon)
    : owner_bone_(std::move(owner_bone)), bind_vertices_(bind_vertices) {
  buildBindPlanes(epsilon);
}

void BodyColliderCache::Collider::buildBindPlanes(double epsilon) {
  double center_x = 0, center_y = 0, center_z = 0;
  for (int vertex = 0; vertex < 8; ++vertex) {
    const double x = bind_vertices_[static_cast<std::size_t>(vertex * 3)];
    const double y = bind_vertices_[static_cast<std::size_t>(vertex * 3 + 1)];
    const double z = bind_vertices_[static_cast<std::size_t>(vertex * 3 + 2)];
    center_x += x;
    center_y += y;
    center_z += z;
    bind_max_radius_ =
        std::max(bind_max_radius_, std::sqrt(x * x + y * y + z * z));
  }
  bind_max_radius_ = std::nextafter(bind_max_radius_ + epsilon,
                                    std::numeric_limits<double>::infinity());
  center_x /= 8.0;
  center_y /= 8.0;
  center_z /= 8.0;
  for (int face = 0; face < 6; ++face) {
    const int a = kFaceVertices[face][0] * 3;
    const int b = kFaceVertices[face][1] * 3;
    const int c = kFaceVertices[face][2] * 3;
    const double ab_x = bind_vertices_[static_cast<std::size_t>(b)] -
                        bind_vertices_[static_cast<std::size_t>(a)];
    const double ab_y = bind_vertices_[static_cast<std::size_t>(b + 1)] -
                        bind_vertices_[static_cast<std::size_t>(a + 1)];
    const double ab_z = bind_vertices_[static_cast<std::size_t>(b + 2)] -
                        bind_vertices_[static_cast<std::size_t>(a + 2)];
    const double ac_x = bind_vertices_[static_cast<std::size_t>(c)] -
                        bind_vertices_[static_cast<std::size_t>(a)];
    const double ac_y = bind_vertices_[static_cast<std::size_t>(c + 1)] -
                        bind_vertices_[static_cast<std::size_t>(a + 1)];
    const double ac_z = bind_vertices_[static_cast<std::size_t>(c + 2)] -
                        bind_vertices_[static_cast<std::size_t>(a + 2)];
    double nx = ab_y * ac_z - ab_z * ac_y;
    double ny = ab_z * ac_x - ab_x * ac_z;
    double nz = ab_x * ac_y - ab_y * ac_x;
    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!std::isfinite(length) || length <= epsilon) {
      throw std::invalid_argument("degenerate cube face");
    }
    nx /= length;
    ny /= length;
    nz /= length;
    const double center_side =
        nx * (center_x - bind_vertices_[static_cast<std::size_t>(a)]) +
        ny * (center_y - bind_vertices_[static_cast<std::size_t>(a + 1)]) +
        nz * (center_z - bind_vertices_[static_cast<std::size_t>(a + 2)]);
    if (center_side > 0) {
      nx = -nx;
      ny = -ny;
      nz = -nz;
    }
    bind_normals_[static_cast<std::size_t>(face * 3)] = nx;
    bind_normals_[static_cast<std::size_t>(face * 3 + 1)] = ny;
    bind_normals_[static_cast<std::size_t>(face * 3 + 2)] = nz;
    bind_constants_[static_cast<std::size_t>(face)] =
        nx * bind_vertices_[static_cast<std::size_t>(a)] +
        ny * bind_vertices_[static_cast<std::size_t>(a + 1)] +
        nz * bind_vertices_[static_cast<std::size_t>(a + 2)];
  }
}

void BodyColliderCache::Collider::update(const BonePoseCalculator::Pose &pose,
                                         bool keep_history) {
  requireFinite3(pose.world_translation, "collider translation");
  if (initialized_ && keep_history) {
    previous_rotation_ = current_rotation_;
    previous_translation_ = current_translation_;
    previous_normals_ = current_normals_;
    previous_constants_ = current_constants_;
    previous_aabb_ = current_aabb_;
  }
  normalizeQuaternion(pose.world_rotation, current_rotation_.data());
  current_translation_ = pose.world_translation;

  current_aabb_[0] = current_aabb_[2] = current_aabb_[4] =
      std::numeric_limits<double>::infinity();
  current_aabb_[1] = current_aabb_[3] = current_aabb_[5] =
      -std::numeric_limits<double>::infinity();
  double rotated[3];
  for (int vertex = 0; vertex < 8; ++vertex) {
    const int offset = vertex * 3;
    RotationUtil::rotateVector(
        current_rotation_.data(),
        bind_vertices_[static_cast<std::size_t>(offset)],
        bind_vertices_[static_cast<std::size_t>(offset + 1)],
        bind_vertices_[static_cast<std::size_t>(offset + 2)], rotated);
    const double x = rotated[0] + current_translation_[0];
    const double y = rotated[1] + current_translation_[1];
    const double z = rotated[2] + current_translation_[2];
    current_vertices_[static_cast<std::size_t>(offset)] = x;
    current_vertices_[static_cast<std::size_t>(offset + 1)] = y;
    current_vertices_[static_cast<std::size_t>(offset + 2)] = z;
    current_aabb_[0] = std::min(current_aabb_[0], x);
    current_aabb_[1] = std::max(current_aabb_[1], x);
    current_aabb_[2] = std::min(current_aabb_[2], y);
    current_aabb_[3] = std::max(current_aabb_[3], y);
    current_aabb_[4] = std::min(current_aabb_[4], z);
    current_aabb_[5] = std::max(current_aabb_[5], z);
  }
  for (int face = 0; face < 6; ++face) {
    const int offset = face * 3;
    RotationUtil::rotateVector(
        current_rotation_.data(),
        bind_normals_[static_cast<std::size_t>(offset)],
        bind_normals_[static_cast<std::size_t>(offset + 1)],
        bind_normals_[static_cast<std::size_t>(offset + 2)], rotated);
    current_normals_[static_cast<std::size_t>(offset)] = rotated[0];
    current_normals_[static_cast<std::size_t>(offset + 1)] = rotated[1];
    current_normals_[static_cast<std::size_t>(offset + 2)] = rotated[2];
    const int vertex = kFaceVertices[face][0] * 3;
    current_constants_[static_cast<std::size_t>(face)] =
        rotated[0] * current_vertices_[static_cast<std::size_t>(vertex)] +
        rotated[1] * current_vertices_[static_cast<std::size_t>(vertex + 1)] +
        rotated[2] * current_vertices_[static_cast<std::size_t>(vertex + 2)];
  }
  if (!initialized_ || !keep_history) {
    previous_rotation_ = current_rotation_;
    previous_translation_ = current_translation_;
    previous_normals_ = current_normals_;
    previous_constants_ = current_constants_;
    previous_aabb_ = current_aabb_;
  }
  for (int axis = 0; axis < 3; ++axis) {
    const auto offset = static_cast<std::size_t>(axis * 2);
    const double previous =
        previous_translation_[static_cast<std::size_t>(axis)];
    const double current = current_translation_[static_cast<std::size_t>(axis)];




    swept_aabb_[offset] = std::min(previous, current) - bind_max_radius_;
    swept_aabb_[offset + 1] = std::max(previous, current) + bind_max_radius_;
  }
  initialized_ = true;
}

double BodyColliderCache::Collider::getBindNormal(int face, int axis) const {
  return bind_normals_[static_cast<std::size_t>(face * 3 + axis)];
}

double BodyColliderCache::Collider::getBindConstant(int face) const {
  return bind_constants_[static_cast<std::size_t>(face)];
}

double BodyColliderCache::Collider::getCurrentNormal(int face, int axis) const {
  return current_normals_[static_cast<std::size_t>(face * 3 + axis)];
}

double BodyColliderCache::Collider::signedDistanceCurrent(int face, double x,
                                                          double y,
                                                          double z) const {
  const int offset = face * 3;
  return current_normals_[static_cast<std::size_t>(offset)] * x +
         current_normals_[static_cast<std::size_t>(offset + 1)] * y +
         current_normals_[static_cast<std::size_t>(offset + 2)] * z -
         current_constants_[static_cast<std::size_t>(face)];
}

double BodyColliderCache::Collider::signedDistancePrevious(int face, double x,
                                                           double y,
                                                           double z) const {
  const int offset = face * 3;
  return previous_normals_[static_cast<std::size_t>(offset)] * x +
         previous_normals_[static_cast<std::size_t>(offset + 1)] * y +
         previous_normals_[static_cast<std::size_t>(offset + 2)] * z -
         previous_constants_[static_cast<std::size_t>(face)];
}

bool BodyColliderCache::Collider::containsCurrent(double x, double y, double z,
                                                  double skin) const {
  if (!pointInAabb(current_aabb_, x, y, z, skin)) {
    return false;
  }
  for (int face = 0; face < 6; ++face) {
    if (signedDistanceCurrent(face, x, y, z) > skin) {
      return false;
    }
  }
  return true;
}

bool BodyColliderCache::Collider::containsPrevious(double x, double y, double z,
                                                   double skin) const {
  if (!pointInAabb(previous_aabb_, x, y, z, skin)) {
    return false;
  }
  for (int face = 0; face < 6; ++face) {
    if (signedDistancePrevious(face, x, y, z) > skin) {
      return false;
    }
  }
  return true;
}

void BodyColliderCache::Collider::toCurrentBind(double x, double y, double z,
                                                double result[3]) const {
  const double px = x - current_translation_[0];
  const double py = y - current_translation_[1];
  const double pz = z - current_translation_[2];
  rotateVectorExplicit(-current_rotation_[0], -current_rotation_[1],
                       -current_rotation_[2], current_rotation_[3], px, py, pz,
                       result);
}

void BodyColliderCache::Collider::toPreviousBind(double x, double y, double z,
                                                 double result[3]) const {
  const double px = x - previous_translation_[0];
  const double py = y - previous_translation_[1];
  const double pz = z - previous_translation_[2];
  rotateVectorExplicit(-previous_rotation_[0], -previous_rotation_[1],
                       -previous_rotation_[2], previous_rotation_[3], px, py,
                       pz, result);
}

void BodyColliderCache::Collider::fromCurrentBind(double x, double y, double z,
                                                  double result[3]) const {
  RotationUtil::rotateVector(current_rotation_.data(), x, y, z, result);
  result[0] += current_translation_[0];
  result[1] += current_translation_[1];
  result[2] += current_translation_[2];
}

void BodyColliderCache::Collider::fromPreviousBind(double x, double y, double z,
                                                   double result[3]) const {
  RotationUtil::rotateVector(previous_rotation_.data(), x, y, z, result);
  result[0] += previous_translation_[0];
  result[1] += previous_translation_[1];
  result[2] += previous_translation_[2];
}

BodyColliderCache::BodyColliderCache(
    const std::vector<loader::Bone> &all_bones,
    const std::set<std::string> &collision_bones) {
  const double model_scale = estimateModelScale(all_bones);
  epsilon_ = std::max(1e-9, model_scale * 1e-10);
  if (collision_bones.empty()) {
    return;
  }
  for (const auto &bone : all_bones) {
    if (bone.name.empty() || !collision_bones.contains(bone.name) ||
        bone.cubes.empty()) {
      continue;
    }
    for (const auto &cube : bone.cubes) {
      const auto size = CubeGeometry::effectiveSize(cube);
      if (size[0] <= epsilon_ || size[1] <= epsilon_ || size[2] <= epsilon_) {
        degenerate_cube_count_++;
        continue;
      }
      colliders_.emplace_back(bone.name, CubeGeometry::bindVertices(cube),
                              epsilon_);
    }
  }
}

void BodyColliderCache::initialize(
    const std::map<std::string, BonePoseCalculator::Pose> &poses) {
  update(poses, false, true);
}

void BodyColliderCache::advance(
    const std::map<std::string, BonePoseCalculator::Pose> &poses,
    bool history_continuous) {
  update(poses, history_continuous, false);
}

void BodyColliderCache::setAuditPose(
    const std::map<std::string, BonePoseCalculator::Pose> &poses) {
  update(poses, false, true);
}

void BodyColliderCache::update(
    const std::map<std::string, BonePoseCalculator::Pose> &poses,
    bool history_continuous, bool initialize_history) {
  for (auto &collider : colliders_) {
    auto it = poses.find(collider.ownerBone());
    if (it == poses.end()) {
      throw std::invalid_argument("missing pose for collision bone: " +
                                  collider.ownerBone());
    }
    collider.update(it->second, history_continuous && !initialize_history);
  }
  sweep_continuous_ = history_continuous && !initialize_history;
  rebuildCandidateIndices();
}

bool BodyColliderCache::containsCurrent(double x, double y, double z,
                                        double skin) const {
  const double maximum_x = x + skin;
  for (const std::size_t index : current_x_order_) {
    const Collider &collider = colliders_[index];
    if (collider.currentAabb()[0] > maximum_x) {
      break;
    }
    if (collider.containsCurrent(x, y, z, skin)) {
      return true;
    }
  }
  return false;
}

bool BodyColliderCache::containsPrevious(double x, double y, double z,
                                         double skin) const {
  const double maximum_x = x + skin;
  for (const std::size_t index : previous_x_order_) {
    const Collider &collider = colliders_[index];
    if (collider.previousAabb()[0] > maximum_x) {
      break;
    }
    if (collider.containsPrevious(x, y, z, skin)) {
      return true;
    }
  }
  return false;
}

void BodyColliderCache::queryCurrentCandidates(
    double x, double y, double z, double skin,
    std::vector<std::size_t> &output) const {
  const Collider::Aabb query{x - skin, x + skin, y - skin,
                             y + skin, z - skin, z + skin};
  queryAabbCandidates(colliders_, current_x_order_, query,
                      &Collider::currentAabb, output);
}

void BodyColliderCache::queryPreviousCandidates(
    double x, double y, double z, double skin,
    std::vector<std::size_t> &output) const {
  const Collider::Aabb query{x - skin, x + skin, y - skin,
                             y + skin, z - skin, z + skin};
  queryAabbCandidates(colliders_, previous_x_order_, query,
                      &Collider::previousAabb, output);
}

void BodyColliderCache::querySweepCandidates(
    double previous_x, double previous_y, double previous_z, double current_x,
    double current_y, double current_z, double skin,
    std::vector<std::size_t> &output) const {
  if (!sweep_continuous_) {
    output.resize(colliders_.size());
    std::iota(output.begin(), output.end(), std::size_t{0});
    return;
  }
  const Collider::Aabb query{std::min(previous_x, current_x) - skin,
                             std::max(previous_x, current_x) + skin,
                             std::min(previous_y, current_y) - skin,
                             std::max(previous_y, current_y) + skin,
                             std::min(previous_z, current_z) - skin,
                             std::max(previous_z, current_z) + skin};
  queryAabbCandidates(colliders_, swept_x_order_, query, &Collider::sweptAabb,
                      output);
}

void BodyColliderCache::rebuildCandidateIndices() {
  const auto rebuild = [this](std::vector<std::size_t> &order,
                              AabbGetter getter) {
    order.resize(colliders_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(),
              [this, getter](std::size_t lhs, std::size_t rhs) {
                const double lhs_min = (colliders_[lhs].*getter)()[0];
                const double rhs_min = (colliders_[rhs].*getter)()[0];
                return lhs_min < rhs_min || (lhs_min == rhs_min && lhs < rhs);
              });
  };
  rebuild(current_x_order_, &Collider::currentAabb);
  rebuild(previous_x_order_, &Collider::previousAabb);
  rebuild(swept_x_order_, &Collider::sweptAabb);
}

double
BodyColliderCache::estimateModelScale(const std::vector<loader::Bone> &bones) {
  double scale = 1.0;
  for (const auto &bone : bones) {
    for (const auto &cube : bone.cubes) {
      for (int axis = 0; axis < 3; ++axis) {
        if (std::isfinite(cube.origin[axis])) {
          scale = std::max(scale, std::abs(cube.origin[axis]));
        }
        if (std::isfinite(cube.size[axis])) {
          scale = std::max(scale, std::abs(cube.size[axis]));
        }
      }
      if (std::isfinite(cube.inflate)) {
        scale = std::max(scale, std::abs(cube.inflate));
      }
    }
  }
  return scale;
}

}
