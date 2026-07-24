#pragma once

#include "xpbd/rigidbody/rigid_body_types.hpp"

#include <memory>
#include <vector>

namespace xpbd::rigidbody {


class RigidBodyBackend {
public:
  virtual ~RigidBodyBackend() = default;

  virtual void setSnapshotLevel(SnapshotLevel level) = 0;
  virtual void setSolverIterations(int iterations) = 0;
  virtual BodyHandle createBody(const BodyDefinition &definition) = 0;
  virtual BodyHandle createGroundPlane(const std::string &name, double height,
                                       double friction, double restitution) = 0;
  virtual void addSpringJoint(const BodyHandle &body_a,
                              const BodyHandle &body_b,
                              const Transform &world_anchor,
                              const JointSettings &settings) = 0;
  virtual SweepResult sweepKinematic(const BodyHandle &handle,
                                     const Transform &from_bone,
                                     const Transform &to_bone) = 0;
  virtual void setKinematicTransform(const BodyHandle &handle,
                                     const Transform &bone_transform, double dt,
                                     KinematicHistoryMode history_mode) = 0;
  virtual void applyCentralForce(const BodyHandle &handle,
                                 const std::array<double, 3> &force) = 0;
  virtual void step(double fixed_dt) = 0;

  virtual CollisionDetectionSnapshot detectContactsWithoutAdvancing() = 0;
  virtual BodyState getBodyState(const BodyHandle &handle) const = 0;
  virtual int getContactCount() const = 0;
  virtual double getMaximumPenetration() const = 0;
  virtual std::vector<ContactSnapshot> getContactSnapshots() const = 0;

  virtual std::vector<JointErrorSnapshot> getJointErrorSnapshots() const = 0;
  virtual int getNativeBulletVersion() const = 0;
};

std::unique_ptr<RigidBodyBackend>
createBulletBackend(double gravity_x, double gravity_y, double gravity_z);






enum class SingleBoxTopologyForTesting { Direct, Compound };

struct BulletBodyConstructionSnapshotForTesting {
  bool compound_shape = false;
  Transform bone_to_com{};
  std::array<double, 3> local_inertia{0, 0, 0};
  std::vector<double> child_margins;
  double friction = 0.0;
  double restitution = 0.0;
  double rolling_friction = 0.0;
  double spinning_friction = 0.0;
};

[[nodiscard]] std::unique_ptr<RigidBodyBackend>
createBulletBackendForTesting(double gravity_x, double gravity_y,
                              double gravity_z,
                              SingleBoxTopologyForTesting topology);

[[nodiscard]] BulletBodyConstructionSnapshotForTesting
getBulletBodyConstructionSnapshotForTesting(const RigidBodyBackend &backend,
                                            const BodyHandle &handle);

}
