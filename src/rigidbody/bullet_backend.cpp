#include "xpbd/rigidbody/rigid_body_backend.hpp"

#include <btBulletDynamicsCommon.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace xpbd::rigidbody {
namespace {

class KinematicSweepCallback final
    : public btCollisionWorld::ClosestConvexResultCallback {
public:
  KinematicSweepCallback(const btVector3 &from, const btVector3 &to,
                         const btCollisionObject *ignored)
      : ClosestConvexResultCallback(from, to), ignored_(ignored) {}

  bool needsCollision(btBroadphaseProxy *proxy) const override {
    if (proxy != nullptr && proxy->m_clientObject == ignored_) {
      return false;
    }
    return ClosestConvexResultCallback::needsCollision(proxy);
  }

private:
  const btCollisionObject *ignored_ = nullptr;
};

btVector3 toBt(const std::array<double, 3> &v) {
  return btVector3(static_cast<btScalar>(v[0]), static_cast<btScalar>(v[1]),
                   static_cast<btScalar>(v[2]));
}

btQuaternion toBtQuat(const std::array<double, 4> &q) {
  return btQuaternion(static_cast<btScalar>(q[0]), static_cast<btScalar>(q[1]),
                      static_cast<btScalar>(q[2]), static_cast<btScalar>(q[3]));
}

btTransform toBtTransform(const Transform &t) {
  btTransform out;
  out.setOrigin(toBt(t.translation));
  out.setRotation(toBtQuat(t.rotation));
  return out;
}

Transform fromBtTransform(const btTransform &t) {
  Transform out;
  const btVector3 o = t.getOrigin();
  out.translation = {o.x(), o.y(), o.z()};
  const btQuaternion q = t.getRotation();
  out.rotation = {q.x(), q.y(), q.z(), q.w()};
  out.normalizeRotation();
  return out;
}

btScalar materialOperand(double final_coefficient, const char *label,
                         double maximum) {
  if (!std::isfinite(final_coefficient) || final_coefficient < 0.0 ||
      final_coefficient > maximum) {
    throw std::invalid_argument(std::string(label) +
                                " must be finite and in [0, " +
                                std::to_string(maximum) + "]");
  }




  return static_cast<btScalar>(std::sqrt(final_coefficient));
}

btScalar boundedOperand(double value, const char *label, double maximum) {
  if (!std::isfinite(value) || value < 0.0 || value > maximum) {
    throw std::invalid_argument(std::string(label) +
                                " must be finite and in [0, " +
                                std::to_string(maximum) + "]");
  }
  return static_cast<btScalar>(value);
}

class BulletBackend final : public RigidBodyBackend {
public:
  BulletBackend(double gx, double gy, double gz,
                SingleBoxTopologyForTesting single_box_topology)
      : force_single_box_compound_(
            single_box_topology == SingleBoxTopologyForTesting::Compound) {
    if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz)) {
      throw std::invalid_argument("gravity must be finite");
    }
    collision_config_ = std::make_unique<btDefaultCollisionConfiguration>();
    dispatcher_ =
        std::make_unique<btCollisionDispatcher>(collision_config_.get());
    broadphase_ = std::make_unique<btDbvtBroadphase>();
    solver_ = std::make_unique<btSequentialImpulseConstraintSolver>();
    solver_->setRandSeed(0);
    world_ = std::make_unique<btDiscreteDynamicsWorld>(
        dispatcher_.get(), broadphase_.get(), solver_.get(),
        collision_config_.get());
    world_->setGravity(btVector3(static_cast<btScalar>(gx),
                                 static_cast<btScalar>(gy),
                                 static_cast<btScalar>(gz)));
    world_->setForceUpdateAllAabbs(true);
  }

  ~BulletBackend() override { close(); }

  void setSnapshotLevel(SnapshotLevel level) override {
    requireOpen();
    snapshot_level_ = level;
    if (level == SnapshotLevel::None) {
      contacts_.clear();
    }
  }

  void setSolverIterations(int iterations) override {
    requireOpen();
    world_->getSolverInfo().m_numIterations = std::max(1, iterations);
  }

  BodyHandle createBody(const BodyDefinition &definition) override {
    requireOpen();
    if (definition.name.empty()) {
      throw std::invalid_argument("body name is required");
    }

    const btScalar mass = definition.motion_type == MotionType::Dynamic
                              ? static_cast<btScalar>(definition.mass)
                              : btScalar(0);
    btCollisionShape *shape = nullptr;
    btCompoundShape *compound = nullptr;
    std::vector<btBoxShape *> childShapes;
    btVector3 principalInertia(0, 0, 0);
    btTransform boneToCom;
    boneToCom.setIdentity();

    if (definition.boxes.size() == 1 && !force_single_box_compound_) {
      const auto &box = definition.boxes.front();
      auto *directBox = new btBoxShape(toBt(box.half_extents));
      const btScalar margin = static_cast<btScalar>(bulletBoxCollisionMargin(
          std::min(box.half_extents[0],
                   std::min(box.half_extents[1], box.half_extents[2]))));
      directBox->setMargin(margin);
      shape = directBox;
      boneToCom = toBtTransform(box.local_transform);
      if (definition.motion_type == MotionType::Dynamic) {
        directBox->calculateLocalInertia(mass, principalInertia);
      }
    } else if (!definition.boxes.empty()) {
      compound = new btCompoundShape();
      double totalVolume = 0.0;
      for (const auto &box : definition.boxes) {
        totalVolume += box.half_extents[0] * box.half_extents[1] *
                       box.half_extents[2] * 8.0;
      }
      std::vector<btScalar> masses;
      masses.reserve(definition.boxes.size());
      for (const auto &box : definition.boxes) {
        const auto &half = box.half_extents;
        const double volume = half[0] * half[1] * half[2] * 8.0;
        const double totalMass = definition.motion_type == MotionType::Dynamic
                                     ? definition.mass
                                     : 1.0;
        masses.push_back(
            static_cast<btScalar>(totalMass * volume / totalVolume));

        auto *child = new btBoxShape(toBt(half));
        const btScalar margin = static_cast<btScalar>(bulletBoxCollisionMargin(
            std::min(half[0], std::min(half[1], half[2]))));
        child->setMargin(margin);
        compound->addChildShape(toBtTransform(box.local_transform), child);
        childShapes.push_back(child);
      }
      btVector3 inertiaLocal;
      compound->calculatePrincipalAxisTransform(masses.data(), boneToCom,
                                                inertiaLocal);
      principalInertia = inertiaLocal;
      btTransform inverseCom = boneToCom.inverse();
      for (int i = 0; i < compound->getNumChildShapes(); ++i) {
        btTransform child = compound->getChildTransform(i);
        compound->updateChildTransform(i, inverseCom * child, false);
      }
      compound->recalculateLocalAabb();
      shape = compound;
    }

    if (shape == nullptr) {
      shape = new btEmptyShape();
    }
    btVector3 inertia = definition.motion_type == MotionType::Dynamic
                            ? principalInertia
                            : btVector3(0, 0, 0);
    if (definition.motion_type == MotionType::Dynamic &&
        definition.boxes.empty()) {
      shape->calculateLocalInertia(mass, inertia);
    }

    btTransform initialBody =
        toBtTransform(definition.initial_bone_transform) * boneToCom;
    auto *body = new btRigidBody(mass, nullptr, shape, inertia);
    body->setWorldTransform(initialBody);
    body->setInterpolationWorldTransform(initialBody);
    body->setFriction(
        materialOperand(definition.friction, "friction", 10.0));
    body->setRestitution(
        materialOperand(definition.restitution, "restitution", 1.0));
    body->setDamping(
        boundedOperand(definition.linear_damping, "linear damping", 1.0),
        boundedOperand(definition.angular_damping, "angular damping", 1.0));

    if (definition.motion_type == MotionType::Kinematic) {
      body->setCollisionFlags(body->getCollisionFlags() |
                              btCollisionObject::CF_KINEMATIC_OBJECT);
      body->setActivationState(DISABLE_DEACTIVATION);
    } else if (definition.motion_type == MotionType::Dynamic) {
      body->setActivationState(DISABLE_DEACTIVATION);
      if (definition.ccd.enabled) {
        body->setCcdMotionThreshold(
            static_cast<btScalar>(definition.ccd.motion_threshold));
        body->setCcdSweptSphereRadius(
            static_cast<btScalar>(definition.ccd.swept_sphere_radius));
      }
    }

    const int id = next_body_id_++;
    body->setUserIndex(id);
    BodyEntry entry;
    entry.motion = definition.motion_type;
    entry.body = body;
    entry.shape = shape;
    entry.compound = compound;
    entry.child_shapes = std::move(childShapes);
    entry.bone_to_com = boneToCom;
    entry.com_to_bone = boneToCom.inverse();
    entry.local_inertia = inertia;
    entry.last_bone = toBtTransform(definition.initial_bone_transform);
    entry.name = definition.name;
    bodies_[id] = std::move(entry);
    world_->addRigidBody(body);
    world_->updateSingleAabb(body);
    return BodyHandle{id, definition.name};
  }

  BodyHandle createGroundPlane(const std::string &name, double height,
                               double friction, double restitution) override {
    requireOpen();
    auto *shape = new btStaticPlaneShape(btVector3(0, 1, 0),
                                         static_cast<btScalar>(height));
    auto *body = new btRigidBody(0, nullptr, shape, btVector3(0, 0, 0));
    btTransform identity;
    identity.setIdentity();
    body->setWorldTransform(identity);
    body->setFriction(materialOperand(friction, "friction", 10.0));
    body->setRestitution(
        materialOperand(restitution, "restitution", 1.0));
    const int id = next_body_id_++;
    body->setUserIndex(id);
    BodyEntry entry;
    entry.motion = MotionType::Static;
    entry.body = body;
    entry.shape = shape;
    entry.bone_to_com.setIdentity();
    entry.com_to_bone.setIdentity();
    entry.last_bone.setIdentity();
    entry.name = name;
    bodies_[id] = std::move(entry);
    world_->addRigidBody(body);
    return BodyHandle{id, name};
  }

  void addSpringJoint(const BodyHandle &body_a, const BodyHandle &body_b,
                      const Transform &world_anchor,
                      const JointSettings &settings) override {
    requireOpen();
    BodyEntry &a = entry(body_a);
    BodyEntry &b = entry(body_b);
    const btTransform anchor = toBtTransform(world_anchor);
    const btTransform frameA = a.body->getWorldTransform().inverse() * anchor;
    const btTransform frameB = b.body->getWorldTransform().inverse() * anchor;
    auto *constraint =
        new btGeneric6DofSpring2Constraint(*a.body, *b.body, frameA, frameB);
    constraint->setLinearLowerLimit(btVector3(0, 0, 0));
    constraint->setLinearUpperLimit(btVector3(0, 0, 0));
    constraint->setAngularLowerLimit(toBt(settings.angular_lower_limit));
    constraint->setAngularUpperLimit(toBt(settings.angular_upper_limit));
    if (settings.stiffness > 0) {
      for (int axis = 3; axis < 6; ++axis) {
        if (!settings.angular_spring_enabled[static_cast<std::size_t>(axis -
                                                                      3)]) {
          continue;
        }
        constraint->enableSpring(axis, true);
        constraint->setStiffness(axis,
                                 static_cast<btScalar>(settings.stiffness),
                                 true);
        constraint->setDamping(axis, static_cast<btScalar>(settings.damping),
                               true);
      }
      constraint->setEquilibriumPoint();
    }
    world_->addConstraint(constraint, true);
    constraints_.push_back(
        JointEntry{constraint, body_a.name, body_b.name, settings});
  }

  SweepResult sweepKinematic(const BodyHandle &handle,
                             const Transform &from_bone,
                             const Transform &to_bone) override {
    requireOpen();
    BodyEntry &e = entry(handle);
    if (e.motion != MotionType::Kinematic) {
      throw std::invalid_argument("only kinematic bodies are swept");
    }
    const btTransform fromBody = toBtTransform(from_bone) * e.bone_to_com;
    const btTransform toBody = toBtTransform(to_bone) * e.bone_to_com;
    btScalar closest = 1;
    std::string closestName;
    const auto sweepBox = [&](btBoxShape *box, const btTransform &from,
                              const btTransform &to) {
      KinematicSweepCallback cb(from.getOrigin(), to.getOrigin(), e.body);
      cb.m_collisionFilterGroup = btBroadphaseProxy::DefaultFilter;
      cb.m_collisionFilterMask = btBroadphaseProxy::AllFilter;
      world_->convexSweepTest(box, from, to, cb, 0.0f);
      if (cb.hasHit() && cb.m_closestHitFraction < closest) {
        closest = cb.m_closestHitFraction;
        const auto *hitBody = btRigidBody::upcast(cb.m_hitCollisionObject);
        if (hitBody != nullptr) {
          const int hitId = hitBody->getUserIndex();
          auto it = bodies_.find(hitId);
          if (it != bodies_.end()) {
            closestName = it->second.name;
          }
        }
      }
    };
    if (e.compound != nullptr) {
      for (int i = 0; i < e.compound->getNumChildShapes(); ++i) {
        btCollisionShape *child = e.compound->getChildShape(i);
        if (child->getShapeType() != BOX_SHAPE_PROXYTYPE) {
          continue;
        }
        sweepBox(static_cast<btBoxShape *>(child),
                 fromBody * e.compound->getChildTransform(i),
                 toBody * e.compound->getChildTransform(i));
      }
    } else if (e.shape != nullptr &&
               e.shape->getShapeType() == BOX_SHAPE_PROXYTYPE) {
      sweepBox(static_cast<btBoxShape *>(e.shape), fromBody, toBody);
    }
    if (closest < 1) {
      return SweepResult{true, closest, closestName};
    }
    return SweepResult::miss();
  }

  void setKinematicTransform(const BodyHandle &handle,
                             const Transform &bone_transform, double dt,
                             KinematicHistoryMode history_mode) override {
    requireOpen();
    if (!std::isfinite(dt) || !(dt > 0)) {
      throw std::invalid_argument("dt must be finite and greater than zero");
    }
    BodyEntry &e = entry(handle);
    if (e.motion != MotionType::Kinematic) {
      throw std::invalid_argument("body is not kinematic: " + handle.name);
    }
    const bool continuous_history =
        history_mode == KinematicHistoryMode::Continuous;
    const btTransform previousBone =
        continuous_history ? e.last_bone : toBtTransform(bone_transform);
    const btTransform previousBody = previousBone * e.bone_to_com;
    const btTransform nextBone = toBtTransform(bone_transform);
    const btTransform nextBody = nextBone * e.bone_to_com;

    btVector3 linearVelocity =
        (nextBody.getOrigin() - previousBody.getOrigin()) /
        static_cast<btScalar>(dt);
    btQuaternion prevQ = previousBody.getRotation();
    btQuaternion nextQ = nextBody.getRotation();
    if (prevQ.dot(nextQ) < 0) {
      nextQ = btQuaternion(-nextQ.x(), -nextQ.y(), -nextQ.z(), -nextQ.w());
    }
    btQuaternion dQ = nextQ * prevQ.inverse();
    btVector3 axis(dQ.getAxis());
    btScalar angle = dQ.getAngle();
    btVector3 angularVelocity = continuous_history
                                    ? axis * (angle / static_cast<btScalar>(dt))
                                    : btVector3(0, 0, 0);
    if (!continuous_history) {
      linearVelocity.setZero();
    }

    e.body->setInterpolationWorldTransform(previousBody);
    e.body->setWorldTransform(nextBody);
    e.body->setLinearVelocity(linearVelocity);
    e.body->setAngularVelocity(angularVelocity);
    e.body->setInterpolationLinearVelocity(linearVelocity);
    e.body->setInterpolationAngularVelocity(angularVelocity);
    e.body->activate(true);
    e.last_bone = nextBone;
    world_->updateSingleAabb(e.body);
    if (!continuous_history) {



      if (btBroadphaseProxy *proxy = e.body->getBroadphaseHandle()) {
        world_->getBroadphase()
            ->getOverlappingPairCache()
            ->cleanProxyFromPairs(proxy, dispatcher_.get());
      }
      for (int i = dispatcher_->getNumManifolds() - 1; i >= 0; --i) {
        btPersistentManifold *manifold =
            dispatcher_->getManifoldByIndexInternal(i);
        if (manifold->getBody0() == e.body || manifold->getBody1() == e.body) {
          dispatcher_->clearManifold(manifold);
        }
      }
      world_->computeOverlappingPairs();
      world_->performDiscreteCollisionDetection();
    }
  }

  void applyCentralForce(const BodyHandle &handle,
                         const std::array<double, 3> &force) override {
    requireOpen();
    BodyEntry &e = entry(handle);
    if (e.motion != MotionType::Dynamic) {
      throw std::invalid_argument("central force requires a dynamic body");
    }
    e.body->applyCentralForce(toBt(force));
    e.body->activate(true);
  }

  void step(double fixed_dt) override {
    requireOpen();
    if (!std::isfinite(fixed_dt) || !(fixed_dt > 0)) {
      throw std::invalid_argument(
          "fixed dt must be finite and greater than zero");
    }
    world_->stepSimulation(static_cast<btScalar>(fixed_dt), 0,
                           static_cast<btScalar>(fixed_dt));
    captureContacts();
  }

  CollisionDetectionSnapshot detectContactsWithoutAdvancing() override {
    requireOpen();
    world_->updateAabbs();
    world_->computeOverlappingPairs();
    world_->performDiscreteCollisionDetection();
    captureContacts();
    return latest_collision_;
  }

  void captureContacts() {
    contacts_.clear();
    latest_collision_ = {};
    const int manifolds = dispatcher_->getNumManifolds();
    for (int i = 0; i < manifolds; ++i) {
      btPersistentManifold *manifold =
          dispatcher_->getManifoldByIndexInternal(i);
      const btCollisionObject *a = manifold->getBody0();
      const btCollisionObject *b = manifold->getBody1();
      const int idA = a->getUserIndex();
      const int idB = b->getUserIndex();
      auto itA = bodies_.find(idA);
      auto itB = bodies_.find(idB);
      if (itA == bodies_.end() || itB == bodies_.end()) {
        continue;
      }
      for (int p = 0; p < manifold->getNumContacts(); ++p) {
        const btManifoldPoint &pt = manifold->getContactPoint(p);
        if (pt.getDistance() > 0) {
          continue;
        }
        const BodyHandle handleA{idA, itA->second.name};
        const BodyHandle handleB{idB, itB->second.name};
        const double penetration = -pt.getDistance();
        ++latest_collision_.contact_count;
        if (penetration > latest_collision_.maximum_penetration) {
          latest_collision_.maximum_penetration = penetration;
          latest_collision_.worst_pair = std::make_pair(handleA, handleB);
        }
        const bool ground = handleA.name == "__ground__" ||
                            handleB.name == "__ground__";
        if (ground) {
          ++latest_collision_.ground_contacts;
        } else {
          ++latest_collision_.self_contacts;
        }
        if (snapshot_level_ != SnapshotLevel::None) {
          ContactSnapshot snap;
          snap.body_a = handleA;
          snap.body_b = handleB;
          snap.penetration = penetration;
          snap.combined_friction = pt.m_combinedFriction;
          snap.combined_restitution = pt.m_combinedRestitution;
          contacts_.push_back(std::move(snap));
        }
      }
    }
  }

  BodyState getBodyState(const BodyHandle &handle) const override {
    requireOpen();
    const BodyEntry &e = entry(handle);
    const btTransform com = e.body->getWorldTransform();
    const btTransform bone = com * e.com_to_bone;
    BodyState state;
    state.bone_transform = fromBtTransform(bone);
    state.com_transform = fromBtTransform(com);
    const btVector3 lin = e.body->getLinearVelocity();
    const btVector3 ang = e.body->getAngularVelocity();
    const btVector3 pivotOffset = bone.getOrigin() - com.getOrigin();
    const btVector3 boneLinear = lin + ang.cross(pivotOffset);
    state.bone_linear_velocity = {boneLinear.x(), boneLinear.y(),
                                  boneLinear.z()};
    state.com_linear_velocity = {lin.x(), lin.y(), lin.z()};
    state.angular_velocity = {ang.x(), ang.y(), ang.z()};
    return state;
  }

  int getContactCount() const override {
    requireOpen();
    return latest_collision_.contact_count;
  }

  double getMaximumPenetration() const override {
    requireOpen();
    return latest_collision_.maximum_penetration;
  }

  std::vector<ContactSnapshot> getContactSnapshots() const override {
    requireOpen();
    return contacts_;
  }

  std::vector<JointErrorSnapshot> getJointErrorSnapshots() const override {
    requireOpen();
    std::vector<JointErrorSnapshot> result;
    result.reserve(constraints_.size());
    for (const JointEntry &entry : constraints_) {
      entry.constraint->calculateTransforms();
      const btVector3 anchor_delta =
          entry.constraint->getCalculatedTransformB().getOrigin() -
          entry.constraint->getCalculatedTransformA().getOrigin();
      JointErrorSnapshot snapshot;
      snapshot.parent_body = entry.parent_body;
      snapshot.child_body = entry.child_body;
      snapshot.anchor_separation =
          static_cast<double>(anchor_delta.length());
      for (int axis = 0; axis < 3; ++axis) {
        const auto index = static_cast<std::size_t>(axis);
        const double angle =
            static_cast<double>(entry.constraint->getAngle(axis));
        snapshot.relative_angles[index] = angle;
        const double lower = entry.settings.angular_lower_limit[index];
        const double upper = entry.settings.angular_upper_limit[index];
        const double excess = angle < lower   ? lower - angle
                              : angle > upper ? angle - upper
                                              : 0.0;
        snapshot.angular_excess[index] = excess;
        snapshot.maximum_angular_excess =
            std::max(snapshot.maximum_angular_excess, excess);
      }
      result.push_back(std::move(snapshot));
    }
    return result;
  }

  int getNativeBulletVersion() const override {
    requireOpen();
    return BT_BULLET_VERSION;
  }

  BulletBodyConstructionSnapshotForTesting
  constructionSnapshotForTesting(const BodyHandle &handle) const {
    requireOpen();
    const BodyEntry &e = entry(handle);
    BulletBodyConstructionSnapshotForTesting snapshot;
    snapshot.compound_shape = e.compound != nullptr;
    snapshot.bone_to_com = fromBtTransform(e.bone_to_com);
    snapshot.local_inertia = {e.local_inertia.x(), e.local_inertia.y(),
                              e.local_inertia.z()};
    if (e.compound != nullptr) {
      snapshot.child_margins.reserve(e.child_shapes.size());
      for (const auto *child : e.child_shapes) {
        snapshot.child_margins.push_back(child->getMargin());
      }
    } else if (e.shape != nullptr &&
               e.shape->getShapeType() == BOX_SHAPE_PROXYTYPE) {
      snapshot.child_margins.push_back(e.shape->getMargin());
    }
    snapshot.friction = e.body->getFriction();
    snapshot.restitution = e.body->getRestitution();
    snapshot.rolling_friction = e.body->getRollingFriction();
    snapshot.spinning_friction = e.body->getSpinningFriction();
    return snapshot;
  }

private:
  struct BodyEntry {
    MotionType motion = MotionType::Dynamic;
    btRigidBody *body = nullptr;
    btCollisionShape *shape = nullptr;
    btCompoundShape *compound = nullptr;
    std::vector<btBoxShape *> child_shapes;
    btTransform bone_to_com;
    btTransform com_to_bone;
    btVector3 local_inertia{0, 0, 0};
    btTransform last_bone;
    std::string name;
  };

  struct JointEntry {
    btGeneric6DofSpring2Constraint *constraint = nullptr;
    std::string parent_body;
    std::string child_body;
    JointSettings settings{};
  };

  void requireOpen() const {
    if (closed_) {
      throw std::logic_error("bullet backend is closed");
    }
  }

  BodyEntry &entry(const BodyHandle &handle) {
    auto it = bodies_.find(handle.id);
    if (it == bodies_.end() || it->second.name != handle.name) {
      throw std::invalid_argument("unknown body handle");
    }
    return it->second;
  }

  const BodyEntry &entry(const BodyHandle &handle) const {
    auto it = bodies_.find(handle.id);
    if (it == bodies_.end() || it->second.name != handle.name) {
      throw std::invalid_argument("unknown body handle");
    }
    return it->second;
  }

  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    for (int i = static_cast<int>(constraints_.size()) - 1; i >= 0; --i) {
      JointEntry &entry = constraints_[static_cast<std::size_t>(i)];
      world_->removeConstraint(entry.constraint);
      delete entry.constraint;
    }
    constraints_.clear();
    for (auto &[id, e] : bodies_) {
      (void)id;
      world_->removeRigidBody(e.body);
      delete e.body;
      delete e.shape;



      for (auto *child : e.child_shapes) {
        delete child;
      }
    }
    bodies_.clear();
    contacts_.clear();
    latest_collision_ = {};
    world_.reset();
    solver_.reset();
    broadphase_.reset();
    dispatcher_.reset();
    collision_config_.reset();
  }

  std::unique_ptr<btDefaultCollisionConfiguration> collision_config_;
  std::unique_ptr<btCollisionDispatcher> dispatcher_;
  std::unique_ptr<btDbvtBroadphase> broadphase_;
  std::unique_ptr<btSequentialImpulseConstraintSolver> solver_;
  std::unique_ptr<btDiscreteDynamicsWorld> world_;
  std::unordered_map<int, BodyEntry> bodies_;
  std::vector<JointEntry> constraints_;
  std::vector<ContactSnapshot> contacts_;
  CollisionDetectionSnapshot latest_collision_{};
  SnapshotLevel snapshot_level_ = SnapshotLevel::ContactsOnly;
  int next_body_id_ = 1;
  bool force_single_box_compound_ = false;
  bool closed_ = false;
};

}

std::unique_ptr<RigidBodyBackend>
createBulletBackend(double gravity_x, double gravity_y, double gravity_z) {
  return std::make_unique<BulletBackend>(
      gravity_x, gravity_y, gravity_z,
      SingleBoxTopologyForTesting::Direct);
}

std::unique_ptr<RigidBodyBackend>
createBulletBackendForTesting(double gravity_x, double gravity_y,
                              double gravity_z,
                              SingleBoxTopologyForTesting topology) {
  return std::make_unique<BulletBackend>(gravity_x, gravity_y, gravity_z,
                                         topology);
}

BulletBodyConstructionSnapshotForTesting
getBulletBodyConstructionSnapshotForTesting(const RigidBodyBackend &backend,
                                            const BodyHandle &handle) {
  const auto *bullet = dynamic_cast<const BulletBackend *>(&backend);
  if (bullet == nullptr) {
    throw std::invalid_argument(
        "construction snapshot requires the native Bullet backend");
  }
  return bullet->constructionSnapshotForTesting(handle);
}

}
