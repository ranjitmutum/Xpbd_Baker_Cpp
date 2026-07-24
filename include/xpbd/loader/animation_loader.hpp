#pragma once

#include "xpbd/loader/bedrock_animation_data.hpp"

#include <filesystem>
#include <string>

namespace xpbd::loader {

// 读取 Bedrock 动画 JSON，并保留关键帧与 Molang 通道信息。
class AnimationLoader {
public:
    [[nodiscard]] static AnimationRoot load(const std::filesystem::path& file_path);
    [[nodiscard]] static AnimationRoot loadFromString(const std::string& json_text);
};

}
