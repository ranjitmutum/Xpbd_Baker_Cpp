#include "xpbd/gfx/texture_image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xpbd::gfx {

void TextureImage::sample(float u, float v, float& r, float& g, float& b, float& a) const {
    if (!valid()) {
        r = g = b = a = 1.0f;
        return;
    }

    u = u - std::floor(u);
    v = v - std::floor(v);
    if (u < 0.0f) {
        u += 1.0f;
    }
    if (v < 0.0f) {
        v += 1.0f;
    }

    int x = static_cast<int>(std::floor(u * static_cast<float>(width)));
    int y = static_cast<int>(std::floor(v * static_cast<float>(height)));
    if (x < 0) {
        x = 0;
    } else if (x >= width) {
        x = width - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= height) {
        y = height - 1;
    }
    const std::uint8_t* p =
        rgba.data() +
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) *
            4u;
    r = p[0] / 255.0f;
    g = p[1] / 255.0f;
    b = p[2] / 255.0f;
    a = p[3] / 255.0f;
}

bool loadTextureImageFromMemory(const void* data, int size, TextureImage& out, std::string* err) {
    out.clear();
    if (!data || size <= 0) {
        if (err) {
            *err = "empty image buffer";
        }
        return false;
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(static_cast<const stbi_uc*>(data), size, &w, &h, &n, 4);
    if (!pixels || w <= 0 || h <= 0) {
        const char* why = stbi_failure_reason();
        if (err) {
            *err = why ? why : "stbi_load_from_memory failed";
        }
        if (pixels) {
            stbi_image_free(pixels);
        }
        return false;
    }
    out.width = w;
    out.height = h;
    out.source_channels = n;
    out.rgba.assign(pixels,
                    pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(pixels);
    return true;
}

bool loadTextureImage(const std::filesystem::path& path, TextureImage& out, std::string* err) {
    out.clear();
    const std::string p = path.string();
    int w = 0, h = 0, n = 0;
    stbi_uc* data = stbi_load(p.c_str(), &w, &h, &n, 4);
    if (!data || w <= 0 || h <= 0) {
        const char* why = stbi_failure_reason();
        if (err) {
            *err = why ? why : "stbi_load failed";
        }
        std::fprintf(stderr, "texture load failed: %s (%s)\n", p.c_str(), why ? why : "?");
        if (data) {
            stbi_image_free(data);
        }
        return false;
    }
    out.width = w;
    out.height = h;
    out.source_channels = n;
    out.path = p;
    out.rgba.assign(data, data + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(data);
    return true;
}

}
