#include "xpbd/gfx/viewport_mesh.hpp"

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/cube_geometry.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/uv_domain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace xpbd::gfx {
namespace {








constexpr int kTexturedFaceCorners[6][4] = {
    {2, 6, 4,
     0},
    {7, 3, 1,
     5},
    {4, 5, 1,
     0},
    {2, 3, 7,
     6},
    {3, 2, 0,
     1},
    {6, 7, 5,
     4},
};



constexpr int kCubeFaces[6][4] = {
    {0, 2, 6, 4},
    {1, 5, 7, 3},
    {0, 1, 5, 4},
    {2, 6, 7, 3},
    {0, 1, 3, 2},
    {4, 6, 7, 5},
};



using FaceTexelUV = ResolvedFaceUv;

FaceUvCorners faceCornerTexels(const FaceTexelUV &face_uv) noexcept {
  return bedrockFaceUvCorners(face_uv);
}

bool skipCoincidentOppositeFace(const loader::Cube &cube,
                                int face_i) noexcept {
  const double size_x = std::abs(cube.size[0]) + 2.0 * cube.inflate;
  const double size_y = std::abs(cube.size[1]) + 2.0 * cube.inflate;
  const double size_z = std::abs(cube.size[2]) + 2.0 * cube.inflate;
  constexpr double kFlatEpsilon = 1.0e-6;
  // A zero-thickness Bedrock cube produces two coincident quads. Keep the
  // positive-axis face; RT is explicitly two-sided, so the other side remains
  // visible without a second coplanar primitive (which would z-fight).
  return (size_x <= kFlatEpsilon &&
          face_i == static_cast<int>(StaticModelFaceDirection::West)) ||
         (size_y <= kFlatEpsilon &&
          face_i == static_cast<int>(StaticModelFaceDirection::Down)) ||
         (size_z <= kFlatEpsilon &&
          face_i == static_cast<int>(StaticModelFaceDirection::North));
}

struct Rgba {
  float r, g, b, a;
};

Rgba roleCubeColor(render::JointRole role) {

  switch (role) {
  case render::JointRole::Physics:
    return {0.55f, 0.72f, 0.98f, 1.0f};
  case render::JointRole::BakedPhysics:
    return {0.45f, 0.88f, 0.98f, 1.0f};
  case render::JointRole::Fixed:
    return {0.92f, 0.55f, 0.55f, 1.0f};
  case render::JointRole::Selected:
    return {1.0f, 0.88f, 0.35f, 1.0f};
  default:
    return {0.82f, 0.80f, 0.76f, 1.0f};
  }
}

Rgba brighten(Rgba c, float k) {
  return {std::clamp(c.r * k, 0.0f, 1.0f), std::clamp(c.g * k, 0.0f, 1.0f),
          std::clamp(c.b * k, 0.0f, 1.0f), c.a};
}

// 贴图模式下选中 / 悬停组的染色（着色器中与纹理颜色相乘，>1 表示提亮）。
constexpr Rgba kSelectedTexturedTint{1.42f, 1.28f, 0.72f, 1.0f};
constexpr Rgba kHoveredTexturedTint{1.20f, 1.23f, 1.32f, 1.0f};

Rgba roleBoneColor(render::JointRole role) {
  switch (role) {
  case render::JointRole::Physics:
    return {0.35f, 0.55f, 0.95f, 1.0f};
  case render::JointRole::BakedPhysics:
    return {0.25f, 0.80f, 0.95f, 1.0f};
  case render::JointRole::Fixed:
    return {0.85f, 0.40f, 0.40f, 1.0f};
  case render::JointRole::Selected:
    return {1.0f, 0.80f, 0.15f, 1.0f};
  default:
    return {0.55f, 0.55f, 0.60f, 1.0f};
  }
}



Rgba tintByNormal(Rgba c, float nx, float ny, float nz) {

  const float top = std::clamp(ny, 0.0f, 1.0f);
  const float side =
      std::clamp(std::abs(nx) * 0.55f + std::abs(nz) * 0.45f, 0.0f, 1.0f);
  const float bot = std::clamp(-ny, 0.0f, 1.0f);
  float mul = 0.72f + 0.28f * top + 0.08f * side - 0.12f * bot;

  float hr = 0.0f, hg = 0.0f, hb = 0.0f;
  if (std::abs(ny) >= std::abs(nx) && std::abs(ny) >= std::abs(nz)) {
    hr = 0.04f * top;
    hg = 0.02f * top;
  } else if (std::abs(nx) >= std::abs(nz)) {
    hb = 0.05f * side;
    hr = -0.02f * side;
  } else {
    hg = 0.03f * side;
    hb = 0.02f * side;
  }
  return {std::clamp(c.r * mul + hr, 0.0f, 1.0f),
          std::clamp(c.g * mul + hg, 0.0f, 1.0f),
          std::clamp(c.b * mul + hb, 0.0f, 1.0f), c.a};
}



Rgba boneAccent(const std::string &name, Rgba base) {
  std::uint32_t h = 2166136261u;
  for (unsigned char ch : name) {
    h ^= ch;
    h *= 16777619u;
  }
  const float a = static_cast<float>((h >> 0) & 255) / 255.0f;
  const float b = static_cast<float>((h >> 8) & 255) / 255.0f;
  const float c = static_cast<float>((h >> 16) & 255) / 255.0f;
  return {std::clamp(base.r * 0.82f + a * 0.18f, 0.0f, 1.0f),
          std::clamp(base.g * 0.82f + b * 0.18f, 0.0f, 1.0f),
          std::clamp(base.b * 0.82f + c * 0.18f, 0.0f, 1.0f), base.a};
}

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
  const float e1x = p[1][0] - p[0][0];
  const float e1y = p[1][1] - p[0][1];
  const float e1z = p[1][2] - p[0][2];
  const float e2x = p[2][0] - p[0][0];
  const float e2y = p[2][1] - p[0][1];
  const float e2z = p[2][2] - p[0][2];
  float cx = e1y * e2z - e1z * e2y;
  float cy = e1z * e2x - e1x * e2z;
  float cz = e1x * e2y - e1y * e2x;
  const bool flip = (cx * nx + cy * ny + cz * nz) < 0.0f;
  if (!flip) {
    pushTri(dst, p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0],
            p[2][1], p[2][2], nx, ny, nz, c);
    pushTri(dst, p[0][0], p[0][1], p[0][2], p[2][0], p[2][1], p[2][2], p[3][0],
            p[3][1], p[3][2], nx, ny, nz, c);
  } else {
    pushTri(dst, p[0][0], p[0][1], p[0][2], p[3][0], p[3][1], p[3][2], p[2][0],
            p[2][1], p[2][2], nx, ny, nz, c);
    pushTri(dst, p[0][0], p[0][1], p[0][2], p[2][0], p[2][1], p[2][2], p[1][0],
            p[1][1], p[1][2], nx, ny, nz, c);
  }
}


void pushThickLine(std::vector<MeshVertex> &dst, float x0, float y0, float z0,
                   float x1, float y1, float z1, float half_w, Rgba c) {
  float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
  const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (len < 1e-6f) {
    return;
  }
  dx /= len;
  dy /= len;
  dz /= len;
  float px = -dz, py = 0.0f, pz = dx;
  float pl = std::sqrt(px * px + py * py + pz * pz);
  if (pl < 1e-5f) {
    px = 1.0f;
    py = 0.0f;
    pz = 0.0f;
    pl = 1.0f;
  }
  px = px / pl * half_w;
  py = py / pl * half_w;
  pz = pz / pl * half_w;
  float qx = dy * pz - dz * py;
  float qy = dz * px - dx * pz;
  float qz = dx * py - dy * px;
  float ql = std::sqrt(qx * qx + qy * qy + qz * qz);
  if (ql < 1e-5f) {
    qx = 0.0f;
    qy = half_w;
    qz = 0.0f;
  } else {
    qx = qx / ql * half_w;
    qy = qy / ql * half_w;
    qz = qz / ql * half_w;
  }
  const float a[4][3] = {{x0 + px + qx, y0 + py + qy, z0 + pz + qz},
                         {x0 - px + qx, y0 - py + qy, z0 - pz + qz},
                         {x0 - px - qx, y0 - py - qy, z0 - pz - qz},
                         {x0 + px - qx, y0 + py - qy, z0 + pz - qz}};
  const float b[4][3] = {{x1 + px + qx, y1 + py + qy, z1 + pz + qz},
                         {x1 - px + qx, y1 - py + qy, z1 - pz + qz},
                         {x1 - px - qx, y1 - py - qy, z1 - pz - qz},
                         {x1 + px - qx, y1 + py - qy, z1 + pz - qz}};
  const float np[3] = {px / half_w, py / half_w, pz / half_w};
  const float nq[3] = {qx / half_w, qy / half_w, qz / half_w};
  float f0[4][3] = {{a[0][0], a[0][1], a[0][2]},
                    {b[0][0], b[0][1], b[0][2]},
                    {b[1][0], b[1][1], b[1][2]},
                    {a[1][0], a[1][1], a[1][2]}};
  pushQuad(dst, f0, nq[0], nq[1], nq[2], c);
  float f1[4][3] = {{a[1][0], a[1][1], a[1][2]},
                    {b[1][0], b[1][1], b[1][2]},
                    {b[2][0], b[2][1], b[2][2]},
                    {a[2][0], a[2][1], a[2][2]}};
  pushQuad(dst, f1, -np[0], -np[1], -np[2], c);
  float f2[4][3] = {{a[2][0], a[2][1], a[2][2]},
                    {b[2][0], b[2][1], b[2][2]},
                    {b[3][0], b[3][1], b[3][2]},
                    {a[3][0], a[3][1], a[3][2]}};
  pushQuad(dst, f2, -nq[0], -nq[1], -nq[2], c);
  float f3[4][3] = {{a[3][0], a[3][1], a[3][2]},
                    {b[3][0], b[3][1], b[3][2]},
                    {b[0][0], b[0][1], b[0][2]},
                    {a[0][0], a[0][1], a[0][2]}};
  pushQuad(dst, f3, np[0], np[1], np[2], c);
}

struct GroundLayout {
  float y = 0.0f;
  float half = 12.0f;
  float step = 1.0f;
};






GroundLayout computeGroundLayout(
    const loader::Geometry *geometry,
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses) {
  GroundLayout g;
  if (geometry == nullptr || poses.empty()) {
    return g;
  }
  float min_x = 1e9f, max_x = -1e9f;
  float min_z = 1e9f, max_z = -1e9f;
  bool any = false;
  std::size_t cube_count = 0;
  for (const auto &bone : geometry->bones) {
    cube_count += bone.cubes.size();
  }
  const auto transform_mode =
      baker::CubeGeometry::recommendedTransformSimdMode(cube_count);
  for (const auto &bone : geometry->bones) {
    auto pit = poses.find(bone.name);
    if (pit == poses.end()) {
      continue;
    }

    {
      const float px = static_cast<float>(pit->second.world_position[0]);
      const float pz = static_cast<float>(pit->second.world_position[2]);
      min_x = std::min(min_x, px);
      max_x = std::max(max_x, px);
      min_z = std::min(min_z, pz);
      max_z = std::max(max_z, pz);
      any = true;
    }
    for (const auto &cube : bone.cubes) {
      const auto bind = baker::CubeGeometry::bindVertices(cube);
      const auto transformed = baker::CubeGeometry::transformPoints8(
          pit->second, bind, transform_mode);
      for (int v = 0; v < 8; ++v) {
        const auto offset = static_cast<std::size_t>(v * 3);
        const float wx = static_cast<float>(transformed[offset]);
        const float wz = static_cast<float>(transformed[offset + 2]);
        min_x = std::min(min_x, wx);
        max_x = std::max(max_x, wx);
        min_z = std::min(min_z, wz);
        max_z = std::max(max_z, wz);
        any = true;
      }
    }
  }
  if (!any) {
    return g;
  }

  // The grid is the world-coordinate reference, not a model pedestal. Keeping
  // it at Y=0 preserves the meaning of authored negative-Y geometry.
  g.y = 0.0f;
  const float span_xz = std::max(max_x - min_x, max_z - min_z);
  g.half = std::clamp(std::max(span_xz * 0.75f, 6.0f), 6.0f, 48.0f);

  g.step = std::clamp(g.half / 6.0f, 0.5f, 4.0f);
  return g;
}

void appendGround(ViewportGpuScene &out, const GroundLayout &layout) {
  const float h = layout.half;
  const float y = layout.y;
  const float p[4][3] = {{-h, y, -h}, {h, y, -h}, {h, y, h}, {-h, y, h}};
  pushQuad(out.solid, p, 0.0f, 1.0f, 0.0f, {0.22f, 0.24f, 0.30f, 1.0f});
}

void appendGrid(ViewportGpuScene &out, const GroundLayout &layout) {


  const float h = layout.half;
  const float y = layout.y + 0.03f;
  const float step = layout.step;
  const Rgba major{0.55f, 0.57f, 0.64f, 1.0f};
  const Rgba minor{0.34f, 0.36f, 0.42f, 1.0f};
  const float half_w = std::clamp(step * 0.02f, 0.012f, 0.04f);
  const int n = std::max(1, static_cast<int>(std::ceil(h / step)));
  for (int i = -n; i <= n; ++i) {
    const float t = static_cast<float>(i) * step;
    if (std::abs(t) > h + 1e-4f) {
      continue;
    }
    const Rgba &c = (i % 5 == 0) ? major : minor;
    pushThickLine(out.solid, t, y, -h, t, y, h, half_w, c);
    pushThickLine(out.solid, -h, y, t, h, y, t, half_w, c);
    out.line_segment_count += 2;
  }
}

const ViewportGpuScene &defaultGroundCache() {
  static const ViewportGpuScene cache = [] {
    ViewportGpuScene result;
    appendGround(result, GroundLayout{});
    appendGrid(result, GroundLayout{});
    return result;
  }();
  return cache;
}

float estimateScale(
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses) {
  if (poses.empty()) {
    return 1.0f;
  }
  float min_y = 1e9f, max_y = -1e9f;
  for (const auto &[_, p] : poses) {
    const float y = static_cast<float>(p.world_position[1]);
    min_y = std::min(min_y, y);
    max_y = std::max(max_y, y);
  }
  const float h = std::max(1.0f, max_y - min_y);

  return std::clamp(h * 0.012f, 0.03f, 0.18f);
}


Rgba sampleAlbedo(const TextureImage *tex, double u, double v,
                  Rgba untextured_base) {
  if (tex == nullptr || !tex->valid()) {
    return untextured_base;
  }
  float tr = 1, tg = 1, tb = 1, ta = 1;
  tex->sampleModelAtlasClamp(u, v, tr, tg, tb, ta);

  if (ta < 0.02f) {
    return {tr, tg, tb, 0.0f};
  }
  return {tr, tg, tb, ta};
}


void applyMcbe(float &x, float &y, float &z) {
  x = -x;
  (void)y;
  z = -z;
}

constexpr Rgba kSelectedOutline{1.0f, 0.82f, 0.12f, 1.0f};
constexpr Rgba kHoverOutline{0.92f, 0.95f, 1.0f, 0.95f};

// 选中 / 悬停组的立方体描边（Blockbench 风格的组高亮）。
void appendCubeOutline(ViewportGpuScene &out, const loader::Cube &cube,
                       const baker::BonePoseCalculator::Pose &pose,
                       bool mcbe_coords, const Rgba &outline_color) {
  constexpr int kCubeEdges[12][2] = {
      {0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
      {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
  };
  const Rgba kOutline = outline_color;
  const auto bind = baker::CubeGeometry::bindVertices(cube);
  const auto transformed = baker::CubeGeometry::transformPoints8(
      pose, bind, core::SimdMode::Auto);
  float world[8][3]{};
  for (int vertex = 0; vertex < 8; ++vertex) {
    const auto offset = static_cast<std::size_t>(vertex * 3);
    world[vertex][0] = static_cast<float>(transformed[offset]);
    world[vertex][1] = static_cast<float>(transformed[offset + 1]);
    world[vertex][2] = static_cast<float>(transformed[offset + 2]);
    if (mcbe_coords) {
      applyMcbe(world[vertex][0], world[vertex][1], world[vertex][2]);
    }
  }
  float center[3]{};
  for (const auto &vertex : world) {
    center[0] += vertex[0] * 0.125f;
    center[1] += vertex[1] * 0.125f;
    center[2] += vertex[2] * 0.125f;
  }
  constexpr float kOutlineExpansion = 1.003f;
  for (auto &vertex : world) {
    vertex[0] = center[0] + (vertex[0] - center[0]) * kOutlineExpansion;
    vertex[1] = center[1] + (vertex[1] - center[1]) * kOutlineExpansion;
    vertex[2] = center[2] + (vertex[2] - center[2]) * kOutlineExpansion;
  }

  MeshVertex line_vertex{};
  line_vertex.r = kOutline.r;
  line_vertex.g = kOutline.g;
  line_vertex.b = kOutline.b;
  line_vertex.a = kOutline.a;
  for (const auto &edge : kCubeEdges) {
    for (int endpoint = 0; endpoint < 2; ++endpoint) {
      const int vertex = edge[endpoint];
      line_vertex.px = world[vertex][0];
      line_vertex.py = world[vertex][1];
      line_vertex.pz = world[vertex][2];
      out.lines.push_back(line_vertex);
    }
  }
  out.line_segment_count += 12;
}

std::array<float, 16>
staticBoneTransform(const baker::BonePoseCalculator::Pose &pose,
                    bool mcbe_coords) {
  if (!std::all_of(pose.world_linear.begin(), pose.world_linear.end(),
                   [](double value) { return std::isfinite(value); }) ||
      !std::isfinite(pose.world_translation[0]) ||
      !std::isfinite(pose.world_translation[1]) ||
      !std::isfinite(pose.world_translation[2])) {
    throw std::invalid_argument("static model bone transform must be finite");
  }
  const auto &linear = pose.world_linear;
  std::array<float, 16> result{
      static_cast<float>(linear[0]),
      static_cast<float>(linear[3]),
      static_cast<float>(linear[6]),
      0.0f,
      static_cast<float>(linear[1]),
      static_cast<float>(linear[4]),
      static_cast<float>(linear[7]),
      0.0f,
      static_cast<float>(linear[2]),
      static_cast<float>(linear[5]),
      static_cast<float>(linear[8]),
      0.0f,
      static_cast<float>(pose.world_translation[0]),
      static_cast<float>(pose.world_translation[1]),
      static_cast<float>(pose.world_translation[2]),
      1.0f,
  };
  if (mcbe_coords) {
    for (int column = 0; column < 4; ++column) {
      result[static_cast<std::size_t>(column * 4)] *= -1.0f;
      result[static_cast<std::size_t>(column * 4 + 2)] *= -1.0f;
    }
  }
  return result;
}

void lerp3(const float a[3], const float b[3], float t, float o[3]) {
  o[0] = a[0] + (b[0] - a[0]) * t;
  o[1] = a[1] + (b[1] - a[1]) * t;
  o[2] = a[2] + (b[2] - a[2]) * t;
}







FaceTexelUV resolveCubeFaceUV(const loader::Cube &cube, int face_i) {
  FaceTexelUV out;
  std::string error;
  if (!resolveBedrockFaceUv(
          cube, static_cast<BedrockUvFace>(face_i), out, &error)) {
    throw std::invalid_argument(error.empty() ? "Bedrock face UV is invalid"
                                              : error);
  }
  return out;
}






void pushTexturedFace(std::vector<MeshVertex> &solid,
                      std::vector<MeshVertex> &transparent, const float p[4][3],
                      float nx, float ny, float nz, Rgba base,
                      const TextureImage *tex, const FaceTexelUV &face_uv,
                      double tex_w, double tex_h) {
  const bool textured = tex != nullptr && tex->valid() && face_uv.present &&
                        tex_w > 0.0 && tex_h > 0.0;

  const int seg = textured ? 8 : 1;


  const auto corner_uv = faceCornerTexels(face_uv);

  auto emitTri = [&](std::vector<MeshVertex> &dst, const float a[3],
                     const float b[3], const float c[3], Rgba ca, Rgba cb,
                     Rgba cc) {
    const float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
    const float e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
    const float cx = e1y * e2z - e1z * e2y;
    const float cy = e1z * e2x - e1x * e2z;
    const float cz = e1x * e2y - e1y * e2x;
    const bool flip = (cx * nx + cy * ny + cz * nz) < 0.0f;
    auto pushV = [&](const float q[3], Rgba col) {
      MeshVertex v{};
      v.px = q[0];
      v.py = q[1];
      v.pz = q[2];
      v.nx = nx;
      v.ny = ny;
      v.nz = nz;
      v.r = col.r;
      v.g = col.g;
      v.b = col.b;
      v.a = col.a;
      dst.push_back(v);
    };
    if (flip) {
      pushV(a, ca);
      pushV(c, cc);
      pushV(b, cb);
    } else {
      pushV(a, ca);
      pushV(b, cb);
      pushV(c, cc);
    }
  };


  auto point = [&](float su, float sv, float o[3]) {
    float e0[3], e1[3];
    lerp3(p[0], p[1], su, e0);
    lerp3(p[3], p[2], su, e1);
    lerp3(e0, e1, sv, o);
  };

  auto sampleUV = [&](float su, float sv, double &ou, double &ov) {
    const double u_a =
        corner_uv[0][0] +
        (corner_uv[1][0] - corner_uv[0][0]) * static_cast<double>(su);
    const double v_a =
        corner_uv[0][1] +
        (corner_uv[1][1] - corner_uv[0][1]) * static_cast<double>(su);
    const double u_b =
        corner_uv[3][0] +
        (corner_uv[2][0] - corner_uv[3][0]) * static_cast<double>(su);
    const double v_b =
        corner_uv[3][1] +
        (corner_uv[2][1] - corner_uv[3][1]) * static_cast<double>(su);
    ou = (u_a + (u_b - u_a) * static_cast<double>(sv)) / tex_w;
    ov = (v_a + (v_b - v_a) * static_cast<double>(sv)) / tex_h;
  };

  for (int j = 0; j < seg; ++j) {
    for (int i = 0; i < seg; ++i) {
      const float su0 = static_cast<float>(i) / static_cast<float>(seg);
      const float sv0 = static_cast<float>(j) / static_cast<float>(seg);
      const float su1 = static_cast<float>(i + 1) / static_cast<float>(seg);
      const float sv1 = static_cast<float>(j + 1) / static_cast<float>(seg);
      const float suc = 0.5f * (su0 + su1);
      const float svc = 0.5f * (sv0 + sv1);
      float q00[3], q10[3], q11[3], q01[3];
      point(su0, sv0, q00);
      point(su1, sv0, q10);
      point(su1, sv1, q11);
      point(su0, sv1, q01);

      Rgba cell = base;
      if (textured) {
        double tu = 0.0, tv = 0.0;
        sampleUV(suc, svc, tu, tv);
        cell = sampleAlbedo(tex, tu, tv, base);
        // 与 base tint 相乘，让选中 / 悬停高亮在贴图模式下也生效。
        cell.r = std::clamp(cell.r * base.r, 0.0f, 1.0f);
        cell.g = std::clamp(cell.g * base.g, 0.0f, 1.0f);
        cell.b = std::clamp(cell.b * base.b, 0.0f, 1.0f);
      }

      if (cell.a < 0.02f) {
        continue;
      }

      auto &dst = (cell.a < 0.98f) ? transparent : solid;
      emitTri(dst, q00, q10, q11, cell, cell, cell);
      emitTri(dst, q00, q11, q01, cell, cell, cell);
    }
  }
}

}

void ViewportMeshBuilder::setGeometry(const loader::Geometry *geometry) {
  geometry_ = geometry;
  setTransformSimdMode(core::SimdMode::Auto);
  pose_evaluator_.reset();
  rest_poses_.clear();
  if (geometry_ != nullptr) {
    pose_evaluator_.emplace(geometry_->bones);
    rest_poses_ = pose_evaluator_->calculate(nullptr, 0.0);
  }
  ground_cache_.clear();
  if (geometry_ != nullptr) {
    const GroundLayout ground = computeGroundLayout(geometry_, rest_poses_);
    appendGround(ground_cache_, ground);
    appendGrid(ground_cache_, ground);
  }
}

void ViewportMeshBuilder::setTransformSimdMode(core::SimdMode mode) {
  if (mode != core::SimdMode::Auto) {
    transform_simd_mode_ = core::selectedSimdMode(mode);
    return;
  }
  std::size_t cube_count = 0;
  if (geometry_ != nullptr) {
    for (const auto &bone : geometry_->bones) {
      cube_count += bone.cubes.size();
    }
  }
  transform_simd_mode_ =
      baker::CubeGeometry::recommendedTransformSimdMode(cube_count);
}

render::JointRole ViewportMeshBuilder::roleFor(const std::string &bone_name,
                                               bool baked_style) const {
  if (!selected_bone_.empty() && bone_name == selected_bone_) {
    return render::JointRole::Selected;
  }
  if (mapper_ != nullptr) {


    if (const auto *bc = mapper_->getBoneConfig(bone_name);
        bc != nullptr && bc->fixed.has_value() && *bc->fixed) {
      return render::JointRole::Fixed;
    }
    if (mapper_->isPhysicsBone(bone_name)) {
      return baked_style ? render::JointRole::BakedPhysics
                         : render::JointRole::Physics;
    }
  }
  return render::JointRole::Default;
}

bool ViewportMeshBuilder::isHidden(const std::string &bone_name) const {
  return hidden_bones_ != nullptr && hidden_bones_->contains(bone_name);
}

void ViewportMeshBuilder::buildStaticIndexedModel(
    StaticIndexedModelMesh &out) const {
  out.clear();
  if (geometry_ == nullptr) {
    return;
  }

  constexpr std::size_t kVerticesPerCube = 6u * 4u;
  constexpr std::size_t kIndicesPerCube = 6u * 6u;
  std::size_t cube_estimate = 0;
  for (const auto &bone : geometry_->bones) {
    cube_estimate += bone.cubes.size();
  }
  out.vertices.reserve(cube_estimate * kVerticesPerCube);
  out.indices.reserve(cube_estimate * kIndicesPerCube);
  out.faces.reserve(cube_estimate * 6u);
  out.bone_names.reserve(geometry_->bones.size());
  for (const auto &bone : geometry_->bones) {
    out.bone_names.push_back(bone.name);
  }

  double tex_w =
      static_cast<double>(std::max(1, geometry_->description.texture_width));
  double tex_h =
      static_cast<double>(std::max(1, geometry_->description.texture_height));
  if (texture_ != nullptr && texture_->valid()) {
    std::string domain_error;
    ResolvedUvDomain domain;
    if (!resolveGeometryUvDomain(*geometry_, texture_->width, texture_->height,
                                 domain, &domain_error)) {
      throw std::invalid_argument(domain_error.empty()
                                      ? "model UV Domain resolution failed"
                                      : domain_error);
    }
    out.uv_domain = domain;
    tex_w = domain.width;
    tex_h = domain.height;
  }

  const auto max_u32 =
      static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
  if (geometry_->bones.size() > max_u32) {
    throw std::overflow_error("static model bone index exceeds uint32 range");
  }

  for (std::size_t bone_i = 0; bone_i < geometry_->bones.size(); ++bone_i) {
    const auto &bone = geometry_->bones[bone_i];
    if (bone.cubes.size() > max_u32) {
      throw std::overflow_error("static model cube index exceeds uint32 range");
    }
    const auto bone_index = static_cast<std::uint32_t>(bone_i);
    for (std::size_t cube_i = 0; cube_i < bone.cubes.size(); ++cube_i) {
      const auto &cube = bone.cubes[cube_i];
      const auto bind = baker::CubeGeometry::bindVertices(cube);
      for (int face_i = 0; face_i < 6; ++face_i) {
        if (skipCoincidentOppositeFace(cube, face_i)) {
          continue;
        }
        const FaceTexelUV face_uv = resolveCubeFaceUV(cube, face_i);
        // Bedrock per-face UV objects are also the face-presence mask:
        // omitted entries must not generate fallback white geometry.
        if (cube.uv_mode == loader::CubeUVMode::PerFace &&
            !face_uv.present) {
          continue;
        }
        const bool textured = face_uv.present;
        const int *face_indices =
            textured ? kTexturedFaceCorners[face_i] : kCubeFaces[face_i];
        float positions[4][3]{};
        for (int corner = 0; corner < 4; ++corner) {
          const auto source =
              static_cast<std::size_t>(face_indices[corner] * 3);
          positions[corner][0] = static_cast<float>(bind[source]);
          positions[corner][1] = static_cast<float>(bind[source + 1u]);
          positions[corner][2] = static_cast<float>(bind[source + 2u]);
        }

        const float e1x = positions[1][0] - positions[0][0];
        const float e1y = positions[1][1] - positions[0][1];
        const float e1z = positions[1][2] - positions[0][2];
        const float e2x = positions[2][0] - positions[0][0];
        const float e2y = positions[2][1] - positions[0][1];
        const float e2z = positions[2][2] - positions[0][2];
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        const float normal_length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (normal_length < 1e-8f) {
          continue;
        }
        nx /= normal_length;
        ny /= normal_length;
        nz /= normal_length;

        if (out.vertices.size() > max_u32 - 4u ||
            out.indices.size() > max_u32 - 6u) {
          throw std::overflow_error(
              "static indexed model mesh exceeds uint32 range");
        }
        const auto first_vertex =
            static_cast<std::uint32_t>(out.vertices.size());
        const auto first_index = static_cast<std::uint32_t>(out.indices.size());
        const auto face_uv_corners = faceCornerTexels(face_uv);
        float uvs[4][2]{};
        for (int corner = 0; corner < 4; ++corner) {
          uvs[corner][0] =
              textured ? static_cast<float>(
                             face_uv_corners[corner][0] / tex_w)
                       : 0.0f;
          uvs[corner][1] =
              textured ? static_cast<float>(
                             face_uv_corners[corner][1] / tex_h)
                       : 0.0f;
        }
        const TangentFrame tangent = computeTangentFrame(
            {positions[0][0], positions[0][1], positions[0][2]},
            {positions[1][0], positions[1][1], positions[1][2]},
            {positions[2][0], positions[2][1], positions[2][2]},
            {nx, ny, nz}, {uvs[0][0], uvs[0][1]},
            {uvs[1][0], uvs[1][1]}, {uvs[2][0], uvs[2][1]});
        for (int corner = 0; corner < 4; ++corner) {
          StaticModelVertex vertex{};
          vertex.px = positions[corner][0];
          vertex.py = positions[corner][1];
          vertex.pz = positions[corner][2];
          vertex.nx = nx;
          vertex.ny = ny;
          vertex.nz = nz;
          vertex.u = uvs[corner][0];
          vertex.v = uvs[corner][1];
          vertex.raw_u = textured ? face_uv_corners[corner][0] : 0.0;
          vertex.raw_v = textured ? face_uv_corners[corner][1] : 0.0;
          vertex.tx = tangent.tangent[0];
          vertex.ty = tangent.tangent[1];
          vertex.tz = tangent.tangent[2];
          vertex.tangent_handedness = tangent.handedness;
          vertex.bone_index = bone_index;
          out.vertices.push_back(vertex);
        }
        out.indices.push_back(first_vertex);
        out.indices.push_back(first_vertex + 1u);
        out.indices.push_back(first_vertex + 2u);
        out.indices.push_back(first_vertex);
        out.indices.push_back(first_vertex + 2u);
        out.indices.push_back(first_vertex + 3u);

        StaticModelFace face{};
        face.first_vertex = first_vertex;
        face.vertex_count = 4u;
        face.first_index = first_index;
        face.index_count = 6u;
        face.bone_index = bone_index;
        face.cube_index = static_cast<std::uint32_t>(cube_i);
        face.direction = static_cast<StaticModelFaceDirection>(face_i);
        face.textured = textured;
        out.faces.push_back(face);
      }
      if (out.cube_count == (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::overflow_error(
            "static model cube count exceeds uint32 range");
      }
      ++out.cube_count;
    }
  }
}

void ViewportMeshBuilder::buildFromPoses(
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    bool baked_style, ViewportGpuScene &out, bool include_model) const {
  out.clear();
  ResolvedUvDomain uv_domain;
  if (include_model && geometry_ != nullptr && !poses.empty() &&
      texture_ != nullptr && texture_->valid()) {
    std::string domain_error;
    if (!resolveGeometryUvDomain(*geometry_, texture_->width, texture_->height,
                                 uv_domain, &domain_error)) {
      throw std::invalid_argument(domain_error.empty()
                                      ? "model UV Domain resolution failed"
                                      : domain_error);
    }
  }
  if (show_ground_) {
    out = geometry_ == nullptr ? defaultGroundCache() : ground_cache_;
  }

  if (geometry_ == nullptr || poses.empty()) {
    return;
  }

  if (include_model) {
    std::size_t cube_est = 0;
    for (const auto &bone : geometry_->bones) {
      if (!isHidden(bone.name)) {
        cube_est += bone.cubes.size();
      }
    }
    out.solid.reserve((std::max)(out.solid.capacity(), cube_est * 36u + 1024u));
  }

  const float bone_half = estimateScale(poses);






  double tex_w =
      static_cast<double>(std::max(1, geometry_->description.texture_width));
  double tex_h =
      static_cast<double>(std::max(1, geometry_->description.texture_height));
  if (uv_domain.valid()) {
    tex_w = uv_domain.width;
    tex_h = uv_domain.height;
  }

  for (const auto &bone : geometry_->bones) {
    if (isHidden(bone.name)) {
      continue;
    }
    auto pose_it = poses.find(bone.name);
    if (pose_it == poses.end()) {
      continue;
    }
    const render::JointRole role = roleFor(bone.name, baked_style);
    const bool is_selected = role == render::JointRole::Selected;
    const bool is_hovered = !is_selected && !hovered_bone_.empty() &&
                            bone.name == hovered_bone_;

    const bool textured = texture_ != nullptr && texture_->valid();
    Rgba cube_base = textured ? Rgba{1.0f, 1.0f, 1.0f, 1.0f}
                              : boneAccent(bone.name, roleCubeColor(role));
    if (textured) {
      if (is_selected) {
        cube_base = kSelectedTexturedTint;
      } else if (is_hovered) {
        cube_base = kHoveredTexturedTint;
      }
    } else if (is_hovered) {
      cube_base = brighten(cube_base, 1.22f);
    }

    if (is_selected || is_hovered) {
      const Rgba outline = is_selected ? kSelectedOutline : kHoverOutline;
      for (const auto &cube : bone.cubes) {
        appendCubeOutline(out, cube, pose_it->second, mcbe_coords_, outline);
      }
    }

    if (include_model) {
      for (const auto &cube : bone.cubes) {
        const auto bind = baker::CubeGeometry::bindVertices(cube);
        const auto transformed = baker::CubeGeometry::transformPoints8(
            pose_it->second, bind, transform_simd_mode_);
        float wx[8], wy[8], wz[8];
        for (int v = 0; v < 8; ++v) {
          const auto offset = static_cast<std::size_t>(v * 3);
          wx[v] = static_cast<float>(transformed[offset]);
          wy[v] = static_cast<float>(transformed[offset + 1]);
          wz[v] = static_cast<float>(transformed[offset + 2]);
          if (mcbe_coords_) {
            applyMcbe(wx[v], wy[v], wz[v]);
          }
        }
        for (int face_i = 0; face_i < 6; ++face_i) {
          if (skipCoincidentOppositeFace(cube, face_i)) {
            continue;
          }
          const FaceTexelUV face_uv = resolveCubeFaceUV(cube, face_i);
          if (cube.uv_mode == loader::CubeUVMode::PerFace &&
              !face_uv.present) {
            continue;
          }
          const bool face_textured = textured && face_uv.present;


          const int *face_idx =
              face_textured ? kTexturedFaceCorners[face_i] : kCubeFaces[face_i];
          const float p[4][3] = {
              {wx[face_idx[0]], wy[face_idx[0]], wz[face_idx[0]]},
              {wx[face_idx[1]], wy[face_idx[1]], wz[face_idx[1]]},
              {wx[face_idx[2]], wy[face_idx[2]], wz[face_idx[2]]},
              {wx[face_idx[3]], wy[face_idx[3]], wz[face_idx[3]]},
          };


          const float e1x = p[1][0] - p[0][0];
          const float e1y = p[1][1] - p[0][1];
          const float e1z = p[1][2] - p[0][2];
          const float e2x = p[2][0] - p[0][0];
          const float e2y = p[2][1] - p[0][1];
          const float e2z = p[2][2] - p[0][2];
          float nx = e1y * e2z - e1z * e2y;
          float ny = e1z * e2x - e1x * e2z;
          float nz = e1x * e2y - e1y * e2x;
          const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
          if (nlen < 1e-8f) {
            continue;
          }
          nx /= nlen;
          ny /= nlen;
          nz /= nlen;
          const Rgba face_base = face_textured
                                     ? cube_base
                                     : tintByNormal(cube_base, nx, ny, nz);
          pushTexturedFace(out.solid, out.transparent, p, nx, ny, nz, face_base,
                           face_textured ? texture_ : nullptr, face_uv, tex_w,
                           tex_h);
        }
        ++out.cube_count;
      }
    }

    if (!show_bones_) {
      continue;
    }



    // Parent-pivot connection segments are intentionally not rendered.
    // They obscured detailed models and are not selection feedback. The
    // white/yellow cube outlines remain in out.lines and continue through
    // the DLSS scene pipeline.
    if (role != render::JointRole::Default) {
      float cx = static_cast<float>(pose_it->second.world_position[0]);
      float cy = static_cast<float>(pose_it->second.world_position[1]);
      float cz = static_cast<float>(pose_it->second.world_position[2]);
      if (mcbe_coords_) {
        applyMcbe(cx, cy, cz);
      }
      const float r = bone_half * 0.9f;
      const Rgba jc = roleBoneColor(role);
      const float verts[6][3] = {{cx, cy + r, cz}, {cx, cy - r, cz},
                                 {cx + r, cy, cz}, {cx - r, cy, cz},
                                 {cx, cy, cz + r}, {cx, cy, cz - r}};
      const int tris[8][3] = {{0, 2, 4}, {0, 4, 3}, {0, 3, 5}, {0, 5, 2},
                              {1, 4, 2}, {1, 3, 4}, {1, 5, 3}, {1, 2, 5}};
      for (const auto &t : tris) {
        const float *a = verts[t[0]];
        const float *b = verts[t[1]];
        const float *c = verts[t[2]];
        float nx =
            (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
        float ny =
            (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
        float nz =
            (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl > 1e-8f) {
          nx /= nl;
          ny /= nl;
          nz /= nl;
        }
        pushTri(out.solid, a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2],
                nx, ny, nz, jc);
      }
    }
  }
}

void ViewportMeshBuilder::buildRest(ViewportGpuScene &out) const {
  buildFromPoses(rest_poses_, false, out);
}

void ViewportMeshBuilder::buildAnimation(const loader::Animation *animation,
                                         double time,
                                         ViewportGpuScene &out) const {
  if (geometry_ == nullptr) {
    buildFromPoses({}, false, out);
    return;
  }
  const auto poses = pose_evaluator_->calculate(animation, time);
  buildFromPoses(poses, false, out);
}

void ViewportMeshBuilder::buildBaked(const baker::BakedFrame &frame,
                                     ViewportGpuScene &out) const {
  buildBaked(frame, nullptr, 0.0, out);
}

void ViewportMeshBuilder::buildBaked(
    const baker::BakedFrame &frame,
    const loader::Animation *reference_animation, double reference_time,
    ViewportGpuScene &out) const {
  if (geometry_ == nullptr) {
    buildFromPoses({}, true, out);
    return;
  }
  const auto poses =
      calculateBakedPoses(frame, reference_animation, reference_time);
  buildFromPoses(poses, true, out);
}

std::map<std::string, baker::BonePoseCalculator::Pose>
ViewportMeshBuilder::calculateBakedPoses(
    const baker::BakedFrame &frame,
    const loader::Animation *reference_animation, double reference_time) const {
  if (geometry_ == nullptr) {
    return {};
  }
  std::map<std::string, std::array<double, 3>> pos_ov;
  std::map<std::string, std::array<double, 3>> rot_ov;
  bool any_local = false;
  for (const auto &bs : frame.bone_states) {
    pos_ov[bs.bone_name] = bs.position;
    rot_ov[bs.bone_name] = bs.rotation;
    any_local = true;
  }
  std::map<std::string, baker::BonePoseCalculator::Pose> poses;
  if (any_local) {
    poses = pose_evaluator_->calculate(reference_animation, reference_time,
                                       &pos_ov, &rot_ov);
  } else if (reference_animation != nullptr) {
    poses = pose_evaluator_->calculate(reference_animation, reference_time);
  } else {
    poses = rest_poses_;
  }
  for (const auto &bs : frame.bone_states) {
    if (bs.has_world_position) {
      auto it = poses.find(bs.bone_name);
      if (it != poses.end()) {
        it->second.world_position = bs.world_position;
      }
    }
  }
  return poses;
}

void ViewportMeshBuilder::buildStaticFrameFromPoses(
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    bool baked_style, StaticModelFrameData &out) const {
  out.clear();
  buildFromPoses(poses, baked_style, out.overlays, false);
  if (geometry_ == nullptr) {
    return;
  }
  if (geometry_->bones.size() >
      static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
    throw std::overflow_error("static model frame bone count exceeds uint32");
  }
  out.bones.reserve(geometry_->bones.size());
  std::uint64_t cube_count = 0;
  const bool has_texture = texture_ != nullptr && texture_->valid();
  for (const auto &bone : geometry_->bones) {
    StaticModelBoneState state{};
    const auto pose = poses.find(bone.name);
    if (pose != poses.end()) {
      state.transform = staticBoneTransform(pose->second, mcbe_coords_);
    }
    const render::JointRole role = roleFor(bone.name, baked_style);
    const bool is_selected = role == render::JointRole::Selected;
    const bool is_hovered = !is_selected && !hovered_bone_.empty() &&
                            bone.name == hovered_bone_;
    Rgba tint = has_texture ? Rgba{1.0f, 1.0f, 1.0f, 1.0f}
                            : boneAccent(bone.name, roleCubeColor(role));
    if (has_texture) {
      if (is_selected) {
        tint = kSelectedTexturedTint;
      } else if (is_hovered) {
        tint = kHoveredTexturedTint;
      }
    } else if (is_hovered) {
      tint = brighten(tint, 1.22f);
    }
    if (isHidden(bone.name)) {
      tint.a = 0.0f;
    }
    state.tint = {tint.r, tint.g, tint.b, tint.a};
    out.bones.push_back(state);
    if (!isHidden(bone.name)) {
      cube_count += bone.cubes.size();
    }
  }
  if (cube_count > (std::numeric_limits<std::uint32_t>::max)()) {
    throw std::overflow_error("static model frame cube count exceeds uint32");
  }
  out.cube_count = static_cast<std::uint32_t>(cube_count);
}

void ViewportMeshBuilder::buildStaticRestFrame(
    StaticModelFrameData &out) const {
  buildStaticFrameFromPoses(rest_poses_, false, out);
}

void ViewportMeshBuilder::buildStaticAnimationFrame(
    const loader::Animation *animation, double time,
    StaticModelFrameData &out) const {
  const auto poses =
      geometry_ == nullptr
          ? std::map<std::string, baker::BonePoseCalculator::Pose>{}
          : pose_evaluator_->calculate(animation, time);
  buildStaticFrameFromPoses(poses, false, out);
}

void ViewportMeshBuilder::buildStaticBakedFrame(
    const baker::BakedFrame &frame,
    const loader::Animation *reference_animation, double reference_time,
    StaticModelFrameData &out) const {
  buildStaticFrameFromPoses(
      calculateBakedPoses(frame, reference_animation, reference_time), true,
      out);
}

void buildSessionViewportScene(const loader::Geometry &geometry,
                               const baker::BoneMapper &bone_mapper,
                               const std::string &selected_bone,
                               const loader::Animation *selected_animation,
                               double preview_time, bool show_baked,
                               const baker::BakedFrame *baked_frame,
                               const TextureImage *texture, bool show_bones,
                               bool mcbe_coords, ViewportGpuScene &out,
                               bool show_ground,
                               const std::set<std::string> *hidden_bones,
                               const std::string &hovered_bone) {
  ViewportMeshBuilder builder;
  builder.setGeometry(geometry.bones.empty() ? nullptr : &geometry);
  builder.setBoneMapper(&bone_mapper);
  builder.setSelectedBone(selected_bone);
  builder.setHoveredBone(hovered_bone);
  builder.setHiddenBones(hidden_bones);
  builder.setTexture(texture);
  builder.setShowBones(show_bones);
  builder.setShowGround(show_ground);
  builder.setMcbeCoords(mcbe_coords);
  if (show_baked && baked_frame != nullptr) {
    builder.buildBaked(*baked_frame, selected_animation, preview_time, out);
  } else if (selected_animation != nullptr) {
    builder.buildAnimation(selected_animation, preview_time, out);
  } else {
    builder.buildRest(out);
  }
}

void buildSessionStaticModelFrame(const loader::Geometry &geometry,
                                  const baker::BoneMapper &bone_mapper,
                                  const std::string &selected_bone,
                                  const loader::Animation *selected_animation,
                                  double preview_time, bool show_baked,
                                  const baker::BakedFrame *baked_frame,
                                  const TextureImage *texture, bool show_bones,
                                  bool mcbe_coords, StaticModelFrameData &out,
                                  bool show_ground,
                                  const std::set<std::string> *hidden_bones) {
  ViewportMeshBuilder builder;
  builder.setGeometry(geometry.bones.empty() ? nullptr : &geometry);
  builder.setBoneMapper(&bone_mapper);
  builder.setSelectedBone(selected_bone);
  builder.setHiddenBones(hidden_bones);
  builder.setTexture(texture);
  builder.setShowBones(show_bones);
  builder.setShowGround(show_ground);
  builder.setMcbeCoords(mcbe_coords);
  if (show_baked && baked_frame != nullptr) {
    builder.buildStaticBakedFrame(*baked_frame, selected_animation,
                                  preview_time, out);
  } else if (selected_animation != nullptr) {
    builder.buildStaticAnimationFrame(selected_animation, preview_time, out);
  } else {
    builder.buildStaticRestFrame(out);
  }
}

}
