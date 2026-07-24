#include "xpbd/baker/rigid_body_collision_auditor.hpp"

#include "xpbd/baker/cube_geometry.hpp"
#include "xpbd/baker/rotation_util.hpp"

#if defined(XPBD_HAS_X86_SIMD)
#include "../core/simd_kernels.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace {

#if !defined(XPBD_HAS_X86_SIMD)
double boxProjectionOverlapFallback(const double *first_xyz_soa,
                                    const double *second_xyz_soa,
                                    const double *axis) noexcept {
  double first_min = std::numeric_limits<double>::infinity();
  double first_max = -std::numeric_limits<double>::infinity();
  double second_min = std::numeric_limits<double>::infinity();
  double second_max = -std::numeric_limits<double>::infinity();
  for (std::size_t vertex = 0; vertex < 8; ++vertex) {
    const double first_projection = first_xyz_soa[vertex] * axis[0] +
                                    first_xyz_soa[8 + vertex] * axis[1] +
                                    first_xyz_soa[16 + vertex] * axis[2];
    const double second_projection = second_xyz_soa[vertex] * axis[0] +
                                     second_xyz_soa[8 + vertex] * axis[1] +
                                     second_xyz_soa[16 + vertex] * axis[2];
    first_min = std::min(first_min, first_projection);
    first_max = std::max(first_max, first_projection);
    second_min = std::min(second_min, second_projection);
    second_max = std::max(second_max, second_projection);
  }
  return std::min(first_max - second_min, second_max - first_min);
}
#endif

}

namespace xpbd::baker {

RigidBodyCollisionAuditor::RigidBodyCollisionAuditor(
    const std::vector<loader::Bone> &all_bones,
    const std::map<std::string, rigidbody::BodyDefinition> &physics_bodies,
    const std::set<std::string> &collision_bones, double unit_scale,
    double maximum_safe_penetration, bool ground_collision_enabled)
    : unit_scale_(unit_scale),
      maximum_safe_penetration_(maximum_safe_penetration),
      ground_collision_enabled_(ground_collision_enabled) {
#if defined(XPBD_HAS_X86_SIMD)
  projection_overlap_kernel_ =
      core::detail::selectedSimdKernelTable().box_projection_overlap;
#else
  projection_overlap_kernel_ = boxProjectionOverlapFallback;
#endif
  tolerance_ = std::max(1e-9, estimateScale(all_bones) * 1e-10);
  physics_cubes_ = collectCompiled(physics_bodies, unit_scale);
  collision_cubes_ = collect(all_bones, collision_bones, tolerance_);
  linked_physics_body_pairs_ =
      collectLinkedPhysicsBodyPairs(all_bones, physics_bodies);
  const double physicsFeature = minimumFeature(physics_cubes_);
  const double collisionFeature = minimumFeature(collision_cubes_);
  if (physicsFeature > 0.0 && collisionFeature > 0.0) {
    minimum_collider_feature_ = std::min(physicsFeature, collisionFeature);
  } else {
    minimum_collider_feature_ =
        physicsFeature > 0.0 ? physicsFeature : collisionFeature;
  }
  maximum_collider_radius_ =
      std::max(maximumRadius(physics_cubes_), maximumRadius(collision_cubes_));
}

RigidBodyCollisionAuditor::AuditResult RigidBodyCollisionAuditor::audit(
    const std::map<std::string, BonePoseCalculator::Pose> &poses) const {
  AuditResult result;
  if (physics_cubes_.empty()) {
    return result;
  }
  const auto physics = transform(physics_cubes_, poses);
  const auto collisions = transform(collision_cubes_, poses);
  result.counters.all_possible_pairs =
      physics.size() * (physics.size() - 1) / 2 +
      physics.size() * collisions.size();
  const auto physics_candidates = sweepSelfCandidates(physics);
  const auto collision_candidates = sweepCrossCandidates(physics, collisions);
  result.counters.broad_phase_candidates =
      physics_candidates.size() + collision_candidates.size();
  const auto recordPenetration =
      [&result](double penetration, const std::string &first,
                const std::string &second) {
        if (penetration > result.maximum_penetration) {
          result.maximum_penetration = penetration;
          result.worst_pair = std::pair{first, second};
        }
      };
  if (ground_collision_enabled_) {
    for (const auto &body : physics) {
      const double penetration = std::max(0.0, -minimumSoaY(body.vertices_soa));
      const double world_penetration = penetration * unit_scale_;
      recordPenetration(world_penetration, body.body_name, "__ground__");
      if (world_penetration > maximum_safe_penetration_ + 1e-9) {
        result.unsafe = true;
      }
    }
  }




  for (const auto &[first, second] : physics_candidates) {
    const auto &body_a = physics[first];
    const auto &body_b = physics[second];
    if (body_a.body_name == body_b.body_name) {
      continue;
    }
    if (body_a.motion_type != rigidbody::MotionType::Dynamic &&
        body_b.motion_type != rigidbody::MotionType::Dynamic) {
      continue;
    }
    if (linked_physics_body_pairs_.contains(
            canonicalPair(body_a.body_name, body_b.body_name))) {
      continue;
    }
    ++result.counters.sat_calls;
    const double pen =
        penetration(body_a, body_b, tolerance_, projection_overlap_kernel_);
    if (pen <= tolerance_) {
      continue;
    }
    const double world_penetration = pen * unit_scale_;
    recordPenetration(world_penetration, body_a.body_name, body_b.body_name);
    if (world_penetration > maximum_safe_penetration_ + 1e-9) {
      result.unsafe = true;
    }
  }
  for (const auto &[body_index, collider_index] : collision_candidates) {
    const auto &body = physics[body_index];
    const auto &collider = collisions[collider_index];
    ++result.counters.sat_calls;
    const double pen =
        penetration(body, collider, tolerance_, projection_overlap_kernel_);
    if (pen <= tolerance_) {
      continue;
    }
    const double world_penetration = pen * unit_scale_;
    recordPenetration(world_penetration, body.body_name, collider.body_name);
    if (world_penetration > maximum_safe_penetration_ + 1e-9) {
      result.unsafe = true;
    }
  }
  return result;
}

double RigidBodyCollisionAuditor::maximumVertexTravel(
    const std::map<std::string, BonePoseCalculator::Pose> &from,
    const std::map<std::string, BonePoseCalculator::Pose> &to) const {
  const auto maximumFor = [&](const std::vector<CubeBinding> &bindings) {
    const auto first = transform(bindings, from);
    const auto second = transform(bindings, to);
    double maximum = 0.0;
    for (std::size_t box = 0; box < first.size(); ++box) {
      for (int vertex = 0; vertex < 8; ++vertex) {
        const double dx =
            second[box].vertices_soa[static_cast<std::size_t>(vertex)] -
            first[box].vertices_soa[static_cast<std::size_t>(vertex)];
        const double dy =
            second[box].vertices_soa[8u + static_cast<std::size_t>(vertex)] -
            first[box].vertices_soa[8u + static_cast<std::size_t>(vertex)];
        const double dz =
            second[box].vertices_soa[16u + static_cast<std::size_t>(vertex)] -
            first[box].vertices_soa[16u + static_cast<std::size_t>(vertex)];
        maximum = std::max(maximum, std::sqrt(dx * dx + dy * dy + dz * dz));
      }
    }
    return maximum;
  };
  return std::max(maximumFor(physics_cubes_), maximumFor(collision_cubes_));
}

std::vector<RigidBodyCollisionAuditor::CubeBinding>
RigidBodyCollisionAuditor::collectCompiled(
    const std::map<std::string, rigidbody::BodyDefinition> &bodies,
    double unit_scale) {
  std::vector<CubeBinding> result;
  if (bodies.empty()) {
    return result;
  }
  for (const auto &[name, def] : bodies) {
    for (const auto &box : def.boxes) {
      CubeBinding binding;
      binding.bone_name = name;
      binding.motion_type = def.motion_type;
      binding.use_affine_pose = false;
      for (int vertex = 0; vertex < 8; ++vertex) {
        std::array<double, 3> corner{
            (vertex & 1) == 0 ? -box.half_extents[0] : box.half_extents[0],
            (vertex & 2) == 0 ? -box.half_extents[1] : box.half_extents[1],
            (vertex & 4) == 0 ? -box.half_extents[2] : box.half_extents[2]};
        const auto rotated =
            RotationUtil::rotateVector(box.local_transform.rotation, corner);
        const int offset = vertex * 3;
        for (int axis = 0; axis < 3; ++axis) {
          binding.bind_vertices[static_cast<std::size_t>(offset + axis)] =
              (rotated[static_cast<std::size_t>(axis)] +
               box.local_transform
                   .translation[static_cast<std::size_t>(axis)]) /
              unit_scale;
        }
      }
      result.push_back(std::move(binding));
    }
  }
  return result;
}

std::vector<RigidBodyCollisionAuditor::CubeBinding>
RigidBodyCollisionAuditor::collect(const std::vector<loader::Bone> &bones,
                                   const std::set<std::string> &included,
                                   double tolerance) {
  std::vector<CubeBinding> result;
  if (included.empty()) {
    return result;
  }
  for (const auto &bone : bones) {
    if (bone.name.empty() || !included.contains(bone.name) ||
        bone.cubes.empty()) {
      continue;
    }
    for (const auto &cube : bone.cubes) {
      const auto size = CubeGeometry::effectiveSize(cube);
      if (size[0] <= tolerance || size[1] <= tolerance ||
          size[2] <= tolerance) {
        continue;
      }
      CubeBinding binding;
      binding.bone_name = bone.name;
      binding.motion_type = rigidbody::MotionType::Kinematic;
      binding.bind_vertices = CubeGeometry::bindVertices(cube);
      binding.use_affine_pose = true;
      result.push_back(std::move(binding));
    }
  }
  return result;
}

std::vector<RigidBodyCollisionAuditor::WorldBox>
RigidBodyCollisionAuditor::transform(
    const std::vector<CubeBinding> &bindings,
    const std::map<std::string, BonePoseCalculator::Pose> &poses) {
  std::vector<WorldBox> result;
  result.reserve(bindings.size());
  for (const auto &binding : bindings) {
    auto pose_it = poses.find(binding.bone_name);
    if (pose_it == poses.end()) {
      throw std::invalid_argument("missing final audit pose for bone: " +
                                  binding.bone_name);
    }
    WorldBox box;
    box.body_name = binding.bone_name;
    box.motion_type = binding.motion_type;
    box.aabb_min.fill(std::numeric_limits<double>::infinity());
    box.aabb_max.fill(-std::numeric_limits<double>::infinity());
    for (int vertex = 0; vertex < 8; ++vertex) {
      const int offset = vertex * 3;
      std::array<double, 3> transformed{};
      if (binding.use_affine_pose) {
        CubeGeometry::transformPoint(
            pose_it->second,
            binding.bind_vertices[static_cast<std::size_t>(offset)],
            binding.bind_vertices[static_cast<std::size_t>(offset + 1)],
            binding.bind_vertices[static_cast<std::size_t>(offset + 2)],
            transformed.data(), 0);
      } else {
        transformed = RotationUtil::rotateVector(
            pose_it->second.world_rotation,
            {binding.bind_vertices[static_cast<std::size_t>(offset)],
             binding.bind_vertices[static_cast<std::size_t>(offset + 1)],
             binding.bind_vertices[static_cast<std::size_t>(offset + 2)]});
        for (std::size_t axis = 0; axis < 3; ++axis) {
          transformed[axis] += pose_it->second.world_position[axis];
        }
      }
      for (int axis = 0; axis < 3; ++axis) {
        const auto coordinate = transformed[static_cast<std::size_t>(axis)];
        box.vertices_soa[static_cast<std::size_t>(axis * 8 + vertex)] =
            coordinate;
        box.aabb_min[static_cast<std::size_t>(axis)] =
            std::min(box.aabb_min[static_cast<std::size_t>(axis)], coordinate);
        box.aabb_max[static_cast<std::size_t>(axis)] =
            std::max(box.aabb_max[static_cast<std::size_t>(axis)], coordinate);
      }
    }
    box.axes = axesFromSoaVertices(box.vertices_soa);
    result.push_back(std::move(box));
  }
  return result;
}

std::vector<RigidBodyCollisionAuditor::CandidatePair>
RigidBodyCollisionAuditor::sweepSelfCandidates(
    const std::vector<WorldBox> &boxes) {
  std::vector<std::size_t> order(boxes.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&boxes](std::size_t a, std::size_t b) {
    if (boxes[a].aabb_min[0] != boxes[b].aabb_min[0]) {
      return boxes[a].aabb_min[0] < boxes[b].aabb_min[0];
    }
    return a < b;
  });

  std::vector<std::size_t> active;
  std::vector<CandidatePair> result;
  for (const std::size_t current : order) {
    active.erase(std::remove_if(active.begin(), active.end(),
                                [&boxes, current](std::size_t other) {
                                  return boxes[other].aabb_max[0] <
                                         boxes[current].aabb_min[0];
                                }),
                 active.end());
    for (const std::size_t other : active) {
      if (!aabbOverlaps(boxes[other], boxes[current])) {
        continue;
      }
      result.emplace_back(std::min(other, current), std::max(other, current));
    }
    active.push_back(current);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<RigidBodyCollisionAuditor::CandidatePair>
RigidBodyCollisionAuditor::sweepCrossCandidates(
    const std::vector<WorldBox> &first, const std::vector<WorldBox> &second) {
  if (first.empty() || second.empty()) {
    return {};
  }

  struct SweepEntry {
    bool from_first = false;
    std::size_t index = 0;
    const WorldBox *box = nullptr;
  };

  std::vector<SweepEntry> entries;
  entries.reserve(first.size() + second.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    entries.push_back({true, index, &first[index]});
  }
  for (std::size_t index = 0; index < second.size(); ++index) {
    entries.push_back({false, index, &second[index]});
  }
  std::sort(entries.begin(), entries.end(),
            [](const SweepEntry &a, const SweepEntry &b) {
              if (a.box->aabb_min[0] != b.box->aabb_min[0]) {
                return a.box->aabb_min[0] < b.box->aabb_min[0];
              }
              if (a.from_first != b.from_first) {
                return a.from_first > b.from_first;
              }
              return a.index < b.index;
            });

  std::vector<const SweepEntry *> active_first;
  std::vector<const SweepEntry *> active_second;
  std::vector<CandidatePair> result;
  for (const auto &current : entries) {
    auto &opposite = current.from_first ? active_second : active_first;
    opposite.erase(std::remove_if(opposite.begin(), opposite.end(),
                                  [&current](const SweepEntry *other) {
                                    return other->box->aabb_max[0] <
                                           current.box->aabb_min[0];
                                  }),
                   opposite.end());
    for (const SweepEntry *other : opposite) {
      if (!aabbOverlaps(*other->box, *current.box)) {
        continue;
      }
      result.emplace_back(other->from_first ? other->index : current.index,
                          other->from_first ? current.index : other->index);
    }
    auto &same = current.from_first ? active_first : active_second;
    same.push_back(&current);
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool RigidBodyCollisionAuditor::aabbOverlaps(const WorldBox &a,
                                             const WorldBox &b) {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (a.aabb_max[axis] < b.aabb_min[axis] ||
        b.aabb_max[axis] < a.aabb_min[axis]) {
      return false;
    }
  }
  return true;
}

std::set<std::pair<std::string, std::string>>
RigidBodyCollisionAuditor::collectLinkedPhysicsBodyPairs(
    const std::vector<loader::Bone> &bones,
    const std::map<std::string, rigidbody::BodyDefinition> &physics_bodies) {
  std::set<std::pair<std::string, std::string>> result;
  std::map<std::string, const loader::Bone *> bonesByName;
  for (const auto &bone : bones) {
    if (!bone.name.empty()) {
      bonesByName[bone.name] = &bone;
    }
  }
  for (const auto &bone : bones) {
    if (!bone.has_parent || !physics_bodies.contains(bone.name)) {
      continue;
    }
    std::string ancestor = bone.parent;
    std::set<std::string> visited;
    while (!ancestor.empty() && visited.insert(ancestor).second) {
      if (physics_bodies.contains(ancestor)) {
        result.insert(canonicalPair(ancestor, bone.name));
        break;
      }
      const auto parent = bonesByName.find(ancestor);
      if (parent == bonesByName.end() || !parent->second->has_parent) {
        break;
      }
      ancestor = parent->second->parent;
    }
  }
  return result;
}

std::pair<std::string, std::string>
RigidBodyCollisionAuditor::canonicalPair(const std::string &first,
                                         const std::string &second) {
  return first <= second ? std::make_pair(first, second)
                         : std::make_pair(second, first);
}

std::array<std::array<double, 3>, 3>
RigidBodyCollisionAuditor::axesFromSoaVertices(
    const std::array<double, 24> &vertices) {
  return {normalizedSoaEdge(vertices, 0, 1), normalizedSoaEdge(vertices, 0, 2),
          normalizedSoaEdge(vertices, 0, 4)};
}

std::array<double, 3> RigidBodyCollisionAuditor::normalizedSoaEdge(
    const std::array<double, 24> &vertices, int from, int to) {
  std::array<double, 3> result{};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    result[axis] = vertices[axis * 8u + static_cast<std::size_t>(to)] -
                   vertices[axis * 8u + static_cast<std::size_t>(from)];
  }
  normalize(result);
  return result;
}

double RigidBodyCollisionAuditor::penetration(
    const WorldBox &a, const WorldBox &b, double tolerance,
    ProjectionOverlapKernel projection_overlap) {
  double minimum = std::numeric_limits<double>::infinity();
  for (const auto &axis : a.axes) {
    const double o = projection_overlap(a.vertices_soa.data(),
                                        b.vertices_soa.data(), axis.data());
    if (o <= tolerance) {
      return 0.0;
    }
    minimum = std::min(minimum, o);
  }
  for (const auto &axis : b.axes) {
    const double o = projection_overlap(a.vertices_soa.data(),
                                        b.vertices_soa.data(), axis.data());
    if (o <= tolerance) {
      return 0.0;
    }
    minimum = std::min(minimum, o);
  }
  for (const auto &axis_a : a.axes) {
    for (const auto &axis_b : b.axes) {
      std::array<double, 3> cross{axis_a[1] * axis_b[2] - axis_a[2] * axis_b[1],
                                  axis_a[2] * axis_b[0] - axis_a[0] * axis_b[2],
                                  axis_a[0] * axis_b[1] -
                                      axis_a[1] * axis_b[0]};
      const double length_squared = dot(cross, cross);
      if (length_squared <= kMinAxisLengthSquared) {
        continue;
      }
      const double inv = 1.0 / std::sqrt(length_squared);
      for (double &c : cross) {
        c *= inv;
      }
      const double o = projection_overlap(a.vertices_soa.data(),
                                          b.vertices_soa.data(), cross.data());
      if (o <= tolerance) {
        return 0.0;
      }
      minimum = std::min(minimum, o);
    }
  }
  return std::isfinite(minimum) ? minimum : 0.0;
}

void RigidBodyCollisionAuditor::normalize(std::array<double, 3> &value) {
  const double length_squared = dot(value, value);
  if (!std::isfinite(length_squared) ||
      length_squared <= kMinAxisLengthSquared) {
    throw std::invalid_argument("final audit cube has a degenerate axis");
  }
  const double inv = 1.0 / std::sqrt(length_squared);
  for (double &c : value) {
    c *= inv;
  }
}

double RigidBodyCollisionAuditor::dot(const std::array<double, 3> &a,
                                      const std::array<double, 3> &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double RigidBodyCollisionAuditor::estimateScale(
    const std::vector<loader::Bone> &bones) {
  double scale = 1.0;
  for (const auto &bone : bones) {
    for (const auto &cube : bone.cubes) {
      for (int axis = 0; axis < 3; ++axis) {
        scale = std::max(scale,
                         std::abs(cube.origin[static_cast<std::size_t>(axis)]));
        scale = std::max(scale,
                         std::abs(cube.size[static_cast<std::size_t>(axis)]));
      }
    }
  }
  return scale;
}

double
RigidBodyCollisionAuditor::minimumSoaY(const std::array<double, 24> &vertices) {
  double minimum = std::numeric_limits<double>::infinity();
  for (std::size_t vertex = 0; vertex < 8; ++vertex) {
    minimum = std::min(minimum, vertices[8u + vertex]);
  }
  return minimum;
}

double RigidBodyCollisionAuditor::minimumFeature(
    const std::vector<CubeBinding> &bindings) {
  double minimum = std::numeric_limits<double>::infinity();
  for (const auto &binding : bindings) {
    for (const int vertex : {1, 2, 4}) {
      const std::size_t offset = static_cast<std::size_t>(vertex * 3);
      const double dx =
          binding.bind_vertices[offset] - binding.bind_vertices[0];
      const double dy =
          binding.bind_vertices[offset + 1] - binding.bind_vertices[1];
      const double dz =
          binding.bind_vertices[offset + 2] - binding.bind_vertices[2];
      const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (length > 0.0) {
        minimum = std::min(minimum, length);
      }
    }
  }
  return std::isfinite(minimum) ? minimum : 0.0;
}

double RigidBodyCollisionAuditor::maximumRadius(
    const std::vector<CubeBinding> &bindings) {
  double maximum = 0.0;
  for (const auto &binding : bindings) {
    for (int vertex = 0; vertex < 8; ++vertex) {
      const std::size_t offset = static_cast<std::size_t>(vertex * 3);
      const double x = binding.bind_vertices[offset];
      const double y = binding.bind_vertices[offset + 1];
      const double z = binding.bind_vertices[offset + 2];
      maximum = std::max(maximum, std::sqrt(x * x + y * y + z * z));
    }
  }
  return maximum;
}

}
