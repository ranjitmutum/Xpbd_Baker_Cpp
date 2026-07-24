#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xpbd::baker {


struct BoneState {
    std::string bone_name;
    std::array<double, 3> position{0.0, 0.0, 0.0};
    std::array<double, 3> rotation{0.0, 0.0, 0.0};
    std::array<double, 3> linear_velocity{0.0, 0.0, 0.0};
    std::array<double, 3> world_position{0.0, 0.0, 0.0};
    bool has_world_position = false;
};

struct BakedFrame {
    double time = 0.0;
    std::vector<BoneState> bone_states;
    std::map<std::string, std::size_t> bone_index;

    void rebuildIndex() {
        bone_index.clear();
        for (std::size_t i = 0; i < bone_states.size(); ++i) {
            if (!bone_states[i].bone_name.empty()) {
                bone_index[bone_states[i].bone_name] = i;
            }
        }
    }

    [[nodiscard]] const BoneState* getBoneState(const std::string& name) const {
        auto it = bone_index.find(name);
        if (it == bone_index.end()) {
            return nullptr;
        }
        return &bone_states[it->second];
    }

    [[nodiscard]] BoneState* getBoneState(const std::string& name) {
        auto it = bone_index.find(name);
        if (it == bone_index.end()) {
            return nullptr;
        }
        return &bone_states[it->second];
    }
};

struct StableFrameLayout {
    using NameToIndexMap = std::map<std::string, std::size_t>;

    std::vector<std::string> bone_names;
    NameToIndexMap index_by_name;

    [[nodiscard]] bool matches(const BakedFrame& frame) const {
        if (frame.bone_states.size() != bone_names.size()) {
            return false;
        }
        for (std::size_t index = 0; index < bone_names.size(); ++index) {
            if (frame.bone_states[index].bone_name != bone_names[index]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static std::optional<StableFrameLayout>
    tryCreate(const std::vector<BakedFrame>& frames) {
        StableFrameLayout layout;
        if (frames.empty()) {
            return layout;
        }
        layout.bone_names.reserve(frames.front().bone_states.size());
        for (std::size_t index = 0;
             index < frames.front().bone_states.size(); ++index) {
            const std::string& name =
                frames.front().bone_states[index].bone_name;
            if (name.empty() ||
                !layout.index_by_name.emplace(name, index).second) {
                return std::nullopt;
            }
            layout.bone_names.push_back(name);
        }
        for (const BakedFrame& frame : frames) {
            if (!layout.matches(frame)) {
                return std::nullopt;
            }
        }
        return layout;
    }
};

}
