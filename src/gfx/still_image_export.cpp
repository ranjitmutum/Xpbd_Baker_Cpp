#include "xpbd/gfx/still_image_export.hpp"

#include "xpbd/gfx/labpbr_authoring.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

namespace xpbd::gfx {
namespace {

void setError(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

template <typename T>
void appendLittleEndian(std::vector<std::uint8_t> &bytes, T value) {
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;
  Unsigned bits = static_cast<Unsigned>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bytes.push_back(static_cast<std::uint8_t>(
        (bits >> static_cast<unsigned>(i * 8u)) & 0xffu));
  }
}

void appendFloat(std::vector<std::uint8_t> &bytes, float value) {
  appendLittleEndian(bytes, std::bit_cast<std::uint32_t>(value));
}

void appendCString(std::vector<std::uint8_t> &bytes, const char *text) {
  const std::size_t length = std::strlen(text);
  bytes.insert(bytes.end(), text, text + length);
  bytes.push_back(0u);
}

void appendAttribute(std::vector<std::uint8_t> &header, const char *name,
                     const char *type,
                     const std::vector<std::uint8_t> &value) {
  appendCString(header, name);
  appendCString(header, type);
  appendLittleEndian(header, static_cast<std::uint32_t>(value.size()));
  header.insert(header.end(), value.begin(), value.end());
}

[[nodiscard]] float halfToFloat(std::uint16_t half) noexcept {
  const std::uint32_t sign =
      static_cast<std::uint32_t>(half & 0x8000u) << 16u;
  std::uint32_t exponent = (half >> 10u) & 0x1fu;
  std::uint32_t mantissa = half & 0x03ffu;
  std::uint32_t bits = 0;
  if (exponent == 0u) {
    if (mantissa == 0u) {
      bits = sign;
    } else {
      std::int32_t unbiased = -14;
      while ((mantissa & 0x0400u) == 0u) {
        mantissa <<= 1u;
        --unbiased;
      }
      mantissa &= 0x03ffu;
      bits = sign |
             (static_cast<std::uint32_t>(unbiased + 127) << 23u) |
             (mantissa << 13u);
    }
  } else if (exponent == 31u) {
    bits = sign | 0x7f800000u | (mantissa << 13u);
  } else {
    bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
  }
  return std::bit_cast<float>(bits);
}

[[nodiscard]] float linearToSrgb(float value) noexcept {
  value = std::clamp(value, 0.0f, 1.0f);
  return value <= 0.0031308f
             ? value * 12.92f
             : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] float decodeSrgbDisplayChannel(float value) noexcept {
  value = std::clamp(value, 0.0f, 1.0f);
  return value <= 0.04045f
             ? value / 12.92f
             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  const std::uint32_t sign = (bits >> 16u) & 0x8000u;
  const std::uint32_t exponent = (bits >> 23u) & 0xffu;
  const std::uint32_t mantissa = bits & 0x7fffffu;
  if (exponent == 0xffu) {
    return static_cast<std::uint16_t>(sign | (mantissa == 0u ? 0x7c00u
                                                             : 0x7e00u));
  }
  const int half_exponent = static_cast<int>(exponent) - 127 + 15;
  if (half_exponent >= 31) {
    return static_cast<std::uint16_t>(sign | 0x7c00u);
  }
  if (half_exponent <= 0) {
    if (half_exponent < -10) {
      return static_cast<std::uint16_t>(sign);
    }
    const std::uint32_t normalized = mantissa | 0x800000u;
    const unsigned shift = static_cast<unsigned>(14 - half_exponent);
    std::uint32_t half_mantissa = normalized >> shift;
    if (((normalized >> (shift - 1u)) & 1u) != 0u) {
      ++half_mantissa;
    }
    return static_cast<std::uint16_t>(sign | half_mantissa);
  }
  std::uint32_t half_mantissa = mantissa >> 13u;
  std::uint32_t rounded_exponent = static_cast<std::uint32_t>(half_exponent);
  if ((mantissa & 0x1000u) != 0u) {
    ++half_mantissa;
    if (half_mantissa == 0x400u) {
      half_mantissa = 0u;
      ++rounded_exponent;
      if (rounded_exponent >= 31u) {
        return static_cast<std::uint16_t>(sign | 0x7c00u);
      }
    }
  }
  return static_cast<std::uint16_t>(
      sign | (rounded_exponent << 10u) | half_mantissa);
}

[[nodiscard]] bool sampleCubemapBackground(
    const StillImageCubemapBackground &background, std::uint32_t image_width,
    std::uint32_t image_height, std::uint32_t pixel_x,
    std::uint32_t pixel_y, std::array<std::uint8_t, 4> &sample) noexcept {
  if (!background.valid() || image_width == 0u || image_height == 0u) {
    return false;
  }
  const float uv_x = (static_cast<float>(pixel_x) + 0.5f) /
                     static_cast<float>(image_width);
  const float uv_y = (static_cast<float>(pixel_y) + 0.5f) /
                     static_cast<float>(image_height);
  const std::array<float, 4> clip{uv_x * 2.0f - 1.0f,
                                  1.0f - uv_y * 2.0f, 1.0f, 1.0f};
  std::array<float, 4> world{};
  for (std::size_t row = 0; row < 4u; ++row) {
    world[row] =
        background.inverse_view_projection[row + 0u] * clip[0] +
        background.inverse_view_projection[row + 4u] * clip[1] +
        background.inverse_view_projection[row + 8u] * clip[2] +
        background.inverse_view_projection[row + 12u] * clip[3];
  }
  const float divisor = (std::max)(std::abs(world[3]), 1.0e-6f);
  float dx = world[0] / divisor - background.camera_position[0];
  float dy = world[1] / divisor - background.camera_position[1];
  float dz = world[2] / divisor - background.camera_position[2];
  const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!std::isfinite(length) || length <= 1.0e-6f) {
    return false;
  }
  dx /= length;
  dy /= length;
  dz /= length;

  const float ax = std::abs(dx);
  const float ay = std::abs(dy);
  const float az = std::abs(dz);
  std::uint32_t face = 0u;
  float uc = 0.0f;
  float vc = 0.0f;
  if (ax >= ay && ax >= az) {
    if (dx >= 0.0f) {
      face = 0u;
      uc = -dz / ax;
      vc = -dy / ax;
    } else {
      face = 1u;
      uc = dz / ax;
      vc = -dy / ax;
    }
  } else if (ay >= az) {
    if (dy >= 0.0f) {
      face = 2u;
      uc = dx / ay;
      vc = dz / ay;
    } else {
      face = 3u;
      uc = dx / ay;
      vc = -dz / ay;
    }
  } else if (dz >= 0.0f) {
    face = 4u;
    uc = dx / az;
    vc = -dy / az;
  } else {
    face = 5u;
    uc = -dx / az;
    vc = -dy / az;
  }

  const float face_size = static_cast<float>(background.face_size);
  const float texel_x =
      std::clamp((uc * 0.5f + 0.5f) * face_size - 0.5f, 0.0f,
                 face_size - 1.0f);
  const float texel_y =
      std::clamp((vc * 0.5f + 0.5f) * face_size - 0.5f, 0.0f,
                 face_size - 1.0f);
  const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(texel_x));
  const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(texel_y));
  const std::uint32_t x1 = (std::min)(x0 + 1u, background.face_size - 1u);
  const std::uint32_t y1 = (std::min)(y0 + 1u, background.face_size - 1u);
  const float tx = texel_x - static_cast<float>(x0);
  const float ty = texel_y - static_cast<float>(y0);
  const std::size_t face_offset =
      static_cast<std::size_t>(face) * background.face_size *
      background.face_size * 4u;
  const auto channel = [&](std::uint32_t x, std::uint32_t y,
                           std::size_t c) {
    const std::size_t index =
        face_offset +
        (static_cast<std::size_t>(y) * background.face_size + x) * 4u + c;
    return static_cast<float>(background.rgba8[index]);
  };
  for (std::size_t c = 0; c < 4u; ++c) {
    const float top = channel(x0, y0, c) * (1.0f - tx) +
                      channel(x1, y0, c) * tx;
    const float bottom = channel(x0, y1, c) * (1.0f - tx) +
                         channel(x1, y1, c) * tx;
    sample[c] = static_cast<std::uint8_t>(std::lround(
        std::clamp(top * (1.0f - ty) + bottom * ty, 0.0f, 255.0f)));
  }
  return true;
}

[[nodiscard]] bool validInput(std::uint32_t width, std::uint32_t height,
                              const std::uint16_t *rgba16f,
                              std::size_t half_word_count,
                              std::string *error) {
  if (width == 0u || height == 0u || rgba16f == nullptr) {
    setError(error, "still image dimensions or pixel pointer are invalid");
    return false;
  }
  const std::size_t max_pixels =
      (std::numeric_limits<std::size_t>::max)() / 4u;
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (pixel_count > max_pixels || half_word_count < pixel_count * 4u) {
    setError(error, "still image readback is smaller than its dimensions");
    return false;
  }
  return true;
}

[[nodiscard]] std::uint16_t finiteHalfOrZero(std::uint16_t value) noexcept {
  const std::uint16_t exponent = value & 0x7c00u;
  return exponent == 0x7c00u ? 0u : value;
}

} // namespace

bool encodeOpenExrRgba16f(
    std::uint32_t width, std::uint32_t height,
    const std::uint16_t *rgba16f, std::size_t half_word_count,
    bool transparent_background, std::vector<std::uint8_t> &encoded,
    std::string *error) {
  encoded.clear();
  if (!validInput(width, height, rgba16f, half_word_count, error)) {
    return false;
  }
  if (width > static_cast<std::uint32_t>(
                  (std::numeric_limits<std::int32_t>::max)()) ||
      height > static_cast<std::uint32_t>(
                   (std::numeric_limits<std::int32_t>::max)())) {
    setError(error, "OpenEXR dimensions exceed the scanline format");
    return false;
  }

  std::vector<std::uint8_t> header;
  appendLittleEndian(header, std::uint32_t{20000630u});
  appendLittleEndian(header, std::uint32_t{2u});

  std::vector<std::uint8_t> channels;
  // OpenEXR ChannelList is ordered lexicographically; scanline payload planes
  // must follow this exact header order.
  for (const char *channel : {"A", "B", "G", "R"}) {
    appendCString(channels, channel);
    appendLittleEndian(channels, std::int32_t{1}); // HALF
    channels.push_back(0u);                        // pLinear
    channels.insert(channels.end(), 3u, 0u);
    appendLittleEndian(channels, std::int32_t{1});
    appendLittleEndian(channels, std::int32_t{1});
  }
  channels.push_back(0u);
  appendAttribute(header, "channels", "chlist", channels);

  appendAttribute(header, "compression", "compression", {0u});

  std::vector<std::uint8_t> window;
  appendLittleEndian(window, std::int32_t{0});
  appendLittleEndian(window, std::int32_t{0});
  appendLittleEndian(window, static_cast<std::int32_t>(width - 1u));
  appendLittleEndian(window, static_cast<std::int32_t>(height - 1u));
  appendAttribute(header, "dataWindow", "box2i", window);
  appendAttribute(header, "displayWindow", "box2i", window);
  appendAttribute(header, "lineOrder", "lineOrder", {0u});

  std::vector<std::uint8_t> scalar;
  appendFloat(scalar, 1.0f);
  appendAttribute(header, "pixelAspectRatio", "float", scalar);
  std::vector<std::uint8_t> center;
  appendFloat(center, 0.0f);
  appendFloat(center, 0.0f);
  appendAttribute(header, "screenWindowCenter", "v2f", center);
  appendAttribute(header, "screenWindowWidth", "float", scalar);
  header.push_back(0u);

  constexpr std::uint64_t kBlockHeaderBytes = 8u;
  const std::uint64_t scanline_data_bytes =
      static_cast<std::uint64_t>(width) * 4u * sizeof(std::uint16_t);
  const std::uint64_t block_bytes =
      kBlockHeaderBytes + scanline_data_bytes;
  const std::uint64_t offset_table_bytes =
      static_cast<std::uint64_t>(height) * sizeof(std::uint64_t);
  const std::uint64_t file_bytes =
      static_cast<std::uint64_t>(header.size()) + offset_table_bytes +
      static_cast<std::uint64_t>(height) * block_bytes;
  if (file_bytes >
      static_cast<std::uint64_t>(
          (std::numeric_limits<std::size_t>::max)())) {
    setError(error, "OpenEXR output is too large for this process");
    return false;
  }
  encoded.reserve(static_cast<std::size_t>(file_bytes));
  encoded = header;

  std::uint64_t block_offset =
      static_cast<std::uint64_t>(header.size()) + offset_table_bytes;
  for (std::uint32_t y = 0; y < height; ++y) {
    appendLittleEndian(encoded, block_offset);
    block_offset += block_bytes;
  }

  constexpr std::array<std::size_t, 4> kChannelOrder{3u, 2u, 1u, 0u};
  for (std::uint32_t y = 0; y < height; ++y) {
    appendLittleEndian(encoded, static_cast<std::int32_t>(y));
    appendLittleEndian(encoded,
                       static_cast<std::uint32_t>(scanline_data_bytes));
    for (const std::size_t channel : kChannelOrder) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const std::size_t pixel =
            (static_cast<std::size_t>(y) * width + x) * 4u;
        const std::uint16_t half =
            channel == 3u && !transparent_background
                ? std::uint16_t{0x3c00u}
                : finiteHalfOrZero(rgba16f[pixel + channel]);
        appendLittleEndian(encoded, half);
      }
    }
  }
  return true;
}

bool writeStillImageRgba16f(
    const std::filesystem::path &path, StillImageFormat format,
    std::uint32_t width, std::uint32_t height,
    const std::uint16_t *rgba16f, std::size_t half_word_count,
    const StillImageDisplayTransform &display, bool transparent_background,
    const float *device_depth, std::size_t depth_count,
    std::string *error, const StillImageCubemapBackground *background) {
  if (!validInput(width, height, rgba16f, half_word_count, error)) {
    return false;
  }

  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * height;
  const bool valid_depth =
      device_depth != nullptr && depth_count >= pixel_count;
  if (background != nullptr && !background->valid()) {
    setError(error, "still image cubemap background is invalid");
    return false;
  }
  const auto far_depth_pixel = [&](std::size_t pixel) {
    return valid_depth && std::isfinite(device_depth[pixel]) &&
           device_depth[pixel] >= 0.999999f;
  };
  const auto source_alpha = [&](std::size_t pixel) {
    const float value = halfToFloat(rgba16f[pixel * 4u + 3u]);
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
  };
  const auto pure_sky_pixel = [&](std::size_t pixel) {
    return far_depth_pixel(pixel) && source_alpha(pixel) <= 1.0e-6f;
  };

  std::vector<std::uint8_t> bytes;
  if (format == StillImageFormat::Exr) {
    std::vector<std::uint16_t> transparent_pixels;
    const std::uint16_t *exr_pixels = rgba16f;
    if ((transparent_background || background != nullptr) && valid_depth) {
      transparent_pixels.assign(rgba16f, rgba16f + pixel_count * 4u);
      for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if (pure_sky_pixel(pixel) && transparent_background) {
          std::fill_n(transparent_pixels.data() + pixel * 4u, 4u,
                      std::uint16_t{0u});
        } else if (far_depth_pixel(pixel) && !transparent_background &&
                   background != nullptr) {
          const std::uint32_t x =
              static_cast<std::uint32_t>(pixel % width);
          const std::uint32_t y =
              static_cast<std::uint32_t>(pixel / width);
          std::array<std::uint8_t, 4> sky{};
          if (sampleCubemapBackground(*background, width, height, x, y,
                                      sky)) {
            const float coverage = source_alpha(pixel);
            for (std::size_t channel = 0; channel < 3u; ++channel) {
              const float sky_linear = decodeSrgbDisplayChannel(
                  static_cast<float>(sky[channel]) / 255.0f);
              float foreground =
                  halfToFloat(rgba16f[pixel * 4u + channel]);
              if (!std::isfinite(foreground)) {
                foreground = 0.0f;
              }
              transparent_pixels[pixel * 4u + channel] = floatToHalf(
                  foreground * coverage + sky_linear * (1.0f - coverage));
            }
            transparent_pixels[pixel * 4u + 3u] = 0x3c00u;
          }
        }
      }
      exr_pixels = transparent_pixels.data();
    }
    if (!encodeOpenExrRgba16f(width, height, exr_pixels,
                              pixel_count * 4u,
                              transparent_background, bytes, error)) {
      return false;
    }
  } else {
    std::vector<std::uint8_t> rgba(pixel_count * 4u, 0u);
    const float safe_exposure =
        std::isfinite(display.exposure)
            ? std::clamp(display.exposure, 0.0f, 65536.0f)
            : 1.0f;
    const float safe_kelvin =
        std::isfinite(display.white_balance_kelvin)
            ? std::clamp(display.white_balance_kelvin, 1000.0f, 40000.0f)
            : 6500.0f;
    const float temperature = safe_kelvin / 6500.0f;
    const std::array<float, 3> white_balance{
        std::pow(temperature, 0.45f), 1.0f,
        std::pow(1.0f / temperature, 0.45f)};
    const float bloom =
        std::isfinite(display.bloom_strength)
            ? (std::max)(0.0f, display.bloom_strength)
            : 0.0f;
    const auto source_rgb = [&](std::size_t pixel, std::size_t channel) {
      float value = halfToFloat(rgba16f[pixel * 4u + channel]);
      return std::isfinite(value) ? (std::max)(value, 0.0f) : 0.0f;
    };
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
      const std::uint32_t x =
          static_cast<std::uint32_t>(pixel % width);
      const std::uint32_t y =
          static_cast<std::uint32_t>(pixel / width);
      if (pure_sky_pixel(pixel) && transparent_background) {
        std::fill_n(rgba.data() + pixel * 4u, 4u, std::uint8_t{0u});
        continue;
      }
      std::array<std::uint8_t, 4> sky{};
      const bool composite_preview_sky =
          !transparent_background && background != nullptr &&
          far_depth_pixel(pixel) &&
          sampleCubemapBackground(*background, width, height, x, y, sky);
      for (std::size_t channel = 0; channel < 3u; ++channel) {
        float value =
            source_rgb(pixel, channel) * safe_exposure *
            white_balance[channel];
        if (bloom > 0.0f) {
          float glow = 0.0f;
          for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
              const std::uint32_t sample_x =
                  static_cast<std::uint32_t>(std::clamp(
                      static_cast<int>(x) + ox, 0,
                      static_cast<int>(width) - 1));
              const std::uint32_t sample_y =
                  static_cast<std::uint32_t>(std::clamp(
                      static_cast<int>(y) + oy, 0,
                      static_cast<int>(height) - 1));
              const std::size_t sample_pixel =
                  static_cast<std::size_t>(sample_y) * width + sample_x;
              glow += (std::max)(
                  source_rgb(sample_pixel, channel) * safe_exposure - 1.0f,
                  0.0f);
            }
          }
          value += glow * (bloom / 9.0f);
        }
        if (display.tone_mapping == 1u) {
          value = value / (1.0f + value);
        } else if (display.tone_mapping == 2u) {
          value = std::clamp(
              (value * (2.51f * value + 0.03f)) /
                  (value * (2.43f * value + 0.59f) + 0.14f),
              0.0f, 1.0f);
        }
        rgba[pixel * 4u + channel] = static_cast<std::uint8_t>(
            std::lround(linearToSrgb(value) * 255.0f));
      }
      const float coverage = source_alpha(pixel);
      if (composite_preview_sky) {
        for (std::size_t channel = 0; channel < 3u; ++channel) {
          rgba[pixel * 4u + channel] = static_cast<std::uint8_t>(std::lround(
              static_cast<float>(rgba[pixel * 4u + channel]) * coverage +
              static_cast<float>(sky[channel]) * (1.0f - coverage)));
        }
      }
      rgba[pixel * 4u + 3u] = static_cast<std::uint8_t>(
          std::lround((transparent_background ? coverage : 1.0f) * 255.0f));
    }
    if (!encodePngRgba8(static_cast<int>(width), static_cast<int>(height),
                        rgba, bytes, error)) {
      return false;
    }
  }

  std::error_code filesystem_error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(),
                                        filesystem_error);
  }
  if (filesystem_error) {
    setError(error, "cannot create output directory: " +
                        filesystem_error.message());
    return false;
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    setError(error, "cannot open still image output");
    return false;
  }
  stream.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!stream.good()) {
    setError(error, "still image output write failed");
    return false;
  }
  return true;
}

} // namespace xpbd::gfx
