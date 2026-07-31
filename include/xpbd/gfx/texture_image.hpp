#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xpbd::gfx {


struct TextureImage {
    int width = 0;
    int height = 0;
    int source_channels = 0;
    std::vector<std::uint8_t> rgba;
    std::string path;

    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    }

    void clear() {
        width = height = source_channels = 0;
        rgba.clear();
        path.clear();
    }


    void sample(float u, float v, float& r, float& g, float& b, float& a) const;

    void sample(float u, float v, float& r, float& g, float& b) const {
        float a = 1.0f;
        sample(u, v, r, g, b, a);
    }
};


bool loadTextureImage(const std::filesystem::path& path, TextureImage& out, std::string* err = nullptr);

bool loadTextureImageFromMemory(const void* data, int size, TextureImage& out,
                                std::string* err = nullptr);

}
