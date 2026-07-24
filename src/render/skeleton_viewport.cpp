#include "xpbd/render/skeleton_viewport.hpp"

#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/baker/cube_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xpbd::render {
namespace {



constexpr int kCubeFaces[6][4] = {
    {0, 2, 6, 4},
    {1, 5, 7, 3},
    {0, 1, 5, 4},
    {2, 6, 7, 3},
    {0, 4, 5, 1},
    {2, 3, 7, 6},
};

struct Rgba {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 0;
};

Rgba roleCubeColor(JointRole role, float shade) {


  float r = 180.0f;
  float g = 180.0f;
  float b = 200.0f;
  float a = 0.42f;
  switch (role) {
  case JointRole::Physics:
  case JointRole::BakedPhysics:
    r = 120.0f;
    g = 170.0f;
    b = 255.0f;
    a = 0.48f;
    break;
  case JointRole::Fixed:
    r = 220.0f;
    g = 90.0f;
    b = 90.0f;
    a = 0.50f;
    break;
  case JointRole::Selected:
    r = 255.0f;
    g = 220.0f;
    b = 80.0f;
    a = 0.55f;
    break;
  default:
    break;
  }
  const float s = std::clamp(shade, 0.35f, 1.0f);
  return {static_cast<std::uint8_t>(std::clamp(r * s, 0.0f, 255.0f)),
          static_cast<std::uint8_t>(std::clamp(g * s, 0.0f, 255.0f)),
          static_cast<std::uint8_t>(std::clamp(b * s, 0.0f, 255.0f)),
          static_cast<std::uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f))};
}

Rgba roleJointColor(JointRole role) {
  switch (role) {
  case JointRole::Physics:
    return {64, 140, 255, 255};
  case JointRole::BakedPhysics:
    return {77, 204, 255, 255};
  case JointRole::Fixed:
    return {230, 77, 77, 255};
  case JointRole::Selected:
    return {255, 217, 64, 255};
  default:
    return {178, 178, 188, 255};
  }
}

Rgba roleSegmentColor(JointRole role) {
  switch (role) {
  case JointRole::Physics:
    return {89, 166, 255, 217};
  case JointRole::BakedPhysics:
    return {102, 217, 255, 230};
  case JointRole::Fixed:
    return {230, 77, 77, 255};
  case JointRole::Selected:
    return {255, 217, 64, 255};
  default:
    return {140, 140, 148, 191};
  }
}

void blendPixel(std::uint8_t *px, Rgba src) {
  if (src.a == 0) {
    return;
  }
  if (src.a >= 255) {
    px[0] = src.r;
    px[1] = src.g;
    px[2] = src.b;
    px[3] = 255;
    return;
  }
  const float sa = src.a / 255.0f;
  const float da = px[3] / 255.0f;
  const float out_a = sa + da * (1.0f - sa);
  if (out_a <= 1e-5f) {
    px[0] = px[1] = px[2] = px[3] = 0;
    return;
  }
  const float inv = 1.0f / out_a;
  px[0] = static_cast<std::uint8_t>(
      std::clamp((src.r * sa + px[0] * da * (1.0f - sa)) * inv, 0.0f, 255.0f));
  px[1] = static_cast<std::uint8_t>(
      std::clamp((src.g * sa + px[1] * da * (1.0f - sa)) * inv, 0.0f, 255.0f));
  px[2] = static_cast<std::uint8_t>(
      std::clamp((src.b * sa + px[2] * da * (1.0f - sa)) * inv, 0.0f, 255.0f));
  px[3] = static_cast<std::uint8_t>(std::clamp(out_a * 255.0f, 0.0f, 255.0f));
}

void fillTriangle(std::vector<std::uint8_t> &rgba, int w, int h, float x0,
                  float y0, float x1, float y1, float x2, float y2,
                  Rgba color) {

  int min_x = static_cast<int>(std::floor(std::min({x0, x1, x2})));
  int max_x = static_cast<int>(std::ceil(std::max({x0, x1, x2})));
  int min_y = static_cast<int>(std::floor(std::min({y0, y1, y2})));
  int max_y = static_cast<int>(std::ceil(std::max({y0, y1, y2})));
  min_x = std::max(0, min_x);
  min_y = std::max(0, min_y);
  max_x = std::min(w - 1, max_x);
  max_y = std::min(h - 1, max_y);
  if (min_x > max_x || min_y > max_y) {
    return;
  }



  auto edge = [](float ax, float ay, float bx, float by, float cx, float cy) {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
  };
  const float area = edge(x0, y0, x1, y1, x2, y2);
  if (std::abs(area) < 1e-4f) {
    return;
  }
  const float inv_area = 1.0f / area;

  for (int y = min_y; y <= max_y; ++y) {
    const float py = static_cast<float>(y) + 0.5f;
    for (int x = min_x; x <= max_x; ++x) {
      const float px = static_cast<float>(x) + 0.5f;
      const float w0 = edge(x1, y1, x2, y2, px, py) * inv_area;
      const float w1 = edge(x2, y2, x0, y0, px, py) * inv_area;
      const float w2 = edge(x0, y0, x1, y1, px, py) * inv_area;
      if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) {
        continue;
      }
      blendPixel(&rgba[static_cast<std::size_t>((y * w + x) * 4)], color);
    }
  }
}

void fillQuad(std::vector<std::uint8_t> &rgba, int w, int h,
              const std::array<float, 8> &xy, Rgba color) {
  fillTriangle(rgba, w, h, xy[0], xy[1], xy[2], xy[3], xy[4], xy[5], color);
  fillTriangle(rgba, w, h, xy[0], xy[1], xy[4], xy[5], xy[6], xy[7], color);
}

void drawThickLine(std::vector<std::uint8_t> &rgba, int w, int h, float x0,
                   float y0, float x1, float y1, float thickness, Rgba color) {
  std::array<float, 8> q{};
  lineToQuad(x0, y0, x1, y1, thickness, q);
  fillQuad(rgba, w, h, q, color);
}

void drawCircle(std::vector<std::uint8_t> &rgba, int w, int h, float cx,
                float cy, float radius, Rgba color) {
  const int min_x = std::max(0, static_cast<int>(std::floor(cx - radius)));
  const int max_x = std::min(w - 1, static_cast<int>(std::ceil(cx + radius)));
  const int min_y = std::max(0, static_cast<int>(std::floor(cy - radius)));
  const int max_y = std::min(h - 1, static_cast<int>(std::ceil(cy + radius)));
  const float r2 = radius * radius;
  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - cx;
      const float dy = static_cast<float>(y) + 0.5f - cy;
      if (dx * dx + dy * dy <= r2) {
        blendPixel(&rgba[static_cast<std::size_t>((y * w + x) * 4)], color);
      }
    }
  }
}

float faceShade(float nx, float ny, float nz) {

  constexpr float lx = 0.35f;
  constexpr float ly = 0.85f;
  constexpr float lz = -0.40f;
  const float inv = 1.0f / std::sqrt(lx * lx + ly * ly + lz * lz);
  const float ndotl =
      std::max(0.0f, nx * lx * inv + ny * ly * inv + nz * lz * inv);
  return 0.45f + 0.55f * ndotl;
}

void expandBounds(float &min_x, float &min_y, float &min_z, float &max_x,
                  float &max_y, float &max_z, double x, double y, double z) {
  min_x = std::min(min_x, static_cast<float>(x));
  min_y = std::min(min_y, static_cast<float>(y));
  min_z = std::min(min_z, static_cast<float>(z));
  max_x = std::max(max_x, static_cast<float>(x));
  max_y = std::max(max_y, static_cast<float>(y));
  max_z = std::max(max_z, static_cast<float>(z));
}

void applyMcbeBasis(double &x, double &z) {
  x = -x;
  z = -z;
}

struct SegmentHit {
  float distance_squared = 0.0f;
  float t = 0.0f;
};

SegmentHit pointSegmentHit(float px, float py, float x0, float y0, float x1,
                           float y1) {
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float length_squared = dx * dx + dy * dy;
  if (length_squared <= 1e-8f) {
    const float ox = px - x0;
    const float oy = py - y0;
    return {ox * ox + oy * oy, 0.0f};
  }
  const float t =
      std::clamp(((px - x0) * dx + (py - y0) * dy) / length_squared, 0.0f,
                 1.0f);
  const float ox = px - (x0 + t * dx);
  const float oy = py - (y0 + t * dy);
  return {ox * ox + oy * oy, t};
}

float triangleEdge(float ax, float ay, float bx, float by, float px,
                   float py) {
  return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

bool pointInTriangle(float px, float py, float ax, float ay, float bx,
                     float by, float cx, float cy) {
  const float e0 = triangleEdge(ax, ay, bx, by, px, py);
  const float e1 = triangleEdge(bx, by, cx, cy, px, py);
  const float e2 = triangleEdge(cx, cy, ax, ay, px, py);
  constexpr float kEpsilon = 1e-4f;
  const bool has_negative = e0 < -kEpsilon || e1 < -kEpsilon ||
                            e2 < -kEpsilon;
  const bool has_positive =
      e0 > kEpsilon || e1 > kEpsilon || e2 > kEpsilon;
  return !(has_negative && has_positive);
}

bool pointInQuad(float px, float py, const std::array<float, 8> &xy) {
  return pointInTriangle(px, py, xy[0], xy[1], xy[2], xy[3], xy[4], xy[5]) ||
         pointInTriangle(px, py, xy[0], xy[1], xy[4], xy[5], xy[6], xy[7]);
}

bool triangleBarycentrics(float px, float py, float ax, float ay, float bx,
                          float by, float cx, float cy, float &w0, float &w1,
                          float &w2) {
  const float denominator =
      (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
  if (std::abs(denominator) <= 1e-8f) {
    return false;
  }
  w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / denominator;
  w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / denominator;
  w2 = 1.0f - w0 - w1;
  constexpr float kEpsilon = 1e-4f;
  return w0 >= -kEpsilon && w1 >= -kEpsilon && w2 >= -kEpsilon;
}

float perspectiveDepth(float d0, float d1, float d2, float w0, float w1,
                       float w2, float fallback) {
  constexpr float kMinDepth = 1e-6f;
  if (!std::isfinite(d0) || !std::isfinite(d1) || !std::isfinite(d2) ||
      d0 <= kMinDepth || d1 <= kMinDepth || d2 <= kMinDepth) {
    return fallback;
  }
  const float inverse_depth = w0 / d0 + w1 / d1 + w2 / d2;
  if (!std::isfinite(inverse_depth) || inverse_depth <= kMinDepth) {
    return fallback;
  }
  return 1.0f / inverse_depth;
}

float faceDepthAt(const ProjectedFace &face, float x, float y) {
  float w0 = 0.0f;
  float w1 = 0.0f;
  float w2 = 0.0f;
  if (triangleBarycentrics(x, y, face.xy[0], face.xy[1], face.xy[2],
                           face.xy[3], face.xy[4], face.xy[5], w0, w1, w2)) {
    return perspectiveDepth(face.depths[0], face.depths[1], face.depths[2], w0,
                            w1, w2, face.depth);
  }
  if (triangleBarycentrics(x, y, face.xy[0], face.xy[1], face.xy[4],
                           face.xy[5], face.xy[6], face.xy[7], w0, w1, w2)) {
    return perspectiveDepth(face.depths[0], face.depths[2], face.depths[3], w0,
                            w1, w2, face.depth);
  }
  return face.depth;
}

float segmentDepthAt(const ProjectedSegment &segment, float t) {
  constexpr float kMinDepth = 1e-6f;
  if (!std::isfinite(segment.depth0) || !std::isfinite(segment.depth1) ||
      segment.depth0 <= kMinDepth || segment.depth1 <= kMinDepth) {
    return segment.depth;
  }
  const float inverse_depth =
      (1.0f - t) / segment.depth0 + t / segment.depth1;
  if (!std::isfinite(inverse_depth) || inverse_depth <= kMinDepth) {
    return segment.depth;
  }
  return 1.0f / inverse_depth;
}

}

void lineToQuad(float x0, float y0, float x1, float y1, float thickness,
                std::array<float, 8> &out_xy) {
  float dx = x1 - x0;
  float dy = y1 - y0;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-4f) {
    const float h = thickness * 0.5f;
    out_xy = {x0 - h, y0 - h, x0 + h, y0 - h, x0 + h, y0 + h, x0 - h, y0 + h};
    return;
  }
  dx /= len;
  dy /= len;
  const float nx = -dy * thickness * 0.5f;
  const float ny = dx * thickness * 0.5f;
  out_xy = {x0 + nx, y0 + ny, x1 + nx, y1 + ny,
            x1 - nx, y1 - ny, x0 - nx, y0 - ny};
}

void SkeletonViewport::setGeometry(const loader::Geometry *geometry) {
  geometry_ = geometry;
}

void SkeletonViewport::setBoneMapper(const baker::BoneMapper *mapper) {
  mapper_ = mapper;
}

std::map<std::string, baker::BonePoseCalculator::Pose>
SkeletonViewport::restPoses() const {
  if (geometry_ == nullptr || geometry_->bones.empty()) {
    return {};
  }
  return baker::BonePoseCalculator::calculate(geometry_->bones, nullptr, 0.0);
}

bool SkeletonViewport::computeBounds(std::array<float, 3> &center,
                                     float &radius) const {
  if (geometry_ == nullptr || geometry_->bones.empty()) {
    return false;
  }
  const auto poses = restPoses();
  if (poses.empty()) {
    return false;
  }
  float min_x = std::numeric_limits<float>::max();
  float min_y = std::numeric_limits<float>::max();
  float min_z = std::numeric_limits<float>::max();
  float max_x = std::numeric_limits<float>::lowest();
  float max_y = std::numeric_limits<float>::lowest();
  float max_z = std::numeric_limits<float>::lowest();
  bool any = false;

  for (const auto &[_, pose] : poses) {
    double x = pose.world_position[0];
    const double y = pose.world_position[1];
    double z = pose.world_position[2];
    if (mcbe_coords_) {
      applyMcbeBasis(x, z);
    }
    expandBounds(min_x, min_y, min_z, max_x, max_y, max_z, x, y, z);
    any = true;
  }
  for (const auto &bone : geometry_->bones) {
    auto pose_it = poses.find(bone.name);
    if (pose_it == poses.end()) {
      continue;
    }
    for (const auto &cube : bone.cubes) {
      const auto bind = baker::CubeGeometry::bindVertices(cube);
      for (int v = 0; v < 8; ++v) {
        double world[3];
        baker::CubeGeometry::transformPoint(
            pose_it->second, bind[static_cast<std::size_t>(v * 3)],
            bind[static_cast<std::size_t>(v * 3 + 1)],
            bind[static_cast<std::size_t>(v * 3 + 2)], world);
        if (mcbe_coords_) {
          applyMcbeBasis(world[0], world[2]);
        }
        expandBounds(min_x, min_y, min_z, max_x, max_y, max_z, world[0],
                     world[1], world[2]);
        any = true;
      }
    }
  }
  if (!any) {
    return false;
  }
  center = {(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f,
            (min_z + max_z) * 0.5f};
  const float dx = max_x - min_x;
  const float dy = max_y - min_y;
  const float dz = max_z - min_z;
  radius = std::max(2.0f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
  return true;
}

JointRole SkeletonViewport::roleFor(const std::string &bone_name,
                                    bool baked_style) const {
  if (!selected_bone_.empty() && bone_name == selected_bone_) {
    return JointRole::Selected;
  }
  if (mapper_ == nullptr) {
    return JointRole::Default;
  }
  if (mapper_->isPhysicsBone(bone_name)) {
    if (mapper_->isFixedBone(bone_name)) {
      return JointRole::Fixed;
    }
    return baked_style ? JointRole::BakedPhysics : JointRole::Physics;
  }
  return JointRole::Default;
}

bool SkeletonViewport::isHidden(const std::string &bone_name) const {
  return hidden_bones_ != nullptr && hidden_bones_->contains(bone_name);
}

void SkeletonViewport::appendGrid(SkeletonDrawList &list,
                                  const ViewportCamera &camera, float view_w,
                                  float view_h) const {

  constexpr float kHalf = 10.0f;
  constexpr float kStep = 1.0f;
  for (float t = -kHalf; t <= kHalf + 0.01f; t += kStep) {
    float x0, y0, d0, x1, y1, d1;
    if (camera.project(t, 0.0f, -kHalf, view_w, view_h, x0, y0, d0) &&
        camera.project(t, 0.0f, kHalf, view_w, view_h, x1, y1, d1)) {
      ProjectedSegment s;
      s.x0 = x0;
      s.y0 = y0;
      s.x1 = x1;
      s.y1 = y1;
      s.depth = 0.5f * (d0 + d1);
      s.depth0 = d0;
      s.depth1 = d1;
      s.thickness = (std::abs(t) < 0.01f) ? 1.6f : 1.0f;
      s.role = JointRole::Default;
      list.grid.push_back(s);
    }
    if (camera.project(-kHalf, 0.0f, t, view_w, view_h, x0, y0, d0) &&
        camera.project(kHalf, 0.0f, t, view_w, view_h, x1, y1, d1)) {
      ProjectedSegment s;
      s.x0 = x0;
      s.y0 = y0;
      s.x1 = x1;
      s.y1 = y1;
      s.depth = 0.5f * (d0 + d1);
      s.depth0 = d0;
      s.depth1 = d1;
      s.thickness = (std::abs(t) < 0.01f) ? 1.6f : 1.0f;
      s.role = JointRole::Default;
      list.grid.push_back(s);
    }
  }
}

void SkeletonViewport::appendGround(SkeletonDrawList &list,
                                    const ViewportCamera &camera, float view_w,
                                    float view_h) const {

  constexpr float kHalf = 10.0f;
  const float corners[4][3] = {{-kHalf, 0.0f, -kHalf},
                               {kHalf, 0.0f, -kHalf},
                               {kHalf, 0.0f, kHalf},
                               {-kHalf, 0.0f, kHalf}};
  float sx[4], sy[4], sd[4];
  for (int i = 0; i < 4; ++i) {
    if (!camera.project(corners[i][0], corners[i][1], corners[i][2], view_w,
                        view_h, sx[i], sy[i], sd[i])) {
      return;
    }
  }
  ProjectedFace face;
  face.xy = {sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], sx[3], sy[3]};
  face.depth = 0.25f * (sd[0] + sd[1] + sd[2] + sd[3]);
  face.depths = {sd[0], sd[1], sd[2], sd[3]};
  face.shade = 0.7f;
  face.is_ground = true;
  list.faces.push_back(face);
}

void SkeletonViewport::appendCubes(
    SkeletonDrawList &list,
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    const ViewportCamera &camera, float view_w, float view_h,
    bool baked_style) const {
  if (geometry_ == nullptr) {
    return;
  }
  for (const auto &bone : geometry_->bones) {
    if (bone.cubes.empty() || isHidden(bone.name)) {
      continue;
    }
    auto pose_it = poses.find(bone.name);
    if (pose_it == poses.end()) {
      continue;
    }
    const JointRole role = roleFor(bone.name, baked_style);
    for (const auto &cube : bone.cubes) {
      const auto bind = baker::CubeGeometry::bindVertices(cube);
      float sx[8], sy[8], sd[8];
      double wx[8], wy[8], wz[8];
      bool ok[8];
      int visible = 0;
      for (int v = 0; v < 8; ++v) {
        double world[3];
        baker::CubeGeometry::transformPoint(
            pose_it->second, bind[static_cast<std::size_t>(v * 3)],
            bind[static_cast<std::size_t>(v * 3 + 1)],
            bind[static_cast<std::size_t>(v * 3 + 2)], world);
        wx[v] = world[0];
        wy[v] = world[1];
        wz[v] = world[2];
        if (mcbe_coords_) {
          applyMcbeBasis(wx[v], wz[v]);
        }
        ok[v] = camera.project(static_cast<float>(wx[v]),
                               static_cast<float>(wy[v]),
                               static_cast<float>(wz[v]), view_w, view_h,
                               sx[v], sy[v], sd[v]);
        if (ok[v]) {
          visible++;
        }
      }
      if (visible < 3) {
        continue;
      }
      for (const auto &face_idx : kCubeFaces) {
        if (!ok[face_idx[0]] || !ok[face_idx[1]] || !ok[face_idx[2]] ||
            !ok[face_idx[3]]) {
          continue;
        }

        const double e1x = wx[face_idx[1]] - wx[face_idx[0]];
        const double e1y = wy[face_idx[1]] - wy[face_idx[0]];
        const double e1z = wz[face_idx[1]] - wz[face_idx[0]];
        const double e2x = wx[face_idx[3]] - wx[face_idx[0]];
        const double e2y = wy[face_idx[3]] - wy[face_idx[0]];
        const double e2z = wz[face_idx[3]] - wz[face_idx[0]];
        double nx = e1y * e2z - e1z * e2y;
        double ny = e1z * e2x - e1x * e2z;
        double nz = e1x * e2y - e1y * e2x;
        const double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nlen < 1e-12) {
          continue;
        }
        nx /= nlen;
        ny /= nlen;
        nz /= nlen;

        ProjectedFace face;
        face.xy = {sx[face_idx[0]], sy[face_idx[0]], sx[face_idx[1]],
                   sy[face_idx[1]], sx[face_idx[2]], sy[face_idx[2]],
                   sx[face_idx[3]], sy[face_idx[3]]};
        face.depth = 0.25f * (sd[face_idx[0]] + sd[face_idx[1]] +
                              sd[face_idx[2]] + sd[face_idx[3]]);
        face.depths = {sd[face_idx[0]], sd[face_idx[1]], sd[face_idx[2]],
                       sd[face_idx[3]]};
        face.shade = faceShade(static_cast<float>(nx), static_cast<float>(ny),
                               static_cast<float>(nz));
        face.role = role;
        face.is_ground = false;
        face.bone_name = bone.name;
        list.faces.push_back(face);
      }
    }
  }
}

SkeletonDrawList SkeletonViewport::projectPoses(
    const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
    const ViewportCamera &camera, float view_w, float view_h,
    bool baked_style) const {
  SkeletonDrawList list;
  if (geometry_ == nullptr || poses.empty() || view_w < 2.0f || view_h < 2.0f) {
    return list;
  }

  appendGround(list, camera, view_w, view_h);
  appendGrid(list, camera, view_w, view_h);
  appendCubes(list, poses, camera, view_w, view_h, baked_style);

  std::map<std::string, std::array<float, 3>> screen;
  float min_x = std::numeric_limits<float>::max();
  float min_y = std::numeric_limits<float>::max();
  float min_z = std::numeric_limits<float>::max();
  float max_x = std::numeric_limits<float>::lowest();
  float max_y = std::numeric_limits<float>::lowest();
  float max_z = std::numeric_limits<float>::lowest();

  for (const auto &bone : geometry_->bones) {
    if (isHidden(bone.name)) {
      continue;
    }
    auto it = poses.find(bone.name);
    if (it == poses.end()) {
      continue;
    }
    const auto &p = it->second.world_position;
    double px = p[0];
    const double py = p[1];
    double pz = p[2];
    if (mcbe_coords_) {
      applyMcbeBasis(px, pz);
    }
    expandBounds(min_x, min_y, min_z, max_x, max_y, max_z, px, py, pz);
    float sx = 0, sy = 0, sd = 0;
    if (!camera.project(static_cast<float>(px), static_cast<float>(py),
                        static_cast<float>(pz), view_w, view_h, sx, sy, sd)) {
      continue;
    }
    screen[bone.name] = {sx, sy, sd};
  }

  if (min_x <= max_x) {
    list.has_bounds = true;
    list.bounds_min = {min_x, min_y, min_z};
    list.bounds_max = {max_x, max_y, max_z};
  }

  for (const auto &bone : geometry_->bones) {
    if (!show_bones_) {
      break;
    }
    auto sit = screen.find(bone.name);
    if (sit == screen.end()) {
      continue;
    }
    if (bone.has_parent && !bone.parent.empty()) {
      auto pit = screen.find(bone.parent);
      if (pit != screen.end()) {
        ProjectedSegment seg;
        seg.x0 = pit->second[0];
        seg.y0 = pit->second[1];
        seg.x1 = sit->second[0];
        seg.y1 = sit->second[1];
        seg.depth = 0.5f * (pit->second[2] + sit->second[2]);
        seg.depth0 = pit->second[2];
        seg.depth1 = sit->second[2];
        seg.role = roleFor(bone.name, baked_style);
        seg.name = bone.name;

        seg.thickness = (seg.role == JointRole::Default) ? 1.4f : 2.2f;
        list.segments.push_back(seg);
      }
    }
  }

  for (const auto &bone : geometry_->bones) {
    if (!show_bones_) {
      break;
    }
    auto sit = screen.find(bone.name);
    if (sit == screen.end()) {
      continue;
    }
    ProjectedJoint j;
    j.x = sit->second[0];
    j.y = sit->second[1];
    j.depth = sit->second[2];
    j.role = roleFor(bone.name, baked_style);
    j.name = bone.name;

    j.radius = (j.role == JointRole::Default) ? 2.5f : 4.0f;
    list.joints.push_back(j);
  }


  std::sort(list.faces.begin(), list.faces.end(),
            [](const ProjectedFace &a, const ProjectedFace &b) {
              return a.depth > b.depth;
            });
  std::sort(list.segments.begin(), list.segments.end(),
            [](const ProjectedSegment &a, const ProjectedSegment &b) {
              return a.depth > b.depth;
            });
  std::sort(list.joints.begin(), list.joints.end(),
            [](const ProjectedJoint &a, const ProjectedJoint &b) {
              return a.depth > b.depth;
            });
  return list;
}

std::string pickBone(const SkeletonDrawList &list, float x, float y,
                     float tolerance) {
  std::string best_name;
  float best_depth = (std::numeric_limits<float>::max)();
  float best_distance_squared = (std::numeric_limits<float>::max)();
  const auto consider = [&](const std::string &name, float depth,
                            float distance_squared) {
    if (name.empty() || !std::isfinite(depth)) {
      return;
    }
    constexpr float kDepthEpsilon = 1e-4f;
    if (depth < best_depth - kDepthEpsilon ||
        (std::abs(depth - best_depth) <= kDepthEpsilon &&
         distance_squared < best_distance_squared)) {
      best_name = name;
      best_depth = depth;
      best_distance_squared = distance_squared;
    }
  };

  for (const auto &face : list.faces) {
    if (!face.is_ground && !face.bone_name.empty() &&
        pointInQuad(x, y, face.xy)) {
      consider(face.bone_name, faceDepthAt(face, x, y), 0.0f);
    }
  }

  const float clamped_tolerance = std::max(0.0f, tolerance);
  for (const auto &segment : list.segments) {
    const float radius = clamped_tolerance + segment.thickness * 0.5f;
    const SegmentHit hit = pointSegmentHit(x, y, segment.x0, segment.y0,
                                           segment.x1, segment.y1);
    if (hit.distance_squared <= radius * radius) {
      consider(segment.name, segmentDepthAt(segment, hit.t),
               hit.distance_squared);
    }
  }

  for (const auto &joint : list.joints) {
    const float dx = x - joint.x;
    const float dy = y - joint.y;
    const float distance_squared = dx * dx + dy * dy;
    const float radius = std::max(joint.radius, clamped_tolerance);
    if (distance_squared <= radius * radius) {
      consider(joint.name, joint.depth, distance_squared);
    }
  }
  return best_name;
}

SkeletonDrawList SkeletonViewport::buildRest(const ViewportCamera &camera,
                                             float view_w, float view_h) const {
  return projectPoses(restPoses(), camera, view_w, view_h, false);
}

SkeletonDrawList
SkeletonViewport::buildAnimation(const loader::Animation *animation,
                                 double time, const ViewportCamera &camera,
                                 float view_w, float view_h) const {
  if (geometry_ == nullptr) {
    return {};
  }
  const auto poses =
      baker::BonePoseCalculator::calculate(geometry_->bones, animation, time);
  return projectPoses(poses, camera, view_w, view_h, false);
}

SkeletonDrawList SkeletonViewport::buildBaked(const baker::BakedFrame &frame,
                                              const ViewportCamera &camera,
                                              float view_w,
                                              float view_h) const {
  return buildBaked(frame, nullptr, 0.0, camera, view_w, view_h);
}

SkeletonDrawList SkeletonViewport::buildBaked(
    const baker::BakedFrame &frame,
    const loader::Animation *reference_animation, double reference_time,
    const ViewportCamera &camera, float view_w, float view_h) const {
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
    poses = baker::BonePoseCalculator::calculate(
        geometry_->bones, reference_animation, reference_time, &pos_ov,
        &rot_ov);
  } else if (reference_animation != nullptr) {
    poses = baker::BonePoseCalculator::calculate(
        geometry_->bones, reference_animation, reference_time);
  } else {
    poses = restPoses();
  }

  for (const auto &bs : frame.bone_states) {
    if (bs.has_world_position) {
      auto it = poses.find(bs.bone_name);
      if (it != poses.end()) {
        it->second.world_position = bs.world_position;
      }
    }
  }
  return projectPoses(poses, camera, view_w, view_h, true);
}

SoftRasterImage softRasterize(const SkeletonDrawList &list, int width,
                              int height) {
  SoftRasterImage img;
  img.width = std::max(1, width);
  img.height = std::max(1, height);


  constexpr std::uint8_t kBgR = 20;
  constexpr std::uint8_t kBgG = 22;
  constexpr std::uint8_t kBgB = 26;
  img.rgba.resize(static_cast<std::size_t>(img.width) *
                  static_cast<std::size_t>(img.height) * 4u);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i] = kBgR;
    img.rgba[i + 1] = kBgG;
    img.rgba[i + 2] = kBgB;
    img.rgba[i + 3] = 255;
  }


  for (const auto &face : list.faces) {
    Rgba color;
    if (face.is_ground) {

      color = {70, 78, 92, 115};
    } else {
      color = roleCubeColor(face.role, face.shade);
    }
    fillQuad(img.rgba, img.width, img.height, face.xy, color);
    if (!face.is_ground && face.role == JointRole::Selected) {
      constexpr Rgba kOutline{255, 210, 32, 255};
      for (int edge = 0; edge < 4; ++edge) {
        const int next = (edge + 1) % 4;
        drawThickLine(img.rgba, img.width, img.height, face.xy[edge * 2],
                      face.xy[edge * 2 + 1], face.xy[next * 2],
                      face.xy[next * 2 + 1], 2.0f, kOutline);
      }
    }
  }

  for (const auto &seg : list.grid) {

    drawThickLine(img.rgba, img.width, img.height, seg.x0, seg.y0, seg.x1,
                  seg.y1, seg.thickness, Rgba{200, 200, 210, 90});
  }
  for (const auto &seg : list.segments) {
    drawThickLine(img.rgba, img.width, img.height, seg.x0, seg.y0, seg.x1,
                  seg.y1, seg.thickness, roleSegmentColor(seg.role));
  }
  for (const auto &j : list.joints) {
    drawCircle(img.rgba, img.width, img.height, j.x, j.y, j.radius,
               roleJointColor(j.role));
  }
  return img;
}

bool writeRgbaBmp(const SoftRasterImage &image, const std::string &path) {
  if (image.width <= 0 || image.height <= 0 ||
      image.rgba.size() <
          static_cast<std::size_t>(image.width) * image.height * 4u) {
    return false;
  }

  const int w = image.width;
  const int h = image.height;

  const std::uint32_t row_bytes = static_cast<std::uint32_t>(w) * 4u;
  const std::uint32_t pixel_bytes = row_bytes * static_cast<std::uint32_t>(h);
  const std::uint32_t file_size = 54u + pixel_bytes;

  std::vector<std::uint8_t> file(file_size, 0);

  file[0] = 'B';
  file[1] = 'M';
  file[2] = static_cast<std::uint8_t>(file_size);
  file[3] = static_cast<std::uint8_t>(file_size >> 8);
  file[4] = static_cast<std::uint8_t>(file_size >> 16);
  file[5] = static_cast<std::uint8_t>(file_size >> 24);
  file[10] = 54;

  file[14] = 40;
  file[18] = static_cast<std::uint8_t>(w);
  file[19] = static_cast<std::uint8_t>(w >> 8);
  file[20] = static_cast<std::uint8_t>(w >> 16);
  file[21] = static_cast<std::uint8_t>(w >> 24);

  file[22] = static_cast<std::uint8_t>(h);
  file[23] = static_cast<std::uint8_t>(h >> 8);
  file[24] = static_cast<std::uint8_t>(h >> 16);
  file[25] = static_cast<std::uint8_t>(h >> 24);
  file[26] = 1;
  file[28] = 32;

  file[34] = static_cast<std::uint8_t>(pixel_bytes);
  file[35] = static_cast<std::uint8_t>(pixel_bytes >> 8);
  file[36] = static_cast<std::uint8_t>(pixel_bytes >> 16);
  file[37] = static_cast<std::uint8_t>(pixel_bytes >> 24);


  for (int y = 0; y < h; ++y) {
    const int src_y = h - 1 - y;
    std::uint8_t *dst =
        file.data() + 54 + static_cast<std::size_t>(y) * row_bytes;
    const std::uint8_t *src =
        image.rgba.data() +
        static_cast<std::size_t>(src_y) * static_cast<std::size_t>(w) * 4u;
    for (int x = 0; x < w; ++x) {
      dst[x * 4 + 0] = src[x * 4 + 2];
      dst[x * 4 + 1] = src[x * 4 + 1];
      dst[x * 4 + 2] = src[x * 4 + 0];
      dst[x * 4 + 3] = src[x * 4 + 3];
    }
  }

  std::error_code ec;
  std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path(), ec);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char *>(file.data()),
            static_cast<std::streamsize>(file.size()));
  return static_cast<bool>(out);
}

std::string viewportFrameBmpPath() {
#if defined(_WIN32)
  char temp[MAX_PATH] = {};
  const DWORD n = GetTempPathA(MAX_PATH, temp);
  if (n == 0 || n >= MAX_PATH) {
    return "xpbd_viewport_frame.bmp";
  }
  return std::string(temp) + "xpbd_baker_viewport_frame.bmp";
#else
  return "/tmp/xpbd_baker_viewport_frame.bmp";
#endif
}

}
