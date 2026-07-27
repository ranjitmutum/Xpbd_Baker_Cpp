#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/loader/model_loader.hpp"
#include "xpbd/render/skeleton_viewport.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool testNegativeYAndGroundReference() {
  const auto geometry = xpbd::loader::ModelLoader::loadFromString(R"json(
{
  "minecraft:geometry": [{
    "description": {"identifier": "geometry.negative_y_test"},
    "bones": [{
      "name": "root",
      "pivot": [0, 0, 0],
      "cubes": [
        {"origin": [-1, 0, -1], "size": [2, 2, 2]},
        {"origin": [-1, -101, -1], "size": [2, 1, 2]}
      ]
    }]
  }]
}
)json");

  bool ok = true;
  ok &= expect(geometry.bones.size() == 1,
               "negative-Y fixture should load one bone");
  ok &= expect(geometry.bones.front().cubes.size() == 2,
               "negative-Y fixture should preserve both cubes");
  ok &= expect(geometry.bones.front().cubes.back().origin[1] == -101.0,
               "model loader must preserve authored negative Y");

  xpbd::gfx::ViewportMeshBuilder builder;
  builder.setGeometry(&geometry);
  builder.setShowBones(false);
  builder.setShowGround(true);
  xpbd::gfx::ViewportGpuScene scene;
  builder.buildRest(scene);

  ok &= expect(scene.solid.size() >= 6,
               "ground-enabled scene should contain a ground quad");
  if (scene.solid.size() >= 6) {
    for (std::size_t i = 0; i < 6; ++i) {
      ok &= expect(std::abs(scene.solid[i].py) <= 1e-6f,
                   "ground quad must stay at world Y=0");
    }
  }
  const auto lowest = std::min_element(
      scene.solid.begin(), scene.solid.end(),
      [](const auto &left, const auto &right) { return left.py < right.py; });
  ok &= expect(lowest != scene.solid.end() && lowest->py < -100.0f,
               "negative-Y cube must remain below the world ground");
  return ok;
}

xpbd::render::SkeletonDrawList thinFaceFixture() {
  xpbd::render::SkeletonDrawList list;
  xpbd::render::ProjectedFace face;
  face.xy = {63.0f, 50.0f, 63.0f, 50.0f,
             63.0f, 100.0f, 63.0f, 100.0f};
  face.depth = 10.0f;
  face.depths = {10.0f, 10.0f, 10.0f, 10.0f};
  face.bone_name = "thin";
  list.faces.push_back(face);
  return list;
}

bool testThinFacePicking() {
  bool ok = true;
  const auto list = thinFaceFixture();
  ok &= expect(xpbd::render::pickBone(list, 67.0f, 75.0f, 6.0f) == "thin",
               "direct picking should accept a thin face within 6 pixels");
  ok &= expect(xpbd::render::pickBone(list, 70.0f, 75.0f, 6.0f).empty(),
               "direct picking should reject a thin face beyond tolerance");

  const auto index =
      xpbd::render::buildBonePickIndex(list, 160.0f, 120.0f, 64.0f, 6.0f);
  ok &= expect(xpbd::render::pickBone(index, 67.0f, 75.0f, 6.0f) == "thin",
               "indexed picking should include padding across cell boundaries");
  ok &= expect(xpbd::render::pickBone(index, 70.0f, 75.0f, 8.0f) == "thin",
               "larger runtime tolerance should fall back without losing hits");
  return ok;
}

} // namespace

int main() {
  const bool ok = testNegativeYAndGroundReference() && testThinFacePicking();
  if (ok) {
    std::cout << "viewport regression tests passed\n";
    return 0;
  }
  return 1;
}
