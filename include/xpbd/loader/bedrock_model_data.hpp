#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xpbd::loader {

inline constexpr std::uint32_t kBedrockTextureDimensionMaximum = 16'384u;

struct FaceUV {
    double u = 0.0;
    double v = 0.0;
    double size_u = 0.0;
    double size_v = 0.0;
    bool present = false;
    int rotation_degrees = 0;
    bool size_explicit = true;
};

enum class CubeUVMode {
    None,
    Box,
    PerFace,
};

struct Cube {
    double origin[3] = {0.0, 0.0, 0.0};
    double size[3] = {1.0, 1.0, 1.0};
    double pivot[3] = {0.0, 0.0, 0.0};
    bool has_pivot = false;
    double rotation[3] = {0.0, 0.0, 0.0};
    bool has_rotation = false;
    double inflate = 0.0;


    CubeUVMode uv_mode = CubeUVMode::None;
    double uv_box[2] = {0.0, 0.0};
    bool mirror = false;
    FaceUV uv_north{};
    FaceUV uv_east{};
    FaceUV uv_south{};
    FaceUV uv_west{};
    FaceUV uv_up{};
    FaceUV uv_down{};
};

struct Bone {
    std::string name;
    std::string parent;
    bool has_parent = false;
    double pivot[3] = {0.0, 0.0, 0.0};
    double rotation[3] = {0.0, 0.0, 0.0};
    std::vector<Cube> cubes;
};

struct GeometryDescription {
    std::string identifier = "unknown";

    int texture_width = 16;
    int texture_height = 16;
    bool has_texture_width = false;
    bool has_texture_height = false;

    [[nodiscard]] bool hasCompleteTextureSize() const noexcept {
        return has_texture_width && has_texture_height;
    }
};

struct Geometry {
    GeometryDescription description;
    std::vector<Bone> bones;
};

struct GeometryRoot {
    std::vector<Geometry> minecraft_geometry;
};

}
