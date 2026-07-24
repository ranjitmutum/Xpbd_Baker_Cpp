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
    } else if (root.contains("elements") && root.contains("meta")) {

        geo = parseBlockbenchModel(root);
    } else {
        throw std::runtime_error(
            "Not a valid Bedrock/Blockbench model (need minecraft:geometry or .bbmodel)");
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

        if (desc.contains("texture_width") && desc.at("texture_width").is_number()) {
            g.description.texture_width =
                std::max(1, static_cast<int>(desc.at("texture_width").get<double>()));
            g.description.has_texture_size = true;
        }
        if (desc.contains("texture_height") && desc.at("texture_height").is_number()) {
            g.description.texture_height =
                std::max(1, static_cast<int>(desc.at("texture_height").get<double>()));
            g.description.has_texture_size = true;
        }
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
        if (std::abs(c.size[i]) + c.inflate * 2.0 < 0.0) {
            throw std::invalid_argument("cube inflate shrinks an effective size below zero");
        }
    }
    return c;
}

FaceUV ModelLoader::parseFaceUV(const nlohmann::json& face_json) {
    FaceUV f;
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
    f.present = true;
    return f;
}

namespace {


void bbToBedrockPosition(double v[3]) {
    v[0] = -v[0];
}

void bbToBedrockRotation(double v[3]) {
    v[0] = -v[0];
    v[1] = -v[1];
}

void flipFaceU(FaceUV& f) {
    if (!f.present) {
        return;
    }

    f.u = f.u + f.size_u;
    f.size_u = -f.size_u;
}





void bbElementToBedrockCube(double from[3], double to[3], Cube& c) {
    const double min_x = std::min(from[0], to[0]);
    const double max_x = std::max(from[0], to[0]);
    const double min_y = std::min(from[1], to[1]);
    const double max_y = std::max(from[1], to[1]);
    const double min_z = std::min(from[2], to[2]);
    const double max_z = std::max(from[2], to[2]);

    c.origin[0] = -max_x;
    c.origin[1] = min_y;
    c.origin[2] = min_z;
    c.size[0] = max_x - min_x;
    c.size[1] = max_y - min_y;
    c.size[2] = max_z - min_z;
}


void swapWestEastUv(Cube& c) {
    if (c.uv_mode != CubeUVMode::PerFace) {

        c.mirror = !c.mirror;
        return;
    }
    std::swap(c.uv_west, c.uv_east);
    flipFaceU(c.uv_north);
    flipFaceU(c.uv_south);
    flipFaceU(c.uv_up);
    flipFaceU(c.uv_down);
}

}

Geometry ModelLoader::parseBlockbenchModel(const nlohmann::json& root) {
    Geometry g;
    g.description.identifier = "geometry.blockbench";
    if (root.contains("name") && root.at("name").is_string()) {
        g.description.identifier = "geometry." + root.at("name").get<std::string>();
    }
    if (root.contains("model_identifier") && root.at("model_identifier").is_string() &&
        !root.at("model_identifier").get<std::string>().empty()) {
        g.description.identifier = root.at("model_identifier").get<std::string>();
    }

    if (root.contains("resolution") && root.at("resolution").is_object()) {
        const auto& res = root.at("resolution");
        if (res.contains("width") && res.at("width").is_number()) {
            g.description.texture_width =
                std::max(1, static_cast<int>(res.at("width").get<double>()));
            g.description.has_texture_size = true;
        }
        if (res.contains("height") && res.at("height").is_number()) {
            g.description.texture_height =
                std::max(1, static_cast<int>(res.at("height").get<double>()));
            g.description.has_texture_size = true;
        }
    }


    struct GroupInfo {
        std::string name;
        std::string uuid;
        double origin[3] = {0, 0, 0};
        double rotation[3] = {0, 0, 0};
        std::string parent_uuid;
        bool has_parent = false;
    };
    std::unordered_map<std::string, GroupInfo> groups;
    if (root.contains("groups") && root.at("groups").is_array()) {
        for (const auto& gj : root.at("groups")) {
            if (!gj.is_object() || !gj.contains("uuid")) {
                continue;
            }
            GroupInfo gi;
            gi.uuid = gj.at("uuid").get<std::string>();
            gi.name = gj.contains("name") && gj.at("name").is_string()
                          ? gj.at("name").get<std::string>()
                          : gi.uuid;
            if (gj.contains("origin") && gj.at("origin").is_array()) {
                parseVector3(gj.at("origin"), gi.origin, "group origin");
            }
            if (gj.contains("rotation") && gj.at("rotation").is_array()) {
                parseVector3(gj.at("rotation"), gi.rotation, "group rotation");
            }

            bbToBedrockPosition(gi.origin);
            bbToBedrockRotation(gi.rotation);
            groups.emplace(gi.uuid, std::move(gi));
        }
    }

    std::unordered_map<std::string, Cube> elements;
    if (root.contains("elements") && root.at("elements").is_array()) {
        for (const auto& ej : root.at("elements")) {
            if (!ej.is_object() || !ej.contains("uuid") || !ej.contains("from") ||
                !ej.contains("to")) {
                continue;
            }
            const std::string uuid = ej.at("uuid").get<std::string>();
            Cube c;
            double from[3] = {0, 0, 0};
            double to[3] = {0, 0, 0};
            parseVector3(ej.at("from"), from, "element from");
            parseVector3(ej.at("to"), to, "element to");
            bbElementToBedrockCube(from, to, c);
            if (ej.contains("origin") && ej.at("origin").is_array()) {
                parseVector3(ej.at("origin"), c.pivot, "element origin");
                bbToBedrockPosition(c.pivot);


                if (std::abs(c.pivot[0]) > 1e-9 || std::abs(c.pivot[1]) > 1e-9 ||
                    std::abs(c.pivot[2]) > 1e-9) {
                    c.has_pivot = true;
                }
            }
            if (ej.contains("rotation") && ej.at("rotation").is_array()) {
                parseVector3(ej.at("rotation"), c.rotation, "element rotation");
                bbToBedrockRotation(c.rotation);
                c.has_rotation =
                    std::abs(c.rotation[0]) > 1e-9 || std::abs(c.rotation[1]) > 1e-9 ||
                    std::abs(c.rotation[2]) > 1e-9;
                if (c.has_rotation && ej.contains("origin") && ej.at("origin").is_array()) {

                    parseVector3(ej.at("origin"), c.pivot, "element origin");
                    bbToBedrockPosition(c.pivot);
                    c.has_pivot = true;
                }
            }
            if (ej.contains("inflate") && ej.at("inflate").is_number()) {
                c.inflate = ej.at("inflate").get<double>();
            }

            if (ej.contains("faces") && ej.at("faces").is_object()) {
                c.uv_mode = CubeUVMode::PerFace;
                const auto& faces = ej.at("faces");
                auto loadFace = [&](const char* key, FaceUV& out) {
                    if (!faces.contains(key) || !faces.at(key).is_object()) {
                        return;
                    }
                    const auto& face = faces.at(key);

                    if (face.contains("texture") && face.at("texture").is_null()) {
                        return;
                    }
                    if (face.contains("uv") && face.at("uv").is_array()) {
                        out = parseFaceUVArray(face.at("uv"));
                    }
                };
                loadFace("north", c.uv_north);
                loadFace("east", c.uv_east);
                loadFace("south", c.uv_south);
                loadFace("west", c.uv_west);
                loadFace("up", c.uv_up);
                loadFace("down", c.uv_down);
            } else if (ej.contains("uv") && ej.at("uv").is_array() && ej.at("uv").size() >= 2) {

                c.uv_mode = CubeUVMode::Box;
                c.uv_box[0] = ej.at("uv").at(0).get<double>();
                c.uv_box[1] = ej.at("uv").at(1).get<double>();
            }
            if (ej.contains("mirror_uv") && ej.at("mirror_uv").is_boolean()) {
                c.mirror = ej.at("mirror_uv").get<bool>();
            }

            swapWestEastUv(c);
            elements.emplace(uuid, std::move(c));
        }
    }



    std::unordered_map<std::string, std::vector<std::string>> group_cubes;
    std::function<void(const nlohmann::json&, const std::string&)> walk;
    walk = [&](const nlohmann::json& node, const std::string& parent_group) {
        if (node.is_string()) {
            if (!parent_group.empty()) {
                group_cubes[parent_group].push_back(node.get<std::string>());
            } else {

                group_cubes["__bb_root__"].push_back(node.get<std::string>());
            }
            return;
        }
        if (!node.is_object() || !node.contains("uuid")) {
            return;
        }
        const std::string uuid = node.at("uuid").get<std::string>();

        if (!groups.count(uuid) && !elements.count(uuid)) {
            GroupInfo gi;
            gi.uuid = uuid;
            gi.name = node.contains("name") && node.at("name").is_string()
                          ? node.at("name").get<std::string>()
                          : uuid;
            if (node.contains("origin") && node.at("origin").is_array()) {
                parseVector3(node.at("origin"), gi.origin, "outliner group origin");
                bbToBedrockPosition(gi.origin);
            }
            if (node.contains("rotation") && node.at("rotation").is_array()) {
                parseVector3(node.at("rotation"), gi.rotation, "outliner group rotation");
                bbToBedrockRotation(gi.rotation);
            }
            groups.emplace(uuid, std::move(gi));
        }
        if (groups.count(uuid)) {
            if (!parent_group.empty()) {
                groups[uuid].parent_uuid = parent_group;
                groups[uuid].has_parent = true;
            }
            if (node.contains("children") && node.at("children").is_array()) {
                for (const auto& child : node.at("children")) {
                    walk(child, uuid);
                }
            }
        } else if (elements.count(uuid) && !parent_group.empty()) {
            group_cubes[parent_group].push_back(uuid);
        }
    };
    if (root.contains("outliner") && root.at("outliner").is_array()) {
        for (const auto& node : root.at("outliner")) {
            walk(node, "");
        }
    }
    if (groups.empty()) {

        GroupInfo root_g;
        root_g.uuid = "root";
        root_g.name = "root";
        groups.emplace("root", root_g);
        for (const auto& [eu, _] : elements) {
            group_cubes["root"].push_back(eu);
        }
    } else if (group_cubes.count("__bb_root__")) {

        GroupInfo root_g;
        root_g.uuid = "__bb_root__";
        root_g.name = "root";
        groups.emplace("__bb_root__", root_g);
    }


    std::unordered_map<std::string, std::string> uuid_to_name;
    uuid_to_name.reserve(groups.size());
    std::unordered_map<std::string, int> name_count;
    for (const auto& [uuid, gi] : groups) {
        std::string name = gi.name.empty() ? uuid : gi.name;
        int& n = name_count[name];
        if (n > 0) {
            name = name + "_" + std::to_string(n);
        }
        ++n;
        uuid_to_name[uuid] = name;
    }

    g.bones.reserve(groups.size());
    for (const auto& [uuid, gi] : groups) {
        Bone b;
        b.name = uuid_to_name.at(uuid);
        b.pivot[0] = gi.origin[0];
        b.pivot[1] = gi.origin[1];
        b.pivot[2] = gi.origin[2];
        b.rotation[0] = gi.rotation[0];
        b.rotation[1] = gi.rotation[1];
        b.rotation[2] = gi.rotation[2];
        if (gi.has_parent && uuid_to_name.count(gi.parent_uuid)) {
            b.parent = uuid_to_name.at(gi.parent_uuid);
            b.has_parent = true;
        }
        auto cit = group_cubes.find(uuid);
        if (cit != group_cubes.end()) {
            for (const auto& eu : cit->second) {
                auto eit = elements.find(eu);
                if (eit != elements.end()) {
                    b.cubes.push_back(eit->second);
                }
            }
        }
        g.bones.push_back(std::move(b));
    }

    return g;
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
