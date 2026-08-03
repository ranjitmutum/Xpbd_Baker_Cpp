#include "xpbd/loader/model_loader.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace xpbd::loader {

Geometry ModelLoader::load(const std::filesystem::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open model file: " + file_path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        return loadFromString(ss.str());
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Invalid model JSON: ") + e.what());
    }
}

Geometry ModelLoader::loadFromString(const std::string& json_text) {
    nlohmann::json root = nlohmann::json::parse(json_text);
    Geometry geo;
    if (root.contains("minecraft:geometry")) {
        geo = parseGeometryRoot(root);
    } else {
        throw std::runtime_error(
            "Not a valid Bedrock geometry model (need minecraft:geometry)");
    }
    validateGeometry(geo);
    return geo;
}

Geometry ModelLoader::parseGeometryRoot(const nlohmann::json& root) {
    const auto& arr = root.at("minecraft:geometry");
    if (!arr.is_array() || arr.empty()) {
        throw std::runtime_error("No geometry found in model file");
    }
    const auto& json = arr.at(0);
    Geometry g;
    if (json.contains("description") && json.at("description").is_object()) {
        const auto& desc = json.at("description");
        if (desc.contains("identifier") && desc.at("identifier").is_string()) {
            g.description.identifier = desc.at("identifier").get<std::string>();
        }

        const auto parseTextureDimension =
            [&](const char* key, int& value, bool& present) {
                if (!desc.contains(key)) {
                    return;
                }
                const auto& declaration = desc.at(key);
                if (!declaration.is_number()) {
                    throw std::invalid_argument(
                        std::string("description.") + key +
                        " must be a finite positive integer");
                }
                const double parsed = declaration.get<double>();
                const double rounded = std::round(parsed);
                if (!std::isfinite(parsed) || parsed <= 0.0 ||
                    parsed != rounded ||
                    rounded > static_cast<double>(
                                  kBedrockTextureDimensionMaximum)) {
                    throw std::invalid_argument(
                        std::string("description.") + key +
                        " must be a finite positive integer no greater than " +
                        std::to_string(kBedrockTextureDimensionMaximum));
                }
                value = static_cast<int>(rounded);
                present = true;
            };
        parseTextureDimension("texture_width", g.description.texture_width,
                              g.description.has_texture_width);
        parseTextureDimension("texture_height", g.description.texture_height,
                              g.description.has_texture_height);
    }
    if (json.contains("bones") && json.at("bones").is_array()) {
        for (const auto& boneJson : json.at("bones")) {
            g.bones.push_back(parseBone(boneJson));
        }
    }
    return g;
}

Bone ModelLoader::parseBone(const nlohmann::json& json) {
    Bone b;
    if (!json.contains("name") || !json.at("name").is_string()) {
        throw std::invalid_argument("bone name is required");
    }
    b.name = json.at("name").get<std::string>();
    if (json.contains("parent") && !json.at("parent").is_null()) {
        b.parent = json.at("parent").get<std::string>();
        b.has_parent = true;
    }
    if (json.contains("pivot")) {
        parseVector3(json.at("pivot"), b.pivot, "bone pivot");
    }
    if (json.contains("rotation")) {
        parseVector3(json.at("rotation"), b.rotation, "bone rotation");
    }
    if (json.contains("cubes") && json.at("cubes").is_array()) {
        for (const auto& cubeJson : json.at("cubes")) {
            b.cubes.push_back(parseCube(cubeJson));
        }
    }
    return b;
}

Cube ModelLoader::parseCube(const nlohmann::json& json) {
    Cube c;
    if (!json.contains("origin")) {
        throw std::invalid_argument("cube origin is required");
    }
    parseVector3(json.at("origin"), c.origin, "cube origin");
    if (!json.contains("size")) {
        throw std::invalid_argument("cube size is required");
    }
    parseVector3(json.at("size"), c.size, "cube size");
    if (json.contains("pivot")) {
        parseVector3(json.at("pivot"), c.pivot, "cube pivot");
        c.has_pivot = true;
    }
    if (json.contains("rotation")) {
        parseVector3(json.at("rotation"), c.rotation, "cube rotation");
        c.has_rotation = true;
    }
    if (json.contains("inflate")) {
        c.inflate = json.at("inflate").get<double>();
        if (!std::isfinite(c.inflate)) {
            throw std::invalid_argument("cube inflate must be finite");
        }
    }
    if (json.contains("mirror") && json.at("mirror").is_boolean()) {
        c.mirror = json.at("mirror").get<bool>();
    }

    if (json.contains("uv")) {
        const auto& uvj = json.at("uv");
        if (uvj.is_array() && uvj.size() >= 2) {
            c.uv_mode = CubeUVMode::Box;
            c.uv_box[0] = uvj.at(0).get<double>();
            c.uv_box[1] = uvj.at(1).get<double>();
            if (!std::isfinite(c.uv_box[0]) || !std::isfinite(c.uv_box[1])) {
                throw std::invalid_argument("cube uv must be finite");
            }
        } else if (uvj.is_object()) {
            c.uv_mode = CubeUVMode::PerFace;
            auto loadFace = [&](const char* key, FaceUV& out) {
                if (!uvj.contains(key) || uvj.at(key).is_null()) {
                    return;
                }
                const auto& face = uvj.at(key);
                if (face.is_object()) {
                    out = parseFaceUV(face);
                } else if (face.is_array()) {
                    out = parseFaceUVArray(face);
                }
            };
            loadFace("north", c.uv_north);
            loadFace("east", c.uv_east);
            loadFace("south", c.uv_south);
            loadFace("west", c.uv_west);
            loadFace("up", c.uv_up);
            loadFace("down", c.uv_down);
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (c.size[i] != 0.0 &&
            std::abs(c.size[i]) + c.inflate * 2.0 < 0.0) {
            throw std::invalid_argument("cube inflate shrinks an effective size below zero");
        }
    }
    return c;
}

FaceUV ModelLoader::parseFaceUV(const nlohmann::json& face_json) {
    FaceUV f;
    f.size_explicit = false;
    if (face_json.contains("uv") && face_json.at("uv").is_array()) {
        const auto& uva = face_json.at("uv");
        if (uva.size() >= 4) {

            return parseFaceUVArray(uva);
        }
        if (uva.size() >= 2) {
            f.u = uva.at(0).get<double>();
            f.v = uva.at(1).get<double>();
        }
    }
    if (face_json.contains("uv_size") && face_json.at("uv_size").is_array() &&
        face_json.at("uv_size").size() >= 2) {
        f.size_u = face_json.at("uv_size").at(0).get<double>();
        f.size_v = face_json.at("uv_size").at(1).get<double>();
        f.size_explicit = true;
    }
    if (face_json.contains("uv_rotation") &&
        face_json.at("uv_rotation").is_number()) {
        const double rotation =
            face_json.at("uv_rotation").get<double>();
        const double rounded = std::round(rotation);
        if (!std::isfinite(rotation) ||
            std::abs(rotation - rounded) > 1.0e-9 ||
            std::abs(rounded) > 1000000000.0 ||
            std::fmod(std::abs(rounded), 90.0) > 1.0e-9) {
            throw std::invalid_argument(
                "face uv_rotation must be a finite multiple of 90 degrees");
        }
        int degrees = static_cast<int>(rounded) % 360;
        if (degrees < 0) {
            degrees += 360;
        }
        f.rotation_degrees = degrees;
    }
    if (!std::isfinite(f.u) || !std::isfinite(f.v) || !std::isfinite(f.size_u) ||
        !std::isfinite(f.size_v)) {
        throw std::invalid_argument("cube face uv must be finite");
    }
    f.present = true;
    return f;
}

FaceUV ModelLoader::parseFaceUVArray(const nlohmann::json& uv_array) {
    FaceUV f;
    if (!uv_array.is_array() || uv_array.size() < 4) {
        throw std::invalid_argument("face uv array must have 4 numbers [u0,v0,u1,v1]");
    }
    const double u0 = uv_array.at(0).get<double>();
    const double v0 = uv_array.at(1).get<double>();
    const double u1 = uv_array.at(2).get<double>();
    const double v1 = uv_array.at(3).get<double>();
    if (!std::isfinite(u0) || !std::isfinite(v0) || !std::isfinite(u1) || !std::isfinite(v1)) {
        throw std::invalid_argument("face uv array components must be finite");
    }
    f.u = u0;
    f.v = v0;
    f.size_u = u1 - u0;
    f.size_v = v1 - v0;
    f.size_explicit = true;
    f.present = true;
    return f;
}


void ModelLoader::parseVector3(const nlohmann::json& array, double out[3], const char* label) {
    if (!array.is_array() || array.size() < 3) {
        throw std::invalid_argument(std::string(label) + " must contain three numbers");
    }
    for (int i = 0; i < 3; ++i) {
        out[i] = array.at(static_cast<std::size_t>(i)).get<double>();
        if (!std::isfinite(out[i])) {
            throw std::invalid_argument(std::string(label) + " components must be finite");
        }
    }
}

void ModelLoader::validateGeometry(const Geometry& geo) {
    std::unordered_map<std::string, const Bone*> byName;
    for (const auto& bone : geo.bones) {
        if (bone.name.empty()) {
            throw std::runtime_error("Geometry contains a bone without a name");
        }
        if (!byName.emplace(bone.name, &bone).second) {
            throw std::runtime_error("Duplicate bone name: " + bone.name);
        }
    }
    for (const auto& bone : geo.bones) {
        if (bone.has_parent && byName.find(bone.parent) == byName.end()) {
            throw std::runtime_error("Missing parent '" + bone.parent +
                                     "' for bone: " + bone.name);
        }
        std::unordered_set<std::string> visited;
        const Bone* current = &bone;
        while (current != nullptr && current->has_parent) {
            if (!visited.insert(current->name).second) {
                throw std::runtime_error("Cyclic bone hierarchy at: " + bone.name);
            }
            auto it = byName.find(current->parent);
            current = it == byName.end() ? nullptr : it->second;
        }
    }
}

const Bone* ModelLoader::findBoneByName(const std::vector<Bone>& bones,
                                        const std::string& name) {
    for (const auto& b : bones) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

}
