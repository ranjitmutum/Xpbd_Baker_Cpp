#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xpbd::gfx {

enum class StillImageFormat : std::uint8_t {
  Png = 0,
  Exr = 1,
};

struct StillImageDisplayTransform {
  float exposure = 1.0f;
  float white_balance_kelvin = 6500.0f;
  float bloom_strength = 0.0f;
  // Matches PathTraceToneMapping: 0 None, 1 Reinhard, 2 ACES.
  std::uint32_t tone_mapping = 0;
};

// Optional display-referred preview cubemap used only for primary-ray sky
// pixels when a preset scene renders a raster skybox that is not a physical
// World environment. Faces are +X, -X, +Y, -Y, +Z, -Z RGBA8. The inverse
// view-projection and camera position use the same OpenGL clip convention as
// the path tracer's primary rays.
struct StillImageCubemapBackground {
  std::uint32_t face_size = 0;
  const std::uint8_t *rgba8 = nullptr;
  std::size_t rgba8_size = 0;
  std::array<float, 16> inverse_view_projection{};
  std::array<float, 3> camera_position{};

  [[nodiscard]] bool valid() const noexcept {
    if (face_size == 0u || rgba8 == nullptr) {
      return false;
    }
    const std::size_t face_pixels =
        static_cast<std::size_t>(face_size) * face_size;
    return face_pixels <= (static_cast<std::size_t>(-1) / 24u) &&
           rgba8_size == face_pixels * 24u;
  }
};

// Writes a Vulkan RGBA16F readback as either display-referred PNG or linear
// half-float OpenEXR. The input vector contains width * height * 4 half words.
[[nodiscard]] bool writeStillImageRgba16f(
    const std::filesystem::path &path, StillImageFormat format,
    std::uint32_t width, std::uint32_t height,
    const std::uint16_t *rgba16f, std::size_t half_word_count,
    const StillImageDisplayTransform &display, bool transparent_background,
    const float *device_depth = nullptr, std::size_t depth_count = 0,
    std::string *error = nullptr,
    const StillImageCubemapBackground *background = nullptr);

// Pure encoder exposed for deterministic regression coverage. Produces a
// standards-compatible uncompressed scanline OpenEXR byte stream.
[[nodiscard]] bool encodeOpenExrRgba16f(
    std::uint32_t width, std::uint32_t height,
    const std::uint16_t *rgba16f, std::size_t half_word_count,
    bool transparent_background, std::vector<std::uint8_t> &encoded,
    std::string *error = nullptr);

} // namespace xpbd::gfx
