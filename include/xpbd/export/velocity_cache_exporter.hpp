#pragma once

#include "xpbd/baker/baked_frame.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace xpbd::export_ {

class VelocityCacheExporter {
public:
    static void exportCache(const std::string& animation_id,
                            const std::vector<baker::BakedFrame>& frames, double dt,
                            const std::filesystem::path& file_path,
                            const std::string& solver_mode = "Unknown");

private:
    static double roundTo4(double value);
};

}
