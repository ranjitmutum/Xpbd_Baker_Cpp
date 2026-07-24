#include "xpbd/export/velocity_cache_exporter.hpp"

#include "xpbd/export/atomic_file_writer.hpp"

#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace xpbd::export_ {

void VelocityCacheExporter::exportCache(const std::string& animation_id,
                                        const std::vector<baker::BakedFrame>& frames, double dt,
                                        const std::filesystem::path& file_path,
                                        const std::string& solver_mode) {
    if (!std::isfinite(dt) || dt <= 0.0) {
        throw std::invalid_argument("dt must be a finite value greater than 0");
    }
    if (animation_id.empty()) {
        throw std::invalid_argument("animation ID must not be blank");
    }
    if (solver_mode.empty()) {
        throw std::invalid_argument("solver mode must not be blank");
    }

    nlohmann::json root = nlohmann::json::object();
    root["format_version"] = "1.0.0";
    root["cache_type"] = "xpbd_bone_velocity";
    root["animation"] = animation_id;
    root["solver_mode"] = solver_mode;
    root["frame_rate"] = roundTo4(1.0 / dt);
    root["space"] = "model";
    root["units"] = "model_units_per_second";

    nlohmann::json frameArray = nlohmann::json::array();
    for (const auto& frame : frames) {
        nlohmann::json frameObject = nlohmann::json::object();
        frameObject["time"] = roundTo4(frame.time);
        nlohmann::json bones = nlohmann::json::object();
        for (const auto& state : frame.bone_states) {
            nlohmann::json bone = nlohmann::json::object();
            nlohmann::json vel = nlohmann::json::array();
            for (int i = 0; i < 3; ++i) {
                const double component = state.linear_velocity[static_cast<std::size_t>(i)];
                if (!std::isfinite(component)) {
                    throw std::invalid_argument("velocity components must be finite");
                }
                vel.push_back(roundTo4(component));
            }
            bone["linear_velocity"] = vel;
            bones[state.bone_name] = bone;
        }
        frameObject["bones"] = bones;
        frameArray.push_back(frameObject);
    }
    root["frames"] = frameArray;

    AtomicFileWriter::writeUtf8(file_path, root.dump(2));
}

double VelocityCacheExporter::roundTo4(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("velocity cache values must be finite");
    }
    return std::round(value * 10000.0) / 10000.0;
}

}
