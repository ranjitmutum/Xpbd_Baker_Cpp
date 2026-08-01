#include "xpbd/gfx/texture_image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace xpbd::gfx {
namespace {

static_assert(std::is_nothrow_move_assignable_v<TextureImage>,
              "transactional texture commit must not throw");

struct EffectiveTextureDecodeLimits {
    std::uint32_t maximum_width = 0;
    std::uint32_t maximum_height = 0;
    std::size_t maximum_pixels = 0;
    std::size_t maximum_decoded_bytes = 0;
    std::size_t maximum_peak_bytes = 0;
    std::size_t retained_resident_bytes = 0;
};

struct TextureDecodeContext {
    std::size_t encoded_bytes = 0;
    int width = 0;
    int height = 0;
    int source_channels = 0;
    std::size_t pixels = 0;
    std::size_t decoded_bytes = 0;
    std::size_t previous_output_bytes = 0;
    std::size_t required_peak_bytes = 0;
    EffectiveTextureDecodeLimits limits{};
};

struct TextureDecodeSource {
    const stbi_uc* memory = nullptr;
    int memory_size = 0;
    const char* filename = nullptr;

    [[nodiscard]] int info(int* width, int* height,
                           int* channels) const noexcept {
        return filename != nullptr
                   ? stbi_info(filename, width, height, channels)
                   : stbi_info_from_memory(memory, memory_size, width, height,
                                           channels);
    }

    [[nodiscard]] stbi_uc* load(int* width, int* height,
                                int* channels) const noexcept {
        return filename != nullptr
                   ? stbi_load(filename, width, height, channels, 4)
                   : stbi_load_from_memory(memory, memory_size, width, height,
                                           channels, 4);
    }
};

struct StbiImageDeleter {
    void operator()(stbi_uc* pixels) const noexcept {
        stbi_image_free(pixels);
    }
};

[[nodiscard]] bool checkedMultiply(std::size_t lhs, std::size_t rhs,
                                   std::size_t& product) noexcept {
    product = 0;
    if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
        return false;
    }
    product = lhs * rhs;
    return true;
}

[[nodiscard]] bool checkedAdd(std::size_t lhs, std::size_t rhs,
                              std::size_t& sum) noexcept {
    sum = 0;
    if (rhs > (std::numeric_limits<std::size_t>::max)() - lhs) {
        return false;
    }
    sum = lhs + rhs;
    return true;
}

[[nodiscard]] EffectiveTextureDecodeLimits effectiveLimits(
    TextureDecodeLimits requested) noexcept {
    return {
        (std::min)(requested.maximum_width, kTextureDecodeMaximumWidth),
        (std::min)(requested.maximum_height, kTextureDecodeMaximumHeight),
        (std::min)(requested.maximum_pixels, kTextureDecodeMaximumPixels),
        (std::min)(requested.maximum_decoded_bytes,
                   kTextureDecodeMaximumRgbaBytes),
        (std::min)(requested.maximum_peak_bytes,
                   kTextureDecodeMaximumPeakBytes),
        requested.retained_resident_bytes,
    };
}

void setErrorNoThrow(std::string* error, const char* message) noexcept {
    if (error == nullptr) {
        return;
    }
    try {
        error->assign(message != nullptr ? message : "texture decode failed");
    } catch (...) {
        // The decode boundary must not leak an allocation exception while
        // attempting to report the original allocation failure.
    }
}

void setContextError(std::string* error, const char* summary,
                     const TextureDecodeContext& context,
                     const char* detail = nullptr) noexcept {
    char message[1536]{};
    std::snprintf(
        message, sizeof(message),
        "%s: encoded=%zu bytes, dimensions=%dx%d, channels=%d, pixels=%zu, "
        "decoded_rgba=%zu bytes, required_peak=%zu bytes "
        "(retained=%zu, previous_output=%zu, decoder=%zu, candidate=%zu); "
        "limits: width=%u, height=%u, maximum_pixels=%zu, "
        "maximum_decoded_bytes=%zu, maximum_peak_bytes=%zu%s%s",
        summary != nullptr ? summary : "texture decode failed",
        context.encoded_bytes, context.width, context.height,
        context.source_channels, context.pixels, context.decoded_bytes,
        context.required_peak_bytes,
        context.limits.retained_resident_bytes,
        context.previous_output_bytes, context.decoded_bytes,
        context.decoded_bytes, context.limits.maximum_width,
        context.limits.maximum_height, context.limits.maximum_pixels,
        context.limits.maximum_decoded_bytes,
        context.limits.maximum_peak_bytes,
        detail != nullptr && detail[0] != '\0' ? "; detail=" : "",
        detail != nullptr ? detail : "");
    setErrorNoThrow(error, message);
}

[[nodiscard]] bool computePeakBytes(TextureDecodeContext& context) noexcept {
    std::size_t peak = context.limits.retained_resident_bytes;
    if (!checkedAdd(peak, context.previous_output_bytes, peak) ||
        !checkedAdd(peak, context.decoded_bytes, peak) ||
        !checkedAdd(peak, context.decoded_bytes, peak)) {
        context.required_peak_bytes =
            (std::numeric_limits<std::size_t>::max)();
        return false;
    }
    context.required_peak_bytes = peak;
    return true;
}

[[nodiscard]] bool decodeTexture(const TextureDecodeSource& source,
                                 std::size_t encoded_bytes,
                                 const std::string* decoded_path,
                                 TextureImage& out, std::string* error,
                                 TextureDecodeLimits requested_limits) {
    TextureDecodeContext context{};
    context.encoded_bytes = encoded_bytes;
    context.previous_output_bytes = out.rgba.capacity();
    context.limits = effectiveLimits(requested_limits);

    try {
        if (source.info(&context.width, &context.height,
                        &context.source_channels) == 0 ||
            context.width <= 0 || context.height <= 0) {
            setContextError(error, "texture stbi_info preflight failed", context,
                            stbi_failure_reason());
            return false;
        }

        if (!checkedMultiply(static_cast<std::size_t>(context.width),
                             static_cast<std::size_t>(context.height),
                             context.pixels) ||
            !checkedTextureRgbaByteCount(
                static_cast<std::size_t>(context.width),
                static_cast<std::size_t>(context.height),
                context.decoded_bytes)) {
            setContextError(error, "texture size arithmetic overflow", context);
            return false;
        }

        const bool peak_valid = computePeakBytes(context);
        if (static_cast<std::uint32_t>(context.width) >
                context.limits.maximum_width ||
            static_cast<std::uint32_t>(context.height) >
                context.limits.maximum_height ||
            context.pixels > context.limits.maximum_pixels ||
            context.decoded_bytes > context.limits.maximum_decoded_bytes ||
            !peak_valid ||
            context.required_peak_bytes > context.limits.maximum_peak_bytes) {
            setContextError(error, "texture decode budget exceeded", context,
                            !peak_valid ? "peak byte arithmetic overflow" : nullptr);
            return false;
        }

        int decoded_width = 0;
        int decoded_height = 0;
        int decoded_channels = 0;
        std::unique_ptr<stbi_uc, StbiImageDeleter> pixels(
            source.load(&decoded_width, &decoded_height, &decoded_channels));
        if (!pixels || decoded_width != context.width ||
            decoded_height != context.height || decoded_channels <= 0) {
            setContextError(error, "texture decode failed after preflight", context,
                            stbi_failure_reason());
            return false;
        }

        TextureImage candidate;
        candidate.width = decoded_width;
        candidate.height = decoded_height;
        candidate.source_channels = decoded_channels;
        if (decoded_path != nullptr) {
            candidate.path = *decoded_path;
        }
        candidate.rgba.assign(pixels.get(),
                              pixels.get() + context.decoded_bytes);
        if (!candidate.valid()) {
            setContextError(error, "texture decode produced an invalid image",
                            context);
            return false;
        }

        out = std::move(candidate);
        if (error != nullptr) {
            error->clear();
        }
        return true;
    } catch (const std::bad_alloc&) {
        setContextError(error, "texture decode allocation failed", context,
                        "std::bad_alloc");
        return false;
    } catch (const std::length_error& exception) {
        setContextError(error, "texture decode allocation failed", context,
                        exception.what());
        return false;
    }
}

} // namespace

bool checkedTextureRgbaByteCount(std::size_t width, std::size_t height,
                                 std::size_t& byte_count) noexcept {
    byte_count = 0;
    std::size_t pixels = 0;
    return checkedMultiply(width, height, pixels) &&
           checkedMultiply(pixels, std::size_t{4}, byte_count);
}

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

bool loadTextureImageFromMemory(const void* data, int size, TextureImage& out,
                                std::string* err,
                                TextureDecodeLimits limits) {
    if (!data || size <= 0) {
        setErrorNoThrow(err, "empty image buffer");
        return false;
    }
    const TextureDecodeSource source{
        static_cast<const stbi_uc*>(data), size, nullptr};
    return decodeTexture(source, static_cast<std::size_t>(size), nullptr, out,
                         err, limits);
}

bool loadTextureImage(const std::filesystem::path& path, TextureImage& out,
                      std::string* err, TextureDecodeLimits limits) {
    try {
        const std::string decoded_path = path.string();
        std::error_code file_size_error;
        const std::uintmax_t raw_file_size =
            std::filesystem::file_size(path, file_size_error);
        const std::size_t encoded_bytes =
            file_size_error
                ? 0u
                : raw_file_size >
                          static_cast<std::uintmax_t>(
                              (std::numeric_limits<std::size_t>::max)())
                      ? (std::numeric_limits<std::size_t>::max)()
                      : static_cast<std::size_t>(raw_file_size);
        const TextureDecodeSource source{nullptr, 0, decoded_path.c_str()};
        const bool loaded = decodeTexture(source, encoded_bytes, &decoded_path,
                                          out, err, limits);
        if (!loaded) {
            std::fprintf(stderr, "texture load failed: %s (%s)\n",
                         decoded_path.c_str(),
                         err != nullptr && !err->empty() ? err->c_str() : "?");
        }
        return loaded;
    } catch (const std::bad_alloc&) {
        setErrorNoThrow(err, "texture decode allocation failed: std::bad_alloc");
        return false;
    } catch (const std::filesystem::filesystem_error& exception) {
        setErrorNoThrow(err, exception.what());
        return false;
    }
}

}
