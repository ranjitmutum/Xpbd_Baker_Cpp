#include "xpbd/gfx/preview_scene.hpp"
#include "xpbd/gfx/world_environment.hpp"
#include "preview_surfaces.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace xpbd::gfx {
namespace {

struct Rgba {
  float r, g, b, a;
};

struct Rgb {
  float r, g, b;
};

void pushTri(std::vector<MeshVertex> &dst, float ax, float ay, float az,
             float bx, float by, float bz, float cx, float cy, float cz,
             float nx, float ny, float nz, Rgba c) {
  MeshVertex v{};
  v.nx = nx;
  v.ny = ny;
  v.nz = nz;
  v.r = c.r;
  v.g = c.g;
  v.b = c.b;
  v.a = c.a;
  v.px = ax;
  v.py = ay;
  v.pz = az;
  dst.push_back(v);
  v.px = bx;
  v.py = by;
  v.pz = bz;
  dst.push_back(v);
  v.px = cx;
  v.py = cy;
  v.pz = cz;
  dst.push_back(v);
}

void pushQuad(std::vector<MeshVertex> &dst, const float p[4][3], float nx,
              float ny, float nz, Rgba c) {
  pushTri(dst, p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0],
          p[2][1], p[2][2], nx, ny, nz, c);
  pushTri(dst, p[0][0], p[0][1], p[0][2], p[2][0], p[2][1], p[2][2], p[3][0],
          p[3][1], p[3][2], nx, ny, nz, c);
}

void pushLine(std::vector<MeshVertex> &dst, float x0, float y0, float z0,
              float x1, float y1, float z1, Rgba c) {
  MeshVertex a{};
  a.px = x0;
  a.py = y0;
  a.pz = z0;
  a.nx = a.ny = a.nz = 0.0f;
  a.r = c.r;
  a.g = c.g;
  a.b = c.b;
  a.a = c.a;
  MeshVertex b = a;
  b.px = x1;
  b.py = y1;
  b.pz = z1;
  dst.push_back(a);
  dst.push_back(b);
}

// --- Grid ------------------------------------------------------------------

void appendBlockbenchGrid(ViewportGpuScene &out, bool show_axes,
                          float half_extent, float minor_step, float major_every,
                          float y) {
  const Rgba minor{0.40f, 0.42f, 0.48f, 0.85f};
  const Rgba major{0.58f, 0.60f, 0.66f, 1.0f};
  const Rgba axis_x{0.92f, 0.28f, 0.28f, 1.0f};
  const Rgba axis_z{0.30f, 0.45f, 0.95f, 1.0f};
  const int n =
      std::max(1, static_cast<int>(std::ceil(half_extent / minor_step)));
  const int major_stride =
      std::max(1, static_cast<int>(std::lround(major_every / minor_step)));
  for (int i = -n; i <= n; ++i) {
    const float t = static_cast<float>(i) * minor_step;
    if (std::abs(t) > half_extent + 1e-4f) {
      continue;
    }
    const bool is_major = (i % major_stride) == 0;
    if (std::abs(t) < 1e-4f && show_axes) {
      continue;
    }
    const Rgba &c = is_major ? major : minor;
    pushLine(out.lines, t, y, -half_extent, t, y, half_extent, c);
    pushLine(out.lines, -half_extent, y, t, half_extent, y, t, c);
    out.line_segment_count += 2;
  }
  if (show_axes) {
    const float axis_len = half_extent;
    pushLine(out.lines, 0, y + 0.001f, 0, axis_len, y + 0.001f, 0, axis_x);
    pushLine(out.lines, 0, y + 0.001f, 0, 0, y + 0.001f, axis_len, axis_z);
    const Rgba axis_x_neg{0.55f, 0.18f, 0.18f, 0.75f};
    const Rgba axis_z_neg{0.18f, 0.28f, 0.55f, 0.75f};
    pushLine(out.lines, 0, y + 0.001f, 0, -axis_len, y + 0.001f, 0, axis_x_neg);
    pushLine(out.lines, 0, y + 0.001f, 0, 0, y + 0.001f, -axis_len, axis_z_neg);
    out.line_segment_count += 4;
    const float mark = std::clamp(half_extent * 0.08f, 0.8f, 2.4f);
    const float nz = -half_extent - mark * 0.2f;
    const Rgba north{0.70f, 0.72f, 0.78f, 1.0f};
    pushLine(out.lines, -mark * 0.5f, y + 0.002f, nz, 0, y + 0.002f, nz - mark,
             north);
    pushLine(out.lines, 0, y + 0.002f, nz - mark, mark * 0.5f, y + 0.002f, nz,
             north);
    pushLine(out.lines, mark * 0.5f, y + 0.002f, nz, -mark * 0.5f, y + 0.002f,
             nz, north);
    out.line_segment_count += 3;
  }
}

// --- Noise -----------------------------------------------------------------

float hash31i(int x, int y, int z) {
  std::uint32_t n = static_cast<std::uint32_t>(x) * 73856093u ^
                    static_cast<std::uint32_t>(y) * 19349663u ^
                    static_cast<std::uint32_t>(z) * 83492791u;
  n ^= n >> 13;
  n *= 1274126177u;
  n ^= n >> 16;
  return static_cast<float>(n & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

float hash21(float x, float y) {
  return hash31i(static_cast<int>(std::floor(x)),
                 static_cast<int>(std::floor(y)), 17);
}

float smoothNoise2(float x, float y) {
  const float ix = std::floor(x);
  const float iy = std::floor(y);
  const float fx = x - ix;
  const float fy = y - iy;
  const float ux = fx * fx * (3.0f - 2.0f * fx);
  const float uy = fy * fy * (3.0f - 2.0f * fy);
  const float a = hash21(ix, iy);
  const float b = hash21(ix + 1.0f, iy);
  const float c = hash21(ix, iy + 1.0f);
  const float d = hash21(ix + 1.0f, iy + 1.0f);
  return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

float smoothNoise3(float x, float y, float z) {
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const int z0 = static_cast<int>(std::floor(z));
  const float fx = x - static_cast<float>(x0);
  const float fy = y - static_cast<float>(y0);
  const float fz = z - static_cast<float>(z0);
  const float ux = fx * fx * (3.0f - 2.0f * fx);
  const float uy = fy * fy * (3.0f - 2.0f * fy);
  const float uz = fz * fz * (3.0f - 2.0f * fz);
  float n = 0.0f;
  for (int dz = 0; dz <= 1; ++dz) {
    for (int dy = 0; dy <= 1; ++dy) {
      for (int dx = 0; dx <= 1; ++dx) {
        const float w = (dx ? ux : 1.0f - ux) * (dy ? uy : 1.0f - uy) *
                        (dz ? uz : 1.0f - uz);
        n += w * hash31i(x0 + dx, y0 + dy, z0 + dz);
      }
    }
  }
  return n;
}

float fbm2(float x, float y, int octaves = 5) {
  float v = 0.0f, a = 0.5f, f = 1.0f, norm = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    v += a * smoothNoise2(x * f, y * f);
    norm += a;
    a *= 0.5f;
    f *= 2.03f;
  }
  return v / std::max(norm, 1e-5f);
}

float fbm3(float x, float y, float z, int octaves = 5) {
  float v = 0.0f, a = 0.5f, f = 1.0f, norm = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    v += a * smoothNoise3(x * f, y * f, z * f);
    norm += a;
    a *= 0.5f;
    f *= 2.02f;
  }
  return v / std::max(norm, 1e-5f);
}

Rgb lerp(Rgb a, Rgb b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

void faceUvToDir(int face, float u, float v, float &dx, float &dy, float &dz) {
  const float uc = 2.0f * u - 1.0f;
  const float vc = 2.0f * v - 1.0f;
  switch (face) {
  case 0:
    dx = 1;
    dy = -vc;
    dz = -uc;
    break;
  case 1:
    dx = -1;
    dy = -vc;
    dz = uc;
    break;
  case 2:
    dx = uc;
    dy = 1;
    dz = vc;
    break;
  case 3:
    dx = uc;
    dy = -1;
    dz = -vc;
    break;
  case 4:
    dx = uc;
    dy = -vc;
    dz = 1;
    break;
  default:
    dx = -uc;
    dy = -vc;
    dz = -1;
    break;
  }
  const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (len > 1e-6f) {
    dx /= len;
    dy /= len;
    dz /= len;
  }
}

// Smooth heightfield: per-vertex normals + alternating diagonals (no flat quads).
// transparent=true writes into environment.transparent (water blend path).
void appendHeightfield(ViewportGpuScene &out, float half, int segs,
                       const std::vector<float> &hmap,
                       const std::vector<Rgba> &cmap,
                       bool transparent = false) {
  const float step = (2.0f * half) / static_cast<float>(segs);
  const int n = segs + 1;
  auto atH = [&](int ix, int iz) -> float {
    ix = std::clamp(ix, 0, segs);
    iz = std::clamp(iz, 0, segs);
    return hmap[static_cast<std::size_t>(iz * n + ix)];
  };
  auto atC = [&](int ix, int iz) -> Rgba {
    return cmap[static_cast<std::size_t>(iz * n + ix)];
  };
  // Per-vertex smooth normals from central differences.
  std::vector<float> nxm(static_cast<std::size_t>(n * n));
  std::vector<float> nym(static_cast<std::size_t>(n * n));
  std::vector<float> nzm(static_cast<std::size_t>(n * n));
  for (int iz = 0; iz <= segs; ++iz) {
    for (int ix = 0; ix <= segs; ++ix) {
      const float hx = (atH(ix + 1, iz) - atH(ix - 1, iz)) / (2.0f * step);
      const float hz = (atH(ix, iz + 1) - atH(ix, iz - 1)) / (2.0f * step);
      float nx = -hx, ny = 1.0f, nz = -hz;
      const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      nx /= len;
      ny /= len;
      nz /= len;
      const std::size_t i = static_cast<std::size_t>(iz * n + ix);
      nxm[i] = nx;
      nym[i] = ny;
      nzm[i] = nz;
    }
  }
  auto atN = [&](int ix, int iz, float &nx, float &ny, float &nz) {
    const std::size_t i = static_cast<std::size_t>(iz * n + ix);
    nx = nxm[i];
    ny = nym[i];
    nz = nzm[i];
  };
  std::vector<MeshVertex> &dst = transparent ? out.transparent : out.solid;
  dst.reserve(dst.size() +
              static_cast<std::size_t>(segs) * static_cast<std::size_t>(segs) *
                  6u);
  auto pushV = [&](int ix, int iz) {
    MeshVertex v{};
    v.px = -half + static_cast<float>(ix) * step;
    v.py = atH(ix, iz);
    v.pz = -half + static_cast<float>(iz) * step;
    atN(ix, iz, v.nx, v.ny, v.nz);
    const Rgba c = atC(ix, iz);
    v.r = c.r;
    v.g = c.g;
    v.b = c.b;
    v.a = c.a;
    dst.push_back(v);
  };
  for (int iz = 0; iz < segs; ++iz) {
    for (int ix = 0; ix < segs; ++ix) {
      // Alternate diagonal → breaks quad striping / faceting.
      if (((ix + iz) & 1) == 0) {
        pushV(ix, iz);
        pushV(ix + 1, iz);
        pushV(ix + 1, iz + 1);
        pushV(ix, iz);
        pushV(ix + 1, iz + 1);
        pushV(ix, iz + 1);
      } else {
        pushV(ix, iz);
        pushV(ix + 1, iz);
        pushV(ix, iz + 1);
        pushV(ix + 1, iz);
        pushV(ix + 1, iz + 1);
        pushV(ix, iz + 1);
      }
    }
  }
}

// --- Sky palette / sampling ------------------------------------------------

struct SkyPalette {
  Rgb zenith;
  Rgb horizon;
  Rgb nadir; // continuous lower hemisphere (not a hard "ground lid")
  Rgb sun_tint{1.0f, 0.95f, 0.85f};
  Rgb nebula_a{0.35f, 0.15f, 0.55f};
  Rgb nebula_b{0.15f, 0.35f, 0.70f};
  float sun_x = 0.35f, sun_y = 0.55f, sun_z = 0.40f;
  float sun_size = 0.04f;
  float sun_bloom = 0.18f;
  float cloud = 0.35f;
  float storm_layers = 0.0f; // multi-layer storm strength
  float star_dense = 0.0f;
  float star_bright = 0.0f;
  float star_scale = 280.0f;
  float milky = 0.0f;
  float nebula = 0.0f;
  bool full_sphere = false;
  bool no_sun = false;
};

void normalizeSun(SkyPalette &p) {
  const float sl =
      std::sqrt(p.sun_x * p.sun_x + p.sun_y * p.sun_y + p.sun_z * p.sun_z);
  if (sl > 1e-5f) {
    p.sun_x /= sl;
    p.sun_y /= sl;
    p.sun_z /= sl;
  }
}

SkyPalette paletteFor(PreviewSceneId id) {
  id = canonicalPreviewSceneId(id);
  SkyPalette p;
  switch (id) {
  case PreviewSceneId::Night:
    p.zenith = {0.015f, 0.025f, 0.08f};
    p.horizon = {0.05f, 0.07f, 0.16f};
    p.nadir = {0.04f, 0.055f, 0.12f}; // soft, near horizon
    p.sun_tint = {0.85f, 0.88f, 1.0f};
    p.sun_x = -0.35f;
    p.sun_y = 0.62f;
    p.sun_z = 0.35f;
    p.sun_size = 0.026f;
    p.sun_bloom = 0.10f;
    p.cloud = 0.55f;
    p.star_dense = 0.12f;
    p.star_bright = 0.03f;
    p.star_scale = 260.0f;
    p.milky = 0.22f;
    break;
  case PreviewSceneId::Sunset:
    p.zenith = {0.10f, 0.16f, 0.42f};
    p.horizon = {0.98f, 0.42f, 0.16f};
    p.nadir = {0.72f, 0.38f, 0.22f};
    p.sun_tint = {1.0f, 0.72f, 0.32f};
    p.sun_x = 0.78f;
    p.sun_y = 0.10f;
    p.sun_z = 0.32f;
    p.sun_size = 0.055f;
    p.sun_bloom = 0.28f;
    p.cloud = 0.78f;
    break;
  case PreviewSceneId::Desert:
    p.zenith = {0.28f, 0.58f, 0.94f};
    p.horizon = {0.94f, 0.78f, 0.48f};
    p.nadir = {0.72f, 0.58f, 0.36f};
    p.sun_tint = {1.0f, 0.94f, 0.72f};
    p.sun_x = 0.25f;
    p.sun_y = 0.78f;
    p.sun_z = 0.35f;
    p.sun_size = 0.052f;
    p.sun_bloom = 0.24f;
    p.cloud = 0.06f;
    break;
  case PreviewSceneId::Ocean:
    // Bright maritime sky: strong zenith blue, soft cyan horizon for water blend.
    p.zenith = {0.12f, 0.38f, 0.88f};
    p.horizon = {0.55f, 0.78f, 0.92f};
    p.nadir = {0.14f, 0.36f, 0.52f};
    p.sun_tint = {1.0f, 0.98f, 0.90f};
    p.sun_x = 0.42f;
    p.sun_y = 0.68f;
    p.sun_z = 0.22f;
    p.sun_size = 0.042f;
    p.sun_bloom = 0.28f; // wider glitter-friendly bloom over water
    p.cloud = 0.36f;
    break;
  case PreviewSceneId::Overcast:
    // Soft continuous grey shell — zenith ≈ nadir to kill polar seams.
    p.zenith = {0.55f, 0.57f, 0.60f};
    p.horizon = {0.64f, 0.66f, 0.68f};
    p.nadir = {0.52f, 0.54f, 0.57f};
    p.no_sun = true;
    p.sun_size = 0.0f;
    p.sun_bloom = 0.06f;
    p.cloud = 0.90f;
    p.full_sphere = true;
    break;
  case PreviewSceneId::Sky:
  default:
    p.zenith = {0.26f, 0.50f, 0.92f};
    p.horizon = {0.70f, 0.84f, 0.95f};
    p.nadir = {0.48f, 0.58f, 0.62f};
    p.sun_tint = {1.0f, 0.97f, 0.88f};
    p.sun_x = 0.35f;
    p.sun_y = 0.70f;
    p.sun_z = 0.40f;
    p.sun_size = 0.045f;
    p.sun_bloom = 0.20f;
    p.cloud = 0.72f;
    break;
  }
  if (!p.no_sun) {
    normalizeSun(p);
  }
  return p;
}

Rgb sampleStars(float dx, float dy, float dz, float density, float scale,
                float intensity, float min_elev) {
  Rgb add{0, 0, 0};
  if (density <= 0.0f || intensity <= 0.0f) {
    return add;
  }
  const float elev_gate =
      min_elev < -1.0f ? 1.0f
                       : std::clamp((dy - min_elev) * 2.0f, 0.0f, 1.0f);
  if (elev_gate <= 0.0f) {
    return add;
  }
  const float px = dx * scale;
  const float py = dy * scale;
  const float pz = dz * scale;
  const int ix = static_cast<int>(std::floor(px));
  const int iy = static_cast<int>(std::floor(py));
  const int iz = static_cast<int>(std::floor(pz));
  const float fx = px - static_cast<float>(ix);
  const float fy = py - static_cast<float>(iy);
  const float fz = pz - static_cast<float>(iz);
  for (int oz = -1; oz <= 1; ++oz) {
    for (int oy = -1; oy <= 1; ++oy) {
      for (int ox = -1; ox <= 1; ++ox) {
        const int cx = ix + ox, cy = iy + oy, cz = iz + oz;
        const float roll = hash31i(cx, cy, cz);
        if (roll > density) {
          continue;
        }
        const float jx = hash31i(cx, cy + 19, cz);
        const float jy = hash31i(cx + 7, cy, cz + 3);
        const float jz = hash31i(cx + 3, cy + 11, cz);
        const float ddx = fx - (static_cast<float>(ox) + jx);
        const float ddy = fy - (static_cast<float>(oy) + jy);
        const float ddz = fz - (static_cast<float>(oz) + jz);
        const float dist2 = ddx * ddx + ddy * ddy + ddz * ddz;
        const float size = 0.018f + 0.050f * hash31i(cx + 1, cy + 2, cz + 3);
        const float core = std::exp(-dist2 / (size * size));
        const float halo = std::exp(-dist2 / (size * size * 9.0f)) * 0.22f;
        if (core + halo < 1e-4f) {
          continue;
        }
        const float mag = 0.55f + 0.90f * hash31i(cx + 5, cy, cz + 9);
        const float cool = hash31i(cx, cy + 4, cz + 2);
        const float b = (core + halo) * mag * intensity * elev_gate;
        add.r += b * (0.88f + 0.12f * cool);
        add.g += b * (0.90f + 0.08f * cool);
        add.b += b * (0.95f + 0.10f * (1.0f - cool * 0.5f));
      }
    }
  }
  return add;
}

// Soft wide horizon — matches GPU atmosphere (no hard night/sunset line).
Rgb atmosphereBase(const SkyPalette &p, float elev) {
  const float e = std::clamp(elev, -1.0f, 1.0f);
  if (p.full_sphere) {
    if (e >= 0.0f) {
      const float t = e * e * (3.0f - 2.0f * e);
      return lerp(p.horizon, p.zenith, std::pow(t, 0.9f));
    }
    const float u = -e;
    const float t = u * u * (3.0f - 2.0f * u);
    return lerp(p.horizon, p.nadir,
                std::pow(std::clamp(t, 0.0f, 1.0f), 1.1f) * 0.75f);
  }
  if (e >= 0.0f) {
    const float t = std::pow(e, 0.9f);
    const float band = std::exp(-e * e * 18.0f);
    Rgb upper = lerp(p.horizon, p.zenith, t);
    return lerp(upper, p.horizon, band * 0.22f);
  }
  const float down = -e;
  float t = down * down * (3.0f - 2.0f * down);
  t = std::pow(std::clamp(t, 0.0f, 1.0f), 1.15f);
  return lerp(p.horizon, lerp(p.horizon, p.nadir, 0.55f), t * 0.7f);
}

// 3D spherical cloud density — continuous across cubemap poles (no 2D project).
float cloudDensity3(float dx, float dy, float dz, float scale, float ox,
                    float oy, float oz, int octaves) {
  return fbm3(dx * scale + ox, dy * scale + oy, dz * scale + oz, octaves);
}

Rgb sampleSky(const SkyPalette &p, float dx, float dy, float dz, float time) {
  // Always unit direction — seed must never inflate sun disc math.
  {
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len > 1e-6f) {
      dx /= len;
      dy /= len;
      dz /= len;
    }
  }
  const float elev = dy;
  Rgb col = atmosphereBase(p, elev);

  // Multi-type fair-weather clouds (cirrus / stratus / cumulus / towers).
  if (p.cloud > 0.0f && p.storm_layers <= 0.0f) {
    auto smooth01 = [](float e0, float e1, float x) {
      const float t =
          std::clamp((x - e0) / std::max(e1 - e0, 1e-5f), 0.0f, 1.0f);
      return t * t * (3.0f - 2.0f * t);
    };
    const float drift = time * 0.0045f;
    const float elev_gate = std::clamp(elev * 1.6f + 0.18f, 0.0f, 1.0f);

    if (p.full_sphere) {
      // Overcast: soft continuous sheet.
      const float n1 =
          cloudDensity3(dx, dy, dz, 3.2f, drift * 0.7f, 0.2f, -drift * 0.4f, 5);
      const float n2 =
          cloudDensity3(dx, dy, dz, 7.0f, -drift * 0.4f + 2.0f, -1.0f, 3.0f, 4);
      float dens = smooth01(0.30f, 0.72f, n1) * smooth01(0.25f, 0.70f, n2);
      dens *=
          p.cloud * (0.55f + 0.45f * std::clamp(elev * 0.5f + 0.55f, 0.f, 1.f));
      const Rgb over_dark = lerp(p.horizon, p.zenith, 0.35f);
      const Rgb over_light = lerp({0.78f, 0.79f, 0.81f}, p.horizon, 0.35f);
      col = lerp(col, lerp(over_dark, over_light, n2), dens * 0.9f);
    } else if (elev > -0.10f) {
      // Cirrus filaments (high, stretched).
      {
        const float n1 = cloudDensity3(dx * 0.45f, dy, dz, 9.5f, drift * 1.8f,
                                       0.9f, -drift * 0.5f, 4);
        const float n2 = cloudDensity3(dx * 0.45f, dy, dz, 18.0f,
                                       -drift + 3.1f, -0.5f, 1.0f, 3);
        float dens = smooth01(0.22f, 0.58f, n1) * smooth01(0.18f, 0.55f, n2);
        dens *= p.cloud * elev_gate * smooth01(-0.05f, 0.40f, elev) * 0.95f;
        const Rgb cirrus = lerp({0.97f, 0.98f, 1.0f}, p.horizon, 0.10f);
        col = lerp(col, cirrus, dens * 0.60f);
      }
      // Altostratus bands.
      {
        const float n1 = cloudDensity3(dx, dy * 0.55f, dz, 4.2f, drift * 0.9f,
                                       0.25f, -drift * 0.5f, 4);
        float dens = smooth01(0.18f, 0.55f, n1);
        dens *= p.cloud * elev_gate *
                std::clamp(1.0f - elev * 0.55f, 0.35f, 1.0f) * 0.75f;
        const Rgb sheet =
            lerp(lerp(p.horizon, p.zenith, 0.2f), {0.88f, 0.90f, 0.94f}, 0.5f);
        col = lerp(col, sheet, dens * 0.52f);
      }
      // Stratus sheet (broad, lower).
      {
        const float n1 = cloudDensity3(dx, dy, dz, 2.8f, drift * 0.55f, 0.15f,
                                       -drift * 0.35f, 4);
        float dens = smooth01(0.12f, 0.52f, n1);
        dens *= p.cloud * elev_gate *
                std::clamp(1.0f - elev * 0.9f, 0.3f, 1.0f) * 0.70f;
        const Rgb sheet =
            lerp(lerp(p.horizon, p.zenith, 0.12f), {0.90f, 0.91f, 0.94f}, 0.55f);
        col = lerp(col, sheet, dens * 0.48f);
      }
      // Cumulus puffs (domain-warped clumps) — main coverage.
      {
        const float w =
            cloudDensity3(dx, dy, dz, 2.5f, drift + 1.0f, 0.3f, -0.5f, 3);
        const float n1 = cloudDensity3(dx + w * 0.15f, dy, dz - w * 0.1f, 5.2f,
                                       drift * 1.15f, 0.3f, -drift * 0.9f, 5);
        const float n2 = cloudDensity3(dx, dy, dz, 9.5f, -drift * 0.5f + 2.0f,
                                       -1.0f, 3.0f, 4);
        float dens = smooth01(0.12f, 0.55f, n1) * smooth01(0.10f, 0.52f, n2);
        dens *= p.cloud * elev_gate * 1.05f;
        const Rgb top = lerp({0.99f, 0.99f, 1.0f}, p.horizon, 0.08f);
        const Rgb bot = lerp(p.horizon, p.zenith, 0.10f);
        const float shade =
            std::clamp(0.30f + 0.70f * elev + 0.12f * n2, 0.0f, 1.0f);
        col = lerp(col, lerp(bot, top, shade), dens * 0.82f);
      }
      // Stratocumulus broken field.
      {
        const float n1 =
            cloudDensity3(dx, dy * 0.7f, dz, 7.0f, drift * 1.4f, 0.5f,
                          -drift * 0.7f, 4);
        float dens = smooth01(0.20f, 0.55f, n1);
        dens *= p.cloud * elev_gate * 0.65f;
        const Rgb ccol = lerp({0.94f, 0.95f, 0.98f}, p.horizon, 0.2f);
        col = lerp(col, ccol, dens * 0.55f);
      }
      // Cumulonimbus towers when cover is high.
      if (p.cloud > 0.35f) {
        const float n1 = cloudDensity3(dx, dy * 1.75f, dz, 3.6f, drift * 0.4f,
                                       0.0f, -drift * 0.25f, 4);
        const float ridge = 1.0f - std::abs(2.0f * n1 - 1.0f);
        float dens = smooth01(0.30f, 0.75f, ridge) * smooth01(0.05f, 0.5f, elev);
        dens *= p.cloud * elev_gate * 0.40f;
        const Rgb tower =
            lerp({0.52f, 0.48f, 0.52f}, {0.96f, 0.90f, 0.84f},
                 std::clamp(elev * 1.3f, 0.0f, 1.0f));
        col = lerp(col, tower, dens * 0.72f);
      }
    }
  }

  // Storm: layered banks + scud (static cubemap; no full-sky grey soup).
  if (p.storm_layers > 0.0f) {
    auto smooth01 = [](float e0, float e1, float x) {
      const float t =
          std::clamp((x - e0) / std::max(e1 - e0, 1e-5f), 0.0f, 1.0f);
      return t * t * (3.0f - 2.0f * t);
    };
    const float drift = time * 0.02f;
    // High deck
    {
      const float n =
          cloudDensity3(dx, dy, dz, 2.6f, drift * 0.8f, 0.1f, -drift * 0.4f, 5);
      float dens = smooth01(0.25f, 0.7f, n);
      dens *= std::clamp(elev * 0.5f + 0.7f, 0.4f, 1.0f) * p.storm_layers;
      col = lerp(col, {0.20f, 0.22f, 0.26f}, dens * 0.6f);
      col = lerp(col, {0.08f, 0.09f, 0.11f}, smooth01(0.45f, 0.9f, dens) * 0.5f);
    }
    // Mid cells
    {
      const float n =
          cloudDensity3(dx, dy, dz, 4.5f, drift * 1.3f, 0.2f, -drift * 0.9f, 5);
      float dens = smooth01(0.3f, 0.75f, n);
      dens *= std::clamp(1.0f - elev * 0.4f, 0.35f, 1.0f) * p.storm_layers;
      col = lerp(col, {0.14f, 0.15f, 0.18f}, dens * 0.65f);
    }
    // Low scud near horizon
    {
      const float n =
          cloudDensity3(dx, dy, dz, 6.5f, drift * 1.8f, -0.3f, -drift, 4);
      float dens = smooth01(0.35f, 0.75f, n) *
                   smooth01(0.25f, -0.05f, elev) *
                   smooth01(-0.35f, 0.05f, elev);
      col = lerp(col, {0.16f, 0.17f, 0.19f}, dens * 0.7f);
    }
  }

  if (p.nebula > 0.0f) {
    // Pure direction-domain 3D fbm — seamless at every cubemap pole/edge.
    const float n1 = fbm3(dx * 2.6f + 1.7f, dy * 2.6f - 0.3f, dz * 2.6f - 0.5f, 6);
    const float n2 = fbm3(dx * 4.2f - 2.0f, dy * 4.2f + 0.8f, dz * 4.2f + 1.1f, 5);
    const float ridge = 1.0f - std::abs(2.0f * n1 - 1.0f);
    const float mask =
        std::pow(std::clamp(ridge * n2, 0.0f, 1.0f), 1.35f) * p.nebula;
    const Rgb neb = lerp(p.nebula_a, p.nebula_b, n2);
    col.r = std::min(1.0f, col.r + neb.r * mask * 0.85f);
    col.g = std::min(1.0f, col.g + neb.g * mask * 0.85f);
    col.b = std::min(1.0f, col.b + neb.b * mask * 0.85f);
  }

  if (p.milky > 0.0f) {
    // Space: galactic plane. End: thin cyan/magenta ribbons (not grey storm).
    const bool endish = p.no_sun && p.nebula > 0.7f && p.star_dense > 0.1f &&
                        p.star_dense < 0.25f;
    if (endish) {
      const float plane = dx * 0.42f + dy * 0.18f - dz * 0.55f;
      const float band = std::exp(-plane * plane * 28.0f);
      const float swirl = fbm3(dx * 6.0f, dy * 6.0f, dz * 6.0f, 4);
      const float dens = band * std::clamp(0.35f + 0.65f * swirl, 0.0f, 1.0f) *
                         p.milky;
      col.r = std::min(1.0f, col.r + dens * 0.45f);
      col.g = std::min(1.0f, col.g + dens * 0.28f);
      col.b = std::min(1.0f, col.b + dens * 0.70f);
    } else {
      const float plane = dx * 0.28f + dy * 0.08f - dz * 0.78f;
      const float band = std::exp(-plane * plane * 16.0f);
      const float structure = fbm3(dx * 5.0f, dy * 5.0f, dz * 5.0f + 2.0f, 5);
      const float amt =
          band * std::pow(std::clamp(structure, 0.0f, 1.0f), 1.2f) * p.milky;
      col.r = std::min(1.0f, col.r + amt * 0.55f);
      col.g = std::min(1.0f, col.g + amt * 0.50f);
      col.b = std::min(1.0f, col.b + amt * 0.72f);
      const Rgb band_stars = sampleStars(
          dx, dy, dz, 0.18f, p.star_scale * 1.5f, 0.55f * p.milky, -2.0f);
      col.r = std::min(1.0f, col.r + band_stars.r);
      col.g = std::min(1.0f, col.g + band_stars.g);
      col.b = std::min(1.0f, col.b + band_stars.b);
    }
  }

  // Safe sun disc (matches GPU applySunSafe — cannot fill half the sky).
  if (!p.no_sun && (p.sun_size > 0.0f || p.sun_bloom > 0.0f)) {
    float sx = p.sun_x, sy = p.sun_y, sz = p.sun_z;
    const float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sl > 1e-6f) {
      sx /= sl;
      sy /= sl;
      sz /= sl;
    }
    const float cosA = std::clamp(dx * sx + dy * sy + dz * sz, -1.0f, 1.0f);
    const float ang = 1.0f - cosA;
    const float size = std::clamp(p.sun_size, 0.0f, 0.07f);
    if (size > 0.0f) {
      const float discR = std::max(size * size * 2.2f, 1e-5f);
      const float disc = std::clamp(1.0f - ang / discR, 0.0f, 1.0f);
      col = lerp(col, p.sun_tint, disc * disc * 0.92f);
    }
    const float bloomAmt = std::clamp(p.sun_bloom, 0.0f, 0.45f);
    if (bloomAmt > 0.0f) {
      const float bloomK =
          std::clamp(14.0f / std::max(bloomAmt, 0.08f), 18.0f, 90.0f);
      const float bloom = std::exp(-ang * bloomK) * bloomAmt;
      col.r = std::min(1.0f, col.r + p.sun_tint.r * bloom);
      col.g = std::min(1.0f, col.g + p.sun_tint.g * bloom);
      col.b = std::min(1.0f, col.b + p.sun_tint.b * bloom);
    }
  }

  const float elev_min = p.full_sphere ? -2.0f : 0.02f;
  if (p.star_dense > 0.0f) {
    const Rgb s =
        sampleStars(dx, dy, dz, p.star_dense, p.star_scale, 0.55f, elev_min);
    col.r = std::min(1.0f, col.r + s.r);
    col.g = std::min(1.0f, col.g + s.g);
    col.b = std::min(1.0f, col.b + s.b);
  }
  if (p.star_bright > 0.0f) {
    const Rgb s = sampleStars(dx, dy, dz, p.star_bright, p.star_scale * 0.55f,
                              1.15f, elev_min);
    col.r = std::min(1.0f, col.r + s.r);
    col.g = std::min(1.0f, col.g + s.g);
    col.b = std::min(1.0f, col.b + s.b);
  }
  return col;
}

constexpr int kSkyboxFaceRes = 384; // faster switch; quality still fine in viewport
constexpr std::size_t kMaximumPreviewHdrBytes = 16u * 1024u * 1024u;
constexpr float kPi = 3.14159265358979323846f;

std::uint64_t previewAssetGeneration(PreviewSceneId id) {
  return kPreviewSkyboxContentVersion +
         (static_cast<std::uint64_t>(canonicalPreviewSceneId(id)) << 16) +
         (static_cast<std::uint64_t>(kSkyboxFaceRes) << 32) +
         (std::uint64_t{1} << 63);
}

void setPreviewError(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool readPreviewHdr(const std::filesystem::path &path,
                    std::vector<std::uint8_t> &encoded,
                    std::string *error) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    setPreviewError(error, "preview HDR asset is unavailable: " +
                               path.string());
    return false;
  }
  const std::streamoff end = input.tellg();
  if (end <= 0 ||
      static_cast<std::uint64_t>(end) >
          static_cast<std::uint64_t>(kMaximumPreviewHdrBytes)) {
    setPreviewError(error, "preview HDR asset has an invalid file size: " +
                               path.string());
    return false;
  }
  encoded.resize(static_cast<std::size_t>(end));
  input.seekg(0, std::ios::beg);
  input.read(reinterpret_cast<char *>(encoded.data()),
             static_cast<std::streamsize>(encoded.size()));
  if (!input) {
    setPreviewError(error, "preview HDR asset could not be read completely: " +
                               path.string());
    return false;
  }
  return true;
}

Rgb sampleEnvironmentBilinear(const FloatEnvironmentImage &image, float u,
                              float v) {
  const int width = static_cast<int>(image.width);
  const int height = static_cast<int>(image.height);
  u -= std::floor(u);
  v = std::clamp(v, 0.0f, 1.0f);
  const float fx = u * static_cast<float>(width) - 0.5f;
  const float fy = v * static_cast<float>(height) - 0.5f;
  const int x0_unwrapped = static_cast<int>(std::floor(fx));
  const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, height - 1);
  const int y1 = std::min(y0 + 1, height - 1);
  const int x0 = ((x0_unwrapped % width) + width) % width;
  const int x1 = (x0 + 1) % width;
  const float tx = fx - std::floor(fx);
  const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
  const auto texel = [&](int x, int y) {
    const std::size_t i =
        (static_cast<std::size_t>(y) * image.width +
         static_cast<std::size_t>(x)) *
        4u;
    return Rgb{image.rgba[i + 0], image.rgba[i + 1], image.rgba[i + 2]};
  };
  const Rgb a = lerp(texel(x0, y0), texel(x1, y0), tx);
  const Rgb b = lerp(texel(x0, y1), texel(x1, y1), tx);
  return lerp(a, b, ty);
}

float previewExposure(const FloatEnvironmentImage &image, PreviewSceneId id) {
  double log_sum = 0.0;
  std::size_t count = 0;
  const PreviewSceneId canonical = canonicalPreviewSceneId(id);
  // Pure-sky assets intentionally contain a dark/empty lower hemisphere.
  // Excluding it prevents auto exposure from clipping the useful sky to gray.
  const std::uint32_t rows =
      canonical == PreviewSceneId::Studio
          ? image.height
          : std::max(1u, static_cast<std::uint32_t>(image.height * 0.48f));
  for (std::uint32_t y = 0; y < rows; ++y) {
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const std::size_t i =
          (static_cast<std::size_t>(y) * image.width + x) * 4u;
      const double r =
          std::max(0.0, static_cast<double>(image.rgba[i + 0]));
      const double g =
          std::max(0.0, static_cast<double>(image.rgba[i + 1]));
      const double b =
          std::max(0.0, static_cast<double>(image.rgba[i + 2]));
      const double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      if (std::isfinite(luminance)) {
        log_sum += std::log(std::max(luminance, 1.0e-6));
        ++count;
      }
    }
  }
  if (count == 0u) {
    return 1.0f;
  }
  const double log_average = std::exp(log_sum / static_cast<double>(count));
  const double key =
      canonical == PreviewSceneId::Night
          ? 0.10
          : (canonical == PreviewSceneId::Studio ? 0.24 : 0.32);
  return static_cast<float>(
      std::clamp(key / std::max(log_average, 1.0e-6), 1.0e-4, 64.0));
}

std::uint8_t toneMapPreviewChannel(float radiance, float exposure) {
  const float mapped =
      1.0f - std::exp(-std::max(radiance, 0.0f) * exposure);
  const float srgb = std::pow(std::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f);
  return static_cast<std::uint8_t>(srgb * 255.0f + 0.5f);
}

bool convertPreviewEnvironment(const FloatEnvironmentImage &image,
                               PreviewSceneId id, PreviewSkybox &out,
                               std::string *error) {
  if (!image.valid() || image.width != image.height * 2u) {
    setPreviewError(error,
                    "preview HDR must be a valid 2:1 equirectangular image");
    return false;
  }

  PreviewSkybox candidate;
  candidate.face_size = kSkyboxFaceRes;
  candidate.rgba.assign(static_cast<std::size_t>(kSkyboxFaceRes) *
                            kSkyboxFaceRes * 4u * 6u,
                        0u);
  candidate.generation = previewAssetGeneration(id);
  candidate.cc0_asset = true;
  const float exposure = previewExposure(image, id);
  const bool synthesize_lower_hemisphere =
      canonicalPreviewSceneId(id) != PreviewSceneId::Studio;

  for (int face = 0; face < 6; ++face) {
    std::uint8_t *base =
        candidate.rgba.data() +
        static_cast<std::size_t>(face) * kSkyboxFaceRes *
            kSkyboxFaceRes * 4u;
    for (int y = 0; y < kSkyboxFaceRes; ++y) {
      for (int x = 0; x < kSkyboxFaceRes; ++x) {
        const float face_u =
            (static_cast<float>(x) + 0.5f) / kSkyboxFaceRes;
        const float face_v =
            (static_cast<float>(y) + 0.5f) / kSkyboxFaceRes;
        float dx = 0.0f;
        float dy = 0.0f;
        float dz = 0.0f;
        faceUvToDir(face, face_u, face_v, dx, dy, dz);
        const float panorama_u =
            0.5f + std::atan2(dz, dx) / (2.0f * kPi);
        Rgb radiance{};
        if (!synthesize_lower_hemisphere) {
          const float panorama_v =
              std::acos(std::clamp(dy, -1.0f, 1.0f)) / kPi;
          radiance =
              sampleEnvironmentBilinear(image, panorama_u, panorama_v);
        } else {
          constexpr float kHorizonLift = 0.08f;
          const float horizon_v = std::acos(kHorizonLift) / kPi;
          const Rgb horizon =
              sampleEnvironmentBilinear(image, panorama_u, horizon_v);
          if (dy >= kHorizonLift) {
            const float panorama_v =
                std::acos(std::clamp(dy, -1.0f, 1.0f)) / kPi;
            radiance =
                sampleEnvironmentBilinear(image, panorama_u, panorama_v);
          } else if (dy >= 0.0f) {
            const float panorama_v =
                std::acos(std::clamp(dy, -1.0f, 1.0f)) / kPi;
            const Rgb source =
                sampleEnvironmentBilinear(image, panorama_u, panorama_v);
            const float t = std::clamp(dy / kHorizonLift, 0.0f, 1.0f);
            const float smooth = t * t * (3.0f - 2.0f * t);
            radiance = lerp(horizon, source, smooth);
          } else {
            const float down = std::clamp(-dy, 0.0f, 1.0f);
            const float mirrored_y = std::max(down, kHorizonLift);
            const float mirrored_v = std::acos(mirrored_y) / kPi;
            Rgb mirrored =
                sampleEnvironmentBilinear(image, panorama_u, mirrored_v);
            const float smooth = down * down * (3.0f - 2.0f * down);
            const float scale = 0.62f - 0.32f * smooth;
            mirrored.r *= scale;
            mirrored.g *= scale;
            mirrored.b *= scale;
            radiance = lerp(horizon, mirrored, smooth);
          }
        }
        const std::size_t i =
            (static_cast<std::size_t>(y) * kSkyboxFaceRes +
             static_cast<std::size_t>(x)) *
            4u;
        base[i + 0] = toneMapPreviewChannel(radiance.r, exposure);
        base[i + 1] = toneMapPreviewChannel(radiance.g, exposure);
        base[i + 2] = toneMapPreviewChannel(radiance.b, exposure);
        base[i + 3] = 255u;
      }
    }
  }

  out = std::move(candidate);
  return true;
}

void generateSkybox(PreviewSceneId id, float scene_seed, PreviewSkybox &out) {
  const int kFace = kSkyboxFaceRes;
  out.face_size = kFace;
  out.rgba.assign(static_cast<std::size_t>(kFace) * kFace * 4u * 6u, 0);
  out.source_identity = "procedural-fallback";
  out.cc0_asset = false;
  const std::uint32_t seed_bits =
      static_cast<std::uint32_t>(scene_seed * 1000.0f) & 0xFFFFu;
  out.generation = kPreviewSkyboxContentVersion +
                   (static_cast<std::uint64_t>(id) << 16) +
                   (static_cast<std::uint64_t>(kFace) << 32) +
                   (static_cast<std::uint64_t>(seed_bits) << 48);
  const SkyPalette pal = paletteFor(id);
  // Seed only rotates the sphere (unit-preserving). Never add large offsets
  // to the direction — that used to explode the sun disc into a white sky.
  const float rot = scene_seed * 0.37f;
  const float ca = std::cos(rot);
  const float sa = std::sin(rot);
  for (int face = 0; face < 6; ++face) {
    std::uint8_t *base =
        out.rgba.data() +
        static_cast<std::size_t>(face) * kFace * kFace * 4u;
    for (int y = 0; y < kFace; ++y) {
      for (int x = 0; x < kFace; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / kFace;
        const float v = (static_cast<float>(y) + 0.5f) / kFace;
        float dx, dy, dz;
        faceUvToDir(face, u, v, dx, dy, dz);
        const float rx = dx * ca + dz * sa;
        const float rz = -dx * sa + dz * ca;
        // time carries a tiny seed so cloud phase differs per switch.
        const Rgb c = sampleSky(pal, rx, dy, rz, std::fmod(scene_seed, 10.0f));
        const std::size_t i =
            (static_cast<std::size_t>(y) * kFace +
             static_cast<std::size_t>(x)) *
            4u;
        base[i + 0] = static_cast<std::uint8_t>(
            std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
        base[i + 1] = static_cast<std::uint8_t>(
            std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
        base[i + 2] = static_cast<std::uint8_t>(
            std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
        base[i + 3] = 255;
      }
    }
  }
}

void applyDir(PreviewSceneLighting &L) {
  const float len = std::sqrt(L.direction[0] * L.direction[0] +
                              L.direction[1] * L.direction[1] +
                              L.direction[2] * L.direction[2]);
  if (len > 1e-6f) {
    L.direction[0] /= len;
    L.direction[1] /= len;
    L.direction[2] /= len;
  }
}

} // namespace

bool loadPreviewSceneSkyboxAsset(PreviewSceneId id,
                                 const std::filesystem::path &asset_root,
                                 PreviewSkybox &out, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  const PreviewSceneId canonical = canonicalPreviewSceneId(id);
  const char *filename = previewSceneAssetFilename(canonical);
  if (asset_root.empty() || filename[0] == '\0') {
    setPreviewError(error, "preview scene has no configured CC0 asset");
    return false;
  }

  const std::filesystem::path path = asset_root / filename;
  std::vector<std::uint8_t> encoded;
  if (!readPreviewHdr(path, encoded, error)) {
    return false;
  }
  FloatEnvironmentImage image;
  HdrDecodeLimits limits;
  limits.maximum_width = 2048u;
  limits.maximum_height = 1024u;
  limits.maximum_decoded_bytes = std::size_t{32} * 1024u * 1024u;
  if (!decodeRadianceHdr(std::span<const std::uint8_t>(encoded), image, error,
                         limits)) {
    return false;
  }

  PreviewSkybox candidate;
  if (!convertPreviewEnvironment(image, canonical, candidate, error)) {
    return false;
  }
  candidate.source_identity = path.string();
  out = std::move(candidate);
  return true;
}

std::array<float, 3> previewLightDirectionFromSide(int light_side) noexcept {
  switch (light_side) {
  case 1:
    return {-0.10f, 0.20f, 1.00f};
  case 2:
    return {0.10f, 0.20f, -1.00f};
  case 3:
    return {1.00f, 0.20f, -0.10f};
  case 4:
    return {-1.00f, 0.20f, 0.10f};
  case 5:
    return {0.20f, -1.00f, 0.00f};
  default:
    return {0.60f, 1.00f, 0.20f};
  }
}

PreviewSceneLighting makePreviewSceneLighting(PreviewSceneId id, float time_sec,
                                              bool dynamic,
                                              float scene_seed) noexcept {
  id = canonicalPreviewSceneId(id);
  (void)scene_seed;
  PreviewSceneLighting L;
  switch (id) {
  case PreviewSceneId::Studio:
    L.direction = previewLightDirectionFromSide(1);
    L.color = {1.02f, 1.02f, 1.06f};
    L.ambient = 0.58f;
    L.intensity = 0.48f;
    L.clear_r = L.clear_g = 0.55f;
    L.clear_b = 0.58f;
    break;
  case PreviewSceneId::Sky: {
    L.direction = {0.35f, 0.70f, 0.40f};
    L.color = {1.05f, 1.02f, 0.95f};
    L.ambient = 0.48f;
    L.intensity = 0.90f;
    L.clear_r = 0.45f;
    L.clear_g = 0.62f;
    L.clear_b = 0.85f;
    break;
  }
  case PreviewSceneId::Night:
    L.direction = {-0.4f, 0.55f, 0.3f};
    L.color = {0.55f, 0.62f, 0.95f};
    L.ambient = 0.20f;
    L.intensity = 0.55f;
    L.clear_r = 0.015f;
    L.clear_g = 0.02f;
    L.clear_b = 0.06f;
    break;
  case PreviewSceneId::Sunset: {
    L.direction = {0.75f, 0.15f, 0.35f};
    L.color = {1.1f, 0.72f, 0.40f};
    L.ambient = 0.35f;
    L.intensity = 0.95f;
    L.clear_r = 0.55f;
    L.clear_g = 0.28f;
    L.clear_b = 0.18f;
    break;
  }
  case PreviewSceneId::Desert:
    L.direction = {0.25f, 0.8f, 0.35f};
    L.color = {1.08f, 0.98f, 0.78f};
    L.ambient = 0.50f;
    L.intensity = 0.95f;
    L.clear_r = 0.55f;
    L.clear_g = 0.72f;
    L.clear_b = 0.88f;
    break;
  case PreviewSceneId::Ocean:
    // Cool key + higher ambient so transparent water and deep body read in 3D.
    L.direction = {0.42f, 0.72f, 0.22f};
    L.color = {0.92f, 0.98f, 1.10f};
    L.ambient = 0.42f;
    L.intensity = 1.05f;
    L.clear_r = 0.28f;
    L.clear_g = 0.52f;
    L.clear_b = 0.78f;
    break;
  case PreviewSceneId::Overcast:
    L.direction = {0.15f, 0.9f, 0.15f};
    L.color = {0.95f, 0.95f, 0.98f};
    L.ambient = 0.62f;
    L.intensity = 0.40f;
    L.clear_r = 0.58f;
    L.clear_g = 0.60f;
    L.clear_b = 0.62f;
    break;
  case PreviewSceneId::None:
  default:
    L.direction = {0.35f, 0.85f, 0.40f};
    L.color = {1, 1, 1};
    L.ambient = 0.38f;
    L.intensity = 0.85f;
    L.clear_r = 26.0f / 255.0f;
    L.clear_g = 28.0f / 255.0f;
    L.clear_b = 34.0f / 255.0f;
    break;
  }

  // Dynamic mode: cheap closed-form light animation (no sky sampling).
  // Keeps raster mesh lighting in sync with animated sky / weather.
  if (dynamic) {
    const float t = time_sec;
    switch (id) {
    case PreviewSceneId::Sky: {
      // Slow sun arc (period ~5 min at full orbit scale).
      const float a = t * 0.018f;
      const float elev = 0.55f + 0.28f * std::sin(a * 0.5f + 0.4f);
      L.direction = {0.35f + 0.35f * std::sin(a), elev,
                     0.40f + 0.25f * std::cos(a)};
      const float day = std::clamp(elev * 1.1f, 0.35f, 1.0f);
      L.intensity = 0.55f + 0.55f * day;
      L.ambient = 0.32f + 0.22f * day;
      L.color = {1.05f, 1.0f + 0.02f * day, 0.92f + 0.06f * (1.0f - day)};
      break;
    }
    case PreviewSceneId::Sunset: {
      // Sun slowly sinks; warmer + dimmer over minutes.
      const float sink = 0.5f + 0.5f * std::sin(t * 0.03f);
      L.direction = {0.75f, 0.06f + 0.16f * sink, 0.35f};
      L.intensity = 0.75f + 0.35f * sink;
      L.ambient = 0.28f + 0.12f * sink;
      L.color = {1.15f, 0.55f + 0.25f * sink, 0.28f + 0.18f * sink};
      break;
    }
    case PreviewSceneId::Ocean: {
      const float a = t * 0.015f;
      L.direction = {0.42f + 0.12f * std::sin(a),
                     0.68f + 0.08f * std::cos(a * 0.6f), 0.22f};
      // Soft key breathe — keeps wave normals lively without strobing.
      L.intensity = 0.95f + 0.14f * (0.5f + 0.5f * std::sin(t * 0.18f));
      L.ambient = 0.38f + 0.08f * (0.5f + 0.5f * std::sin(t * 0.12f + 1.0f));
      L.color = {0.92f, 0.98f + 0.02f * std::sin(t * 0.1f), 1.08f};
      break;
    }
    default:
      break;
    }
  }

  applyDir(L);
  return L;
}

namespace {
std::uint32_t g_scene_rng = 0xC0FFEEu;

float nextSceneSeed() {
  // Keep normal interactive scene switching varied, while allowing unattended
  // native/SR/RR captures to render identical geometry in separate processes.
  if (const char *fixed = std::getenv("XPBD_PREVIEW_SCENE_SEED");
      fixed != nullptr && fixed[0] != '\0') {
    char *end = nullptr;
    const float parsed = std::strtof(fixed, &end);
    if (end != fixed && end != nullptr && end[0] == '\0' &&
        std::isfinite(parsed)) {
      return std::clamp(parsed, 1.0f, 97.999f);
    }
  }
  // LCG — different every switch without pulling <random>.
  g_scene_rng = g_scene_rng * 1664525u + 1013904223u;
  // Mix wall time for extra entropy when available.
  const auto now = static_cast<std::uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFu);
  g_scene_rng ^= now * 0x9E3779B9u;
  const float u = static_cast<float>(g_scene_rng & 0xFFFFFFu) / 16777215.0f;
  return u * 97.0f + 1.0f; // [1, 98)
}
} // namespace

void buildViewportRasterScene(PreviewSceneId id, bool show_grid, bool show_axes,
                              bool dynamic, float time_sec,
                              ViewportRasterScene &out,
                              const std::filesystem::path &asset_root) {
  id = canonicalPreviewSceneId(id);
  const PreviewSceneId previous_id = out.id;
  const bool previous_dynamic = out.surface_dynamic_baked;
  const bool use_dynamic =
      dynamic && previewSceneSupportsDynamic(id);

  // Reseed whenever scene or dynamic mode changes (static + dynamic).
  if (previous_id != id || previous_dynamic != use_dynamic ||
      out.scene_seed <= 0.0f) {
    out.scene_seed = nextSceneSeed();
  }

  // Desert is static. The osgw-derived Ocean surface is rebuilt at ~20 Hz.
  const bool animate_surface =
      use_dynamic && id == PreviewSceneId::Ocean;
  constexpr float kOceanPeriod = 1.0f / 20.0f;
  const bool surface_stale =
      animate_surface &&
      (time_sec - out.surface_time_baked >= kOceanPeriod ||
       !out.surface_dynamic_baked);
  const bool rebuild_env =
      previous_id != id || surface_stale ||
      (id == PreviewSceneId::Ocean &&
       out.surface_dynamic_baked != animate_surface) ||
      (id == PreviewSceneId::Desert && out.environment.solid.empty());

  if (rebuild_env) {
    out.environment.clear();
  }
  out.grid.clear();
  out.id = id;
  out.lighting =
      makePreviewSceneLighting(id, time_sec, use_dynamic, out.scene_seed);
  out.show_grid = show_grid;
  out.show_axes = show_axes;
  out.environment_unlit = true;
  out.solid_ground = false;
  out.show_environment = false;

  if (previewSceneUsesSkybox(id)) {
    const std::uint64_t expected_asset_generation =
        previewAssetGeneration(id);
    if (previous_id != id || !out.skybox.valid() ||
        (out.skybox.cc0_asset &&
         out.skybox.generation != expected_asset_generation)) {
      std::string asset_error;
      if (!loadPreviewSceneSkyboxAsset(id, asset_root, out.skybox,
                                       &asset_error)) {
        generateSkybox(id, out.scene_seed, out.skybox);
      }
    }

    if (id == PreviewSceneId::Desert) {
      if (rebuild_env) {
        detail::appendOpenSourceDesertSurface(out.environment, out.scene_seed);
        out.surface_time_baked = time_sec;
        out.surface_dynamic_baked = false;
      }
      out.show_environment = true;
    } else if (id == PreviewSceneId::Ocean) {
      if (rebuild_env) {
        detail::appendOpenSourceOceanSurface(
            out.environment, time_sec, animate_surface, out.scene_seed);
        out.surface_time_baked = time_sec;
        out.surface_dynamic_baked = animate_surface;
      }
      out.show_environment = true;
    }
  } else {
    out.skybox.clear();
  }

  if (rebuild_env) {
    ++out.geometry_generation;
  }

  if (show_grid || show_axes) {
    const float gy =
        (id == PreviewSceneId::Ocean || id == PreviewSceneId::Desert) ? 0.08f
                                                                     : 0.02f;
    // Larger grid for larger studio / open scenes.
    const float grid_half = (id == PreviewSceneId::Studio) ? 48.0f : 32.0f;
    appendBlockbenchGrid(out.grid, show_axes, grid_half, 1.0f, 16.0f, gy);
  }
}

} // namespace xpbd::gfx
