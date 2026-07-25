#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/physics_baker.hpp"
#include "xpbd/baker/rigid_body_joint_auditor.hpp"
#include "xpbd/baker/rigid_body_input_compat.hpp"
#include "xpbd/export/animation_exporter.hpp"
#include "xpbd/export/velocity_cache_exporter.hpp"
#include "xpbd/loader/animation_loader.hpp"
#include "xpbd/loader/model_loader.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

double parseStrictCliNumber(const std::string &text, const char *field) {
  try {
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    while (consumed < text.size() &&
           std::isspace(static_cast<unsigned char>(text[consumed]))) {
      ++consumed;
    }
    if (consumed != text.size()) {
      throw std::runtime_error(std::string(field) +
                               " must be a complete numeric value");
    }
    if (!std::isfinite(value)) {
      throw std::runtime_error(std::string(field) + " must be finite");
    }
    return value;
  } catch (const std::invalid_argument &) {
    throw std::runtime_error(std::string(field) + " must be numeric");
  } catch (const std::out_of_range &) {
    throw std::runtime_error(std::string(field) + " must be finite");
  }
}

void printUsage(const char *argv0) {
  std::cerr
      << "xpbd_cli — headless XPBD / Bullet bone bake\n\n"
      << "Usage:\n"
      << "  " << argv0 << " bake --model <geo.json> --anim <animation.json>\n"
      << "      --out <baked.animation.json> [options]\n\n"
      << "Options:\n"
      << "  --source <clip>     Clip name when the animation file contains "
         "several\n"
      << "  --bones <a,b,c>     Physics bones (default: all non-root children "
         "+ roots)\n"
      << "  --mode xpbd|bullet  Solver (default: xpbd)\n"
      << "  --loop auto|once|loop  Loop mode (default: auto)\n"
      << "  --dt <seconds>      Step dt (default: 0.0166667)\n"
      << "  --assume-molang-zero  Explicitly use zero for unsupported "
         "position/rotation Molang\n"
      << "  --diagnostics        Capture and print full rigid-body joint "
         "diagnostics\n"
      << "  --velocity <path>   Also write velocity cache JSON\n"
      << "  --id <anim_id>      Output animation id (default: <source>.baked)\n"
      << "  -h, --help          Show this help\n\n"
      << "Example:\n"
      << "  " << argv0
      << " bake --model model.geo.json --anim idle.animation.json "
         "--out idle.baked.json --bones mid,tip\n";
}

void printRigidBodyDiagnostics(const xpbd::baker::PhysicsBaker &baker,
                               const xpbd::baker::BoneMapper &mapper) {
  std::cerr << "diagnostics.final_joints unsafe_samples="
            << baker.getUnsafeFinalJointCount()
            << " maximum_anchor_separation="
            << baker.getMaximumFinalJointAnchorSeparation()
            << " worst_pair=" << baker.getWorstFinalJointLinearParent() << "->"
            << baker.getWorstFinalJointLinearChild()
            << " maximum_angular_excess="
            << baker.getMaximumFinalJointAngularExcessRadians() << "\n";

  const auto trace = baker.getRigidBodyStepTrace();
  if (!trace || !trace->enabled) {
    std::cerr << "diagnostics.live_trace unavailable\n";
    return;
  }

  using Pair = std::pair<std::string, std::string>;
  struct Maximum {
    double separation = 0.0;
    double time = 0.0;
    std::uint64_t output_step = 0;
    int substep = 0;
  };
  std::map<Pair, Maximum> maxima;
  for (const auto &sample : trace->samples) {
    for (const auto &joint : sample.joint_errors) {
      auto &maximum = maxima[{joint.parent_body, joint.child_body}];
      if (joint.anchor_separation > maximum.separation) {
        maximum.separation = joint.anchor_separation;
        maximum.time = sample.sample_time;
        maximum.output_step = sample.output_step_index;
        maximum.substep = sample.substep_index;
      }
    }
  }
  std::cerr << "diagnostics.live_trace captured="
            << trace->captured_sample_count
            << " retained=" << trace->samples.size()
            << " dropped=" << trace->dropped_sample_count << "\n";
  for (const auto &[pair, maximum] : maxima) {
    std::cerr << "diagnostics.live_joint pair=" << pair.first << "->"
              << pair.second << " maximum_anchor_separation="
              << maximum.separation << " time=" << maximum.time
              << " output_step=" << maximum.output_step
              << " substep=" << maximum.substep << "\n";
  }

  struct FinalMaximum {
    double separation = 0.0;
    double time = 0.0;
    std::size_t unsafe_samples = 0;
  };
  std::map<Pair, FinalMaximum> finalMaxima;
  xpbd::baker::RigidBodyJointAuditor finalAuditor(
      mapper, baker.sampleOutputReferencePoses(0.0));
  for (const auto &frame : baker.frames()) {
    const auto audit = finalAuditor.auditQuantizedFrame(
        frame, baker.getOutputReferenceAnimation(),
        baker.getOutputReferenceTime(frame.time));
    for (const auto &violation : audit.violations) {
      auto &maximum =
          finalMaxima[{violation.parent_bone, violation.child_bone}];
      ++maximum.unsafe_samples;
      if (violation.linear_anchor_separation > maximum.separation) {
        maximum.separation = violation.linear_anchor_separation;
        maximum.time = frame.time;
      }
    }
  }
  for (const auto &[pair, maximum] : finalMaxima) {
    std::cerr << "diagnostics.final_joint pair=" << pair.first << "->"
              << pair.second << " maximum_anchor_separation="
              << maximum.separation << " time=" << maximum.time
              << " unsafe_discrete_frames=" << maximum.unsafe_samples << "\n";
  }

  const auto &frames = baker.frames();
  const auto printEndpoint = [&](const char *label,
                                 const xpbd::baker::BakedFrame &frame) {
    for (const auto &state : frame.bone_states) {
      std::cerr << "diagnostics.endpoint label=" << label
                << " time=" << frame.time << " bone=" << state.bone_name
                << " position=" << state.position[0] << ","
                << state.position[1] << "," << state.position[2]
                << " rotation=" << state.rotation[0] << ","
                << state.rotation[1] << "," << state.rotation[2]
                << " world_position=" << state.world_position[0] << ","
                << state.world_position[1] << "," << state.world_position[2]
                << "\n";
    }
  };
  if (!frames.empty()) {
    printEndpoint("first", frames.front());
    if (frames.size() > 1) {
      printEndpoint("penultimate", frames[frames.size() - 2]);
      printEndpoint("last", frames.back());
    }
  }
}

std::vector<std::string> splitCsv(const std::string &s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {

    const auto a = item.find_first_not_of(" \t");
    const auto b = item.find_last_not_of(" \t");
    if (a == std::string::npos) {
      continue;
    }
    out.push_back(item.substr(a, b - a + 1));
  }
  return out;
}

int cmdBake(int argc, char **argv) {
  std::filesystem::path model_path;
  std::filesystem::path anim_path;
  std::filesystem::path out_path;
  std::filesystem::path velocity_path;
  std::string bones_csv;
  std::string source_clip;
  std::string mode = "xpbd";
  std::string loop_mode = "auto";
  std::string anim_id;
  bool assume_molang_zero = false;
  bool full_diagnostics = false;
  double dt = 1.0 / 60.0;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char *name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[++i];
    };
    if (arg == "--model") {
      model_path = need("--model");
    } else if (arg == "--anim") {
      anim_path = need("--anim");
    } else if (arg == "--out") {
      out_path = need("--out");
    } else if (arg == "--velocity") {
      velocity_path = need("--velocity");
    } else if (arg == "--source") {
      source_clip = need("--source");
    } else if (arg == "--bones") {
      bones_csv = need("--bones");
    } else if (arg == "--mode") {
      mode = need("--mode");
    } else if (arg == "--loop") {
      loop_mode = need("--loop");
    } else if (arg == "--dt") {
      dt = parseStrictCliNumber(need("--dt"), "--dt");
    } else if (arg == "--assume-molang-zero") {
      assume_molang_zero = true;
    } else if (arg == "--diagnostics") {
      full_diagnostics = true;
    } else if (arg == "--id") {
      anim_id = need("--id");
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (model_path.empty() || anim_path.empty() || out_path.empty()) {
    throw std::runtime_error("--model, --anim, and --out are required");
  }

  const auto geometry = xpbd::loader::ModelLoader::load(model_path);
  const auto anim_root = xpbd::loader::AnimationLoader::load(anim_path);
  if (anim_root.animations.empty()) {
    throw std::runtime_error("animation file has no clips");
  }
  std::string source_name;
  const xpbd::loader::Animation *source_anim = nullptr;
  if (!source_clip.empty()) {
    const auto selected = anim_root.animations.find(source_clip);
    if (selected == anim_root.animations.end()) {
      throw std::runtime_error("animation clip not found: " + source_clip);
    }
    source_name = selected->first;
    source_anim = &selected->second;
  } else {
    source_name = anim_root.animation_order.empty()
                      ? anim_root.animations.begin()->first
                      : anim_root.animation_order.front();
    source_anim = &anim_root.animations.at(source_name);
    if (anim_root.animations.size() > 1) {
      std::cerr
          << "note: multiple animations found; using first source-order clip: "
          << source_name << " (override with --source)\n";
    }
  }

  xpbd::baker::BoneMapper mapper(geometry.bones);
  if (!bones_csv.empty()) {
    for (const auto &name : splitCsv(bones_csv)) {
      mapper.addPhysicsBone(name);
    }
  } else {
    for (const auto &bone : geometry.bones) {
      mapper.addPhysicsBone(bone.name);
    }
  }
  if (mapper.physicsBones().empty()) {
    throw std::runtime_error("no physics bones selected");
  }

  auto &cfg = mapper.config();
  cfg.allow_input_only_molang_zero_fallback = assume_molang_zero;
  cfg.allow_selected_molang_zero_fallback = assume_molang_zero;
  if (assume_molang_zero) {
    std::cerr << "warning: unsupported position/rotation Molang is explicitly "
                 "sampled as "
                 "zero\n";
  }
  if (mode == "bullet" || mode == "rigid" || mode == "rb") {
    cfg.simulation_mode = xpbd::baker::BoneMapper::SimulationMode::RigidBody;
  } else if (mode == "xpbd") {
    cfg.simulation_mode = xpbd::baker::BoneMapper::SimulationMode::Xpbd;
  } else {
    throw std::runtime_error("unknown --mode (use xpbd or bullet)");
  }
  if (full_diagnostics) {
    cfg.rigid_body_snapshot_level =
        xpbd::rigidbody::SnapshotLevel::FullDiagnostics;
    cfg.rigid_body_step_trace_capacity = 4096;
  }
  if (loop_mode == "once") {
    cfg.loop_mode = xpbd::baker::BoneMapper::LoopMode::ForceOnce;
  } else if (loop_mode == "loop") {
    cfg.loop_mode = xpbd::baker::BoneMapper::LoopMode::ForceLoop;
  } else {
    cfg.loop_mode = xpbd::baker::BoneMapper::LoopMode::Auto;
  }

  xpbd::loader::Animation physics_source_animation = *source_anim;
  const auto compatibility =
      xpbd::baker::prepareRigidBodyInputCompatibility(
          mapper, physics_source_animation);
  if (mapper.physicsBones().empty()) {
    throw std::runtime_error(
        "rigid-body compatibility preflight removed every selected physics "
        "bone; select at least one bone that owns a usable cube");
  }
  for (const auto &bone : compatibility.skipped_blocked_dynamic_bones) {
    std::cerr << "warning: auto-skipped blocked empty rigid body: " << bone
              << "\n";
  }
  for (const auto &bone : compatibility.promoted_animated_compound_bones) {
    std::cerr << "warning: promoted animated compound descendant to rigid "
                 "body in physics copy: "
              << bone << "\n";
  }
  for (const auto &bone : compatibility.repaired_source_scale_bones) {
    std::cerr << "warning: repaired degenerate scale in physics copy: "
              << bone << "\n";
  }

  std::cerr << "model:  " << model_path << " (" << geometry.bones.size()
            << " bones)\n";
  std::cerr << "anim:   " << source_name
            << " (len=" << source_anim->animation_length << ")\n";
  std::cerr << "mode:   " << mode
            << "  physics_bones=" << mapper.physicsBones().size() << "\n";

  xpbd::baker::PhysicsBaker baker(mapper);
  baker.setSourceAnimation(&physics_source_animation);
  baker.setDt(dt);
  baker.initialize();

  const int total = baker.totalSteps();
  std::cerr << "bake:   " << total << " steps...\n";
  int last_pct = -1;
  while (baker.currentStep() < baker.totalSteps()) {
    baker.step();
    const int pct = total > 0 ? static_cast<int>(100.0 * baker.currentStep() /
                                                 static_cast<double>(total))
                              : 100;
    if (pct != last_pct && (pct % 10 == 0 || baker.currentStep() == total)) {
      std::cerr << "  " << pct << "%\n";
      last_pct = pct;
    }
  }
  baker.finalizeFrames();
  if (full_diagnostics &&
      cfg.simulation_mode ==
          xpbd::baker::BoneMapper::SimulationMode::RigidBody) {
    printRigidBodyDiagnostics(baker, mapper);
  }
  baker.requireSafeForExport();

  if (anim_id.empty()) {
    anim_id = source_name + ".baked";
  }

  xpbd::export_::AnimationExporter::exportAnimation(
      anim_id, source_anim, baker.frames(), baker.isLooping(), out_path);
  std::cerr << "wrote:  " << out_path << " (" << baker.frames().size()
            << " frames)\n";

  if (!velocity_path.empty()) {
    const double output_interval = baker.outputFrameInterval();
    const double velocity_interval =
        std::isfinite(output_interval) && output_interval > 0.0
            ? output_interval
            : dt;
    xpbd::export_::VelocityCacheExporter::exportCache(
        anim_id, baker.frames(), velocity_interval, velocity_path);
    std::cerr << "wrote:  " << velocity_path << "\n";
  }

  return 0;
}

}

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      printUsage(argv[0]);
      return 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "bake") {
      return cmdBake(argc, argv);
    }
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
      printUsage(argv[0]);
      return 0;
    }
    std::cerr << "unknown command: " << cmd << "\n\n";
    printUsage(argv[0]);
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
