#pragma once

#include "xpbd/loader/bedrock_model_data.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace xpbd::loader {

// 读取 Bedrock 几何模型，并转换为内部骨骼与立方体描述。
class ModelLoader {
public:






    [[nodiscard]] static Geometry load(const std::filesystem::path& file_path);
    [[nodiscard]] static Geometry loadFromString(const std::string& json_text);

    [[nodiscard]] static const Bone* findBoneByName(const std::vector<Bone>& bones,
                                                    const std::string& name);

private:
    static void validateGeometry(const Geometry& geo);
    static Geometry parseGeometryRoot(const nlohmann::json& root);
    static Bone parseBone(const nlohmann::json& json);
    static Cube parseCube(const nlohmann::json& json);
    static FaceUV parseFaceUV(const nlohmann::json& face_json);
    static FaceUV parseFaceUVArray(const nlohmann::json& uv_array);
    static void parseVector3(const nlohmann::json& array, double out[3], const char* label);
};

}
