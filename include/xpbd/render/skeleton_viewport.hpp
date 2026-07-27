#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/bone_pose_calculator.hpp"
#include "xpbd/core/simd_dispatch.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/render/viewport_camera.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xpbd::render {

enum class JointRole {
  Default,
  Physics,
  Fixed,
  Selected,
  BakedPhysics,
};

struct ProjectedJoint {
  float x = 0.0f;
  float y = 0.0f;
  float depth = 0.0f;
  float radius = 4.0f;
  JointRole role = JointRole::Default;
  std::string name;
};

struct ProjectedSegment {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float depth = 0.0f;
  float depth0 = 0.0f;
  float depth1 = 0.0f;
  float thickness = 2.0f;
  JointRole role = JointRole::Default;
  std::string name;
};


struct ProjectedFace {
  std::array<float, 8> xy{};
  float depth = 0.0f;
  std::array<float, 4> depths{};
  float shade = 1.0f;
  JointRole role = JointRole::Default;
  bool is_ground = false;
  std::string bone_name;
};

struct SkeletonDrawList {
  std::vector<ProjectedSegment> grid;
  std::vector<ProjectedSegment> segments;
  std::vector<ProjectedJoint> joints;
  std::vector<ProjectedFace> faces;
  std::array<float, 3> bounds_min{0, 0, 0};
  std::array<float, 3> bounds_max{0, 0, 0};
  bool has_bounds = false;
};

struct BonePickDiagnostics {
  std::uint32_t candidate_face_count = 0;
  std::uint32_t total_face_count = 0;
};

// 缓存投影后的精确拾取数据，并用屏幕网格缩小每次查询的候选面集合。
struct BonePickIndex {
  SkeletonDrawList draw_list;
  float cell_size = 64.0f;
  float face_padding = 6.0f;
  std::uint32_t columns = 0;
  std::uint32_t rows = 0;
  std::vector<std::uint32_t> cell_offsets;
  std::vector<std::uint32_t> face_indices;
};





struct SoftRasterImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};





// 软件视口渲染与拾取辅助；GPU 后端不可用时也可用于预览和测试。
class SkeletonViewport {
public:
  void setGeometry(const loader::Geometry *geometry);
  void setBoneMapper(const baker::BoneMapper *mapper);
  void setSelectedBone(std::string name) { selected_bone_ = std::move(name); }
  void setHiddenBones(const std::set<std::string> *hidden_bones) {
    hidden_bones_ = hidden_bones;
  }
  void setShowBones(bool show_bones) { show_bones_ = show_bones; }
  void setMcbeCoords(bool mcbe_coords) { mcbe_coords_ = mcbe_coords; }

  [[nodiscard]] SkeletonDrawList buildRest(const ViewportCamera &camera,
                                           float view_w, float view_h,
                                           bool sort_by_depth = true) const;
  [[nodiscard]] SkeletonDrawList
  buildAnimation(const loader::Animation *animation, double time,
                 const ViewportCamera &camera, float view_w,
                 float view_h, bool sort_by_depth = true) const;
  [[nodiscard]] SkeletonDrawList buildBaked(const baker::BakedFrame &frame,
                                            const ViewportCamera &camera,
                                            float view_w, float view_h,
                                            bool sort_by_depth = true) const;
  [[nodiscard]] SkeletonDrawList
  buildBaked(const baker::BakedFrame &frame,
             const loader::Animation *reference_animation,
             double reference_time, const ViewportCamera &camera, float view_w,
             float view_h, bool sort_by_depth = true) const;


  [[nodiscard]] bool computeBounds(std::array<float, 3> &center,
                                   float &radius) const;

private:
  const loader::Geometry *geometry_ = nullptr;
  const baker::BoneMapper *mapper_ = nullptr;
  const std::set<std::string> *hidden_bones_ = nullptr;
  std::string selected_bone_;
  bool show_bones_ = true;
  bool mcbe_coords_ = false;
  core::SimdMode transform_simd_mode_ = core::SimdMode::SSE2;

  [[nodiscard]] std::map<std::string, baker::BonePoseCalculator::Pose>
  restPoses() const;
  [[nodiscard]] SkeletonDrawList projectPoses(
      const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
      const ViewportCamera &camera, float view_w, float view_h,
      bool baked_style, bool sort_by_depth) const;
  void appendGrid(SkeletonDrawList &list, const ViewportCamera &camera,
                  float view_w, float view_h) const;
  void appendGround(SkeletonDrawList &list, const ViewportCamera &camera,
                    float view_w, float view_h) const;
  void appendCubes(
      SkeletonDrawList &list,
      const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
      const ViewportCamera &camera, float view_w, float view_h,
      bool baked_style) const;
  [[nodiscard]] JointRole roleFor(const std::string &bone_name,
                                  bool baked_style) const;
  [[nodiscard]] bool isHidden(const std::string &bone_name) const;
};






[[nodiscard]] std::string pickBone(const SkeletonDrawList &list, float x,
                                   float y, float tolerance = 6.0f);

[[nodiscard]] BonePickIndex buildBonePickIndex(SkeletonDrawList draw_list,
                                               float view_w, float view_h,
                                               float cell_size = 64.0f,
                                               float face_padding = 6.0f);

[[nodiscard]] std::string
pickBone(const BonePickIndex &index, float x, float y, float tolerance = 6.0f,
         BonePickDiagnostics *diagnostics = nullptr);


void lineToQuad(float x0, float y0, float x1, float y1, float thickness,
                std::array<float, 8> &out_xy);


SoftRasterImage softRasterize(const SkeletonDrawList &list, int width,
                              int height);





bool writeRgbaBmp(const SoftRasterImage &image, const std::string &path);


std::string viewportFrameBmpPath();

}
