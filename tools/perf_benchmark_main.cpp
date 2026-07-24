#include "xpbd/baker/bake_profiler.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/final_pose_reconstructor.hpp"
#include "xpbd/baker/loop_seam_corrector.hpp"
#include "xpbd/baker/output_timeline_resampler.hpp"
#include "xpbd/baker/physics_baker.hpp"
#include "xpbd/baker/rigid_body_collision_auditor.hpp"
#include "xpbd/constraints/distance_constraint.hpp"
#include "xpbd/core/simd_dispatch.hpp"
#include "xpbd/core/xpbd_engine.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/models/particle.hpp"
#include "xpbd/rigidbody/rigid_body_bake_session.hpp"

#include "../src/core/simd_kernels.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::size_t cube_count = 100;
  std::size_t particle_count = 512;
  std::size_t bone_count = 256;
  std::size_t value_count = 262144;
  int warmup = 5;
  int samples = 30;
  int xpbd_steps_per_sample = 30;
  int kernel_iterations = 16;
  int rigid_substeps = 4;
  int trace_capacity = 256;
  std::string diagnostics = "contacts";
  std::string mode = "all";
  std::filesystem::path output;
};

[[noreturn]] void usageError(std::string_view message) {
  throw std::invalid_argument(
      std::string(message) +
      "\nusage: xpbd_perf_benchmark "
      "[--mode all|uv|xpbd|reconstruct|bake|rigid|frame-layout|audit|simd] "
      "[--cubes "
      "N] "
      "[--particles N] [--bones N] [--warmup N] [--samples N] "
      "[--xpbd-steps N] [--values N] [--kernel-iterations N] "
      "[--rigid-substeps N] [--diagnostics none|contacts|full] "
      "[--trace-capacity N] "
      "[--output result.json]");
}

template <typename Integer>
Integer parsePositive(std::string_view text, std::string_view flag) {
  Integer value{};
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value <= 0) {
    usageError(std::string(flag) + " requires a positive integer");
  }
  return value;
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    auto next = [&]() -> std::string_view {
      if (++index >= argc) {
        usageError(std::string(flag) + " requires a value");
      }
      return argv[index];
    };
    if (flag == "--mode") {
      options.mode = std::string(next());
      if (options.mode != "all" && options.mode != "uv" &&
          options.mode != "xpbd" && options.mode != "reconstruct" &&
          options.mode != "bake" && options.mode != "rigid" &&
          options.mode != "frame-layout" && options.mode != "audit" &&
          options.mode != "simd") {
        usageError("--mode must be all, uv, xpbd, reconstruct, bake, rigid, "
                   "frame-layout, audit, or simd");
      }
    } else if (flag == "--cubes") {
      options.cube_count = parsePositive<std::size_t>(next(), flag);
    } else if (flag == "--particles") {
      options.particle_count = parsePositive<std::size_t>(next(), flag);
    } else if (flag == "--bones") {
      options.bone_count = parsePositive<std::size_t>(next(), flag);
    } else if (flag == "--values") {
      options.value_count = parsePositive<std::size_t>(next(), flag);
    } else if (flag == "--warmup") {
      options.warmup = parsePositive<int>(next(), flag);
    } else if (flag == "--samples") {
      options.samples = parsePositive<int>(next(), flag);
    } else if (flag == "--xpbd-steps") {
      options.xpbd_steps_per_sample = parsePositive<int>(next(), flag);
    } else if (flag == "--kernel-iterations") {
      options.kernel_iterations = parsePositive<int>(next(), flag);
    } else if (flag == "--rigid-substeps") {
      options.rigid_substeps = parsePositive<int>(next(), flag);
    } else if (flag == "--trace-capacity") {
      options.trace_capacity = parsePositive<int>(next(), flag);
    } else if (flag == "--diagnostics") {
      options.diagnostics = std::string(next());
      if (options.diagnostics != "none" &&
          options.diagnostics != "contacts" &&
          options.diagnostics != "full") {
        usageError("--diagnostics must be none, contacts, or full");
      }
    } else if (flag == "--output") {
      options.output = std::filesystem::path(next());
    } else if (flag == "--help" || flag == "-h") {
      std::cout
          << "usage: xpbd_perf_benchmark "
             "[--mode all|uv|xpbd|reconstruct|bake|rigid|frame-layout|audit|simd] "
             "[--cubes N] [--particles N] [--bones N] [--warmup N] [--samples "
             "N] "
             "[--xpbd-steps N] [--values N] [--kernel-iterations N] "
             "[--rigid-substeps N] [--diagnostics none|contacts|full] "
             "[--trace-capacity N] "
             "[--output result.json]\n";
      std::exit(0);
    } else {
      usageError(std::string("unknown option: ") + std::string(flag));
    }
  }
  if (options.cube_count > 100000 || options.particle_count > 1000000 ||
      options.bone_count > 100000 || options.value_count > 100000000 ||
      options.samples > 100000 || options.kernel_iterations > 100000 ||
      options.xpbd_steps_per_sample > 100000 ||
      options.rigid_substeps > 100000 || options.trace_capacity > 1000000) {
    usageError("requested benchmark size is outside the safety limit");
  }
  return options;
}

struct Distribution {
  double median_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
  double minimum_ms = 0.0;
  double maximum_ms = 0.0;
};

double percentile(const std::vector<double> &sorted, double quantile) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double position = quantile * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const double fraction = position - static_cast<double>(lower);
  return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

Distribution summarize(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  Distribution result;
  if (values.empty()) {
    return result;
  }
  result.minimum_ms = values.front();
  result.maximum_ms = values.back();
  result.median_ms = percentile(values, 0.50);
  result.p95_ms = percentile(values, 0.95);
  result.p99_ms = percentile(values, 0.99);
  return result;
}

template <typename Function>
Distribution measure(int warmup, int samples, Function &&function) {
  for (int index = 0; index < warmup; ++index) {
    function();
  }
  std::vector<double> timings;
  timings.reserve(static_cast<std::size_t>(samples));
  for (int index = 0; index < samples; ++index) {
    const auto start = Clock::now();
    function();
    const auto end = Clock::now();
    timings.push_back(
        std::chrono::duration<double, std::milli>(end - start).count());
  }
  return summarize(std::move(timings));
}

nlohmann::json toJson(const Distribution &value) {
  return {{"median_ms", value.median_ms},
          {"p95_ms", value.p95_ms},
          {"p99_ms", value.p99_ms},
          {"minimum_ms", value.minimum_ms},
          {"maximum_ms", value.maximum_ms}};
}

xpbd::loader::Geometry makeUvGeometry(std::size_t cubeCount) {
  xpbd::loader::Geometry geometry;
  geometry.description.identifier = "benchmark.uv_heavy";
  geometry.description.texture_width = 64;
  geometry.description.texture_height = 64;
  geometry.description.has_texture_size = true;
  xpbd::loader::Bone bone;
  bone.name = "root";
  bone.cubes.reserve(cubeCount);

  const auto side = static_cast<std::size_t>(
      std::ceil(std::cbrt(static_cast<double>(cubeCount))));
  for (std::size_t index = 0; index < cubeCount; ++index) {
    xpbd::loader::Cube cube;
    const std::size_t x = index % side;
    const std::size_t y = (index / side) % side;
    const std::size_t z = index / (side * side);
    cube.origin[0] = static_cast<double>(x) * 2.0;
    cube.origin[1] = static_cast<double>(y) * 2.0;
    cube.origin[2] = static_cast<double>(z) * 2.0;
    cube.size[0] = cube.size[1] = cube.size[2] = 1.0;
    cube.uv_mode = xpbd::loader::CubeUVMode::Box;
    cube.uv_box[0] = static_cast<double>((index % 8) * 8);
    cube.uv_box[1] = static_cast<double>(((index / 8) % 8) * 8);
    cube.mirror = (index % 2) != 0;
    bone.cubes.push_back(cube);
  }
  geometry.bones.push_back(std::move(bone));
  return geometry;
}

xpbd::gfx::TextureImage makeBenchmarkTexture() {
  xpbd::gfx::TextureImage texture;
  texture.width = 64;
  texture.height = 64;
  texture.path = "synthetic://uv-benchmark";
  texture.rgba.resize(64u * 64u * 4u);
  for (int y = 0; y < texture.height; ++y) {
    for (int x = 0; x < texture.width; ++x) {
      const auto offset = static_cast<std::size_t>((y * texture.width + x) * 4);
      texture.rgba[offset] =
          static_cast<std::uint8_t>((x * 37 + y * 11) & 0xff);
      texture.rgba[offset + 1] =
          static_cast<std::uint8_t>((x * 13 + y * 29) & 0xff);
      texture.rgba[offset + 2] =
          static_cast<std::uint8_t>((x * 7 + y * 53) & 0xff);
      texture.rgba[offset + 3] = 255;
    }
  }
  return texture;
}

nlohmann::json benchmarkUv(const Options &options) {
  auto geometry = makeUvGeometry(options.cube_count);
  auto texture = makeBenchmarkTexture();
  xpbd::gfx::ViewportMeshBuilder builder;
  builder.setGeometry(&geometry);
  builder.setTexture(&texture);
  builder.setShowBones(false);
  builder.setShowGround(false);

  xpbd::gfx::ViewportGpuScene legacy;
  const auto legacyTiming = measure(options.warmup, options.samples,
                                    [&] { builder.buildRest(legacy); });

  xpbd::gfx::StaticIndexedModelMesh indexed;
  const auto indexedTiming = measure(options.warmup, options.samples, [&] {
    builder.buildStaticIndexedModel(indexed);
  });

  xpbd::gfx::StaticModelFrameData staticFrame;
  const auto staticFrameTiming = measure(options.warmup, options.samples, [&] {
    builder.buildStaticRestFrame(staticFrame);
  });

  // Exercise the cached rest-pose ground/grid path separately. This remains a
  // structural regression metric: steady frames may copy the small overlay,
  // but must not revisit every cube to recompute its bounds.
  builder.setShowGround(true);
  xpbd::gfx::StaticModelFrameData staticGroundFrame;
  const auto staticGroundFrameTiming =
      measure(options.warmup, options.samples,
              [&] { builder.buildStaticRestFrame(staticGroundFrame); });

  const std::size_t legacyVertices =
      legacy.solid.size() + legacy.transparent.size();
  const std::size_t legacyBytes =
      legacyVertices * sizeof(xpbd::gfx::MeshVertex);
  const std::size_t indexedBytes =
      indexed.vertices.size() * sizeof(xpbd::gfx::StaticModelVertex) +
      indexed.indices.size() * sizeof(std::uint32_t);
  const std::size_t overlayVertices = staticFrame.overlays.solid.size() +
                                      staticFrame.overlays.transparent.size() +
                                      staticFrame.overlays.lines.size();
  const std::size_t staticFrameBytes =
      staticFrame.bones.size() * sizeof(xpbd::gfx::StaticModelBoneState) +
      overlayVertices * sizeof(xpbd::gfx::MeshVertex);
  const std::size_t staticGroundOverlayVertices =
      staticGroundFrame.overlays.solid.size() +
      staticGroundFrame.overlays.transparent.size() +
      staticGroundFrame.overlays.lines.size();
  const std::size_t staticGroundFrameBytes =
      staticGroundFrame.bones.size() * sizeof(xpbd::gfx::StaticModelBoneState) +
      staticGroundOverlayVertices * sizeof(xpbd::gfx::MeshVertex);
  return {{"cube_count", options.cube_count},
          {"texture", {{"width", texture.width}, {"height", texture.height}}},
          {"legacy_per_frame",
           {{"timing", toJson(legacyTiming)},
            {"solid_vertices", legacy.solid.size()},
            {"transparent_vertices", legacy.transparent.size()},
            {"vertex_bytes", legacyBytes}}},
          {"static_indexed_rebuild",
           {{"timing", toJson(indexedTiming)},
            {"vertices", indexed.vertices.size()},
            {"indices", indexed.indices.size()},
            {"faces", indexed.faces.size()},
            {"resource_bytes", indexedBytes}}},
          {"static_steady_frame",
           {{"timing", toJson(staticFrameTiming)},
            {"bone_states", staticFrame.bones.size()},
            {"overlay_vertices", overlayVertices},
            {"upload_bytes", staticFrameBytes}}},
          {"static_ground_steady_frame",
           {{"timing", toJson(staticGroundFrameTiming)},
            {"bone_states", staticGroundFrame.bones.size()},
            {"overlay_vertices", staticGroundOverlayVertices},
            {"upload_bytes", staticGroundFrameBytes}}},
          {"vertex_reduction_ratio",
           indexed.vertices.empty()
               ? 0.0
               : static_cast<double>(legacyVertices) /
                     static_cast<double>(indexed.vertices.size())},
          {"byte_reduction_ratio", indexedBytes == 0
                                       ? 0.0
                                       : static_cast<double>(legacyBytes) /
                                             static_cast<double>(indexedBytes)},
          {"steady_frame_time_reduction_ratio",
           staticFrameTiming.median_ms <= 0.0
               ? 0.0
               : legacyTiming.median_ms / staticFrameTiming.median_ms},
          {"steady_frame_upload_reduction_ratio",
           staticFrameBytes == 0 ? 0.0
                                 : static_cast<double>(legacyBytes) /
                                       static_cast<double>(staticFrameBytes)}};
}

nlohmann::json benchmarkXpbd(const Options &options) {
  if (options.particle_count < 2) {
    usageError("--particles must be at least 2 for the XPBD chain");
  }
  std::vector<std::unique_ptr<xpbd::models::Particle>> particles;
  std::vector<std::unique_ptr<xpbd::constraints::DistanceConstraint>>
      constraints;
  particles.reserve(options.particle_count);
  constraints.reserve(options.particle_count - 1);

  xpbd::core::XpbdEngine engine;
  for (std::size_t index = 0; index < options.particle_count; ++index) {
    const double mass = index == 0 ? 0.0 : 1.0;
    auto particle = std::make_unique<xpbd::models::Particle>(mass);
    particle->position().set(0.0, -static_cast<double>(index), 0.0);
    particle->prevPosition().set(particle->position());
    engine.addParticle(particle.get());
    particles.push_back(std::move(particle));
  }
  for (std::size_t index = 1; index < options.particle_count; ++index) {
    auto constraint = std::make_unique<xpbd::constraints::DistanceConstraint>(
        static_cast<int>(index - 1), static_cast<int>(index), 1.0, 1e-8, 1e-4);
    engine.addConstraint(constraint.get());
    constraints.push_back(std::move(constraint));
  }
  engine.setSolverIterations(8);
  engine.setGravity({0.0, -9.8, 0.0});
  engine.setAerodynamics({2.0, 0.0, -1.0}, 0.05, 0.0);
  constexpr double dt = 1.0 / 60.0;
  const auto timing = measure(options.warmup, options.samples, [&] {
    for (int step = 0; step < options.xpbd_steps_per_sample; ++step) {
      engine.step(dt);
    }
  });

  const auto &tail = particles.back()->position();
  return {{"particle_count", options.particle_count},
          {"constraint_count", constraints.size()},
          {"solver_iterations", 8},
          {"steps_per_sample", options.xpbd_steps_per_sample},
          {"timing", toJson(timing)},
          {"nanoseconds_per_particle_step",
           timing.median_ms * 1.0e6 /
               (static_cast<double>(options.particle_count) *
                static_cast<double>(options.xpbd_steps_per_sample))},
          {"checksum", {tail.x, tail.y, tail.z}}};
}

struct ProfiledBakeRun {
  xpbd::baker::BakeProfiler::Snapshot snapshot;
  std::size_t frame_count = 0;
  int simulation_steps = 0;
};

ProfiledBakeRun runProfiledBake(const Options &options) {
  using xpbd::baker::BakeProfiler;
  using xpbd::baker::BoneMapper;
  using xpbd::baker::PhysicsBaker;
  using xpbd::loader::Animation;
  using xpbd::loader::Bone;

  std::vector<Bone> bones;
  bones.reserve(options.bone_count);
  for (std::size_t index = 0; index < options.bone_count; ++index) {
    Bone bone;
    bone.name = "profile_bone_" + std::to_string(index);
    if (index > 0) {
      bone.has_parent = true;
      bone.parent = "profile_bone_" + std::to_string(index - 1);
    }
    bone.pivot[0] = std::sin(static_cast<double>(index) * 0.13) * 0.25;
    bone.pivot[1] = static_cast<double>(index) * 0.75;
    bone.pivot[2] = std::cos(static_cast<double>(index) * 0.17) * 0.25;
    bones.push_back(std::move(bone));
  }

  BoneMapper mapper(std::move(bones));
  for (const auto &bone : mapper.allBones()) {
    mapper.addPhysicsBone(bone.name);
  }
  if (!mapper.allBones().empty()) {
    BoneMapper::BonePhysicsConfig fixed_root;
    fixed_root.fixed = true;
    mapper.setBoneConfig(mapper.allBones().front().name, &fixed_root);
  }
  auto &config = mapper.config();
  config.loop_mode = BoneMapper::LoopMode::ForceOnce;
  config.transition_duration = 0.0;
  config.solver_iterations = 8;
  config.enable_ground_collision = false;
  config.simd_mode = xpbd::core::SimdMode::Auto;

  constexpr double dt = 1.0 / 60.0;
  Animation animation;
  animation.animation_length =
      static_cast<double>(options.xpbd_steps_per_sample) * dt;
  animation.loop = false;
  animation.loop_behavior = Animation::LoopBehavior::Once;

  const BakeProfiler profiler = BakeProfiler::enabled();
  PhysicsBaker baker(mapper);
  baker.setSourceAnimation(&animation);
  baker.setDt(dt);
  baker.setProfiler(profiler);
  baker.initialize();
  baker.runToEnd();
  return {profiler.snapshot(), baker.frames().size(), baker.totalSteps()};
}

nlohmann::json benchmarkBakeProfile(const Options &options) {
  ProfiledBakeRun last;
  std::vector<double> profiledBakeTotals;
  profiledBakeTotals.reserve(
      static_cast<std::size_t>(options.warmup + options.samples));
  const auto constructionInclusiveTiming =
      measure(options.warmup, options.samples, [&] {
        last = runProfiledBake(options);
        const auto elapsed =
            last.snapshot.stage(xpbd::baker::BakeProfiler::Stage::TotalBake)
                .elapsed;
        profiledBakeTotals.push_back(static_cast<double>(elapsed.count()) *
                                     1.0e-6);
      });
  profiledBakeTotals.erase(profiledBakeTotals.begin(),
                           profiledBakeTotals.begin() +
                               static_cast<std::ptrdiff_t>(options.warmup));
  const Distribution profiledBakeTiming =
      summarize(std::move(profiledBakeTotals));
  const auto &snapshot = last.snapshot;
  const auto totalElapsed =
      snapshot.stage(xpbd::baker::BakeProfiler::Stage::TotalBake).elapsed;
  const double totalNanoseconds = static_cast<double>(totalElapsed.count());

  nlohmann::json stages = nlohmann::json::object();
  for (std::size_t index = 0; index < xpbd::baker::BakeProfiler::kStageCount;
       ++index) {
    const auto stage = static_cast<xpbd::baker::BakeProfiler::Stage>(index);
    const auto &stats = snapshot.stage(stage);
    const double elapsedMilliseconds =
        static_cast<double>(stats.elapsed.count()) * 1.0e-6;
    stages[std::string(xpbd::baker::BakeProfiler::stageName(stage))] = {
        {"calls", stats.calls},
        {"elapsed_ms", elapsedMilliseconds},
        {"share_of_total",
         totalNanoseconds <= 0.0
             ? 0.0
             : static_cast<double>(stats.elapsed.count()) / totalNanoseconds},
    };
  }

  nlohmann::json counters = nlohmann::json::object();
  for (int id = 0;
       id < static_cast<int>(xpbd::baker::BakeProfiler::Counter::Count); ++id) {
    counters[std::string(xpbd::baker::BakeProfiler::counterName(id))] =
        snapshot.counter(id);
  }
  return {
      {"bone_count", options.bone_count},
      {"requested_steps", options.xpbd_steps_per_sample},
      {"simulation_steps", last.simulation_steps},
      {"output_frames", last.frame_count},
      {"profiled_bake_timing", toJson(profiledBakeTiming)},
      {"construction_plus_bake_timing", toJson(constructionInclusiveTiming)},
      {"stages", std::move(stages)},
      {"counters", std::move(counters)},
  };
}

xpbd::rigidbody::SnapshotLevel
rigidSnapshotLevel(const std::string &diagnostics) {
  if (diagnostics == "none") {
    return xpbd::rigidbody::SnapshotLevel::None;
  }
  if (diagnostics == "full") {
    return xpbd::rigidbody::SnapshotLevel::FullDiagnostics;
  }
  return xpbd::rigidbody::SnapshotLevel::ContactsOnly;
}

struct RigidBenchmarkSample {
  double construction_ms = 0.0;
  double advance_ms = 0.0;
  double capture_ms = 0.0;
  std::size_t output_count = 0;
  std::uint64_t trace_captured = 0;
  std::uint64_t trace_dropped = 0;
  std::size_t trace_retained = 0;
  double checksum = 0.0;
};

RigidBenchmarkSample runRigidBenchmarkSample(const Options &options) {
  using xpbd::baker::BoneMapper;
  using xpbd::baker::BonePoseCalculator;
  using xpbd::loader::Bone;
  using xpbd::loader::Cube;
  using xpbd::rigidbody::RigidBodyBakeSession;

  const auto constructionStart = Clock::now();
  std::vector<Bone> bones;
  bones.reserve(options.bone_count);
  constexpr std::size_t kBodiesPerRow = 16;
  constexpr double kSpacing = 3.0;
  for (std::size_t index = 0; index < options.bone_count; ++index) {
    Bone bone;
    bone.name = "rigid_body_" + std::to_string(index);
    bone.pivot[0] =
        static_cast<double>(index % kBodiesPerRow) * kSpacing;
    bone.pivot[1] =
        20.0 +
        static_cast<double>((index / kBodiesPerRow) % kBodiesPerRow) *
            kSpacing;
    bone.pivot[2] =
        static_cast<double>(index / (kBodiesPerRow * kBodiesPerRow)) *
        kSpacing;
    Cube cube;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      cube.origin[axis] = bone.pivot[axis] - 0.5;
      cube.size[axis] = 1.0;
    }
    bone.cubes.push_back(std::move(cube));
    bones.push_back(std::move(bone));
  }

  BoneMapper mapper(std::move(bones));
  for (std::size_t index = 0; index < mapper.allBones().size(); ++index) {
    const auto &bone = mapper.allBones()[index];
    mapper.addPhysicsBone(bone.name);
    BoneMapper::BonePhysicsConfig perBody;
    perBody.particle_mass =
        0.75 + static_cast<double>(index % 5U) * 0.125;
    perBody.animation_pull_compliance =
        0.025 + static_cast<double>(index % 3U) * 0.01;
    perBody.gravity_scale =
        0.8 + static_cast<double>(index % 4U) * 0.1;
    perBody.wind_influence =
        0.7 + static_cast<double>(index % 5U) * 0.075;
    perBody.turbulence_influence =
        0.6 + static_cast<double>(index % 7U) * 0.05;
    perBody.fixed = false;
    mapper.setBoneConfig(bone.name, &perBody);
  }
  auto &config = mapper.config();
  config.simulation_mode = BoneMapper::SimulationMode::RigidBody;
  config.loop_mode = BoneMapper::LoopMode::ForceOnce;
  config.transition_duration = 0.0;
  config.rigid_body_substeps = options.rigid_substeps;
  config.rigid_body_unit_scale = 1.0;
  config.rigid_body_ccd = false;
  config.enable_ground_collision = false;
  config.gravity_y = -9.8;
  config.wind_speed = 5.0;
  config.wind_direction_degrees = 37.0;
  config.wind_elevation_degrees = 11.0;
  config.air_drag = 0.35;
  config.turbulence = 0.8;
  config.rigid_body_snapshot_level =
      rigidSnapshotLevel(options.diagnostics);
  config.rigid_body_step_trace_enabled =
      config.rigid_body_snapshot_level ==
      xpbd::rigidbody::SnapshotLevel::FullDiagnostics;
  config.rigid_body_step_trace_capacity = options.trace_capacity;

  const auto reference =
      BonePoseCalculator::calculate(mapper.allBones(), nullptr, 0.0);
  auto session = RigidBodyBakeSession::create(mapper, nullptr, reference);
  const auto constructionEnd = Clock::now();

  RigidBenchmarkSample result;
  result.construction_ms =
      std::chrono::duration<double, std::milli>(constructionEnd -
                                                constructionStart)
          .count();
  constexpr double kOutputDt = 1.0 / 60.0;
  std::vector<RigidBodyBakeSession::BoneOutput> outputs;
  for (int step = 0; step < options.xpbd_steps_per_sample; ++step) {
    const double startTime = static_cast<double>(step) * kOutputDt;
    const double endTime = static_cast<double>(step + 1) * kOutputDt;
    const auto advanceStart = Clock::now();
    session->advance(startTime, endTime, kOutputDt, true, 0.0, &reference);
    const auto advanceEnd = Clock::now();
    result.advance_ms +=
        std::chrono::duration<double, std::milli>(advanceEnd - advanceStart)
            .count();

    const auto captureStart = Clock::now();
    session->captureBoneOutputsInto(reference, outputs);
    const auto captureEnd = Clock::now();
    result.capture_ms +=
        std::chrono::duration<double, std::milli>(captureEnd - captureStart)
            .count();
  }

  result.output_count = outputs.size();
  for (const auto &output : outputs) {
    result.checksum +=
        static_cast<double>(output.bone_name.size()) * 1.0e-6 +
        output.position[0] + output.position[1] * 0.5 +
        output.position[2] * 0.25 + output.rotation[0] * 0.125 +
        output.rotation[1] * 0.0625 + output.rotation[2] * 0.03125 +
        output.linear_velocity[0] * 0.015625 +
        output.linear_velocity[1] * 0.0078125 +
        output.linear_velocity[2] * 0.00390625 +
        output.world_position[0] * 0.001953125 +
        output.world_position[1] * 0.0009765625 +
        output.world_position[2] * 0.00048828125;
  }
  const auto &trace = session->stepTrace();
  result.trace_captured = trace.captured_sample_count;
  result.trace_dropped = trace.dropped_sample_count;
  result.trace_retained = trace.samples.size();
  return result;
}

nlohmann::json benchmarkRigid(const Options &options) {
  std::vector<double> constructionTimings;
  std::vector<double> advanceTimings;
  std::vector<double> captureTimings;
  std::vector<double> hotLoopTimings;
  std::vector<double> totalTimings;
  constructionTimings.reserve(static_cast<std::size_t>(options.samples));
  advanceTimings.reserve(static_cast<std::size_t>(options.samples));
  captureTimings.reserve(static_cast<std::size_t>(options.samples));
  hotLoopTimings.reserve(static_cast<std::size_t>(options.samples));
  totalTimings.reserve(static_cast<std::size_t>(options.samples));

  RigidBenchmarkSample last;
  const int runCount = options.warmup + options.samples;
  for (int run = 0; run < runCount; ++run) {
    last = runRigidBenchmarkSample(options);
    if (run < options.warmup) {
      continue;
    }
    constructionTimings.push_back(last.construction_ms);
    advanceTimings.push_back(last.advance_ms);
    captureTimings.push_back(last.capture_ms);
    hotLoopTimings.push_back(last.advance_ms + last.capture_ms);
    totalTimings.push_back(last.construction_ms + last.advance_ms +
                           last.capture_ms);
  }

  const Distribution construction =
      summarize(std::move(constructionTimings));
  const Distribution advance = summarize(std::move(advanceTimings));
  const Distribution capture = summarize(std::move(captureTimings));
  const Distribution hotLoop = summarize(std::move(hotLoopTimings));
  const Distribution total = summarize(std::move(totalTimings));
  const double bodySubsteps =
      static_cast<double>(options.bone_count) *
      static_cast<double>(options.xpbd_steps_per_sample) *
      static_cast<double>(options.rigid_substeps);
  return {
      {"body_count", options.bone_count},
      {"output_steps", options.xpbd_steps_per_sample},
      {"fixed_substeps", options.rigid_substeps},
      {"snapshot_level",
       xpbd::rigidbody::snapshotLevelName(
           rigidSnapshotLevel(options.diagnostics))},
      {"trace_capacity", options.trace_capacity},
      {"construction_timing", toJson(construction)},
      {"advance_timing", toJson(advance)},
      {"capture_timing", toJson(capture)},
      {"hot_loop_timing", toJson(hotLoop)},
      {"construction_plus_hot_loop_timing", toJson(total)},
      {"advance_nanoseconds_per_body_substep",
       bodySubsteps <= 0.0 ? 0.0
                           : advance.median_ms * 1.0e6 / bodySubsteps},
      {"output_count", last.output_count},
      {"trace",
       {{"captured", last.trace_captured},
        {"dropped", last.trace_dropped},
        {"retained", last.trace_retained}}},
      {"checksum", last.checksum},
  };
}

struct FrameLayoutBenchmarkFixture {
  std::vector<xpbd::baker::BakedFrame> frames;
  std::map<std::string, xpbd::loader::Bone> bones_by_name;
  std::set<std::string> corrected_bones;
  double clip_length = 0.0;
};

FrameLayoutBenchmarkFixture
makeFrameLayoutBenchmarkFixture(const Options &options) {
  FrameLayoutBenchmarkFixture fixture;
  fixture.clip_length =
      static_cast<double>(options.xpbd_steps_per_sample) / 60.0;
  std::vector<std::string> boneNames;
  boneNames.reserve(options.bone_count);
  for (std::size_t boneIndex = 0; boneIndex < options.bone_count;
       ++boneIndex) {
    std::ostringstream name;
    name << "bone_" << std::setw(6) << std::setfill('0') << boneIndex;
    boneNames.push_back(name.str());
    xpbd::loader::Bone bone;
    bone.name = boneNames.back();
    bone.rotation[0] = static_cast<double>(boneIndex % 17U) * 0.125;
    bone.rotation[1] = static_cast<double>(boneIndex % 13U) * -0.1;
    bone.rotation[2] = static_cast<double>(boneIndex % 11U) * 0.075;
    fixture.bones_by_name.emplace(bone.name, bone);
    fixture.corrected_bones.insert(bone.name);
  }

  const std::size_t frameCount =
      static_cast<std::size_t>(options.xpbd_steps_per_sample) + 1U;
  fixture.frames.reserve(frameCount);
  for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
    xpbd::baker::BakedFrame frame;
    frame.time = static_cast<double>(frameIndex) / 60.0;
    frame.bone_states.reserve(options.bone_count);
    for (std::size_t boneIndex = 0; boneIndex < options.bone_count;
         ++boneIndex) {
      const double phase = static_cast<double>(frameIndex) * 0.017 +
                           static_cast<double>(boneIndex) * 0.013;
      xpbd::baker::BoneState state;
      state.bone_name = boneNames[boneIndex];
      state.position = {std::sin(phase) * 0.25,
                        static_cast<double>(boneIndex) * 0.01 +
                            std::cos(phase * 0.7) * 0.1,
                        std::sin(phase * 0.3) * 0.2};
      state.rotation = {std::sin(phase * 0.5) * 12.0,
                        std::cos(phase * 0.4) * 8.0,
                        std::sin(phase * 0.6) * 5.0};
      state.linear_velocity = {std::cos(phase) * 0.25,
                               -std::sin(phase * 0.7) * 0.07,
                               std::cos(phase * 0.3) * 0.06};
      state.world_position = {
          state.position[0] + static_cast<double>(boneIndex % 7U),
          state.position[1], state.position[2]};
      state.has_world_position = true;
      frame.bone_states.push_back(std::move(state));
    }
    frame.rebuildIndex();
    fixture.frames.push_back(std::move(frame));
  }
  return fixture;
}

double frameLayoutChecksum(
    const std::vector<xpbd::baker::BakedFrame> &frames) {
  double checksum = 0.0;
  for (const auto &frame : frames) {
    checksum += frame.time * 1.0e-7;
    for (const auto &state : frame.bone_states) {
      checksum += static_cast<double>(state.bone_name.size()) * 1.0e-9 +
                  state.position[0] + state.position[1] * 0.5 +
                  state.position[2] * 0.25 + state.rotation[0] * 0.125 +
                  state.rotation[1] * 0.0625 +
                  state.rotation[2] * 0.03125;
    }
  }
  return checksum;
}

nlohmann::json benchmarkFrameLayout(const Options &options) {
  const auto fixture = makeFrameLayoutBenchmarkFixture(options);
  std::vector<xpbd::baker::BakedFrame> resampled;
  const auto resampleTiming = measure(options.warmup, options.samples, [&] {
    resampled = xpbd::baker::OutputTimelineResampler::resample(
        fixture.frames, fixture.bones_by_name, 1.0 / 120.0,
        fixture.clip_length, xpbd::baker::OutputEndpointPolicy::Closed);
  });

  xpbd::baker::LoopSeamCorrector::Result corrected;
  const auto seamTiming = measure(options.warmup, options.samples, [&] {
    corrected = xpbd::baker::LoopSeamCorrector::correctCopy(
        fixture.frames, fixture.bones_by_name, fixture.corrected_bones, 0.25,
        true);
  });

  return {
      {"bone_count", options.bone_count},
      {"source_frame_count", fixture.frames.size()},
      {"resampled_frame_count", resampled.size()},
      {"resample_timing", toJson(resampleTiming)},
      {"loop_seam_timing", toJson(seamTiming)},
      {"resample_checksum", frameLayoutChecksum(resampled)},
      {"loop_seam_checksum", frameLayoutChecksum(corrected.frames)},
  };
}

std::vector<xpbd::loader::Bone> makeReconstructionBones(std::size_t count) {
  std::vector<xpbd::loader::Bone> bones;
  bones.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    xpbd::loader::Bone bone;
    bone.name = "bone_" + std::to_string(index);
    if (index > 0) {
      bone.has_parent = true;
      bone.parent = "bone_" + std::to_string(index - 1);
    }
    bone.pivot[0] = static_cast<double>(index % 7) * 0.125;
    bone.pivot[1] = static_cast<double>(index) * 0.25;
    bone.pivot[2] = static_cast<double>(index % 5) * -0.2;
    bone.rotation[0] = static_cast<double>(index % 11) * 0.15;
    bone.rotation[1] = static_cast<double>(index % 13) * -0.1;
    bone.rotation[2] = static_cast<double>(index % 17) * 0.075;
    bones.push_back(std::move(bone));
  }
  return bones;
}

nlohmann::json benchmarkReconstruction(const Options &options) {
  auto bones = makeReconstructionBones(options.bone_count);
  const auto reference =
      xpbd::baker::BonePoseCalculator::calculate(bones, nullptr, 0.0);
  std::map<std::string, xpbd::baker::FinalPoseReconstructor::WorldTarget>
      targets;
  for (std::size_t index = 0; index < bones.size(); index += 3) {
    const auto found = reference.find(bones[index].name);
    if (found == reference.end()) {
      continue;
    }
    xpbd::baker::FinalPoseReconstructor::WorldTarget target;
    target.position = found->second.world_position;
    target.position[0] += std::sin(static_cast<double>(index) * 0.17) * 0.05;
    target.position[2] += std::cos(static_cast<double>(index) * 0.11) * 0.05;
    if ((index & 1u) == 0u) {
      target.rotation = found->second.world_rotation;
    }
    targets.emplace(bones[index].name, target);
  }

  xpbd::baker::FinalPoseReconstructor::Result result;
  const auto compileAndReconstructTiming =
      measure(options.warmup, options.samples, [&] {
        result = xpbd::baker::FinalPoseReconstructor::reconstruct(
            bones, reference, targets);
      });
  const auto evaluator = xpbd::baker::FinalPoseReconstructor::compile(bones);
  const auto compiledTiming = measure(options.warmup, options.samples, [&] {
    result = evaluator.reconstruct(reference, targets);
  });
  xpbd::baker::FinalPoseReconstructor::Evaluator::ReconstructionScratch
      scratch;
  const auto scratchTiming = measure(options.warmup, options.samples, [&] {
    (void)evaluator.reconstructInto(reference, targets, scratch);
  });
  double checksum = 0.0;
  for (const auto &[name, pose] : scratch.result().world_poses) {
    checksum += static_cast<double>(name.size()) * 1e-6 +
                pose.world_position[0] + pose.world_position[1] * 0.5 +
                pose.world_position[2] * 0.25;
  }
  return {{"bone_count", bones.size()},
          {"target_count", targets.size()},
          {"compile_and_reconstruct",
           {{"timing", toJson(compileAndReconstructTiming)}}},
          {"compiled_reconstruct", {{"timing", toJson(compiledTiming)}}},
          {"compiled_reconstruct_into",
           {{"timing", toJson(scratchTiming)}}},
          {"compiled_speedup", compiledTiming.median_ms <= 0.0
                                   ? 0.0
                                   : compileAndReconstructTiming.median_ms /
                                         compiledTiming.median_ms},
          {"scratch_speedup_vs_compiled",
           scratchTiming.median_ms <= 0.0
               ? 0.0
               : compiledTiming.median_ms / scratchTiming.median_ms},
          {"checksum", checksum}};
}

nlohmann::json benchmarkFinalAudit(const Options &options) {
  using xpbd::baker::BonePoseCalculator;
  using xpbd::baker::RigidBodyCollisionAuditor;
  using xpbd::loader::Bone;
  using xpbd::rigidbody::BodyDefinition;
  using xpbd::rigidbody::BoxShape;
  using xpbd::rigidbody::MotionType;

  std::vector<Bone> bones;
  std::map<std::string, BodyDefinition> bodies;
  std::map<std::string, BonePoseCalculator::Pose> poses;
  bones.reserve(options.bone_count);
  constexpr double pi = 3.14159265358979323846;
  for (std::size_t index = 0; index < options.bone_count; ++index) {
    const std::string name = "audit_body_" + std::to_string(index);
    Bone bone;
    bone.name = name;
    bones.push_back(std::move(bone));

    BodyDefinition body;
    body.name = name;
    body.motion_type = MotionType::Dynamic;
    BoxShape box;
    box.half_extents = {0.9 + static_cast<double>(index % 5u) * 0.025,
                        0.95 + static_cast<double>(index % 7u) * 0.02,
                        1.0 + static_cast<double>(index % 3u) * 0.03};
    body.boxes.push_back(box);
    bodies.emplace(name, std::move(body));

    BonePoseCalculator::Pose pose;
    pose.world_translation = {static_cast<double>(index % 4u) * 0.10,
                              static_cast<double>((index / 4u) % 4u) * 0.08,
                              static_cast<double>((index / 16u) % 4u) * 0.06};
    pose.world_position = pose.world_translation;
    std::array<double, 3> rotation_axis{0.25 + static_cast<double>(index % 3u),
                                        0.5 + static_cast<double>(index % 5u),
                                        0.75 + static_cast<double>(index % 7u)};
    const double axis_length = std::sqrt(rotation_axis[0] * rotation_axis[0] +
                                         rotation_axis[1] * rotation_axis[1] +
                                         rotation_axis[2] * rotation_axis[2]);
    for (double &component : rotation_axis) {
      component /= axis_length;
    }
    const double half_angle =
        (static_cast<double>((index * 37u) % 180u) * pi / 180.0) * 0.5;
    const double sine = std::sin(half_angle);
    pose.world_rotation = {rotation_axis[0] * sine, rotation_axis[1] * sine,
                           rotation_axis[2] * sine, std::cos(half_angle)};
    poses.emplace(name, pose);
  }

  RigidBodyCollisionAuditor auditor(bones, bodies, {}, 1.0, 1.0e9, false);
  RigidBodyCollisionAuditor::AuditResult audit;
  const auto timing = measure(options.warmup, options.samples,
                              [&] { audit = auditor.audit(poses); });
  return {
      {"body_count", options.bone_count},
      {"timing", toJson(timing)},
      {"all_possible_pairs", audit.counters.all_possible_pairs},
      {"broad_phase_candidates", audit.counters.broad_phase_candidates},
      {"sat_calls", audit.counters.sat_calls},
      {"maximum_penetration", audit.maximum_penetration},
      {"unsafe", audit.unsafe},
  };
}

nlohmann::json benchmarkSimd(const Options &options) {
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||                \
    defined(__x86_64__)
  std::vector<double> base(options.value_count);
  std::vector<double> delta(options.value_count);
  std::vector<double> output(options.value_count);
  for (std::size_t index = 0; index < options.value_count; ++index) {
    base[index] = static_cast<double>(index % 1021) * 0.001 - 0.5;
    delta[index] = static_cast<double>(index % 509) * -0.002 + 0.25;
  }
  constexpr double scale = 0.375;
  const auto run = [&](xpbd::core::detail::DenseScaledAddKernel kernel) {
    return measure(options.warmup, options.samples, [&] {
      for (int iteration = 0; iteration < options.kernel_iterations;
           ++iteration) {
        kernel(output.data(), base.data(), delta.data(), scale,
               options.value_count);
      }
    });
  };

  const auto sse2 = run(xpbd::core::detail::denseScaledAddSse2);
  const auto capabilities = xpbd::core::detectSimdCapabilities();
  nlohmann::json result = {
      {"available", true},
      {"value_count", options.value_count},
      {"iterations_per_sample", options.kernel_iterations},
      {"sse2", {{"timing", toJson(sse2)}}},
      {"avx2_usable", capabilities.avx2Usable()},
  };
  if (capabilities.avx2Usable()) {
    const auto avx2 = run(xpbd::core::detail::denseScaledAddAvx2);
    result["avx2"] = {{"timing", toJson(avx2)}};
    result["avx2_speedup"] =
        avx2.median_ms <= 0.0 ? 0.0 : sse2.median_ms / avx2.median_ms;
    result["meets_1_25x_kernel_gate"] =
        avx2.median_ms > 0.0 && sse2.median_ms / avx2.median_ms >= 1.25;
  }
  double checksum = 0.0;
  for (std::size_t index = 0; index < output.size(); index += 4093) {
    checksum += output[index];
  }
  result["checksum"] = checksum;

  struct ProjectionCase {
    std::array<double, 24> first{};
    std::array<double, 24> second{};
    std::array<double, 3> axis{};
  };
  const std::size_t projectionCaseCount =
      std::clamp(options.value_count / 24u, std::size_t{1}, std::size_t{65536});
  std::vector<ProjectionCase> projectionCases(projectionCaseCount);
  for (std::size_t sample = 0; sample < projectionCases.size(); ++sample) {
    auto &entry = projectionCases[sample];
    for (std::size_t vertex = 0; vertex < 8; ++vertex) {
      const double phase =
          static_cast<double>((sample * 17u + vertex * 13u) % 65521u);
      entry.first[vertex] = std::sin(phase * 0.017) * 4.0 - 0.25;
      entry.first[8 + vertex] = std::cos(phase * 0.011) * 2.0 + 0.5;
      entry.first[16 + vertex] = std::sin(phase * 0.007 + 0.3) * 3.0;
      entry.second[vertex] = std::cos(phase * 0.013 + 0.2) * 3.5 + 0.75;
      entry.second[8 + vertex] = std::sin(phase * 0.019 - 0.1) * 2.5 - 0.4;
      entry.second[16 + vertex] = std::cos(phase * 0.005 + 0.6) * 4.5;
    }
    entry.axis = {std::sin(static_cast<double>(sample) * 0.31) + 0.25,
                  std::cos(static_cast<double>(sample) * 0.19) - 0.5,
                  std::sin(static_cast<double>(sample) * 0.07 + 0.8) + 0.125};
    const double length = std::sqrt(entry.axis[0] * entry.axis[0] +
                                    entry.axis[1] * entry.axis[1] +
                                    entry.axis[2] * entry.axis[2]);
    for (double &component : entry.axis) {
      component /= length;
    }
  }

  double projectionChecksum = 0.0;
  const auto runProjection =
      [&](xpbd::core::detail::BoxProjectionOverlapKernel kernel) {
        return measure(options.warmup, options.samples, [&] {
          double sum = 0.0;
          for (int iteration = 0; iteration < options.kernel_iterations;
               ++iteration) {
            for (const auto &entry : projectionCases) {
              sum += kernel(entry.first.data(), entry.second.data(),
                            entry.axis.data());
            }
          }
          projectionChecksum = sum;
        });
      };
  const auto projectionScalar =
      runProjection(xpbd::core::detail::boxProjectionOverlapScalar);
  const auto projectionSse2 =
      runProjection(xpbd::core::detail::boxProjectionOverlapSse2);
  nlohmann::json projection = {
      {"case_count", projectionCases.size()},
      {"iterations_per_sample", options.kernel_iterations},
      {"scalar", {{"timing", toJson(projectionScalar)}}},
      {"sse2", {{"timing", toJson(projectionSse2)}}},
      {"sse2_vs_scalar_speedup",
       projectionSse2.median_ms <= 0.0
           ? 0.0
           : projectionScalar.median_ms / projectionSse2.median_ms},
      {"sse2_meets_1_25x_over_scalar_gate",
       projectionSse2.median_ms > 0.0 &&
           projectionScalar.median_ms / projectionSse2.median_ms >= 1.25},
      {"fastest_usable_kernel",
       projectionSse2.median_ms < projectionScalar.median_ms ? "sse2"
                                                             : "scalar"},
  };
  if (capabilities.avx2Usable()) {
    const auto projectionAvx2 =
        runProjection(xpbd::core::detail::boxProjectionOverlapAvx2);
    const double avx2VsScalar =
        projectionAvx2.median_ms <= 0.0
            ? 0.0
            : projectionScalar.median_ms / projectionAvx2.median_ms;
    const double avx2VsSse2 =
        projectionAvx2.median_ms <= 0.0
            ? 0.0
            : projectionSse2.median_ms / projectionAvx2.median_ms;
    projection["avx2"] = {{"timing", toJson(projectionAvx2)}};
    projection["avx2_vs_scalar_speedup"] = avx2VsScalar;
    projection["avx2_vs_sse2_speedup"] = avx2VsSse2;
    projection["meets_1_25x_over_scalar_gate"] = avx2VsScalar >= 1.25;
    projection["meets_1_25x_over_sse2_gate"] = avx2VsSse2 >= 1.25;
    if (projectionAvx2.median_ms <
        std::min(projectionScalar.median_ms, projectionSse2.median_ms)) {
      projection["fastest_usable_kernel"] = "avx2";
    }
  }
  projection["checksum"] = projectionChecksum;
  result["box_projection_overlap"] = std::move(projection);
  return result;
#else
  (void)options;
  return {{"available", false}, {"reason", "non-x86 build"}};
#endif
}

std::string compilerName() {
#if defined(__clang__)
  return "clang " + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__) + "." +
         std::to_string(__clang_patchlevel__);
#elif defined(_MSC_VER)
  return "msvc " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
  return "gcc " + std::to_string(__GNUC__) + "." +
         std::to_string(__GNUC_MINOR__) + "." +
         std::to_string(__GNUC_PATCHLEVEL__);
#else
  return "unknown";
#endif
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    nlohmann::json report = {{"schema", "xpbd.performance.v1"},
                             {"compiler", compilerName()},
                             {"build_type",
#if defined(NDEBUG)
                              "Release"
#else
                              "Debug"
#endif
                             },
                             {"warmup", options.warmup},
                             {"samples", options.samples}};
    if (options.mode == "all" || options.mode == "uv") {
      report["uv"] = benchmarkUv(options);
    }
    if (options.mode == "all" || options.mode == "xpbd") {
      report["xpbd"] = benchmarkXpbd(options);
    }
    if (options.mode == "all" || options.mode == "reconstruct") {
      report["reconstruction"] = benchmarkReconstruction(options);
    }
    if (options.mode == "bake") {
      report["bake_profile"] = benchmarkBakeProfile(options);
    }
    if (options.mode == "rigid") {
      report["rigid"] = benchmarkRigid(options);
    }
    if (options.mode == "frame-layout") {
      report["stable_frame_layout"] = benchmarkFrameLayout(options);
    }
    if (options.mode == "audit") {
      report["final_collision_audit"] = benchmarkFinalAudit(options);
    }
    if (options.mode == "all" || options.mode == "simd") {
      report["simd"] = benchmarkSimd(options);
    }

    const std::string encoded = report.dump(2) + "\n";
    std::cout << encoded;
    if (!options.output.empty()) {
      if (options.output.has_parent_path()) {
        std::filesystem::create_directories(options.output.parent_path());
      }
      std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
      if (!output) {
        throw std::runtime_error("cannot open benchmark output: " +
                                 options.output.string());
      }
      output << encoded;
      if (!output) {
        throw std::runtime_error("cannot write benchmark output: " +
                                 options.output.string());
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "xpbd_perf_benchmark: " << error.what() << '\n';
    return 2;
  }
}
