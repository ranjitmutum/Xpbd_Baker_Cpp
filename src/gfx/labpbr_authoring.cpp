#include "xpbd/gfx/labpbr_authoring.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace xpbd::gfx {
namespace {

std::uint8_t encodeUnit(float value, float scale) noexcept {
  if (!std::isfinite(value)) {
    value = 0.0f;
  }
  return static_cast<std::uint8_t>(
      std::lround(std::clamp(value, 0.0f, 1.0f) * scale));
}

double edge(double ax, double ay, double bx, double by, double px,
            double py) noexcept {
  return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void appendBigEndian32(std::vector<std::uint8_t> &bytes,
                       std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 24u));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16u));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (const std::uint8_t byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          0u - static_cast<std::uint32_t>(crc & 1u);
      crc = (crc >> 1u) ^ (0xedb88320u & mask);
    }
  }
  return ~crc;
}

std::uint32_t adler32(std::span<const std::uint8_t> bytes) noexcept {
  constexpr std::uint32_t kMod = 65521u;
  std::uint32_t a = 1u;
  std::uint32_t b = 0u;
  for (const std::uint8_t byte : bytes) {
    a = (a + byte) % kMod;
    b = (b + a) % kMod;
  }
  return (b << 16u) | a;
}

void appendPngChunk(std::vector<std::uint8_t> &png, const char type[4],
                    std::span<const std::uint8_t> payload) {
  appendBigEndian32(png, static_cast<std::uint32_t>(payload.size()));
  const std::size_t crc_begin = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), payload.begin(), payload.end());
  appendBigEndian32(
      png, crc32(std::span<const std::uint8_t>(png).subspan(
               crc_begin, 4u + payload.size())));
}

std::uint32_t rotateRight(std::uint32_t value, unsigned shift) noexcept {
  return std::rotr(value, static_cast<int>(shift));
}

std::array<std::uint8_t, 32>
sha256(std::span<const std::uint8_t> input) {
  static constexpr std::array<std::uint32_t, 64> kRound{
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
      0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
      0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
      0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
      0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
      0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
      0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  std::array<std::uint32_t, 8> state{
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  const std::uint64_t bit_length =
      static_cast<std::uint64_t>(input.size()) * 8u;
  std::vector<std::uint8_t> padded(input.begin(), input.end());
  padded.push_back(0x80u);
  while ((padded.size() % 64u) != 56u) {
    padded.push_back(0u);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(
        static_cast<std::uint8_t>(bit_length >> static_cast<unsigned>(shift)));
  }

  for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64u) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16u; ++i) {
      const std::size_t offset = chunk + i * 4u;
      words[i] = (static_cast<std::uint32_t>(padded[offset]) << 24u) |
                 (static_cast<std::uint32_t>(padded[offset + 1u]) << 16u) |
                 (static_cast<std::uint32_t>(padded[offset + 2u]) << 8u) |
                 static_cast<std::uint32_t>(padded[offset + 3u]);
    }
    for (std::size_t i = 16u; i < words.size(); ++i) {
      const std::uint32_t s0 =
          rotateRight(words[i - 15u], 7u) ^
          rotateRight(words[i - 15u], 18u) ^ (words[i - 15u] >> 3u);
      const std::uint32_t s1 =
          rotateRight(words[i - 2u], 17u) ^
          rotateRight(words[i - 2u], 19u) ^ (words[i - 2u] >> 10u);
      words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
    }

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::uint32_t big_sigma1 =
          rotateRight(e, 6u) ^ rotateRight(e, 11u) ^ rotateRight(e, 25u);
      const std::uint32_t choose = (e & f) ^ (~e & g);
      const std::uint32_t temp1 =
          h + big_sigma1 + choose + kRound[i] + words[i];
      const std::uint32_t big_sigma0 =
          rotateRight(a, 2u) ^ rotateRight(a, 13u) ^ rotateRight(a, 22u);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = big_sigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t i = 0; i < state.size(); ++i) {
    digest[i * 4u] = static_cast<std::uint8_t>(state[i] >> 24u);
    digest[i * 4u + 1u] = static_cast<std::uint8_t>(state[i] >> 16u);
    digest[i * 4u + 2u] = static_cast<std::uint8_t>(state[i] >> 8u);
    digest[i * 4u + 3u] = static_cast<std::uint8_t>(state[i]);
  }
  return digest;
}

struct ChannelClaim {
  std::uint8_t value = 0;
  std::set<std::string> groups;
  std::set<std::uint8_t> values;
};

void claimChannel(
    std::uint32_t texel, LabPbrOverrideChannel channel, std::uint8_t value,
    const std::string &group, TextureImage &output,
    std::map<std::uint64_t, ChannelClaim> &claims) {
  const auto channel_index = static_cast<std::uint8_t>(channel);
  const std::uint64_t key =
      static_cast<std::uint64_t>(texel) * 4u + channel_index;
  auto [it, inserted] =
      claims.try_emplace(key, ChannelClaim{value, {group}, {value}});
  it->second.groups.insert(group);
  it->second.values.insert(value);
  if (inserted || it->second.value == value) {
    output.rgba[static_cast<std::size_t>(texel) * 4u + channel_index] =
        value;
  }
}

} // namespace

bool LabPbrUvCoverage::valid() const noexcept {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const auto texel_count = static_cast<std::uint64_t>(width) *
                           static_cast<std::uint64_t>(height);
  if (texel_count >
      static_cast<std::uint64_t>(
          (std::numeric_limits<std::uint32_t>::max)())) {
    return false;
  }
  for (const auto &[group, runs] : group_runs) {
    (void)group;
    bool have_previous = false;
    UvRun previous{};
    for (const UvRun &run : runs) {
      if (run.y >= static_cast<std::uint32_t>(height) || run.x0 > run.x1 ||
          run.x1 >= static_cast<std::uint32_t>(width)) {
        return false;
      }
      if (have_previous &&
          (run.y < previous.y ||
           (run.y == previous.y && run.x0 <= previous.x1 + 1u))) {
        return false;
      }
      previous = run;
      have_previous = true;
    }
  }
  return true;
}

const std::vector<UvRun> *
LabPbrUvCoverage::find(std::string_view group_name) const {
  const auto found = group_runs.find(group_name);
  return found == group_runs.end() ? nullptr : &found->second;
}

std::uint64_t
LabPbrUvCoverage::texelCount(std::string_view group_name) const noexcept {
  const auto found = group_runs.find(group_name);
  if (found == group_runs.end()) {
    return 0u;
  }
  std::uint64_t count = 0u;
  for (const UvRun &run : found->second) {
    const std::uint64_t run_count =
        static_cast<std::uint64_t>(run.x1) - run.x0 + 1u;
    if (run_count > (std::numeric_limits<std::uint64_t>::max)() - count) {
      return (std::numeric_limits<std::uint64_t>::max)();
    }
    count += run_count;
  }
  return count;
}

std::optional<std::uint32_t>
LabPbrUvCoverage::firstTexel(std::string_view group_name) const noexcept {
  const auto found = group_runs.find(group_name);
  if (found == group_runs.end() || found->second.empty() || width <= 0) {
    return std::nullopt;
  }
  const UvRun &run = found->second.front();
  const auto texel = static_cast<std::uint64_t>(run.y) *
                         static_cast<std::uint64_t>(width) +
                     run.x0;
  if (texel > static_cast<std::uint64_t>(
                  (std::numeric_limits<std::uint32_t>::max)())) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(texel);
}

std::uint8_t encodeLabPbrEmission(float emission) noexcept {
  return encodeUnit(emission, 254.0f);
}

std::uint8_t encodeLabPbrRoughness(float roughness) noexcept {
  if (!std::isfinite(roughness)) {
    roughness = 1.0f;
  }
  return encodeUnit(1.0f - std::clamp(roughness, 0.0f, 1.0f), 255.0f);
}

std::uint8_t encodeLabPbrPorosity(float porosity) noexcept {
  return encodeUnit(porosity, 64.0f);
}

std::uint8_t encodeLabPbrSubsurface(float subsurface) noexcept {
  return static_cast<std::uint8_t>(
      65u + encodeUnit(subsurface, 190.0f));
}

bool validGroupLabPbrOverride(const GroupLabPbrOverride &override_value,
                              std::string *error) {
  const auto invalid_unit = [](float value) {
    return !std::isfinite(value) || value < 0.0f || value > 1.0f;
  };
  const char *message = nullptr;
  if (override_value.group_name.empty()) {
    message = "group name is empty";
  } else if (override_value.emission_enabled &&
             invalid_unit(override_value.emission)) {
    message = "emission must be finite and in [0,1]";
  } else if (override_value.roughness_enabled &&
             invalid_unit(override_value.roughness)) {
    message = "roughness must be finite and in [0,1]";
  } else if (override_value.porosity_enabled &&
             override_value.subsurface_scattering &&
             invalid_unit(override_value.subsurface)) {
    message = "subsurface scattering must be finite and in [0,1]";
  } else if (override_value.porosity_enabled &&
             !override_value.subsurface_scattering &&
             invalid_unit(override_value.porosity)) {
    message = "porosity must be finite and in [0,1]";
  } else if (override_value.metal_enabled && override_value.metal &&
             override_value.metal_code < 230u) {
    message = "metal code must be in [230,255]";
  } else if (override_value.metal_enabled && !override_value.metal &&
             override_value.dielectric_f0 > 229u) {
    message = "dielectric F0 byte must be in [0,229]";
  }
  if (error != nullptr) {
    *error = message == nullptr ? std::string{} : std::string(message);
  }
  return message == nullptr;
}

bool rasterizeLabPbrUvCoverage(const StaticIndexedModelMesh &mesh, int width,
                               int height, LabPbrUvCoverage &out,
                               std::string *error) {
  const auto fail_coverage = [&](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (width <= 0 || height <= 0) {
    return fail_coverage("UV coverage dimensions must be positive");
  }
  if (!mesh.uv_domain.valid()) {
    return fail_coverage("UV coverage requires a resolved model UV Domain");
  }
  if (mesh.uv_domain.imported_width != width ||
      mesh.uv_domain.imported_height != height) {
    return fail_coverage(
        "UV coverage dimensions do not match the resolved imported atlas");
  }
  const std::size_t width_size = static_cast<std::size_t>(width);
  const std::size_t height_size = static_cast<std::size_t>(height);
  if (width_size > (std::numeric_limits<std::size_t>::max)() / height_size) {
    return fail_coverage("UV coverage dimensions overflow");
  }
  const std::size_t texel_count = width_size * height_size;
  if (texel_count > static_cast<std::size_t>(
                        (std::numeric_limits<std::uint32_t>::max)())) {
    return fail_coverage("UV coverage texel indices exceed uint32 range");
  }
  try {
    LabPbrUvCoverage result;
    result.width = width;
    result.height = height;
    std::map<std::string, std::vector<const StaticModelFace *>, std::less<>>
        grouped_faces;
    for (const StaticModelFace &face : mesh.faces) {
      if (!face.textured) {
        continue;
      }
      if (face.bone_index >= mesh.bone_names.size() ||
          face.first_vertex > mesh.vertices.size() ||
          face.vertex_count > mesh.vertices.size() - face.first_vertex ||
          face.vertex_count == 0u ||
          face.first_index > mesh.indices.size() ||
          face.index_count > mesh.indices.size() - face.first_index ||
          face.index_count == 0u || face.index_count % 3u != 0u) {
        return fail_coverage("UV coverage mesh face ranges are invalid");
      }
      grouped_faces[mesh.bone_names[face.bone_index]].push_back(&face);
    }

    if (grouped_faces.empty()) {
      out = std::move(result);
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }

    std::vector<std::uint8_t> marked(texel_count, 0u);
    std::vector<std::uint32_t> touched;
    touched.reserve(texel_count);
    constexpr double kInsideEpsilon = 1.0e-9;
    constexpr double kDomainEpsilon = 1.0e-9;

    for (const auto &[group_name, faces] : grouped_faces) {
      touched.clear();
      for (const StaticModelFace *face_ptr : faces) {
        const StaticModelFace &face = *face_ptr;
        for (std::uint32_t local = 0; local + 2u < face.index_count;
             local += 3u) {
          const std::uint32_t ia = mesh.indices[face.first_index + local];
          const std::uint32_t ib =
              mesh.indices[face.first_index + local + 1u];
          const std::uint32_t ic =
              mesh.indices[face.first_index + local + 2u];
          if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() ||
              ic >= mesh.vertices.size()) {
            return fail_coverage("UV coverage mesh index is out of range");
          }
          const std::size_t vertex_end =
              static_cast<std::size_t>(face.first_vertex) +
              face.vertex_count;
          if (ia < face.first_vertex || ia >= vertex_end ||
              ib < face.first_vertex || ib >= vertex_end ||
              ic < face.first_vertex || ic >= vertex_end ||
              mesh.vertices[ia].bone_index != face.bone_index ||
              mesh.vertices[ib].bone_index != face.bone_index ||
              mesh.vertices[ic].bone_index != face.bone_index) {
            return fail_coverage(
                "UV coverage mesh face ownership is inconsistent");
          }
          const auto &a = mesh.vertices[ia];
          const auto &b = mesh.vertices[ib];
          const auto &c = mesh.vertices[ic];
          const auto atlas_coordinate = [&](double raw_u, double raw_v,
                                            double &x, double &y) {
            const double normalized_u = mesh.uv_domain.normalizeU(raw_u);
            const double normalized_v = mesh.uv_domain.normalizeV(raw_v);
            if (!std::isfinite(normalized_u) ||
                !std::isfinite(normalized_v) ||
                normalized_u < -kDomainEpsilon ||
                normalized_u > 1.0 + kDomainEpsilon ||
                normalized_v < -kDomainEpsilon ||
                normalized_v > 1.0 + kDomainEpsilon) {
              return false;
            }
            x = std::clamp(normalized_u, 0.0, 1.0) *
                static_cast<double>(width);
            y = std::clamp(normalized_v, 0.0, 1.0) *
                static_cast<double>(height);
            return true;
          };
          double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0, cx = 0.0,
                 cy = 0.0;
          if (!atlas_coordinate(a.raw_u, a.raw_v, ax, ay) ||
              !atlas_coordinate(b.raw_u, b.raw_v, bx, by) ||
              !atlas_coordinate(c.raw_u, c.raw_v, cx, cy)) {
            return fail_coverage(
                "UV coverage vertex lies outside the resolved model Domain");
          }
          const double area = edge(ax, ay, bx, by, cx, cy);
          if (!std::isfinite(area) || std::abs(area) <= kInsideEpsilon) {
            continue;
          }
          const int min_x =
              std::clamp(static_cast<int>(std::floor(
                             (std::min)({ax, bx, cx}))),
                         0, width - 1);
          const int max_x =
              std::clamp(static_cast<int>(std::ceil(
                             (std::max)({ax, bx, cx}))) -
                             1,
                         0, width - 1);
          const int min_y =
              std::clamp(static_cast<int>(std::floor(
                             (std::min)({ay, by, cy}))),
                         0, height - 1);
          const int max_y =
              std::clamp(static_cast<int>(std::ceil(
                             (std::max)({ay, by, cy}))) -
                             1,
                         0, height - 1);
          for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
              const double px = static_cast<double>(x) + 0.5;
              const double py = static_cast<double>(y) + 0.5;
              const double e0 = edge(ax, ay, bx, by, px, py);
              const double e1 = edge(bx, by, cx, cy, px, py);
              const double e2 = edge(cx, cy, ax, ay, px, py);
              const bool inside =
                  area > 0.0
                      ? e0 >= -kInsideEpsilon &&
                            e1 >= -kInsideEpsilon &&
                            e2 >= -kInsideEpsilon
                      : e0 <= kInsideEpsilon &&
                            e1 <= kInsideEpsilon &&
                            e2 <= kInsideEpsilon;
              if (inside) {
                const auto texel =
                    static_cast<std::size_t>(y) * width_size +
                    static_cast<std::size_t>(x);
                if (marked[texel] == 0u) {
                  marked[texel] = 1u;
                  touched.push_back(static_cast<std::uint32_t>(texel));
                }
              }
            }
          }
        }
      }

      std::sort(touched.begin(), touched.end());
      std::size_t run_count = 0u;
      std::uint32_t previous = 0u;
      for (std::size_t i = 0; i < touched.size(); ++i) {
        const std::uint32_t texel = touched[i];
        if (i == 0u || texel / static_cast<std::uint32_t>(width) !=
                           previous / static_cast<std::uint32_t>(width) ||
            texel != previous + 1u) {
          ++run_count;
        }
        previous = texel;
      }
      std::vector<UvRun> runs;
      runs.reserve(run_count);
      for (const std::uint32_t texel : touched) {
        const std::uint32_t y =
            texel / static_cast<std::uint32_t>(width);
        const std::uint32_t x =
            texel % static_cast<std::uint32_t>(width);
        if (!runs.empty() && runs.back().y == y &&
            x == runs.back().x1 + 1u) {
          runs.back().x1 = x;
        } else {
          runs.push_back({y, x, x});
        }
      }
      for (const std::uint32_t texel : touched) {
        marked[texel] = 0u;
      }
      result.group_runs.emplace(group_name, std::move(runs));
    }

    out = std::move(result);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::bad_alloc &) {
    return fail_coverage("UV coverage allocation exceeded the memory budget");
  } catch (const std::length_error &) {
    return fail_coverage("UV coverage allocation size is invalid");
  }
}

LabPbrUvCoverage
rasterizeLabPbrUvCoverage(const StaticIndexedModelMesh &mesh, int width,
                          int height) {
  LabPbrUvCoverage result;
  rasterizeLabPbrUvCoverage(mesh, width, height, result, nullptr);
  return result;
}

bool materializeLabPbrSpecular(int width, int height,
                               const TextureImage *imported_specular,
                               TextureImage &out, std::string *error) {
  const auto fail = [&](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (width <= 0 || height <= 0) {
    return fail("specular atlas dimensions must be positive");
  }
  const auto width_size = static_cast<std::size_t>(width);
  const auto height_size = static_cast<std::size_t>(height);
  if (width_size > (std::numeric_limits<std::size_t>::max)() / height_size) {
    return fail("specular atlas dimensions overflow");
  }
  const std::size_t texel_count = width_size * height_size;
  if (texel_count >
          (std::numeric_limits<std::size_t>::max)() / 4u ||
      texel_count > static_cast<std::size_t>(
                        (std::numeric_limits<std::uint32_t>::max)())) {
    return fail("specular atlas dimensions overflow");
  }
  if (imported_specular != nullptr && imported_specular->valid() &&
      (imported_specular->width != width ||
       imported_specular->height != height)) {
    return fail("imported specular dimensions do not match atlas");
  }
  try {
    TextureImage candidate;
    candidate.width = width;
    candidate.height = height;
    candidate.source_channels = 4;
    if (imported_specular != nullptr && imported_specular->valid()) {
      candidate.rgba = imported_specular->rgba;
    } else {
      candidate.rgba.resize(texel_count * 4u);
      for (std::size_t texel = 0; texel < texel_count; ++texel) {
        candidate.rgba[texel * 4u + 0u] = 0u;
        candidate.rgba[texel * 4u + 1u] = 10u;
        candidate.rgba[texel * 4u + 2u] = 0u;
        candidate.rgba[texel * 4u + 3u] = 0u;
      }
    }
    out = std::move(candidate);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::bad_alloc &) {
    return fail("specular atlas allocation exceeded the memory budget");
  } catch (const std::length_error &) {
    return fail("specular atlas allocation size is invalid");
  }
}

LabPbrCompositionResult composeLabPbrSpecular(
    int width, int height, const TextureImage *imported_specular,
    const LabPbrUvCoverage &coverage,
    const std::map<std::string, GroupLabPbrOverride> &overrides) {
  LabPbrCompositionResult result;
  if (width <= 0 || height <= 0) {
    result.errors.emplace_back("invalid specular atlas dimensions");
    return result;
  }
  const std::size_t texel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (texel_count >
          (std::numeric_limits<std::size_t>::max)() / 4u ||
      texel_count >
          static_cast<std::size_t>(
              (std::numeric_limits<std::uint32_t>::max)())) {
    result.errors.emplace_back("specular atlas dimensions overflow");
    return result;
  }
  if (imported_specular != nullptr && imported_specular->valid()) {
    if (imported_specular->width != width ||
        imported_specular->height != height) {
      result.errors.emplace_back(
          "imported specular dimensions do not match atlas");
      return result;
    }
  }
  if (overrides.empty()) {
    result.specular_materialization_deferred = true;
    result.deferred_width = width;
    result.deferred_height = height;
    return result;
  }
  if (!coverage.valid() || coverage.width != width ||
      coverage.height != height) {
    result.errors.emplace_back("invalid or mismatched UV coverage dimensions");
    return result;
  }
  std::string materialize_error;
  if (!materializeLabPbrSpecular(width, height, imported_specular,
                                 result.specular, &materialize_error)) {
    result.errors.push_back(std::move(materialize_error));
    return result;
  }

  std::map<std::uint64_t, ChannelClaim> claims;
  for (const auto &[group_name, override_value] : overrides) {
    std::string validation_error;
    if (!validGroupLabPbrOverride(override_value, &validation_error)) {
      result.errors.push_back(group_name + ": " + validation_error);
      continue;
    }
    const auto *runs = coverage.find(group_name);
    if (runs == nullptr || runs->empty()) {
      result.warnings.push_back(group_name +
                                ": no textured UV texels are covered");
      continue;
    }
    for (const UvRun &run : *runs) {
      for (std::uint32_t x = run.x0;; ++x) {
        const auto texel64 = static_cast<std::uint64_t>(run.y) *
                                 static_cast<std::uint64_t>(width) +
                             x;
        if (texel64 >= texel_count) {
          result.errors.push_back(group_name +
                                  ": UV coverage contains invalid texel");
          break;
        }
        const auto texel = static_cast<std::uint32_t>(texel64);
        if (override_value.roughness_enabled) {
          claimChannel(texel, LabPbrOverrideChannel::Roughness,
                       encodeLabPbrRoughness(override_value.roughness),
                       group_name, result.specular, claims);
        }
        if (override_value.metal_enabled) {
          claimChannel(texel, LabPbrOverrideChannel::Metal,
                       override_value.metal ? override_value.metal_code
                                            : override_value.dielectric_f0,
                       group_name, result.specular, claims);
        }
        if (override_value.porosity_enabled) {
          claimChannel(texel, LabPbrOverrideChannel::Porosity,
                       override_value.subsurface_scattering
                           ? encodeLabPbrSubsurface(
                                 override_value.subsurface)
                           : encodeLabPbrPorosity(override_value.porosity),
                       group_name, result.specular, claims);
        }
        if (override_value.emission_enabled) {
          claimChannel(texel, LabPbrOverrideChannel::Emission,
                       encodeLabPbrEmission(override_value.emission),
                       group_name, result.specular, claims);
        }
        if (x == run.x1) {
          break;
        }
      }
    }
  }

  for (const auto &[key, claim] : claims) {
    if (claim.values.size() <= 1u) {
      continue;
    }
    LabPbrUvConflict conflict;
    conflict.texel_index = static_cast<std::uint32_t>(key / 4u);
    conflict.channel =
        static_cast<LabPbrOverrideChannel>(key % 4u);
    conflict.groups.assign(claim.groups.begin(), claim.groups.end());
    conflict.encoded_values.assign(claim.values.begin(), claim.values.end());
    result.conflicts.push_back(std::move(conflict));
  }
  return result;
}

bool buildAuthoredResolvedMaterial(
    const TextureImage &base, const ResolvedMaterialTable &source,
    const TextureImage *authored_normal,
    const TextureImage *authored_specular, ResolvedMaterialTable &out,
    std::string *error, std::uint64_t maximum_peak_bytes) {
  if (!base.valid() ||
      (authored_specular != nullptr &&
       (!authored_specular->valid() ||
        authored_specular->width != base.width ||
        authored_specular->height != base.height ||
        authored_specular->source_channels != 4))) {
    if (error != nullptr) {
      *error = "authored specular image does not match the base atlas";
    }
    return false;
  }
  const TextureImage *normal = authored_normal;
  if (normal == nullptr && source.normal_map_active) {
    normal = &source.normal_image;
  }
  const TextureImage *specular = authored_specular;
  if (specular == nullptr && source.specular_map_active) {
    specular = &source.specular_image;
  }
  if (normal != nullptr &&
      (!normal->valid() || normal->width != base.width ||
       normal->height != base.height || normal->source_channels < 3)) {
    if (error != nullptr) {
      *error = "normal image does not match the base atlas or lacks RGB";
    }
    return false;
  }

  LabPbrMemoryEstimateRequest memory_request;
  memory_request.width = static_cast<std::uint64_t>(base.width);
  memory_request.height = static_cast<std::uint64_t>(base.height);
  memory_request.resident_rgba_image_count =
      1u + (normal != nullptr ? 1u : 0u) +
      (specular != nullptr ? 1u : 0u);
  memory_request.candidate_rgba_image_count =
      memory_request.resident_rgba_image_count;
  memory_request.resolved_texel_bytes_per_pixel =
      kLabPbrResolvedTexelBytesPerPixel;
  const TextureImage *old_images[] = {
      &out.base_image, &out.normal_image, &out.specular_image};
  for (const TextureImage *old_image : old_images) {
    if (old_image->rgba.capacity() >
        (std::numeric_limits<std::uint64_t>::max)() -
            memory_request.resident_fixed_bytes) {
      if (error != nullptr) {
        *error = "LabPBR budget preflight failed: resident byte overflow";
      }
      return false;
    }
    memory_request.resident_fixed_bytes +=
        static_cast<std::uint64_t>(old_image->rgba.capacity());
  }
  LabPbrMemoryEstimate memory_estimate;
  if (!preflightLabPbrMemory(memory_request, maximum_peak_bytes,
                             memory_estimate, error)) {
    return false;
  }

  try {
    ResolvedMaterialTable material = source;
    material.width = base.width;
    material.height = base.height;
    material.base_image = base;
    if (authored_normal != nullptr || authored_specular != nullptr) {
      material.format = LabPbrFormat::LabPbr13;
    }
    material.normal_map_active = normal != nullptr;
    material.normal_image = normal == nullptr ? TextureImage{} : *normal;
    material.specular_map_active = specular != nullptr;
    material.specular_image =
        specular == nullptr ? TextureImage{} : *specular;
    out = std::move(material);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::bad_alloc &) {
    if (error != nullptr) {
      *error = "LabPBR budget stage failed while allocating compact material images";
    }
    return false;
  } catch (const std::length_error &exception) {
    if (error != nullptr) {
      *error = std::string("LabPBR budget stage failed: ") + exception.what();
    }
    return false;
  }
}

std::string sha256Hex(std::span<const std::uint8_t> bytes) {
  const auto digest = sha256(bytes);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2u);
  for (const std::uint8_t byte : digest) {
    result.push_back(kHex[byte >> 4u]);
    result.push_back(kHex[byte & 0x0fu]);
  }
  return result;
}

bool importReadOnlyIrisNormal(const std::filesystem::path &path,
                              int expected_width, int expected_height,
                              ReadOnlyIrisNormalAsset &out,
                              std::string *error,
                              TextureDecodeLimits limits) {
  try {
    const auto add_retained = [&](std::size_t bytes) {
      if (bytes > (std::numeric_limits<std::size_t>::max)() -
                      limits.retained_resident_bytes) {
        return false;
      }
      limits.retained_resident_bytes += bytes;
      return true;
    };
    if (!add_retained(out.original_file_bytes.capacity()) ||
        !add_retained(out.decoded.rgba.capacity())) {
      if (error != nullptr) {
        *error =
            "Iris Normal budget stage failed before snapshot: retained byte overflow";
      }
      return false;
    }
    const std::size_t effective_peak =
        (std::min)(limits.maximum_peak_bytes,
                   kTextureDecodeMaximumPeakBytes);
    if (limits.retained_resident_bytes > effective_peak) {
      if (error != nullptr) {
        *error =
            "Iris Normal budget stage failed before snapshot: retained state exceeds the peak limit";
      }
      return false;
    }

    FileByteSnapshot snapshot;
    if (!snapshotFileBytes(
            path, snapshot, error, "Iris Normal",
            static_cast<std::uintmax_t>(
                effective_peak - limits.retained_resident_bytes))) {
      return false;
    }

    TextureImage decoded;
    std::string decode_error;
    if (!loadTextureImageFromMemory(
            snapshot.bytes->data(), static_cast<int>(snapshot.bytes->size()),
            decoded, &decode_error, limits)) {
      if (error != nullptr) {
        *error = "Iris Normal Decode stage failed: " +
                 (decode_error.empty() ? std::string("invalid image")
                                       : decode_error);
      }
      return false;
    }
    if (decoded.source_channels != 4) {
      if (error != nullptr) {
        *error = "Iris Normal Decode stage failed: image must be RGBA";
      }
      return false;
    }
    if (decoded.width != expected_width ||
        decoded.height != expected_height) {
      if (error != nullptr) {
        *error = "Iris Normal Domain stage failed: dimensions do not match "
                 "the base atlas";
      }
      return false;
    }

    std::size_t copy_peak = limits.retained_resident_bytes;
    const auto add_copy_peak = [&copy_peak](std::size_t bytes) {
      if (bytes >
          (std::numeric_limits<std::size_t>::max)() - copy_peak) {
        return false;
      }
      copy_peak += bytes;
      return true;
    };
    if (!add_copy_peak(snapshot.bytes->capacity()) ||
        !add_copy_peak(decoded.rgba.capacity()) ||
        !add_copy_peak(snapshot.bytes->size()) ||
        copy_peak > effective_peak) {
      if (error != nullptr) {
        *error =
            "Iris Normal budget stage failed before encoded-byte copy: peak limit exceeded";
      }
      return false;
    }

    ReadOnlyIrisNormalAsset imported;
    imported.source_path = snapshot.path;
    imported.original_file_bytes.assign(snapshot.bytes->begin(),
                                        snapshot.bytes->end());
    imported.sha256 = sha256Hex(imported.original_file_bytes);
    imported.decoded = std::move(decoded);
    imported.decoded.path = pathUtf8String(snapshot.path);
    out = std::move(imported);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::bad_alloc &) {
    if (error != nullptr) {
      *error = "Iris Normal budget stage failed: std::bad_alloc";
    }
    return false;
  } catch (const std::length_error &exception) {
    if (error != nullptr) {
      *error = std::string("Iris Normal budget stage failed: ") +
               exception.what();
    }
    return false;
  }
}

bool encodePngRgba8(int width, int height,
                    std::span<const std::uint8_t> rgba,
                    std::vector<std::uint8_t> &png, std::string *error) {
  if (width <= 0 || height <= 0) {
    if (error != nullptr) {
      *error = "PNG dimensions must be positive";
    }
    return false;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
  const std::size_t expected = row_bytes * static_cast<std::size_t>(height);
  if (row_bytes / 4u != static_cast<std::size_t>(width) ||
      expected / static_cast<std::size_t>(height) != row_bytes ||
      rgba.size() != expected) {
    if (error != nullptr) {
      *error = "PNG RGBA byte count does not match dimensions";
    }
    return false;
  }
  const std::size_t height_size = static_cast<std::size_t>(height);
  if (expected >
      (std::numeric_limits<std::size_t>::max)() - height_size) {
    if (error != nullptr) {
      *error = "PNG scanline dimensions overflow";
    }
    return false;
  }
  const std::size_t scanline_size = expected + height_size;
  const std::size_t block_count =
      scanline_size / 65535u + (scanline_size % 65535u != 0u ? 1u : 0u);
  const std::size_t maximum_size =
      (std::numeric_limits<std::size_t>::max)();
  if (scanline_size > maximum_size - 6u ||
      block_count > (maximum_size - scanline_size - 6u) / 5u ||
      scanline_size + block_count * 5u + 6u >
          static_cast<std::size_t>(
              (std::numeric_limits<std::uint32_t>::max)())) {
    if (error != nullptr) {
      *error = "PNG IDAT payload exceeds the 32-bit chunk limit";
    }
    return false;
  }

  std::vector<std::uint8_t> scanlines;
  scanlines.reserve(scanline_size);
  for (int y = 0; y < height; ++y) {
    scanlines.push_back(0u);
    const auto row = rgba.subspan(static_cast<std::size_t>(y) * row_bytes,
                                  row_bytes);
    scanlines.insert(scanlines.end(), row.begin(), row.end());
  }

  std::vector<std::uint8_t> zlib;
  zlib.reserve(scanline_size + block_count * 5u + 6u);
  zlib.push_back(0x78u);
  zlib.push_back(0x01u);
  std::size_t offset = 0;
  while (offset < scanlines.size()) {
    const std::size_t block_size =
        (std::min)(std::size_t{65535u}, scanlines.size() - offset);
    const bool final = offset + block_size == scanlines.size();
    zlib.push_back(final ? 0x01u : 0x00u);
    const auto length = static_cast<std::uint16_t>(block_size);
    const auto inverse = static_cast<std::uint16_t>(~length);
    zlib.push_back(static_cast<std::uint8_t>(length));
    zlib.push_back(static_cast<std::uint8_t>(length >> 8u));
    zlib.push_back(static_cast<std::uint8_t>(inverse));
    zlib.push_back(static_cast<std::uint8_t>(inverse >> 8u));
    zlib.insert(zlib.end(), scanlines.begin() +
                                static_cast<std::ptrdiff_t>(offset),
                scanlines.begin() +
                    static_cast<std::ptrdiff_t>(offset + block_size));
    offset += block_size;
  }
  appendBigEndian32(zlib, adler32(scanlines));

  png.clear();
  static constexpr std::array<std::uint8_t, 8> kSignature{
      0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au};
  png.insert(png.end(), kSignature.begin(), kSignature.end());
  std::array<std::uint8_t, 13> ihdr{};
  ihdr[0] = static_cast<std::uint8_t>(
      static_cast<std::uint32_t>(width) >> 24u);
  ihdr[1] = static_cast<std::uint8_t>(
      static_cast<std::uint32_t>(width) >> 16u);
  ihdr[2] =
      static_cast<std::uint8_t>(static_cast<std::uint32_t>(width) >> 8u);
  ihdr[3] = static_cast<std::uint8_t>(width);
  ihdr[4] = static_cast<std::uint8_t>(
      static_cast<std::uint32_t>(height) >> 24u);
  ihdr[5] = static_cast<std::uint8_t>(
      static_cast<std::uint32_t>(height) >> 16u);
  ihdr[6] =
      static_cast<std::uint8_t>(static_cast<std::uint32_t>(height) >> 8u);
  ihdr[7] = static_cast<std::uint8_t>(height);
  ihdr[8] = 8u;
  ihdr[9] = 6u;
  appendPngChunk(png, "IHDR", ihdr);
  appendPngChunk(png, "IDAT", zlib);
  appendPngChunk(png, "IEND", {});
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace xpbd::gfx
