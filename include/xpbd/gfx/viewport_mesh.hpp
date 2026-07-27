#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/core/simd_dispatch.hpp"
#include "xpbd/gfx/texture_image.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/render/skeleton_viewport.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xpbd::gfx {


struct MeshVertex {
  float px = 0, py = 0, pz = 0;
  float nx = 0, ny = 1, nz = 0;
  float r = 1, g = 1, b = 1, a = 1;
};


enum class StaticModelFaceDirection : std::uint8_t {
  West = 0,
  East = 1,
  Down = 2,
  Up = 3,
  North = 4,
  South = 5,
};








struct StaticModelVertex {
  float px = 0, py = 0, pz = 0;
  float nx = 0, ny = 1, nz = 0;
  float u = 0, v = 0;
  std::uint32_t bone_index = 0;
};


struct StaticModelFace {
  std::uint32_t first_vertex = 0;
  std::uint32_t vertex_count = 0;
  std::uint32_t first_index = 0;
  std::uint32_t index_count = 0;
  std::uint32_t bone_index = 0;
  std::uint32_t cube_index = 0;
  StaticModelFaceDirection direction = StaticModelFaceDirection::West;
  bool textured = false;
};






struct StaticIndexedModelMesh {
  std::vector<StaticModelVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<StaticModelFace> faces;
  std::vector<std::string> bone_names;
  std::uint32_t cube_count = 0;

  void clear() {
    vertices.clear();
    indices.clear();
    faces.clear();
    bone_names.clear();
    cube_count = 0;
  }
};

struct ViewportGpuScene {
  std::vector<MeshVertex> solid;
  std::vector<MeshVertex> transparent;
  std::vector<MeshVertex> lines;
  int cube_count = 0;
  int line_segment_count = 0;
  void clear() {
    solid.clear();
    transparent.clear();
    lines.clear();
    cube_count = 0;
    line_segment_count = 0;
  }
};


struct StaticModelBoneState {

  std::array<float, 16> transform{1, 0, 0, 0, 0, 1, 0, 0,
                                  0, 0, 1, 0, 0, 0, 0, 1};

  std::array<float, 4> tint{1, 1, 1, 1};
};



struct StaticModelFrameData {
  std::vector<StaticModelBoneState> bones;


  ViewportGpuScene overlays;
  std::uint32_t cube_count = 0;

  void clear() {
    bones.clear();
    overlays.clear();
    cube_count = 0;
  }
};






// 根据 Bedrock 模型与当前姿势生成视口可绘制网格。
class ViewportMeshBuilder {
public:


  void setGeometry(const loader::Geometry *geometry);
  void setBoneMapper(const baker::BoneMapper *mapper) { mapper_ = mapper; }
  void setSelectedBone(std::string name) { selected_bone_ = std::move(name); }
  void setHoveredBone(std::string name) { hovered_bone_ = std::move(name); }
  void setHiddenBones(const std::set<std::string> *hidden_bones) {
    hidden_bones_ = hidden_bones;
  }
  void setTexture(const TextureImage *tex) { texture_ = tex; }
  void setShowBones(bool v) { show_bones_ = v; }
  void setShowGround(bool v) { show_ground_ = v; }
  void setMcbeCoords(bool v) { mcbe_coords_ = v; }
  void setTransformSimdMode(core::SimdMode mode);
  [[nodiscard]] core::SimdMode transformSimdMode() const {
    return transform_simd_mode_;
  }

  void buildRest(ViewportGpuScene &out) const;
  void buildAnimation(const loader::Animation *animation, double time,
                      ViewportGpuScene &out) const;
  void buildBaked(const baker::BakedFrame &frame, ViewportGpuScene &out) const;
  void buildBaked(const baker::BakedFrame &frame,
                  const loader::Animation *reference_animation,
                  double reference_time, ViewportGpuScene &out) const;

  void buildStaticRestFrame(StaticModelFrameData &out) const;
  void buildStaticAnimationFrame(const loader::Animation *animation,
                                 double time, StaticModelFrameData &out) const;
  void buildStaticBakedFrame(const baker::BakedFrame &frame,
                             const loader::Animation *reference_animation,
                             double reference_time,
                             StaticModelFrameData &out) const;





  void buildStaticIndexedModel(StaticIndexedModelMesh &out) const;

private:
  const loader::Geometry *geometry_ = nullptr;
  const baker::BoneMapper *mapper_ = nullptr;
  const std::set<std::string> *hidden_bones_ = nullptr;
  const TextureImage *texture_ = nullptr;
  std::string selected_bone_;
  std::string hovered_bone_;
  bool show_bones_ = true;
  bool show_ground_ = true;
  bool mcbe_coords_ = false;
  core::SimdMode transform_simd_mode_ = core::SimdMode::SSE2;
  std::optional<baker::BonePoseCalculator::Evaluator> pose_evaluator_;
  std::map<std::string, baker::BonePoseCalculator::Pose> rest_poses_;
  ViewportGpuScene ground_cache_;

  void buildFromPoses(
      const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
      bool baked_style, ViewportGpuScene &out, bool include_model = true) const;
  void buildStaticFrameFromPoses(
      const std::map<std::string, baker::BonePoseCalculator::Pose> &poses,
      bool baked_style, StaticModelFrameData &out) const;
  [[nodiscard]] std::map<std::string, baker::BonePoseCalculator::Pose>
  calculateBakedPoses(const baker::BakedFrame &frame,
                      const loader::Animation *reference_animation,
                      double reference_time) const;
  [[nodiscard]] render::JointRole roleFor(const std::string &bone_name,
                                          bool baked_style) const;
  [[nodiscard]] bool isHidden(const std::string &bone_name) const;
};


void buildSessionViewportScene(const loader::Geometry &geometry,
                               const baker::BoneMapper &bone_mapper,
                               const std::string &selected_bone,
                               const loader::Animation *selected_animation,
                               double preview_time, bool show_baked,
                               const baker::BakedFrame *baked_frame,
                               const TextureImage *texture, bool show_bones,
                               bool mcbe_coords, ViewportGpuScene &out,
                               bool show_ground = true,
                               const std::set<std::string> *hidden_bones =
                                   nullptr,
                               const std::string &hovered_bone = {});



void buildSessionStaticModelFrame(const loader::Geometry &geometry,
                                  const baker::BoneMapper &bone_mapper,
                                  const std::string &selected_bone,
                                  const loader::Animation *selected_animation,
                                  double preview_time, bool show_baked,
                                  const baker::BakedFrame *baked_frame,
                                  const TextureImage *texture, bool show_bones,
                                  bool mcbe_coords, StaticModelFrameData &out,
                                  bool show_ground = true,
                                  const std::set<std::string> *hidden_bones =
                                      nullptr);


inline void buildSessionViewportScene(
    const loader::Geometry &geometry, const baker::BoneMapper &bone_mapper,
    const std::string &selected_bone,
    const loader::Animation *selected_animation, double preview_time,
    bool show_baked, const baker::BakedFrame *baked_frame,
    ViewportGpuScene &out) {
  buildSessionViewportScene(geometry, bone_mapper, selected_bone,
                            selected_animation, preview_time, show_baked,
                            baked_frame, nullptr, true, false, out, true);
}

}
