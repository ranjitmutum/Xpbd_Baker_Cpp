#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpbd::gfx {

inline constexpr std::uint32_t kTextureDecodeMaximumWidth = 16'384u;
inline constexpr std::uint32_t kTextureDecodeMaximumHeight = 16'384u;
inline constexpr std::size_t kTextureDecodeMaximumPixels = 134'217'728u;
inline constexpr std::size_t kTextureDecodeMaximumRgbaBytes =
    std::size_t{512} * 1024u * 1024u;
// The decoder and the candidate TextureImage coexist until the transactional
// commit. Keep their combined allocation (plus retained caller state) bounded.
inline constexpr std::size_t kTextureDecodeMaximumPeakBytes =
    std::size_t{1024} * 1024u * 1024u;
inline constexpr std::uintmax_t kFileByteSnapshotMaximumBytes =
    static_cast<std::uintmax_t>((std::numeric_limits<int>::max)());

struct TextureDecodeLimits {
    std::uint32_t maximum_width = kTextureDecodeMaximumWidth;
    std::uint32_t maximum_height = kTextureDecodeMaximumHeight;
    std::size_t maximum_pixels = kTextureDecodeMaximumPixels;
    std::size_t maximum_decoded_bytes = kTextureDecodeMaximumRgbaBytes;
    std::size_t maximum_peak_bytes = kTextureDecodeMaximumPeakBytes;
    // Caller-owned memory that remains resident during decode, excluding the
    // current contents of the `out` TextureImage (which are counted here).
    std::size_t retained_resident_bytes = 0;
};

struct TextureImageHeader {
    int width = 0;
    int height = 0;
    int source_channels = 0;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && source_channels > 0;
    }
};

[[nodiscard]] bool checkedTextureRgbaByteCount(
    std::size_t width, std::size_t height,
    std::size_t& byte_count) noexcept;

struct FileByteSnapshot {
    std::filesystem::path path;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type write_time{};
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;

    [[nodiscard]] bool valid() const noexcept {
        return !path.empty() && bytes != nullptr &&
               size <= kFileByteSnapshotMaximumBytes &&
               bytes->size() == static_cast<std::size_t>(size);
    }
};

[[nodiscard]] std::string
pathUtf8String(const std::filesystem::path& path);

// Windows file operations use an internal extended-length spelling. Session
// metadata and diagnostics continue to retain the ordinary absolute path.
[[nodiscard]] std::filesystem::path
pathForFilesystemIo(const std::filesystem::path& path);

bool snapshotFileBytes(
    const std::filesystem::path& path, FileByteSnapshot& out,
    std::string* error = nullptr, std::string_view label = "Texture",
    std::uintmax_t maximum_bytes = kFileByteSnapshotMaximumBytes);

struct TextureImage {
    int width = 0;
    int height = 0;
    int source_channels = 0;
    std::vector<std::uint8_t> rgba;
    std::string path;

    [[nodiscard]] bool valid() const {
        std::size_t expected_bytes = 0;
        return width > 0 && height > 0 &&
               checkedTextureRgbaByteCount(
                   static_cast<std::size_t>(width),
                   static_cast<std::size_t>(height), expected_bytes) &&
               rgba.size() == expected_bytes;
    }

    void clear() {
        width = height = source_channels = 0;
        rgba.clear();
        path.clear();
    }


    void sample(float u, float v, float& r, float& g, float& b, float& a) const;

    // Model atlases have a resolved finite UV domain. Sampling outside the
    // normalized edge must never wrap into an unrelated atlas cell.
    void sampleModelAtlasClamp(double u, double v, float& r, float& g,
                               float& b, float& a) const;

    void sample(float u, float v, float& r, float& g, float& b) const {
        float a = 1.0f;
        sample(u, v, r, g, b, a);
    }
};

// Model-material images are immutable after a successful transaction. Keep
// sharing local to the LabPBR ownership chain instead of changing the storage
// semantics of TextureImage throughout the project.
using SharedTextureImage = std::shared_ptr<const TextureImage>;

bool loadTextureImage(const std::filesystem::path& path, TextureImage& out,
                      std::string* err = nullptr,
                      TextureDecodeLimits limits = {});

bool loadTextureImageFromMemory(const void* data, int size, TextureImage& out,
                                 std::string* err = nullptr,
                                 TextureDecodeLimits limits = {});

// Reads and validates dimensions/channels/budgets without allocating decoded
// pixels. `out` is replaced only after all checks succeed.
bool inspectTextureImageFromMemory(const void* data, int size,
                                   TextureImageHeader& out,
                                   std::string* err = nullptr,
                                   TextureDecodeLimits limits = {});

}
