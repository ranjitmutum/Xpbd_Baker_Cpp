#include "xpbd/gfx/texture_image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
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

    [[nodiscard]] int info(int* width, int* height,
                           int* channels) const noexcept {
        return stbi_info_from_memory(memory, memory_size, width, height,
                                     channels);
    }

    [[nodiscard]] stbi_uc* load(int* width, int* height,
                                int* channels) const noexcept {
        return stbi_load_from_memory(memory, memory_size, width, height,
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

[[nodiscard]] std::filesystem::path
normalizedPublicPath(const std::filesystem::path& path) {
    std::filesystem::path ordinary = path;
#if defined(_WIN32)
    const auto native = ordinary.native();
    constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
    constexpr std::wstring_view kExtendedUncPrefix = L"\\\\?\\UNC\\";
    if (native.starts_with(kExtendedUncPrefix)) {
        ordinary = std::filesystem::path(
            std::wstring(L"\\\\") +
            native.substr(kExtendedUncPrefix.size()));
    } else if (native.starts_with(kExtendedPrefix)) {
        ordinary = std::filesystem::path(
            native.substr(kExtendedPrefix.size()));
    }
#endif
    std::error_code error;
    auto absolute = std::filesystem::absolute(ordinary, error);
    if (error) {
        return ordinary.lexically_normal();
    }
    return absolute.lexically_normal();
}

void setSnapshotError(std::string* error, std::string_view label,
                      std::string_view stage,
                      const std::filesystem::path& path,
                      std::string_view detail) noexcept {
    if (error == nullptr) {
        return;
    }
    try {
        *error = std::string(label) + " " + std::string(stage) +
                 " stage failed for '" + pathUtf8String(path) + "'";
        if (!detail.empty()) {
            *error += ": ";
            *error += detail;
        }
    } catch (...) {
        setErrorNoThrow(error, "file snapshot error reporting failed");
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
        !checkedAdd(peak, context.encoded_bytes, peak) ||
        !checkedAdd(peak, context.decoded_bytes, peak) ||
        !checkedAdd(peak, context.decoded_bytes, peak)) {
        context.required_peak_bytes =
            (std::numeric_limits<std::size_t>::max)();
        return false;
    }
    context.required_peak_bytes = peak;
    return true;
}

[[nodiscard]] bool inspectTextureSource(
    const TextureDecodeSource& source, std::size_t encoded_bytes,
    std::size_t previous_output_bytes, TextureImageHeader& out,
    TextureDecodeContext& context, std::string* error,
    TextureDecodeLimits requested_limits) noexcept {
    context = {};
    context.encoded_bytes = encoded_bytes;
    context.previous_output_bytes = previous_output_bytes;
    context.limits = effectiveLimits(requested_limits);

    if (source.info(&context.width, &context.height,
                    &context.source_channels) == 0 ||
        context.width <= 0 || context.height <= 0) {
        setContextError(error,
                        "texture Header stage failed (stbi_info_from_memory)",
                        context, stbi_failure_reason());
        return false;
    }

    if (!checkedMultiply(static_cast<std::size_t>(context.width),
                         static_cast<std::size_t>(context.height),
                         context.pixels) ||
        !checkedTextureRgbaByteCount(
            static_cast<std::size_t>(context.width),
            static_cast<std::size_t>(context.height),
            context.decoded_bytes)) {
        setContextError(error,
                        "texture budget stage failed: size arithmetic overflow",
                        context);
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
        setContextError(error, "texture budget stage failed", context,
                        !peak_valid ? "peak byte arithmetic overflow" : nullptr);
        return false;
    }

    const TextureImageHeader candidate{
        context.width, context.height, context.source_channels};
    out = candidate;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

[[nodiscard]] bool decodeTexture(const TextureDecodeSource& source,
                                 std::size_t encoded_bytes,
                                 const std::string* decoded_path,
                                 TextureImage& out, std::string* error,
                                 TextureDecodeLimits requested_limits) {
    TextureDecodeContext context{};
    TextureImageHeader header;
    if (!inspectTextureSource(source, encoded_bytes, out.rgba.capacity(),
                              header, context, error, requested_limits)) {
        return false;
    }

    try {
        int decoded_width = 0;
        int decoded_height = 0;
        int decoded_channels = 0;
        std::unique_ptr<stbi_uc, StbiImageDeleter> pixels(
            source.load(&decoded_width, &decoded_height, &decoded_channels));
        if (!pixels || decoded_width != context.width ||
            decoded_height != context.height || decoded_channels <= 0) {
            setContextError(error,
                            "texture Decode stage failed after Header", context,
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
            setContextError(error,
                            "texture Decode stage produced an invalid image",
                            context);
            return false;
        }

        out = std::move(candidate);
        if (error != nullptr) {
            error->clear();
        }
        return true;
    } catch (const std::bad_alloc&) {
        setContextError(error,
                        "texture budget stage failed during Decode allocation",
                        context,
                        "std::bad_alloc");
        return false;
    } catch (const std::length_error& exception) {
        setContextError(error,
                        "texture budget stage failed during Decode allocation",
                        context,
                        exception.what());
        return false;
    }
}

} // namespace

std::string pathUtf8String(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::filesystem::path
pathForFilesystemIo(const std::filesystem::path& path) {
#if defined(_WIN32)
    auto preferred = path;
    preferred.make_preferred();
    const auto native = preferred.native();
    constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
    if (native.starts_with(kExtendedPrefix) || !preferred.is_absolute()) {
        return preferred;
    }
    if (native.starts_with(L"\\\\")) {
        return std::filesystem::path(
            std::wstring(L"\\\\?\\UNC\\") + native.substr(2u));
    }
    return std::filesystem::path(std::wstring(kExtendedPrefix) + native);
#else
    return path;
#endif
}

bool snapshotFileBytes(const std::filesystem::path& path,
                       FileByteSnapshot& out, std::string* error,
                       std::string_view label,
                       std::uintmax_t maximum_bytes) {
    const auto normalized = normalizedPublicPath(path);
    const auto io_path = pathForFilesystemIo(normalized);
    try {
        std::error_code filesystem_error;
        if (!std::filesystem::is_regular_file(io_path, filesystem_error)) {
            setSnapshotError(
                error, label, "file-check", normalized,
                filesystem_error ? filesystem_error.message()
                                 : "path is missing or is not a regular file");
            return false;
        }

        const auto initial_size =
            std::filesystem::file_size(io_path, filesystem_error);
        if (filesystem_error) {
            setSnapshotError(error, label, "file-check", normalized,
                             filesystem_error.message());
            return false;
        }
        const auto effective_maximum =
            (std::min)(maximum_bytes, kFileByteSnapshotMaximumBytes);
        if (initial_size > effective_maximum) {
            setSnapshotError(
                error, label, "budget", normalized,
                "encoded file is " + std::to_string(initial_size) +
                    " bytes; maximum is " +
                    std::to_string(effective_maximum) + " bytes");
            return false;
        }

        const auto initial_time =
            std::filesystem::last_write_time(io_path, filesystem_error);
        if (filesystem_error) {
            setSnapshotError(error, label, "file-check", normalized,
                             filesystem_error.message());
            return false;
        }

        std::ifstream input(io_path, std::ios::binary);
        if (!input) {
            setSnapshotError(error, label, "read", normalized,
                             "could not open the file");
            return false;
        }
        auto mutable_bytes = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(initial_size));
        if (!mutable_bytes->empty()) {
            input.read(reinterpret_cast<char*>(mutable_bytes->data()),
                       static_cast<std::streamsize>(mutable_bytes->size()));
            if (!input ||
                input.gcount() !=
                    static_cast<std::streamsize>(mutable_bytes->size())) {
                setSnapshotError(error, label, "read", normalized,
                                 "file could not be read completely");
                return false;
            }
        }

        filesystem_error.clear();
        const auto final_size =
            std::filesystem::file_size(io_path, filesystem_error);
        if (filesystem_error) {
            setSnapshotError(error, label, "read", normalized,
                             "size recheck failed: " +
                                 filesystem_error.message());
            return false;
        }
        filesystem_error.clear();
        const auto final_time =
            std::filesystem::last_write_time(io_path, filesystem_error);
        if (filesystem_error || final_size != initial_size ||
            final_time != initial_time) {
            setSnapshotError(
                error, label, "read", normalized,
                filesystem_error
                    ? "timestamp recheck failed: " +
                          filesystem_error.message()
                    : "file changed while it was being read");
            return false;
        }

        FileByteSnapshot candidate;
        candidate.path = normalized;
        candidate.size = initial_size;
        candidate.write_time = initial_time;
        candidate.bytes = std::move(mutable_bytes);
        if (!candidate.valid()) {
            setSnapshotError(error, label, "read", normalized,
                             "snapshot invariants are invalid");
            return false;
        }
        out = std::move(candidate);
        if (error != nullptr) {
            error->clear();
        }
        return true;
    } catch (const std::bad_alloc&) {
        setSnapshotError(error, label, "budget", normalized,
                         "std::bad_alloc");
        return false;
    } catch (const std::length_error& exception) {
        setSnapshotError(error, label, "budget", normalized,
                         exception.what());
        return false;
    } catch (const std::filesystem::filesystem_error& exception) {
        setSnapshotError(error, label, "file-check", normalized,
                         exception.what());
        return false;
    } catch (const std::exception& exception) {
        setSnapshotError(error, label, "read", normalized,
                         exception.what());
        return false;
    }
}

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

void TextureImage::sampleModelAtlasClamp(double u, double v, float& r,
                                         float& g, float& b, float& a) const {
    if (!valid() || !std::isfinite(u) || !std::isfinite(v)) {
        r = g = b = a = 1.0f;
        return;
    }

    const double clamped_u = std::clamp(u, 0.0, 1.0);
    const double clamped_v = std::clamp(v, 0.0, 1.0);
    const int x = std::clamp(
        static_cast<int>(std::floor(clamped_u * static_cast<double>(width))),
        0, width - 1);
    const int y = std::clamp(
        static_cast<int>(std::floor(clamped_v * static_cast<double>(height))),
        0, height - 1);
    const std::uint8_t* p =
        rgba.data() +
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(x)) *
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
        setErrorNoThrow(
            err,
            "texture Header stage failed (stbi_info_from_memory): empty image buffer");
        return false;
    }
    const TextureDecodeSource source{static_cast<const stbi_uc*>(data), size};
    return decodeTexture(source, static_cast<std::size_t>(size), nullptr, out,
                         err, limits);
}

bool inspectTextureImageFromMemory(const void* data, int size,
                                   TextureImageHeader& out,
                                   std::string* err,
                                   TextureDecodeLimits limits) {
    if (!data || size <= 0) {
        setErrorNoThrow(
            err,
            "texture Header stage failed (stbi_info_from_memory): empty image buffer");
        return false;
    }
    const TextureDecodeSource source{static_cast<const stbi_uc*>(data), size};
    TextureDecodeContext context{};
    TextureImageHeader candidate;
    if (!inspectTextureSource(source, static_cast<std::size_t>(size), 0u,
                              candidate, context, err, limits)) {
        return false;
    }
    out = candidate;
    return true;
}

bool loadTextureImage(const std::filesystem::path& path, TextureImage& out,
                      std::string* err, TextureDecodeLimits limits) {
    try {
        std::size_t retained_with_previous_output = 0;
        if (!checkedAdd(limits.retained_resident_bytes, out.rgba.capacity(),
                        retained_with_previous_output)) {
            setErrorNoThrow(
                err,
                "Texture budget stage failed: retained byte arithmetic overflow");
            return false;
        }
        limits.retained_resident_bytes = retained_with_previous_output;

        FileByteSnapshot snapshot;
        const std::size_t effective_peak_limit =
            (std::min)(limits.maximum_peak_bytes,
                       kTextureDecodeMaximumPeakBytes);
        if (limits.retained_resident_bytes > effective_peak_limit) {
            setErrorNoThrow(
                err,
                "Texture budget stage failed before snapshot: retained state already exceeds the peak limit");
            return false;
        }
        const std::size_t snapshot_limit =
            effective_peak_limit - limits.retained_resident_bytes;
        if (!snapshotFileBytes(path, snapshot, err, "Texture",
                               snapshot_limit)) {
            return false;
        }

        TextureImage candidate;
        if (!loadTextureImageFromMemory(snapshot.bytes->data(),
                                        static_cast<int>(snapshot.bytes->size()),
                                        candidate, err, limits)) {
            const std::string decoded_path = pathUtf8String(snapshot.path);
            std::fprintf(stderr, "texture load failed: %s (%s)\n",
                         decoded_path.c_str(),
                         err != nullptr && !err->empty() ? err->c_str() : "?");
            return false;
        }
        candidate.path = pathUtf8String(snapshot.path);
        out = std::move(candidate);
        if (err != nullptr) {
            err->clear();
        }
        return true;
    } catch (const std::bad_alloc&) {
        setErrorNoThrow(
            err,
            "Texture budget stage failed: texture decode allocation failed: std::bad_alloc");
        return false;
    } catch (const std::filesystem::filesystem_error& exception) {
        if (err != nullptr) {
            try {
                *err = std::string("Texture file-check stage failed: ") +
                       exception.what();
            } catch (...) {
                setErrorNoThrow(err, "Texture file-check stage failed");
            }
        }
        return false;
    }
}

}
